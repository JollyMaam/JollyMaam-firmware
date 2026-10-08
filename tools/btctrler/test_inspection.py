"""Run with: python3 tools/btctrler/test_inspection.py (or unittest discover -s tools/btctrler)"""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import inspection  # noqa: E402


IR = '''define void @foo() { ret void }
declare void @bar()
!10 = distinct !DISubprogram(name: "foo", scope: !0)
!2 = distinct !DISubprogram(name: "foo", scope: !0)
!9 = !DILexicalBlock(scope: !10)
!4 = !DILexicalBlock(scope: !2)
!7 = !DILexicalBlockFile(scope: !9)
!11 = !DILocation(line: 1, scope: !10, inlinedAt: !0)
!12 = !DILocation(line: 2, scope: !2, inlinedAt: !0)
!13 = !DILocation(line: 3, scope: !9, inlinedAt: !0)
!14 = !DILocation(line: 4, scope: !4, inlinedAt: !0)
!15 = !DILocation(line: 5, scope: !7, inlinedAt: !0)
'''


class InspectionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.clang = self.root / 'clang'
        self.clang.write_bytes(b'decoder version 1')
        self.source = self.root / 'archive.a'
        self.source.write_bytes(b'archive1')
        self.members = {'a.o': b'member1', 'b.o': b'member2'}
        self.decodes = []
        self.outputs = self.root / 'output'
        self.cache = self.root / 'cache'

    def run_tool(self, args, **kwargs):
        if args[0] == 'ar':
            for name, data in self.members.items():
                (Path(kwargs['cwd']) / name).write_bytes(data)
            return subprocess.CompletedProcess(args, 0)
        self.assertEqual(args[0], str(self.clang))
        data = Path(args[-3]).read_bytes()
        self.decodes.append(data)
        return subprocess.CompletedProcess(args, 0, IR.encode(), b'')

    def inspect(self, owned=('foo',)):
        with patch('inspection.subprocess.check_output',
                   return_value='\n'.join(self.members)), \
             patch('inspection.subprocess.run', side_effect=self.run_tool), \
             contextlib.redirect_stdout(io.StringIO()):
            inspection.inspect(self.source, self.outputs, str(self.clang), 'ar',
                               owned, cache=self.cache)
        return json.loads((self.outputs / 'inventory.json').read_text())

    def test_scope_order_and_all_metadata(self):
        data = inspection.scan(IR)
        self.assertEqual(data['definitions'], ['foo'])
        self.assertEqual(data['symbol_references'], {'foo': 1, 'bar': 1})
        self.assertEqual(data['inlined_locations']['foo'],
                         [line for line in IR.splitlines() if '!DILocation' in line])

    def test_reuse_and_changed_member(self):
        first = self.inspect()
        mtimes = {p.name: p.stat().st_mtime_ns for p in self.outputs.glob('*.ll')}
        self.assertEqual(len(self.decodes), 2)
        self.assertEqual(self.inspect(), first)
        self.assertEqual(len(self.decodes), 2)
        self.assertEqual(mtimes, {p.name: p.stat().st_mtime_ns
                                  for p in self.outputs.glob('*.ll')})
        self.members['b.o'] = b'member3'
        self.inspect()
        self.assertEqual(self.decodes, [b'member1', b'member2', b'member3'])

    def test_ownership_changes_without_decoding(self):
        self.inspect()
        result = self.inspect(('bar',))
        self.assertEqual(len(self.decodes), 2)
        for member in result['members']:
            self.assertEqual(member['owned_inlined_locations'], {})
            self.assertEqual(member['owned_symbol_references'], {'bar': 1})

    def test_shared_cache_across_output_directories(self):
        first = self.inspect()
        self.outputs = self.root / 'other-worktree' / 'output'
        self.assertEqual(self.inspect(), first)
        self.assertEqual(len(self.decodes), 2)

    def test_removed_member_and_native_elf(self):
        self.inspect()
        del self.members['b.o']
        self.members['native.o'] = b'\x7fELF native member'
        result = self.inspect()
        self.assertFalse((self.outputs / 'b.o.ll').exists())
        self.assertFalse((self.outputs / 'native.o.ll').exists())
        self.assertEqual(len(self.decodes), 2)
        self.assertEqual(result['members'][1]['format'], 'native ELF')

    def test_decoder_change_and_corrupt_cache(self):
        self.inspect()
        self.clang.write_bytes(b'decoder version 2')
        self.inspect()
        self.assertEqual(len(self.decodes), 4)
        # Corrupt cached IR for both decoder versions; only the current key
        # is used, and one member must be decoded again.
        for p in self.cache.rglob(hashlib.sha256(b'member1').hexdigest() + '.ll'):
            p.write_bytes(b'corrupt')
        self.inspect()
        self.assertEqual(len(self.decodes), 5)

    def test_unused_versions_are_pruned(self):
        self.inspect()
        old = next(self.cache.iterdir())
        self.clang.write_bytes(b'decoder version 2')
        self.inspect()
        self.assertEqual(len(list(self.cache.iterdir())), 2)
        stale = time.time() - inspection.STALE_SECONDS - 60
        os.utime(old, (stale, stale))
        self.inspect()
        self.assertEqual(len(list(self.cache.iterdir())), 1)
        self.assertFalse(old.exists())

    def test_concurrent_same_member_is_decoded_once(self):
        member = self.root / 'a.o'
        member.write_bytes(b'member1')
        self.cache.mkdir()
        started, release = threading.Event(), threading.Event()
        results, errors = [], []

        def decode(args, **kwargs):
            self.decodes.append(b'member1')
            started.set()
            self.assertTrue(release.wait(2))
            return subprocess.CompletedProcess(args, 0, IR.encode(), b'')

        def worker():
            try:
                results.append(inspection.member_data(member, self.cache, str(self.clang)))
            except Exception as e:
                errors.append(e)

        with patch('inspection.subprocess.run', side_effect=decode):
            a, b = threading.Thread(target=worker), threading.Thread(target=worker)
            a.start()
            self.assertTrue(started.wait(2))
            b.start()
            b.join(.05)
            self.assertTrue(b.is_alive())
            release.set()
            a.join(2)
            b.join(2)
        self.assertFalse(a.is_alive() or b.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual(len(self.decodes), 1)
        self.assertEqual(sorted(r[2] for r in results), [False, True])

    def test_failed_decode_is_not_cached(self):
        member = self.root / 'bad.o'
        member.write_bytes(b'bad bitcode')
        self.cache.mkdir()
        result = subprocess.CompletedProcess([], 1, b'', b'invalid bitcode')
        with patch('inspection.subprocess.run', return_value=result):
            with self.assertRaisesRegex(RuntimeError, 'bad.o:.*invalid bitcode'):
                inspection.member_data(member, self.cache, str(self.clang))
        self.assertEqual(list(self.cache.glob('*.json')), [])
        self.assertEqual(list(self.cache.glob('*.ll')), [])


if __name__ == '__main__':
    unittest.main()
