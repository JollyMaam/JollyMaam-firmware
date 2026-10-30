#!/usr/bin/env python3
"""Compare jlnewfw image behavior against a separately built reference.

Pass --reference OLD_BINARY --candidate NEW_BINARY IMAGE [IMAGE ...].
Only image commands run; no device access occurs. Temporary outputs are removed.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(tool, *args):
    result = subprocess.run([str(tool), *map(str, args)], capture_output=True)
    if result.returncode:
        raise AssertionError(f"{tool}: {args}: {result.stderr.decode(errors='replace')}")
    return result.stdout, result.stderr


def files(folder):
    return {p.relative_to(folder): p.read_bytes()
            for p in folder.rglob('*') if p.is_file()}


def compare(reference, candidate, image):
    for command in ('list', 'verify'):
        assert run(reference, command, image) == run(candidate, command, image), (image, command)
    with tempfile.TemporaryDirectory(prefix='jollymaam-images-') as temp:
        root = Path(temp)
        old, new = root / 'old', root / 'new'
        assert run(reference, 'unpack', image, old) == run(candidate, 'unpack', image, new)
        assert files(old) == files(new), f"unpacked files differ: {image}"
    print(f"Matched list, verify and unpack: {image}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', required=True, type=Path)
    parser.add_argument('--candidate', required=True, type=Path)
    parser.add_argument('images', nargs='+', type=Path)
    args = parser.parse_args()
    for image in args.images:
        compare(args.reference.resolve(), args.candidate.resolve(), image.resolve())


if __name__ == '__main__':
    main()
