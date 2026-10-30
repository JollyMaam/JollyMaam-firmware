#!/usr/bin/env python3
"""Compare SDK and production q32s frame LLVM in a freestanding 32-bit model.

Requires host clang with i386 code generation, GCC's -m32 assembler/linker,
and a Linux kernel that executes i386 binaries. No 32-bit libc is needed.
The original instruction bodies and actual 32-bit layouts are retained.
Volatile loads/stores become ordered model events. Target attributes, debug
metadata, sections and retention roots are removed ONLY from the host model;
their production properties require the separate ownership/layout audit.
Defined public globals share fixture storage; private constants are renamed.
Old memory intrinsics are upgraded without changing their lengths. Two old
constant-expression shifts become equivalent ordinary LLVM instructions.
See bredr_frame_trace.c for scenarios and model limitations.
"""
from pathlib import Path
import argparse
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def model(text, prefix):
    functions = set(re.findall(r'^define [^\n]*@([^ (]+)', text, re.M))
    private = set(re.findall(r'^@([\w.$]+) = (?:private|internal)', text, re.M))
    renamed = functions | private
    text = re.sub(r'@([\w.$]+)',
                  lambda m: '@' + prefix + '_' + m[1] if m[1] in renamed else m[0], text)
    lines = []
    reads = writes = 0
    for line in text.splitlines():
        if (line.startswith(('!', 'attributes ', 'target ', 'source_filename', ';'))
                or '@llvm.dbg.' in line or line.startswith('@llvm.used')
                or line.startswith('@llvm.compiler.used')):
            continue
        line = re.sub(r',? ![\w.]+ !\d+', '', line)
        line = re.sub(r' #\d+', '', line)
        line = re.sub(r' section "[^"]+"', '', line)
        if line.startswith('define '):
            line = re.sub(r'\b(internal|private) ', '', line)
        if (line.startswith('@') and ' external ' not in line
                and not any(line.startswith('@' + prefix + '_' + n + ' ') for n in private)):
            name, rhs = line.split(' = ', 1)
            # This member's three defined public globals are zero-initialized.
            assert 'zeroinitializer' in rhs or re.search(r'global i32 0\b', rhs), line
            typ = rhs.split('global ', 1)[1].split(' ', 1)[0]
            line = name + ' = external global ' + typ
        if re.match(r'declare void @llvm\.(?:memcpy|memset|lifetime)', line):
            continue
        if '@llvm.lifetime.' in line:
            continue
        line = line.replace('llvm.memcpy.p0i8.p0i8.i32', 'llvm.memcpy.p0.p0.i32')
        line = line.replace('llvm.memset.p0i8.i32', 'llvm.memset.p0.i32')
        line = re.sub(r'(@llvm.mem(?:cpy|set)\.[^(]+\(.*), i32 \d+, i1 (false|true)\)',
                      r'\1, i1 \2)', line)
        load = re.match(r'(\s*%[\w.]+ = )load volatile (i16|i32), \2\* (.+), align \d+$', line)
        if load:
            line = f'{load[1]}call {load[2]} @model_read{load[2][1:]}({load[2]}* {load[3]})'
            reads += 1
        store = re.match(r'\s*store volatile (i16|i32) (.*), \1\* (.*), align \d+$', line)
        if store:
            line = f'  call void @model_write{store[1][1:]}({store[1]}* {store[3]}, {store[1]} {store[2]})'
            writes += 1
        assert 'load volatile' not in line and 'store volatile' not in line, line
        shift = re.search(r'lshr \(i32 (ptrtoint \([^)]+\)), i32 (\d+)\)', line)
        if shift:
            name = '%model_ce_' + str(len(lines))
            lines.append(f'  {name} = lshr i32 {shift[1]}, {shift[2]}')
            line = line.replace(shift[0], name)
        lines.append(line)
    assert reads and writes
    return '\n'.join(lines) + '''
declare void @llvm.memcpy.p0.p0.i32(i8*, i8*, i32, i1)
declare void @llvm.memset.p0.i32(i8*, i8, i32, i1)
declare i16 @model_read16(i16*)
declare i32 @model_read32(i32*)
declare void @model_write16(i16*, i16)
declare void @model_write32(i32*, i32)
'''


def run(args, out):
    env = dict(os.environ, PATH='/work/bin:' + os.environ.get('PATH', ''))
    obj = ROOT / 'build/vanilla-bd19/src/btctrler/bd19/bredr_frame.o'
    with (out / 'build.log').open('w') as log:
        subprocess.run(['make', str(obj.relative_to(ROOT))], cwd=ROOT, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    decoder = ROOT / 'external/toolchains/jieli-linux-toolchains-20250324.1/q32s/bin/clang'
    owned = out / 'owned.ll'
    subprocess.run([str(decoder), '-target', 'q32s', '-x', 'ir', '-S', '-emit-llvm',
                    str(obj), '-o', str(owned)], check=True)
    original = ROOT / 'build/controller-ir/bd19/01--bredr_frame.c.o.ll'
    for prefix, path in [('sdk', original), ('owned', owned)]:
        ll = out / (prefix + '-model.ll')
        ll.write_text(model(path.read_text(), prefix))
        subprocess.run([args.clang, '-target', 'i386-linux-gnu', '-Wno-override-module',
                        '-ffunction-sections', '-fdata-sections', '-fno-pic', '-c', '-x', 'ir',
                        str(ll), '-o', str(out / (prefix + '.o'))], check=True)
    executable = out / 'check'
    subprocess.run([args.cc, '-m32', '-std=c11', '-O2', '-ffunction-sections', '-fdata-sections',
                    '-fno-builtin', '-fno-pie', '-fno-stack-protector', '-nostdlib', '-static',
                    '-no-pie', '-Wl,--gc-sections', str(ROOT / 'tools/btctrler/bredr_frame_trace.c'),
                    str(out / 'sdk.o'), str(out / 'owned.o'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clang', required=True)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        run(args, args.output_dir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix='btctrler-frame-ir-') as temp:
            run(args, Path(temp))
