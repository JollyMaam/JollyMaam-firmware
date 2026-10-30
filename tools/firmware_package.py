#!/usr/bin/env python3
"""Build a reproducible, stored-entry ZIP of every supported firmware target."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

PRODUCTS = {
    'gamesir-cyclone-2-pad': ('GS_C2_ADC_DEVICE', 'BR23'),
    'gamesir-cyclone-2-dongle': ('GS_C2_Dongle', 'BR23'),
    'gamesir-nova-2-lite-pad': ('AC695X_TRANS', 'BR23'),
    'gamesir-nova-2-lite-dongle': ('AC632N_TRANS', 'BD19'),
    '8bitdo-ultimate2c-wireless-pad': ('U2C_WIRELESS', 'TLSR8208'),
}


def build(build_dir, sdk, output):
    files = {}
    manifest = {'format': 'jollymaam-firmware', 'version': 1, 'targets': []}
    for board, (product, chip) in PRODUCTS.items():
        target = {'board': board, 'product': product, 'chip': chip, 'artifacts': [],
                  'preserve': [{'offset': 0x64000, 'size': 0x1c000}]
                  if board == 'gamesir-nova-2-lite-pad' else []}
        for role, name in [('install', 'firmware.img'), ('update', 'update.bin')]:
            path = f'firmware/{board}/{name}'
            data = (build_dir / board / name).read_bytes()
            files[path] = data
            target['artifacts'].append({'role': role, 'path': path, 'size': len(data),
                                        'sha256': hashlib.sha256(data).hexdigest()})
        if chip in ('BR23', 'BD19'):
            path = f'loaders/{chip.lower()}loader.bin'
            files[path] = (sdk / 'cpu' / chip.lower() / 'tools' / Path(path).name).read_bytes()
            target['loader'] = {'path': path, 'size': len(files[path]),
                                'sha256': hashlib.sha256(files[path]).hexdigest()}
        else:
            target['model'] = 0x301b
        manifest['targets'].append(target)
    files['manifest.json'] = (json.dumps(manifest, sort_keys=True, indent=2) + '\n').encode()
    # Read every input before opening output; a missing artifact cannot truncate it.
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_STORED, allowZip64=False) as archive:
        for path, data in sorted(files.items()):
            info = zipfile.ZipInfo(path, date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data)


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=root / 'build')
    parser.add_argument('--sdk', type=Path, default=root / 'external/jieli-fw-AC63_BT_SDK')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    build(args.build_dir, args.sdk, args.output)


if __name__ == '__main__':
    main()
