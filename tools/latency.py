#!/usr/bin/env python3
"""Measure injected button edge timing without requiring D-Bus or root.

The event timestamp and host-read timestamp are retained in CSV.  This tool
reports edge spacing, not USB-to-host latency: the injected command and evdev
clocks are not correlated.
"""
import argparse
import csv
import fcntl
import glob
import os
import select
import struct
import sys
import termios
import time

EVENT = struct.Struct("@llHHi")
EV_KEY = 1
EV_SYN = 0
SYN_DROPPED = 3
EVIOCSCLOCKID = 0x400445a0

def _request(code, length):
    # Linux _IOC(_IOC_READ, 'E', code, length).
    return 0x80000000 | (length << 16) | (ord("E") << 8) | code

def evdev_text(fd, code):
    data = bytearray(256)
    try:
        fcntl.ioctl(fd, _request(code, len(data)), data)
    except OSError:
        return ""
    return bytes(data).split(b"\0", 1)[0].decode(errors="replace")

def find_event(name, unique):
    if not name and not unique:
        return None
    paths = []
    for root in ("/dev/bwrap/input", "/dev/bwrap", "/dev/input"):
        paths.extend(glob.glob(root + "/event*"))
    for path in sorted(set(paths)):
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
        except OSError:
            continue
        got_name, got_unique = evdev_text(fd, 0x06), evdev_text(fd, 0x08)
        os.close(fd)
        if (not name or name == got_name) and (not unique or unique == got_unique):
            return path
    return None

def open_cdc(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK | os.O_CLOEXEC)
    attrs = termios.tcgetattr(fd)
    termios.cfmakeraw(attrs) if hasattr(termios, "cfmakeraw") else None
    # Python's cfmakeraw is not available on every Linux build.
    if not hasattr(termios, "cfmakeraw"):
        attrs[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP | termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
        attrs[1] &= ~termios.OPOST
        attrs[2] &= ~(termios.CSIZE | termios.PARENB); attrs[2] |= termios.CS8
        attrs[3] &= ~(termios.ECHONL | termios.ISIG | termios.ICANON | termios.ECHO | termios.IEXTEN)
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    # CDC implementations commonly wait for host open/ready. ENOTTY is fine.
    if hasattr(termios, "TIOCMBIS"):
        try:
            fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack("I", termios.TIOCM_DTR | termios.TIOCM_RTS))
        except OSError as exc:
            if exc.errno not in (25, 22):
                os.close(fd); raise
        time.sleep(0.05)
    return fd

def set_monotonic_clock(fd):
    try:
        fcntl.ioctl(fd, EVIOCSCLOCKID, struct.pack("i", time.CLOCK_MONOTONIC))
        return True
    except OSError:
        return False

def write_command(fd, text, timeout=3.0):
    payload = (text + "\r").encode()
    end = time.monotonic() + timeout
    offset = 0
    while offset < len(payload):
        try:
            offset += os.write(fd, payload[offset:])
        except BlockingIOError:
            if time.monotonic() >= end: raise TimeoutError("CDC write timeout")
            select.select([], [fd], [], max(0, end - time.monotonic()))
    return end

def command(fd, text, timeout=3.0):
    end = write_command(fd, text, timeout)
    output = bytearray()
    while time.monotonic() < end:
        ready, _, _ = select.select([fd], [], [], max(0, end - time.monotonic()))
        if not ready: break
        try: chunk = os.read(fd, 4096)
        except BlockingIOError: continue
        if not chunk: break
        output += chunk
        # A quiet interval avoids waiting for the full deadline on normal replies.
        if select.select([fd], [], [], 0.25)[0] == []: break
    return bytes(output)

def percentile(values, fraction):
    if not values: return None
    values = sorted(values)
    return values[min(len(values) - 1, int((len(values) - 1) * fraction))]

def injection_parameters(period, count):
    period_ms = int(period)
    if period_ms < 20 or period_ms > 1000 or period_ms % 2 or count <= 0:
        raise ValueError("period must be an even value from 20 through 1000 ms; count must be positive")
    duration_ms = period_ms * count
    if duration_ms > 60000:
        raise ValueError("period_ms * count must be at most 60000 ms")
    return period_ms, duration_ms, "debug inject a %d %d" % (period_ms, duration_ms)

def run(args):
    try:
        period_ms, duration_ms, inject = injection_parameters(args.period, args.count)
    except ValueError as exc:
        raise SystemExit(str(exc))
    event_path = args.event or find_event(args.device_name, args.device_unique)
    if not event_path:
        raise SystemExit("event node not supplied; use --event or matching --device-name/--device-unique")
    event_fd = os.open(event_path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    clock_set = set_monotonic_clock(event_fd)
    cdc_fd = open_cdc(args.cdc)
    events = []
    cdc_errors = []
    dropped = False
    cdc_buffer = ""
    started = False
    try:
        print(command(cdc_fd, "status list inject").decode(errors="replace"), end="")
        # Discard queued edges from before this measurement.
        try:
            while os.read(event_fd, EVENT.size * 64):
                pass
        except (BlockingIOError, OSError):
            pass
        write_command(cdc_fd, inject)
        end = time.monotonic() + duration_ms / 1000.0 + 2.0
        while time.monotonic() < end:
            ready, _, _ = select.select([event_fd, cdc_fd], [], [], min(0.25, end - time.monotonic()))
            if not ready: continue
            if cdc_fd in ready:
                try:
                    cdc_buffer += os.read(cdc_fd, 4096).decode(errors="replace")
                    lines = cdc_buffer.splitlines(keepends=True)
                    cdc_buffer = lines.pop() if lines and not lines[-1].endswith(("\n", "\r")) else ""
                    for line in lines:
                        line = line.strip()
                        upper = line.upper()
                        if "OK INJECTION STARTED" in upper:
                            started = True
                        if "ERR" in upper or any(word in upper for word in ("ERROR", "UNKNOWN", "INVALID")):
                            cdc_errors.append(line)
                except (BlockingIOError, OSError):
                    pass
            if event_fd in ready:
                try: data = os.read(event_fd, EVENT.size * 64)
                except BlockingIOError: continue
                host_ns = time.monotonic_ns()
                for offset in range(0, len(data) - EVENT.size + 1, EVENT.size):
                    sec, usec, typ, code, value = EVENT.unpack_from(data, offset)
                    if typ == EV_SYN and code == SYN_DROPPED:
                        dropped = True
                    elif typ == EV_KEY and code == args.code and value in (0, 1):
                        events.append((host_ns, sec, usec, typ, code, value))
    finally:
        try: command(cdc_fd, "debug inject stop")
        except (OSError, TimeoutError): pass
        os.close(cdc_fd); os.close(event_fd)
    with open(args.csv, "w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(("host_read_ns", "event_sec", "event_usec", "type", "code", "value"))
        writer.writerows(events)
    presses = sum(event[-1] == 1 for event in events)
    releases = sum(event[-1] == 0 for event in events)
    duplicates = sum(a[-1] == b[-1] for a, b in zip(events, events[1:]))
    spacing = [(b[1] + b[2] / 1e6) - (a[1] + a[2] / 1e6) for a, b in zip(events, events[1:])]
    expected = args.count
    print("device=%s clock=%s presses=%d releases=%d missing_press=%d missing_release=%d duplicates=%d dropped=%s" %
          (event_path, "monotonic" if clock_set else "kernel-default", presses, releases,
           max(0, expected - presses), max(0, expected - releases), duplicates, dropped))
    values = tuple("%.3f" % (value * 1000.0) if value is not None else "n/a" for value in
                   (percentile(spacing, .50), percentile(spacing, .95), percentile(spacing, .99)))
    print("interarrival_ms p50=%s p95=%s p99=%s samples=%d" % (values + (len(spacing),)))
    if cdc_errors:
        print("cdc_errors=" + " | ".join(cdc_errors), file=sys.stderr)
    if dropped:
        print("warning: evdev reported SYN_DROPPED; counts are incomplete", file=sys.stderr)
    failures = []
    if not started: failures.append("injection was not acknowledged")
    if cdc_errors: failures.append("CDC reported an error")
    if dropped: failures.append("evdev SYN_DROPPED")
    if presses != expected or releases != expected: failures.append("press/release count mismatch")
    if duplicates: failures.append("duplicate consecutive edges")
    if failures:
        raise SystemExit("measurement failed: " + "; ".join(failures))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cdc", help="explicit CDC ACM path")
    parser.add_argument("--event", help="explicit evdev event path")
    parser.add_argument("--device-name", help="exact EVIOCGNAME match when discovering")
    parser.add_argument("--device-unique", help="exact EVIOCGUNIQ match when discovering")
    parser.add_argument("--code", type=int, default=304, help="evdev key code (default BTN_SOUTH)")
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--period", type=float, default=100.0, help="injection period in ms (default 100)")
    parser.add_argument("--csv", default="latency-events.csv")
    parser.add_argument("--dry-run", action="store_true", help="print the validated injection command and exit")
    args = parser.parse_args()
    if args.dry_run:
        try:
            period_ms, duration_ms, inject = injection_parameters(args.period, args.count)
        except ValueError as exc:
            parser.error(str(exc))
        print("%s duration_ms=%d capture_s=%.3f" % (inject, duration_ms, duration_ms / 1000.0 + 2.0))
        return
    if not args.cdc:
        parser.error("--cdc is required unless --dry-run is used")
    try: run(args)
    except (OSError, TimeoutError) as exc: parser.error(str(exc))

if __name__ == "__main__": main()
