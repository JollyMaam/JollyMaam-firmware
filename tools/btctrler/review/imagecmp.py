#!/usr/bin/env python3
"""Compare the firmware built at two commits, ignoring only the BUILD_ID.

    python3 tools/btctrler/review/imagecmp.py BASE NEW

Run in the main checkout with a clean tree (worktrees embed different path
strings, so their layouts differ).  Checks out each commit, rebuilds usb.o
(BUILD_ID is not a make dependency), builds, and compares build/text.bin and
build/data.bin.  Both commit ids have the same length, so a translation that
LTO reduces to the same code leaves only the hash bytes different.  Restores
the original HEAD afterwards.  Exit 0: identical apart from BUILD_ID.
"""
import os
import subprocess
import sys
from pathlib import Path


def git(*args):
    return subprocess.check_output(['git', *args], text=True).strip()


def build(commit):
    subprocess.run(['git', 'checkout', '-q', commit], check=True)
    Path('src/usb.c').touch()
    env = dict(os.environ, PATH='/work/bin:' + os.environ['PATH'])
    subprocess.run(['make', '-j32'], check=True, env=env, stdout=subprocess.DEVNULL)
    ident = git('describe', '--always', '--dirty')
    return {f: Path('build', f).read_bytes() for f in ('text.bin', 'data.bin')}, ident


def main():
    base, new = sys.argv[1:3]
    if git('status', '--porcelain', '--untracked-files=no'):
        sys.exit('imagecmp: tracked changes in the checkout; commit or stash first')
    head = git('symbolic-ref', '-q', '--short', 'HEAD') or git('rev-parse', 'HEAD')
    try:
        a, ida = build(base)
        b, idb = build(new)
    finally:
        subprocess.run(['git', 'checkout', '-q', head], check=True)
    if len(ida) != len(idb):
        sys.exit(f'imagecmp: build ids differ in length ({ida}, {idb})')
    same = True
    for name in a:
        x, y = a[name], b[name]
        if len(x) != len(y):
            print(f'{name}: size {len(x)} -> {len(y)}')
            same = False
            continue
        diff = [i for i in range(len(x)) if x[i] != y[i]]
        # Allow differences only inside the build id string.
        at = x.find(ida.encode())
        rest = [i for i in diff if not (at >= 0 and at <= i < at + len(ida))]
        print(f'{name}: {len(diff)} bytes differ, {len(rest)} outside BUILD_ID')
        if rest:
            same = False
            print('  first offsets:', ', '.join(hex(i) for i in rest[:16]))
    print('IDENTICAL apart from BUILD_ID' if same else 'DIFFERENT')
    sys.exit(0 if same else 1)


if __name__ == '__main__':
    main()
