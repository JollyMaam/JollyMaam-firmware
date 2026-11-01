#!/usr/bin/env python3
"""Check release linkage without opening devices or executing foreign binaries."""
import argparse
import re
import subprocess


def check(binary, platform, readelf='readelf', objdump='objdump'):
    def output(*args):
        return subprocess.run(args, check=True, capture_output=True, text=True).stdout
    if platform == 'linux':
        if re.search(r'\bINTERP\b', output(readelf, '-l', str(binary))) or re.search(r'\bNEEDED\b', output(readelf, '-d', str(binary))):
            raise ValueError('Linux executable must be fully static')
    else:
        imports = re.findall(r'DLL Name:\s*(\S+)', output(objdump, '-p', str(binary)))
        allowed = {'hid.dll', 'kernel32.dll', 'msvcrt.dll', 'setupapi.dll', 'cfgmgr32.dll'}
        unexpected = {name.lower() for name in imports} - allowed
        if not imports or unexpected:
            raise ValueError(f'Unexpected Windows runtime DLLs: {sorted(unexpected)}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['linux', 'windows'], required=True)
    parser.add_argument('--readelf', default='readelf')
    parser.add_argument('--objdump', default='objdump')
    parser.add_argument('binary')
    args = parser.parse_args()
    check(args.binary, args.platform, args.readelf, args.objdump)


if __name__ == '__main__':
    main()
