"""Remap text <-> the firmware's bytes (struct remap_rule in src/pad.h)."""
import re
import unittest
from pathlib import Path
import padremap as remap
import padtool

PAD_H = (Path(__file__).parent.parent / "src" / "pad.h").read_text()


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
            name = {"select": "minus", "start": "plus", "screenshot": "capture"}.get(name.lower(), name.lower())
            self.assertEqual(remap.key_bit(name), int(bit))
        for define, value in (("REMAP_INPUTS", remap.INPUTS), ("REMAP_RULES", remap.RULES),
                              ("REMAP_MAX_HZ", remap.MAX_HZ), ("REMAP_OFF", remap.OFF)):
            self.assertEqual(int(re.search(define + r"\s+(\w+)", PAD_H).group(1), 0), value)

    def test_map_round_trip(self):
        raw = remap.parse_map("x=y y=x a=b b=a capture=none l4=a r4=r4")
        self.assertEqual(len(raw), remap.INPUTS)
        self.assertEqual(raw[3], 4 + 1)
        self.assertEqual(raw[2], remap.OFF)
        self.assertEqual(raw[9], 0)                     # itself is the default
        self.assertEqual(raw[16], 0)
        self.assertEqual(remap.show_map(raw), "a=b b=a capture=none x=y y=x l4=a")
        self.assertEqual(remap.parse_map(remap.show_map(raw)), raw)
        base = remap.decode_map(raw)
        self.assertEqual(len(base), 19)
        self.assertEqual((base["x"], base["capture"], base["r4"]), ("y", None, "r4"))
        self.assertEqual(remap.encode_map(base), raw)
        self.assertEqual(remap.show_map(bytes(20)), "identity")
        self.assertEqual(remap.parse_map("identity"), bytes(20))
        for bad in ("x", "q=a", "a=q"):
            with self.assertRaises(remap.RemapError):
                remap.parse_map(bad)

    def test_rules_round_trip(self):
        text = ("r4 held hold l1+r1; a held turbo a@max; l4 pressed toggle_turbo a@15; "
                "r4 tapped:300 tap a; r4 held_for:300 hold l1; capture held nothing; "
                "m double_tapped:250 tap home; l3 pressed toggle_hold up; "
                "r4 held turbo_modifier a+b@10; l4 released toggle_turbo_modifier x@max")
        raw = remap.parse_rules(text)
        self.assertEqual(len(raw), remap.RULES * remap.RULE_SIZE)
        self.assertEqual(remap.show_rules(raw), text)
        self.assertEqual(raw[0:10], bytes([1, 9, 0, 0, 0xC0, 0, 0, 0, 0, 0]))
        self.assertEqual(raw[30:40], bytes([5, 9, 30, 3, 1, 0, 0, 0, 0, 0]))
        self.assertEqual(raw[80:90], bytes([1, 9, 0, 6, 3, 0, 0, 0, 10, 0]))
        with self.assertRaises(remap.RemapError):
            remap.parse_rule("r4 pressed turbo_modifier a")
        self.assertEqual(remap.show_rules(bytes(160)), "none")
        self.assertEqual(remap.parse_rules("none"), bytes(160))
        self.assertEqual(remap.parse_rule("select held hold start")["outputs"], ["plus"])

    def test_rules_the_firmware_would_refuse(self):
        for bad in ("a held tap b", "a pressed hold b", "a tapped:0 tap b", "a held turbo b@31",
                    "a sometimes hold b", "a held hold q", "a held", "a tapped:3000 tap b"):
            with self.subTest(bad), self.assertRaises(remap.RemapError):
                remap.parse_rules(bad)
        with self.assertRaises(remap.RemapError):
            remap.parse_rules("; ".join(["a held hold b"] * 17))

    def test_presets(self):
        self.assertEqual(remap.show_map(remap.parse_map("swap-ab-xy")), "a=b b=a x=y y=x")
        base = remap.decode_map(remap.parse_map("capture=none l4=a a=up"))
        base = remap.apply_preset(base, "swap-ab-xy")
        self.assertEqual(remap.show_map(remap.encode_map(base)), "a=b b=a capture=none x=y y=x l4=a")

    def test_preset_files(self):
        data = remap.export_preset("Kart", remap.parse_map("a=b b=a"), remap.parse_rules("r4 held turbo a@max"))
        self.assertEqual(data, {"format": 1, "name": "Kart", "map": "a=b b=a", "rules": "r4 held turbo a@max"})
        self.assertEqual(remap.import_preset(data),
                         ("Kart", remap.parse_map("a=b b=a"), remap.parse_rules("r4 held turbo a@max")))
        for bad in ([], {"format": 2}, {"format": 1, "rules": "a held tap b"}):
            with self.subTest(bad), self.assertRaises(remap.RemapError):
                remap.import_preset(bad)

    def test_screenshot_is_capture(self):
        self.assertEqual(remap.show_map(remap.parse_map("screenshot=none")), "capture=none")
        self.assertEqual(remap.show_rule(remap.parse_rule("screenshot pressed tap home")), "capture pressed tap home")

    def test_name(self):
        self.assertEqual(remap.parse_name("Mario Kart", 24), b"Mario Kart".ljust(24, b"\0"))
        self.assertEqual(remap.show_name(remap.parse_name("Zelda: TotK ✓", 24)), "Zelda: TotK ✓")
        with self.assertRaises(remap.RemapError):
            remap.parse_name("x" * 25, 24)

    def test_targets(self):
        self.assertEqual(padtool.target_index("remap-B"), 11)
        self.assertEqual(padtool.kind_of(17), 3)
        self.assertEqual(padtool.kind_of(9), 2)
        old = [{"kind": 0}, {"kind": 2}]
        self.assertEqual(padtool.targets(old), list(range(10)))
        self.assertEqual(padtool.targets(old + [{"kind": 3}]), list(range(18)))
        rules = {"kind": 3, "name": "rules", "type": padtool.T_BYTES, "size": 160}
        self.assertEqual(padtool.show_value(rules, padtool.parse_value(rules, "a held hold b")), "a held hold b")


if __name__ == "__main__":
    unittest.main()
