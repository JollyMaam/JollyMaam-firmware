#!/usr/bin/env python3
"""Package desktop releases or an installed SDK using reproducible ZIP entries."""
import argparse
from pathlib import Path
import tempfile
import zipfile

from tools.jollymaam_windows_dist import build as build_windows

LINUX_README = '''JollyMaam firmware updater for Linux

Extract the ZIP, then run ./jollymaam update firmware.zip in a terminal.
The app lists supported devices and asks for confirmation before updating.
Run ./jollymaam help for commands and device/product selection options.
Device access requires suitable HID and raw SCSI/block-device permissions.
The executable is fully static; no hidapi or libusb installation is needed.
'''


def write_zip(output, files):
    """Replace the output only after writing every entry, preserving Unix modes."""
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output.parent, delete=False) as temp:
        temporary = Path(temp.name)
    try:
        with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, (data, mode) in sorted(files.items()):
                info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.external_attr = (0o100000 | mode) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, data)
        temporary.chmod(0o644)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)


def build_desktop(executable, firmware, output, platform):
    """Bundle an updater with a validated firmware ZIP, never overwriting inputs."""
    if output.resolve() in (executable.resolve(), firmware.resolve()):
        raise ValueError('output must differ from input files')
    output.parent.mkdir(parents=True, exist_ok=True)
    if platform == 'windows':
        build_windows(executable, firmware, output)
        return
    binary = executable.read_bytes()
    if binary[:4] != b'\x7fELF':
        raise ValueError('not a Linux executable')
    with zipfile.ZipFile(firmware) as archive:
        if archive.testzip() or 'manifest.json' not in archive.namelist():
            raise ValueError('invalid firmware ZIP')
    write_zip(output, {'jollymaam': (binary, 0o755),
                       'firmware.zip': (firmware.read_bytes(), 0o644),
                       'README.txt': (LINUX_README.encode(), 0o644)})


def build_sdk(root, output):
    """Archive a Make-installed SDK with paths relative to its installation root."""
    paths = sorted(path for path in root.rglob('*') if path.is_file())
    if not paths:
        raise ValueError('empty SDK installation')
    if output.resolve().is_relative_to(root.resolve()):
        raise ValueError('output must be outside the SDK installation')
    write_zip(output, {path.relative_to(root).as_posix():
                      (path.read_bytes(), 0o755 if path.stat().st_mode & 0o111 else 0o644)
                      for path in paths})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    desktop = sub.add_parser('desktop')
    desktop.add_argument('--platform', choices=['linux', 'windows'], required=True)
    desktop.add_argument('--exe', type=Path, required=True)
    desktop.add_argument('--firmware', type=Path, required=True)
    desktop.add_argument('output', type=Path)
    sdk = sub.add_parser('sdk')
    sdk.add_argument('--root', type=Path, required=True)
    sdk.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.command == 'desktop':
        build_desktop(args.exe, args.firmware, args.output, args.platform)
    else:
        build_sdk(args.root, args.output)


if __name__ == '__main__':
    main()
