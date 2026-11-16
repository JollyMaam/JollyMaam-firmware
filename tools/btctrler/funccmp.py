#!/usr/bin/env python3
"""Compare compiled owned C with an original SDK member, function by function.

    python3 tools/btctrler/funccmp.py SDK.ll OBJECT.o [FUNCTION...] [-d]

OBJECT is LTO bitcode (decoded with the q32s clang) or textual IR. Each body
is canonicalized with compare.canonical_function; private constants (strings)
are replaced by their initializers, `.llvm.N` suffixes, `nobuiltin` call
attributes and K&R-declaration bitcasts are ignored. SAME is exact canonical
equality. DIFF is triage only: -d prints a diff with value/block numbers
erased, which is where reordering, inlining and branch polarity show up.
Build OBJECT with build/vanilla-bd19-os/... for -Os inlining like the SDK.
"""
import difflib
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.btctrler import compare  # noqa: E402

CLANG = 'external/toolchains/jieli-linux-toolchains-20250324.1/q32s/bin/clang'


def load(path):
    if path.endswith('.o'):
        return subprocess.check_output([CLANG, '-target', 'q32s', '-x', 'ir', '-S',
                                        '-emit-llvm', path, '-o', '-'], text=True)
    return Path(path).read_text()


def normalized(text):
    mod = compare.module(text)
    private = {}
    for name, line in mod['globals'].items():
        if re.search(r'= (private|internal) unnamed_addr constant', line):
            value = re.sub(r'^@[\w.$-]+ = ', '', line)
            private[name] = re.sub(r', align \d+.*$', '', value)
    result = {}
    for name, canon in mod['canonical'].items():
        body = '\n'.join(canon) if isinstance(canon, tuple) else canon
        body = re.sub(r'@([\w.$-]+)', lambda m: '@<' + private[m[1]] + '>'
                      if m[1] in private else m[0], body)
        body = re.sub(r'\.llvm\.\d+', '', body).replace('nobuiltin ', '')
        body = re.sub(r'bitcast \(([^()]*?) \(\.\.\.\)\* (@[\w.$-]+) to [^()]*\([^()]*\)\*\)',
                      r'\2', body)
        result[re.sub(r'\.llvm\.\d+', '', name)] = body
    return result


def main(argv):
    args = [a for a in argv if not a.startswith('-')]
    show = '-d' in argv
    sdk, ours = normalized(load(args[0])), normalized(load(args[1]))
    for name in args[2:] or sorted(sdk):
        if name not in sdk:
            print('NOTSDK ', name)
        elif name not in ours:
            print('MISSING', name)
        elif sdk[name] == ours[name]:
            print('SAME   ', name)
        else:
            print('DIFF   ', name)
            if show:
                a, b = sdk[name].split('\n', 1), ours[name].split('\n', 1)
                if a[0] != b[0]:
                    ta, tb = set(a[0].split()), set(b[0].split())
                    print('  header sdk-only:', ' '.join(sorted(ta - tb)),
                          '| ours-only:', ' '.join(sorted(tb - ta)))
                loose = [re.sub(r'%(v|bb)\d+', r'%\1', x[1] if len(x) > 1 else '')
                         for x in (a, b)]
                sys.stdout.writelines(difflib.unified_diff(
                    loose[0].splitlines(True), loose[1].splitlines(True), 'sdk', 'ours', n=2))
    if not args[2:]:
        for name in sorted(set(ours) - set(sdk)):
            print('EXTRA  ', name)


if __name__ == '__main__':
    main(sys.argv[1:])
