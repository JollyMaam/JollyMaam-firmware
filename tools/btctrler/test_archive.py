"""Regression checks for the BD19 archive's repeated-name ownership boundary."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.btctrler.archive import index_entries, read_archive, write_archive
from tools.btctrler import controller


def record(name, data):
    header = f'{name:<16}{0:<12}{0:<6}{0:<6}{100644:<8}{len(data):<10}`\n'.encode()
    return header + data + (b'\n' if len(data) % 2 else b'')


class Archives(unittest.TestCase):
    def test_link_retention_cannot_be_replaced_by_compiler_only_retention(self):
        expected = controller.retention_roots('@llvm.used = appending global [1 x i8*] [i8* @bt]\n')
        weaker = controller.retention_roots('@llvm.compiler.used = appending global [1 x i8*] [i8* @bt]\n')
        with self.assertRaisesRegex(ValueError, 'lost SDK retention roots'):
            controller.require_retention(expected, weaker)
        controller.require_retention(expected, expected)
        controller.require_retention(weaker, expected)

    def test_shared_sources_form_transitive_ownership_groups(self):
        a = {'member': 'a', 'sources': ['one.c']}
        b = {'member': 'b', 'sources': ['one.c', 'two.c']}
        c = {'member': 'c', 'sources': ['two.c']}
        d = {'member': 'd', 'sources': ['separate.c']}
        groups = controller.ownership_groups({'members': [a, c, b, d]})
        self.assertEqual([{e['member'] for e in group} for group in groups], [{'a', 'b', 'c'}, {'d'}])

    def test_conflicting_duplicate_sdk_exports_are_rejected(self):
        from tools.btctrler.compare import module
        first = module('define i32 @same_name() {\n  ret i32 1\n}')
        second = module('define i32 @same_name() {\n  ret i32 2\n}')
        controller.consistent_duplicates([('first', first), ('same', first)])
        with self.assertRaisesRegex(ValueError, 'different SDK definitions'):
            controller.consistent_duplicates([('first', first), ('second', second)])
        first = module('declare void @notify(i8*)\ndefine void @same_name(i8* %p) {\n  call void @notify(i8* %p)\n  ret void\n}')
        second = module('declare void @notify(i8* readonly)\ndefine void @same_name(i8* %p) {\n  call void @notify(i8* %p)\n  ret void\n}')
        with self.assertRaisesRegex(ValueError, 'different SDK definitions'):
            controller.consistent_duplicates([('first', first), ('second', second)])

    def test_index_distinguishes_repeated_member_occurrences(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'input.a'
            names = b'first\0second\0'
            size = 12 + len(names)
            first_offset = 8 + len(record('/', bytes(size)))
            first = record('tws_pair.c.o/', b'first payload')
            second_offset = first_offset + len(first)
            index = b''.join(n.to_bytes(4, 'big') for n in (2, first_offset, second_offset)) + names
            source.write_bytes(b'!<arch>\n' + record('/', index) + first +
                               record('tws_pair.c.o/', b'second payload'))
            self.assertEqual([(n, m.occurrence, m.data) for n, m in index_entries(source)],
                             [('first', 1, b'first payload'), ('second', 2, b'second payload')])
            invalid = (1).to_bytes(4, 'big') + (0).to_bytes(4, 'big') + b'bad\0'
            source.write_bytes(b'!<arch>\n' + record('/', invalid) + first)
            with self.assertRaisesRegex(ValueError, 'outside archive members'):
                index_entries(source)

    def test_repeated_names_long_names_order_and_padding(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, output = Path(tmp) / 'input.a', Path(tmp) / 'output.a'
            table = record('//', b'a_long_filename.c.o/\n')
            source.write_bytes(b'!<arch>\n' + table + record('tws_pair.c.o/', b'first') +
                               record('/0', b'long') + record('tws_pair.c.o/', b'second'))
            members, names = read_archive(source)
            self.assertEqual([(m.name, m.occurrence, m.data) for m in members], [
                ('tws_pair.c.o', 1, b'first'), ('a_long_filename.c.o', 1, b'long'),
                ('tws_pair.c.o', 2, b'second')])
            write_archive(output, [members[0], members[2]], names)
            reread, _ = read_archive(output)
            self.assertEqual([(m.name, m.occurrence, m.data) for m in reread], [
                ('tws_pair.c.o', 1, b'first'), ('tws_pair.c.o', 2, b'second')])

    def test_reject_truncated_and_unsafe_members(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'input.a'
            for data in (b'!<arch>\n' + record('ok/', b'test')[:-2],
                         b'!<arch>\n' + record('../escape/', b'test')):
                source.write_bytes(data)
                with self.assertRaises(ValueError):
                    read_archive(source)

    def test_external_definitions_exclude_private_helpers_and_declarations(self):
        text = '''define internal void @helper() {
}
define weak i8 @api() {
}
@pool = common global [4 x i8] zeroinitializer, align 4
@llvm.used = appending global [1 x i8*] zeroinitializer
@callback = alias void (), void ()* @api
@private = private constant [4 x i8] c"abc\\00"
@imported = external global i32
'''
        self.assertEqual(controller.symbols(text), {'functions': ['api'], 'globals': ['callback', 'pool']})

    def test_target_and_cache_do_not_cross_chips(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            decoder = root / 'clang'
            decoder.write_bytes(b'test decoder')
            source = root / 'input.a'
            source.write_bytes(b'!<arch>\n' + record('test.c.o/', b'BC content'))
            members, table = read_archive(source)
            calls = []

            def decode(command):
                calls.append(command)
                return b'define void @api() {\n  ret void\n}\n'

            def paths(chip):
                return source, root, 'q32s' if chip == 'bd19' else 'pi32v2', 'pinned'

            with patch.object(controller, 'original', return_value=(members, table)), \
                 patch.object(controller, 'paths', side_effect=paths), \
                 patch.object(controller.subprocess, 'check_output', side_effect=decode):
                for chip in ('bd19', 'bd19', 'br23'):
                    controller.inspect(chip, root / 'ir')
            self.assertEqual([c[c.index('-target') + 1] for c in calls], ['q32s', 'pi32v2'])

    def test_reject_stale_ownership_hash(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'input.a'
            source.write_bytes(b'!<arch>\n' + record('member.c.o/', b'payload'))
            data = {'chip': 'bd19', 'schema': 1, 'members': [
                {'member': 'member.c.o', 'occurrence': 1, 'sha256': 'stale', 'sources': []}]}
            manifest = root / 'manifest.json'
            manifest.write_text(json.dumps(data))
            with patch.object(controller, 'original', return_value=read_archive(source)):
                with self.assertRaisesRegex(ValueError, 'fingerprint changed'):
                    controller.retained(root / 'output.a', manifest)


if __name__ == '__main__':
    unittest.main()
