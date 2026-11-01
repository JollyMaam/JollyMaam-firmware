#!/usr/bin/env python3
"""What the ROM loader would see on this USB port.

    usbprobe.py DEVICE [--rom] [--io BITS] [--out FILE]
    usbprobe.py FILE                  analyse a saved run again

Needs a USB_PROBE test build (make gamesir-cyclone-2-pad EXTRA_DEFINES=-DUSB_PROBE).  The
pad leaves the bus for about 3 s: attached by a GPIO pull-up and answering
nothing, as the ROM loader is while it learns its crystal from the host's
frames (platform_jieli.c usb_probe()).  Then it attaches as itself again.

--rom records what the ROM's own measurement captures (timer 3 on D+ rising
edges, 6000 counts per 1 ms frame) and runs the ROM's rule on it: four
16-bit deltas in a row each within 260 of the one before.  Without it, the
runs of non-idle line states are listed with their gaps.  --io adds bits to
USB_IO_CON0 (0x7100: IO_PU_MODE, SR, DPDIEH, DMDIEH).
"""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "padtool"))
import padtool  # noqa: E402

PROBE_AT = 0x22210
CMD_USB_PROBE = 0x3f
CMD_PEEK = 0x15
LSB_PER_US = 24          # timer 1 counts (lsb clock)
ISR_MERGE = 60           # osc/4 counts: the ROM's interrupt reads one capture per burst
PHASES = {0: "off 300 ms", 1: "on 1000 ms", 2: "off 200 ms", 3: "on 1000 ms", 4: "off 300 ms", 5: "end"}


def run(path, rom, io):
    padtool.cmd(path, CMD_USB_PROBE, (1 if rom else 0) | io)
    time.sleep(8)        # the probe, then a new enumeration
    for _ in range(40):
        try:
            head = padtool.cmd(path, CMD_PEEK, PROBE_AT - 16, 16)
            break
        except OSError:
            time.sleep(0.5)
    else:
        sys.exit(f"{path}: did not come back")
    n, idle, lost, done = struct.unpack("<4I", head)
    if done != 0x50524f42:
        sys.exit(f"{path}: the probe did not finish")
    print(f"{n} events, idle state {idle}, {lost & 0xffff} lost, io bits {lost >> 16:#x}")
    raw = b""
    while len(raw) < 8 * n:
        raw += padtool.cmd(path, CMD_PEEK, PROBE_AT + len(raw), min(480, 8 * n - len(raw)))
    return [struct.unpack_from("<II", raw, 8 * i) for i in range(n)]


def rom_lock(captures):
    """The ROM's timer 3 interrupt: the time of the fourth delta in a row
    within 260 of the one before, or None; and the deltas it saw.  Traffic
    other than the frames can lock it too, on the wrong period."""
    merged = []
    for t, v in captures:
        if merged and (v - merged[-1][1]) & 0xffff < ISR_MERGE:
            merged[-1] = (merged[-1][0], v)
        else:
            merged.append((t, v))
    last = prev = count = 0
    deltas = []
    for t, v in merged:
        d = (v - last) & 0xffff
        last = v
        deltas.append(d)
        if prev and prev - 260 < d < prev + 260:
            count += 1
            if count == 4:
                return t, deltas
        else:
            count = 0
        prev = d
    return None, deltas


def show(name, start, events):
    captures = [(t, v) for t, v, kind in events if kind == "capture"]
    if captures:
        at, deltas = rom_lock(captures)
        lock = "never locks"
        if at:
            period = sum(deltas[-4:]) / 4
            lock = (f"locks at {(at - start) / LSB_PER_US / 1000:.0f} ms on {period:.0f} counts"
                    f" ({'the frames' if abs(period - 6000) < 30 else 'not the frames: a wrong crystal'})")
        print(f"  {name}: {len(captures)} captures, the ROM {lock}")
        print("    deltas:", " ".join(str(d) for d in deltas[:60]))
        return
    runs = [(t, v) for t, v, kind in events if kind == "run"]
    print(f"  {name}: {len(runs)} runs")
    prev, out = start, []
    for t, v in runs[:50]:
        out.append(f"+{(t - prev) / LSB_PER_US:.0f}us[{v & 3}:{(v >> 2) / LSB_PER_US:.1f}]")
        prev = t
    if out:
        print("    " + " ".join(out))


def analyse(ev):
    current, events = None, []
    for t, v in ev:
        if v >= 0xfffffff0:
            if current:
                show(*current, events)
            current, events = (PHASES.get(v & 15, v & 15), t), []
        elif v & 0x80000000:
            events.append((t, v & 0x7fffffff, "capture"))
        else:
            events.append((t, v, "run"))
    if current and events:
        show(*current, events)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("device", help="the pad's /dev/sg node, or a saved run")
    p.add_argument("--rom", action="store_true", help="the ROM's own measurement")
    p.add_argument("--io", type=lambda x: int(x, 0), default=0, help="USB_IO_CON0 bits to add")
    p.add_argument("--out", help="save the events here")
    a = p.parse_args()
    if os.path.isfile(a.device) and not a.device.startswith("/dev/"):
        ev = [tuple(int(x, 0) for x in line.split()) for line in open(a.device) if line.strip()]
    else:
        ev = run(a.device, a.rom, a.io & 0x7100)
        if a.out:
            with open(a.out, "w") as f:
                f.write("".join(f"{t} {v:#x}\n" for t, v in ev))
    analyse(ev)


if __name__ == "__main__":
    main()
