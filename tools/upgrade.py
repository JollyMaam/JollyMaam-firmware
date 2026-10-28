#!/usr/bin/env python3
"""Flash every pad and dongle running this firmware with this tree's build.

    tools/upgrade.py            build, then flash what is connected and what
                                gets plugged in, one at a time, until Ctrl-C
    tools/upgrade.py --once     only what is connected now
    tools/upgrade.py --dongles  dongles only (--pads: pads only)

A device is known by its tool device's IDENTIFY (the image's PID and flash
UID).  Each is flashed once per run, through its board's make target, by its
USB port.  ROM loaders are left alone: their PID comes from the flash, which
may be blank.
"""
import argparse
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "padtool"))
from padtransport import scsi, tool_paths, usb_location  # noqa: E402

OP = 0xF5
TARGETS = {                       # the image's PID: its make target
    "GS_C2_ADC_DEVICE": "gamesir-cyclone-2-pad",
    "AC695X_TRANS": "gamesir-nova-2-lite-pad",
    "GS_C2_Dongle": "gamesir-cyclone-2-dongle",
    "U2C_WIRELESS": "8bitdo-ultimate2c-wireless-pad",
}


def identify(path):
    try:
        d = scsi(path, [OP, 0x00], 88, 1000)
    except OSError:
        return None
    if len(d) < 56 or d[:4] != b"C2PD":
        return None
    return d[40:56].split(b"\0")[0].decode(errors="replace"), d[24:40].hex(), d[6]


def connected():
    """(port, pid, uid, board) of our firmware; board: IDENTIFY's, 0 pad, 1 dongle."""
    found = []
    for path in tool_paths():
        i = identify(path)
        if i:
            found.append((usb_location(path), *i))
    return found


def back_on_hid(uid, seconds=30):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        for path in tool_paths():
            i = identify(path)
            if i and i[1] == uid:
                return path
        time.sleep(1)
    return None


def flash(port, pid, uid):
    target = TARGETS[pid]
    print(f"\n== {pid} at {port}: make {target}-flash", flush=True)
    r = subprocess.run(["make", "-C", ROOT, f"{target}-flash", f"DEVICE={port}"])
    if r.returncode:
        print(f"!! {pid} at {port}: flashing failed (make exit {r.returncode})", flush=True)
        return False
    new = back_on_hid(uid)
    print(f"ok {pid} at {port}: running, {new}" if new else
          f"!! {pid} at {port}: flashed, but no HID tool device with its UID after 30 s", flush=True)
    return bool(new)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--once", action="store_true", help="only what is connected now")
    p.add_argument("--no-build", action="store_true", help="flash the images already built")
    only = p.add_mutually_exclusive_group()
    only.add_argument("--pads", action="store_const", const=0, dest="board", help="pads only")
    only.add_argument("--dongles", action="store_const", const=1, dest="board", help="dongles only")
    a = p.parse_args()
    if not a.no_build:
        subprocess.run(["make", "-C", ROOT, "-j16", *TARGETS.values()], check=True)
    done, skipped, results = set(), set(), []
    print("\nWaiting for devices (Ctrl-C to stop)." if not a.once else "", flush=True)
    try:
        while True:
            for port, pid, uid, board in connected():
                if uid in done or (a.board is not None and board != a.board):
                    continue
                if pid not in TARGETS:
                    if uid not in skipped:
                        print(f"-- {pid} at {port}: no target here (left alone)", flush=True)
                        skipped.add(uid)
                    continue
                done.add(uid)
                results.append((pid, port, uid, flash(port, pid, uid)))
            if a.once:
                break
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    print("\nSummary:")
    for pid, port, uid, ok in results:
        print(f"  {'ok  ' if ok else 'FAIL'} {pid:18} {port:10} {uid}")
    if not results:
        print("  nothing flashed")


if __name__ == "__main__":
    main()
