#!/usr/bin/env python3
"""Assemble BR23 ELF sections and extend them to the stock app.bin slot."""

import argparse
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("sections", nargs="+", type=Path)
    parser.add_argument("--raw", required=True, type=Path)
    parser.add_argument("--stock", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    payload = b"".join(section.read_bytes() for section in args.sections)
    stock = args.stock.read_bytes()
    if len(payload) > len(stock):
        raise SystemExit(
            f"SDK image is {len(payload)} bytes; slot is {len(stock)} bytes"
        )

    args.raw.parent.mkdir(parents=True, exist_ok=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.raw.write_bytes(payload)
    args.output.write_bytes(payload + stock[len(payload) :])
    print(f"SDK load image: {len(payload)} bytes; padded slot: {len(stock)} bytes")


if __name__ == "__main__":
    main()
