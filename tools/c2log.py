#!/usr/bin/env python3
"""Capture the controller's USB console for a while.

The firmware buffers runtime diagnostics and only flushes them while a host has
the CDC port open (see docs/usb-control.md), so a capture has to be running
before the interesting traffic happens.  Starting a profile can reset the chip
and re-enumerate USB, so this tool reopens the port when that happens.

    tools/c2log.py -t 60 -o /work/nslog/log2.txt --command 'connect 0'

The device's own debug lines already carry a [seconds.ms] prefix; commands sent
by this tool are written as ">>> ..." lines so the two can be told apart.
"""

import argparse
import errno
import os
import select
import struct
import sys
import termios
import time

DEFAULT_PATHS = ("/dev/bwrap/ttyACM0", "/dev/bwrap/ttyACM1",
                 "/dev/ttyACM0", "/dev/ttyACM1")


def find_port(requested):
    if requested:
        return requested
    for path in DEFAULT_PATHS:
        if os.path.exists(path):
            return path
    return None


def configure(fd):
    attributes = termios.tcgetattr(fd)
    attributes[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK |
                       termios.ISTRIP | termios.INLCR | termios.IGNCR |
                       termios.ICRNL | termios.IXON)
    attributes[1] &= ~termios.OPOST
    attributes[2] &= ~(termios.CSIZE | termios.PARENB)
    attributes[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
    attributes[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON |
                       termios.ISIG | termios.IEXTEN)
    attributes[4] = termios.B115200
    attributes[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attributes)
    # The firmware considers the console open only when the host asserts these.
    try:
        lines = termios.TIOCM_DTR | termios.TIOCM_RTS
        import fcntl
        fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack("i", lines))
    except (OSError, AttributeError):
        pass
    time.sleep(0.05)


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK | os.O_CLOEXEC)
    try:
        configure(fd)
    except Exception:
        os.close(fd)
        raise
    return fd


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--port")
    parser.add_argument("-t", "--duration", type=float, default=60.0)
    parser.add_argument("-o", "--output")
    parser.add_argument("--command", action="append", default=[],
                       help="send this console command once the port is open "
                            "(may be repeated; each is sent one second apart)")
    parser.add_argument("--command-delay", type=float, default=1.0,
                       help="seconds to wait before the first command")
    parser.add_argument("-q", "--quiet", action="store_true")
    args = parser.parse_args()

    output = open(args.output, "w") if args.output else None

    def note(text):
        if output:
            output.write(text + "\n")
            output.flush()
        if not args.quiet:
            print(text, flush=True)

    deadline = time.monotonic() + args.duration
    next_command = None
    fd = None
    byte_count = 0
    reopen_count = 0

    try:
        while time.monotonic() < deadline:
            if fd is None:
                path = find_port(args.port)
                if path is None:
                    time.sleep(0.5)
                    continue
                try:
                    fd = open_port(path)
                except OSError as error:
                    if error.errno not in (errno.ENODEV, errno.ENOENT,
                                           errno.EBUSY, errno.EIO):
                        raise
                    time.sleep(0.5)
                    continue
                if reopen_count:
                    note("# reopened %s" % path)
                next_command = time.monotonic() + args.command_delay

            if args.command and next_command is not None and \
                    time.monotonic() >= next_command:
                command = args.command.pop(0)
                os.write(fd, (command + "\r").encode())
                note(">>> " + command)
                next_command = (time.monotonic() + 1.0
                                if args.command else None)

            try:
                readable, _, _ = select.select([fd], [], [], 0.2)
            except OSError:
                readable = [fd]
            if not readable:
                continue
            try:
                data = os.read(fd, 4096)
            except OSError as error:
                if error.errno in (errno.EIO, errno.EAGAIN):
                    data = b""
                else:
                    raise
            if not data:
                os.close(fd)
                fd = None
                reopen_count += 1
                time.sleep(0.3)
                continue
            byte_count += len(data)
            text = data.decode("utf-8", "replace")
            if output:
                output.write(text)
                output.flush()
            if not args.quiet:
                sys.stdout.write(text)
                sys.stdout.flush()
    finally:
        if fd is not None:
            os.close(fd)
        if output:
            note("# %u bytes, %u reopen(s), %.1fs"
                 % (byte_count, reopen_count, args.duration))
            output.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
