#!/usr/bin/env python3
"""Report rate and end-to-end latency of the connected slot, seen from this PC.

The pad makes random-phase A presses and releases at the GPIO sample, before
the debounce (`debug probe`), and stamps each edge with its microsecond clock
twice: when it "happened" and when the report carrying it went to the
profile.  This script maps the pad's clock onto CLOCK_MONOTONIC (`debug clock`
round trips, minimum-RTT samples, a straight-line fit for drift) and reads the
host's evdev node with CLOCK_MONOTONIC timestamps, so each edge gets:

    pad   edge -> report handed to the profile (poll, debounce, report tick)
    air   that -> host evdev event (stack, radio, host HID stack)
    total edge -> host evdev event

The report rate is counted three ways: reports the pad handed to the profile
(`debug timing`), reports arriving on the host's hidraw node, and the pad's
own last-second count.  The container and the host share one kernel, so
evdev/hidraw timestamps are the host's.

Leave the pad alone while it runs: any key edge on the device is taken as a
probe edge.  Needs the padbus daemon; the slot must be connected.

    tools/reportlat.py                   # 60 s, one edge every 50 ms
    tools/reportlat.py -t 20 --csv out.csv
"""

from __future__ import annotations

import argparse
import csv
import fcntl
import glob
import os
import re
import select
import statistics
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import padbus  # noqa: E402

CLIENT = "reportlat"
EVENT = struct.Struct("@llHHi")
EV_KEY = 1
EVIOCSCLOCKID = 0x400445a0


class Pad:
    def __init__(self, socket_path: str):
        self.sock = padbus.client_socket(socket_path)
        padbus.request(self.sock, op="hello", client=CLIENT)
        reply = padbus.request(self.sock, op="request", client=CLIENT)
        if not reply.get("ok"):
            raise SystemExit("padbus control is held by %s" % reply.get("owner"))

    # Host monotonic ns spans of our own commands: console output blocks the
    # pad's app task, so edges around them measure the tool, not the pad.
    busy: list = []

    def cmd(self, line: str, timeout: float = 5.0) -> list[str]:
        start = time.monotonic_ns()
        try:
            return self._cmd(line, timeout)
        finally:
            self.busy.append((start, time.monotonic_ns()))

    def _cmd(self, line: str, timeout: float) -> list[str]:
        reply = padbus.request(self.sock, op="cmd", client=CLIENT, line=line,
                               timeout=timeout, quiet=True)
        if not reply.get("ok"):
            raise RuntimeError(f"{line!r}: {reply.get('error', reply)}")
        return reply.get("lines", [])

    def close(self):
        padbus.request(self.sock, op="release", client=CLIENT)
        self.sock.close()


def sync_sample(pad: Pad, tries: int = 8):
    """(host_ns, pad_us, rtt_ns) of the tightest `debug clock` round trip."""
    best = None
    for _ in range(tries):
        t0 = time.monotonic_ns()
        lines = pad.cmd("debug clock")
        t1 = time.monotonic_ns()
        m = next((re.match(r"clock (\d+)", l) for l in lines
                  if l.startswith("clock ")), None)
        if not m:
            continue
        sample = ((t0 + t1) // 2, int(m.group(1)), t1 - t0)
        if best is None or sample[2] < best[2]:
            best = sample
    return best


class ClockMap:
    """pad microseconds -> host monotonic ns, least squares over samples."""

    def __init__(self):
        self.samples = []

    def add(self, sample):
        if sample:
            self.samples.append(sample)

    def fit(self):
        xs = [unwrap for unwrap in self._pad_unwrapped()]
        ys = [s[0] for s in self.samples]
        if len(xs) == 1:
            self.slope, self.offset = 1000.0, ys[0] - xs[0] * 1000.0
            return
        mx, my = statistics.fmean(xs), statistics.fmean(ys)
        sxx = sum((x - mx) ** 2 for x in xs)
        self.slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
        self.offset = my - self.slope * mx
        self.residual_us = max(abs(y - (self.slope * x + self.offset))
                               for x, y in zip(xs, ys)) / 1000.0

    def _pad_unwrapped(self):
        base = self.samples[0][1]
        return [base + ((s[1] - base) & 0xffffffff) for s in self.samples]

    def host_ns(self, pad_us: int) -> float:
        base = self.samples[0][1]
        return self.slope * (base + ((pad_us - base) & 0xffffffff)) + self.offset


def sysfs_uniq(path: str) -> str:
    name = os.path.basename(path)
    for pattern in (f"/sys/class/input/{name}/device/uevent",
                    f"/sys/class/hidraw/{name}/device/uevent"):
        try:
            with open(pattern) as f:
                text = f.read()
        except OSError:
            continue
        m = re.search(r"^(?:UNIQ|HID_UNIQ)=\"?([0-9a-fA-F:]*)", text, re.M)
        if m:
            return m.group(1).lower()
    return ""


def find_nodes(address: str):
    events = [p for p in sorted(glob.glob("/dev/bwrap/event*"))
              if sysfs_uniq(p) == address]
    raws = [p for p in sorted(glob.glob("/dev/bwrap/hidraw*"))
            if sysfs_uniq(p) == address]
    return events, raws


def percentiles(values, points=(0.5, 0.95, 0.99)):
    if not values:
        return ["n/a"] * (len(points) + 2)
    v = sorted(values)
    out = [v[min(len(v) - 1, int(round((len(v) - 1) * p)))] for p in points]
    return [f"{x:.1f}" for x in (v[0], *out, v[-1])]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("-t", "--seconds", type=float, default=60.0)
    parser.add_argument("--period", type=int, default=100,
                        help="probe press+release period, ms (two edges each)")
    parser.add_argument("--csv", help="write every matched edge here")
    parser.add_argument("--socket", default=padbus.DEFAULT_SOCKET)
    args = parser.parse_args()

    pad = Pad(args.socket)
    try:
        run(pad, args)
    finally:
        try:
            pad.cmd("debug probe stop")
        except Exception:
            pass
        pad.close()


def run(pad: Pad, args):
    stat = " ".join(pad.cmd("debug stat"))
    m = re.search(r"profile=(\S+) state=(\S+).*addr=([0-9a-f]{12})", stat)
    if not m or m.group(2) != "connected":
        raise SystemExit(f"slot not connected: {stat}")
    if any(l.strip().endswith("=true") for l in pad.cmd("config log")):
        raise SystemExit("turn log.console, log.btstack and log.hcidump off: "
                         "their output stalls the pad and skews the numbers")
    profile = m.group(1)
    address = ":".join(m.group(3)[i:i + 2] for i in range(0, 12, 2))
    events, raws = find_nodes(address)
    if not events:
        raise SystemExit(f"no evdev node for {address} in /dev/bwrap")
    hz_setting = " ".join(pad.cmd("status radio.slot") +
                          [l for l in pad.cmd("config profile") if "report_hz" in l])

    event_fds = []
    for path in events:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        fcntl.ioctl(fd, EVIOCSCLOCKID, struct.pack("i", time.CLOCK_MONOTONIC))
        event_fds.append(fd)
    raw_fds = [os.open(p, os.O_RDONLY | os.O_NONBLOCK) for p in raws]
    for fd in event_fds + raw_fds:
        try:
            while os.read(fd, 4096):
                pass
        except BlockingIOError:
            pass

    clock = ClockMap()
    clock.add(sync_sample(pad))
    edges = int(args.seconds * 2000 / args.period)
    pad.cmd("debug timing reset")
    pad.cmd(f"debug probe {edges} {args.period}")
    started = time.monotonic()
    key_events = []          # (host_ns, code, value)
    raw_times = []
    records = []
    last_take = last_sync = started
    fds = event_fds + raw_fds
    lost = 0
    while True:
        now = time.monotonic()
        ready, _, _ = select.select(fds, [], [], 0.05)
        for fd in ready:
            try:
                data = os.read(fd, EVENT.size * 64 if fd in event_fds else 256)
            except BlockingIOError:
                continue
            if fd in raw_fds:
                raw_times.append(time.monotonic_ns())
                continue
            for off in range(0, len(data) - EVENT.size + 1, EVENT.size):
                sec, usec, typ, code, value = EVENT.unpack_from(data, off)
                if typ == EV_KEY and value in (0, 1):
                    key_events.append((sec * 1_000_000_000 + usec * 1000,
                                       code, value))
        if now - last_take >= 1.0:
            active, lost = take(pad, records)
            last_take = now
            if not active and now - started > 1.0:
                break
        if now - last_sync >= 5.0:
            clock.add(sync_sample(pad))
            last_sync = now
        if now - started > args.seconds + 10:
            break
    time.sleep(0.3)
    take(pad, records)
    clock.add(sync_sample(pad))
    clock.fit()
    timing_lines = pad.cmd("debug timing")
    for fd in fds:
        os.close(fd)

    # Match each probe edge to the first key event of the same value after
    # its send time (less 2 ms of clock slack) and before the next edge's.
    records.sort(key=lambda r: r[0])
    code_counts = {}
    for _, code, _ in key_events:
        code_counts[code] = code_counts.get(code, 0) + 1
    code = max(code_counts, key=code_counts.get) if code_counts else None
    keys = [(t, v) for t, c, v in key_events if c == code]
    rows = []
    skipped = 0
    k = 0
    for i, (index, value, arrival, sent) in enumerate(records):
        a_ns = clock.host_ns(arrival)
        s_ns = clock.host_ns(sent)
        limit = clock.host_ns(records[i + 1][2]) + 200e6 if i + 1 < len(records) else float("inf")
        while k < len(keys) and keys[k][0] < s_ns - 2e6:
            k += 1
        if k < len(keys) and keys[k][1] == value and keys[k][0] < limit:
            host = keys[k][0]
            k += 1
            if any(b0 - 25e6 <= a_ns <= b1 + 2e6 for b0, b1 in pad.busy):
                skipped += 1
                continue
            rows.append((index, value, (s_ns - a_ns) / 1e6, (host - s_ns) / 1e6,
                         (host - a_ns) / 1e6))
    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(("index", "value", "pad_ms", "air_ms", "total_ms"))
            w.writerows(rows)

    t = dict(re.findall(r"(\w+)=(\d+)", " ".join(timing_lines)))
    bins = next((l.split()[1:] for l in timing_lines if l.startswith("send_ms")), [])
    elapsed = (raw_times[-1] - raw_times[0]) / 1e9 if len(raw_times) > 1 else 0
    raw_iv = [(b - a) / 1e6 for a, b in zip(raw_times, raw_times[1:])]
    print(f"profile={profile} addr={address} {hz_setting.strip()}")
    print(f"clock fit: {len(clock.samples)} syncs, drift {(clock.slope / 1000 - 1) * 1e6:+.1f} ppm, "
          f"worst residual {getattr(clock, 'residual_us', 0):.0f} us")
    sends = int(t.get("sends", 0))
    avg = int(t.get("send_avg_us", 0))
    print(f"pad sends: {sends} ({1e6 / avg if avg else 0:.1f} Hz avg, min "
          f"{int(t.get('send_min_us', 0)) / 1000:.1f} max {int(t.get('send_max_us', 0)) / 1000:.1f} ms); "
          f"polls min {int(t.get('poll_min_us', 0)) / 1000:.1f} max {int(t.get('poll_max_us', 0)) / 1000:.1f} ms")
    if bins:
        hist = " ".join(f"{i}:{n}" for i, n in enumerate(bins) if n != "0")
        print(f"pad send interval histogram (ms:count): {hist}")
    if raw_times:
        print(f"host hidraw: {len(raw_times)} reports, "
              f"{(len(raw_times) - 1) / elapsed if elapsed else 0:.1f} Hz; interval ms "
              "min/p50/p95/p99/max " + "/".join(percentiles(raw_iv)))
    else:
        print("host hidraw: no node")
    print(f"edges: {len(records)} from the pad, {len(keys)} on the host (key {code}), "
          f"{len(rows)} kept, {skipped} dropped near our commands, {lost} lost on the pad")
    for name, col in (("pad  ", 2), ("air  ", 3), ("total", 4)):
        print(f"latency {name} ms min/p50/p95/p99/max " +
              "/".join(percentiles([r[col] for r in rows])))


def take(pad: Pad, records):
    active = 0
    lost = 0
    while True:
        lines = pad.cmd("debug probe take")
        got = 0
        for line in lines:
            m = re.match(r"p (\d+) (\d) (\d+) (\d+)", line)
            if m:
                records.append(tuple(int(x) for x in m.groups()))
                got += 1
            m = re.search(r"lost=(\d+) active=(\d)", line)
            if m:
                lost, active = int(m.group(1)), int(m.group(2))
        if got < 48:
            return active, lost


if __name__ == "__main__":
    main()
