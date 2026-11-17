#!/usr/bin/env python3
"""Differentially check BD19 bredr_frame math/table helpers against original IR.

The TWS seed test covers every 16-bit input. The other arithmetic tests use
fixed edges and deterministic random 32-bit inputs; AFH packing tests cover all
single-bit maps, all-ones, and random 10-byte maps. Phase/forward/preserve
checks cover all16-bit inputs at selected boundaries. The preserve function's
frame-offset GEP is rebased to one scalar; extracted C uses32-bit unsigned
longs to model q32s. This supplements the actual-target LLVM trace checker;
it does not validate MMIO.
"""
from pathlib import Path
import argparse
import ctypes as C
import random
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--clang', default=shutil.which('clang'))
parser.add_argument('--cc', default='gcc')
args = parser.parse_args()
if not args.clang:
    parser.error('pass --clang PATH to a host clang')
raw = (root / 'build/controller-ir/bd19/01--bredr_frame.c.o.ll').read_text()
source = (root / 'src/platform/jieli/btctrler/bd19/bredr_frame.c').read_text()


def sdk_function(name):
    match = re.search(r'^define[^\n]*@' + re.escape(name) + r'\([^\n]*\)[^{]*\{.*?^}', raw, re.M | re.S)
    assert match, name
    body = match.group()
    body = '\n'.join(line for line in body.splitlines() if '@llvm.dbg.' not in line)
    body = re.sub(r',? ![\w.]+ !\d+', '', body)
    body = re.sub(r'#\d+', '', body)
    return body


def owned_function(name):
    match = re.search(r'^[^\n]*\b' + re.escape(name) + r'\([^;]*?\n\{.*?^}', source, re.M | re.S)
    assert match, name
    return match.group()


oracle_math = '\n'.join(sdk_function(n) for n in
                         ['sync_generation', 'acc_rand_1', 'tws_acc_generation'])
for name in ['sync_generation', 'acc_rand_1', 'tws_acc_generation']:
    oracle_math = oracle_math.replace('@' + name, '@oracle_' + name)
oracle_afh = sdk_function('bdhw_set_frq_table').replace('@bdhw_set_frq_table', '@oracle_bdhw_set_frq_table')
oracle_phase = sdk_function('clkn0_phase').replace('@clkn0_phase', '@oracle_clkn0_phase')
oracle_preserve = sdk_function('preserve_bitoff_cal')
oracle_preserve = oracle_preserve.replace('%struct.ctrl_frame* nocapture readonly', 'i32*')
# The SDK only reads ctrl_frame.clk_offset here; rebase that target field to arg0.
oracle_preserve = re.sub(r'  %6 = getelementptr[^\n]*\n  %7 = load i32, i32\* %6, align 4',
                         '  %7 = load i32, i32* %0, align 4', oracle_preserve)
assert '%struct.ctrl_frame' not in oracle_preserve
oracle_preserve = oracle_preserve.replace('@clkn0_phase', '@oracle_clkn0_phase')
oracle_forward = sdk_function('s_forward_offset_cal').replace('%struct.ctrl_frame* nocapture readnone', 'i8*').replace('@clkn0_phase', '@oracle_clkn0_phase')
for name in ['preserve_bitoff_cal', 's_forward_offset_cal']:
    if name == 'preserve_bitoff_cal': oracle_preserve = oracle_preserve.replace('@preserve_bitoff_cal', '@oracle_preserve_bitoff_cal')
    else: oracle_forward = oracle_forward.replace('@s_forward_offset_cal', '@oracle_s_forward_offset_cal')
# LLVM 21 changed the overloaded memset intrinsic name and length type.
for name in ['oracle_bdhw_set_frq_table']:
    oracle_afh = oracle_afh.replace('llvm.memset.p0i8.i32', 'llvm.memset.p0.i64')
oracle_afh = re.sub(r'(@llvm\.memset\.p0\.i64\(i8\* [^,]+, i8 0, )i32 (\d+), i32 1, i1 false\)',
                    r'\1i64 \2, i1 false)', oracle_afh)
if '@llvm.memset.p0.i64' in oracle_afh:
    oracle_afh += '\ndeclare void @llvm.memset.p0.i64(i8*, i8, i64, i1)'

helpers = '\n'.join(owned_function(n) for n in
                     ['bit_count_range', 'transition_count', 'sync_generation', 'acc_rand_1', 'tws_acc_generation'])
math_c = helpers.replace('unsigned long', 'uint32_t')
for name in ['sync_generation', 'acc_rand_1', 'tws_acc_generation']:
    math_c = math_c.replace(name + '(', 'owned_' + name + '(')
af_h_c = owned_function('bdhw_set_frq_table').replace('bdhw_set_frq_table', 'owned_bdhw_set_frq_table')
width_c = '\n'.join(owned_function(n) for n in ['clkn0_phase', 'preserve_bitoff_cal', 's_forward_offset_cal'])
width_c = width_c.replace('RF_CODE ', '').replace('clkn0_phase(', 'owned_clkn0_phase(')
width_c = width_c.replace('preserve_bitoff_cal(', 'owned_preserve_bitoff_cal(').replace('s_forward_offset_cal(', 'owned_s_forward_offset_cal(')
af_h_c = af_h_c.replace('RF_CODE ', '')
owned_c = '''#include <stdint.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int8_t s8; typedef int16_t s16; typedef int32_t s32;\nstruct ctrl_frame { u32 clk_offset; };\n''' + math_c + '\n' + af_h_c + '\n' + width_c + '''\nvoid owned_preserve(u32 clk, u32 bitreg, u16 time, u32 *co, u16 *bo) { struct ctrl_frame f={clk}; owned_preserve_bitoff_cal(&f,bitreg,co,bo,time); }\nvoid owned_forward(u32 *co, u16 *bo, u16 sniffer, u8 mode) { struct ctrl_frame f={0}; owned_s_forward_offset_cal(&f,co,bo,sniffer,mode); }\n'''

with tempfile.TemporaryDirectory(prefix='btctrler-bredr-frame-') as tmp:
    out = Path(tmp)
    (out / 'oracle.ll').write_text(oracle_math + '\n' + oracle_afh + '\n' + oracle_phase + '\n' + oracle_preserve + '\n' + oracle_forward + '''\ndefine void @oracle_preserve(i32 %clk, i32 %bitreg, i16 %time, i32* %co, i16* %bo) {\n %ptr = alloca i32\n store i32 %clk, i32* %ptr\n call void @oracle_preserve_bitoff_cal(i32* %ptr, i32 %bitreg, i32* %co, i16* %bo, i16 %time)\n ret void\n}\ndefine void @oracle_forward(i32* %co, i16* %bo, i16 %sniffer, i8 %mode) {\n call void @oracle_s_forward_offset_cal(i8* null, i32* %co, i16* %bo, i16 %sniffer, i8 %mode)\n ret void\n}\n''')
    (out / 'owned.c').write_text(owned_c)
    (out / 'oracle.o').unlink(missing_ok=True)
    subprocess.run([args.clang, '-Wno-override-module', '-fPIC', '-x', 'ir', '-c',
                    str(out / 'oracle.ll'), '-o', str(out / 'oracle.o')], check=True)
    subprocess.run([args.cc, '-shared', '-fPIC', '-O2', str(out / 'owned.c'),
                    str(out / 'oracle.o'), '-o', str(out / 'check.so')], check=True)
    lib = C.CDLL(str(out / 'check.so'))
    for name in ['sync_generation', 'acc_rand_1', 'tws_acc_generation']:
        oracle_fn = getattr(lib, 'oracle_' + name)
        owned_fn = getattr(lib, 'owned_' + name)
        oracle_fn.restype = owned_fn.restype = C.c_uint8 if name == 'sync_generation' else C.c_uint32
        oracle_fn.argtypes = owned_fn.argtypes = [C.c_uint16 if name == 'tws_acc_generation' else C.c_uint32]
    for name in ['oracle_clkn0_phase', 'owned_clkn0_phase']:
        getattr(lib, name).argtypes = [C.c_int16]
        getattr(lib, name).restype = C.c_uint8
    lib.oracle_preserve.argtypes = lib.owned_preserve.argtypes = [C.c_uint32,C.c_uint32,C.c_uint16,C.POINTER(C.c_uint32),C.POINTER(C.c_uint16)]
    lib.oracle_forward.argtypes = lib.owned_forward.argtypes = [C.POINTER(C.c_uint32),C.POINTER(C.c_uint16),C.c_uint16,C.c_uint8]
    for name in ['oracle_bdhw_set_frq_table', 'owned_bdhw_set_frq_table']:
        getattr(lib, name).argtypes = [C.POINTER(C.c_uint8)] * 3

    checked_math = 0
    def check_math(name, value):
        global checked_math
        a = getattr(lib, 'oracle_' + name)(value)
        b = getattr(lib, 'owned_' + name)(value)
        assert a == b, (name, hex(value), a, b)
        checked_math += 1

    for seed in range(65536):
        check_math('tws_acc_generation', seed)
    edges = [0, 1, 0x11111111, 0x8e89bed6, 0xffffffff, 0xdeadbeef, 0x01000001]
    rng = random.Random(0xBD19)
    values = edges + [rng.getrandbits(32) for _ in range(100000)]
    for value in values:
        check_math('sync_generation', value)
        check_math('acc_rand_1', value)

    checked_maps = 0
    freq_a, packed_a = (C.c_uint8 * 80)(), (C.c_uint8 * 10)()
    freq_b, packed_b = (C.c_uint8 * 80)(), (C.c_uint8 * 10)()
    map_a = (C.c_uint8 * 10)()
    def check_map(values):
        global checked_maps
        for i, value in enumerate(values):
            map_a[i] = value
        lib.oracle_bdhw_set_frq_table(freq_a, map_a, packed_a)
        lib.owned_bdhw_set_frq_table(freq_b, map_a, packed_b)
        assert bytes(freq_a) == bytes(freq_b), (values, bytes(freq_a), bytes(freq_b))
        assert bytes(packed_a) == bytes(packed_b), (values, bytes(packed_a), bytes(packed_b))
        checked_maps += 1
    for byte in range(10):
        for bit in range(8):
            values = [0] * 10
            values[byte] = 1 << bit
            check_map(values)
    check_map([0xff] * 10)
    for _ in range(100000):
        check_map([rng.randrange(256) for _ in range(10)])
    checked_widths = 0
    for phase in range(65536):
        expected = lib.oracle_clkn0_phase(C.c_int16(phase).value)
        actual = lib.owned_clkn0_phase(C.c_int16(phase).value)
        assert expected == actual, ('phase',phase,expected,actual)
        checked_widths += 1
    for bitreg in range(65536):
        for time in (0,1,624,625,626,1249,1250,65535):
            ca, cb = C.c_uint32(), C.c_uint32(); ba, bb = C.c_uint16(), C.c_uint16()
            lib.oracle_preserve(0xdeadbeef,bitreg,time,C.byref(ca),C.byref(ba))
            lib.owned_preserve(0xdeadbeef,bitreg,time,C.byref(cb),C.byref(bb))
            assert (ca.value,ba.value)==(cb.value,bb.value), ('preserve',bitreg,time,ca.value,ba.value,cb.value,bb.value)
            checked_widths += 1
    for sniffer in range(65536):
        for mode in (0,1):
            ca,cb=C.c_uint32(0xfedcba98),C.c_uint32(0xfedcba98); ba,bb=C.c_uint16(0xffff),C.c_uint16(0xffff)
            lib.oracle_forward(C.byref(ca),C.byref(ba),sniffer,mode)
            lib.owned_forward(C.byref(cb),C.byref(bb),sniffer,mode)
            assert (ca.value,ba.value)==(cb.value,bb.value), ('forward',sniffer,mode,ca.value,ba.value,cb.value,bb.value)
            checked_widths += 1
    print(f'BD19 bredr_frame raw SDK LLVM differential: {checked_math} math cases, {checked_maps} AFH maps, and {checked_widths} width cases passed')
