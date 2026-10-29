#!/usr/bin/env python3
"""The deepest stack use of a tc32 (Telink) image, from its disassembly.

    stackdepth.py [--tools DIR] [--show NAME]... ELF [RULE]...

Each function's frame is what it pushes before its first branch plus its
"tsub sp"; its
callees are its tjl calls and its tail jumps into other functions.  The main
thread starts at main and has the RAM from the end of .bss to the top of
SRAM; interrupts start at __irq and have the SDK's irq_stk.  Prints the
deepest path of each and what is left; exits 1 if either does not fit.

Pointer calls are not followed by themselves.  RULEs add and remove edges:
  f=g,h      f calls g and h (a pointer call); @sym is every function a
             struct sym points to, @sym:i its i-th word only
  a>b        a never calls b
  a>>z       nothing below a calls z (a path taken only for some arguments)
  z<<a       z is called only below a
--show NAME prints the function's callees with the depth below each.
"""
import argparse
import functools
import re
import subprocess
import sys

ap = argparse.ArgumentParser()
ap.add_argument('--tools', default='', help='directory of tc32-elf-objdump and tc32-elf-nm')
ap.add_argument('--show', action='append', default=[])
ap.add_argument('elf')
ap.add_argument('rules', nargs='*')
args = ap.parse_args()


def tool(name, *a):
    return subprocess.run([args.tools.rstrip('/') + '/' + name if args.tools else name] + list(a),
                          capture_output=True, text=True, check=True).stdout


funcs = {}            # address -> name, frame, callees, pointer calls
names = {}            # name -> addresses (static functions share names)
cur = None
for line in tool('tc32-elf-objdump', '-d', args.elf).splitlines():
    m = re.match(r'^([0-9a-f]+) <([^>]+)>:', line)
    if m:
        cur = funcs.setdefault(int(m.group(1), 16), dict(name=m.group(2), frame=0, calls=set(),
                                                           pointer=0, prologue=True))
        names.setdefault(m.group(2), []).append(int(m.group(1), 16))
        continue
    m = re.match(r'^\s*[0-9a-f]+:\s+[0-9a-f ]+\s+(t\w+)\s*(.*)$', line)
    if not m or cur is None:
        continue
    op, arg = m.groups()
    if op.startswith('tj'):
        cur['prologue'] = False
    if op == 'tpush' and cur['prologue']:
        cur['frame'] += 4 * len(re.findall(r'r\d+|lr', arg))
    elif op == 'tsub' and arg.startswith('sp, #'):
        cur['frame'] += int(re.match(r'sp, #(\d+)', arg).group(1))
    elif op in ('tjl', 'tj'):
        t = re.match(r'([0-9a-f]+) <([^>+]+)>', arg)
        if t:
            cur['calls'].add(int(t.group(1), 16))
    elif op == 'tjex' and not arg.startswith('lr'):
        cur['pointer'] += 1

symbols = {}
for line in tool('tc32-elf-nm', '-S', args.elf).splitlines():
    p = line.split()
    if len(p) == 4:
        symbols[p[3]] = (int(p[0], 16), int(p[1], 16))
    elif len(p) == 3:
        symbols.setdefault(p[2], (int(p[0], 16), 0))


def pointers(sym):
    """The functions struct sym points to (sym:i: its i-th word)."""
    sym, _, index = sym.partition(':')
    at, size = symbols[sym]
    data = b''
    for line in tool('tc32-elf-objdump', '-s', '--start-address=%d' % at,
                     '--stop-address=%d' % (at + size), args.elf).splitlines():
        m = re.match(r'^ [0-9a-f]+ ((?:[0-9a-f]{8} ?){1,4})', line)
        if m:
            data += bytes.fromhex(m.group(1).replace(' ', ''))
    words = [int.from_bytes(data[i:i + 4], 'little') & ~1 for i in range(0, len(data), 4)]
    if index:
        words = [words[int(index)]]
    return [w for w in words if w in funcs and funcs[w]['name'] != '__start']


def lookup(name):
    if name.startswith('@'):
        return pointers(name[1:])
    if name not in names:
        sys.exit('stackdepth: no function %s (a rule to update?)' % name)
    return names[name]


cuts, never, only = set(), {}, {}
for rule in args.rules:
    if '=' in rule:
        f, callees = rule.split('=')
        for a in lookup(f):
            for c in callees.split(','):
                funcs[a]['calls'].update(lookup(c))
    elif '>>' in rule:
        a, z = rule.split('>>')
        for x in lookup(a):
            never.setdefault(x, set()).update(lookup(z))
    elif '<<' in rule:
        z, a = rule.split('<<')
        for x in lookup(a):
            only.setdefault(x, set()).update(lookup(z))
    else:
        a, b = rule.split('>')
        cuts.update((x, y) for x in lookup(a) for y in lookup(b))
banned_at_start = frozenset(z for zs in only.values() for z in zs)
active = set()        # on the path being walked: recursion


@functools.lru_cache(None)
def depth(a, banned=banned_at_start):
    """Deepest use from a, with the functions in banned not called below it."""
    f = funcs.get(a)
    if f is None:
        return 0, ('%x?' % a,)
    if a in active:
        return 0, (f['name'] + ' (recursion)',)
    active.add(a)
    banned = (banned - only.get(a, set())) | never.get(a, set())
    best = (0, ())
    for c in f['calls']:
        if c == a or c in banned or (a, c) in cuts:
            continue
        d = depth(c, banned)
        if d[0] > best[0]:
            best = d
    active.discard(a)
    note = '  (%d pointer calls)' % f['pointer'] if f['pointer'] else ''
    return f['frame'] + best[0], ('%s %d%s' % (f['name'], f['frame'], note),) + best[1]


for name in args.show:
    for a in lookup(name):
        f = funcs[a]
        below = sorted(((depth(c)[0], funcs[c]['name']) for c in f['calls'] if c != a), reverse=True)
        print('%s %d: %s' % (f['name'], f['frame'], ', '.join('%s %d' % (n, d) for d, n in below)))

fits = True
end_bss, sram = symbols['_end_bss_'][0], symbols['__SRAM_SIZE'][0]
for root, room in (('main', sram - end_bss), ('__irq', symbols['irq_stk'][1])):
    used, path = depth(names[root][0])
    print('%s: %d of %d bytes, %d left' % (root, used, room, room - used))
    for step in path:
        print('    ' + step)
    fits &= used <= room
sys.exit(0 if fits else 1)
