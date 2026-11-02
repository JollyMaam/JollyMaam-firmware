#!/usr/bin/env python3
"""Read Nova power/GPIO state; requires the power-snapshot diagnostic build."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import struct
import padtool as pad


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', default='5-2.2')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    pad.DEVICE = args.device
    path = pad.need_device()
    identity = pad.cmd(path, 0, length=88)
    if identity[40:56].split(b'\0')[0] != b'AC695X_TRANS':
        parser.error('Selected device is not a Nova 2 Lite')
    if pad.cmd(path, 0x3e, 0xfffffffd, 4) != b'PWR1':
        parser.error('This firmware does not support the power snapshot')
    names = [f'P3_ANA_CON{i}' for i in range(10)] + ['P3_ANA_KEEP', 'P3_VLD_KEEP']
    values = b''.join(pad.cmd(path, 0x3e, arg, 4) for arg in (0xfffffff0, 0xfffffff1, 0xfffffff2))
    result = dict(timestamp=datetime.now(timezone.utc).isoformat(),
                  flash_id=identity[24:40].hex(),
                  build=identity[8:24].split(b'\0')[0].decode(),
                  p33=dict(zip(names, (f'{v:02x}' for v in values))))
    status = pad.cmd(path, 1, length=16)
    result['status'] = dict(state=status[0], usb=status[2],
                            battery_mv=struct.unpack_from('<H', status, 4)[0],
                            uptime_ms=struct.unpack_from('<I', status, 8)[0])
    result['mmio'] = {}
    for base, count in ((0x1e5000, 36), (0x1e5100, 16)):
        data = pad.cmd(path, 0x15, base, 4 * count)
        result['mmio'].update({f'{base + 4*i:08x}': f'{v:08x}'
                               for i, (v,) in enumerate(struct.iter_unpack('<I', data))})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Never overwrite a previously captured state.
    with args.output.open('x') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(json.dumps(result['p33'], indent=2))
    print(f'Saved {args.output}')


if __name__ == '__main__':
    main()
