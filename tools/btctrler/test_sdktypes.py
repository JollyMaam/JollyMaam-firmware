"""A local enum's value must survive recovery beside a conflicting enum."""
from pathlib import Path
import tempfile
import unittest

from tools.btctrler.sdktypes import generate


class RecoveredTypes(unittest.TestCase):
    def test_colliding_local_enumerators_retain_both_values(self):
        metadata = '''!0 = !DIFile(filename: "first.c", directory: "/sdk")
!1 = !DIFile(filename: "second.c", directory: "/sdk")
!2 = !DICompositeType(tag: DW_TAG_enumeration_type, name: "first_state", file: !0, line: 1, size: 32, elements: !3)
!3 = !{!4}
!4 = !DIEnumerator(name: "SEND_WAIT", value: 0)
!5 = !DICompositeType(tag: DW_TAG_enumeration_type, name: "second_state", file: !1, line: 1, size: 32, elements: !6)
!6 = !{!7}
!7 = !DIEnumerator(name: "SEND_WAIT", value: 1)
'''
        with tempfile.TemporaryDirectory() as tmp:
            source, header = Path(tmp) / 'enums.ll', Path(tmp) / 'sdk.h'
            source.write_text(metadata)
            generate(header, [source], {}, 'q32s')
            text = header.read_text()
            self.assertIn('enum first_state { SEND_WAIT = 0 }', text)
            self.assertIn('enum second_state { second_state_SEND_WAIT = 1 }', text)


if __name__ == '__main__':
    unittest.main()
