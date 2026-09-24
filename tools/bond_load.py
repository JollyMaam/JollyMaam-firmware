#!/usr/bin/env python3
"""Put a saved bond back on the pad over the USB console.

Reads a ``BOND`` line written by tools/bond_capture.py (see
/work/cyclone2-bonds/) and replays it:

    tools/bond_load.py -p /dev/bwrap/ttyjl0 /work/cyclone2-bonds/switch2.bond

Three console commands do the work:

    setaddr  <local>   give the pad back the address the console knows
    restore  <slot> <peer> <key> <type>
                       point the slot at the console and store the link key

``setaddr`` restarts the profile on its own, so it runs first and the tool
waits for the console to come back before sending ``restore``.
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from bond_capture import Console  # noqa: E402  (same directory, same job)


def read_bond(path):
    with open(path, "r", errors="replace") as handle:
        text = handle.read()
    match = re.search(r"^BOND local=([0-9a-f:]{17}) peer=([0-9a-f:]{17}) "
                      r"type=(\d+) key=([0-9a-f]{32})", text, re.M)
    if not match:
        raise SystemExit(f"{path}: no BOND line found")
    local, peer, key_type, key = match.groups()
    return local, peer, key_type, key


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--port", default="/dev/bwrap/ttyjl0")
    parser.add_argument("--slot", type=int, default=0)
    parser.add_argument("--no-setaddr", action="store_true",
                        help="leave the pad's current address alone")
    parser.add_argument("--trace", action="store_true",
                        help="also enable the HCI packet trace (hcidump on)")
    parser.add_argument("-o", "--output", help="also write the console to here")
    parser.add_argument("bond")
    parser.add_argument("-q", "--quiet", action="store_true")
    args = parser.parse_args()

    local, peer, key_type, key = read_bond(args.bond)
    print(f"local={local} peer={peer} type={key_type} key={key}",
          file=sys.stderr)

    output = open(args.output, "w") if args.output else open(os.devnull, "w")
    with output:
        console = Console(args.port, output, not args.quiet)
        console.command("debug on", wait=0.5)
        if args.trace:
            console.command("hcidump on", wait=0.5)
        if not args.no_setaddr:
            # This restarts the profile, so give the controller time to come
            # back before the next command.
            console.command(f"setaddr {local}", wait=4.0)
        console.command(f"restore {args.slot} {peer} {key} {key_type}", wait=4.0)
        console.command("connect status", wait=2.0)
        console.command("keys", wait=1.5)

    text = console.text
    for line in text.splitlines():
        if "key slot=" in line or "keys count=" in line or "^slot=" in line:
            print(line.strip(), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
