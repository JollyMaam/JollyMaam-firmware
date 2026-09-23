#!/usr/bin/env python3
"""Cyclone 2 firmware stress test.

Broadly, this drives the controller's USB console and checks that the firmware
survives the transitions that keep breaking it:

  * profile switches across all four slots (Switch, Switch 2, generic) and
    repeated switches into the same slot,
  * the pairing -> disconnected -> connecting cycle,
  * a dwell long enough to cross the 30 s connect and 60 s pairing timeouts,
  * rapid back-to-back command bursts, so a profile switch lands while an
    earlier one is still in progress,
  * the console commands that touch the Bluetooth bond store while the radio
    is stopped (`unkey`, `delall`, `bondstore`, `nvscan`).

"Survives" means three things, checked throughout:

  1. the console keeps answering (every step must produce a readable reply);
  2. no command that should succeed fails.  The firmware's designed
     "profile transition already in progress" rejection is counted, not
     treated as a failure;
  3. the chip's preserved boot counter (`status` -> `boots=`) never changes.

The boot counter is the authoritative reset check.  A chip reset
(`p33_soft_reset`) or any crash that reboots the pad increments it and
re-enumerates the USB console.  The console greeting is deliberately *not* used
as the signal: the firmware re-sends that greeting whenever the host's CDC line
state flaps, which it also does for reasons unrelated to the pad, so counting
it reports reboots that did not happen.

When the counter moves, the run also reports the latched reset-source registers
and how long the previous boot survived (see docs/reboot-hunting.md), so a
reset under test arrives with its cause attached rather than as unexplained
silence.  That is why the test owns the console for the whole run: two
processes cannot share one tty - the kernel hands each byte to a single reader,
so a second opener would steal replies from both sides.

The pad must be plugged in and running the application firmware.  Commands are
the same ones a person types at the console (see `help`).

Usage:
    tools/stress.py [--rounds N] [--dwell SECONDS] [--quick] [--port DEV]

Exit status: 0 = passed, 1 = a check failed, 2 = no console found.
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

STATUS_RE = re.compile(r'slot=(\d+) profile=(\w+) state=(\w+) attempt=(\d+)')
BOOTS_RE = re.compile(r'boots=([0-9a-fA-F]{8})')
SOURCE_RE = re.compile(r'reset-source=(\d+) p33=(\d+) rtc=(\d+)')
UPTIME_RE = re.compile(r'uptime=(\d+) ms')

# A command gets this long to produce output, and a reply counts as finished
# once the line has been quiet for this long.
RESPONSE_LIMIT_S = 4.0
QUIET_S = 0.35

# The 33 s dwell exists to cross CYCLONE2_CONNECT_TIMEOUT_MS (30 s) in the
# firmware; the pairing timeout is 60 s, so a longer --dwell crosses both.
DEFAULT_DWELL_S = 33.0

# One rapid burst: switches between every slot plus the commands that reach the
# bond store.  Spacing is short enough that later commands land while earlier
# transitions are still running.
RAPID_BURST = [
    'pair 0', 'connect 1', 'disconnect', 'connect 0', 'pair 3 fresh',
    'connect 2', 'connect status', 'disconnect', 'connect 0', 'pair 1',
    'unkey fa:09:3a:f3:ed:ed', 'newaddr', 'connect status', 'connect 3',
    'disconnect',
    'delall', 'connect 0', 'bondstore', 'nvscan', 'connect status',
]
RAPID_SPACING_S = 0.02

# Accepted (designed) rejection: the firmware refuses a new switch until the
# one in progress has finished.  Bounded rejection is not a crash.
REJECTED = 'ERR profile transition already in progress'


def console_candidates():
    """Console nodes to try, in order, without duplicates."""
    paths = []
    for pattern in ('/dev/bwrap/ttyjl*', '/dev/bwrap/ttyACM*', '/dev/ttyACM*'):
        paths.extend(sorted(glob.glob(pattern)))
    return list(dict.fromkeys(paths))


class Console:
    """A raw serial connection to the configuration console."""

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
        # Modem lines are what the firmware treats as "a terminal is attached".
        for flag in (termios.TIOCMBIC, termios.TIOCMBIS):
            fcntl.ioctl(fd, flag,
                        struct.pack('I', termios.TIOCM_DTR | termios.TIOCM_RTS))
            time.sleep(0.3)

    def open(self):
        ports = [self.requested] if self.requested else console_candidates()
        for path in ports:
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
            self.drain(0.5)
            return True
        return False

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def drain(self, seconds, quiet=QUIET_S):
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

    def ask(self, command, seconds=RESPONSE_LIMIT_S):
        """Send one console command and return everything it printed."""
        if self.fd is None:
            return ''
        try:
            os.write(self.fd, (command + '\r\n').encode())
        except OSError:
            self.close()
            return ''
        return self.drain(seconds)

    def state(self):
        """(slot, profile, state, attempts) from `connect status`, or None."""
        match = STATUS_RE.search(self.ask('connect status'))
        return match.groups() if match else None

    def boots(self):
        """The preserved boot counter, or None when the console is gone."""
        match = BOOTS_RE.search(self.ask('status'))
        return match.group(1) if match else None

    def sample(self):
        """(boots, p33, rtc, uptime) from `status`, or None if unavailable."""
        out = self.ask('status', 6.0)
        boots = BOOTS_RE.search(out)
        source = SOURCE_RE.search(out)
        if not boots or not source:
            return None
        uptime = UPTIME_RE.search(out)
        return (boots.group(1), int(source.group(2)), int(source.group(3)),
                int(uptime.group(1)) if uptime else -1)

    def reopen(self, seconds=10.0):
        """Wait for the console to come back after it went away."""
        self.close()
        deadline = time.time() + seconds
        while time.time() < deadline:
            if self.open():
                return True
            time.sleep(0.5)
        return False


class Report:
    def __init__(self):
        self.checks = 0
        self.failures = []
        self.rejected = 0
        self.boots = None
        self.resets = 0

    def fail(self, message):
        self.failures.append(message)
        print(f"    FAIL: {message}")


def check_reset(console, report):
    """Re-read the boot counter; report a reset and its latched cause.

    This is the whole reason the reset source is instrumented: without it a
    reset only shows up as the console going quiet, which is also what a busy
    device or a bad cable looks like.  Reopening the console here is safe
    because this process is the only thing that has it open.
    """
    sample = console.sample()
    if sample is None:
        if not console.reopen():
            report.fail('the console did not come back')
            return
        sample = console.sample()
        if sample is None:
            report.fail('the console came back but did not answer "status"')
            return
    boots, p33, rtc, uptime = sample
    if report.boots is not None and boots != report.boots:
        report.resets += 1
        print(f"    !! the chip reset: boots {report.boots} -> {boots}, "
              f"p33={p33} rtc={rtc}, uptime {uptime} ms")
    report.boots = boots


def expect(report, what, state, predicate):
    """Check one `connect status` reading, failing when it is missing."""
    report.checks += 1
    if state is None:
        report.fail(f"{what}: no answer from the console")
        return
    problem = predicate(state)
    if problem:
        report.fail(f"{what}: {problem}")


def phase_profile_switches(console, report, rounds):
    print(f"-- profile switches, all four slots x{rounds}")
    for _ in range(rounds):
        for slot in range(4):
            console.ask(f'connect {slot}', 2.5)
            time.sleep(0.35)
            expect(report, f'connect {slot}', console.state(),
                   lambda s, slot=slot:
                   None if s[0] == str(slot) and s[2] != 'off'
                   else f'slot={s[0]} state={s[2]}')
        for slot in range(4):
            console.ask(f'pair {slot}', 2.5)
            time.sleep(0.35)
            expect(report, f'pair {slot}', console.state(),
                   lambda s: None if s[2] == 'pairing'
                   else f'expected pairing, state={s[2]}')


def phase_pair_cycle(console, report, rounds):
    print(f"-- pairing -> disconnected -> connecting x{rounds}")
    for index in range(rounds):
        slot = index % 4
        console.ask(f'pair {slot}', 2.5)
        time.sleep(0.4)
        expect(report, f'cycle {index}: pair {slot}', console.state(),
               lambda s: None if s[2] == 'pairing' else f'state={s[2]}')
        console.ask('disconnect', 2.5)
        time.sleep(0.6)
        expect(report, f'cycle {index}: disconnect', console.state(),
               lambda s: None if s[2] == 'off' else f'expected off, state={s[2]}')
        console.ask(f'connect {slot}', 2.5)
        time.sleep(0.4)
        expect(report, f'cycle {index}: connect {slot}', console.state(),
               lambda s: None if s[2] != 'off' else 'left the radio off')


def phase_rapid(console, report, rounds):
    print(f"-- rapid bursts x{rounds} (switches that land mid-transition)")
    for index in range(rounds):
        for command in RAPID_BURST:
            if console.fd is None:
                report.fail(f'rapid burst {index}: console went away at {command!r}')
                break
            try:
                os.write(console.fd, (command + '\r\n').encode())
            except OSError as exc:
                report.fail(f'rapid burst {index}: write failed at {command!r}: {exc}')
                console.close()
                break
            time.sleep(RAPID_SPACING_S)
        out = console.drain(0.6)
        report.rejected += out.count(REJECTED)
        for line in out.splitlines():
            if line.startswith('ERR') and REJECTED not in line:
                report.fail(f'rapid burst {index}: {line}')
        report.checks += 1


def phase_dwell(console, report, seconds):
    print(f"-- timeout dwell {seconds:.0f} s (crosses the connect timeout)")
    console.ask('connect 0', 2.5)
    time.sleep(seconds)
    state = console.state()
    expect(report, f'dwell {seconds:.0f}s: connect status', state,
           lambda s: None if s[2] != 'off' else 'radio went off during the dwell')
    report.checks += 1
    if console.boots() is None:
        report.fail(f'dwell {seconds:.0f}s: no answer to status afterwards')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--rounds', type=int, default=2,
                        help='repeat count for the switch/cycle/rapid phases')
    parser.add_argument('--dwell', type=float, default=DEFAULT_DWELL_S,
                        help='seconds to idle in connect state (default 33)')
    parser.add_argument('--quick', action='store_true',
                        help='skip the timeout dwell')
    parser.add_argument('--port', help='console device (default: auto-detect)')
    args = parser.parse_args()

    console = Console(args.port)
    if not console.open():
        print('stress: no Cyclone 2 console found '
              '(looked for /dev/bwrap/ttyjl*, /dev/ttyACM*)')
        return 2

    report = Report()
    sample = None
    for _ in range(5):
        sample = console.sample()
        if sample is not None:
            break
        time.sleep(1.0)
    if sample is None:
        print(f'stress: {console.path} did not answer "status"')
        console.close()
        return 2
    report.boots = sample[0]
    baseline = report.boots
    print(f'stress: console {console.path}, baseline boots={baseline} '
          f'p33={sample[1]} rtc={sample[2]}')
    print('stress: the check that matters is that boots= never changes')

    phase_profile_switches(console, report, args.rounds)
    check_reset(console, report)
    phase_pair_cycle(console, report, args.rounds)
    check_reset(console, report)
    phase_rapid(console, report, args.rounds)
    check_reset(console, report)
    if not args.quick:
        phase_dwell(console, report, args.dwell)
        check_reset(console, report)

    final = report.boots
    if final is None:
        report.fail('the console stopped answering by the end of the run')
    elif final != baseline:
        report.fail(f'the chip reset during the run: boots {baseline} -> {final}')
    console.close()

    print()
    print(f'stress: {report.checks} checks, {len(report.failures)} failures, '
          f'{report.rejected} designed transition rejections, '
          f'{report.resets} reset(s), boots {baseline} -> {final}')
    if report.failures:
        print('stress: FAIL')
        return 1
    print('stress: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
