#!/usr/bin/env python3
"""Pair a RAM-only pad to a console and save the bond it produces.

The RAM-only build (BOND_STORE_RAM=1) starts every boot with no bonds, so a
pairing produces the only copy of the link key the pad will ever have, and a
reset destroys it.  This tool holds the USB console open across the whole
sequence, so the run is captured end to end, and reads the host's link-key
store out while the pairing is live:

    tools/bond_capture.py -p /dev/bwrap/ttyjl0 -o /work/nslog/bond1.txt \\
        --slot 0 --fresh

The file it writes ends with the part worth keeping: one machine-readable
``BOND`` line holding the pad's address, the console's address and the 16-byte
link key.  That line is what `restore` (or `setaddr` + `setkey`) puts back on a
later boot, so a reconnect can be tested without pairing again.

Nothing here writes the pad's flash on the RAM-only build: the pairing itself
is the only state, and it lives in RAM.
"""

import argparse
import errno
import glob
import os
import re
import select
import sys
import termios
import time


def configure(fd):
    import fcntl
    import struct

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
    # The firmware only counts the console open while the host asserts these,
    # and a controller reset leaves them deasserted, so clear before setting.
    try:
        lines = termios.TIOCM_DTR | termios.TIOCM_RTS
        fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack("i", lines))
        fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack("i", lines))
    except (OSError, AttributeError):
        pass
    time.sleep(0.05)


class Console:
    """One open console, re-opened across the resets a profile start causes."""

    # termios.error is not an OSError subclass, and a reset makes both it and
    # EIO show up here, so every console call has to survive either.
    TTY_ERRORS = (OSError, termios.error)

    def __init__(self, path, output, verbose):
        self.path = path
        self.output = output
        self.verbose = verbose
        self.text = ""
        self.fd = -1
        self.open()

    def note(self, text):
        self.output.write(text + "\n")
        self.output.flush()
        if self.verbose:
            print(text, flush=True)

    def open(self):
        deadline = time.monotonic() + 30.0
        while True:
            # A reset re-enumerates the CDC device and it comes back on a
            # different ttyjl number, which leaves the old node present but
            # unopenable.  Fall back to whatever node answers.
            path = self.path if self.path and os.path.exists(self.path) else None
            candidates = sorted(glob.glob("/dev/bwrap/ttyjl*"))
            if path is None and candidates:
                path = candidates[0]
            tried = []
            if path is not None:
                tried.append(path)
            for candidate in candidates:
                if candidate not in tried:
                    tried.append(candidate)
            for path in tried:
                try:
                    self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY |
                                      os.O_NONBLOCK)
                    configure(self.fd)
                except self.TTY_ERRORS:
                    try:
                        os.close(self.fd)
                    except self.TTY_ERRORS:
                        pass
                    self.fd = -1
                    continue
                else:
                    self.path = path
                    self.note(f"# opened {path}")
                    return
            if time.monotonic() >= deadline:
                raise OSError("console did not come back")
            time.sleep(0.4)

    def reopen(self):
        try:
            os.close(self.fd)
        except OSError:
            pass
        self.fd = -1
        self.open()

    def read(self, deadline, quiet=0.2):
        """Drain the console until ``deadline``, or ``quiet`` after the last byte.

        The deadline is a hard cap.  While a session is up the reports arrive
        faster than ``quiet``, so a loop that only extends the limit on every
        chunk would never return.
        """
        got = b""
        limit = max(deadline, time.monotonic() + quiet)
        last_line_state = 0.0
        while time.monotonic() < limit:
            now = time.monotonic()
            if self.fd < 0:
                self.reopen()
            elif now - last_line_state >= 1.0:
                # A reset clears the control lines on the device side.
                last_line_state = now
                try:
                    configure(self.fd)
                except self.TTY_ERRORS:
                    self.reopen()
                    continue
            try:
                ready, _, _ = select.select([self.fd], [], [], 0.2)
            except self.TTY_ERRORS:
                self.reopen()
                continue
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 4096)
            except self.TTY_ERRORS as error:
                if getattr(error, "errno", None) in (errno.EIO, errno.EAGAIN,
                                                     None):
                    self.reopen()
                    continue
                raise
            if not chunk:
                continue
            got += chunk
            limit = min(deadline, time.monotonic() + quiet)
            text = chunk.decode("utf-8", "replace")
            self.text += text
            self.output.write(text)
            self.output.flush()
            if self.verbose:
                sys.stdout.write(text)
                sys.stdout.flush()
        return got

    def send(self, command):
        self.note(f">>> {command}")
        deadline = time.monotonic() + 10.0
        while True:
            if self.fd < 0:
                self.reopen()
            try:
                os.write(self.fd, command.encode() + b"\r")
                return
            except self.TTY_ERRORS:
                if time.monotonic() >= deadline:
                    raise
                self.reopen()

    def command(self, text, wait=1.2):
        self.send(text)
        time.sleep(wait)
        self.read(time.monotonic() + 0.4)

    def state(self, timeout=2.0):
        self.send("connect status")
        self.read(time.monotonic() + timeout)
        matches = re.findall(r"state=(\w+)", self.text)
        return matches[-1] if matches else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--port", default="/dev/bwrap/ttyjl0")
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("--slot", type=int, default=0)
    parser.add_argument("--fresh", action="store_true",
                        help="also give the pad a new Bluetooth address")
    parser.add_argument("--pair-wait", type=float, default=180.0,
                        help="seconds to wait for the console to pair")
    parser.add_argument("--steady", type=float, default=4.0,
                        help="seconds to let the session run before dumping")
    parser.add_argument("--reconnect", action="store_true",
                        help="after dumping, disconnect and page the console")
    parser.add_argument("--leave-cgo-wait", type=float, default=0.0,
                        help="with --reconnect: pause this long first so the "
                             "console can leave Change Grip/Order (press Home), "
                             "which is when a console seems to commit the bond")
    parser.add_argument("--watch", type=float, default=20.0)
    parser.add_argument("-q", "--quiet", action="store_true")
    args = parser.parse_args()

    with open(args.output, "w") as output:
        console = Console(args.port, output, not args.quiet)
        console.command("debug on")
        console.command("hcidump on")
        console.command("connect status")
        console.command(f"pair {args.slot}" + (" fresh" if args.fresh else ""))

        print("waiting for the console to pair; put it in Change Grip/Order",
              file=sys.stderr, flush=True)
        deadline = time.monotonic() + args.pair_wait
        state = None
        while time.monotonic() < deadline:
            state = console.state()
            if state == "connected":
                break
        print(f"state={state}", file=sys.stderr, flush=True)

        time.sleep(args.steady)
        console.command("connect status")
        console.send("keys")
        time.sleep(1.5)
        console.read(time.monotonic() + 0.5)

        if args.reconnect:
            if args.leave_cgo_wait > 0:
                print(f"leaving Change Grip/Order: press Home now, "
                      f"{args.leave_cgo_wait:.0f}s", file=sys.stderr,
                      flush=True)
                console.read(time.monotonic() + args.leave_cgo_wait)
            console.command("disconnect", wait=1.0)
            time.sleep(3.0)
            console.read(time.monotonic() + 1.0)
            console.command(f"connect {args.slot}", wait=1.0)
            # Give the page a few attempts and report what each one did.
            deadline = time.monotonic() + 45.0
            while time.monotonic() < deadline:
                console.read(time.monotonic() + 2.0)
                if "hid channels open" in console.text.split(">>> disconnect")[-1]:
                    break

        console.read(time.monotonic() + args.watch)

    summarise(args.output)
    return 0


def summarise(path):
    """Pull the pad address, console address and key out of the finished run."""
    with open(path, "r", errors="replace") as handle:
        text = handle.read()

    addresses = re.findall(r"address=([0-9a-f:]{17})", text)
    local = addresses[-1] if addresses else "?"
    keys = re.findall(r"key slot=\d+ peer=([0-9a-f:]{17}) type=(\d+) "
                      r"key=([0-9a-f]{32})", text)

    with open(path, "a") as handle:
        handle.write("\n# captured bond summary\n")
        if not keys:
            handle.write("# no link key in the host store: the pairing never "
                         "reached put_link_key\n")
        for peer, key_type, key in keys:
            handle.write(f"BOND local={local} peer={peer} "
                         f"type={key_type} key={key}\n")

    print(f"local address {local}, {len(keys)} stored key(s)", file=sys.stderr)
    for peer, key_type, key in keys:
        print(f"BOND local={local} peer={peer} type={key_type} key={key}",
              file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
