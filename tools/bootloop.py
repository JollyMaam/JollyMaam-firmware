#!/usr/bin/env python3
"""Restart the pad over and over and check that it comes back on USB.

    bootloop.py [--mode flash|reboot|both] [--count N] [--out FILE]

Each round restarts the pad (flash: `make flash` of the current build, which
boots through the ROM loader; reboot: `padtool reboot`), waits for its new
boot in the log, and shows how long that took and any hang or assert lines.
Every round is one line on stdout and in --out.  A pad that does not come back
stops the loop. If the USB tool device stops taking commands, a USB session or
a replug can recover it.
"""
import argparse
import os
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
    starts = [i for i, (q, l) in enumerate(lines or []) if "boot: reset source" in l and q > mark]
    return [l for _, l in lines[starts[-1]:]] if starts else None


def wait_for_pad(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        rc, out = padtool("status", timeout=10)
        if rc == 0 and out.startswith("state"):
            return True
        time.sleep(1)
    return False


def new_boot(mark, seconds):
    """The log of a boot after log sequence number mark, or None if none shows in time."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        lines = new_boot_lines(mark)
        if lines:
            return lines
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
    p.add_argument("--out", help="also append each line here")
    a = p.parse_args()

    out = open(a.out, "a", buffering=1) if a.out else None
    tally = {"ok": 0}

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
        t0 = time.monotonic()
        time.sleep(1)
        if not wait_for_pad(60):
            say(f"{stamp} {n:4} {mode:6} pad did not come back: stopping")
            break
        back = time.monotonic() - t0
        lines = new_boot(mark, 20)
        if not lines:
            say(f"{stamp} {n:4} {mode:6} no new boot in the log: stopping")
            break
        for l in lines:
            if any(k in l for k in ("hang:", "assert")):
                say(f"{stamp} {n:4} {mode:6}   {l.strip()}")
        tally["ok"] += 1
        say(f"{stamp} {n:4} {mode:6} back in {back:4.1f} s  {lines[0].strip()}")
    say(f"tally: {tally['ok']} of {a.count} came back")


if __name__ == "__main__":
    main()
