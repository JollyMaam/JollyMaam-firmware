"""Read GNU ar archives without losing repeated member names."""
from collections import Counter
from dataclasses import dataclass
import hashlib
from pathlib import Path


def sha256(data):
    return hashlib.sha256(data).hexdigest()


@dataclass(frozen=True)
class Member:
    name: str
    occurrence: int
    header: bytes
    data: bytes
    offset: int = 0

    @property
    def key(self):
        return f'{self.occurrence:02d}--{self.name}'

    @property
    def digest(self):
        return sha256(self.data)


def read_archive(path):
    data = Path(path).read_bytes()
    if not data.startswith(b'!<arch>\n'):
        raise ValueError(f'{path}: expected a GNU ar archive')
    offset, names, counts, members, table = 8, b'', Counter(), [], b''
    while offset < len(data):
        member_offset = offset
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:] != b'`\n':
            raise ValueError(f'{path}: invalid member header at {offset}')
        size = int(header[48:58])
        payload = data[offset + 60:offset + 60 + size]
        if len(payload) != size:
            raise ValueError(f'{path}: truncated member at {offset}')
        offset += 60 + size + size % 2
        name = header[:16].decode('ascii').strip()
        if name == '//':
            names = payload
            table = header + payload + (b'\n' if size % 2 else b'')
            continue
        if name in ('/', '/SYM64/'):
            continue
        if name.startswith('/') and name[1:].isdigit():
            index = int(name[1:])
            if index >= len(names) or b'/\n' not in names[index:]:
                raise ValueError(f'{path}: invalid long member name')
            name = names[index:].split(b'/\n', 1)[0].decode('utf-8')
        elif name.startswith('#1/'):
            raise ValueError('BSD extended names are not supported')
        else:
            name = name.removesuffix('/')
        if not name or '/' in name or name in ('.', '..'):
            raise ValueError(f'{path}: unsafe member name {name!r}')
        counts[name] += 1
        members.append(Member(name, counts[name], header, payload, member_offset))
    if offset != len(data):
        raise ValueError(f'{path}: missing archive padding')
    return members, table


def index_entries(path):
    """Resolve GNU ar symbol offsets to exact member occurrences."""
    members, _ = read_archive(path)
    by_offset = {m.offset: m for m in members}
    data = Path(path).read_bytes()
    offset = 8
    while offset < len(data):
        header = data[offset:offset + 60]
        size = int(header[48:58])
        name = header[:16].strip()
        payload = data[offset + 60:offset + 60 + size]
        offset += 60 + size + size % 2
        if name not in (b'/', b'/SYM64/'):
            continue
        width = 4 if name == b'/' else 8
        count = int.from_bytes(payload[:width], 'big')
        end = width * (count + 1)
        if len(payload) < width or end > len(payload):
            raise ValueError(f'{path}: truncated symbol index')
        strings = payload[end:].split(b'\0')
        if len(strings) < count + 1:
            raise ValueError(f'{path}: truncated symbol names')
        result = []
        for i in range(count):
            member_offset = int.from_bytes(payload[width * (i + 1):width * (i + 2)], 'big')
            if member_offset not in by_offset:
                raise ValueError(f'{path}: symbol points outside archive members')
            result.append((strings[i].decode(), by_offset[member_offset]))
        return result
    raise ValueError(f'{path}: missing symbol index')


def write_archive(path, members, name_table):
    """Preserve names, order, headers and payloads; caller rebuilds the index."""
    with Path(path).open('wb') as out:
        out.write(b'!<arch>\n' + name_table)
        for member in members:
            out.write(member.header + member.data)
            if len(member.data) % 2:
                out.write(b'\n')
