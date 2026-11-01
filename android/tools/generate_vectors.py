#!/usr/bin/env python3
"""Regenerate checked-in Android interoperability fixtures from Python padtool."""
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools' / 'padtool'))
import padremap
import padcurves
import padflash
from test_padflash import image

out = root / 'android/app/src/test/resources'
fixtures = {}
for kind, examples, parse, show in (
    ('maps', ['identity', 'swap-ab-xy', 'r4=none l3=r3 r3=l3', 'capture=none start=home'], padremap.parse_map, padremap.show_map),
    ('rules', ['none', 'r4 held turbo_modifier a+b@15', 'r4 tapped:300 tap a; r4 held_for:300 hold l1; l4 pressed toggle_turbo a@15', 'a held turbo a@max', 'home double_tapped:2550 toggle_hold plus', 'l1 held_for:305 nothing'], padremap.parse_rules, padremap.show_rules),
):
    fixtures[kind] = [{'input': text, 'hex': parse(text).hex(), 'text': show(parse(text))} for text in examples]
fixtures['curves'] = [{'name': name, 'hex': padcurves.preset(name).hex()} for name in padcurves.PRESET_NAMES]
(out / 'python-vectors.json').write_text(json.dumps(fixtures, indent=2) + '\n')
original = image(resource=b'Android/Python interoperability')
(out / 'image.bin').write_bytes(original)
(out / 'image-rekey.bin').write_bytes(padflash.rekey_image(original, 0x1234))
(out / 'image-dongle.bin').write_bytes(image(pid='GS_C2_Dongle'))
print('Wrote Python padtool interoperability fixtures')
