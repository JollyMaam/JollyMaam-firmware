"""Target-aware inspection and whole-member BD19 ownership boundaries."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from .archive import read_archive, sha256

ROOT = Path(__file__).resolve().parents[2]
CONTROLLER = 'src/platform/jieli/btctrler/'
MANIFEST = ROOT / 'tools/btctrler/bd19.json'
CHIPS = {
    'br23': ('pi32v2', 'f63db0097b5d41096b3228ca038898f317694d6d293d1ba48beddfab85778869'),
    'bd19': ('q32s', 'e35f01d94229fffe7e2734d69fae0e7b1910e5f07a53ce397b38253932366f2e'),
}


def paths(chip):
    target, expected = CHIPS[chip]
    # JIELI_SDK (as for make) points a worktree at another checkout's SDK.
    sdk = ROOT / os.environ.get('JIELI_SDK', 'external/jieli-fw-AC63_BT_SDK') / f'cpu/{chip}/liba/btctrler.a'
    tools = ROOT / f'external/toolchains/jieli-linux-toolchains-20250324.1/{target}/bin'
    return sdk, tools, target, expected


def original(chip):
    sdk, _, _, expected = paths(chip)
    if sha256(sdk.read_bytes()) != expected:
        raise ValueError(f'{chip}: SDK archive changed; repeat the ownership audit')
    return read_archive(sdk)


def atomic_write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as out:
        tmp = Path(out.name)
        try:
            out.write(data)
            out.close()
            os.replace(tmp, path)
        finally:
            tmp.unlink(missing_ok=True)


def symbols(text):
    """External definitions required when replacing an entire member."""
    functions, globals_ = [], []
    for line in text.splitlines():
        if line.startswith('define '):
            if re.search(r'\b(internal|private|available_externally)\b', line.split('@', 1)[0]):
                continue
            functions.append(re.search(r'@([^ (]+)\(', line)[1])
        elif line.startswith('@'):
            if line.startswith('@llvm.'):
                continue
            rhs = line.split(' = ', 1)[1]
            if re.search(r'\b(external|internal|private|available_externally)\b', rhs.split(',', 1)[0]):
                continue
            globals_.append(line[1:].split(' = ', 1)[0])
    return {'functions': sorted(functions), 'globals': sorted(globals_)}


def retention_roots(text):
    roots = {'used': set(), 'compiler.used': set()}
    for kind, initializer in re.findall(r'^@llvm\.(used|compiler\.used) = ([^\n]*)', text, re.M):
        roots[kind].update(re.findall(r'@([\w.$-]+)', initializer))
    return roots


def require_retention(expected, provided):
    missing = expected['used'] - provided['used']
    missing |= expected['compiler.used'] - (provided['used'] | provided['compiler.used'])
    if missing:
        raise ValueError(f'owned C lost SDK retention roots: {sorted(missing)}')


def elf_globals(data):
    """Global defined symbols of a little-endian ELF32 object: name -> (section, bytes)."""
    import struct
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2e)
    sections = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]

    def string(table, offset):
        start = sections[table][4] + offset
        return data[start:data.index(b'\0', start)].decode()

    result = {}
    for sh in sections:
        if sh[1] != 2:                      # SHT_SYMTAB
            continue
        for offset in range(sh[4], sh[4] + sh[5], 16):
            name, value, size, info, _, shndx = struct.unpack_from('<IIIBBH', data, offset)
            if info >> 4 == 0 or shndx == 0 or shndx >= 0xff00:
                continue                    # local or undefined
            target = sections[shndx]
            content = data[target[4]:target[4] + target[5]]
            result[string(sh[6], name)] = (string(shstrndx, target[0]),
                                           content[value:value + size] if size else content[value:])
    return result


def ir_global_bytes(text, name):
    """Section and initializer bytes of a constant byte-array global in textual IR."""
    m = re.search(r'^@' + re.escape(name) + r' = [^\n]*\[\d+ x i8\] c"((?:[^"\\]|\\[0-9A-Fa-f]{2})*)"'
                  r'[^\n]*section "([^"]+)"', text, re.M)
    if not m:
        return None
    raw = re.sub(r'\\([0-9A-Fa-f]{2})', lambda h: chr(int(h[1], 16)), m[1]).encode('latin-1')
    return m[2], raw


def inspect(chip, output):
    members, _ = original(chip)
    _, tools, target, _ = paths(chip)
    clang = tools / 'clang'
    decoder = sha256(clang.read_bytes())
    scanner = sha256(Path(__file__).read_bytes())
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)

    def decode(member):
        row = dict(member=member.name, occurrence=member.occurrence,
                   sha256=member.digest, bytes=len(member.data))
        if member.data.startswith(b'\x7fELF'):
            atomic_write(output / member.key, member.data)
            return dict(row, format='elf', object=member.key)
        key = sha256(f'{target}:{decoder}:{scanner}:{member.digest}'.encode())
        cached = output / '.cache' / (key + '.ll')
        if not cached.exists():
            with tempfile.TemporaryDirectory(prefix='controller-decode-') as tmp:
                source = Path(tmp) / member.name
                source.write_bytes(member.data)
                data = subprocess.check_output([str(clang), '-target', target, '-x', 'ir',
                                                '-S', '-emit-llvm', str(source), '-o', '-'])
            # ModuleID contains a random temporary path; make artifacts stable.
            data = re.sub(rb'^; ModuleID = .*\n', b'', data, count=1)
            atomic_write(cached, data)
        text = cached.read_text()
        filename = member.key + '.ll'
        atomic_write(output / filename, text.encode())
        return dict(row, format='llvm', ir=filename, **symbols(text),
                    definitions=re.findall(r'^define [^\n]*@([^ (]+)\(', text, re.M),
                    references=sorted(set(re.findall(r'@([\w.$]+)', text))),
                    types=dict(re.findall(r'^(%[\w.$]+) = type (.*)$', text, re.M)),
                    inlined_locations=re.findall(r'^!\d+ = !DILocation\([^\n]*inlinedAt: !\d+[^\n]*', text, re.M))

    with ThreadPoolExecutor(max_workers=8) as pool:
        rows = list(pool.map(decode, members))
    result = dict(chip=chip, target=target, archive_sha256=paths(chip)[3], members=rows)
    atomic_write(output / 'inventory.json', (json.dumps(result, indent=2) + '\n').encode())
    return result


def manifest(path=MANIFEST):
    data = json.loads(Path(path).read_text())
    if data.get('schema') != 1 or data.get('chip') != 'bd19':
        raise ValueError('expected a schema 1 BD19 ownership manifest')
    keys = set()
    for entry in data['members']:
        key = entry['member'], entry['occurrence']
        if key in keys:
            raise ValueError(f'duplicate ownership entry {key}')
        keys.add(key)
        sources = set()
        for source in entry['sources']:
            p = Path(source)
            if not source.startswith(CONTROLLER) or '..' in p.parts or p.suffix != '.c':
                raise ValueError(f'invalid owned source {source}')
            if source in sources:
                raise ValueError(f'source repeated in one member: {source}')
            if not (ROOT / source).is_file():
                raise ValueError(f'missing owned source {source}')
            sources.add(source)
    return data


def ownership_groups(data):
    """Connected members sharing sources are accepted/audited as one unit."""
    pending = list(data['members'])
    groups = []
    while pending:
        group = [pending.pop(0)]
        sources = set(group[0]['sources'])
        changed = True
        while changed:
            changed = False
            for entry in pending[:]:
                if sources.intersection(entry['sources']):
                    pending.remove(entry)
                    group.append(entry)
                    sources.update(entry['sources'])
                    changed = True
        groups.append(group)
    return groups


def consistent_duplicates(modules):
    """A shared C export cannot silently choose between different SDK bodies."""
    from .compare import dependencies, strip_metadata
    seen = {}
    for label, mod in modules:
        for kind, names in symbols_for_module(mod).items():
            for name in names:
                if kind == 'functions':
                    content = mod['functions'][name]
                    deps = dependencies(mod, name)
                    definition = mod['canonical'][name]
                else:
                    text = strip_metadata(mod['globals'][name])
                    view = dict(mod, functions={'__global_check': text})
                    content, deps, definition = text, dependencies(view, '__global_check'), text
                referenced = set(re.findall(r'@([\w.$-]+)', content + '\n' + '\n'.join(deps[1].values())))
                imported = {n: mod['declarations'][n] for n in referenced if n in mod['declarations']}
                signature = definition, deps, imported
                key = kind, name
                if key in seen and seen[key][1] != signature:
                    raise ValueError(f'different SDK definitions of {name}: {seen[key][0]} / {label}')
                seen[key] = label, signature


def symbols_for_module(mod):
    text = '\n'.join(mod['functions'].values()) + '\n' + '\n'.join(mod['globals'].values())
    return symbols(text)


def selected_sources(path=MANIFEST):
    data = manifest(path)
    return sorted({s for entry in data['members'] for s in entry['sources']})


def complete(manifest_path=MANIFEST):
    """Every SDK member occurrence is owned: nothing links from btctrler.a."""
    data = manifest(manifest_path)
    members, _ = original('bd19')
    accepted = {(e['member'], e['occurrence']): e for e in data['members']}
    present = {(m.name, m.occurrence) for m in members}
    if accepted.keys() - present:
        raise ValueError(f'owned members absent from SDK: {accepted.keys() - present}')
    for member in members:
        entry = accepted.get((member.name, member.occurrence))
        if entry is None:
            raise ValueError(f'{member.key}: SDK member not owned')
        if entry['sha256'] != member.digest:
            raise ValueError(f'{member.key}: ownership fingerprint changed')
    print(f'bd19: all {len(members)} SDK btctrler.a members owned; no archive is linked')


def audit(board, manifest_path=MANIFEST):
    from .compare import module
    data = manifest(manifest_path)
    inventory = inspect('bd19', ROOT / 'build/controller-ir/bd19')
    rows = {(r['member'], r['occurrence']): r for r in inventory['members']}
    clang = paths('bd19')[1] / 'clang'
    for group in ownership_groups(data):
        required = {'functions': set(), 'globals': set()}
        retained = {'used': set(), 'compiler.used': set()}
        elf = {}
        modules = []
        for entry in group:
            sdk = rows[entry['member'], entry['occurrence']]
            if entry['sha256'] != sdk['sha256']:
                raise ValueError(f'{entry["member"]}: ownership fingerprint changed')
            if sdk['format'] == 'elf':
                elf.update(elf_globals((ROOT / 'build/controller-ir/bd19' / sdk['object']).read_bytes()))
                required['globals'].update(elf)
                continue
            for kind in required:
                required[kind].update(sdk[kind])
            sdk_text = (ROOT / 'build/controller-ir/bd19' / sdk['ir']).read_text()
            for kind, names in retention_roots(sdk_text).items():
                retained[kind].update(names)
            if len(group) > 1:
                modules.append((f'{entry["member"]}/{entry["occurrence"]}',
                                module(sdk_text)))
        consistent_duplicates(modules)
        provided = {'functions': set(), 'globals': set()}
        provided_retained = {'used': set(), 'compiler.used': set()}
        texts = []
        sources = sorted({s for entry in group for s in entry['sources']})
        for source in sources:
            obj = ROOT / 'build' / board / Path(source).with_suffix('.o')
            text = subprocess.check_output([str(clang), '-target', 'q32s', '-x', 'ir',
                                            '-S', '-emit-llvm', str(obj), '-o', '-'], text=True)
            for kind, names in symbols(text).items():
                provided[kind].update(names)
            for kind, names in retention_roots(text).items():
                provided_retained[kind].update(names)
            texts.append(text)
        for name, expected in elf.items():
            # A plain ELF member carries data only: same section, same bytes.
            if not any(ir_global_bytes(t, name) == expected for t in texts):
                raise ValueError(f'{name}: section/bytes differ from the SDK ELF member')
        require_retention(retained, provided_retained)
        for kind in provided:
            if provided[kind] != required[kind]:
                raise ValueError(f'{[e["member"] for e in group]} {kind}: '
                                 f'missing {required[kind] - provided[kind]}, '
                                 f'extra {provided[kind] - required[kind]}')
        label = ' + '.join(f'{e["member"]}/{e["occurrence"]}' for e in group)
        print(f'{label}: external definitions match')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    p = commands.add_parser('inspect')
    p.add_argument('--chip', choices=CHIPS, required=True)
    p.add_argument('--output', type=Path)
    p = commands.add_parser('complete')
    p.add_argument('--manifest', type=Path, default=MANIFEST)
    p = commands.add_parser('sources')
    p.add_argument('--manifest', type=Path, default=MANIFEST)
    p = commands.add_parser('audit')
    p.add_argument('--board', default='vanilla-bd19')
    p.add_argument('--manifest', type=Path, default=MANIFEST)
    p = commands.add_parser('compare')
    p.add_argument('--ir', type=Path, default=ROOT / 'build/controller-ir')
    p.add_argument('--output', type=Path, default=ROOT / 'build/controller-ir/reuse.json')
    p.add_argument('--owned-board', default='vanilla-br23')
    args = parser.parse_args()
    if args.command == 'inspect':
        result = inspect(args.chip, args.output or ROOT / f'build/controller-ir/{args.chip}')
        print(f'{args.chip}: {len(result["members"])} members inspected')
    elif args.command == 'complete':
        complete(args.manifest)
    elif args.command == 'sources':
        print(' '.join(selected_sources(args.manifest)))
    elif args.command == 'audit':
        audit(args.board, args.manifest)
    else:
        from .compare import compare
        compare(args.ir, args.output, args.owned_board)


if __name__ == '__main__':
    main()
