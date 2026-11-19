#!/usr/bin/env python3
"""Recover the controller's C types from the debug metadata in its IR.

    sdktypes.py OUT.h IR.ll...

Every struct, union, enum and typedef the SDK archive's debug info describes
is written as C, grouped by the original source file and in source-line
order, followed by offset and size assertions taken from the metadata.  The
header is written once and checked in; owned controller C uses it in place
of per-field accessors.  Rerun after an SDK update and diff.
"""
import re
import sys
from pathlib import Path
from collections import defaultdict

NODE = re.compile(r'^!(\d+) = (?:distinct )?(!DI\w+\((.*)\)|!\{(.*)\})$')
FIELD = re.compile(r'(\w+): ("(?:[^"\\]|\\.)*"|![\d]+|i64 -?\d+|[\w|.-]+)')
PREFIX = '/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/'


# Standard C library names the archive also defines; use the C headers.
SKIP = {'memcpy', 'memset', 'memcmp', 'memmove', 'strlen', 'strcpy', 'strcmp', 'strncmp',
        'printf', 'sprintf', 'snprintf', 'vsprintf', 'vsnprintf', 'puts', 'putchar',
        'malloc', 'free', 'calloc', 'realloc', 'rand', 'srand', 'abs'}


STD_TYPEDEFS = {'va_list', 'size_t', 'ssize_t', 'ptrdiff_t', 'wchar_t', 'int8_t', 'int16_t',
                'int32_t', 'int64_t', 'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t',
                'intptr_t', 'uintptr_t', 'bool'}
# Reserved implementation names (__x) belong to the C library.
RESERVED = re.compile(r'^__')


class Node(dict):
    pass


def parse(path):
    nodes = {}
    for line in Path(path).read_text().splitlines():
        m = NODE.match(line.rstrip())
        if not m:
            continue
        n = Node()
        if m.group(4) is not None:
            n['kind'] = 'list'
            n['items'] = [int(x) for x in re.findall(r'!(\d+)', m.group(4))]
        else:
            n['kind'] = m.group(2)[3:m.group(2).index('(')]
            for k, v in FIELD.findall(m.group(3)):
                n[k] = v
        nodes[int(m.group(1))] = n
    return nodes


class Module:
    """One IR file's metadata, with references resolved lazily."""

    def __init__(self, path):
        self.nodes = parse(path)

    def ref(self, v):
        if v is None or v == 'null':
            return None
        return self.nodes[int(v[1:])]

    def file(self, n):
        f = self.ref(n.get('file'))
        if not f:
            return '?'
        return f['filename'].strip('"').replace(PREFIX, '')


def num(n, k, default=0):
    v = n.get(k)
    if v is None:
        return default
    return int(v.split()[-1])


def name(n):
    v = n.get('name')
    return v.strip('"') if v else None


class Types:
    """Types merged across modules by identity (kind, name, file, line)."""

    def __init__(self):
        self.defs = {}           # key -> definition record
        self.order = []
        self.ids, self.keys, self.renamed = {}, [], {}
        self.functions, self.globals = {}, {}

    # ---- type identity and declarators -------------------------------------

    def key(self, m, n):
        k = n['kind']
        if k == 'CompositeType' and n['tag'] in ('DW_TAG_structure_type', 'DW_TAG_union_type',
                                                 'DW_TAG_enumeration_type'):
            return (n['tag'], name(n), m.file(n), num(n, 'line'), num(n, 'size'))
        if k == 'DerivedType' and n['tag'] == 'DW_TAG_typedef':
            return ('typedef', name(n), m.file(n), num(n, 'line'))
        return None

    def visit(self, m, n):
        """Record n (and everything it needs); return its C spelling pieces."""
        k = self.key(m, n)
        if k is None or k in self.defs:
            return k
        rec = {'key': k, 'file': m.file(n), 'line': num(n, 'line')}
        self.defs[k] = rec
        if k[0] == 'typedef':
            rec['type'] = self.spell(m, m.ref(n.get('baseType')))
            rec['deps'] = self.value_deps(m, m.ref(n.get('baseType')))
        elif k[0] == 'DW_TAG_enumeration_type':
            items = m.ref(n.get('elements'))
            rec['enumerators'] = [(name(m.nodes[i]), num(m.nodes[i], 'value'))
                                  for i in (items['items'] if items else [])]
            rec['size'] = num(n, 'size')
            rec['deps'] = set()
        else:
            rec['size'] = num(n, 'size')
            rec['members'], rec['deps'] = self.members(m, n)
            rec['declaration'] = 'DIFlagFwdDecl' in n.get('flags', '')
        self.order.append(k)
        return k

    def members(self, m, n):
        out, deps = [], set()
        items = m.ref(n.get('elements'))
        for i in (items['items'] if items else []):
            e = m.nodes[i]
            if e.get('tag') != 'DW_TAG_member':
                continue
            t = m.ref(e.get('baseType'))
            bits = num(e, 'size') if 'DIFlagBitField' in e.get('flags', '') else None
            inline = None
            if (t and t['kind'] == 'CompositeType' and name(t) is None and
                    t['tag'] in ('DW_TAG_structure_type', 'DW_TAG_union_type')):
                inline = self.members(m, t)
                deps |= inline[1]
                inline = (t['tag'], num(t, 'size'), inline[0])
            else:
                deps |= self.value_deps(m, t)
            out.append({'name': name(e), 'type': None if inline else self.spell(m, t),
                        'inline': inline, 'offset': num(e, 'offset'), 'bits': bits,
                        'size': num(e, 'size')})
        return out, deps

    def value_deps(self, m, t):
        """Types needed complete (by value) to declare something of type t."""
        while t is not None:
            k = self.key(m, t)
            if k is not None:
                self.visit(m, t)
                return {k}
            if t['kind'] == 'DerivedType' and t['tag'] in ('DW_TAG_pointer_type',):
                self.spell(m, t)       # record pointee, but no ordering need
                return set()
            if t['kind'] == 'SubroutineType':
                self.spell(m, t)
                return set()
            if t['kind'] == 'CompositeType' and t['tag'] == 'DW_TAG_array_type':
                t = m.ref(t.get('baseType'))
                continue
            if t['kind'] == 'DerivedType':
                t = m.ref(t.get('baseType'))
                continue
            return set()
        return set()

    def spell(self, m, t):
        """A declarator template: '{}' is replaced by the declared name."""
        if t is None:
            return 'void {}'
        k = t['kind']
        if k == 'BasicType':
            return name(t) + ' {}'
        if k == 'CompositeType':
            tag = t['tag']
            if tag == 'DW_TAG_array_type':
                sub = m.ref(t.get('elements'))
                dims = ''
                for i in (sub['items'] if sub else []):
                    c = num(m.nodes[i], 'count', -1)
                    dims += f'[{c}]' if c >= 0 else '[]'
                return self.spell(m, m.ref(t.get('baseType'))).replace('{}', '{}' + dims)
            key = self.visit(m, t)
            word = {'DW_TAG_structure_type': 'struct', 'DW_TAG_union_type': 'union',
                    'DW_TAG_enumeration_type': 'enum'}[tag]
            return f'{word} @@{self.ident(key)}@@ {{}}'
        if k == 'SubroutineType':
            return self.function(m, t, '{}')
        if k == 'DerivedType':
            tag = t['tag']
            base = m.ref(t.get('baseType'))
            if tag == 'DW_TAG_typedef':
                key = self.visit(m, t)
                return f'@@{self.ident(key)}@@ {{}}'
            if tag == 'DW_TAG_pointer_type':
                if base is not None and base['kind'] == 'SubroutineType':
                    return self.function(m, base, '(*{})')
                inner = self.spell(m, base)
                if base is not None and base['kind'] == 'CompositeType' and base['tag'] == 'DW_TAG_array_type':
                    return inner.replace('{}', '(*{})', 1)
                return inner.replace('{}', '*{}')
            if tag in ('DW_TAG_const_type', 'DW_TAG_volatile_type'):
                q = 'const' if tag == 'DW_TAG_const_type' else 'volatile'
                inner = self.spell(m, base)
                if inner.startswith(('(', '*')) or '*{}' in inner:
                    return inner.replace('*{}', f'*{q} {{}}')
                return f'{q} {inner}'
            if tag == 'DW_TAG_member':
                return self.spell(m, base)
        return 'void {}'

    def function(self, m, sub, declarator):
        types = m.ref(sub.get('types'))
        # Function type lists use null for void: read the literal list.
        ret, args = 'void {}', []
        refs = types.get('literal', []) if types else []
        if refs:
            ret = self.spell(m, m.ref(refs[0]))
            args = ['...' if r == 'null' else self.spell(m, m.ref(r)).replace('{}', '').strip()
                    for r in refs[1:]]
        # Only "..." means an unprototyped K&R declaration, f().
        params = '' if args == ['...'] else ', '.join(args) if args else 'void'
        return ret.replace('{}', f'{declarator}({params})')

    def ident(self, key):
        if key not in self.ids:
            self.ids[key] = len(self.ids)
            self.keys.append(key)
        return self.ids[key]

    def tag_name(self, key):
        if key in self.renamed:
            return self.renamed[key]
        if key[1]:
            return key[1]
        return 'anon_%s_%d' % (re.sub(r'\W', '_', key[2].rsplit('/', 1)[-1].split('.')[0]), key[3])


def reparse_lists(module, path):
    """Keep null entries in metadata lists (function types use them)."""
    for line in Path(path).read_text().splitlines():
        m = re.match(r'^!(\d+) = (?:distinct )?!\{(.*)\}$', line.rstrip())
        if m and int(m.group(1)) in module.nodes:
            module.nodes[int(m.group(1))]['literal'] = re.findall(r'null|!\d+', m.group(2))


def decl(template, ident):
    return template.replace('{}', ident).replace('  ', ' ').strip()


def emit_members(members, indent, out, padded=False):
    cursor = 0
    for e in members:
        if padded and e['bits'] is None and e['offset'] // 8 > cursor:
            out.append(f"{indent}unsigned char pad_{cursor}[{e['offset'] // 8 - cursor}];")
        if e['bits'] is None:
            cursor = (e['offset'] + e['size']) // 8
        if e['inline']:
            tag, _, sub = e['inline']
            out.append(f"{indent}{'struct' if tag == 'DW_TAG_structure_type' else 'union'} {{")
            emit_members(sub, indent + '    ', out)
            out.append(f"{indent}}}{' ' + e['name'] if e['name'] else ''};")
        else:
            bits = f" : {e['bits']}" if e['bits'] is not None else ''
            out.append(f"{indent}{decl(e['type'], e['name'] or '')}{bits};")


def assertions(types, rec, out):
    word = {'DW_TAG_structure_type': 'struct', 'DW_TAG_union_type': 'union'}[rec['key'][0]]
    tname = f'{word} {types.tag_name(rec["key"])}'
    if rec['size']:
        out.append(f'_Static_assert(sizeof({tname}) == {rec["size"] // 8}, "{tname}");')

    def walk(members, base, path):
        for e in members:
            if e['inline']:
                walk(e['inline'][2], base + e['offset'],
                     path + ([e['name']] if e['name'] else []))
            elif e['bits'] is None and e['name']:
                off = base + e['offset']
                field = '.'.join(path + [e['name']])
                out.append(f'_Static_assert(offsetof({tname}, {field}) == {off // 8}, "{field}");')
    walk(rec['members'], 0, [])


def main():
    # Debug info does not say which structs were packed.  Compile the layout
    # checks with the target compiler and pack exactly the structs that fail.
    import argparse, subprocess
    p = argparse.ArgumentParser()
    p.add_argument('out')
    p.add_argument('ir', nargs='+')
    p.add_argument('--cc', required=True)
    p.add_argument('--target', choices=('pi32v2', 'q32s'), default='pi32v2')
    a = p.parse_args()
    # Try each struct naturally laid out, then packed, then packed with
    # explicit padding; advance only structs whose member structs pass.
    mode = {}
    for _ in range(12):
        types = generate(a.out, a.ir, mode, a.target)
        r = subprocess.run([a.cc, '-target', a.target, '-std=gnu11', '-fsyntax-only', '-ferror-limit=0',
                            '-x', 'c', a.out], capture_output=True, text=True)
        if r.returncode == 0:
            print(f'layout: {sum(v == 1 for v in mode.values())} packed, '
                  f'{sum(v == 2 for v in mode.values())} padded', file=sys.stderr)
            return
        lines = Path(a.out).read_text().splitlines()
        failed = set()
        for n in re.findall(r'[\w./-]*\.h:(\d+):\d+: error: static_assert', r.stderr):
            m = re.search(r'(?:struct|union) (\w+)', lines[int(n) - 1])
            if m:
                failed.add(m.group(1))
        others = [l for l in r.stderr.splitlines() if 'error:' in l and 'static_assert' not in l]
        if others or not failed:
            sys.exit(r.stderr)
        inner = types.member_structs
        advance = {f for f in failed if not (inner.get(f, set()) & failed)}
        for f in advance:
            if mode.get(f, 0) == 2:
                sys.exit(f'no layout fits {f}')
            mode[f] = mode.get(f, 0) + 1
    sys.exit('layout did not converge')


def generate(out_path, paths, mode, target="pi32v2"):
    types = Types()
    for path in paths:
        mod = Module(path)
        reparse_lists(mod, path)
        for n in list(mod.nodes.values()):
            if types.key(mod, n):
                types.visit(mod, n)
        args = defaultdict(dict)
        for n in mod.nodes.values():
            if n['kind'] == 'LocalVariable' and 'arg' in n:
                args[n['scope']][num(n, 'arg')] = name(n)
        for i, n in mod.nodes.items():
            if n.get('isLocal') != 'false':
                continue
            if n.get('isDefinition') != 'true':
                continue
            if n['kind'] == 'Subprogram' and name(n) not in types.functions:
                sub = mod.ref(n.get('type'))
                names = args.get('!%d' % i, {})
                lst = mod.ref(sub.get('types')) if sub else None
                refs = lst.get('literal', []) if lst else []
                ret = types.spell(mod, mod.ref(refs[0])) if refs else 'void {}'
                params = []
                for j, r in enumerate(refs[1:], 1):
                    params.append('...' if r == 'null' else
                                  decl(types.spell(mod, mod.ref(r)), names.get(j) or ''))
                if 'DIFlagPrototyped' not in n.get('flags', '') and params == ['...']:
                    params = []
                text = ', '.join(params) if params else ('void' if 'DIFlagPrototyped' in n.get('flags', '') else '')
                types.functions[name(n)] = (mod.file(n), num(n, 'line'), decl(ret, f'{name(n)}({text})'))
            elif n['kind'] == 'GlobalVariable' and name(n) not in types.globals:
                types.globals[name(n)] = (mod.file(n), num(n, 'line'),
                                          decl(types.spell(mod, mod.ref(n.get('type'))), name(n)))
    # Drop forward declarations that have a definition elsewhere.
    defined = {(k[0], k[1]) for k, r in types.defs.items()
               if k[0] != 'typedef' and not r.get('declaration')}
    keep = [k for k in types.order
            if not (k[0] != 'typedef' and types.defs[k].get('declaration') and (k[0], k[1]) in defined)]
    # Identical names from different headers would collide in C.
    seen = defaultdict(list)
    for k in keep:
        if k[1] and not types.defs[k].get('declaration'):
            seen[(k[0], k[1])].append(k)
    dupes = {n: ks for n, ks in seen.items() if len(ks) > 1}
    for n, ks in dupes.items():
        sizes = {k[-1] for k in ks if k[0] != 'typedef'}
        if len(sizes) > 1:
            print(f'warning: {n[1]} has several layouts: {ks}', file=sys.stderr)
        if len(sizes) > 1:
            # File-local types: keep each, named after its source file.
            for k in ks[1:]:
                types.renamed[k] = k[1] + '_' + re.sub(r'\W', '_', k[2].rsplit('/', 1)[-1].split('.')[0])
            continue
        for k in ks[1:]:
            keep.remove(k)
            types.renamed[k] = types.tag_name(ks[0])
    # Order: by-value dependencies first, otherwise by file and line.
    keep.sort(key=lambda k: (types.defs[k]['file'], types.defs[k]['line']))
    canon = {(k[0], k[1]): k for k in keep if k[1]}
    done, ordered = set(), []

    def named(r):
        text = r.get('type', '') + repr(r.get('members', ''))
        return {types.keys[int(i)] for i in re.findall(r'@@(\d+)@@', text)
                if types.keys[int(i)][0] == 'typedef'}

    def place(k):
        if k in done:
            return
        done.add(k)
        for d in sorted(types.defs[k]['deps'] | named(types.defs[k]), key=lambda d: (types.defs[d]['file'], types.defs[d]['line'])):
            d = canon.get((d[0], d[1]), d) if d[1] else d
            if d in types.defs and d in keep_set:
                place(d)
        ordered.append(k)
    keep_set = set(keep)
    for k in keep:
        place(k)

    out = [f'/* Recovered {target} controller (btctrler.a) types, from the archive\'s debug',
           ' * metadata by tools/btctrler/sdktypes.py.  Names, layout and source files',
           ' * are the SDK\'s; the assertions check the layout this compiler gives. */',
           f'#ifndef BTCTRLER_SDK_{target.upper()}_H', f'#define BTCTRLER_SDK_{target.upper()}_H', '',
           '#include <stdarg.h>', '#include <stdbool.h>', '#include <stddef.h>',
           '#include <stdint.h>', '']
    words = {'DW_TAG_structure_type': 'struct', 'DW_TAG_union_type': 'union',
             'DW_TAG_enumeration_type': 'enum'}
    tags = sorted({(words[k[0]], types.tag_name(k)) for k in ordered
                   if k[0] in ('DW_TAG_structure_type', 'DW_TAG_union_type')})
    out += [f'{w} {n};' for w, n in tags] + ['']
    checks, current, enumerators = [], None, {}
    for k in ordered:
        r = types.defs[k]
        if r['file'] != current:
            current = r['file']
            out.append(f'/* ---- {current} */')
        if k[0] == 'typedef' and (k[1] in STD_TYPEDEFS or RESERVED.match(k[1])):
            continue
        if k[0] == 'typedef':
            out.append(f'typedef {decl(r["type"], types.tag_name(k))};')
        elif k[0] == 'DW_TAG_enumeration_type':
            if not r['enumerators']:
                continue
            # C enum constants share one namespace, unlike file-local SDK
            # enums. Keep the first spelling and qualify colliding declarations
            # by source/type so no value is silently borrowed from another enum.
            emitted = []
            for n, v in r['enumerators']:
                if n in enumerators:
                    qualified = types.tag_name(k) + '_' + n
                    if qualified in enumerators:
                        raise ValueError(f'duplicate recovered enumerator {qualified}')
                    n = qualified
                enumerators[n] = v
                emitted.append((n, v))
            body = ', '.join(f'{n} = {v}' for n, v in emitted)
            out.append(f'enum {types.tag_name(k)} {{ {body} }};  /* line {r["line"]} */')
        elif not r.get('declaration'):
            out.append(f'{words[k[0]]} {types.tag_name(k)} {{  /* line {r["line"]} */')
            how = mode.get(types.tag_name(k), 0)
            emit_members(r['members'], '    ', out, padded=how == 2)
            size = r['size'] // 8
            align = min(4, size & -size) if size else 1
            if how == 2 and r['members'] and not any(e['bits'] for e in r['members']):
                last = r['members'][-1]
                end = (last['offset'] + last['size']) // 8
                if size > end:
                    out.append(f'    unsigned char pad_{end}[{size - end}];')
            out.append(['};', '} __attribute__((packed));',
                        f'}} __attribute__((packed, aligned({align})));'][how])
            assertions(types, r, checks)
    types.member_structs = {}
    for k in ordered:
        r = types.defs[k]
        if 'members' in r:
            text = repr(r['members'])
            types.member_structs[types.tag_name(k)] = {
                types.tag_name(types.keys[int(i)])
                for i in re.findall(r'(?:struct|union) @@(\d+)@@ \{\}(?:\[\d+\])*\'', text)}
    out += ['', '/* ---- Globals and functions the archive defines, by source file. */']
    for kind, table in (('extern ', types.globals), ('', types.functions)):
        current = None
        for n, (f, line, text) in sorted(table.items(), key=lambda i: (i[1][0], i[1][1])):
            if n in SKIP:
                continue
            if f != current:
                current = f
                out.append(f'/* {f} */')
            out.append(f'{kind}{text};')
    out += ['', '/* Layout checks, for the target (host tests have wider pointers). */',
            '#ifndef UNITTEST'] + checks + ['#endif', '', '#endif', '']
    text = '\n'.join(out)
    text = re.sub(r'@@(\d+)@@', lambda m: types.tag_name(types.keys[int(m.group(1))]), text)
    Path(out_path).write_text(text)
    return types


if __name__ == '__main__':
    main()
