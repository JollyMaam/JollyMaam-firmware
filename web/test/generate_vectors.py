#!/usr/bin/env python3
"""Regenerate python-vectors.json (remap and curve text and bytes) from Python padtool, for remap.test.mjs."""
import json
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tools' / 'padtool'))
import padremap
import padcurves

out = Path(__file__).resolve().parent
fixtures = {}
for kind, examples, parse, show in (
    ('maps', ['identity', 'swap-faces', 'r4=none l3=r3 r3=l3', 'capture=none start=home'], padremap.parse_map, padremap.show_map),
    ('rules', ['none', 'r4 held turbo_modifier south+east@15', 'r4 tapped:300 tap south; r4 held_for:300 hold l1; l4 pressed toggle_turbo south@15', 'south held turbo south@max', 'home double_tapped:2550 toggle_hold plus', 'l1 held_for:305 nothing', 'l4 pressed play_macro 2; r4 held_for:300 macro 1; l3 double_tapped:300 toggle_macro 8'], padremap.parse_rules, padremap.show_rules),
    ('macros', ['none', 'jump: +south 40 -south', 'dash: +right 100 +east 40 -east -right 2000; : +l1 6 -l1; Größe: 5000 +up 1 -up'], padremap.parse_macros, padremap.show_macros),
):
    fixtures[kind] = [{'input': text, 'hex': parse(text).hex(), 'text': show(parse(text))} for text in examples]
fixtures['curves'] = [{'name': name, 'hex': padcurves.preset(name).hex()} for name in padcurves.PRESET_NAMES]
(out / 'python-vectors.json').write_text(json.dumps(fixtures, indent=2) + '\n')
print('Wrote python-vectors.json')
