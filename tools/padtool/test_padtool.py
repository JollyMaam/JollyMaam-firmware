"""restore-settings goes on past a target the pad refuses and lists it."""
import json
import os
import tempfile
import unittest
from unittest.mock import patch

import padtool


def table():
    t = padtool.FieldTable()
    t.append({"kind": 1, "id": 1, "type": padtool.T_NUMBER, "size": 1, "max": 255, "dev": 0,
              "name": "steady_brightness", "help": "", "names": [], "defaults": [b"\x02"]})
    t.append({"kind": 3, "id": 1, "type": padtool.T_NUMBER, "size": 1, "max": 255, "dev": 0,
              "name": "rumble", "help": "", "names": [], "defaults": [b"\x03"]})
    return t


class Restore(unittest.TestCase):
    def run_restore(self, refuse):
        backup = {"targets": {
            "palette": {"steady_brightness": {"set": True, "value": "4"}},
            "remap-south": {"rumble": {"set": True, "value": "5"}},
            "remap-base": {"rumble": {"set": True, "value": "6"}},
        }}
        sent = []

        def try_send(path, items, journal=True):
            sent.append(items[0][0])
            return "ephemeral mode: only the remap in use can change" if items[0][0] in refuse else None
        with tempfile.TemporaryDirectory() as d:
            file = os.path.join(d, "backup.json")
            with open(file, "w") as f:
                json.dump(backup, f)
            with patch.object(padtool, "fields", return_value=table()), \
                 patch.object(padtool, "try_send", side_effect=try_send):
                try:
                    padtool.restore("path", file, False)
                    return sent, None
                except SystemExit as error:
                    return sent, str(error)

    def test_every_target_is_tried_and_the_refused_listed(self):
        a = padtool.target_index("remap-south")
        sent, error = self.run_restore({a})
        self.assertEqual(sent, [padtool.target_index(n) for n in ("palette", "remap-south", "remap-base")])
        self.assertIn("all but 1", error)
        self.assertIn("remap-south (ephemeral mode", error)
        self.assertNotIn("palette (", error)

    def test_all_accepted(self):
        sent, error = self.run_restore(set())
        self.assertEqual(len(sent), 3)
        self.assertIsNone(error)


class Targets(unittest.TestCase):
    def test_palette_and_remaps_only_where_described(self):
        slot, palette, cal, remap = ({"kind": k} for k in range(4))
        self.assertEqual(padtool.targets([slot, palette, cal]), list(range(10)))
        self.assertEqual(padtool.targets([slot, cal]), [0, 1, 2, 3, 4, 5, 6, 7, 9])   # one-colour lights
        self.assertEqual(padtool.targets([slot, cal, remap])[:10], [0, 1, 2, 3, 4, 5, 6, 7, 9, padtool.BASE_TARGET])


class Layout(unittest.TestCase):
    @staticmethod
    def entry(number, kind, label):
        return bytes([number, kind]) + label.encode().ljust(8, b"\0") + bytes([0, 0])

    def test_pages_and_names(self):
        table = [self.entry(0, 0, "B"), self.entry(1, 0, "A"), self.entry(32, 4, ""), self.entry(10, 6, "View")]
        asked = []

        def cmd(path, sub, arg=0, length=0, out=None):
            asked.append(arg)
            here = table[arg:arg + 2]                     # two a reply
            return bytes([1, 12, len(table), len(here)]) + b"".join(here)
        with patch.object(padtool, "cmd", side_effect=cmd):
            layout = padtool.read_layout("p")
        self.assertEqual(asked, [0, 2])
        self.assertEqual([(e["name"], e["kind"], e["label"]) for e in layout],
                         [("south", "face", "B"), ("east", "face", "A"), ("left_stick", "stick", ""), ("minus", "menu", "View")])

    def test_old_firmware(self):
        with patch.object(padtool, "cmd", return_value=b""):
            self.assertIsNone(padtool.read_layout("p"))


if __name__ == "__main__":
    unittest.main()


class OldSlotTest(unittest.TestCase):
    def test_transport_becomes_radio_and_usb_type(self):
        v = {"type": {"value": "switch2", "set": True}, "transport": {"value": "bluetooth+usb", "set": True},
             "grips": {"value": "off", "set": False}}
        n = padtool.old_slot(v)
        self.assertEqual(n["radio"]["value"], "bluetooth")
        self.assertEqual(n["radio_type"]["value"], "switch2")
        self.assertEqual(n["usb_type"]["value"], "switch2")
        self.assertNotIn("transport", n)
        self.assertEqual(padtool.old_slot({"type": v["type"], "transport": {"value": "dongle", "set": False}})
                         ["usb_type"]["value"], "none")
        self.assertEqual(padtool.old_slot({"transport": {"value": "usb", "set": True}})["radio"]["value"], "off")
        self.assertIs(padtool.old_slot(n), n)
