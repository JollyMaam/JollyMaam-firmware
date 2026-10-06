#!/usr/bin/env python3
"""Compare owned C against the SDK bodies it replaced, as a review aid.

    crosscheck.py [FUNCTION...]

For each owned function (the fingerprint tables in recover.py), lists the
calls and struct fields (type, GEP index path) that only one side uses:
the SDK body from build/controller-ir/sdk/*.ll, the owned one from the
build's bitcode in build/src/btctrler/*.o.  Inlining and helpers make some
differences expected; a field present on one side only, of the same struct,
is the thing to read (e.g. payload_head vs data in bredr_tx_bulk).
Needs `make` and `make controller-ir` first.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLANG = '/shared/jieli-linux-toolchains-20250324.1/pi32v2/bin/clang'
QUIET = re.compile(r'^(llvm\.|printf$|log_print$|cpu_assert_debug$|cpu_reset$|btctrler_assert_failed$|puts$)')


def owned_names():
    text = (ROOT / 'tools/btctrler/recover.py').read_text()
    names = set(re.findall(r"^\s+'(\w+)': '[0-9a-f]{64}'", text, re.M))
    names |= set(re.findall(r"(?:extract|own)\(text, '(\w+)'", text))
    return names


def bodies(text):
    out = {}
    for m in re.finditer(r'^define [^\n]*?@([\w.]+)\(.*?\n\}', text, re.M | re.S):
        out[m.group(1)] = m.group(0)
    return out


def struct_types(text):
    """Struct name -> list of member types (top level), for GEP chains."""
    return {m.group(1): m.group(2) for m in
            re.finditer(r'^%(struct\.[\w.]+) = type (?:<)?\{ (.*) \}', text, re.M)}


def norm(name):
    return re.sub(r'\.\d+$', '', name)


def summary(body):
    calls = {c for c in re.findall(r'call [^@\n]*?@([\w.]+)(?:\(| to )', body) if not QUIET.match(c)}
    fields = set()
    for m in re.finditer(r'getelementptr (?:inbounds )?%(struct\.[\w.]+), [^,]+, i32 0((?:, i32 \d+)+)', body):
        path = tuple(int(x) for x in re.findall(r'i32 (\d+)', m.group(2)))
        fields.add((norm(m.group(1)), path))
    # Byte-offset accesses (i8 GEPs off a struct) can't be compared by name.
    return calls, fields


def load(paths):
    """Function -> (body, file, {same-file static helpers' bodies})."""
    out = {}
    for p in paths:
        text = p.read_text() if p.suffix == '.ll' else subprocess.run(
            [CLANG, '-target', 'pi32v2', '-x', 'ir', '-S', '-emit-llvm', str(p), '-o', '-'],
            check=True, capture_output=True, text=True).stdout
        defs = bodies(text)
        for name, body in defs.items():
            out.setdefault(name, (body, p.name, defs))
    return out


def expanded(name, entry):
    """A function's summary with its same-file internal helpers folded in,
    so inlining choices don't show up as differences."""
    body, _, defs = entry
    calls, fields = summary(body)
    seen, todo = {name}, list(calls)
    while todo:
        callee = todo.pop()
        if callee in seen or callee not in defs or not re.match(r'define internal', defs[callee]):
            continue
        seen.add(callee)
        c, f = summary(defs[callee])
        calls |= c; fields |= f; todo += c
    return calls - seen, fields


def main():
    names = sys.argv[1:] or sorted(owned_names())
    sdk = load(sorted((ROOT / 'build/controller-ir/sdk').glob('*.ll')))
    own = load(sorted((ROOT / 'build/src/btctrler').glob('*.o')))
    for name in names:
        if name not in sdk or name not in own:
            print(f'{name}: missing on {"SDK" if name not in sdk else "owned"} side')
            continue
        (sc, sf), (oc, of) = expanded(name, sdk[name]), expanded(name, own[name])
        diff = []
        if sc - oc: diff.append(f'  calls SDK only:   {" ".join(sorted(sc - oc))}')
        if oc - sc: diff.append(f'  calls owned only: {" ".join(sorted(oc - sc))}')
        if sf - of: diff.append(f'  fields SDK only:   {" ".join(f"{s[7:]}{list(p)}" for s, p in sorted(sf - of))}')
        if of - sf: diff.append(f'  fields owned only: {" ".join(f"{s[7:]}{list(p)}" for s, p in sorted(of - sf))}')
        if diff:
            print(f'{name} ({sdk[name][1]} -> {own[name][1]})')
            print('\n'.join(diff))


if __name__ == '__main__':
    main()
