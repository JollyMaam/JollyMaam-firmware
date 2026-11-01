"""Shared preset behaviour for CLI and GUI."""
import unittest
from unittest.mock import patch
import padcurves as curves
import padtool


class CurveTests(unittest.TestCase):
    def test_presets_are_fifteen_monotone_points(self):
        for name in curves.PRESET_NAMES:
            points = curves.preset(name)
            self.assertEqual(len(points), 15)
            self.assertEqual(curves.validate(points), points)
            self.assertEqual(curves.identify(points), name)
        self.assertEqual(curves.preset('linear')[7], 128)
        self.assertEqual(curves.preset('concave')[7], 64)

    def test_cli_preset_and_disable_share_point_definitions(self):
        table = [{'kind': 0, 'id': 36, 'name': 'left_curve_enabled', 'type': padtool.T_ENUM,
                  'names': ['disabled', 'enabled'], 'size': 1, 'dev': False},
                 {'kind': 0, 'id': 38, 'name': 'left_curve_points', 'type': padtool.T_BYTES, 'size': 15, 'dev': False}]
        with patch.object(padtool, 'fields', return_value=table), patch.object(padtool, 'send') as send, \
             patch.object(padtool, 'ephemeral_now', return_value=False):
            padtool.set_values('fake', 'A', ['left_curve', 'concave'])
            send.assert_called_with('fake', [(0, 38, curves.preset('concave')), (0, 36, b'\1')])
            padtool.set_values('fake', 'A', ['left_curve', 'disabled'])
            send.assert_called_with('fake', [(0, 36, b'\0')])
        self.assertEqual(padtool.parse_value(table[1], 's-curve'), curves.preset('s-curve'))


if __name__ == '__main__':
    unittest.main()
