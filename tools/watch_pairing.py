#!/usr/bin/env python3
"""Keep the controller pairable while capturing the USB console.

Pairing mode is armed by ``pair N`` and the firmware drops out of it after a
minute, so a console-side retry can easily miss the window.  This tool polls
``debug stat`` and re-arms pairing only once the firmware has actually
left pairing mode because that window expired; it never interrupts a pairing
in progress.  Everything the device prints is written to the output file.

    tools/watch_pairing.py -o /work/nslog/pair1.txt -t 600

The port is discovered as ``/dev/bwrap/ttyjlN``; the device re-enumerates
after every profile start, so a fixed path cannot be used.
"""

import argparse
import glob
import os
import re
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from probe_reconnect import Console  # noqa: E402


def find_port():
    devices = sorted(glob.glob("/dev/bwrap/ttyjl*"))
    return devices[0] if devices else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("-t", "--timeout", type=float, default=600.0)
    parser.add_argument("--slot", type=int, default=0)
    parser.add_argument("--poll", type=float, default=3.0)
    parser.add_argument("--no-rearm", action="store_true",
                        help="only observe; never send 'pair'")
    parser.add_argument("--rearm-after", type=float, default=25.0,
                        help="re-arm pairing after this many seconds without "
                             "Bluetooth traffic from the device")
    args = parser.parse_args()

    deadline = time.monotonic() + args.timeout
    with open(args.output, "w") as output:
        console = None
        last = None
        next_poll = 0.0
        quiet_since = time.monotonic()
        while time.monotonic() < deadline:
            if console is None:
                port = find_port()
                if port is None:
                    time.sleep(0.5)
                    continue
                print(f"port={port}")
                output.write(f"\n>>> opened {port}\n")
                console = Console(port, output)
                next_poll = 0.0
                last = None
            try:
                chunk = console.read(time.monotonic() + 1.0)
            except OSError:
                console.close()
                console = None
                continue
            # Anything the device prints about the radio, from either the
            # vendor trace (DEBUG_LOG builds) or this application's own
            # messages.  Status replies are excluded so idle polling does not
            # look like activity.
            if any(marker in chunk for marker in (
                    b"LMP", b"COMM_EDR", b"SDK:", b"hci peer", b"connected",
                    b"encrypted", b"HID", b"Switch", b"profile", b"link key")):
                quiet_since = time.monotonic()
            if time.monotonic() < next_poll:
                continue
            next_poll = time.monotonic() + args.poll
            states = re.findall(r"state=(\w+)", console.text)
            state = states[-1] if states else None
            if state is None and not states:
                # Nothing seen yet: ask the device instead of re-arming, so an
                # already-running pairing window is not reset.
                state = "unknown"
            if state != last:
                print(f"state={state}")
                last = state
            quiet = time.monotonic() - quiet_since
            if args.no_rearm:
                command = "debug stat"
            elif quiet >= args.rearm_after:
                # Nothing has happened on the radio for a while, so whatever
                # the last attempt left behind is stuck; open a fresh window.
                command = f"pair {args.slot}"
                quiet_since = time.monotonic()
            elif state in ("unknown", None, "pairing", "connected"):
                # Nothing to change: either the status is still unknown or a
                # pairing/session is already running.
                command = "debug stat"
            else:
                # ``off``/``connecting``: the pairing window expired without a
                # console, so open another one.
                command = f"pair {args.slot}"
            try:
                console.send(command)
                console.read(time.monotonic() + 2.0)
            except OSError:
                console.close()
                console = None
    return 0


if __name__ == "__main__":
    sys.exit(main())
