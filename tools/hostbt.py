#!/usr/bin/env python3
"""A Linux Bluetooth host the pad can be tested against, driven remotely.

Run the server on the machine that owns the Bluetooth adapter:

    sudo python3 tools/hostbt.py serve [--btsnoop FILE]
    sudo python3 tools/hostbt.py serve --listen 192.168.10.1:3260 --token SECRET

It keeps one ``bluetoothctl`` session open (agent NoInputNoOutput, so pairing
needs no confirmation) and, when it runs as root, a ``btmon`` beside it.  Every
line either of them prints goes into one numbered log.  The server listens on
127.0.0.1 by default, so anything sharing this machine's network namespace -
the development container included - can drive it with the same script.
--listen puts it on another address, such as a second machine's LAN side;
give it a --token then, and the clients the same token (--token or
HOSTBT_TOKEN), since the server runs as root.  Clients find it with --host
HOST:PORT or HOSTBT_HOST.

    tools/hostbt.py bt "scan on"                   send a bluetoothctl command
    tools/hostbt.py run "pair AA:.." --wait 'Pairing successful|Failed' -t 30
    tools/hostbt.py seq                            current log position
    tools/hostbt.py since SEQ [REGEX]              log lines after SEQ
    tools/hostbt.py wait SEQ REGEX [-t SECONDS]    block for a matching line
    tools/hostbt.py sniff HANDLE [SLOTS]           sniff mode, like a Switch
    tools/hostbt.py unsniff HANDLE
    tools/hostbt.py lephy HANDLE                   LE: ask for the 2M PHY
    tools/hostbt.py conninfo AA:..                 RSSI, TX power, link quality
    tools/hostbt.py watch [AA:..] [--gap-ms 30]    per-second link monitor
    tools/hostbt.py unwatch

watch reads the pad's hidraw node on the server (any Bluetooth HID device if
no address is given; it follows reconnections) and every second logs, with
tag W, the reports received, the longest gap between two, the gaps over the
threshold and the link's RSSI, TX power and link quality.  The server's
terminal shows the same as a line per second with bars, for the player.

Log lines read ``seq time tag text``; the tag is ``B`` for bluetoothctl, ``M``
for btmon, ``W`` for watch and ``S`` for the server itself.  Only
bluetoothctl commands can be sent, plus sniff/unsniff for one ACL handle (run
as a fixed ``hcitool cmd``) and fixed btmgmt/hcitool queries for a validated
address; nothing is passed to a shell.  Standard library only, so it runs with the
host's own python3.
"""

from __future__ import annotations

import argparse
import collections
import glob
import os
import pty
import re
import select
import shutil
import signal
import socket
import socketserver
import subprocess
import sys
import threading
import time

DEFAULT_PORT = int(os.environ.get("HOSTBT_PORT", "47807"))
DEFAULT_HOST = os.environ.get("HOSTBT_HOST", f"127.0.0.1:{DEFAULT_PORT}")
ADDRESS = re.compile(r"^[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}$")
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|[\x01\x02\r]")
PROMPT = re.compile(r"^(?:\[[^\]]*\](?:#|>) ?)+")
# bluetoothctl asks these even with a NoInputNoOutput agent in some versions.
AUTO_YES = re.compile(r"\((?:yes/no)\)")


class Log:
    def __init__(self, size: int) -> None:
        self.lines: collections.deque = collections.deque(maxlen=size)
        self.seq = 0
        self.cond = threading.Condition()

    def add(self, tag: str, text: str) -> None:
        with self.cond:
            self.seq += 1
            self.lines.append((self.seq, time.time(), tag, text))
            self.cond.notify_all()

    def since(self, seq: int, pattern: re.Pattern | None = None) -> list:
        with self.cond:
            return [line for line in self.lines
                    if line[0] > seq and (pattern is None or pattern.search(line[3]))]

    def wait(self, seq: int, pattern: re.Pattern, timeout: float):
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                for line in self.lines:
                    if line[0] > seq and pattern.search(line[3]):
                        return line
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                self.cond.wait(left)


def format_line(line) -> str:
    seq, stamp, tag, text = line
    clock = time.strftime("%H:%M:%S", time.localtime(stamp))
    return f"{seq} {clock}.{int(stamp * 1000) % 1000:03d} {tag} {text}"


class Child:
    """A program on a pty, so it line-buffers as it would in a terminal."""

    def __init__(self, log: Log, tag: str, argv: list[str],
                 on_line=None) -> None:
        self.log = log
        self.tag = tag
        self.argv = argv
        self.on_line = on_line
        self.master = -1
        self.process: subprocess.Popen | None = None
        self.lock = threading.Lock()

    def start(self) -> None:
        master, slave = pty.openpty()
        self.process = subprocess.Popen(self.argv, stdin=slave, stdout=slave,
                                        stderr=slave, close_fds=True,
                                        start_new_session=True)
        os.close(slave)
        self.master = master
        self.log.add("S", f"started {' '.join(self.argv)} pid {self.process.pid}")
        threading.Thread(target=self.read_loop, daemon=True).start()

    def read_loop(self) -> None:
        pending = ""
        while True:
            try:
                data = os.read(self.master, 4096)
            except OSError:
                break
            if not data:
                break
            pending += ANSI.sub("", data.decode("utf-8", "replace"))
            *lines, pending = pending.split("\n")
            # A question such as "(yes/no):" has no newline after it.
            if pending and AUTO_YES.search(pending):
                lines.append(pending)
                pending = ""
            for line in lines:
                line = PROMPT.sub("", line).rstrip()
                if not line:
                    continue
                self.log.add(self.tag, line)
                if self.on_line is not None:
                    self.on_line(self, line)
        self.log.add("S", f"{self.argv[0]} exited")

    def alive(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def write(self, line: str) -> None:
        with self.lock:
            os.write(self.master, (line + "\n").encode())

    def stop(self) -> None:
        if self.alive():
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(3)
            except subprocess.TimeoutExpired:
                self.process.kill()


def answer_questions(child: Child, line: str) -> None:
    if AUTO_YES.search(line):
        child.log.add("S", "answering yes")
        child.write("yes")


def run_fixed(argv: list[str], timeout: float = 3.0) -> str:
    """A fixed command (validated arguments, no shell); its output, or ''."""
    if shutil.which(argv[0]) is None:
        return ""
    try:
        result = subprocess.run(argv, capture_output=True, text=True,
                                timeout=timeout)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return result.stdout + result.stderr


def conn_info(address: str) -> dict:
    """RSSI and TX power (btmgmt conn-info) and link quality (hcitool lq) of
    the connection to ADDRESS; missing values are None."""
    info = {"rssi": None, "tx": None, "lq": None}
    for kind in ("0", "1", "2"):        # BR/EDR, LE public, LE random
        text = run_fixed(["btmgmt", "conn-info", "-t", kind, address])
        rssi = re.search(r"RSSI\s+(-?\d+)", text)
        if rssi:
            info["rssi"] = int(rssi.group(1))
            tx = re.search(r"TX power\s+(-?\d+)", text)
            info["tx"] = int(tx.group(1)) if tx else None
            break
    lq = re.search(r"Link quality:\s*(\d+)", run_fixed(["hcitool", "lq", address]))
    if lq:
        info["lq"] = int(lq.group(1))
    return info


def hidraw_nodes() -> list[tuple[str, str]]:
    """(node, address) of every Bluetooth HID device's hidraw node."""
    nodes = []
    for uevent in sorted(glob.glob("/sys/class/hidraw/hidraw*/device/uevent")):
        try:
            fields = dict(line.split("=", 1) for line in
                          open(uevent).read().splitlines() if "=" in line)
        except OSError:
            continue
        if not fields.get("HID_ID", "").startswith("0005:"):
            continue
        name = uevent.split("/")[4]
        nodes.append((f"/dev/{name}", fields.get("HID_UNIQ", "").lower()))
    return nodes


def bar(value: float, full: float, width: int = 12) -> str:
    filled = max(0, min(width, round(width * value / full))) if full else 0
    return "#" * filled + "." * (width - filled)


class Watch:
    """Per-second link monitor for one pad (see the module docstring)."""

    def __init__(self, log: Log, address: str | None, gap_ms: float) -> None:
        self.log = log
        self.address = address.lower() if address else None
        self.gap_ms = gap_ms
        self.running = True
        self.info = {"rssi": None, "tx": None, "lq": None}
        self.peer: str | None = None
        threading.Thread(target=self.read_loop, daemon=True).start()
        threading.Thread(target=self.info_loop, daemon=True).start()

    def stop(self) -> None:
        self.running = False

    def find(self) -> tuple[str, str] | None:
        for node, uniq in hidraw_nodes():
            if self.address is None or uniq == self.address:
                return node, uniq
        return None

    def info_loop(self) -> None:
        # Apart from the reader, so the queries never delay its timestamps.
        while self.running:
            peer = self.peer
            self.info = conn_info(peer) if peer and ADDRESS.match(peer) else \
                {"rssi": None, "tx": None, "lq": None}
            time.sleep(1.0)

    def read_loop(self) -> None:
        fd = -1
        node = None
        last = None
        second_end = time.monotonic() + 1.0
        count = 0
        worst = 0.0
        gaps = 0
        while self.running:
            if fd < 0:
                found = self.find()
                if found:
                    node, self.peer = found
                    try:
                        fd = os.open(node, os.O_RDONLY | os.O_NONBLOCK)
                        self.log.add("W", f"reading {node} ({self.peer})")
                        last = None
                    except OSError as error:
                        self.log.add("W", f"{node}: {error}")
                        fd = -1
            now = time.monotonic()
            if now >= second_end:
                if last is not None:
                    worst = max(worst, (now - last) * 1000.0)
                self.report(node if fd >= 0 else None, count, worst, gaps)
                second_end += 1.0
                if second_end < now:
                    second_end = now + 1.0
                count, worst, gaps = 0, 0.0, 0
                continue
            if fd < 0:
                time.sleep(min(0.2, second_end - now))
                continue
            ready, _, _ = select.select([fd], [], [], max(0.0, second_end - now))
            if not ready:
                continue
            try:
                os.read(fd, 1024)
            except BlockingIOError:
                continue
            except OSError:
                self.log.add("W", f"{node} gone (disconnected)")
                os.close(fd)
                fd = -1
                continue
            stamp = time.monotonic()
            if last is not None:
                gap = (stamp - last) * 1000.0
                worst = max(worst, gap)
                if gap > self.gap_ms:
                    gaps += 1
            last = stamp
            count += 1
        if fd >= 0:
            os.close(fd)
        self.log.add("W", "stopped")

    def report(self, node, count: int, worst: float, gaps: int) -> None:
        info = self.info
        rssi, tx, lq = info["rssi"], info["tx"], info["lq"]
        self.log.add("W", f"reports={count} gap_max_ms={worst:.1f} "
                          f"gaps_over_{self.gap_ms:.0f}ms={gaps} rssi={rssi} "
                          f"tx={tx} lq={lq} node={node}")
        if node is None:
            line = "no pad"
        else:
            rssi_text = f"{rssi:4d} dBm {bar(rssi + 100, 70)}" if rssi is not None \
                else "  RSSI n/a"
            line = (f"{count:4d}/s  longest gap {worst:6.1f} ms "
                    f"{bar(min(worst, 200.0), 200.0)}  over {self.gap_ms:.0f} ms: "
                    f"{gaps:3d}  {rssi_text}"
                    + (f"  LQ {lq}" if lq is not None else ""))
        print(time.strftime("%H:%M:%S ") + line, flush=True)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address, log: Log, ctl: Child, token: str) -> None:
        super().__init__(address, Handler)
        self.log = log
        self.ctl = ctl
        self.token = token
        self.watch: Watch | None = None


class Handler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        request = self.rfile.readline().decode("utf-8", "replace").rstrip("\n")
        if self.server.token:
            given, _, request = request.partition(" ")
            if given != "@" + self.server.token:
                self.wfile.write(b"error bad token\n")
                return
        verb, _, rest = request.partition(" ")
        log: Log = self.server.log
        ctl: Child = self.server.ctl
        try:
            if verb == "bt":
                if not ctl.alive():
                    ctl.start()
                    for line in ("agent NoInputNoOutput", "default-agent"):
                        ctl.write(line)
                seq = log.seq
                log.add("S", f"> {rest}")
                ctl.write(rest)
                reply = [f"ok {seq}"]
            elif verb in ("sniff", "unsniff"):
                # The only raw HCI this server sends: put the ACL with the
                # given handle into (or out of) sniff mode, the way a Switch
                # does right after connecting.  Arguments are validated as
                # numbers; nothing reaches a shell.
                args = rest.split()
                handle = int(args[0], 0)
                if not 0 <= handle <= 0x0eff:
                    raise ValueError("handle out of range")
                hl = ["0x%02x" % (handle & 0xff), "0x%02x" % (handle >> 8)]
                if verb == "sniff":
                    slots = int(args[1], 0) if len(args) > 1 else 24
                    if not 2 <= slots <= 0x800:
                        raise ValueError("interval out of range")
                    iv = ["0x%02x" % (slots & 0xff), "0x%02x" % (slots >> 8)]
                    cmd = ["hcitool", "cmd", "0x02", "0x0003", *hl, *iv, *iv,
                           "0x01", "0x00", "0x00", "0x00"]
                else:
                    cmd = ["hcitool", "cmd", "0x02", "0x0004", *hl]
                log.add("S", "> " + " ".join(cmd))
                result = subprocess.run(cmd, capture_output=True, text=True,
                                        timeout=5)
                for line in (result.stdout + result.stderr).splitlines():
                    log.add("S", line)
                reply = [f"ok {log.seq} rc={result.returncode}"]
            elif verb == "lephy":
                # LE Set PHY: prefer 2M both ways on one LE connection handle
                # (fixed hcitool form, validated number, no shell).
                handle = int(rest.split()[0], 0)
                if not 0 <= handle <= 0x0eff:
                    raise ValueError("handle out of range")
                cmd = ["hcitool", "cmd", "0x08", "0x0032",
                       "0x%02x" % (handle & 0xff), "0x%02x" % (handle >> 8),
                       "0x00", "0x02", "0x02", "0x00", "0x00"]
                log.add("S", "> " + " ".join(cmd))
                result = subprocess.run(cmd, capture_output=True, text=True,
                                        timeout=5)
                for line in (result.stdout + result.stderr).splitlines():
                    log.add("S", line)
                reply = [f"ok {log.seq} rc={result.returncode}"]
            elif verb == "conninfo":
                address = rest.strip()
                if not ADDRESS.match(address):
                    raise ValueError("not an address")
                info = conn_info(address)
                reply = [f"rssi={info['rssi']} tx={info['tx']} lq={info['lq']}"]
            elif verb == "watch":
                args = rest.split()
                address = None
                gap_ms = 30.0
                for arg in args:
                    if ADDRESS.match(arg):
                        address = arg
                    else:
                        gap_ms = float(arg)
                if self.server.watch is not None:
                    self.server.watch.stop()
                self.server.watch = Watch(log, address, gap_ms)
                log.add("S", f"watch {address or 'any Bluetooth HID'} "
                             f"gaps over {gap_ms:.0f} ms")
                reply = [f"ok {log.seq}"]
            elif verb == "unwatch":
                if self.server.watch is not None:
                    self.server.watch.stop()
                    self.server.watch = None
                reply = [f"ok {log.seq}"]
            elif verb == "seq":
                reply = [str(log.seq)]
            elif verb == "since":
                seq, _, pattern = rest.partition(" ")
                regex = re.compile(pattern) if pattern else None
                reply = [format_line(line) for line in log.since(int(seq), regex)]
            elif verb == "wait":
                seq, timeout, pattern = rest.split(" ", 2)
                line = log.wait(int(seq), re.compile(pattern), float(timeout))
                reply = [format_line(line) if line else "timeout"]
            else:
                reply = [f"error unknown request {verb!r}"]
        except (ValueError, re.error) as error:
            reply = [f"error {error}"]
        self.wfile.write(("\n".join(reply) + "\n").encode())


def serve(args) -> None:
    log = Log(args.lines)
    ctl = Child(log, "B", [args.bluetoothctl], on_line=answer_questions)
    ctl.start()
    for line in ("power on", "agent NoInputNoOutput", "default-agent"):
        ctl.write(line)
    monitors = []
    if os.geteuid() == 0 and not args.no_btmon:
        monitors.append(Child(log, "M", ["btmon"]))
        if args.btsnoop:
            monitors.append(Child(log, "S", ["btmon", "-w", args.btsnoop]))
        for monitor in monitors:
            monitor.start()
    else:
        log.add("S", "btmon not started (needs root, or --no-btmon given)")
    host, _, port = args.listen.rpartition(":")
    if host not in ("127.0.0.1", "localhost", "::1") and not args.token:
        print("hostbt: --listen off this machine needs --token", file=sys.stderr)
        sys.exit(2)
    for tool in ("btmgmt", "hcitool"):
        if shutil.which(tool) is None:
            log.add("S", f"{tool} not found: conninfo/watch go without it")
            print(f"hostbt: {tool} not found", flush=True)
    server = Server((host, int(port)), log, ctl, args.token)
    print(f"hostbt: listening on {host}:{port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        for child in [ctl, *monitors]:
            child.stop()


TARGET = ("127.0.0.1", DEFAULT_PORT)
TOKEN = ""


def request(port: int, line: str, timeout: float) -> str:
    del port    # kept for the call sites; the target is TARGET
    if TOKEN:
        line = f"@{TOKEN} {line}"
    with socket.create_connection(TARGET, timeout=timeout) as sock:
        sock.sendall((line + "\n").encode())
        chunks = []
        while True:
            data = sock.recv(65536)
            if not data:
                break
            chunks.append(data)
    return b"".join(chunks).decode("utf-8", "replace")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", type=int, default=None,
                        help="server port on 127.0.0.1 (older form of --host)")
    parser.add_argument("--host", default=DEFAULT_HOST,
                        help="server HOST:PORT (HOSTBT_HOST)")
    parser.add_argument("--token", default=os.environ.get("HOSTBT_TOKEN", ""),
                        help="shared secret the server was started with "
                             "(HOSTBT_TOKEN)")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("serve", help="run the host side (use sudo for btmon)")
    p.add_argument("--btsnoop", help="also write a btsnoop capture here")
    p.add_argument("--no-btmon", action="store_true")
    p.add_argument("--lines", type=int, default=200000,
                   help="how many log lines to keep")
    p.add_argument("--bluetoothctl", default="bluetoothctl",
                   help=argparse.SUPPRESS)
    p.add_argument("--listen", default=f"127.0.0.1:{DEFAULT_PORT}",
                   help="HOST:PORT to listen on (default 127.0.0.1:%(default)s); "
                        "anything else needs --token")
    p.add_argument("--token", default=os.environ.get("HOSTBT_TOKEN", ""),
                   help="shared secret clients must send (HOSTBT_TOKEN)")

    p = sub.add_parser("sniff", help="put ACL HANDLE into sniff mode")
    p.add_argument("handle")
    p.add_argument("slots", nargs="?", default="24",
                   help="sniff interval in 0.625 ms slots (Switch: 24)")
    p = sub.add_parser("unsniff", help="take ACL HANDLE out of sniff mode")
    p.add_argument("handle")
    p = sub.add_parser("lephy", help="ask for the 2M PHY on LE HANDLE")
    p.add_argument("handle")
    p = sub.add_parser("conninfo", help="RSSI, TX power, link quality of ADDR")
    p.add_argument("address")
    p = sub.add_parser("watch", help="per-second link monitor on the server")
    p.add_argument("address", nargs="?", default="")
    p.add_argument("--gap-ms", type=float, default=30.0)
    sub.add_parser("unwatch", help="stop the link monitor")
    p = sub.add_parser("bt", help="send one bluetoothctl command")
    p.add_argument("line")
    p = sub.add_parser("run", help="send a command and wait for a reply line")
    p.add_argument("line")
    p.add_argument("--wait", required=True, help="regex to wait for")
    p.add_argument("-t", "--timeout", type=float, default=15.0)
    sub.add_parser("seq", help="print the current log position")
    p = sub.add_parser("since", help="print log lines after SEQ")
    p.add_argument("seq", type=int)
    p.add_argument("regex", nargs="?", default="")
    p = sub.add_parser("wait", help="wait for a log line matching REGEX")
    p.add_argument("seq", type=int)
    p.add_argument("regex")
    p.add_argument("-t", "--timeout", type=float, default=15.0)

    args = parser.parse_args()
    if args.command == "serve":
        serve(args)
        return
    global TARGET, TOKEN
    if args.port is not None:
        TARGET = ("127.0.0.1", args.port)
    else:
        host, _, port = args.host.rpartition(":")
        TARGET = (host, int(port))
    TOKEN = args.token
    if args.command == "conninfo":
        sys.stdout.write(request(0, f"conninfo {args.address}", 10))
        return
    if args.command == "watch":
        sys.stdout.write(request(0, f"watch {args.address} {args.gap_ms}".replace("  ", " "), 5))
        return
    if args.command == "unwatch":
        sys.stdout.write(request(0, "unwatch", 5))
        return
    if args.command == "bt":
        reply = request(args.port, f"bt {args.line}", 5)
    elif args.command == "run":
        start = request(args.port, f"bt {args.line}", 5).split()[1]
        reply = request(args.port, f"wait {start} {args.timeout} {args.wait}",
                        args.timeout + 5)
        if reply.strip() == "timeout":
            sys.stdout.write(request(args.port, f"since {start} ^", 5))
            sys.stdout.write("timeout\n")
            sys.exit(1)
    elif args.command == "sniff":
        reply = request(args.port, f"sniff {args.handle} {args.slots}", 10)
    elif args.command == "unsniff":
        reply = request(args.port, f"unsniff {args.handle}", 10)
    elif args.command == "lephy":
        reply = request(args.port, f"lephy {args.handle}", 10)
    elif args.command == "seq":
        reply = request(args.port, "seq", 5)
    elif args.command == "since":
        reply = request(args.port, f"since {args.seq} {args.regex}".rstrip(), 30)
    else:
        reply = request(args.port, f"wait {args.seq} {args.timeout} {args.regex}",
                        args.timeout + 5)
        if reply.strip() == "timeout":
            sys.stdout.write(reply)
            sys.exit(1)
    sys.stdout.write(reply)


if __name__ == "__main__":
    main()
