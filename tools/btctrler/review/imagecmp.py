#!/usr/bin/env python3
"""Compare the firmware built at two commits, ignoring only the BUILD_ID.

    python3 tools/btctrler/review/imagecmp.py BASE NEW

Builds both commits in one dedicated worktree (/work/cyclone2-imagecmp,
created on first use, detached), so the main checkout and other sessions are
not disturbed; both builds share the directory, so path strings match.
Rebuilds usb.o (BUILD_ID is not a make dependency) and compares
build/text.bin and build/data.bin.  Both commit ids have the same length, so
a translation that LTO reduces to the same code leaves only the hash bytes
different.  Exit 0: identical apart from BUILD_ID.
"""
import fcntl
import os
import subprocess
import sys
from pathlib import Path


WORKTREE = Path('/work/cyclone2-imagecmp')


def git(*args, cwd=None):
    return subprocess.check_output(['git', *args], text=True, cwd=cwd).strip()


def build(commit):
    subprocess.run(['git', 'checkout', '-q', '--detach', commit], check=True, cwd=WORKTREE)
    (WORKTREE / 'src/usb.c').touch()
    env = dict(os.environ, PATH='/work/bin:' + os.environ['PATH'])
    subprocess.run(['make', '-j32'], check=True, env=env, cwd=WORKTREE,
                   stdout=subprocess.DEVNULL)
    ident = git('describe', '--always', '--dirty', cwd=WORKTREE)
    return {f: (WORKTREE / 'build' / f).read_bytes() for f in ('text.bin', 'data.bin')}, ident


def main():
    if len(sys.argv) != 3 or sys.argv[1].startswith('-'):
        sys.exit(__doc__)
    base, new = (git('rev-parse', c) for c in sys.argv[1:3])
    # One shared worktree: serialize concurrent runs (reviewer and workers).
    lock = open(Path(git('rev-parse', '--git-common-dir')).resolve() / 'imagecmp.lock', 'a')
    fcntl.flock(lock, fcntl.LOCK_EX)
    if not WORKTREE.exists():
        subprocess.run(['git', 'worktree', 'add', '-q', '--detach', str(WORKTREE), base],
                       check=True)
    a, ida = build(base)
    b, idb = build(new)
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
