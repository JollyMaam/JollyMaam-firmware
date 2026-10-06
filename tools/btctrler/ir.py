"""Shared checked LLVM IR boundaries; no permissive SDK-version fallback."""
import hashlib
import re
import subprocess


def function(text, name):
    """Return a definition's body bounds, excluding braces."""
    match = re.search(r'^define [^\n]*@' + re.escape(name) + r'\([^\n]*\{\n', text, re.M)
    if not match:
        raise RuntimeError(f'controller: missing function {name}')
    return match.end(), text.index('\n}', match.end())


def extract(text, name, signature, expected):
    """Replace an audited definition with an attribute-free C declaration."""
    start, end = function(text, name)
    body = text[start:end]
    if hashlib.sha256(body.encode()).hexdigest() != expected:
        raise RuntimeError(f'controller: {name} changed; review owned C against SDK')
    header = text.rfind('\ndefine ', 0, start) + 1
    return text[:header] + 'declare ' + signature + '\n' + text[end + 2:], body


def rewrite_member(path, clang, transforms):
    """Decode once, apply transforms in order, verify/re-emit once."""
    ir = path.with_suffix('.ll')
    subprocess.run([clang, '-target', 'pi32v2', '-x', 'ir', '-S', '-emit-llvm',
                    str(path), '-o', str(ir)], check=True)
    text = ir.read_text()
    for transform in transforms:
        text = transform(text)
    ir.write_text(text)
    destination = path.with_suffix('.rewritten')
    subprocess.run([clang, '-target', 'pi32v2', '-x', 'ir', '-c', '-emit-llvm',
                    str(ir), '-o', str(destination)], check=True)
    destination.replace(path)


def debug_location(line: str) -> str:
    match = re.search(r"!dbg !\d+", line)
    if match is None:
        raise RuntimeError(f"line has no debug location to reuse: {line}")
    return ", " + match.group(0)




def _plain_calls(text, name):
    """Make every call of name use the C calling convention."""
    return re.sub(r'(call )fastcc ([^\n]*@' + re.escape(name) + r'\()', r'\1\2', text)


def export(text, name):
    """Give a file-static SDK function external linkage and the C calling
    convention, so owned C can call it.  Its body is unchanged."""
    pattern = re.compile(r'^define internal (?:fastcc )?([^\n]*@' + re.escape(name) + r'\()', re.M)
    if len(pattern.findall(text)) != 1:
        raise RuntimeError(f'controller: static {name} not found')
    return _plain_calls(pattern.sub(r'define \1', text), name)


def own(text, name, signature, expected):
    """Remove an SDK definition that owned C replaces; its (possibly
    file-static, fastcc) callers then call the C function."""
    text, body = extract(text, name, signature, expected)
    return _plain_calls(text, name), body


ABI_ATTRS = ('zeroext', 'signext', 'inreg', 'sret', 'byval')


def declaration(text, name):
    """The C-ABI declaration of a defined function: linkage, calling
    convention, names and optimizer attributes dropped; zeroext etc. kept."""
    m = re.search(r'^define ([^\n]*?)@' + re.escape(name) + r'\(([^\n]*?)\)[^\n]*\{$', text, re.M)
    if not m:
        raise RuntimeError(f'controller: missing function {name}')
    words = [w for w in m.group(1).split() if w not in (
        'internal', 'fastcc', 'local_unnamed_addr', 'unnamed_addr', 'dso_local', 'weak', 'hidden', 'noalias')]
    params = []
    depth, cur = 0, ''
    for ch in m.group(2) + ',':
        if ch in '([{<':
            depth += 1
        elif ch in ')]}>':
            depth -= 1
        if ch == ',' and depth == 0:
            if cur.strip():
                parts = cur.split()
                if len(parts) > 1 and parts[-1].startswith('%'):
                    parts = parts[:-1]           # the parameter's name
                keep = [p for p in parts if
                        (p in ABI_ATTRS or not re.match(r'(nocapture|readonly|readnone|writeonly|nonnull|noalias|returned|align|dereferenceable)', p))]
                params.append(' '.join(keep))
            cur = ''
        else:
            cur += ch
    return f'{" ".join(words)} @{name}({", ".join(params)})'


def own_all(text, table):
    """Own every function in {name: fingerprint}; see own().  All bodies are
    checked first: owning one rewrites calls inside the others."""
    decls = {}
    for name, expected in table.items():
        start, end = function(text, name)
        if hashlib.sha256(text[start:end].encode()).hexdigest() != expected:
            raise RuntimeError(f'controller: {name} changed; review owned C against SDK')
        decls[name] = declaration(text, name)
    for name in table:
        start, end = function(text, name)
        header = text.rfind('\ndefine ', 0, start) + 1
        text = _plain_calls(text[:header] + 'declare ' + decls[name] + '\n' + text[end + 2:], name)
    return text


def fingerprints(path, names):
    """Print table lines for owning names from a member's IR."""
    text = open(path).read()
    for name in names:
        start, end = function(text, name)
        print(f"    '{name}': '{hashlib.sha256(text[start:end].encode()).hexdigest()}',")


if __name__ == '__main__':
    import sys
    fingerprints(sys.argv[1], sys.argv[2:])
