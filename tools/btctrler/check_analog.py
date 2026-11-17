#!/usr/bin/env python3
"""Compare BD19 analog SDK and recovered target LLVM under software inputs.

Requires make controller-ir and the compiled BD19 analog object. Volatile
32-bit MMIO operations become ordered model calls in isolated host modules;
private entry points become callable. No hardware addresses are accessed.
This is sampled trace/final-state evidence, not radio/timing/IRQ equivalence.
"""
from pathlib import Path
import argparse
from contextlib import nullcontext
import re
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--clang', default=shutil.which('clang'))
parser.add_argument('--cc', default='gcc')
parser.add_argument('--output-dir', type=Path, help='keep isolated IR/binaries for debugging')
args = parser.parse_args()
if not args.clang:
    parser.error('pass --clang PATH to a host clang')
root = Path(__file__).resolve().parents[2]
original = (root / 'build/controller-ir/bd19/01--analog.c.o.ll').read_text()
names = re.findall(r'^define[^\n]*@([\w.]+)\(', original, re.M)
assert len(names) == 65, 'SDK member changed; re-review the model and ABI dispatch'
for name in names:
    assert f'!strcmp(argv[1],"{name}")' in (root / 'tools/btctrler/analog_trace.c').read_text(), name

def native(text):
    text = '\n'.join(line for line in text.splitlines()
                     if not line.startswith(('target ', 'attributes ', '!', 'declare void @llvm.dbg.'))
                     and '@llvm.dbg.' not in line)
    text = re.sub(r',? ![\w.]+ !\d+', '', text)
    text = re.sub(r'#\d+', '', text)
    text = text.replace('define internal fastcc', 'define').replace(' fastcc ', ' ')
    text = re.sub(r'^(define )internal ', r'\1', text, flags=re.M)
    # Include the two RX trim registers selected through an SSA pointer.
    # Named temporaries preserve the numbering of existing numeric SSA values.
    lines = []
    for number, line in enumerate(text.splitlines()):
        load = re.match(r'(\s*%\w+ = )load volatile i32, i32\* (.+), align \d+', line)
        store = re.match(r'\s*store volatile i32 ([%\w-]+), i32\* (.+), align \d+', line)
        if load or store:
            pointer = load[2] if load else store[2]
            direct = re.fullmatch(r'inttoptr \(i32 (\d+) to i32\*\)', pointer)
            address = direct[1] if direct else f'%model_addr_{number}'
            if not direct:
                lines.append(f'  {address} = ptrtoint i32* {pointer} to i32')
            lines.append(f'{load[1]}call i32 @mmio_read(i32 {address})' if load else
                         f'  call void @mmio_write(i32 {address}, i32 {store[1]})')
        else:
            lines.append(line)
    text = '\n'.join(lines)
    assert not re.search(r'(?:load|store) volatile ', text), 'unmodeled hardware access'
    return text + '\ndeclare i32 @mmio_read(i32)\ndeclare void @mmio_write(i32,i32)\n'

if args.output_dir:
    args.output_dir.mkdir(parents=True, exist_ok=True)
with (nullcontext(args.output_dir) if args.output_dir else tempfile.TemporaryDirectory(prefix='btctrler-analog-')) as tmp:
    out = Path(tmp)
    q32 = root / 'external/toolchains/jieli-linux-toolchains-20250324.1/q32s/bin/clang'
    subprocess.run([str(q32), '-target', 'q32s', '-x', 'ir', '-S', '-emit-llvm',
                    str(root / 'build/vanilla-bd19/src/platform/jieli/btctrler/bd19/analog.o'),
                    '-o', str(out / 'owned.ll')], check=True)
    for name, text in [('oracle', original), ('owned', (out / 'owned.ll').read_text())]:
        (out / (name + '.ll')).write_text(native(text))
        subprocess.run([args.clang, '-Wno-override-module', '-fPIC', '-x', 'ir', '-c',
                        str(out / (name + '.ll')), '-o', str(out / (name + '.o'))], check=True)
        subprocess.run([args.cc, '-O0', '-fno-builtin', '-rdynamic',
                        str(root / 'tools/btctrler/analog_trace.c'), str(out / (name + '.o')),
                        '-ldl', '-o', str(out / name)], check=True)
    count = 0
    cases = [(name, scene, {'bta_pll_bank_scan_tmp': 2401, 'get_bta_pll_bank': 100,
                            'bta_pll_bank_set': 4, 'btadc_sel': 192}.get(name, 1))
             for name in names for scene in range(10)]
    for name in ['get_bta_pll_bank', 'bta_pll_bank_set', 'bt_osc_offset_set',
                 'bt_osc_offset_ext_updata', 'bt_osc_offset_ext_save', 'btadc_sel',
                 'BT_CP_EN', 'btpll_trim_en', 'rxadc_ref_trim_set', 'umixer_vblo_trim_set']:
        cases.extend((name, 0, value) for value in [0, 7, 96, 144, 240, 255, 65535, 0xffffffff])
    for name, scene, value in cases:
        results = []
        for exe in ['oracle', 'owned']:
            proc = subprocess.run([str(out / exe), name, str(scene), str(value)],
                                  capture_output=True, timeout=10, check=True)
            assert re.fullmatch(rb'[0-9a-f]{16} [0-9]+\n', proc.stderr), (name, exe, proc.stderr)
            results.append(proc.stderr)
        assert results[0] == results[1], (name, scene, value, results)
        count += 1
    print('analog raw SDK vs recovered target LLVM:', count, 'modeled executions passed')
