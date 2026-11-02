#!/usr/bin/env python3
"""Manual Nova LED timing experiments; requires a NOVA_LED_PROBE build.

DEV_ROOT=/dev/bwrap /work/bin/python3 tools/padtool/nova_led_probe.py --device 5-2.2
Nothing advances automatically. Results append to a JSONL file. No settings
are written; quitting clears the diagnostic override and sends a dark frame.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import struct

import padtool as pad

# (zero high, one high, total) counts of consecutive GPIO writes.
WAVES = [(2, 7, 12), (2, 7, 12), (2, 6, 10), (2, 8, 14),
         (3, 8, 14), (3, 9, 16), (2, 9, 16), (3, 10, 18),
         (4, 11, 20), (4, 12, 22), (2, 10, 18)]
COLOURS = {'w': 0, 'r': 1, 'g': 2, 'b': 3, 'd': 4}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', default='5-2.2')
    parser.add_argument('--output', type=Path, default=Path('build/nova-led-probe/results.jsonl'))
    parser.add_argument('--list', action='store_true', help='list timings without opening a device')
    args = parser.parse_args()
    if args.list:
        for i, (zero, one, total) in enumerate(WAVES):
            print(f'{i}: zero={zero}/{total}, one={one}/{total}'
                  + (' original C driver' if i == 0 else ' uninterrupted assembly pulses'))
        return
    pad.DEVICE = args.device
    path = pad.need_device()
    identity = pad.cmd(path, 0, length=88)
    product = identity[40:56].split(b'\0')[0]
    if product != b'AC695X_TRANS':
        parser.error('Selected device is not the Nova 2 Lite pad')
    if pad.cmd(path, 0x3e, 0xffffffff, 4) != b'NLP1':
        parser.error('Install the NOVA_LED_PROBE diagnostic build first')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    wave, colour = 0, 'w'

    def send():
        answer = pad.cmd(path, 0x3e, wave | COLOURS[colour] << 8, 4)
        if answer != struct.pack('<I', 1):
            raise OSError('Diagnostic command was rejected')
        z, o, total = WAVES[wave]
        print(f'Timing {wave}: zero {z} high/{total-z} low; '
              f'one {o} high/{total-o} low; colour {colour}')

    print('Watch the Home light. Each choice stays until you change it.')
    print('w/r/g/b/d: white/red/green/blue/dark; note TEXT: record what you see')
    print('Enter or n: next timing (wraps around), p: previous, q: stop. Notes are optional.')
    try:
        with args.output.open('a', buffering=1) as log:
            send()
            while True:
                command = input('LED> ').strip()
                if command == 'q':
                    break
                if command in COLOURS:
                    colour = command
                    send()
                elif command.startswith('note ') and command[5:].strip():
                    log.write(json.dumps(dict(timestamp=datetime.now(timezone.utc).isoformat(),
                                              flash_id=identity[24:40].hex(),
                                              build=identity[8:24].split(b'\0')[0].decode(),
                                              timing=wave, counts=WAVES[wave], colour=colour,
                                              observation=command[5:].strip())) + '\n')
                    print('Recorded. Timing is unchanged.')
                elif command in ('', 'n', 'p'):
                    wave = (wave + (-1 if command == 'p' else 1)) % len(WAVES)
                    send()
                elif command:
                    print('Enter: next; w/r/g/b/d: colour; note TEXT; p: previous; q: stop.')
    except (EOFError, KeyboardInterrupt):
        print('\nStopping.')
    finally:
        pad.cmd(path, 0x3e, 255, 4)
        print(f'Diagnostic stopped. Results: {args.output}')


if __name__ == '__main__':
    main()
