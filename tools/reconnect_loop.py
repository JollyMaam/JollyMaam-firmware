#!/usr/bin/env python3
"""Capture a Switch bond and exercise reconnects through the padbus daemon.

The capture saves the slot's bond as `export` lines (tools/padbond.py) in a
protected file.  The padbus log can contain HCI link-key packets, so it is
chmod'd to 0600 before the capture starts.  The loop keeps padbus as the sole
serial owner and imports the saved bond - which carries the pad's address -
at startup and after each chip reset.

Examples:
    tools/reconnect_loop.py capture --output /work/nslog/switch-golden.bond
    tools/reconnect_loop.py loop --bond /work/nslog/switch-golden.bond --cycles 20

Run `capture` while the Switch is in Change Grip/Order.  It uses `pair 0`
(without `--fresh`) so the slot keeps its derived address.
After the script saves the bond, leave the Switch on HOME and run `loop`.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import socket
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import padbond  # noqa: E402
import padbus  # noqa: E402


CLIENT = "reconnect-loop"
ADDRESS = r"[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}"


class Session:
    def __init__(self, socket_path: str, client: str = CLIENT):
        self.sock = padbus.client_socket(socket_path)
        self.client = client
        hello = padbus.request(self.sock, op="hello", client=client)
        if not hello.get("ok", True):
            raise RuntimeError(f"padbus hello failed: {hello}")
        reply = padbus.request(self.sock, op="request", client=client)
        if not reply.get("ok"):
            raise RuntimeError("padbus control is held by " +
                               str(reply.get("owner", "another client")))

    def close(self):
        try:
            padbus.request(self.sock, op="release", client=self.client)
        finally:
            self.sock.close()

    def lines(self):
        # HCI tracing can append dozens of lines between polls. Keep enough
        # history that the short Link Key Request event is not pushed out of
        # the server's tail before we can snapshot its decision markers.
        reply = padbus.request(self.sock, op="tail", n=1000)
        return reply.get("log", [])

    def send(self, command: str) -> str:
        reply = padbus.request(self.sock, op="send", client=self.client,
                               line=command)
        if not reply.get("ok"):
            raise RuntimeError("padbus refused command: " +
                               str(reply.get("error", reply)))
        return f">>> [{self.client}] {command}"

    def after(self, marker: str) -> list[str]:
        lines = self.lines()
        positions = [i for i, line in enumerate(lines) if line == marker]
        return lines[positions[-1] + 1:] if positions else []

    def wait(self, marker: str, timeout: float, success=(), failure=()):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            lines = self.after(marker)
            for line in lines:
                if any(pattern in line for pattern in success):
                    return "success", line, lines
                if any(pattern in line for pattern in failure):
                    return "failure", line, lines
            time.sleep(0.25)
        return "timeout", "", self.after(marker)

    def cmd(self, command: str, timeout: float = 10.0) -> list[str]:
        """Run a command and wait for it; its output lines (raises on ERR)."""
        reply = padbus.request(self.sock, op="cmd", client=self.client,
                               line=command, timeout=timeout)
        if not reply.get("ok"):
            raise RuntimeError(f"{command!r}: {reply.get('error', reply)}")
        return reply.get("lines", [])

    def command(self, command: str, expected: str, timeout: float = 4.0):
        marker = self.send(command)
        result, line, lines = self.wait(marker, timeout,
                                        success=(expected,))
        if result != "success":
            raise RuntimeError(f"{command!r} did not return {expected!r}: "
                               + " | ".join(lines[-6:]))
        return lines


def capture_marker_ring(session: Session, label: str) -> bool:
    """Print only SRAM marker words from `debug bringup`, never HCI packet bytes."""
    marker = session.send("debug bringup")
    deadline = time.monotonic() + 0.8
    while time.monotonic() < deadline:
        for line in session.after(marker):
            if not line.startswith("reset-mailbox="):
                continue
            match = re.search(
                r"bt-log[0-9a-fA-F]+/[0-9a-fA-F]+:(.*?)\s+nvram=", line)
            if match:
                words = re.findall(
                    r"(?<![0-9a-fA-F])([0-9a-fA-F]{8})"
                    r"(?![0-9a-fA-F])", match.group(1))
                print(f"{label} markers: " + " ".join(
                    word[:4] for word in words), flush=True)
                return True
        time.sleep(0.02)
    return False


def wait_link_result(session: Session, marker: str, timeout: float):
    """Poll quickly and snapshot around link setup and the key request."""
    deadline = time.monotonic() + timeout
    snapshots = set()
    while time.monotonic() < deadline:
        lines = session.after(marker)
        triggers = (
            ("connected", "CONNECTED_PEER_EVENT"),
            ("link-key-request", "HCI t4 < 1706"),
        )
        for label, trigger in triggers:
            if label in snapshots:
                continue
            if trigger == "CONNECTED_PEER_EVENT":
                matched = any(re.search(r"\bconnected peer\b", line) and
                              "disconnected peer" not in line
                              for line in lines)
            else:
                matched = any(trigger in line for line in lines)
            if not matched:
                continue
            snapshots.add(label)
            try:
                if not capture_marker_ring(session, label):
                    print(f"{label} markers: unavailable",
                          flush=True)
            except RuntimeError:
                print(f"{label} markers: unavailable",
                      flush=True)
        for line in lines:
            if "BTSTACK: hid channels open" in line:
                return "success", line, lines
            if any(pattern in line for pattern in (
                    "link key rejected", "disconnected peer",
                    "# RESET: boot counter")):
                return "failure", line, lines
        time.sleep(0.03)
    return "timeout", "", session.after(marker)


def protect_log(path: str):
    try:
        os.chmod(path, 0o600)
    except FileNotFoundError:
        pass


def capture(args):
    protect_log(args.log)
    session = Session(args.socket)
    try:
        session.command("config log.hcidump=true", "OK config log.hcidump=true")
        pair_command = f"pair {args.slot}" + (" --fresh" if args.fresh else "")
        marker = session.send(pair_command)
        print("Pairing started; waiting for an encrypted Switch link.",
              flush=True)
        result, line, pairing_lines = session.wait(
            marker, args.timeout,
            success=("encrypted peer ",),
            failure=("link key rejected", "# RESET: boot counter"))
        if result != "success":
            raise RuntimeError("pairing did not complete; " +
                               (line or "timed out"))

        # The key reaches the slot's bond with the encryption; let the rest of
        # the exchange land before reading it back.
        time.sleep(2.0)
        lines = [line for line in session.cmd(f"config get --changed profile.{args.slot}")
                 if line.startswith("config ")]
        key = next((line.split("=", 1)[1].strip() for line in lines
                    if ".bond.link_key" in line), None)
        if not key:
            raise RuntimeError("pair completed but no key was stored")
        padbond.write(args.output, lines)
        fingerprint = hashlib.sha256(bytes.fromhex(key)).hexdigest()[:16]
        print(f"Saved slot {args.slot}: {len(lines)} settings, key "
              f"SHA256[:16]={fingerprint}", flush=True)
        print(f"Protected bond file: {args.output} (mode 0600)", flush=True)
        print("Leave CGO for HOME, then start the reconnect loop.", flush=True)
        session.command("config log.hcidump=false", "OK config log.hcidump=false")
    finally:
        session.close()


def query_state(session: Session) -> str:
    try:
        lines = session.cmd("debug stat")
    except RuntimeError:
        return "unknown"
    for line in lines:
        match = re.search(r"\bstate=(\w+)", line)
        if match:
            return match.group(1)
    return "unknown"


def restore(session: Session, bond, slot: int, set_address: bool):
    """Import the saved bond (its address included) and page the console."""
    session.command("config log.hcidump=true", "OK config log.hcidump=true")
    for line in bond:
        if set_address or not re.match(r"config profile\.\w+\.address", line):
            session.cmd(line)
    marker = session.send(f"connect {slot}")
    result, line, _ = wait_link_result(session, marker, 12.0)
    return result, line, marker


def reconnect_attempt(session: Session, bond, slot: int, set_address: bool,
    timeout: float):
    if set_address:
        return restore(session, bond, slot, True)

    state = query_state(session)
    if state != "connected":
        return restore(session, bond, slot, False)

    marker = session.send("disconnect")
    session.wait(marker, 5.0,
                 success=("disconnected peer", "OK already disconnected"),
                 failure=("# RESET: boot counter",))

    session.command("config log.hcidump=true", "OK config log.hcidump=true")
    marker = session.send(f"connect {slot}")
    result, line, _ = wait_link_result(session, marker, timeout)
    return result, line, marker


def loop(args):
    protect_log(args.log)
    bond = padbond.read(args.bond, args.slot)
    key = next((line.split("=", 1)[1].strip() for line in bond
                if ".bond.link_key" in line), "")
    fingerprint = hashlib.sha256(bytes.fromhex(key)).hexdigest()[:16] if key else "-"
    print(f"Using saved bond ({len(bond)} settings), key SHA256[:16]="
          f"{fingerprint}", flush=True)
    session = Session(args.socket)
    # Start each run from the protected record so the local address and peer
    # slot do not depend on what survived in flash (or in a RAM-mode store).
    needs_address = not args.already_restored
    try:
        for cycle in range(1, args.cycles + 1):
            result, line, marker = reconnect_attempt(
                session, bond, args.slot, needs_address, args.timeout)
            if result == "success":
                print(f"attempt {cycle}: encrypted; HID channels opened",
                      flush=True)
                # Stop the high-volume packet trace between attempts.  It is
                # enabled again immediately before the next reconnect.
                try:
                    session.command("config log.hcidump=false", "OK config log.hcidump=false")
                except RuntimeError:
                    pass
                needs_address = False
                continue

            if result == "failure":
                print(f"attempt {cycle}: disconnected: {line}", flush=True)
                # The fast snapshot above normally catches this earlier. Try
                # once more in case this failure used a different path.
                try:
                    if not capture_marker_ring(session, "failure"):
                        print("failure markers: unavailable before reset",
                              flush=True)
                except RuntimeError:
                    print("failure markers: unavailable before reset",
                          flush=True)
            else:
                print(f"attempt {cycle}: reconnect timed out", flush=True)

            # The console daemon reports the boot-counter change after a chip
            # reset.  Look only at lines after this attempt's command so a
            # previous reset cannot trigger a premature address restore.
            deadline = time.monotonic() + 8.0
            reset_seen = False
            while time.monotonic() < deadline:
                attempt_lines = session.after(marker)
                if any(line.startswith("# RESET: boot counter")
                       for line in attempt_lines):
                    reset_seen = True
                    break
                time.sleep(0.25)
            needs_address = reset_seen
    finally:
        session.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", default=padbus.DEFAULT_SOCKET)
    parser.add_argument("--log", default=padbus.DEFAULT_LOG,
                        help="padbus log path (chmod'd to 0600)")
    sub = parser.add_subparsers(dest="mode", required=True)
    capture_parser = sub.add_parser("capture", help="pair once and save a bond file")
    capture_parser.add_argument("--output", required=True,
                                help="new bond file path; existing files are kept")
    capture_parser.add_argument("--slot", type=int, default=0)
    capture_parser.add_argument("--fresh", action="store_true",
                                help="pair under a new local Bluetooth address")
    capture_parser.add_argument("--timeout", type=float, default=180.0)
    loop_parser = sub.add_parser("loop", help="repeat disconnect/reconnect attempts")
    loop_parser.add_argument("--bond", required=True)
    loop_parser.add_argument("--slot", type=int, default=0)
    loop_parser.add_argument("--cycles", type=int, default=20)
    loop_parser.add_argument("--timeout", type=float, default=12.0)
    loop_parser.add_argument("--already-restored", action="store_true",
                             help="use the bond and local address already loaded in RAM")
    args = parser.parse_args()

    try:
        if args.mode == "capture":
            capture(args)
        else:
            loop(args)
    except (OSError, RuntimeError, socket.error) as error:
        print(f"reconnect_loop: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
