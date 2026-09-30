#!/usr/bin/env python3
"""Pair a slot with a console and save the bond to a file.

    tools/bond_capture.py -o /work/cyclone2-bonds/$(date +%Y%m%d)-switch.bond \\
        --slot 0 [--fresh] [--no-write]

Starts pairing (`pair N`, or `pair N --fresh` for a new address), waits until
the slot is connected, and saves `config get --changed profile.A` - the slot's
type, bond address, peer and keys - to a new file readable only by its owner.
tools/bond_load.py puts it back.

--no-write sets global.autocommit off first, so the pairing is never written
to the pad's flash (a reboot forgets it; the file is then the only copy).

Put the console in Change Grip/Order (Switch) before running it.
"""

import argparse
import sys
import time

import padbond


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-o", "--output", required=True,
                        help="new bond file; an existing one is never replaced")
    parser.add_argument("--slot", default="A",
                        help="A B X Y up down left right (the Home chord) or 0..7")
    parser.add_argument("--fresh", action="store_true",
                        help="pair under a new address (pair N --fresh)")
    parser.add_argument("--no-write", action="store_true",
                        help="config global.autocommit=false first: RAM only")
    parser.add_argument("--pair-wait", type=float, default=180.0,
                        help="seconds to wait for the console")
    parser.add_argument("--client", default="bond-capture")
    args = parser.parse_args()

    pad = padbond.Pad(args.client)
    try:
        if args.no_write:
            pad.cmd("config global.autocommit=false")
        pad.cmd(f"pair {padbond.slot_name(args.slot)}" +
                (" --fresh" if args.fresh else ""))
        print("pairing; put the console in Change Grip/Order", file=sys.stderr)
        deadline = time.monotonic() + args.pair_wait
        while time.monotonic() < deadline:
            if pad.stat().get("state") == "connected":
                break
            time.sleep(1.0)
        else:
            raise RuntimeError("the console did not connect in time")
        # The key arrives before the HID channels open; give the rest of the
        # exchange (the Switch 2 LTK) a moment too.
        time.sleep(2.0)
        lines = pad.export(args.slot)
        if not any(".bond.link_key" in line or ".bond.ltk" in line
                   for line in lines):
            raise RuntimeError("connected, but the slot holds no key")
        padbond.write(args.output, lines)
        print(f"saved {len(lines)} settings to {args.output} (mode 0600)",
              file=sys.stderr)
    except RuntimeError as error:
        print(f"bond_capture: {error}", file=sys.stderr)
        return 1
    finally:
        pad.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
