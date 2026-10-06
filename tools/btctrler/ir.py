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


