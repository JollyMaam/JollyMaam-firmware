#!/usr/bin/env python3
"""Dump the whole bring-up marker ring through padbus, grouped by boot.

`status` prints only the newest 96 markers, which is usually not enough to
see how the *previous* boot ended after a reset.  The ring itself holds 2000
entries in RAM that survives a reset; this reads all of it with `peek`.

    tools/ringdump.py                  last 3 boots
    tools/ringdump.py --boots 6

Each entry is (code << 16) | low 16 bits of the boot counter; entries are
printed as 4-digit codes.  Addresses come from build/app.map.
"""

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOG = Path("/work/nslog/padbus.log")
ENTRIES = 2000
CLIENT = "ringdump"


def symbol(name: str) -> int:
    text = (ROOT / "build/app.map").read_text(errors="replace")
    match = re.search(r"0x([0-9a-f]+)\s+" + re.escape(name) + r"\s*$", text, re.M)
    if not match:
        sys.exit(f"{name} not found in build/app.map")
    return int(match.group(1), 16)


def padbus(*args: str) -> None:
    subprocess.run([str(ROOT / "tools/padbus.py"), *args], check=True,
                   stdout=subprocess.DEVNULL)


def peek(address: int, length: int) -> bytes:
    start = LOG.stat().st_size
    padbus("send", CLIENT, f"peek 0x{address:x} {length}")
    want = re.compile(r"^([0-9a-f]{%d})\s*$" % (2 * length), re.M)
    for _ in range(100):
        time.sleep(0.03)
        with LOG.open("rb") as f:
            f.seek(start)
            tail = f.read().decode("utf-8", "replace")
        found = want.search(tail)
        if found:
            return bytes.fromhex(found.group(1))
    sys.exit(f"no answer to peek 0x{address:x}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--boots", type=int, default=3)
    parser.add_argument("--tail", type=int, default=0,
                        help="only the last N markers of each boot")
    args = parser.parse_args()
    ring = symbol("cyclone2_bringup_log")
    head_address = symbol("cyclone2_bringup_log_head")
    padbus("request", CLIENT)
    try:
        head = int.from_bytes(peek(head_address, 4), "little")
        raw = b"".join(peek(ring + offset, 64)
                       for offset in range(0, ENTRIES * 4, 64))
    finally:
        padbus("release", CLIENT)
    values = [int.from_bytes(raw[i:i + 4], "little") for i in range(0, len(raw), 4)]
    count = min(head, ENTRIES)
    ordered = [values[(head - count + i) % ENTRIES] for i in range(count)]
    boots: list[tuple[int, list[int]]] = []
    for value in ordered:
        boot, code = value & 0xffff, value >> 16
        if not boots or boots[-1][0] != boot:
            boots.append((boot, []))
        boots[-1][1].append(code)
    for boot, codes in boots[-args.boots:]:
        print(f"[boot {boot:04x}] {len(codes)} markers")
        if args.tail:
            codes = codes[-args.tail:]
        line = " ".join(f"{c:04x}" for c in codes)
        for i in range(0, len(line), 150):
            print("  " + line[i:i + 150])


if __name__ == "__main__":
    main()
