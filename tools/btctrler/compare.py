"""Conservative cross-chip reuse census; classifications never accept code."""
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import subprocess

from .controller import ROOT, atomic_write, paths


def split_parameters(text):
    result, start, depth = [], 0, 0
    for i, ch in enumerate(text + ','):
        if ch in '([{<':
            depth += 1
        elif ch in ')]}>':
            depth -= 1
        elif ch == ',' and depth == 0:
            result.append(text[start:i].strip())
            start = i + 1
    return [p for p in result if p]


def parameter_bounds(header):
    start = header.index('(', header.index('@')) + 1
    depth = 1
    for i in range(start, len(header)):
        if header[i] == '(':
            depth += 1
        elif header[i] == ')':
            depth -= 1
            if depth == 0:
                return start, i
    raise ValueError(f'unbalanced function header: {header}')


def strip_metadata(text):
    return re.sub(r'"(?:[^"\\]|\\.)*"|,? ![\w.]+ !\d+',
                  lambda m: m[0] if m[0].startswith('"') else '', text)


def attributes(text, groups):
    def expand(match):
        tokens = re.findall(r'"[^"\n]*"(?:="[^"\n]*")?|[^\s{}]+', groups[match[1]])
        # These choose code generation for the already-known target. Preserve
        # calling conventions, interrupt/naked attributes and semantic flags.
        tokens = [t for t in tokens if t != 'minsize' and
                  not t.startswith(('"target-cpu"', '"target-features"'))]
        return '{' + ' '.join(sorted(tokens)) + '}'
    return re.sub(r'"(?:[^"\\]|\\.)*"|#(\d+)',
                  lambda m: m[0] if m[0].startswith('"') else expand(m), text)


def canonical_function(text, groups):
    """Rename values/blocks bijectively; preserve operand identities and order.

    Equality is a triage signal. Debug/analysis metadata is omitted; optimizer
    loop rotation or inlining is deliberately NOT treated as equivalence.
    """
    header, body = text.split('\n', 1)
    header = strip_metadata(header)
    start, end = parameter_bounds(header)
    mapping, slot, parameters = {}, 0, []
    for i, parameter in enumerate(split_parameters(header[start:end])):
        if parameter == '...':
            parameters.append(parameter)
            continue
        named = re.search(r'\s(%[\w.$-]+)$', parameter)
        if named:
            old = named[1]
            parameter = parameter[:named.start()].rstrip()
            if old[1:].isdigit():
                slot = max(slot, int(old[1:]) + 1)
        else:
            old = '%' + str(slot)
            slot += 1
        mapping[old] = f'%arg{i}'
        parameters.append(parameter)
    signature = attributes(header[:start] + ', '.join(parameters) + header[end:], groups)
    signature = re.sub(r'^(define|declare) ', '', signature).removesuffix(' {')
    # Unnamed blocks are printed as LLVM comments, but they are CFG labels.
    body = re.sub(r'^; <label>:(\d+):.*$', r'\1:', body, flags=re.M)
    lines = []
    for line in body.splitlines():
        if '@llvm.dbg.' in line or line.lstrip().startswith(';'):
            continue
        line = strip_metadata(line)
        # Do not split semicolons or % tokens inside quoted strings/asm.
        line = re.sub(r'"(?:[^"\\]|\\.)*"|;.*',
                      lambda m: m[0] if m[0].startswith('"') else '', line).strip()
        if line:
            lines.append(line)
    implicit_entry = not re.fullmatch(r'[\w.$-]+:', lines[0])
    if implicit_entry:
        mapping['%' + str(slot)] = '%bb0'
    labels = re.findall(r'^([\w.$-]+):$', '\n'.join(lines), re.M)
    for i, label in enumerate(labels, int(implicit_entry)):
        mapping['%' + label] = f'%bb{i}'
    for i, value in enumerate(re.findall(r'^(%[\w.$-]+) =', '\n'.join(lines), re.M)):
        mapping[value] = f'%v{i}'
    result = ['bb0:'] if implicit_entry else []
    for line in lines:
        if re.fullmatch(r'[\w.$-]+:', line):
            result.append(mapping['%' + line[:-1]][1:] + ':')
            continue
        line = re.sub(r'"(?:[^"\\]|\\.)*"|%[\w.$-]+',
                      lambda m: m[0] if m[0].startswith('"') else mapping.get(m[0], m[0]), line)
        result.append(attributes(line, groups))
    return signature, '\n'.join(result)


def module(text):
    groups = dict(re.findall(r'^attributes #(\d+) = (.*)$', text, re.M))
    functions = {m[1]: m[0] for m in re.finditer(
        r'^define [^\n]*@([^ (]+)\([^\n]*\n.*?^}', text, re.M | re.S)}
    declarations = {}
    for line, name in re.findall(r'^(declare [^\n]*@([^ (]+)\([^\n]*)$', text, re.M):
        declarations[name] = canonical_function(line + ' {\n}', groups)[0]
    return dict(functions=functions, groups=groups,
                canonical={n: canonical_function(f, groups) for n, f in functions.items()},
                types=dict(re.findall(r'^(%[\w.$-]+) = type (.*)$', text, re.M)),
                globals={n: strip_metadata(line) for line, n in
                         re.findall(r'^(@([\w.$-]+) = [^\n]*)$', text, re.M)},
                declarations=declarations)


def dependencies(mod, function):
    """Include transitive type/global definitions, not just their names."""
    types, globals_, todo = {}, {}, [mod['functions'][function]]
    while todo:
        text = todo.pop()
        for name in re.findall(r'%[\w.$-]+', text):
            if name in mod['types'] and name not in types:
                types[name] = mod['types'][name]
                todo.append(types[name])
        for name in re.findall(r'@([\w.$-]+)', text):
            if name in mod['globals'] and name not in globals_:
                globals_[name] = mod['globals'][name]
                todo.append(globals_[name])
    return types, globals_


def difference(a, b):
    return sorted(n for n in a.keys() | b.keys() if a.get(n) != b.get(n))


def load(directory):
    inventory = json.loads((directory / 'inventory.json').read_text())
    modules, index, by_name = {}, {}, defaultdict(list)
    for row in inventory['members']:
        if row['format'] != 'llvm':
            continue
        key = row['member'], row['occurrence']
        mod = module((directory / row['ir']).read_text())
        modules[key] = mod
        for name in mod['functions']:
            identity = (*key, name)
            index[identity] = mod
            by_name[name].append(identity)
    return modules, index, by_name


def owned_exports(board):
    """Locate actual firmware definitions, including assembly-name aliases."""
    directory = ROOT / 'build' / board
    sources = defaultdict(list)
    clang = paths('br23')[1] / 'clang'
    objects = sorted((directory / 'src/btctrler').glob('*.o'))
    if not objects:
        raise ValueError(f'no owned controller objects: build {board} first')

    def read(obj):
        text = subprocess.check_output([str(clang), '-target', 'pi32v2', '-x', 'ir',
                                        '-S', '-emit-llvm', str(obj), '-o', '-'], text=True)
        names = []
        for header in re.findall(r'^define [^\n]*', text, re.M):
            if not re.search(r'\b(internal|private|available_externally)\b', header.split('@', 1)[0]):
                names.append(re.search(r'@([^ (]+)\(', header)[1])
        return str(obj.relative_to(directory).with_suffix('.c')), names

    with ThreadPoolExecutor(max_workers=8) as pool:
        for source, names in pool.map(read, objects):
            for name in names:
                sources[name].append(source)
    return sources, len(objects)


def compare(directory, output, board):
    _, br23, br_names = load(directory / 'br23')
    _, bd19, bd_names = load(directory / 'bd19')
    owned, object_count = owned_exports(board)
    rows = []
    for identity, b in sorted(bd19.items()):
        member, occurrence, name = identity
        counterpart = identity if identity in br23 else None
        if counterpart is None and len(br_names[name]) == len(bd_names[name]) == 1:
            counterpart = br_names[name][0]
        row = dict(member=member, occurrence=occurrence, function=name,
                   owned_sources=owned.get(name, []),
                   internal=bool(re.search(r'^define (?:internal|private)\b', b['functions'][name])))
        if counterpart is None:
            row['classification'] = 'no_unique_br23_counterpart'
            rows.append(row)
            continue
        a = br23[counterpart]
        row['br23_member'], row['br23_occurrence'], _ = counterpart
        sig_a, body_a = a['canonical'][name]
        sig_b, body_b = b['canonical'][name]
        row['same_instructions'] = body_a == body_b
        row['same_signature_attributes_section'] = sig_a == sig_b
        at, ag = dependencies(a, name)
        bt, bg = dependencies(b, name)
        row['type_differences'] = difference(at, bt)
        row['global_differences'] = difference(ag, bg)
        references = set(re.findall(r'@([\w.$-]+)', a['functions'][name] + b['functions'][name]))
        row['declaration_differences'] = difference(
            {n: a['declarations'][n] for n in references if n in a['declarations']},
            {n: b['declarations'][n] for n in references if n in b['declarations']})
        if not row['same_instructions']:
            row['classification'] = 'different_instructions'
        elif (not row['same_signature_attributes_section'] or row['type_differences'] or
              row['global_differences'] or row['declaration_differences']):
            row['classification'] = 'same_instructions_check_boundaries'
        else:
            row['classification'] = 'reuse_candidate'
        rows.append(row)
    counts = Counter(r['classification'] for r in rows)
    public = [r for r in rows if not r['internal']]
    boundaries = [r for r in rows if r['classification'] == 'same_instructions_check_boundaries']
    hotspots = {kind: dict(Counter(n for r in boundaries for n in r[kind]).most_common())
                for kind in ('type_differences', 'global_differences', 'declaration_differences')}
    result = dict(schema=1, notice='Conservative triage, not equivalence proof or automatic acceptance. '
                  'Different instructions can still come from the same C after optimization. '
                  'Body comparison omits debug/analysis metadata and target code-generation attributes. '
                  'Callee implementations and current app changes require separate review. '
                  'Existing C coverage reflects compiled board objects; rebuild that board first.',
                  owned_board=board, owned_objects_scanned=object_count,
                  counts=dict(counts), public_counts=dict(Counter(r['classification'] for r in public)),
                  boundary_hotspots=hotspots,
                  reuse_candidates_with_owned_c=sum(r['classification'] == 'reuse_candidate' and
                                                   bool(r['owned_sources']) for r in public),
                  functions=rows)
    atomic_write(output, (json.dumps(result, indent=2) + '\n').encode())
    member_rows = defaultdict(list)
    for row in rows:
        member_rows[(row['member'], row['occurrence'])].append(row)
    md = ['# BR23 to BD19 reuse census', '', result['notice'], '',
          '| Classification | Definitions |', '| --- | ---: |']
    md += [f'| {kind} | {count} |' for kind, count in sorted(counts.items())]
    md += ['', f'Public reuse candidates already supplied by C: {result["reuse_candidates_with_owned_c"]}.', '',
           'Candidate means reuse the existing C and audit its boundary/app changes; it does not',
           'mean automatically accept a whole member. Callees can legitimately be chip-specific.', '',
           '## Repeated boundaries', '',
           'Review a shared type/global/interface once, then reuse that finding for its callers.',
           'Type checks include transitive definitions, so these counts are conservative.', '']
    for kind, names in hotspots.items():
        md += [f'### {kind}', '', '| Boundary | Matching bodies affected |', '| --- | ---: |']
        md += [f'| {name} | {count} |' for name, count in list(names.items())[:12]]
        md += ['']
    md += ['## Members', '',
           '| Member / occurrence | Reuse candidates | Boundary checks | Different instructions | Unmatched |',
           '| --- | ---: | ---: | ---: | ---: |']
    for (member, occurrence), items in sorted(member_rows.items()):
        c = Counter(r['classification'] for r in items)
        md.append(f'| {member} / {occurrence} | {c["reuse_candidate"]} | '
                  f'{c["same_instructions_check_boundaries"]} | {c["different_instructions"]} | '
                  f'{c["no_unique_br23_counterpart"]} |')
    atomic_write(output.with_suffix('.md'), ('\n'.join(md) + '\n').encode())
    print(f'BD19: {len(rows)} definitions; {object_count} BR23 owned objects scanned')
    for kind, count in sorted(counts.items()):
        print(f'  {kind}: {count}')
    print(f'  public reuse candidates with existing C: {result["reuse_candidates_with_owned_c"]}')
    return result
