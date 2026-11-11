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
    ('rules', ['none', 'r4 held turbo_modifier south+east@15', 'r4 tapped:300 tap south; r4 held_for:300 hold l1; l4 pressed toggle_turbo south@15', 'south held turbo south@max', 'home double_tapped:2550 toggle_hold plus', 'l1 held_for:305 nothing'], padremap.parse_rules, padremap.show_rules),
):
    fixtures[kind] = [{'input': text, 'hex': parse(text).hex(), 'text': show(parse(text))} for text in examples]
fixtures['curves'] = [{'name': name, 'hex': padcurves.preset(name).hex()} for name in padcurves.PRESET_NAMES]
(out / 'python-vectors.json').write_text(json.dumps(fixtures, indent=2) + '\n')
print('Wrote python-vectors.json')
