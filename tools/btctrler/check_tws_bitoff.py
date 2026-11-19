#!/usr/bin/env python3
"""Compare BD19 TWS bit-offset arithmetic with the isolated SDK LLVM body.

The frame GEPs are rebased to single input halfwords. No other operation in
that SDK body changes. This checks arithmetic only, not scheduling or MMIO.
"""
from pathlib import Path
import argparse
import ctypes as C
import random
import re
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--clang', default=shutil.which('clang'))
parser.add_argument('--cc', default='gcc')
args = parser.parse_args()
if not args.clang:
    parser.error('pass --clang PATH to a host clang')
root = Path(__file__).resolve().parents[2]
raw = (root / 'build/controller-ir/bd19/01--tws_link.c.o.ll').read_text()
f = re.search(r'^define[^\n]*@tws_link_cacl_bitoff\([^\n]*\n.*?^}', raw, re.M | re.S)[0]
f = '\n'.join(line for line in f.splitlines() if '@llvm.dbg.' not in line)
f = re.sub(r',? ![\w.]+ !\d+', '', f)
f = re.sub(r'#\d+', '', f)
f = f.replace('@tws_link_cacl_bitoff', '@oracle_bitoff')
f = re.sub(r'getelementptr inbounds %struct.ctrl_frame, %struct.ctrl_frame\* (%\w+), i32 0, i32 96', r'getelementptr inbounds i16, i16* \1, i32 0', f)
f = f.replace('%struct.ctrl_frame*', 'i16*')
assert '%struct.' not in f, 'SDK input layout changed; re-review the extraction'
source = (root / 'src/platform/jieli/btctrler/bd19/tws_link.c').read_text()
g = re.search(r'^void TWS_CODE tws_link_cacl_bitoff\([^;]*?\n\{.*?^}', source, re.M | re.S)[0]
g = g.replace('void TWS_CODE tws_link_cacl_bitoff', 'void owned_bitoff')
g = '#include <stdint.h>\ntypedef uint16_t u16;typedef int16_t s16;struct ctrl_frame { u16 bitoff; };\n' + g
with tempfile.TemporaryDirectory(prefix='btctrler-tws-bitoff-') as tmp:
    out = Path(tmp)
    (out / 'oracle.ll').write_text(f + '\n')
    (out / 'owned.c').write_text(g + '\n')
    subprocess.run([args.clang, '-Wno-override-module', '-fPIC', '-x', 'ir', '-c', str(out / 'oracle.ll'), '-o', str(out / 'oracle.o')], check=True)
    subprocess.run([args.cc, '-shared', '-fPIC', '-O2', str(out / 'owned.c'), str(out / 'oracle.o'), '-o', str(out / 'check.so')], check=True)
    lib = C.CDLL(str(out / 'check.so'))
    for name in ['oracle_bitoff', 'owned_bitoff']:
        getattr(lib, name).argtypes = [C.POINTER(C.c_uint16)] * 3
    a, b, x, y = [C.c_uint16() for _ in range(4)]
    count = 0
    def check(av, bv):
        global count
        a.value, b.value = av, bv
        lib.oracle_bitoff(C.byref(a), C.byref(b), C.byref(x))
        lib.owned_bitoff(C.byref(a), C.byref(b), C.byref(y))
        assert x.value == y.value, (av, bv, x.value, y.value)
        count += 1
    for av in range(65536):
        for bv in [0, 624, 1249, 32767, 32768, 65535]:
            check(av, bv)
    rng = random.Random(192619)
    for _ in range(100000):
        check(rng.getrandbits(16), rng.getrandbits(16))
    print('TWS bitoff raw SDK LLVM vs recovered C:', count, 'cases passed')
