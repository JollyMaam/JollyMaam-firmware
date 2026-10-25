#!/usr/bin/env python3
"""Telink app images and the 8BitDo boot loader (2dc8:3208, HID).

  pack   app.bin from objcopy -> the image the boot loader starts: the stamp
         in header bytes 2..7, the size at 0x18, padded to 16 bytes with
         0xff, then the CRC (CRC-32 without its final inversion) of it all
  flash  into the boot loader (our firmware: its REBOOT_LOADER command),
         erase and program the image's sectors (the header's last), read
         them back, start the app
  loader into the boot loader and wait there
  scan   boot loaders and our firmware

The loader's protocol: 64-byte output reports (Linux: an unnumbered report,
so a 0 first), 05 then <u16 command, u16 0, u16 length, u16 CRC-16/MODBUS of
the data, u32 total, u32 address> and up to 46 data bytes; each reply
echoes the header with a status in place of the 0.  Commands: 3 program,
4 erase a 4 KiB sector, 5 read, 7 start the app, 8 identify (Ultimate 2C:
1b 30).  Its erase skips sectors below 0x6000 (the loader's own).
"""
import argparse
import os
from pathlib import Path
import select
import struct
import sys
import time
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "padtool"))
from padtransport import scsi, device_paths, usb_location  # noqa: E402

LOADER = (0x2dc8, 0x3208)
SECTOR = 4096
CHUNK = 46
OP = 0xF5


def pack(app, stamp, limit):
    image = bytearray(app)
    if len(image) < 0x20 or image[8:12] != b"KNLT":
        raise ValueError("not a Telink app image (no KNLT at 8)")
    image[2:8] = stamp
    image += b"\xff" * (-len(image) % 16)
    struct.pack_into("<I", image, 0x18, len(image) + 4)
    image += struct.pack("<I", zlib.crc32(image) ^ 0xFFFFFFFF)
    if len(image) > limit:
        raise ValueError(f"image is {len(image)} bytes, more than the {limit} it may take")
    return bytes(image)


def check(image):
    """The checks the boot loader makes of a packed image, as far as known."""
    if image[8:12] != b"KNLT" or struct.unpack_from("<I", image, 0x18)[0] != len(image):
        raise ValueError("bad image header")
    if struct.unpack_from("<I", image, len(image) - 4)[0] != zlib.crc32(image[:-4]) ^ 0xFFFFFFFF:
        raise ValueError("bad image CRC")


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def hidraw_nodes():
    """(node, vid, pid, USB port) of each hidraw interface."""
    found = []
    for sys_node in sorted(Path("/sys/class/hidraw").glob("hidraw*")):
        path = next((p for p in (f"/dev/bwrap/{sys_node.name}", f"/dev/{sys_node.name}")
                     if os.access(p, os.R_OK | os.W_OK)), None)
        if not path:
            continue
        usb = (sys_node / "device").resolve().parent.parent
        try:
            vid = int((usb / "idVendor").read_text(), 16)
            pid = int((usb / "idProduct").read_text(), 16)
        except OSError:
            continue
        found.append((path, vid, pid, usb.name))
    return found


def loaders(port=None):
    return [(p, loc) for p, vid, pid, loc in hidraw_nodes()
            if (vid, pid) == LOADER and (port is None or loc == port)]


def ours(pid=None, port=None):
    """(sg node, USB port, identity) of our firmware, for one image PID."""
    found = []
    for path in device_paths():
        try:
            inquiry = scsi(path, [0x12, 0, 0, 0, 40], 40, 1000)
            if len(inquiry) < 40 or inquiry[36:40] != b"C2PD":
                continue
            identity = scsi(path, [OP, 0x00], 88, 1000)
        except OSError:
            continue
        if len(identity) < 56 or identity[:4] != b"C2PD":
            continue
        image_pid = identity[40:56].split(b"\0")[0].decode(errors="replace")
        loc = usb_location(path)
        if (pid is None or image_pid == pid) and (port is None or loc == port):
            found.append((path, loc, identity))
    return found


class Loader:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)

    def close(self):
        os.close(self.fd)

    def request(self, command, address, data=b"", length=None):
        report = bytearray(64)
        report[0] = 5
        count = len(data) if length is None else length
        total = SECTOR if command in (3, 4) else count
        struct.pack_into("<HHHHII", report, 1, command, 0, count, crc16(data) if data else 0, total, address)
        report[17:17 + len(data)] = data
        while select.select([self.fd], [], [], 0)[0]:
            os.read(self.fd, 256)
        if os.write(self.fd, b"\0" + report) != 65:
            raise OSError("short HID write")
        if command == 7:
            return b""
        deadline = time.monotonic() + 10
        while True:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.fd], [], [], left)[0]:
                raise TimeoutError(f"no answer to command {command} at {address:#x}")
            reply = os.read(self.fd, 256)
            if len(reply) < 18 or reply[:2] != b"\0\x05":
                raise OSError(f"bad reply {reply.hex()}")
            status, echoed, n, _, _, at = struct.unpack_from("<HHHHII", reply, 2)
            if status or echoed != command:
                raise OSError(f"command {command} at {address:#x}: reply {reply[:18].hex()}")
            if command == 5 and at != address:
                continue                 # a late reply to an earlier read
            return reply[18:18 + n]

    def identify(self):
        return self.request(8, 0, length=4)

    def read(self, address, count):
        out = bytearray()
        while len(out) < count:
            n = min(CHUNK, count - len(out))
            part = self.request(5, address + len(out), length=n)
            if len(part) != n:
                raise OSError(f"short read at {address + len(out):#x}")
            out += part
        return bytes(out)

    def write_sector(self, address, data):
        self.request(4, address)
        if self.read(address, SECTOR) != b"\xff" * SECTOR:
            raise OSError(f"sector {address:#x} did not erase")
        for offset in range(0, SECTOR, CHUNK):
            self.request(3, address + offset, data[offset:offset + CHUNK])
        if self.read(address, SECTOR) != data:
            raise OSError(f"sector {address:#x} does not read back as written")

    def start_app(self):
        self.request(7, 0)


def wait_loader(port, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        found = loaders(port)
        if found:
            time.sleep(0.2)
            return found[0][0]
        time.sleep(0.1)
    return None


def enter_loader(pid, device, seconds):
    """The boot loader's hidraw node: one already there, or our firmware sent to it."""
    found = loaders(device)
    if len(found) > 1:
        sys.exit("bitdoboot: more than one boot loader; pick one with --device PORT")
    if found:
        return found[0][0]
    running = ours(pid, device)
    if len(running) > 1:
        sys.exit("bitdoboot: more than one pad; pick one with --device PORT")
    if not running:
        return None
    path, port, _ = running[0]
    print(f"bitdoboot: {port}: firmware into the boot loader")
    try:
        scsi(path, [OP, 0x11], 0, 2000)
    except OSError:
        pass                             # it may leave before the status gets back
    return wait_loader(port, seconds)


def flash(args):
    image = Path(args.image).read_bytes()
    check(image)
    end = args.start + len(image) + (-len(image) % SECTOR)
    if end > args.end:
        sys.exit(f"bitdoboot: image ends at {end:#x}, past {args.end:#x}")
    path = enter_loader(args.pid, args.device, args.wait)
    if not path:
        sys.exit("bitdoboot: no boot loader and no pad running this firmware" +
                 (f"\n{args.hint}" if args.hint else ""))
    loader = Loader(path)
    try:
        model = loader.identify()
        print(f"bitdoboot: boot loader {path}, model {model.hex()}")
        padded = image + b"\xff" * (end - args.start - len(image))
        sectors = list(range(args.start + SECTOR, end, SECTOR)) + [args.start]
        for i, address in enumerate(sectors, 1):
            offset = address - args.start
            print(f"\rbitdoboot: sector {i}/{len(sectors)} at {address:#x}", end="", flush=True)
            loader.write_sector(address, padded[offset:offset + SECTOR])
        print("\nbitdoboot: written and read back; starting it")
        loader.start_app()
    finally:
        loader.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack")
    p.add_argument("--stamp", required=True, help="header bytes 2..7, hex")
    p.add_argument("--max", type=lambda s: int(s, 0), required=True, help="the most bytes it may take")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("app")
    for name in ("flash", "loader"):
        p = sub.add_parser(name)
        p.add_argument("--pid", required=True, help="the image PID our firmware answers with")
        p.add_argument("--device", help="the USB port (as scan lists it)")
        p.add_argument("--wait", type=float, default=15)
        p.add_argument("--hint", help="how to enter the boot loader by hand")
        if name == "flash":
            p.add_argument("--start", type=lambda s: int(s, 0), required=True)
            p.add_argument("--end", type=lambda s: int(s, 0), required=True)
            p.add_argument("image")
    sub.add_parser("scan")
    args = ap.parse_args()
    if args.cmd == "pack":
        stamp = bytes.fromhex(args.stamp)
        if len(stamp) != 6:
            sys.exit("bitdoboot: the stamp is 6 bytes")
        Path(args.output).write_bytes(pack(Path(args.app).read_bytes(), stamp, args.max))
    elif args.cmd == "flash":
        flash(args)
    elif args.cmd == "loader":
        path = enter_loader(args.pid, args.device, args.wait)
        if not path:
            sys.exit("bitdoboot: no boot loader and no pad running this firmware" +
                     (f"\n{args.hint}" if args.hint else ""))
        print(f"bitdoboot: boot loader at {path}")
    else:
        for path, loc in loaders():
            print(f"{loc}\tboot loader\t{path}")
        for path, loc, identity in ours():
            name = identity[56:88].split(b"\0")[0].decode(errors="replace")
            pid = identity[40:56].split(b"\0")[0].decode(errors="replace")
            print(f"{loc}\t{name} ({pid})\t{path}")


if __name__ == "__main__":
    main()
