#!/usr/bin/env python3
"""Log a pad's battery voltage and USB power with UTC timestamps.

    DEV_ROOT=/dev/bwrap python3 tools/padtool/battery.py --output build/battery.csv

The default product is the Ultimate 2C Wireless (U2C_WIRELESS). Only IDENTIFY
and STATUS are sent. Reconnects follow the same flash ID; disconnects are
logged and retried. Voltage is a measurement, not a capacity percentage or a
charging-complete indication. Ctrl-C stops recording.
"""
import argparse
import csv
from datetime import datetime, timezone
from pathlib import Path
import struct
import sys
import time

import padtool as pad
from padtransport import tool_paths

DETAIL_FIELDS = ('battery_percent', 'charging', 'battery_low')
FIELDS = ('timestamp_utc', 'elapsed_s', 'battery_mv', 'usb_powered', 'uptime_s', 'event', 'device')


class Monitor:
    def __init__(self, product, device=None, details=False):
        self.details = details
        self.product = product
        self.requested = device
        self.path = None
        self.uid = None

    def connect(self):
        """Select one running pad, retaining its identity across USB renumbering."""
        found = []
        for path, _, identity in pad.discover_devices(paths=tool_paths()):
            product = identity[40:56].split(b'\0')[0].decode(errors='replace')
            uid = identity[24:40].hex()
            if identity[6] != pad.BOARD_IDS['pad'] or identity[7] == 1 or product != self.product:
                continue
            if self.uid is not None:
                if uid != self.uid:
                    continue
            elif self.requested and self.requested not in (path, uid):
                continue
            found.append((path, uid))
        if len(found) > 1:
            raise ValueError('Multiple matching pads; select one with --device PATH or FLASH_ID')
        if found:
            self.path, self.uid = found[0]
        return self.path is not None

    def sample(self):
        """Read STATUS or record a missing connection; do not change pad state."""
        if self.path is None and not self.connect():
            return dict(event='disconnected', device='')
        path = self.path
        try:
            data = pad.cmd(path, 0x01, length=16)
            if len(data) < 16:
                raise OSError('Short STATUS response')
            reading = dict(battery_mv=struct.unpack_from('<H', data, 4)[0],
                        usb_powered=data[2], uptime_s=f'{struct.unpack_from("<I", data, 8)[0] / 1000:.3f}',
                        event='sample', device=path)
            if self.details:
                reading.update(pad.battery_status(data))
            return reading
        except OSError as error:
            self.path = None
            return dict(event=f'disconnected: {error}', device=path)


def positive_interval(value):
    """Reject invalid sampling periods before opening a device or output file."""
    import math
    interval = float(value)
    if not math.isfinite(interval) or interval <= 0:
        raise argparse.ArgumentTypeError('interval must be a positive number of seconds')
    return interval


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--product', default='U2C_WIRELESS')
    parser.add_argument('--device', help='USB tool path or full flash ID (otherwise select the only matching pad)')
    parser.add_argument('--interval', type=positive_interval, default=30, metavar='SECONDS')
    parser.add_argument('--details', action='store_true', help='include estimated percent, charging state and low-battery status')
    parser.add_argument('--output', type=Path, help='also append CSV readings to this file')
    parser.add_argument('--samples', type=int, help='stop after this many rows, including disconnects')
    args = parser.parse_args()
    if args.samples is not None and args.samples <= 0:
        parser.error('--samples must be positive')
    monitor = Monitor(args.product, args.device, args.details)
    fields = FIELDS + DETAIL_FIELDS if args.details else FIELDS
    output = None
    try:
        writers = [csv.DictWriter(sys.stdout, fieldnames=fields)]
        writers[0].writeheader()
        if args.output:
            if args.output.exists() and args.output.stat().st_size:
                with args.output.open(newline='') as previous:
                    if next(csv.reader(previous), None) != list(fields):
                        parser.error('--output CSV columns differ; use a new file or the matching --details setting')
            output = args.output.open('a', newline='')
            writer = csv.DictWriter(output, fieldnames=fields)
            if output.tell() == 0:
                writer.writeheader()
            writers.append(writer)
        start = time.monotonic()
        count = 0
        while args.samples is None or count < args.samples:
            row = monitor.sample()
            row.update(timestamp_utc=datetime.now(timezone.utc).isoformat(timespec='seconds'),
                       elapsed_s=f'{time.monotonic() - start:.1f}')
            for writer in writers:
                writer.writerow(row)
            sys.stdout.flush()
            if output:
                output.flush()
            count += 1
            if args.samples is None or count < args.samples:
                time.sleep(args.interval)
    except KeyboardInterrupt:
        pass
    except (OSError, ValueError) as error:
        parser.exit(1, f'battery: {error}\n')
    finally:
        if output:
            output.close()


if __name__ == '__main__':
    main()
