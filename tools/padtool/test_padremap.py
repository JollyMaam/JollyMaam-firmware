"""Remap text <-> the firmware's bytes (struct remap_rule in src/core/pad.h)."""
import re
import unittest
from pathlib import Path
import padremap as remap
import padtool

PAD_H = (Path(__file__).resolve().parents[2] / "src" / "core" / "pad.h").read_text()


def c_enum(first):
    body = re.search(r"enum \{ (" + first + r"[^}]*)\}", PAD_H).group(1)
    return [n.strip() for n in body.split(",") if n.strip() and not n.strip().endswith("_COUNT")]


class RemapTextTests(unittest.TestCase):
    def test_tables_match_the_firmware(self):
        self.assertEqual([t.removeprefix("RT_").lower() for t in c_enum("RT_NONE")],
                         ["none", "held", "held_for", "pressed", "released", "tapped", "double"])
        self.assertEqual(len(c_enum("RT_NONE")), len(remap.TRIGGERS))
        self.assertEqual([e.removeprefix("RE_").lower() for e in c_enum("RE_HOLD")], list(remap.EFFECTS))
        for name, bit in re.findall(r"PAD_(\w+) = 1 << (\d+)", PAD_H):
            if int(bit) >= remap.INPUTS:
                continue                 # the triggers' presses: not remap keys
            name = {"select": "minus", "start": "plus", "screenshot": "capture"}.get(name.lower(), name.lower())
            self.assertEqual(remap.key_bit(name), int(bit))
        for define, value in (("REMAP_INPUTS", remap.INPUTS), ("REMAP_RULES", remap.RULES),
                              ("REMAP_MAX_HZ", remap.MAX_HZ), ("REMAP_OFF", remap.OFF)):
            self.assertEqual(int(re.search(define + r"\s+(\w+)", PAD_H).group(1), 0), value)

    def test_map_round_trip(self):
        raw = remap.parse_map("west=north north=west south=east east=south capture=none l4=south r4=r4")
        self.assertEqual(len(raw), remap.INPUTS)
        self.assertEqual(raw[3], 4 + 1)
        self.assertEqual(raw[2], remap.OFF)
        self.assertEqual(raw[9], 0)                     # itself is the default
        self.assertEqual(raw[16], 0)
        self.assertEqual(remap.show_map(raw), "south=east east=south capture=none west=north north=west l4=south")
        self.assertEqual(remap.parse_map(remap.show_map(raw)), raw)
        base = remap.decode_map(raw)
        self.assertEqual(len(base), 21)
        self.assertEqual((base["west"], base["capture"], base["r4"]), ("north", None, "r4"))
        self.assertEqual(remap.encode_map(base), raw)
        self.assertEqual(remap.show_map(bytes(22)), "identity")
        self.assertEqual(remap.parse_map("identity"), bytes(22))
        for bad in ("west", "q=south", "south=q"):
            with self.assertRaises(remap.RemapError):
                remap.parse_map(bad)

    def test_rules_round_trip(self):
        text = ("r4 held hold l1+r1; south held turbo south@max; l4 pressed toggle_turbo south@15; "
                "r4 tapped:300 tap south; r4 held_for:300 hold l1; capture held nothing; "
                "m double_tapped:250 tap home; l3 pressed toggle_hold up; "
                "r4 held turbo_modifier south+east@10; l4 released toggle_turbo_modifier west@max")
        raw = remap.parse_rules(text)
        self.assertEqual(len(raw), remap.RULES * remap.RULE_SIZE)
        self.assertEqual(remap.show_rules(raw), text)
        self.assertEqual(raw[0:10], bytes([1, 9, 0, 0, 0xC0, 0, 0, 0, 0, 0]))
        self.assertEqual(raw[30:40], bytes([5, 9, 30, 3, 1, 0, 0, 0, 0, 0]))
        self.assertEqual(raw[80:90], bytes([1, 9, 0, 6, 3, 0, 0, 0, 10, 0]))
        with self.assertRaises(remap.RemapError):
            remap.parse_rule("r4 pressed turbo_modifier south")
        self.assertEqual(remap.show_rules(bytes(160)), "none")
        self.assertEqual(remap.parse_rules("none"), bytes(160))
        self.assertEqual(remap.parse_rule("select held hold start")["outputs"], ["plus"])

    def test_rules_sized_for_the_pad(self):
        text = "; ".join(["south held hold east"] * 8)
        self.assertEqual(len(remap.parse_rules(text, 8)), 8 * remap.RULE_SIZE)
        self.assertEqual(remap.show_rules(remap.parse_rules(text, 8)), text)
        with self.assertRaises(remap.RemapError):
            remap.parse_rules(text + "; east held hold south", 8)
        self.assertEqual(len(remap.parse_rules("none")), remap.RULES * remap.RULE_SIZE)

    def test_rules_the_firmware_would_refuse(self):
        for bad in ("south held tap east", "south pressed hold east", "south tapped:0 tap east", "south held turbo east@31",
                    "south sometimes hold east", "south held hold q", "south held", "south tapped:3000 tap east"):
            with self.subTest(bad), self.assertRaises(remap.RemapError):
                remap.parse_rules(bad)
        with self.assertRaises(remap.RemapError):
            remap.parse_rules("; ".join(["south held hold east"] * 17))

    def test_presets(self):
        self.assertEqual(remap.show_map(remap.parse_map("swap-faces")), "south=east east=south west=north north=west")
        base = remap.decode_map(remap.parse_map("capture=none l4=south south=up"))
        base = remap.apply_preset(base, "swap-faces")
        self.assertEqual(remap.show_map(remap.encode_map(base)), "south=east east=south capture=none west=north north=west l4=south")

    def test_preset_files(self):
        data = remap.export_preset("Kart", remap.parse_map("south=east east=south"), remap.parse_rules("r4 held turbo south@max"))
        self.assertEqual(data, {"format": 1, "name": "Kart", "map": "south=east east=south", "rules": "r4 held turbo south@max"})
        self.assertEqual(remap.import_preset(data),
                         ("Kart", remap.parse_map("south=east east=south"), remap.parse_rules("r4 held turbo south@max")))
        for bad in ([], {"format": 2}, {"format": 1, "rules": "south held tap east"}):
            with self.subTest(bad), self.assertRaises(remap.RemapError):
                remap.import_preset(bad)

    def test_screenshot_is_capture(self):
        self.assertEqual(remap.show_map(remap.parse_map("screenshot=none")), "capture=none")
        self.assertEqual(remap.show_rule(remap.parse_rule("screenshot pressed tap home")), "capture pressed tap home")

    def test_macro_rules(self):
        text = "l4 pressed play_macro 2; r4 held macro 1; l3 tapped:200 toggle_macro 8"
        raw = remap.parse_rules(text)
        self.assertEqual(remap.show_rules(raw), text)
        self.assertEqual(raw[4:9], b"\0\0\0\0\2")                  # outputs empty, the rate byte the number
        for bad in ("l4 pressed play_macro 9", "l4 held play_macro 1", "l4 pressed macro 1"):
            with self.assertRaises(remap.RemapError):
                remap.parse_rules(bad)

    def test_macros(self):
        text = "jump: +south 40 -south; dash: +right 100 +east 40 -east -right 2000; : +l1 -l1"
        raw = remap.parse_macros(text, 200)
        self.assertEqual(len(raw), 200)
        self.assertEqual(remap.show_macros(raw), text)
        self.assertEqual(raw[:10], bytes([9, 4]) + b"jump" + bytes([0, 0x20, 10, 0]))
        self.assertEqual(remap.show_macros(remap.parse_macros("a: 1 +up 3 -up", 200)), "a: +up 4 -up")   # 4 ms steps
        self.assertEqual(remap.show_macros(bytes(200)), "none")
        for bad in ("x +up", "x: +nokey", "x: up", "x" * 17 + ": +up", "x: " + "+up 40 -up " * 60):
            with self.assertRaises(remap.RemapError):
                remap.parse_macros(bad, 200)

    def test_name(self):
        self.assertEqual(remap.parse_name("Mario Kart", 24), b"Mario Kart".ljust(24, b"\0"))
        self.assertEqual(remap.show_name(remap.parse_name("Zelda: TotK ✓", 24)), "Zelda: TotK ✓")
        with self.assertRaises(remap.RemapError):
            remap.parse_name("x" * 25, 24)

    def test_targets(self):
        self.assertEqual(padtool.target_index("remap-east"), 11)
        self.assertEqual(padtool.kind_of(17), 3)
        self.assertEqual(padtool.kind_of(9), 2)
        self.assertEqual(padtool.targets([{"kind": 0}]), [0, 1, 2, 3, 4, 5, 6, 7, 9])   # no palette fields
        self.assertEqual(padtool.targets([{"kind": 1}, {"kind": 3}]), list(range(10)) + [18] + list(range(10, 18)))
        self.assertEqual(padtool.targets([{"kind": 1}, {"kind": 3}, {"kind": 4}])[-9:], [27] + list(range(19, 27)))
        self.assertEqual((padtool.target_index("macros-base"), padtool.kind_of(19)), (27, 4))
        rules = {"kind": 3, "name": "rules", "type": padtool.T_BYTES, "size": 160}
        self.assertEqual(padtool.show_value(rules, padtool.parse_value(rules, "south held hold east")), "south held hold east")


if __name__ == "__main__":
    unittest.main()
