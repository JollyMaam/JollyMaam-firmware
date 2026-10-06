"""Regression checks for extraction declarations and callback ABI types.

Run with python3 tools/btctrler/test_ir.py.
"""
import hashlib
import unittest
from ir import declaration, function, own_all


class DeclarationTests(unittest.TestCase):
    def check(self, header, expected):
        text = header + '\nentry:\n  ret void\n}\n'
        self.assertEqual(declaration(text, 'handler'), expected)

    def test_callback_followed_by_scalar(self):
        self.check('define internal i32 @handler(void (i8)* nocapture readnone %fun, i8 zeroext %priority) #3 {',
                   'i32 @handler(void (i8)*, i8 zeroext)')

    def test_callback_with_multiple_arguments(self):
        self.check('define void @handler(void (i32, %struct.conn*)* %callback) local_unnamed_addr #0 {',
                   'void @handler(void (i32, %struct.conn*)*)')

    def test_nested_callback_and_aggregate(self):
        self.check('define void @handler(void (void (i8, i32)*, i32)* %callback, { i32, [4 x i8] } %state) {',
                   'void @handler(void (void (i8, i32)*, i32)*, { i32, [4 x i8] })')

    def test_scalar_attributes_and_varargs(self):
        self.check('define internal fastcc zeroext i8 @handler(i8* nocapture readonly %data, i16 zeroext %handle, ...) #0 {',
                   'zeroext i8 @handler(i8*, i16 zeroext, ...)')

    def test_empty_parameters_and_trailing_parentheses(self):
        self.check('define i32 @handler() #0 section ".code(callback)" {',
                   'i32 @handler()')

    def test_extraction_preserves_callback_global_reference(self):
        text = ('@slot = global i32 (void (i8)*, i8)* @handler\n'
                'define internal i32 @handler(void (i8)* nocapture readnone %fun, i8 zeroext %priority) #3 {\n'
                'entry:\n  ret i32 0\n}\n')
        start, end = function(text, 'handler')
        fingerprint = hashlib.sha256(text[start:end].encode()).hexdigest()
        result = own_all(text, {'handler': fingerprint})
        self.assertIn('@slot = global i32 (void (i8)*, i8)* @handler', result)
        self.assertIn('declare i32 @handler(void (i8)*, i8 zeroext)', result)
        self.assertNotIn('define ', result)
        with self.assertRaises(RuntimeError):
            own_all(text.replace('ret i32 0', 'ret i32 1'), {'handler': fingerprint})


if __name__ == '__main__':
    unittest.main()
