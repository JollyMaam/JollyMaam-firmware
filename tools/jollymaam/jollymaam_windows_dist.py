#!/usr/bin/env python3
"""Package a cross-built Windows updater and its firmware ZIP for distribution."""
import argparse
from pathlib import Path
import zipfile

README = '''JollyMaam firmware updater for 64-bit Windows

Extract all files into one directory, then open Update firmware.cmd.
The terminal app lists supported devices and asks for yes before updating.
You can also run: jollymaam.exe update firmware.zip
Select a specific endpoint/product with:
  jollymaam.exe update --device PATH --product PRODUCT firmware.zip
Add --yes to skip confirmation when both selections are supplied.
Use jollymaam.exe enter-loader --yes HID_PATH to enter the original bootloader
from our running firmware or updater.

Supported discovery: our firmware's tool devices and supported stock bootloaders.
Ordinary stock gamepad modes are not discovered yet. Use r to rescan.
If Jieli raw-device access is denied, run the launcher from an Administrator
terminal. No additional hidapi, libusb or compiler DLLs are needed.

Jieli devices remain in their loader after verification. The 8BitDo and custom
updater request firmware restart. Interrupted updates can require loader recovery.
This build passed offline tests under Wine; real Windows USB behavior is not yet
hardware-validated.
'''
LAUNCHER = '@echo off\r\ncd /d "%~dp0"\r\njollymaam.exe update firmware.zip\r\npause\r\n'


def build(executable, firmware, output):
    if output.resolve() in (executable.resolve(), firmware.resolve()):
        raise ValueError('output must differ from input files')
    files = {'jollymaam.exe': executable.read_bytes(), 'firmware.zip': firmware.read_bytes(),
             'README.txt': README.replace('\n', '\r\n').encode(),
             'Update firmware.cmd': LAUNCHER.encode()}
    if files['jollymaam.exe'][:2] != b'MZ':
        raise ValueError('not a Windows executable')
    with zipfile.ZipFile(firmware) as archive:
        if archive.testzip() or 'manifest.json' not in archive.namelist():
            raise ValueError('invalid firmware ZIP')
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 0
            archive.writestr(info, data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--firmware', required=True, type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    build(args.exe, args.firmware, args.output)


if __name__ == '__main__':
    main()
