#!/usr/bin/env python3
"""Assemble BR23 ELF sections into the load-order app.bin image."""

import argparse
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("sections", nargs="+", type=Path)
    parser.add_argument("--raw", type=Path)
    parser.add_argument("--stock", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    payload = b"".join(section.read_bytes() for section in args.sections)
    if (args.raw is None) != (args.stock is None):
        parser.error("--raw and --stock must be used together")

    output = payload
    if args.stock is not None:
        stock = args.stock.read_bytes()
        if len(payload) > len(stock):
            raise SystemExit(
                f"SDK image is {len(payload)} bytes; slot is {len(stock)} bytes"
            )
        output = payload + stock[len(payload) :]
        args.raw.parent.mkdir(parents=True, exist_ok=True)
        args.raw.write_bytes(payload)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output)
    print(f"SDK load image: {len(payload)} bytes; output: {len(output)} bytes")


if __name__ == "__main__":
    main()
