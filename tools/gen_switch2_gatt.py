#!/usr/bin/env python3
"""Generate the Switch 2 (Pro Controller 2) GATT database for BTstack.

The layout is ndeadly's table (switch2_controller_research,
bluetooth_interface.md "GATT Attributes"), Pro Controller variant without the
headset-audio attributes, so the handles come out exactly as a real
controller's: the console may address them directly.  BTstack's .gatt compiler
cannot express the custom 128-bit descriptors, so the database is written out
here in BTstack's binary format (att_db.c):

    u8  version (ATT_DB_VERSION = 1)
    per attribute: u16 size, u16 flags, u16 handle, uuid (2 or 16 bytes LE), value
    u16 0 terminator

    tools/gen_switch2_gatt.py > src/switch2_att_db.h
"""

import sys
import uuid as uuidlib

READ, WRITE_NR, WRITE, NOTIFY = 0x02, 0x04, 0x08, 0x10
DYNAMIC, UUID128 = 0x100, 0x200

SERVICE, CHARACTERISTIC, CCC = 0x2800, 0x2803, 0x2902


def u128(text):
    return uuidlib.UUID(text).bytes[::-1]          # little endian


def u16le(v):
    return bytes((v & 0xff, v >> 8))


class Db:
    def __init__(self):
        self.entries = []          # (handle, flags, uuid bytes, value)
        self.handle = 0
        self.names = []

    def add(self, flags, uuid, value=b""):
        self.handle += 1
        if isinstance(uuid, int):
            ub = u16le(uuid)
        else:
            ub = u128(uuid)
            flags |= UUID128
        self.entries.append((self.handle, flags, ub, value))
        return self.handle

    def service(self, uuid):
        value = u16le(uuid) if isinstance(uuid, int) else u128(uuid)
        return self.add(READ, SERVICE, value)

    def characteristic(self, uuid, props, value=None, name=None):
        decl = self.handle + 1
        vh = decl + 1
        ub = u16le(uuid) if isinstance(uuid, int) else u128(uuid)
        self.add(READ, CHARACTERISTIC, bytes((props,)) + u16le(vh) + ub)
        if value is None:
            h = self.add(props | DYNAMIC, uuid)
        else:
            h = self.add(props, uuid, value)
        assert h == vh
        if name:
            self.names.append((name, h))
        return h

    def descriptor(self, uuid, name=None):
        h = self.add(READ | WRITE | WRITE_NR | DYNAMIC, uuid)
        if name:
            self.names.append((name, h))
        return h

    def blob(self):
        out = bytearray((1,))
        for handle, flags, ub, value in self.entries:
            body = u16le(flags) + u16le(handle) + ub + value
            out += u16le(len(body) + 2) + body
        out += b"\x00\x00"
        return bytes(out)


RATE = "679d5510-5a24-4dee-9557-95df80486ecb"
UNKNOWN_DESC = "b746df8c-f358-495b-9cd2-e3bbeda4f979"


def build():
    d = Db()
    d.service("00c5af5d-1964-4e30-8f51-1956f96bd280")
    d.characteristic("00c5af5d-1964-4e30-8f51-1956f96bd281", READ, name="INFO_1")
    d.characteristic("00c5af5d-1964-4e30-8f51-1956f96bd282", WRITE, name="UNKNOWN_WRITE")
    d.characteristic("00c5af5d-1964-4e30-8f51-1956f96bd283", READ, name="INFO_2")

    d.service("ab7de9be-89fe-49ad-828f-118f09df7fd0")
    d.characteristic("ab7de9be-89fe-49ad-828f-118f09df7fd2", READ | NOTIFY, name="COMMON_REPORT")
    d.descriptor(CCC, "COMMON_REPORT_CCC")
    d.descriptor(RATE)
    d.characteristic("7492866c-ec3e-4619-8258-32755ffcc0f8", READ | NOTIFY, name="INPUT_REPORT")
    d.descriptor(CCC, "INPUT_REPORT_CCC")
    d.descriptor(RATE)
    d.characteristic("cc483f51-9258-427d-a939-630c31f72b05", WRITE_NR, name="VIBRATION")
    d.characteristic("649d4ac9-8eb7-4e6c-af44-1ea54fe5f005", WRITE_NR, name="COMMAND")
    d.characteristic("3dacbc7e-6955-40b5-8eaf-6f9809e8b379", WRITE_NR, name="VIBRATION_COMMAND")
    d.characteristic("4147423d-fdae-4df7-a4f7-d23e5df59f8d", WRITE_NR, name="FIRMWARE_UPDATE")
    d.characteristic("c765a961-d9d8-4d36-a20a-5315b111836a", NOTIFY, name="RESPONSE_1")
    d.descriptor(CCC, "RESPONSE_1_CCC")
    d.descriptor(UNKNOWN_DESC)
    d.characteristic("506d9f7d-4278-4e95-a549-326ba77657e0", NOTIFY, name="RESPONSE_2")
    d.descriptor(CCC, "RESPONSE_2_CCC")
    d.descriptor(UNKNOWN_DESC)
    d.characteristic("d3bd69d2-841c-4241-ab15-f86f406d2a80", NOTIFY, name="UNKNOWN_NOTIFY")
    d.descriptor(CCC, "UNKNOWN_NOTIFY_CCC")
    d.descriptor(UNKNOWN_DESC)
    d.characteristic("ab7de9be-89fe-49ad-828f-118f09df7fde", READ | NOTIFY, name="UNKNOWN_REPORT")
    d.descriptor(CCC, "UNKNOWN_REPORT_CCC")
    d.descriptor(RATE)
    d.characteristic("ab7de9be-89fe-49ad-828f-118f09df7fdf", WRITE_NR, name="UNKNOWN_OUTPUT")

    d.service(0x1800)
    d.characteristic(0x2a00, READ, b"Pro Controller")
    d.characteristic(0x2a01, READ, u16le(0x03c4))          # appearance: gamepad
    d.service(0x1801)
    return d


def check(d):
    # Handles from ndeadly's table; the console may use them directly.
    expect = {"INFO_1": 0x03, "UNKNOWN_WRITE": 0x05, "INFO_2": 0x07,
              "COMMON_REPORT": 0x0a, "COMMON_REPORT_CCC": 0x0b,
              "INPUT_REPORT": 0x0e, "INPUT_REPORT_CCC": 0x0f,
              "VIBRATION": 0x12, "COMMAND": 0x14, "VIBRATION_COMMAND": 0x16,
              "FIRMWARE_UPDATE": 0x18, "RESPONSE_1": 0x1a, "RESPONSE_1_CCC": 0x1b,
              "RESPONSE_2": 0x1e, "RESPONSE_2_CCC": 0x1f, "UNKNOWN_NOTIFY": 0x22,
              "UNKNOWN_NOTIFY_CCC": 0x23, "UNKNOWN_REPORT": 0x26,
              "UNKNOWN_REPORT_CCC": 0x27, "UNKNOWN_OUTPUT": 0x2a}
    got = dict(d.names)
    for name, handle in expect.items():
        if got.get(name) != handle:
            sys.exit(f"{name}: handle {got.get(name)} != {handle:#x}")
    if d.handle != 0x30:
        sys.exit(f"last handle {d.handle:#x} != 0x30")


def main():
    d = build()
    check(d)
    blob = d.blob()
    print("/* Generated by tools/gen_switch2_gatt.py - do not edit. */")
    print("#ifndef CYCLONE2_SWITCH2_ATT_DB_H")
    print("#define CYCLONE2_SWITCH2_ATT_DB_H\n")
    print("#include <stdint.h>\n")
    print(f"/* BTstack ATT DB, {len(blob)} bytes, handles 0x0001..0x{d.handle:04x}. */")
    print("static const uint8_t switch2_att_db[] = {")
    for i in range(0, len(blob), 16):
        print("    " + ",".join(f"0x{b:02x}" for b in blob[i:i + 16]) + ",")
    print("};\n")
    for name, handle in d.names:
        print(f"#define SWITCH2_ATT_{name}_HANDLE 0x{handle:04x}U")
    print("\n#endif")


if __name__ == "__main__":
    main()
