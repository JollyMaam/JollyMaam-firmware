#!/usr/bin/env python3
"""Drive one disconnect/reconnect cycle and capture the USB console.

The firmware buffers its runtime diagnostics and only flushes them while a
host holds the CDC port open, so a reconnect can
only be debugged by a tool that is holding the port across the whole cycle.

    tools/probe_reconnect.py -p /dev/bwrap/ttyjl0 -o /work/nslog/reconnect1.txt

The sequence is: poll ``debug stat`` until the peer is connected, drop the
link with ``disconnect``, wait, then page the saved peer again with
``connect 0``.  Everything the device prints, and every command sent, is
written to the output file.
"""

import argparse
import glob
import os
import re
import select
import sys
import time


def configure(fd):
    import fcntl
    import struct
    import termios

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
        # Clear first: after the controller resets it comes back with DTR/RTS
        # deasserted, and setting bits that the tty already believes are set
        # would not make the kernel send a new SET_CONTROL_LINE_STATE - so the
        # firmware would keep the console "closed" and drop all its output.
        fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack("i", lines))
        fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack("i", lines))
    except (OSError, AttributeError):
        pass
    time.sleep(0.05)


class Console:
    def __init__(self, path, output):
        self.path = path
        self.output = output
        self.text = ""
        self.fd = -1
        self.last_line_state = 0.0
        self.open()

    def open(self):
        """Open the port, waiting for it to enumerate if needed.

        Starting a profile resets the chip, so the console disappears for a few
        seconds and comes back - possibly on a different ttyjl number.
        """
        deadline = time.monotonic() + 30.0
        while True:
            path = self.path if os.path.exists(self.path) else None
            if path is None:
                candidates = sorted(glob.glob("/dev/bwrap/ttyjl*"))
                path = candidates[0] if candidates else None
            if path is not None:
                try:
                    self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY |
                                      os.O_NONBLOCK)
                except OSError:
                    self.fd = -1
                else:
                    self.path = path
                    configure(self.fd)
                    self.output.write(f"\n>>> opened {path}\n")
                    self.output.flush()
                    return
            if time.monotonic() >= deadline:
                raise OSError("console did not come back")
            time.sleep(0.4)

    def reopen(self):
        self.close()
        self.open()

    def close(self):
        if self.fd < 0:
            return
        try:
            os.close(self.fd)
        except OSError:
            pass
        self.fd = -1

    def read(self, deadline, quiet=0.2):
        """Read until ``deadline`` after the last byte seen."""
        got = b""
        now = time.monotonic()
        limit = max(deadline, now + quiet)
        while True:
            now = time.monotonic()
            if now >= limit:
                break
            if self.fd < 0:
                self.reopen()
            else:
                # The controller resets mid-session and comes back with its
                # control-line state cleared.  A SET_CONTROL_LINE_STATE sent
                # before it finished enumerating is lost, so keep re-asserting
                # it until the firmware sees an open terminal and flushes.
                if now - self.last_line_state >= 1.0:
                    self.last_line_state = now
                    try:
                        configure(self.fd)
                    except Exception:
                        self.reopen()
                        continue
            ready, _, _ = select.select([self.fd], [], [], min(limit - now, 0.2))
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 4096)
            except OSError:
                self.reopen()
                continue
            if not chunk:
                continue
            got += chunk
            limit = time.monotonic() + quiet
        if got:
            self.text += got.decode("utf-8", "replace")
            self.output.write(got.decode("utf-8", "replace"))
            self.output.flush()
        return got

    def send(self, command):
        line = f"\n>>> {command}\n"
        self.output.write(line)
        self.output.flush()
        deadline = time.monotonic() + 10.0
        while True:
            if self.fd < 0:
                self.reopen()
            try:
                os.write(self.fd, command.encode() + b"\n")
                return
            except OSError:
                self.close()
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.3)

    def status(self, timeout=2.0):
        self.send("debug stat")
        self.read(time.monotonic() + timeout)
        matches = re.findall(r"state=(\w+)", self.text)
        return matches[-1] if matches else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--port", required=True)
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("--wait-connected", type=float, default=0.0,
                        help="seconds to wait for state=connected before acting")
    parser.add_argument("--settle", type=float, default=5.0,
                        help="seconds to wait after disconnect")
    parser.add_argument("--watch", type=float, default=30.0,
                        help="seconds to keep capturing after connect")
    args = parser.parse_args()

    with open(args.output, "w") as output:
        console = Console(args.port, output)
        try:
            deadline = time.monotonic() + args.wait_connected
            state = console.status()
            print(f"state={state}")
            while args.wait_connected and time.monotonic() < deadline:
                if state == "connected":
                    break
                time.sleep(2.0)
                state = console.status()
                print(f"state={state}")

            console.read(time.monotonic() + 1.0)
            console.send("disconnect")
            console.read(time.monotonic() + args.settle)
            console.send("connect 0")
            console.read(time.monotonic() + args.watch)
            state = console.status()
            print(f"state after reconnect: {state}")
            console.read(time.monotonic() + 2.0)
        finally:
            console.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
