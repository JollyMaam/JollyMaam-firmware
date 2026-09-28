#!/usr/bin/env python3
"""One owner of the pad's USB console, with a shared log and a control lock.

The console is a single terminal: two programs reading it split each other's
output, and a command from one arrives in the middle of the other's trace.  So
exactly one process opens it - this daemon - and everyone else talks to the
daemon over a Unix socket:

    tools/padbus.py serve -p /dev/bwrap/ttyjl0        # owns the port
    tools/padbus.py watch                             # stream everything
    tools/padbus.py request codex && tools/padbus.py send codex stat
    tools/padbus.py release codex
    tools/pad_tui.py                                  # the interactive view

Control is exclusive: only the client holding it may `send`, so the log reads
as one narrative.  Everything the pad prints and every command anyone sends is
appended to one log file and broadcast to every attached client, which is what
makes a session followable from both sides at once.

Protocol: newline-delimited JSON over an AF_UNIX socket.  Requests carry "op"
(and "client" where it matters) and get one reply object; events are pushed to
every connected client as {"ev": ...}.

The daemon drives the console in its machine mode (STX on every open): no
echo, no prompt, commands go out as "@ID line", and the pad tags what comes
back - "=ID text" for a command's output, ".ID OK" / ".ID ERR msg" when it is
done, "! text" for a log line.  So a command's output is never confused with
the log, and `cmd` can hand a caller exactly its own lines and whether the
command worked.  A pad whose firmware predates machine mode ignores the STX and
its output is logged as it comes.
"""

import argparse
from datetime import datetime
import json
import os
import re
import select
import socket
import sys
import termios
import threading
import time

DEFAULT_SOCKET = "/tmp/padbus.sock"
DEFAULT_LOG = "/work/nslog/padbus.log"
LOG_RING = 4000
# The pad's USB bridge.  A reset re-enumerates, so the kernel's own view of the
# bus is the earliest and cheapest sign that something happened - no USB
# traffic and nothing printed.
PAD_USB = ("4c4a", "4155")

# The liveness ping: ENQ (0x05) in, ACK (0x06) back.  One byte each way, never
# echoed and never part of a command, so it costs the console nothing - and
# because the firmware answers it from the application poll, a missing answer
# means the application stopped, not merely that USB went away.
ENQ = b"\x05"
ACK = "\x06"
# Machine mode: no echo or prompt, every line tagged (see the docstring).
STX = b"\x02"
SOH = b"\x01"


def wall_clock_timestamp():
    """Return an unambiguous local wall-clock timestamp with milliseconds."""
    return datetime.now().astimezone().isoformat(timespec="milliseconds")


def printable(text):
    """Replace the bytes a terminal cannot draw.

    The console is a byte stream.  A control character gets into it whenever
    the pad is poked with binary, when a boot comes up garbled, or when a reset
    lands in the middle of a line - and curses raises ValueError on an
    embedded NUL, which kills the whole screen rather than one line.  Cleaning
    text here, before the ring, the log file and every client see it, means
    one fix covers all three.
    """
    out = []
    for char in text:
        code = ord(char)
        if char == "\t" or (0x20 <= code < 0x7f) or code >= 0xa0:
            out.append(char)
        else:
            out.append("?")
    return "".join(out)


def pad_usb_present():
    import glob

    for dev in glob.glob("/sys/bus/usb/devices/*"):
        try:
            with open(os.path.join(dev, "idVendor")) as handle:
                vendor = handle.read().strip()
            with open(os.path.join(dev, "idProduct")) as handle:
                product = handle.read().strip()
        except OSError:
            continue
        if vendor == PAD_USB[0] and product == PAD_USB[1]:
            return True
    return False


def active_pad_ttys():
    """Return the current bwrap nodes for this pad, with their device numbers.

    The host's udev helper creates /dev/bwrap/ttyjlN as device nodes.  Old
    nodes can remain after a remove event, so their existence alone does not
    mean that the corresponding tty is still registered with the kernel.
    Match the live ttyACM sysfs entry and verify st_rdev after opening it.
    """
    import glob

    active = []
    for tty in sorted(glob.glob("/sys/class/tty/ttyACM[0-9]*")):
        name = os.path.basename(tty)
        number = name[len("ttyACM"):]
        if not number.isdigit():
            continue
        try:
            major_text, minor_text = open(os.path.join(tty, "dev")).read().strip().split(":")
            expected = (int(major_text), int(minor_text))
        except (OSError, ValueError):
            continue

        device = os.path.realpath(tty)
        while device.startswith("/sys/"):
            try:
                vendor = open(os.path.join(device, "idVendor")).read().strip()
                product = open(os.path.join(device, "idProduct")).read().strip()
            except OSError:
                vendor = product = ""
            if (vendor, product) == PAD_USB:
                active.append(("/dev/bwrap/ttyjl" + number, expected))
                break
            parent = os.path.dirname(device)
            if parent == device:
                break
            device = parent
    return active


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
    try:
        lines = termios.TIOCM_DTR | termios.TIOCM_RTS
        fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack("i", lines))
        fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack("i", lines))
    except (OSError, AttributeError):
        pass
    time.sleep(0.05)


class Pad:
    """The pad's console: opened once, read continuously, written by the owner."""

    def __init__(self, path):
        self.path = path
        self.fd = -1
        self.fd_lock = threading.RLock()
        # Reported to the server so a reset shows up as an event rather than as
        # a gap in the log: losing the CDC device means the chip re-enumerated.
        self.events = []
        self.link_up = False
        self.opened_once = False
        self.open()

    def open(self):
        deadline = time.monotonic() + 30.0
        while True:
            with self.fd_lock:
                if self.fd >= 0:
                    return
                candidates = active_pad_ttys()
                if self.path in [path for path, _ in candidates]:
                    candidates.sort(key=lambda item: item[0] != self.path)
                for path, expected in candidates:
                    fd = -1
                    try:
                        fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                        actual = os.fstat(fd).st_rdev
                        if (os.major(actual), os.minor(actual)) != expected:
                            os.close(fd)
                            continue
                        configure(fd)
                    except (OSError, termios.error):
                        if fd >= 0:
                            try:
                                os.close(fd)
                            except OSError:
                                pass
                        continue
                    self.fd = fd
                    self.path = path
                    try:
                        os.write(fd, STX)
                    except OSError:
                        pass
                    if not self.link_up:
                        self.link_up = True
                        # Only a *return* is worth reporting; the first open is not.
                        if self.opened_once:
                            self.events.append("back")
                        self.opened_once = True
                    return
            if time.monotonic() >= deadline:
                raise OSError("pad console did not come back")
            time.sleep(0.4)

    def _close_locked(self):
        if self.fd >= 0:
            try:
                os.close(self.fd)
            except OSError:
                pass
        self.fd = -1
        if self.link_up:
            self.link_up = False
            self.events.append("lost")

    def close(self):
        with self.fd_lock:
            self._close_locked()

    def write(self, line):
        return self.write_raw(line.encode() + b"\r")

    def write_raw(self, payload):
        # A reset re-enumerates the pad; reopen from the live sysfs endpoint.
        for attempt in (0, 1):
            with self.fd_lock:
                try:
                    if self.fd < 0:
                        raise OSError("pad console is closed")
                    os.write(self.fd, payload)
                    return True
                except (OSError, termios.error):
                    self._close_locked()
            if attempt == 0:
                try:
                    self.open()
                except OSError:
                    return False
        return False

    def write_now(self, payload):
        """Write without waiting for the port: used by the ping, which must
        keep its own clock running even while the console is away."""
        with self.fd_lock:
            try:
                if self.fd < 0:
                    return False
                os.write(self.fd, payload)
                return True
            except (OSError, termios.error):
                self._close_locked()
                return False

    def read(self):
        """Return whatever is readable now, as text ("" if nothing)."""
        with self.fd_lock:
            if self.fd < 0:
                need_open = True
            else:
                need_open = False
        if need_open:
            self.open()
        with self.fd_lock:
            fd = self.fd
        # Wait without the lock.  Holding it through the select made every
        # write (a command, the ping) queue behind the reader for up to its
        # timeout, and the reader re-took it at once, so a command could wait
        # a second or two before it even reached the pad.
        try:
            ready, _, _ = select.select([fd], [], [], 0.2)
            if not ready:
                return ""
            data = os.read(fd, 4096)
        except (OSError, ValueError, termios.error):
            data = None
        if not data:
            # CDC ACM commonly reports unplug as EOF rather than EIO.
            with self.fd_lock:
                if self.fd == fd:
                    self._close_locked()
            return ""
        return data.decode("utf-8", "replace")


class Server:
    def __init__(self, pad_path, socket_path, log_path, heartbeat=0.0,
                 ping_ms=500.0):
        self.pad = Pad(pad_path)
        self.socket_path = socket_path
        self.log_path = log_path
        self.heartbeat = heartbeat
        self.ping_interval = ping_ms / 1000.0
        self.ping_timeout = max(2.0 * self.ping_interval, 0.35)
        self.last_ack = time.monotonic()
        self.last_ack_wall = wall_clock_timestamp()
        self.ack_missing = False
        self.last_io = time.monotonic()
        self.last_command = 0.0
        self.check_after_loss = False
        self.last_boot = None
        self.last_uptime = None
        self.clients = {}           # socket -> client name
        self.lock = threading.Lock()
        self.owner = None
        self.ring = []
        # Commands on their way: id -> {client, lines, quiet, done, ok, error}
        self.pending = {}
        self.next_id = 1
        self.partial = ""
        self.partial_time = 0.0
        self.shutting_down = False
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        if os.path.exists(socket_path):
            os.unlink(socket_path)
        self.listener.bind(socket_path)
        self.listener.listen(8)
        self.logfile = open(log_path, "a", buffering=1)
        self.note(f"padbus attached to {self.pad.path}")

    def note(self, text):
        """A line about the session itself (not from the pad)."""
        self.emit("# " + text)

    def note_at(self, text):
        """Log a host-detected event with its wall-clock time."""
        self.note("[%s] %s" % (wall_clock_timestamp(), text))

    def emit(self, line):
        # Pad output keeps its firmware timestamp; note_at() timestamps
        # host-detected events where the wall-clock time matters.
        line = printable(line.rstrip("\r"))
        if not line.strip():
            return
        self.ring.append(line)
        if len(self.ring) > LOG_RING:
            del self.ring[:len(self.ring) - LOG_RING]
        self.logfile.write(line + "\n")
        self.broadcast({"ev": "log", "line": line})

    def track_resets(self, line):
        """Watch the pad's own status for a reset, and say so loudly.

        `stat` carries the boot counter and the uptime, so a change in either
        is a reset - whatever else the pad was doing at the time.  The pad's
        heartbeat (see the `heartbeat` loop) is what feeds this when the console
        is otherwise quiet.
        """
        boot = re.search(r'boots=([0-9a-fA-F]+)', line)
        uptime = re.search(r'\bup=(\d+)', line)
        if uptime:
            value = int(uptime.group(1))
            if self.last_uptime is not None and value < self.last_uptime:
                self.note_at("RESET: uptime went backwards (%d -> %d ms)"
                             % (self.last_uptime, value))
            self.last_uptime = value
        if boot:
            value = boot.group(1).lower()
            if self.last_boot is None:
                # First reading: nothing to compare against, so say nothing
                # rather than claim the boot counter is unchanged.
                pass
            elif value != self.last_boot:
                self.note_at("RESET: boot counter %s -> %s"
                             % (self.last_boot, value))
            elif self.check_after_loss:
                self.note("same boot counter %s - the re-enumeration was not a "
                          "chip reset" % value)
            self.last_boot = value
            self.check_after_loss = False

    def heartbeat_loop(self):
        """Optional console poll.  Off by default: watching the USB bus tells
        us the same thing without printing anything."""
        if self.heartbeat <= 0:
            return
        while True:
            time.sleep(0.5)
            now = time.monotonic()
            if now - self.last_io < self.heartbeat:
                continue
            if now - self.last_command < self.heartbeat:
                continue
            self.last_command = now
            self.submit("padbus", "stat")

    def usb_watch_loop(self):
        """The silent heartbeat: ask the kernel, not the pad.

        A reset re-enumerates the USB device, so the device node disappearing
        from sysfs is the earliest reliable sign - it needs no traffic on the
        console (nothing is printed) and no control transfer (which this
        container cannot issue anyway: it has no /dev/bus/usb device nodes).

        On the way back we ask for `stat` once, because the boot counter is what
        distinguishes a real reset from a USB hiccup.
        """
        self.pad_usb = pad_usb_present()
        if not self.pad_usb:
            self.note_at("pad USB device not present")
        while True:
            time.sleep(1.0)
            present = pad_usb_present()
            if present == self.pad_usb:
                continue
            self.pad_usb = present
            if present:
                self.note_at("pad USB device back - reading the boot counter")
                self.check_after_loss = True
                self.submit("padbus", "stat")
            else:
                self.note_at("pad USB device gone - reset, reflash, or unplugged")
                self.check_after_loss = True

    def ping_loop(self):
        """Ask "are you alive?" and say exactly when the answer stops.

        The firmware answers with one ACK byte from its application poll, so
        the gap between the last answer and the first silence brackets the
        moment the pad died - that is what the timing here is for.  A miss and
        the recovery are both logged with a millisecond timestamp.
        """
        if self.ping_interval <= 0:
            return
        # Check often enough that the notice is prompt; the ping itself goes
        # out on the requested interval.
        tick = min(0.1, self.ping_interval / 2.0)
        next_ping = time.monotonic()
        while True:
            time.sleep(tick)
            now = time.monotonic()
            silent = now - self.last_ack
            if silent > self.ping_timeout:
                if not self.ack_missing:
                    self.ack_missing = True
                    self.note_at("no ACK - pad not answering; last answer "
                                 "%s (%.0f ms earlier)"
                                 % (self.last_ack_wall, silent * 1000))
            elif self.ack_missing:
                self.ack_missing = False
                self.note_at("ACK again - silent for %.0f ms"
                             % (silent * 1000))
            if now >= next_ping:
                next_ping = now + self.ping_interval
                # Non-blocking: reopening the port is the reader's job, and
                # waiting for it here would stop this clock exactly when it
                # matters most.
                self.pad.write_now(ENQ)

    def broadcast(self, message):
        payload = (json.dumps(message) + "\n").encode()
        with self.lock:
            clients = list(self.clients)
        for sock in clients:
            try:
                sock.sendall(payload)
            except OSError:
                self.drop(sock)

    def drop(self, sock):
        with self.lock:
            name = self.clients.pop(sock, None)
        try:
            sock.close()
        except OSError:
            pass
        # The holder is gone (TUI quit, kill, crash).  Hand control back, so
        # the next client can take it without waiting for a release that will
        # never come.
        if name and self.owner == name:
            self.set_owner(None)

    def set_owner(self, owner):
        if owner == self.owner:
            return
        self.owner = owner
        self.note("control: " + (owner if owner else "released"))
        self.broadcast({"ev": "owner", "owner": owner})

    def submit(self, client, line, quiet=False):
        """Send one command, tagged; returns its pending record (or None)."""
        with self.lock:
            ident = str(self.next_id)
            self.next_id += 1
            record = {"client": client, "lines": [], "quiet": quiet,
                      "done": threading.Event(), "ok": None, "error": None,
                      "at": time.monotonic()}
            self.pending[ident] = record
            # Forget commands that never finished (a reset ate them).
            for stale in [key for key, value in self.pending.items()
                          if time.monotonic() - value["at"] > 120]:
                del self.pending[stale]
        if not self.pad.write("@%s %s" % (ident, line)):
            with self.lock:
                self.pending.pop(ident, None)
            return None
        return record

    def tagged(self, line):
        """Handle one tagged line from the pad; False if it was not one."""
        if line.startswith("! "):
            self.emit(line[2:])
            return True
        if not line or line[0] not in "=.":
            return False
        ident, _, rest = line[1:].partition(" ")
        with self.lock:
            record = self.pending.get(ident)
            if record is not None and line[0] == ".":
                del self.pending[ident]
        if record is None:
            return False
        if line[0] == "=":
            record["lines"].append(rest)
            if "stat up=" in rest:
                self.track_resets(rest)
            if not record["quiet"]:
                self.emit(rest)
            return True
        record["ok"] = rest == "OK"
        record["error"] = rest[4:] if rest.startswith("ERR ") else None
        if not record["quiet"]:
            if not record["ok"]:
                self.emit("ERR " + (record["error"] or rest))
            elif not record["lines"]:
                self.emit("OK")
        record["done"].set()
        return True

    def handle(self, client_name, request):
        op = request.get("op")
        client = request.get("client") or client_name or "client"
        reply = {"ok": True}

        if op == "hello":
            reply["owner"] = self.owner
            reply["log"] = self.ring[-200:]
        elif op == "tail":
            reply["log"] = self.ring[-int(request.get("n", 200)):]
            reply["owner"] = self.owner
        elif op == "request":
            if self.owner in (None, client):
                self.set_owner(client)
                reply["owner"] = self.owner
            else:
                reply["ok"] = False
                reply["owner"] = self.owner
                reply["error"] = "held by %s" % self.owner
        elif op == "release":
            if self.owner == client:
                self.set_owner(None)
            reply["owner"] = self.owner
        elif op == "send":
            if self.owner != client:
                reply["ok"] = False
                reply["error"] = "not the owner (held by %s)" % self.owner
            else:
                line = request.get("line", "")
                self.emit(">>> [%s] %s" % (client, line))
                reply["sent"] = self.submit(client, line) is not None
        elif op == "cmd":
            # Like send, but wait for the pad to finish and hand back exactly
            # this command's output and whether it worked.
            if self.owner != client:
                reply["ok"] = False
                reply["error"] = "not the owner (held by %s)" % self.owner
            else:
                line = request.get("line", "")
                quiet = bool(request.get("quiet"))
                if not quiet:
                    self.emit(">>> [%s] %s" % (client, line))
                record = self.submit(client, line, quiet)
                if record is None:
                    reply["ok"] = False
                    reply["error"] = "the pad is not there"
                elif not record["done"].wait(float(request.get("timeout", 10))):
                    reply["ok"] = False
                    reply["error"] = "no answer"
                    reply["lines"] = record["lines"]
                else:
                    reply["ok"] = bool(record["ok"])
                    reply["lines"] = record["lines"]
                    if record["error"]:
                        reply["error"] = record["error"]
        elif op == "complete":
            # What Tab would offer for a line.  Read-only, so anyone may ask,
            # and it is not logged.
            line = request.get("line", "")
            record = self.submit(client, "complete " + line, quiet=True)
            reply["complete"] = {"line": line, "candidates": []}
            if record is not None and record["done"].wait(2.0):
                reply["complete"]["candidates"] = record["lines"]
        elif op == "quit":
            # A new daemon asks this one to stand down before it binds, so the
            # socket always belongs to exactly one owner.  The reply is sent by
            # client_thread; the accept loop notices the flag and shuts down.
            self.shutting_down = True
        else:
            reply["ok"] = False
            reply["error"] = "unknown op %r" % (op,)
        return reply

    def pad_reader(self):
        """Everything the pad prints, one thread, one log."""
        while True:
            while self.pad.events:
                event = self.pad.events.pop(0)
                if event == "lost":
                    # The USB watcher announces this; all we do here is make
                    # sure the boot counter gets read when it comes back.
                    self.check_after_loss = True
                else:
                    self.check_after_loss = True
            try:
                text = self.pad.read()
            except OSError:
                time.sleep(0.5)
                continue
            now = time.monotonic()
            if text:
                self.last_io = now
            if not text:
                # A prompt has no trailing newline.  Emitting it only after the
                # line has been quiet for a moment keeps a chunk boundary in
                # the middle of a debug line ("[1.234] " then the text) from
                # turning one message into two.
                if self.partial and now - self.partial_time > 0.4:
                    self.emit(self.partial)
                    self.partial = ""
                continue
            self.partial += text
            self.partial_time = now
            if ACK in self.partial:
                # The ping answer: consume it before anything else sees it, so
                # it never reaches the log or gets glued to a line of output.
                self.last_ack = now
                self.last_ack_wall = wall_clock_timestamp()
                self.partial = self.partial.replace(ACK, "")
            while "\n" in self.partial:
                line, self.partial = self.partial.split("\n", 1)
                line = line.rstrip("\r")
                if self.tagged(line):
                    continue
                if "stat up=" in line:
                    self.track_resets(line)
                self.emit(line)

    def client_thread(self, conn):
        buf = b""
        try:
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    if not raw.strip():
                        continue
                    try:
                        request = json.loads(raw.decode("utf-8", "replace"))
                    except ValueError:
                        continue
                    # Remember who this connection is, so that closing it can
                    # release the control it holds.  `hello` is the only
                    # request that carries a client name up front; later ones
                    # name themselves for the `send` check alone.
                    if request.get("op") == "hello":
                        with self.lock:
                            self.clients[conn] = request.get("client") or "client"
                    reply = self.handle(None, request)
                    conn.sendall((json.dumps(reply) + "\n").encode())
        except OSError:
            pass
        finally:
            self.drop(conn)

    def serve(self):
        threading.Thread(target=self.pad_reader, daemon=True).start()
        threading.Thread(target=self.heartbeat_loop, daemon=True).start()
        threading.Thread(target=self.usb_watch_loop, daemon=True).start()
        threading.Thread(target=self.ping_loop, daemon=True).start()
        # A timeout (rather than a blocking accept) is what lets a `quit`
        # request, handled on a client thread, actually end this loop.
        self.listener.settimeout(0.5)
        while not self.shutting_down:
            try:
                conn, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                time.sleep(0.2)
                continue
            with self.lock:
                self.clients[conn] = None
            threading.Thread(target=self.client_thread, args=(conn,),
                             daemon=True).start()
        self.shutdown()

    def shutdown(self):
        self.note("shutting down")
        with self.lock:
            for conn in list(self.clients):
                try:
                    conn.close()
                except OSError:
                    pass
            self.clients.clear()
        try:
            self.listener.close()
        except OSError:
            pass
        # Hand the console back in its human mode, so a terminal opened next
        # gets echo and a prompt (closing the port does not tell the pad).
        self.pad.write_now(SOH)
        self.pad.close()
        try:
            os.unlink(self.socket_path)
        except OSError:
            pass
        try:
            self.logfile.flush()
            self.logfile.close()
        except OSError:
            pass


def client_socket(path):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(path)
    return sock


def probe_socket(path, timeout=0.5):
    """Return a connected probe socket, or None if nothing is listening."""
    probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    probe.settimeout(timeout)
    try:
        probe.connect(path)
    except OSError:
        probe.close()
        return None
    return probe


def take_over(path, timeout=5.0):
    """Ask any daemon already on `path` to quit, and wait until it is gone.

    Starting a second daemon on one console is what produced the fake "console
    lost" events earlier, so a fresh daemon has to be the only one.  Rather than
    refuse to start, it evicts the incumbent through the protocol the incumbent
    already speaks, then waits for the socket to stop answering.
    """
    deadline = time.monotonic() + timeout
    while True:
        probe = probe_socket(path)
        if probe is None:
            return 0
        try:
            probe.sendall((json.dumps({"op": "quit",
                                       "client": "takeover"}) + "\n").encode())
        except OSError:
            pass
        try:
            probe.close()
        except OSError:
            pass
        if time.monotonic() >= deadline:
            return 1
        time.sleep(0.1)


def request(sock, **fields):
    """Send one request and return its reply.

    Replies and broadcasts share the connection, so anything tagged "ev" is a
    broadcast for everyone else and is skipped.
    """
    sock.sendall((json.dumps(fields) + "\n").encode())
    buf = b""
    while True:
        while b"\n" not in buf:
            chunk = sock.recv(4096)
            if not chunk:
                return {}
            buf += chunk
        raw, buf = buf.split(b"\n", 1)
        if not raw.strip():
            continue
        try:
            message = json.loads(raw.decode("utf-8", "replace"))
        except ValueError:
            continue
        if "ev" in message:
            continue
        return message


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", default=DEFAULT_SOCKET)
    parser.add_argument("--log", default=DEFAULT_LOG)
    sub = parser.add_subparsers(dest="op", required=True)

    serve = sub.add_parser("serve")
    serve.add_argument("-p", "--port", default="/dev/bwrap/ttyjl0")
    serve.add_argument("--heartbeat", type=float, default=0.0,
                       help="optional console poll, in seconds, for a reset "
                            "check when the pad has been quiet (0 = off; the "
                            "USB watcher already covers resets and prints "
                            "nothing)")
    serve.add_argument("--ping-ms", type=float, default=500.0,
                       help="ENQ/ACK liveness ping interval in milliseconds "
                            "(0 disables it).  The ping leaves no mark on the "
                            "console; a missed answer is logged with a "
                            "millisecond timestamp")

    watch = sub.add_parser("watch")
    watch.add_argument("client", nargs="?", default="watch")

    for name in ("request", "release"):
        sub.add_parser(name).add_argument("client")

    sub.add_parser("quit")

    send = sub.add_parser("send")
    send.add_argument("client")
    send.add_argument("line", nargs="+")

    cmd = sub.add_parser("cmd", help="send a command, wait for it, print "
                         "its output; exit status 1 on ERR")
    cmd.add_argument("client")
    cmd.add_argument("line", nargs="+")
    cmd.add_argument("--timeout", type=float, default=10.0)
    cmd.add_argument("--quiet", action="store_true",
                     help="keep it out of the shared log")

    tail = sub.add_parser("tail")
    tail.add_argument("-n", type=int, default=40)

    args = parser.parse_args()

    if args.op == "serve":
        # One owner per console: evict any daemon already on this socket before
        # binding.  Two daemons interleave output, fight over the control line,
        # and write the same log twice - and the losing one looks like a pad
        # reset.  See take_over().
        if take_over(args.socket) != 0:
            print("padbus: a daemon is still answering on %s after being asked "
                  "to quit - not starting a second one" % args.socket,
                  file=sys.stderr)
            return 1
        Server(args.port, args.socket, args.log,
               heartbeat=args.heartbeat, ping_ms=args.ping_ms).serve()
        return 0

    sock = client_socket(args.socket)
    if args.op == "watch":
        sock.sendall((json.dumps({"op": "hello",
                                  "client": args.client}) + "\n").encode())
        buf = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                try:
                    message = json.loads(raw.decode("utf-8", "replace"))
                except ValueError:
                    continue
                if message.get("ev") == "log":
                    print(message["line"], flush=True)
                elif message.get("ev") == "owner":
                    print("# control: %s" % (message.get("owner") or "released"),
                          flush=True)
        return 0

    if args.op == "tail":
        reply = request(sock, op="tail", n=args.n)
        print("owner: %s" % (reply.get("owner") or "none"))
        for line in reply.get("log", []):
            print(line)
        return 0

    if args.op == "quit":
        reply = request(sock, op="quit", client="cli")
        print(json.dumps(reply))
        return 0 if reply.get("ok") else 1

    if args.op == "cmd":
        reply = request(sock, op="cmd", client=args.client,
                        line=" ".join(args.line), timeout=args.timeout,
                        quiet=args.quiet)
        for line in reply.get("lines", []):
            print(line)
        if not reply.get("ok"):
            print("ERR " + reply.get("error", "failed"), file=sys.stderr)
            return 1
        return 0

    if args.op in ("request", "release"):
        reply = request(sock, op=args.op, client=args.client)
    else:
        reply = request(sock, op="send", client=args.client,
                        line=" ".join(args.line))
    print(json.dumps(reply))
    return 0 if reply.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
