#!/usr/bin/env python3
"""Put a saved bond back on the pad and (optionally) reconnect to it.

    tools/bond_load.py /work/cyclone2-bonds/20260925-switch2-STOCK-3.52.bond
    tools/bond_load.py --slot 1 --connect my-switch.bond

Reads a bond file (tools/padbond.py: `export` lines, or an old BOND line),
imports it into the slot, and with --connect pages the console.  The file
carries the pad's address the console knows (config profile.X.address), so
nothing else needs restoring.  Whether it is also written to flash follows
global.autocommit; --no-write sets autocommit off first.
"""

import argparse
import sys

import padbond


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("bond")
    parser.add_argument("--slot", default="A",
                        help="A B X Y up down left right (the Home chord) or 0..7")
    parser.add_argument("--connect", action="store_true",
                        help="page the bonded console afterwards")
    parser.add_argument("--no-write", action="store_true",
                        help="config global.autocommit=false first: RAM only")
    parser.add_argument("--client", default="bond-load")
    args = parser.parse_args()

    lines = padbond.read(args.bond, args.slot)
    pad = padbond.Pad(args.client)
    try:
        if args.no_write:
            pad.cmd("config global.autocommit=false")
        pad.load(lines)
        for line in pad.export(args.slot):
            print(line)
        if args.connect:
            print("\n".join(pad.cmd(f"connect {padbond.slot_name(args.slot)}")))
    except RuntimeError as error:
        print(f"bond_load: {error}", file=sys.stderr)
        return 1
    finally:
        pad.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
