#!/usr/bin/env python3
"""Regenerate the small, synthetic package used by Android JNI instrumentation."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.jollymaam.test_jollymaam_jni import package


def main():
    output = ROOT / 'android/app/src/androidTest/assets/firmware-fixture.zip'
    output.write_bytes(package())
    print(output)


if __name__ == '__main__':
    main()
