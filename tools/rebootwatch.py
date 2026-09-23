#!/usr/bin/env python3
"""Watch the pad for chip resets and report what the reset source says.

A reset shows up three ways, and this watches all of them:

  * the USB CDC console goes away and comes back - the node disappears, or
    reads and writes fail with EIO, and it often re-enumerates as a different
    /dev/bwrap/ttyjlN;
  * `status`'s preserved boot counter (`boots=`) increments;
  * `status`'s `uptime=` starts over.

The firmware latches the PMU reset-source registers at the top of app_main and
prints them in `status` as `p33=` and `rtc=`, so every reboot is reported with
them and with how long the previous boot survived.  Those registers are the
only available clue to *why* the chip reset; `reset-source=` is the SDK's own
decoder, which reads 0 on this part.

Liveness is polled with a short `connect status` (a few dozen bytes) and the
full `status` is read only every few seconds, because its bring-up ring is
several kilobytes.

Usage:
    tools/rebootwatch.py [--seconds N] [--poll SECONDS] [--port DEV]

Exit status: 0 = ran to completion, 2 = no console found.
"""

import argparse
import errno
import fcntl
import glob
import os
import re
import select
import struct
import sys
import termios
import time

BOOTS_RE = re.compile(r'boots=([0-9a-fA-F]{8})')
SOURCE_RE = re.compile(r'reset-source=(\d+) p33=(\d+) rtc=(\d+)')
UPTIME_RE = re.compile(r'uptime=(\d+) ms')


def candidates():
    paths = []
    for pattern in ('/dev/bwrap/ttyjl*', '/dev/bwrap/ttyACM*', '/dev/ttyACM*'):
        paths.extend(sorted(glob.glob(pattern)))
    return list(dict.fromkeys(paths))


class Console:
    """A console connection that can be reopened when the node re-enumerates."""

    def __init__(self, requested=None):
        self.requested = requested
        self.fd = None
        self.path = None

    @staticmethod
    def _configure(fd):
        attributes = termios.tcgetattr(fd)
        attributes[0] = 0
        attributes[1] = 0
        attributes[2] = termios.CLOCAL | termios.CREAD | termios.CS8
        attributes[3] = 0
        attributes[4] = termios.B115200
        attributes[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, attributes)
        for flag in (termios.TIOCMBIC, termios.TIOCMBIS):
            fcntl.ioctl(fd, flag,
                        struct.pack('I', termios.TIOCM_DTR | termios.TIOCM_RTS))
            time.sleep(0.2)

    def open(self):
        paths = [self.requested] if self.requested else candidates()
        for path in paths:
            try:
                fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            except OSError:
                continue
            try:
                self._configure(fd)
            except OSError:
                os.close(fd)
                continue
            self.fd = fd
            self.path = path
            self.drain(0.4)
            return True
        return False

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def drain(self, seconds, quiet=0.2):
        out = b''
        if self.fd is None:
            return ''
        deadline = time.time() + seconds
        last = time.time()
        while time.time() < deadline:
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if not ready:
                if out and (time.time() - last) >= quiet:
                    break
                continue
            try:
                chunk = os.read(self.fd, 8192)
            except OSError as exc:
                if exc.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                    self.close()
                break
            if chunk:
                out += chunk
                last = time.time()
        return out.decode('utf-8', 'replace')

    def ask(self, command, seconds=2.0):
        if self.fd is None:
            return ''
        try:
            os.write(self.fd, (command + '\r\n').encode())
        except OSError:
            self.close()
            return ''
        return self.drain(seconds)

    def alive(self):
        return 'slot=' in self.ask('connect status')

    def sample(self):
        """(boots, p33, rtc, uptime) from `status`, or None."""
        out = self.ask('status', 6.0)
        boots = BOOTS_RE.search(out)
        source = SOURCE_RE.search(out)
        uptime = UPTIME_RE.search(out)
        if not boots or not source:
            return None
        return (boots.group(1), int(source.group(2)), int(source.group(3)),
                int(uptime.group(1)) if uptime else -1)


def stamp():
    return time.strftime('%H:%M:%S')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--seconds', type=float, default=300.0,
                        help='how long to watch (default 300)')
    parser.add_argument('--poll', type=float, default=0.5,
                        help='liveness poll interval in seconds (default 0.5)')
    parser.add_argument('--status-every', type=float, default=5.0,
                        help='seconds between full status reads (default 5)')
    parser.add_argument('--port', help='console device (default: auto-detect)')
    args = parser.parse_args()

    console = Console(args.port)
    if not console.open():
        print('rebootwatch: no Cyclone 2 console found')
        return 2

    print(f'rebootwatch: {console.path}, watching for {args.seconds:.0f}s')

    sample = None
    for _ in range(5):
        sample = console.sample()
        if sample is not None:
            break
        time.sleep(1.0)
    if sample is None:
        print('rebootwatch: the console did not answer "status"')
        console.close()
        return 2
    boots, p33, rtc, uptime = sample
    print(f'{stamp()}  start        boots={boots} p33={p33} rtc={rtc} '
          f'uptime={uptime} ms')

    started = time.time()
    last_status = time.time()
    lost_at = None
    lost_uptime = uptime
    resets = 0

    while time.time() - started < args.seconds:
        time.sleep(args.poll)

        if not console.alive():
            if lost_at is None:
                lost_at = time.time()
                lost_uptime = uptime
                print(f'{stamp()}  console lost  (uptime was {uptime} ms) '
                      f'- re-enumeration means the chip reset')
            console.close()
            if console.open():
                sample = console.sample()
                if sample is None:
                    continue
                new_boots, p33, rtc, uptime = sample
                resets += 1
                print(f'{stamp()}  console back  {console.path} '
                      f'after {time.time() - lost_at:.1f}s down: '
                      f'boots {boots} -> {new_boots}, '
                      f'previous boot survived {lost_uptime} ms, '
                      f'p33={p33} rtc={rtc}')
                boots = new_boots
                lost_at = None
                last_status = time.time()
            continue

        if time.time() - last_status >= args.status_every:
            sample = console.sample()
            last_status = time.time()
            if sample is None:
                continue
            new_boots, new_p33, new_rtc, new_uptime = sample
            if new_boots != boots:
                resets += 1
                print(f'{stamp()}  REBOOT        boots {boots} -> {new_boots} '
                      f'(console stayed), previous boot survived {uptime} ms, '
                      f'p33={new_p33} rtc={new_rtc}')
                boots = new_boots
            p33, rtc, uptime = new_p33, new_rtc, new_uptime

    print(f'{stamp()}  done          boots={boots} p33={p33} rtc={rtc} '
          f'uptime={uptime} ms; {resets} reset(s) seen')
    console.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
