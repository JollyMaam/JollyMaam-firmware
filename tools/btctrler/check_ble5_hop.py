#!/usr/bin/env python3
"""Check BD19 channel-selection repair arithmetic against original SDK LLVM.

Rebase the SDK's seven frame/shadow GEPs to halfword inputs. The arithmetic,
branches and volatile result writes remain unchanged. Extract the production
C function against the same model. This checks arithmetic, not radio timing.
"""
from pathlib import Path
import argparse
import ctypes as C
import random
import re
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root))
from tools.btctrler.compare import module

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--clang', default=shutil.which('clang'))
parser.add_argument('--cc', default='gcc')
args = parser.parse_args()
if not args.clang:
    parser.error('pass --clang PATH to a host clang')
raw = (root / 'build/controller-ir/bd19/01--RF_ble5.c.o.ll').read_text()
body = module(raw)['canonical']['repair_algorithm2_hop'][1].removesuffix('}')
body = re.sub(r' \{[^}]*\}', '', body)
body = re.sub(r'^%\w+ = getelementptr inbounds %struct.ble_hw,.*$', '', body, flags=re.M)
body = re.sub(r'^%\w+ = load %struct.ble4_hw\*,.*$', '', body, flags=re.M)
slots = {'0, i32 44': 0, '0, i32 25': 1, '0, i32 5': 2,
         '0, i32 6': 3, '0, i32 23': 4, '0, i32 24': 5,
         '58, i32 0': 6, '58, i32 1': 7, '58, i32 2': 8}
seen = set()
def rebase(match):
    fields = match[2]
    assert fields in slots, fields
    seen.add(fields)
    return f'{match[1]} = getelementptr inbounds i16, i16* %arg0, i32 {slots[fields]}'
body = re.sub(r'^(%\w+) = getelementptr inbounds %struct.ble4_hw, %struct.ble4_hw\* %\w+, i32 0, i32 (.*)$', rebase, body, flags=re.M)
assert seen == set(slots), seen
body = body.replace('%struct.ble_hw*', 'i16*')
assert '%struct.' not in body
oracle = '''@config_btctler_le_features = constant i64 16384
define i32 @__get_ble_anchor_con(i16* %hw, i8 %idx) { ret i32 64 }
define void @oracle_hop(i16* %arg0) {
''' + body + '\n}\n'
source = (root / 'src/platform/jieli/btctrler/bd19/RF_ble5.c').read_text()
owned = re.search(r'^BLE5_CODE void repair_algorithm2_hop\([^\n]*\n.*?^}', source, re.M | re.S)[0]
owned = owned.replace('BLE5_CODE ', '')
model = '''#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
struct frame { volatile u16 EVTCOUNT, CHMAP2, BDADDR0, BDADDR1, CHMAP0, CHMAP1; };
struct ble4_hw { struct frame ble_fp; u16 CHMAP[3]; };
struct ble_hw { struct ble4_hw *ble4_hw; };
static const u64 config_btctler_le_features = 16384;
static u32 __get_ble_anchor_con(struct ble_hw *hw, u8 index) { return 64; }
''' + owned + '''
void owned_hop(u16 *p) { struct ble_hw hw = {(void *)p}; repair_algorithm2_hop(&hw); }
'''
with tempfile.TemporaryDirectory(prefix='btctrler-ble5-hop-') as tmp:
    out = Path(tmp)
    (out / 'oracle.ll').write_text(oracle)
    (out / 'owned.c').write_text(model)
    subprocess.run([args.clang, '-Wno-override-module', '-fPIC', '-x', 'ir', '-c', str(out / 'oracle.ll'), '-o', str(out / 'oracle.o')], check=True)
    subprocess.run([args.cc, '-shared', '-fPIC', '-O2', str(out / 'owned.c'), str(out / 'oracle.o'), '-o', str(out / 'check.so')], check=True)
    lib = C.CDLL(str(out / 'check.so'))
    for name in ['oracle_hop', 'owned_hop']:
        getattr(lib, name).argtypes = [C.POINTER(C.c_uint16)]
    rng = random.Random(192619)
    count = 0
    def check(values):
        global count
        a, b = [(C.c_uint16 * 9)(*values) for _ in range(2)]
        lib.oracle_hop(a)
        lib.owned_hop(b)
        assert list(a) == list(b), (values, list(a), list(b))
        count += 1
    for event in range(65536):
        for address in [0, 0x1234, 0xffff]:
            check([event, 37 << 5, address, 0x89ab, 0xabcd, 0x1234, 0x5555, 0xaaaa, 0x15])
    for _ in range(100000):
        check([rng.getrandbits(16) for _ in range(9)])
    print('BLE5 hop repair raw SDK LLVM vs recovered C:', count, 'cases passed')
