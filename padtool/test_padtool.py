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
            "remap-a": {"rumble": {"set": True, "value": "5"}},
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
        a = padtool.target_index("remap-a")
        sent, error = self.run_restore({a})
        self.assertEqual(sent, [padtool.target_index(n) for n in ("palette", "remap-a", "remap-base")])
        self.assertIn("all but 1", error)
        self.assertIn("remap-a (ephemeral mode", error)
        self.assertNotIn("palette (", error)

    def test_all_accepted(self):
        sent, error = self.run_restore(set())
        self.assertEqual(len(sent), 3)
        self.assertIsNone(error)


if __name__ == "__main__":
    unittest.main()
