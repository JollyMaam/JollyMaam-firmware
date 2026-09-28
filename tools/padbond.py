"""Bond files: saving a slot's bond off the pad and putting it back.

A bond file is what `config export profile.A` prints - `config.PATH = VALUE`
lines, secret keys included - commands that, pasted back, restore it
takes.  Files from before the config rework hold one line instead,

    BOND local=aa:.. peer=aa:.. type=4 key=<32 hex>

and are read as the same four settings.  Either kind can be loaded into any
slot: the slot number in the file is replaced.

Everything goes through the padbus daemon (tools/padbus.py), which owns the
console.
"""

import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import padbus  # noqa: E402

ADDRESS = r"[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}"
LEGACY = re.compile(rf"^BOND local=({ADDRESS}) peer=({ADDRESS}) type=(\d+) "
                    r"key=([0-9a-fA-F]{32})", re.M)
# `config profile.A.type=x`, and `config.profile.A.type = x` from before.
SETTING = re.compile(r"^config[ .]profile\.\w+\.(bond\.\w+|type|address|rumble)"
                     r"\s*=\s*(\S+)", re.M)


SLOTS = "ABXY"      # named after the Home+button chord that selects them


def slot_name(slot):
    """A, B, X or Y, from a name (any case) or a number 0..3."""
    text = str(slot).strip().upper()
    if text in SLOTS and len(text) == 1:
        return text
    if text.isdigit() and int(text) < len(SLOTS):
        return SLOTS[int(text)]
    raise RuntimeError(f"no slot {slot!r}: use A, B, X or Y")


def read(path, slot):
    """The settings a bond file holds, as import lines for `slot`."""
    slot = slot_name(slot)
    text = Path(path).read_text(errors="replace")
    lines = [f"config profile.{slot}.{leaf}={value}"
             for leaf, value in SETTING.findall(text)]
    if lines:
        return lines
    match = LEGACY.search(text)
    if not match:
        raise RuntimeError(f"{path}: neither config lines nor a BOND line")
    local, peer, key_type, key = match.groups()
    return [f"config profile.{slot}.address={local.lower()}",
            f"config profile.{slot}.bond.peer={peer.lower()}",
            f"config profile.{slot}.bond.link_key={key.lower()}",
            f"config profile.{slot}.bond.key_type={key_type}"]


def write(path, lines):
    """Save lines to a new file readable by its owner only (keys inside)."""
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as output:
        output.write("\n".join(lines) + "\n")


class Pad:
    """Control of the pad through padbus, with `cmd` for each command."""

    def __init__(self, client, socket_path=padbus.DEFAULT_SOCKET):
        self.client = client
        self.sock = padbus.client_socket(socket_path)
        padbus.request(self.sock, op="hello", client=client)
        reply = padbus.request(self.sock, op="request", client=client)
        if not reply.get("ok"):
            raise RuntimeError("padbus control is held by " +
                               str(reply.get("owner", "another client")))

    def close(self):
        try:
            padbus.request(self.sock, op="release", client=self.client)
        finally:
            self.sock.close()

    def cmd(self, line, timeout=10.0, check=True, quiet=False):
        """Run one command; its output lines.  Raises on ERR when check."""
        reply = padbus.request(self.sock, op="cmd", client=self.client,
                               line=line, timeout=timeout, quiet=quiet)
        if check and not reply.get("ok"):
            raise RuntimeError(f"{line!r}: {reply.get('error', 'failed')}")
        return reply.get("lines", [])

    def stat(self):
        """The `stat` line as a dict (state, slot, boots, ...)."""
        for line in self.cmd("debug stat", quiet=True):
            if line.startswith("stat "):
                return dict(field.split("=", 1) for field in line.split()[1:]
                            if "=" in field)
        return {}

    def export(self, slot):
        """Slot `slot`'s settings that differ from the defaults."""
        return [line for line in self.cmd(f"config export profile.{slot_name(slot)}")
                if line.startswith("config ")]

    def load(self, lines):
        """Set each setting; raises on the first one the pad refuses."""
        for line in lines:
            self.cmd(line)
