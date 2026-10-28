#!/usr/bin/env python3
"""Inspect retained BR23 LLVM code and emit a function/ownership inventory."""
import argparse
from pathlib import Path
from btctrler.legacy_archive import inspect

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--clang', required=True)
    p.add_argument('--ar', required=True)
    args = p.parse_args()
    inspect(args.source.resolve(), args.output.resolve(), args.clang, args.ar)
