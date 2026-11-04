"""Check the build-time schema contract and its invalidation inputs."""
import hashlib
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SettingsSchemaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.out = Path(cls.temp.name)
        cls.cc = shlex.split(os.environ.get("HOST_CC", "cc"))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def export(self, name, defaults, flags=(), failure=None):
        folder = self.out / name
        folder.mkdir()
        (folder / "board.inc").write_text(defaults)
        exe = folder / "export"
        subprocess.run(self.cc + [
            "-O2", "-std=c99", "-ffunction-sections", "-fdata-sections",
            "-iquote", str(folder), "-iquote", str(ROOT / "src/core"),
            *flags, str(ROOT / "tools/jollymaam/settings_schema.c"),
            str(ROOT / "src/lib/sha256.c"), "-Wl,--gc-sections", "-o", str(exe),
        ], check=True, capture_output=True)
        schema, header = folder / "schema.bin", folder / "schema.h"
        result = subprocess.run([str(exe), str(schema), str(header)], capture_output=True, text=True)
        if failure:
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(failure, result.stderr)
            self.assertFalse(schema.exists())
            self.assertFalse(header.exists())
            return
        self.assertEqual(result.returncode, 0, result.stderr)
        data = schema.read_bytes()
        info = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", header.read_text()))
        self.assertEqual(data[:8], b"JMSC\x01\x00\x02\x00")
        self.assertEqual(info[:2], b"\x01\x01")
        self.assertEqual(info[2:4], data[8:10])
        self.assertEqual(info[4:], hashlib.sha256(data).digest()[:16])
        fields = struct.unpack_from("<H", data, 8)[0]
        at = 10
        for _ in range(fields):
            size = struct.unpack_from("<H", data, at)[0]
            self.assertGreaterEqual(size, 8)
            self.assertEqual(data[at + 2], 1)  # FIELD version
            at += 2 + size
        self.assertEqual(struct.unpack_from("<H", data, at)[0], 19)
        at += 2
        for target in range(19):
            self.assertEqual(data[at], target)
            self.assertIn(data[at + 1], (0, 1))
            count = data[at + 2]
            at += 3
            for _ in range(count):
                at += 2 + data[at + 1]
        self.assertEqual(at, len(data))
        return data, info, at

    def test_stability_and_board_defaults(self):
        defaults = (ROOT / "src/boards/gamesir-cyclone-2/board.inc").read_text()
        first = self.export("first", defaults)
        self.assertEqual(first, self.export("repeat", defaults))
        # Whitespace and unrelated build options must not invalidate the cache.
        self.assertEqual(first, self.export("unrelated", "\n" + defaults, ["-DLOG_LINES=9"]))
        changed = defaults.replace(".steady = 10", ".steady = 11")
        self.assertNotEqual(first[1], self.export("brightness", changed)[1])
        # A long list default is not included in FIELD's short defaults.
        changed = defaults.replace(".kick_ms = { 0, 0, 0, 25, 30 }", ".kick_ms = { 1, 0, 0, 25, 30 }")
        self.assertNotEqual(first[1], self.export("long-default", changed)[1])
        # Both capabilities and writable-target changes invalidate the schema.
        self.assertNotEqual(first[1], self.export("no-audio", defaults, ["-DSLOT_USB_AUDIO=0"])[1])
        self.assertNotEqual(first[1], self.export("fewer-slots", defaults, ["-DSLOTS_STORED=4"])[1])

    def test_storage_capacity(self):
        defaults = (ROOT / "src/boards/8bitdo-ultimate2c-wireless/board.inc").read_text()
        self.export("capacity-ok", defaults, ["-DSETTINGS_STORE_TELINK", "-DSLOTS_STORED=4",
                    "-DREMAPS_STORED=4", "-DREMAP_RULES=8", "-DRECORD_MAX=152", "-DSLOT_USB_AUDIO=0"])
        self.export("record-overflow", defaults, ["-DRECORD_MAX=8"], failure="record limit")
        self.export("sector-overflow", defaults, ["-DSETTINGS_STORE_TELINK", "-DREMAP_RULES=8"],
                    failure="compaction sector")

    def test_jieli_storage_capacity(self):
        defaults = (ROOT / "src/boards/gamesir-cyclone-2/board.inc").read_text()
        self.export("jieli-capacity-ok", defaults, ["-DSETTINGS_STORE_JIELI_BYTES=16384",
                    "-DSETTINGS_STORE_JIELI_RESERVE=1024"])
        self.export("jieli-area-overflow", defaults, ["-DSETTINGS_STORE_JIELI_BYTES=8192",
                    "-DSETTINGS_STORE_JIELI_RESERVE=1024"], failure="Jieli VM compaction area")
        self.export("jieli-sdk-overflow", defaults, ["-DSETTINGS_STORE_JIELI_BYTES=16384",
                    "-DSETTINGS_STORE_JIELI_RESERVE=4096"], failure="Jieli VM compaction area")

    def test_layout_checked(self):
        defaults = (ROOT / "src/boards/gamesir-cyclone-2/board.inc").read_text()
        twice = defaults.replace('{ IN_EAST,  ', '{ IN_SOUTH, ', 1)
        self.assertNotEqual(twice, defaults)
        self.export("layout-twice", twice, failure="there twice")
        self.export("layout-bit-15", defaults.replace("{ IN_M, ", "{ 15, ", 1), failure="not an input")

    def test_boards(self):
        infos = []
        for board, flags in [
            ("gamesir-cyclone-2", []),
            ("gamesir-nova-2-lite", []),
            ("8bitdo-ultimate2c-wireless", ["-DSLOTS_STORED=4", "-DSLOT_USB_AUDIO=0", "-DREMAP_RULES=8"]),
        ]:
            defaults = (ROOT / "src/boards" / board / "board.inc").read_text()
            infos.append(self.export(board, defaults, flags)[1])
        self.assertEqual(len(set(infos)), 3)


if __name__ == "__main__":
    unittest.main()
