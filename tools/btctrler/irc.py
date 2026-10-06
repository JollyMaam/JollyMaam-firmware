#!/usr/bin/env python3
"""Print SDK functions from controller IR as pseudo-C, for recovering them.

    irc.py MEMBER.ll FUNCTION...      (build/controller-ir/sdk/*.ll)

Struct fields are named from the debug info by byte offset (LLVM merges
bitfields, so IR field numbers are not member numbers).  Single-use pure
values are folded into expressions; log_print calls become comments with
their format string; source lines from !dbg are shown on the right.  The
output is a reading aid, not compilable C: control flow stays as blocks.
"""
import re
import sys

# ---- types ------------------------------------------------------------------

class T:
    def __init__(self, kind, **kw):
        self.kind = kind
        self.__dict__.update(kw)

    def __repr__(self):
        return f'T({self.kind},{self.__dict__})'


def parse_type(s, i=0):
    """Parse an LLVM type at s[i:]; return (type, end)."""
    while s[i] == ' ':
        i += 1
    if s.startswith('void', i):
        t, i = T('void'), i + 4
    elif s[i] == 'i' and s[i + 1].isdigit():
        m = re.match(r'i(\d+)', s[i:])
        t, i = T('int', bits=int(m.group(1))), i + m.end()
    elif s.startswith('float', i):
        t, i = T('int', bits=32), i + 5
    elif s.startswith('double', i):
        t, i = T('int', bits=64), i + 6
    elif s[i] == '%':
        m = re.match(r'%([\w.]+)', s[i:])
        t, i = T('named', name=m.group(1)), i + m.end()
    elif s[i] == '[':
        m = re.match(r'\[(\d+) x ', s[i:])
        el, j = parse_type(s, i + m.end())
        t, i = T('array', n=int(m.group(1)), el=el), j + 1
    elif s.startswith('<{', i) or s[i] == '{':
        packed = s[i] == '<'
        i += 2 if packed else 1
        fields = []
        while True:
            while s[i] == ' ':
                i += 1
            if s[i] == '}':
                i += 1
                break
            f, i = parse_type(s, i)
            fields.append(f)
            while s[i] in ' ,':
                i += 1
        if packed:
            i += 1   # '>'
        t = T('struct', fields=fields, packed=packed, name=None)
    else:
        raise ValueError(f'type at {s[i:i+40]!r}')
    while True:
        if s.startswith('*', i):
            t, i = T('ptr', to=t), i + 1
        elif s.startswith(' (', i) or s.startswith('(', i):
            j = s.index('(', i) + 1
            args = []
            while s[j] != ')':
                if s.startswith('...', j):
                    args.append(T('void')); j += 3
                else:
                    a, j = parse_type(s, j)
                    args.append(a)
                while s[j] in ' ,':
                    j += 1
            t, i = T('func', ret=t, args=args), j + 1
        else:
            return t, i


class Module:
    def __init__(self, path, text=None):
        self.path = path
        self.text = text if text is not None else open(path).read()
        self.siblings = None
        self.structs = {}
        for m in re.finditer(r'^%([\w.]+) = type (.*)$', self.text, re.M):
            if m.group(2) == 'opaque':
                continue
            t, _ = parse_type(m.group(2))
            t.name = m.group(1)
            self.structs[m.group(1)] = t
        self.strings = {}
        for m in re.finditer(r'^@([\w.]+) = [^\n]*constant \[\d+ x i8\] c"((?:[^"\\]|\\.)*)"', self.text, re.M):
            self.strings[m.group(1)] = decode(m.group(2))
        self.lines = {n: l for n, l in re.findall(r'^!(\d+) = !DILocation\(line: (\d+)', self.text, re.M)}
        self.di_members = self.debug_members()

    # -- layout
    def resolve(self, t):
        return self.structs[t.name] if t.kind == 'named' else t

    def size_align(self, t):
        t = self.resolve(t)
        if t.kind == 'int':
            b = max(1, (t.bits + 7) // 8)
            return b, min(b, 4) if b != 3 else 4
        if t.kind in ('ptr', 'func'):
            return 4, 4
        if t.kind == 'array':
            s, a = self.size_align(t.el)
            return s * t.n, a
        if t.kind == 'struct':
            off, align = 0, 1
            for f in t.fields:
                s, a = self.size_align(f)
                if t.packed:
                    a = 1
                off = (off + a - 1) // a * a + s
                align = max(align, a)
            return (off + align - 1) // align * align, align
        return 0, 1

    def offsets(self, t):
        t = self.resolve(t)
        out, off = [], 0
        for f in t.fields:
            s, a = self.size_align(f)
            if t.packed:
                a = 1
            off = (off + a - 1) // a * a
            out.append(off)
            off += s
        return out

    # -- debug info: struct name -> [(byte offset, bits, name)]
    def debug_members(self):
        nodes = {}
        for n, body in re.findall(r'^!(\d+) = (?:distinct )?(!DI\w+\(.*\)|!\{.*\})$', self.text, re.M):
            nodes[n] = body
        out = {}
        for n, body in nodes.items():
            if not body.startswith('!DICompositeType(tag: DW_TAG_structure_type') and \
               not body.startswith('!DICompositeType(tag: DW_TAG_union_type'):
                continue
            name = re.search(r'name: "(\w+)"', body)
            el = re.search(r'elements: !(\d+)', body)
            if not name or not el:
                continue
            members = []
            for m in re.findall(r'!(\d+)', nodes.get(el.group(1), '')):
                d = nodes.get(m, '')
                if 'DW_TAG_member' not in d:
                    continue
                nm = re.search(r'name: "(\w+)"', d)
                off = re.search(r'offset: (\d+)', d)
                size = re.search(r'size: (\d+)', d)
                members.append((int(off.group(1)) if off else 0, int(size.group(1)) if size else 0,
                                nm.group(1) if nm else '_', 'DIFlagBitField' in d))
            if members:
                out.setdefault(name.group(1), members)
        return out

    def sibling_members(self, name):
        """Struct members from the other archive members' debug info."""
        if self.siblings is None:
            import glob, os
            self.siblings = {}
            for f in sorted(glob.glob(os.path.join(os.path.dirname(self.path) or '.', '*.ll'))):
                if f != self.path:
                    for k, v in Module.__new__(Module).__class__.debug_members(
                            type('M', (), {'text': open(f).read()})()).items():
                        if v:
                            self.siblings.setdefault(k, v)
        return self.siblings.get(name)

    def field_name(self, struct_t, index):
        """Name of IR field `index` of a struct type, via debug offsets."""
        t = self.resolve(struct_t)
        name = (t.name or '').split('.', 1)[-1]
        name = re.sub(r'\.\d+$', '', name)
        offs = self.offsets(t)
        byte = offs[index]
        size, _ = self.size_align(t.fields[index])
        members = self.di_members.get(name) or self.sibling_members(name)
        if not members:
            return f'f{index}'
        hits = [m for m in members if m[0] // 8 == byte and not m[3]]
        if hits:
            return hits[0][2]
        bits = [m[2] for m in members if m[3] and byte * 8 <= m[0] < (byte + size) * 8]
        if bits:
            return 'bits(' + ','.join(bits) + ')'
        return f'f{index}@{byte}'


def wrapped(text):
    """Is text one parenthesized group?"""
    if not text.startswith('('):
        return False
    depth = 0
    for i, ch in enumerate(text):
        depth += ch == '('
        depth -= ch == ')'
        if depth == 0:
            return i == len(text) - 1
    return False


def local(n):
    n = n.replace('.', '_')
    return 'v' + n if n[0].isdigit() else n


def decode(s):
    return re.sub(r'\\([0-9A-Fa-f]{2})', lambda m: chr(int(m.group(1), 16)), s).rstrip('\x00')


# ---- values -----------------------------------------------------------------

def split_args(s):
    """Split a comma-separated operand list at top level."""
    out, depth, cur, q = [], 0, '', False
    for ch in s:
        if ch == '"':
            q = not q
        if not q:
            if ch in '([{<':
                depth += 1
            elif ch in ')]}>':
                depth -= 1
            elif ch == ',' and depth == 0:
                out.append(cur.strip()); cur = ''
                continue
        cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


ATTRS = r'(?:(?:zeroext|signext|nonnull|noalias|nocapture|readonly|returned|inreg|align \d+|dereferenceable\(\d+\))\s+)*'


class Func:
    def __init__(self, mod, name):
        self.mod = mod
        m = re.search(r'^define [^\n]*@' + re.escape(name) + r'\((.*?)\)[^\n]*\{\n(.*?)^\}', mod.text, re.M | re.S)
        if not m:
            raise SystemExit(f'{name}: not defined here')
        self.header = m.group(0).split('\n', 1)[0]
        self.params = {}
        for a in split_args(m.group(1)):
            pm = re.match(r'(.*?)\s*' + ATTRS + r'%([\w.]+)$', a)
            if pm:
                ty, _ = parse_type(pm.group(1).split(' ')[0] if ' ' not in pm.group(1).strip() else pm.group(1).strip().split(' ')[0])
                self.params[pm.group(2)] = ty
        self.body = m.group(2)
        self.types = dict(self.params)
        self.defs, self.uses = {}, {}
        self.lines = []
        for line in self.body.split('\n'):
            if 'llvm.dbg.' in line or 'llvm.lifetime' in line:
                continue
            self.lines.append(line)
            d = re.match(r'\s+%([\w.]+) = ', line)
            if d:
                self.defs[d.group(1)] = line
            code = re.sub(r', !\w+ !\d+', '', line.split(' = ', 1)[-1] if d else line)
            for u in re.findall(r'%([\w.]+)', code):
                self.uses[u] = self.uses.get(u, 0) + 1
        self.folded = {}
        # Values a phi reads must keep their names: the phi prints first.
        self.in_phi = set()
        for line in self.lines:
            if ' = phi ' in line:
                self.in_phi |= set(re.findall(r'\[ %([\w.]+),', line))

    # -- operand expression
    def value(self, v):
        v = v.strip()
        v = re.sub(r'^' + ATTRS, '', v)
        tm = re.match(r'(.*?)\s+(%[\w.]+|@[\w.]+|-?\d+|null|undef|true|false|zeroinitializer|'
                      r'getelementptr .*|bitcast .*|inttoptr .*|ptrtoint .*)$', v)
        ty = None
        if tm:
            try:
                ty, _ = parse_type(re.sub(r'^' + ATTRS, '', tm.group(1)))
            except (ValueError, IndexError):
                ty = None
            v = tm.group(2)
        return self.expr(v, ty)

    def expr(self, v, ty=None):
        if v.startswith('%'):
            n = v[1:]
            if n in self.folded:
                return self.folded[n]
            return local(n)
        if v.startswith('@'):
            n = v[1:]
            if n in self.mod.strings:
                return '"' + self.mod.strings[n].encode('unicode_escape').decode() + '"'
            return '&' + n          # a global's address; loads and stores strip the &
        if v in ('null', 'zeroinitializer'):
            return '0'
        if v == 'true':
            return '1'
        if v == 'false':
            return '0'
        if v.startswith('getelementptr'):
            m = re.match(r'getelementptr (?:inbounds )?\((.*)\)$', v)
            return self.gep(split_args(m.group(1)))
        m = re.match(r'(bitcast|inttoptr|ptrtoint) \((.*) to (.*)\)$', v)
        if m:
            return self.value(m.group(2))
        return v

    def gep(self, parts):
        """parts: [source type, ptr operand, idx...] -> address expression."""
        src, _ = parse_type(parts[0])
        base = self.value(parts[1])
        idx = [self.value(p) for p in parts[2:]]
        if idx[0] != '0':
            base = f'({base} + {idx[0]})'
        if len(idx) == 1:
            return base
        if base.startswith('*'):
            base = f'({base})'
        out, t, first = base, src, True
        if base.startswith('&') and re.match(r'&[\w.]+$', base):
            out, first = base[1:], False      # &g -> g.field / g[i]
        for i in idx[1:]:
            r = self.mod.resolve(t)
            if r.kind == 'struct':
                fname = self.mod.field_name(t, int(i))
                out = f'{out}->{fname}' if first else f'{out}.{fname}'
                t = r.fields[int(i)]
            else:
                out = f'{out}[{i}]'
                t = r.el
            first = False
        if out.endswith('[0]'):
            return out[:-3]        # array decay: &x[0] is x
        return '&' + out

    # -- statements
    def statement(self, line):
        line = re.sub(r'\s*#\d+(?=,|$)', '', line)
        dbg = re.search(r'!dbg !(\d+)', line)
        src = self.mod.lines.get(dbg.group(1)) if dbg else None
        line = re.sub(r', !\w+(?:\.\w+)* !\d+', '', line).rstrip()
        d = re.match(r'\s+%([\w.]+) = (.*)$', line)
        name, rhs = (d.group(1), d.group(2)) if d else (None, line.strip())
        text = self.translate(name, rhs)
        if text is None:
            return None
        if name and self.uses.get(name, 0) == 1 and self.foldable(rhs) and name not in self.in_phi:
            self.folded[name] = f'({text})' if re.search(r'[ ?]', text) and not wrapped(text) else text
            return None
        if name:
            text = f'{local(name)} = {text}'
        return text + ';', src

    def foldable(self, rhs):
        return re.match(r'(getelementptr|add|sub|mul|and|or|xor|shl|lshr|ashr|udiv|sdiv|urem|srem|'
                        r'icmp|zext|sext|trunc|bitcast|select|load)\b', rhs) is not None

    def translate(self, name, rhs):
        op = rhs.split(' ', 1)[0]
        if op == 'getelementptr':
            m = re.match(r'getelementptr (?:inbounds )?(.*)$', rhs)
            return self.gep(split_args(m.group(1)))
        if op == 'load':
            m = re.match(r'load (?:volatile )?([^,]+), (.*?)(?:, align \d+)?$', rhs)
            p = self.value(m.group(2))
            return p[1:] if p.startswith('&') else f'*{p}'
        if op == 'store':
            m = re.match(r'store (?:volatile )?(.*?), ([^,]+\*?[^,]*? [%@][\w.]+|[^,]*getelementptr.*?\)|[^,]+)(?:, align \d+)?$', rhs)
            parts = split_args(rhs[len('store '):].replace(', align', ' ,align').split(' ,align')[0])
            v, p = self.value(parts[0]), self.value(parts[1])
            target = p[1:] if p.startswith('&') else f'*{p}'
            return f'{target} = {v}'
        m = re.match(r'(add|sub|mul|and|or|xor|shl|lshr|ashr|udiv|sdiv|urem|srem)(?: nuw| nsw| exact)* (\S+) (.*)$', rhs)
        if m:
            a, b = split_args(m.group(3))
            sym = {'add': '+', 'sub': '-', 'mul': '*', 'and': '&', 'or': '|', 'xor': '^', 'shl': '<<',
                   'lshr': '>>', 'ashr': '>>s', 'udiv': '/', 'sdiv': '/s', 'urem': '%', 'srem': '%s'}[m.group(1)]
            return f'{self.expr(a)} {sym} {self.expr(b)}'
        m = re.match(r'icmp (\w+) (\S+) (.*)$', rhs)
        if m:
            a, b = split_args(m.group(3))
            sym = {'eq': '==', 'ne': '!=', 'ugt': '>', 'uge': '>=', 'ult': '<', 'ule': '<=',
                   'sgt': '>s', 'sge': '>=s', 'slt': '<s', 'sle': '<=s'}[m.group(1)]
            return f'{self.expr(a)} {sym} {self.expr(b)}'
        m = re.match(r'(zext|sext|trunc|bitcast|inttoptr|ptrtoint) (.*) to (.*)$', rhs)
        if m:
            inner = self.value(m.group(2))
            if m.group(1) == 'trunc':
                return f'({m.group(3).strip()}){inner}'
            if m.group(1) == 'sext':
                return f'(signed){inner}'
            return inner
        if op == 'select':
            c, a, b = split_args(rhs[len('select '):])
            return f'({self.value(c)} ? {self.value(a)} : {self.value(b)})'
        if op == 'phi':
            m = re.match(r'phi (\S+) (.*)$', rhs)
            arms = re.findall(r'\[ (.*?), %([\w.]+) \]', m.group(2))
            return 'φ(' + ', '.join(f'{self.expr(v)} @{b}' for v, b in arms) + ')'
        if op == 'alloca':
            m = re.match(r'alloca (.*?)(?:, align \d+)?$', rhs)
            return f'local {m.group(1)}'
        if 'call ' in rhs.split('(')[0] + ' ':
            m = re.match(r'(?:tail |musttail |notail )?call (?:fastcc )?(?:' + ATTRS + r')?(.*?)(@[\w.]+|%[\w.]+)\((.*)\)\s*$', rhs)
            if not m:
                m = re.match(r'(?:tail )?call .*? bitcast \(.*? (@[\w.]+) to .*?\)\((.*)\)\s*$', rhs)
                fn, args = m.group(1), m.group(2)
            else:
                fn, args = m.group(2), m.group(3)
            fn = fn[1:]
            args = [self.value(a) for a in split_args(args)]
            if fn == 'log_print':
                return f'// log {args[2] if len(args) > 2 else ""} {", ".join(args[3:])}'
            if fn.startswith('llvm.memcpy') or fn.startswith('llvm.memmove'):
                return f'{fn.split(".")[1]}({args[0]}, {args[1]}, {args[2]})'
            if fn.startswith('llvm.memset'):
                return f'memset({args[0]}, {args[1]}, {args[2]})'
            return f'{fn}({", ".join(args)})'
        if op == 'br':
            m = re.match(r'br i1 (\S+), label %([\w.]+), label %([\w.]+)', rhs)
            if m:
                return f'if ({self.expr(m.group(1))}) goto {m.group(2)}; else goto {m.group(3)}'
            return 'goto ' + re.match(r'br label %([\w.]+)', rhs).group(1)
        if op == 'switch':
            return None   # handled in print_function
        if op == 'ret':
            return 'return' + ('' if rhs == 'ret void' else ' ' + self.value(rhs[4:]))
        if op == 'unreachable':
            return 'unreachable'
        return '?? ' + rhs

    def print(self):
        print(self.header.replace('define ', '').split(' section')[0].split(' !dbg')[0] + ' {')
        lines = self.lines
        i = 0
        while i < len(lines):
            line = lines[i]
            if re.match(r'^[\w.]+:', line):
                print(line.split(':')[0] + ':')
                i += 1
                continue
            m = re.match(r'^; <label>:(\d+):', line)
            if m:
                print(m.group(1) + ':')
                i += 1
                continue
            if line.strip().startswith('switch '):
                sw = line
                while ']' not in sw:
                    i += 1
                    sw += ' ' + lines[i].strip()
                m = re.match(r'\s*switch (\S+ \S+), label %([\w.]+) \[(.*)\]', sw)
                if not m:
                    print('    ' + re.sub(r', !\w+ !\d+', '', sw.strip()))
                    i += 1
                    continue
                arms = re.findall(r'\S+ (-?\d+), label %([\w.]+)', m.group(3))
                print(named(f'    switch ({self.value(m.group(1))}) {{ ' +
                      ' '.join(f'case {v}: goto {b};' for v, b in arms) +
                      f' default: goto {m.group(2)}; }}'))
                i += 1
                continue
            if line.strip():
                r = self.statement(line)
                if r:
                    text, src = r
                    if text.startswith('// log'):
                        text = text[:-1]
                    text = named(text)
                    print(f'    {text:<96} // L{src}' if src else f'    {text}')
            i += 1
        print('}')


ENUMS = {}


def load_enums():
    """Stage/step names from the recovered header, for readability."""
    import os
    h = os.path.join(os.path.dirname(__file__), '..', '..', 'src', 'btctrler', 'sdk.h')
    for line in open(h):
        m = re.match(r'enum \w+ \{ (LMP_STAGE_CONN = 0.*?|LMP_TX_NULL = 0.*?) \};', line)
        if m:
            key = 'stage' if m.group(1).startswith('LMP_STAGE') else 'step'
            ENUMS[key] = {int(v): n for n, v in re.findall(r'(\w+) = (-?\d+)', m.group(1))}


def named(text):
    def sub(m):
        table = ENUMS.get(m.group(1))
        v = int(m.group(4))
        return f'{m.group(1)}{m.group(2)} {m.group(3)} {table[v]}' if table and v in table else m.group(0)
    text = re.sub(r'(step|stage)(\)*) (=|==|!=) (\d+)\b', sub, text)
    sw = re.match(r'(\s*switch \(\*?[\w>.-]*?(step|stage)\) \{)(.*)', text)
    if sw and ENUMS.get(sw.group(2)):
        t = ENUMS[sw.group(2)]
        text = sw.group(1) + re.sub(r'case (\d+):', lambda m: f'case {t.get(int(m.group(1)), m.group(1))}:', sw.group(3))
    return text


def main():
    load_enums()
    mod = Module(sys.argv[1])
    for name in sys.argv[2:]:
        Func(mod, name).print()
        print()


if __name__ == '__main__':
    main()
