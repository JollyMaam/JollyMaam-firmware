#!/usr/bin/env python3
"""Compare BD19 widening arithmetic and modeled writes with original SDK LLVM.

Requires controller IR and the recovered RF_ble target object. Only the two
window halfwords are rebased; hardware reads, writes, IRQ and sync operations
become ordered model events. Clock/bit-offset samples and drift-enable calls
are modeled boundaries. This does not establish real timing or IRQ safety.
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
from tools.btctrler.compare import module, parameter_bounds, split_parameters

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--clang', default=shutil.which('clang'))
parser.add_argument('--cc', default='gcc')
parser.add_argument('--output-dir', type=Path)
args = parser.parse_args()
if not args.clang:
    parser.error('pass --clang PATH to host clang')

def isolate(text, helpers=False):
    m = module(text)
    parts = ['''%struct.ble_hw = type { i16* }
%struct.ble_hw_base = type { [8 x i16], %struct.ble_hw* }
%struct.ble_baseband_layer_handle = type { %struct.ble_hw_base }
@bb_hdl = external global %struct.ble_baseband_layer_handle*
declare i32 @__get_clkn(%struct.ble_hw*)
declare void @local_irq_disable(...)
declare void @local_irq_enable(...)
declare void @model_sync()
declare i32 @model_read(i32)
declare void @model_write(i32,i32)
declare void @model_frame_write(i16*,i16)
declare i32 @model_putchar(i32)
''']
    if helpers:
        names = ['HW_ID', '__set_ble_anchor_con', '__set_bitoffset',
                 't_get_bitoffset', '__set_widen_drift_enable']
    else:
        parts.append('declare signext i16 @t_get_bitoffset(%struct.ble_hw*)\n'
                     'declare void @__set_widen_drift_enable(%struct.ble_hw*,i8 zeroext)')
        names = ['HW_ID', '__set_ble_anchor_con', '__set_bitoffset', '__hw_set_widen_drift']
    for name in names:
        signature, body = m['canonical'][name]
        signature = re.sub(r' \{[^}]*\}', '', signature)
        signature = signature.replace('internal ', '').replace('fastcc ', '')
        start, end = parameter_bounds(signature)
        parameters = split_parameters(signature[start:end])
        signature = signature[:start] + ', '.join(f'{p} %arg{i}' for i, p in enumerate(parameters)) + signature[end:]
        body = re.sub(r' \{[^}]*\}', '', body).replace('fastcc ', '')
        body = re.sub(r'getelementptr inbounds %struct.ble4_hw, %struct.ble4_hw\* (%\w+), i32 0, i32 0, i32 (17|18)',
                      lambda x: f'getelementptr inbounds i16, i16* {x[1]}, i32 {int(x[2])-17}', body)
        body = body.replace('%struct.ble4_hw*', 'i16*')
        body = body.replace('@putchar(', '@model_putchar(')
        lines = []
        for line in body.splitlines():
            if 'call void asm sideeffect' in line:
                assert 'csync' in line, line
                line = 'call void @model_sync()'
            load = re.fullmatch(r'(%\w+ = )load volatile i32, i32\* inttoptr \(i32 (\d+) to i32\*\), align \d+', line)
            store = re.fullmatch(r'store volatile i32 ([%\w-]+), i32\* inttoptr \(i32 (\d+) to i32\*\), align \d+', line)
            frame = re.fullmatch(r'store volatile i16 ([%\w-]+), i16\* (%\w+), align \d+', line)
            if load:
                line = f'{load[1]}call i32 @model_read(i32 {load[2]})'
            elif store:
                line = f'call void @model_write(i32 {store[2]}, i32 {store[1]})'
            elif frame:
                line = f'call void @model_frame_write(i16* {frame[2]}, i16 {frame[1]})'
            lines.append(line)
        body = '\n'.join(lines)
        assert not re.search(r'(?:load|store) volatile ', body), (name, body)
        assert '%struct.ble4_hw' not in body, (name, body)
        parts.append('define ' + signature + ' {\n' + body)
    return '\n\n'.join(parts) + '\n'

driver = r'''
#include <stdint.h>
#include <stdlib.h>
struct hw { uint16_t *frame; };
struct base { uint16_t inst[8]; struct hw *hw; };
static struct base base;
struct base *bb_hdl = &base;
static struct hw hw;
static uint16_t frame[4];
static uint32_t clk, ra, rb, rc;
static int16_t bitoff;
static int replace;
static uint64_t trace;
static uint32_t events;
static int irq_mutate;
static struct hw *new_hw_base;
static void event(uint32_t a, uint32_t b) {
    trace = (trace ^ a) * UINT64_C(1099511628211);
    trace = (trace ^ b) * UINT64_C(1099511628211);
    ++events;
}
uint32_t __get_clkn(struct hw *p) {
    event(1, clk);
    if (replace) p->frame = frame + 2;
    return clk;
}
#ifndef HELPER_MODE
int16_t t_get_bitoffset(struct hw *p) { event(2, (uint16_t)bitoff); return bitoff; }
void __set_widen_drift_enable(struct hw *p, uint8_t en) { event(3, en); }
#else
extern int16_t t_get_bitoffset(struct hw *);
extern void __set_widen_drift_enable(struct hw *, uint8_t);
#endif
void local_irq_disable(void) { event(4, 0); }
void local_irq_enable(void) {
    event(5, 0);
    if (irq_mutate) base.hw = new_hw_base;
}
void model_sync(void) { event(6, 0); }
uint32_t model_read(uint32_t a) {
    uint32_t v;
    if (a == 1867788) v = ra;
    else if (a == 1867796) v = rb;
    else if (a == 1867812) v = rc;
    else abort();
    event(a, v);
    return v;
}
void model_write(uint32_t a, uint32_t v) { event(a, v); }
void model_frame_write(uint16_t *p, uint16_t v) {
    if (p < frame || p > frame + 3) abort();
    event(10 + (uint32_t)(p - frame), v);
    *p = v;
}
int model_putchar(int ch) { event(7, ch); return ch; }
#ifndef HELPER_MODE
extern uint8_t __hw_set_widen_drift(struct hw *, uint32_t, uint32_t);
uint64_t run(uint32_t widen, uint32_t window, uint32_t clock,
             uint32_t a, uint32_t b, int16_t offset, uint8_t id, int mutate) {
    hw.frame = frame;
    base.hw = (void *)((uintptr_t)&hw - (uint32_t)id * 28);
    clk = clock; ra = a; rb = b; bitoff = offset; replace = mutate & 1;
    irq_mutate = (mutate >> 1) & 1;
    new_hw_base = (void *)((uintptr_t)&hw - (uint32_t)((id + 1) & 7) * 28);
    frame[0] = frame[2] = 0xaaaa;
    frame[1] = frame[3] = 0x5555;
    trace = UINT64_C(14695981039346656037); events = 0;
    event(8, __hw_set_widen_drift(&hw, widen, window));
    for (int i=0; i<4; ++i) event(20+i, frame[i]);
    return trace ^ events;
}
#else
uint64_t run_helper(uint32_t mode, uint32_t control, uint8_t enable,
                    uint8_t id, int mutate) {
    hw.frame = frame;
    base.hw = (void *)((uintptr_t)&hw - (uint32_t)id * 28);
    new_hw_base = (void *)((uintptr_t)&hw - (uint32_t)((id + 1) & 7) * 28);
    irq_mutate = mutate;
    rc = control;
    trace = UINT64_C(14695981039346656037); events = 0;
    if (mode) {
        __set_widen_drift_enable(&hw, enable);
    } else {
        event(8, (uint16_t)t_get_bitoffset(&hw));
    }
    return trace ^ events;
}
#endif
'''

def run(out):
    q32 = root / 'external/toolchains/jieli-linux-toolchains-20250324.1/q32s/bin/clang'
    subprocess.run([str(q32), '-target', 'q32s', '-x', 'ir', '-S', '-emit-llvm',
                    str(root / 'build/vanilla-bd19/src/platform/jieli/btctrler/bd19/RF_ble.o'), '-o', str(out / 'owned.ll')], check=True)
    (out / 'driver.c').write_text(driver)
    libs, helper_libs = [], []
    for helpers in [False, True]:
        for name, text in [('oracle', (root / 'build/controller-ir/bd19/01--RF_ble.c.o.ll').read_text()),
                           ('owned', (out / 'owned.ll').read_text())]:
            name += '-helpers' if helpers else '-core'
            (out / (name + '.ll')).write_text(isolate(text, helpers))
            subprocess.run([args.clang, '-Wno-override-module', '-fPIC', '-x', 'ir', '-c',
                            str(out / (name + '.ll')), '-o', str(out / (name + '.o'))], check=True)
            subprocess.run([args.cc, '-shared', '-fPIC', '-Wl,-Bsymbolic',
                            *(['-DHELPER_MODE'] if helpers else []), str(out / 'driver.c'),
                            str(out / (name + '.o')), '-o', str(out / (name + '.so'))], check=True)
            lib = C.CDLL(str(out / (name + '.so')))
            if helpers:
                lib.run_helper.argtypes = [C.c_uint32, C.c_uint32, C.c_uint8, C.c_uint8, C.c_int]
                lib.run_helper.restype = C.c_uint64
                helper_libs.append(lib)
            else:
                lib.run.argtypes = [C.c_uint32] * 5 + [C.c_int16, C.c_uint8, C.c_int]
                lib.run.restype = C.c_uint64
                libs.append(lib)
    count = 0
    def check(values):
        nonlocal count
        results = [lib.run(*values) for lib in libs]
        assert results[0] == results[1], (values, results)
        count += 1
    for widen in [0, 1, 624, 625, 626, 40959374, 40959375, 40960000, 0xffffffff]:
        for clock in [0, 1, 2, 65535, 65536, 0xffffffff]:
            for a, b in [(0, 0), (624, 0), (625, 255), (1023, 255), (0xffffffff, 0xffffffff)]:
                for offset in [-32768, -2048, -626, -625, -1, 0, 624, 625, 2047, 32767]:
                    for mutate in [0, 1, 2, 3]:
                        check((widen, 0x12345678, clock, a, b, offset, 7, mutate))
    rng = random.Random(192619)
    for _ in range(100000):
        check((rng.getrandbits(32), rng.getrandbits(32), rng.choice([0, 1, 65535, rng.getrandbits(32)]),
               rng.getrandbits(32), rng.getrandbits(32), rng.randrange(-32768, 32768), rng.randrange(8), rng.randrange(4)))
    print('BLE widening SDK vs recovered target LLVM:', count, 'modeled cases passed')
    count = 0
    def check_helper(values):
        nonlocal count
        results = [lib.run_helper(*values) for lib in helper_libs]
        assert results[0] == results[1], (values, results)
        count += 1
    for word in range(65536):
        for high in [0, 0x5a5a0000]:
            check_helper((0, word | high, 0, word & 7, 0))
    for control in [0, 1, 2047, 2048, 4095, 32768, 65535, 0x12345678, 0xffffffff]:
        for enable in range(256):
            for hw_id in [0, 7]:
                for mutate in [0, 1]:
                    check_helper((1, control, enable, hw_id, mutate))
    print('BLE bit-offset/drift-enable SDK vs recovered target LLVM:', count, 'modeled cases passed')

if args.output_dir:
    args.output_dir.mkdir(parents=True, exist_ok=True)
    run(args.output_dir)
else:
    with tempfile.TemporaryDirectory(prefix='btctrler-ble-widen-') as tmp:
        run(Path(tmp))
