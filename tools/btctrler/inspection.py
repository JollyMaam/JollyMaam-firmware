"""Cached IR decoding and ownership-independent controller metadata scans."""
from collections import Counter, defaultdict
import fcntl
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def scan(text):
    """Index debug metadata once, then follow lexical scopes per function."""
    functions = defaultdict(list)
    children = defaultdict(list)
    locations = defaultdict(list)
    order = {}
    for index, line in enumerate(text.splitlines()):
        node = re.match(r'^!(\d+) =', line)
        if node:
            order[node[1]] = index
        m = re.match(r'^!(\d+) = .*!DISubprogram\(name: "([^"\n]+)"', line)
        if m:
            functions[m[2]].append(m[1])
        m = re.match(r'^!(\d+) = (?:distinct )?!DILexicalBlock(?:File)?\([^\n]*scope: !(\d+)', line)
        if m:
            children[m[2]].append(m[1])
        m = re.match(r'^!\d+ = !DILocation\([^\n]*scope: !(\d+), inlinedAt: !\d+', line)
        if m:
            locations[m[1]].append(line)
    inlined = {}
    for name, roots in functions.items():
        scopes = list(roots)
        seen = set(scopes)
        for scope in scopes:
            for child in children[scope]:
                if child not in seen:
                    seen.add(child)
                    scopes.append(child)
        # Preserve the old inventory's metadata order at each scope depth.
        # Function scopes are first, followed by lexical scopes in IR order.
        depths = {s: 0 for s in roots}
        for scope in scopes:
            for child in children[scope]:
                if child not in depths:
                    depths[child] = depths[scope] + 1
        scopes.sort(key=lambda s: (depths[s], order[s]))
        found = [line for scope in scopes for line in locations[scope]]
        if found:
            inlined[name] = found
    return {
        'definitions': re.findall(r'^define [^\n]*@([^ (]+)\(', text, re.M),
        'symbol_references': dict(Counter(re.findall(r'@([^\s()]+)\(', text))),
        'inlined_locations': inlined,
    }


def atomic_write(path, data):
    path = Path(path)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as f:
        temporary = Path(f.name)
        try:
            f.write(data)
            f.close()
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


def cache_root(output):
    # All worktrees share the same Git directory, so they share decoded IR.
    result = subprocess.run(['git', 'rev-parse', '--git-common-dir'], cwd=output,
                            capture_output=True, text=True)
    if result.returncode == 0:
        return (output / result.stdout.strip()).resolve() / 'controller-ir-cache'
    return output.parent / '.controller-ir-cache'


def member_data(path, cache, clang):
    member_sha = digest(path)
    record = cache / (member_sha + '.json')
    ir = cache / (member_sha + '.ll')
    # Concurrent workers must never read half-written cache entries or decode
    # the same member simultaneously. Each member has its own lock.
    with (cache / (member_sha + '.lock')).open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            metadata = json.loads(record.read_text())
            assert metadata['sha256'] == member_sha
            if metadata.get('format') == 'native ELF':
                return metadata, None, True
            data = ir.read_bytes()
            assert hashlib.sha256(data).hexdigest() == metadata['ir_sha256']
            return metadata, data, True
        except (OSError, ValueError, KeyError, AssertionError):
            pass
        if path.read_bytes().startswith(b'\x7fELF'):
            metadata = {'sha256': member_sha, 'format': 'native ELF',
                        'definitions': [], 'inlined_locations': {}}
            data = None
        else:
            result = subprocess.run([clang, '-target', 'pi32v2', '-x', 'ir', '-S',
                                     '-emit-llvm', str(path), '-o', '-'],
                                    capture_output=True)
            if result.returncode:
                raise RuntimeError(f'{path.name}: unable to decode LLVM bitcode: '
                                   + result.stderr.decode(errors='replace'))
            data = result.stdout
            metadata = {'sha256': member_sha, **scan(data.decode()),
                        'ir_sha256': hashlib.sha256(data).hexdigest()}
            atomic_write(ir, data)
        atomic_write(record, (json.dumps(metadata) + '\n').encode())
        return metadata, data, False


def inspect(source, output, clang, ar, owned, cache=None):
    source, output = Path(source), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    # The key covers the actual decoder and scanner implementation, not
    # mtimes or source paths. Ownership changes only filter cached metadata.
    key = hashlib.sha256(json.dumps({'decoder': digest(clang),
                                    'scanner': digest(__file__),
                                    'target': 'pi32v2', 'schema': 1},
                                   sort_keys=True).encode()).hexdigest()
    cache = (Path(cache) if cache is not None else cache_root(output)) / key
    cache.mkdir(parents=True, exist_ok=True)
    members = subprocess.check_output([ar, 't', str(source)], text=True).splitlines()
    inventory, expected = [], set()
    reused = 0
    with tempfile.TemporaryDirectory(prefix='cyclone2-controller-inspect-') as tmp:
        directory = Path(tmp)
        subprocess.run([ar, 'x', str(source)], cwd=directory, check=True)
        for member in members:
            metadata, data, hit = member_data(directory / member, cache, clang)
            reused += hit
            item = {'member': member, 'sha256': metadata['sha256'],
                    'definitions': metadata['definitions'],
                    'owned_inlined_locations': {n: metadata['inlined_locations'][n]
                                               for n in owned
                                               if n in metadata['inlined_locations']}}
            if data is None:
                item['format'] = metadata['format']
            else:
                filename = member + '.ll'
                expected.add(filename)
                ir = output / filename
                if not ir.exists() or ir.read_bytes() != data:
                    atomic_write(ir, data)
                item['owned_symbol_references'] = {
                    n: metadata['symbol_references'][n] for n in owned
                    if n in metadata['symbol_references']}
            inventory.append(item)
    for stale in output.glob('*.ll'):
        if stale.name not in expected:
            stale.unlink()
    atomic_write(output / 'inventory.json',
                 (json.dumps({'archive_sha256': digest(source),
                              'members': inventory}, indent=2) + '\n').encode())
    inline_members = [x['member'] for x in inventory if x['owned_inlined_locations']]
    print(f'{len(members)} members, {sum(len(x["definitions"]) for x in inventory)} definitions; '
          f'owned function inline evidence: {inline_members}; '
          f'cache reused {reused}/{len(members)}')
