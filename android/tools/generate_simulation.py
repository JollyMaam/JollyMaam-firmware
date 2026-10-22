#!/usr/bin/env python3
"""Export the firmware's real field table/defaults using its host-test stubs."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='padtool-simulation-') as temp:
    binary = str(Path(temp) / 'export')
    subprocess.run(shlex.split(os.environ.get('HOST_CC', 'gcc')) + [
        '-std=c11', '-O2', '-Wno-unused-function',
        str(root / 'android/tools/export_simulation.c'), '-o', binary,
    ], check=True)
    raw = subprocess.check_output([binary])
    data = json.loads(raw)
    assert data['format'] == 1 and len(data['values']) == len(data['defaults']) == 19
    target = root / 'android/app/src/debug/assets/simulated-pad.json'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(raw)
    print(f'Exported {len(data["fields"])} fields and 19 targets to {target}')
