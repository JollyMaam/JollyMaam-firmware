#!/usr/bin/env python3
"""Restart the pad over and over and check each boot's tick against the host.

    bootloop.py [--mode flash|reboot|both] [--count N] [--keep-going] [--out FILE]

Each round restarts the pad (flash: `make flash` of the current build, which
boots through the ROM loader; reboot: `padtool reboot`), waits for it, and
reads the boot's "clock check" line (1000 host frames against the pad's
milliseconds: healthy 998-1000), and any hang or assert lines.  A slow boot
stops the loop with the pad left as it is, for a look, unless --keep-going.
Every round is one line on stdout and in --out.  A pad that does not come back
stops it too. If the USB tool device stops taking commands, a USB session or
a replug can recover it.
"""
import argparse
import os
import re
import subprocess
import sys
import struct
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
PADTOOL_DIR = os.path.join(REPO, "padtool")
PADTOOL = [sys.executable, os.path.join(PADTOOL_DIR, "padtool.py")]
sys.path.insert(0, PADTOOL_DIR)
import padtool as pt  # noqa: E402
CLOCK = re.compile(r"clock check: (\d+) host frames in (\d+) ms")


def padtool(*args, timeout=15):
    try:
        r = subprocess.run(PADTOOL + list(args), capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout
    except subprocess.TimeoutExpired:
        return -1, ""


def log_lines():
    """The pad's log as (sequence number, text), or None while it does not answer."""
    path = pt.find_device()
    if not path:
        return None
    out, seq = [], 1
    try:
        while True:
            d = pt.cmd(path, 0x02, seq, 512)
            first, n = struct.unpack_from("<II", d)
            lines = d[8:8 + n].decode("utf-8", "replace").splitlines()
            if not lines:
                return out
            out += [(first + i, l) for i, l in enumerate(lines)]
            seq = first + len(lines)
    except OSError:
        return None


def new_boot_lines(mark):
    """The log of the newest boot, if it began after sequence number mark."""
    lines = log_lines()
    starts = [i for i, (q, l) in enumerate(lines or []) if "tick: timer3" in l and q > mark]
    return [l for _, l in lines[starts[-1]:]] if starts else None


def wait_for_pad(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        rc, out = padtool("status", timeout=10)
        if rc == 0 and out.startswith("state"):
            return True
        time.sleep(1)
    return False


def boot_clock(mark, seconds):
    """The clock check of a boot after log sequence number mark: (frames, ms),
    or None if none shows in time."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        for l in new_boot_lines(mark) or []:
            m = CLOCK.search(l)
            if m:
                return int(m.group(1)), int(m.group(2))
        time.sleep(1)
    return None


def restart(mode):
    if mode == "flash":
        r = subprocess.run(["make", "-C", REPO, "flash"], capture_output=True, text=True, timeout=200)
        return r.returncode == 0 and "written and verified" in r.stdout
    rc, _ = padtool("reboot")
    return rc == 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--mode", choices=["flash", "reboot", "both"], default="flash")
    p.add_argument("--count", type=int, default=100)
    p.add_argument("--keep-going", action="store_true", help="do not stop at a slow boot")
    p.add_argument("--out", help="also append each line here")
    a = p.parse_args()

    out = open(a.out, "a", buffering=1) if a.out else None
    tally = {}

    def say(line):
        print(line, flush=True)
        if out:
            out.write(line + "\n")

    if not wait_for_pad(30):
        sys.exit("bootloop: no pad")
    for n in range(1, a.count + 1):
        mode = a.mode if a.mode != "both" else ("flash" if n % 2 else "reboot")
        stamp = time.strftime("%H:%M:%S")
        lines = log_lines() or [(0, "")]
        mark = lines[-1][0]
        if not restart(mode):
            say(f"{stamp} {n:4} {mode:6} restart failed")
            if not wait_for_pad(60):
                say("pad gone: stopping")
                break
            continue
        time.sleep(1)
        if not wait_for_pad(60):
            say(f"{stamp} {n:4} {mode:6} pad did not come back: stopping")
            break
        clock = boot_clock(mark, 20)
        if not clock:
            say(f"{stamp} {n:4} {mode:6} no new boot with a clock check: stopping")
            break
        frames, ms = clock
        for l in new_boot_lines(mark) or []:
            if any(k in l for k in ("hang:", "assert")):
                say(f"{stamp} {n:4} {mode:6}   {l.strip()}")
        slow = ms < frames * 90 // 100   # real slow boots read 150-400 ms
        key = (mode, slow)
        tally[key] = tally.get(key, 0) + 1
        say(f"{stamp} {n:4} {mode:6} {frames} frames in {ms:4} ms{'  SLOW' if slow else ''}")
        if slow and not a.keep_going:
            say("slow boot: stopping with the pad as it is")
            break
    say("tally: " + ", ".join(f"{m} {'slow' if s else 'ok'} {c}" for (m, s), c in sorted(tally.items())))


if __name__ == "__main__":
    main()
