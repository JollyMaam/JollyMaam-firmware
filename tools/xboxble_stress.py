#!/usr/bin/env python3
"""Stress the xboxble profile against a Linux host driven through hostbt.py.

Each cycle breaks the link one way, waits for it to come back, and then proves
that input reaches the host: it makes the pad press A (`debug inject`) and
looks for an input-report notification with A set in the host's btmon
trace.  A link that reconnects but never encrypts (so the host's HID driver
never gets input) fails the cycle.

Ways to break the link, picked in turn or with --only:

  padcycle   pad `disconnect`, then `connect SLOT` (restarts the pad's stack)
  hostdrop   the host disconnects; the pad re-advertises, the host reconnects
  reboot     pad `reboot`, then `connect SLOT`
  repair     host forgets the pad, pad `pair SLOT`, host pairs again
  dwell      stay connected --dwell seconds; no disconnect allowed

Needs: `tools/padbus.py serve` (this takes its control lock) and
`hostbt.py serve` on the host (run as root so btmon is in its log).  The pad
must be bonded with the host in SLOT already, unless the first step is
`repair`.  Turns on log.console for the run (the pad's LE lines are how a
failure gets its cause) and restores it afterwards.  Leave log.hcidump off:
its per-packet output resets the pad (12 of 15 pairing-mode reconnects).

    tools/xboxble_stress.py --slot b --cycles 20
    tools/xboxble_stress.py --only repair,padcycle --cycles 6
"""

from __future__ import annotations

import argparse
import itertools
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostbt  # noqa: E402
import padbus  # noqa: E402

CLIENT = "xboxble-stress"
INPUT_HANDLE = 0x001D
BUTTON_A = (13, 0x01)
MODES = ("padcycle", "hostdrop", "reboot", "repair", "dwell")


class Host:
    def __init__(self, port: int):
        self.port = port

    def req(self, line: str, timeout: float = 10.0) -> str:
        return hostbt.request(self.port, line, timeout)

    def seq(self) -> int:
        return int(self.req("seq").split()[0])

    def bt(self, line: str) -> int:
        return int(self.req("bt " + line).split()[1])

    def since(self, seq: int, regex: str = "") -> list[str]:
        reply = self.req(f"since {seq} {regex}".rstrip(), 30)
        return [line for line in reply.splitlines() if line.strip()]

    def wait(self, seq: int, regex: str, timeout: float) -> str | None:
        reply = self.req(f"wait {seq} {timeout} {regex}", timeout + 5).strip()
        return None if reply == "timeout" else reply

    def notifications(self, seq: int):
        """(seq, data) for every input-report notification after seq."""
        lines = self.since(seq)
        out = []
        state = 0
        for line in lines:
            text = line.split(" ", 3)[3] if line.count(" ") >= 3 else ""
            if "ATT: Handle Value Notification" in text:
                state = 1
            elif state == 1 and text.strip().startswith("Handle:"):
                m = re.match(r"\s*Handle: 0x([0-9a-fA-F]+)", text)
                state = 2 if m and int(m.group(1), 16) == INPUT_HANDLE else 0
            elif state == 2 and text.strip().startswith("Data["):
                state = 3
            elif state == 3:
                hexpart = text.strip()[:47].split()
                try:
                    out.append((int(line.split()[0]),
                                bytes(int(b, 16) for b in hexpart)))
                except ValueError:
                    pass
                state = 0
        return out


class Pad:
    def __init__(self, socket_path: str):
        self.sock = padbus.client_socket(socket_path)
        padbus.request(self.sock, op="hello", client=CLIENT)
        reply = padbus.request(self.sock, op="request", client=CLIENT)
        if not reply.get("ok"):
            raise SystemExit("padbus control is held by %s; release it "
                             "(pad_tui) and retry" % reply.get("owner"))

    def close(self):
        padbus.request(self.sock, op="release", client=CLIENT)
        self.sock.close()

    def cmd(self, line: str, timeout: float = 10.0) -> list[str]:
        reply = padbus.request(self.sock, op="cmd", client=CLIENT, line=line,
                               timeout=timeout)
        if not reply.get("ok"):
            raise RuntimeError(f"{line!r}: {reply.get('error', reply)}")
        return reply.get("lines", [])

    def send(self, line: str):
        padbus.request(self.sock, op="send", client=CLIENT, line=line)

    def log(self, n: int = 2000) -> list[str]:
        return padbus.request(self.sock, op="tail", n=n).get("log", [])

    def after_last(self, command: str) -> list[str]:
        """Pad log lines after the last time this client sent `command`."""
        lines = self.log(400)
        echo = f">>> [{CLIENT}] {command}"
        marks = [i for i, l in enumerate(lines) if l == echo]
        return lines[marks[-1] + 1:] if marks else []

    def stat(self) -> dict:
        for _ in range(40):
            try:
                for line in self.cmd("debug stat", 3):
                    if line.startswith("stat "):
                        return dict(f.split("=", 1) for f in line.split()[1:]
                                    if "=" in f)
            except RuntimeError:
                pass
            time.sleep(0.5)
        raise RuntimeError("pad console does not answer")


class Stress:
    def __init__(self, args):
        self.args = args
        self.host = Host(args.hostbt_port)
        self.pad = Pad(args.socket)
        self.slot = args.slot.upper()
        self.addr = None
        self.boots = None
        self.results: list[tuple[str, str, float]] = []

    # -- helpers -----------------------------------------------------------
    def note(self, text: str):
        print(time.strftime("%H:%M:%S ") + text, flush=True)

    def setup(self):
        for line in self.pad.cmd(f"config profile.{self.slot}"):
            m = re.search(r"\.address=([0-9a-f:]{17})", line)
            if m:
                self.addr = m.group(1).upper()
        if not self.addr:
            raise SystemExit("cannot read the slot's address")
        self.console_was = next((l.split("=")[1].split()[0] for l in
                                 self.pad.cmd("config log.console")
                                 if "=" in l), "true")
        self.pad.cmd("config log.console=true")
        self.boots = self.pad.stat().get("boots")
        self.note(f"slot {self.slot} address {self.addr} boots {self.boots}")

    def check_boots(self, expect_reset: bool = False):
        boots = self.pad.stat().get("boots")
        if boots != self.boots and not expect_reset:
            self.boots = boots
            raise RuntimeError(f"pad reset unexpectedly (boots {boots})")
        self.boots = boots

    def host_connected(self, seq: int, timeout: float) -> bool:
        return self.host.wait(
            seq, rf"\[CHG\] Device {self.addr} Connected: yes", timeout) is not None

    def input_works(self, timeout: float) -> float:
        """Seconds until an injected A press shows up on the host, or raise."""
        start = time.monotonic()
        deadline = start + timeout
        while time.monotonic() < deadline:
            seq = self.host.seq()
            self.pad.cmd("debug inject a 100 600")
            time.sleep(0.9)
            index, mask = BUTTON_A
            for _, data in self.host.notifications(seq):
                if len(data) > index and data[index] & mask:
                    return time.monotonic() - start
        raise RuntimeError("no A press reached the host")

    def pad_le_lines(self, since_marker: str) -> list[str]:
        lines = self.pad.log()
        try:
            i = max(i for i, l in enumerate(lines) if since_marker in l)
        except ValueError:
            i = 0
        keep = ("LE ", "xbox-ble", "BT2", "disconnected", "start slot",
                "BTSTACK:")
        return [l for l in lines[i:] if any(k in l for k in keep)]

    def host_summary(self, seq: int) -> list[str]:
        pat = (r"Encryption Change|Encryption: |Disconnect Complete|Reason:|"
               r"LTK|Long Term Key|Pairing Failed|Security Request|"
               rf"Device {self.addr} (Connected|Paired|Bonded)|"
               r"Connection Complete|Failed")
        return self.host.since(seq, pat)[-25:]

    # -- ways to break the link --------------------------------------------
    def padcycle(self):
        self.pad.cmd("disconnect")
        time.sleep(self.args.gap)
        seq = self.host.seq()
        self.pad.cmd(f"connect {self.slot}")
        return seq

    def hostdrop(self):
        seq = self.host.seq()
        self.host.bt(f"disconnect {self.addr}")
        if not self.host.wait(seq, rf"Device {self.addr} Connected: no", 10):
            raise RuntimeError("host did not disconnect")
        seq = self.host.seq()
        time.sleep(self.args.gap)
        # BlueZ reconnects a bonded HID device on its own when it advertises;
        # ask as well, in case passive scanning is slow.
        if not self.host_connected(seq, 8):
            self.host.bt(f"connect {self.addr}")
        return seq

    def reboot(self):
        self.pad.send("reboot")
        time.sleep(3)
        self.pad.stat()
        self.check_boots(expect_reset=True)
        seq = self.host.seq()
        self.pad.cmd(f"connect {self.slot}")
        return seq

    def repair(self):
        # Pad first, the way a person re-pairs: otherwise the host can find
        # the old session's reconnect advertisement and pair with the stack
        # that `pair` is about to replace.
        self.pad.cmd(f"pair {self.slot}")
        deadline = time.monotonic() + 10
        while not any("pairing advertisement" in l
                      for l in self.pad.after_last(f"pair {self.slot}")):
            if time.monotonic() > deadline:
                raise RuntimeError("pad never started its pairing advertisement")
            time.sleep(0.2)
        seq = self.host.seq()
        self.host.bt(f"remove {self.addr}")
        self.host.wait(seq, rf"\[DEL\] Device {self.addr}|not available", 5)
        seq = self.host.seq()
        self.host.bt("scan le")
        found = self.host.wait(seq, rf"\[NEW\] Device {self.addr}", 20)
        self.host.bt("scan off")
        if not found:
            raise RuntimeError("host never saw the pairing advertisement")
        seq = self.host.seq()
        self.host.bt(f"pair {self.addr}")
        result = self.host.wait(
            seq, r"Pairing successful|Failed to pair|AuthenticationFailed|"
            r"AuthenticationCanceled|ConnectionAttemptFailed|not available", 30)
        if not result or "successful" not in result:
            raise RuntimeError(f"pairing failed: {result}")
        self.host.bt(f"trust {self.addr}")
        return seq

    def dwell(self):
        seq = self.host.seq()
        self.note(f"  dwelling {self.args.dwell:.0f} s")
        time.sleep(self.args.dwell)
        drops = self.host.since(seq, rf"Device {self.addr} Connected: no")
        if drops:
            raise RuntimeError(f"link dropped while idle: {drops[0]}")
        return None

    # -- run ---------------------------------------------------------------
    def cycle(self, n: int, mode: str):
        marker = f">>> [{CLIENT}] "
        started = time.monotonic()
        seq0 = self.host.seq()
        self.pad.send("status radio.state")  # a marker in the pad log
        pad_marker = self.pad.log(5)[-1] if self.pad.log(5) else ""
        try:
            seq = getattr(self, mode)()
            if seq is not None and not self.host_connected(seq, self.args.timeout):
                raise RuntimeError("host never reconnected")
            took = self.input_works(self.args.timeout)
            if mode != "reboot":
                self.check_boots()
            elapsed = time.monotonic() - started
            self.results.append((mode, "ok", elapsed))
            self.note(f"#{n} {mode}: ok, input after {took:.1f} s "
                      f"(cycle {elapsed:.1f} s)")
            return True
        except Exception as error:  # report and keep going
            elapsed = time.monotonic() - started
            self.results.append((mode, str(error), elapsed))
            self.note(f"#{n} {mode}: FAIL - {error}")
            for line in self.host_summary(seq0):
                print("    host " + line)
            for line in self.pad_le_lines(pad_marker or marker)[-30:]:
                print("    pad  " + line)
            return False

    def run(self):
        self.setup()
        modes = self.args.only.split(",") if self.args.only else list(MODES)
        for mode in modes:
            if mode not in MODES:
                raise SystemExit(f"unknown mode {mode}")
        order = itertools.cycle(modes)
        failures = 0
        for n in range(1, self.args.cycles + 1):
            if not self.cycle(n, next(order)):
                failures += 1
                if self.args.stop:
                    break
                self.recover()
        summary = {}
        for mode, result, _ in self.results:
            ok, total = summary.get(mode, (0, 0))
            summary[mode] = (ok + (result == "ok"), total + 1)
        self.note("summary: " + ", ".join(
            f"{m} {ok}/{total}" for m, (ok, total) in summary.items()))
        return failures

    def recover(self):
        """Get back to a connected, working link before the next cycle."""
        self.note("  recovering")
        try:
            seq = self.padcycle()
            if self.host_connected(seq, self.args.timeout):
                self.input_works(self.args.timeout)
                return
        except Exception as error:
            self.note(f"  plain reconnect did not recover: {error}")
        try:
            seq = self.repair()
            self.host_connected(seq, self.args.timeout)
            self.input_works(self.args.timeout)
        except Exception as error:
            self.note(f"  re-pair did not recover: {error}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--slot", default="b")
    parser.add_argument("--cycles", type=int, default=10)
    parser.add_argument("--only", help="comma-separated modes: " + ",".join(MODES))
    parser.add_argument("--timeout", type=float, default=20.0,
                        help="seconds to reconnect and to see input")
    parser.add_argument("--gap", type=float, default=1.0,
                        help="seconds between breaking and restoring")
    parser.add_argument("--dwell", type=float, default=60.0)
    parser.add_argument("--stop", action="store_true",
                        help="stop at the first failure instead of recovering")
    parser.add_argument("--socket", default=padbus.DEFAULT_SOCKET
                        if hasattr(padbus, "DEFAULT_SOCKET") else "/tmp/padbus.sock")
    parser.add_argument("--hostbt-port", type=int, default=hostbt.DEFAULT_PORT)
    args = parser.parse_args()
    stress = Stress(args)
    try:
        failures = stress.run()
    finally:
        if getattr(stress, "console_was", "true") != "true":
            stress.pad.cmd("config log.console=" + stress.console_was)
        stress.pad.close()
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
