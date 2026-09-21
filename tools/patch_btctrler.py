#!/usr/bin/env python3
"""Patch the BR23 controller archive's remote sniff veto.

The Jieli archive is LLVM bitcode. Replace only ``lmp_esco_sniff_check`` in
LLVM IR and re-emit that member as bitcode so the vendor archive's LTO symbol
index and the other members remain usable by the linker.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import tempfile
from pathlib import Path


FUNCTION = "lmp_esco_sniff_check"


def run(command: list[str], cwd: Path | None = None) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def patch_bitcode(source: Path, destination: Path, clang: str) -> None:
    ir = source.with_suffix(".ll")
    run([
        clang,
        "-target",
        "pi32v2",
        "-x",
        "ir",
        "-S",
        "-emit-llvm",
        str(source),
        "-o",
        str(ir),
    ])
    lines = ir.read_text().splitlines()

    start = next(
        (index for index, line in enumerate(lines)
         if re.match(rf"^define\b.*@{FUNCTION}\(", line)),
        None,
    )
    if start is None:
        raise RuntimeError(f"{FUNCTION} was not found in {source}")

    return_match = re.match(
        r"^define(?:\s+\w+)*\s+(i\d+|void)\s+@",
        lines[start],
    )
    if return_match is None:
        raise RuntimeError(f"could not determine the return type of {FUNCTION}")

    end = next(
        (index for index in range(start + 1, len(lines)) if lines[index] == "}"),
        None,
    )
    if end is None:
        raise RuntimeError(f"end of {FUNCTION} was not found")

    lines[start:end + 1] = [
        lines[start],
        "entry:",
        f"  ret {return_match.group(1)} 1",
        "}",
    ]
    ir.write_text("\n".join(lines) + "\n")
    run([clang, "-target", "pi32v2", "-x", "ir", "-c", "-emit-llvm", str(ir), "-o", str(destination)])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--ar", default="ar")
    args = parser.parse_args()
    args.source = args.source.resolve()
    args.destination = args.destination.resolve()

    members = subprocess.run(
        [args.ar, "t", str(args.source)],
        check=True,
        text=True,
        capture_output=True,
    ).stdout.splitlines()
    if "lmp.c.o" not in members:
        raise RuntimeError("btctrler archive has no lmp.c.o member")

    args.destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cyclone2-btctrler-") as temp_name:
        temp = Path(temp_name)
        run([args.ar, "x", str(args.source)], cwd=temp)
        patch_bitcode(temp / "lmp.c.o", temp / "lmp.c.o.patched", args.clang)
        (temp / "lmp.c.o.patched").replace(temp / "lmp.c.o")
        run([args.ar, "rcs", str(args.destination), *members], cwd=temp)


if __name__ == "__main__":
    main()
