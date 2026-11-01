#!/usr/bin/env python3
"""Regenerate the small, synthetic package used by Android JNI instrumentation."""
import io
from pathlib import Path
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.jollymaam.test_firmware_package import PackageTests


def main():
    output = ROOT / 'android/app/src/androidTest/assets/firmware-fixture.zip'
    # Keep ZIP metadata deterministic; the shared fixture constructs the images
    # and hashes checked by both desktop package tests and the native bridge.
    with zipfile.ZipFile(io.BytesIO(PackageTests().manifest_fixture())) as source:
        with zipfile.ZipFile(output, 'w') as target:
            for entry in source.infolist():
                target.writestr(zipfile.ZipInfo(entry.filename), source.read(entry))
    print(output)


if __name__ == '__main__':
    main()
