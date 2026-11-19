"""Cosmetic renaming must not hide layout, data, ABI or instruction changes."""
import json
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.btctrler.compare import canonical_function, compare, dependencies, module


class ReuseCensus(unittest.TestCase):
    def test_ssa_and_entry_labels_are_cosmetic(self):
        named = '''define i32 @sum(i32 %a, i32 %b) #0 {
entry:
  %result = add i32 %a, %b, !dbg !8
  ret i32 %result
}'''
        numbered = '''define i32 @sum(i32, i32) #9 {
  %3 = add i32 %0, %1
  ret i32 %3
}'''
        left = canonical_function(named, {'0': '{ nounwind minsize "target-cpu"="r3" }'})
        right = canonical_function(numbered, {'9': '{ "target-features"="+no-constpool" nounwind }'})
        self.assertEqual(left, right)
        self.assertNotEqual(left[1], canonical_function(numbered.replace('%0, %1', '%1, %0'),
                                                       {'9': '{ nounwind }'})[1])

    def test_semantic_instructions_and_attributes_are_preserved(self):
        text = 'define signext i8 @read(i8* %p) #0 section ".radio" {\n  %x = load volatile i8, i8* %p\n  ret i8 %x\n}'
        base = canonical_function(text, {'0': '{ nounwind "interrupt" }'})
        for changed in (text.replace('volatile ', ''), text.replace('signext', 'zeroext'),
                        text.replace('.radio', '.text')):
            self.assertNotEqual(base, canonical_function(changed, {'0': '{ nounwind "interrupt" }'}))
        self.assertNotEqual(base, canonical_function(text, {'0': '{ nounwind }'}))

    def test_quoted_asm_contents_are_not_renamed_or_stripped(self):
        text = '''define void @asm_test(i32 %value) {
entry:
  call void asm sideeffect "literal %value; #999 !dbg !123", "r"(i32 %value)
  ret void
}'''
        _, body = canonical_function(text, {})
        self.assertIn('"literal %value; #999 !dbg !123"', body)
        self.assertIn('(i32 %arg0)', body)

    def test_transitive_layouts_and_global_initializers_are_visible(self):
        text = '''%struct.outer = type { %struct.inner }
%struct.inner = type { i32 }
@table = constant %struct.outer zeroinitializer, !dbg !3
declare signext i8 @helper(i8* readonly) #0
define i32 @get() {
  %value = load i32, i32* bitcast (%struct.outer* @table to i32*)
  ret i32 %value
}
attributes #0 = { nounwind }
'''
        mod = module(text)
        types, globals_ = dependencies(mod, 'get')
        self.assertEqual(set(types), {'%struct.outer', '%struct.inner'})
        self.assertEqual(globals_['table'], '@table = constant %struct.outer zeroinitializer')
        self.assertIn('signext i8 @helper(i8* readonly)', mod['declarations']['helper'])

    def test_matching_body_with_changed_layout_is_not_a_reuse_candidate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for chip, field in [('br23', 'i32'), ('bd19', 'i64')]:
                directory = root / chip
                directory.mkdir()
                text = '%struct.state = type { i32, ' + field + ''' }
define i32 @get(%struct.state* %s) {
entry:
  %field = getelementptr %struct.state, %struct.state* %s, i32 0, i32 0
  %value = load i32, i32* %field
  ret i32 %value
}
'''
                (directory / '01--state.c.o.ll').write_text(text)
                (directory / 'inventory.json').write_text(json.dumps({'members': [
                    {'member': 'state.c.o', 'occurrence': 1, 'format': 'llvm', 'ir': '01--state.c.o.ll'}]}))
            with patch('tools.btctrler.compare.owned_exports', return_value=({'get': ['state.c']}, 1)), \
                 contextlib.redirect_stdout(io.StringIO()):
                result = compare(root, root / 'reuse.json', 'test-board')
            row = result['functions'][0]
            self.assertTrue(row['same_instructions'])
            self.assertEqual(row['classification'], 'same_instructions_check_boundaries')
            self.assertEqual(row['type_differences'], ['%struct.state'])

    def test_moved_symbol_is_matched_but_ambiguous_local_helper_is_not(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for chip, members in [('br23', ['old.c.o', 'another.c.o']), ('bd19', ['new.c.o'])]:
                directory = root / chip
                directory.mkdir()
                inventory = []
                for i, name in enumerate(members):
                    text = 'define internal void @helper() {\n  ret void\n}\n'
                    if i == 0:
                        text += 'define i32 @moved() {\n  ret i32 7\n}\n'
                    filename = name + '.ll'
                    (directory / filename).write_text(text)
                    inventory.append({'member': name, 'occurrence': 1, 'format': 'llvm', 'ir': filename})
                (directory / 'inventory.json').write_text(json.dumps({'members': inventory}))
            with patch('tools.btctrler.compare.owned_exports', return_value=({'moved': ['moved.c']}, 1)), \
                 contextlib.redirect_stdout(io.StringIO()):
                result = compare(root, root / 'reuse.json', 'test-board')
            rows = {r['function']: r for r in result['functions']}
            self.assertEqual(rows['moved']['classification'], 'reuse_candidate')
            self.assertEqual(rows['moved']['br23_member'], 'old.c.o')
            self.assertEqual(rows['helper']['classification'], 'no_unique_br23_counterpart')


if __name__ == '__main__':
    unittest.main()
