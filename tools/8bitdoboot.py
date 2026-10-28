#!/usr/bin/env python3
"""Telink app images and the 8BitDo boot loader (2dc8:3208, HID).

  pack   app.bin from objcopy -> the image the boot loader starts: the stamp
         in header bytes 2..7, the size at 0x18, padded to 16 bytes with
         0xff, then the CRC (CRC-32 without its final inversion) of it all
  main   a packed app above the updater (src/updater/): the updater's header
         ("MAIN", size, CRC16 of the app, entry, format 1, its offset, the
         PID, CRC16 of those fields; 0x120 bytes) and the app: update.bin
  flash  into the boot loader (stock or our firmware),
         erase and program the image's sectors (the header's last), read
         them back, start the app; --also ADDR:FILE first puts FILE there
  loader into the boot loader and wait there
  scan   boot loaders, stock devices and our firmware (read-only identify)

The loader's protocol: 64-byte output reports (Linux: an unnumbered report,
so a 0 first), 05 then <u16 command, u16 0, u16 length, u16 CRC-16/MODBUS of
the data, u32 total, u32 address> and up to 46 data bytes; each reply
echoes the header with a status in place of the 0.  Commands: 3 program,
4 erase a 4 KiB sector, 5 read, 7 start the app, 8 identify (Ultimate 2C:
1b 30).  Its erase skips sectors below 0x6000 (the loader's own).
"""
import argparse
import errno
import os
from pathlib import Path
import select
import struct
import sys
import time
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "padtool"))
from padtransport import scsi, tool_paths, usb_location  # noqa: E402

LOADER = (0x2dc8, 0x3208)
MODELS = {"U2C_WIRELESS": 0x301b, "U2C_RECEIVER": 0x301c}
STOCK_PIDS = {0x310a, 0x301b, 0x301c, 0x301d}
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


MAIN_HEAD_SIZE = 0x120


def crc16_ccitt(data):
    """Non-reflected CRC-CCITT from 0, as the updater and jlnewfw use."""
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1) & 0xFFFF
    return crc


def main_image(app, pid, at, entry):
    head = bytearray(b"\xff" * MAIN_HEAD_SIZE)
    struct.pack_into("<4sIIIII16s", head, 0, b"MAIN", len(app), crc16_ccitt(app), entry, 1, at,
                     pid.encode().ljust(16, b"\0"))
    struct.pack_into("<I", head, 40, crc16_ccitt(head[4:40]))
    return bytes(head) + app


def check(image):
    """The checks the boot loader makes of a packed image, as far as known."""
    if len(image) < 32 or image[8:12] != b"KNLT" or struct.unpack_from("<I", image, 0x18)[0] != len(image):
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
    """(tool device, USB port, identity) of our firmware, for one image PID."""
    found = []
    for path in tool_paths():
        try:
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

    def request(self, command, address, data=b"", length=None, timeout=10):
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
        deadline = time.monotonic() + timeout
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
            if n > CHUNK or len(reply) < 18 + n:
                raise OSError("truncated or oversized boot loader reply")
            return reply[18:18 + n]

    def identify(self):
        return self.request(8, 0, length=4, timeout=1)

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


class Stock(Loader):
    """Stock HID vendor interface: numbered 0x81 output, 0x02 input."""

    def identify(self):
        report = bytearray(64)
        report[:2] = b"\x81\x05"
        struct.pack_into("<H", report, 2, 8)
        while select.select([self.fd], [], [], 0)[0]:
            os.read(self.fd, 256)
        if os.write(self.fd, report) != len(report):
            raise OSError("short stock HID identify write")
        deadline = time.monotonic() + 0.5
        while True:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.fd], [], [], left)[0]:
                raise TimeoutError("stock model query did not answer")
            reply = os.read(self.fd, 256)
            if len(reply) < 20 or reply[:2] != b"\x02\x05":
                continue
            status, command, count = struct.unpack_from("<HHH", reply, 2)
            if status == 0 and command == 8 and count == 2:
                return reply[18:20]

    def reboot_loader(self):
        # The 0x81 is the report ID, not an unnumbered-report payload byte.
        report = bytes.fromhex("81 05 00 51 00").ljust(64, b"\0")
        try:
            written = os.write(self.fd, report)
        except OSError as error:
            if error.errno in (errno.ENODEV, errno.EPIPE, errno.ECONNRESET, errno.ESHUTDOWN):
                return                  # reset may win the USB completion race
            raise
        if written != len(report):
            raise OSError("short stock HID boot-entry write")


def has_stock_output(descriptor):
    """Whether this interface has an Output item for report ID 0x81."""
    offset, report_id = 0, 0
    while offset < len(descriptor):
        prefix = descriptor[offset]
        if prefix == 0xfe:
            if offset + 2 >= len(descriptor):
                return False
            offset += 3 + descriptor[offset + 1]
            continue
        size = (0, 1, 2, 4)[prefix & 3]
        if offset + 1 + size > len(descriptor):
            return False
        value = int.from_bytes(descriptor[offset + 1:offset + 1 + size], "little")
        if prefix & 0xfc == 0x84:
            report_id = value
        elif prefix & 0xfc == 0x90 and report_id == 0x81:
            return True
        offset += 1 + size
    return False


def stock(pid=None, port=None):
    """Identify before selecting: the stock pad and receiver can share USB PID 310a."""
    found = []
    for path, vid, product, loc in hidraw_nodes():
        if vid != 0x2dc8 or product not in STOCK_PIDS or (port is not None and port != loc):
            continue
        descriptor = Path("/sys/class/hidraw") / Path(path).name / "device/report_descriptor"
        try:
            if not has_stock_output(descriptor.read_bytes()):
                continue
            device = Stock(path)
            try:
                model = device.identify()
            finally:
                device.close()
        except OSError:
            continue
        product_pid = next((p for p, m in MODELS.items() if model == struct.pack("<H", m)), None)
        if product_pid and (pid is None or product_pid == pid):
            found.append((path, loc, product_pid))
    return found


def matching_loaders(pid, port=None):
    found = []
    for path, loc in loaders(port):
        try:
            device = Loader(path)
            try:
                model = device.identify()
            finally:
                device.close()
        except OSError:
            continue
        if model == struct.pack("<H", MODELS[pid]):
            found.append((path, loc))
    return found


def wait_loader(port, seconds, pid):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        found = matching_loaders(pid, port)
        if found:
            time.sleep(0.2)
            return found[0][0]
        time.sleep(0.1)
    return None


def wait_entered_loader(port, seconds, pid):
    path = wait_loader(port, seconds, pid)
    if path is None:
        sys.exit(f"8bitdoboot: requested boot entry on {port}, but no 2dc8:3208 loader "
                 f"with model {MODELS[pid]:04x} appeared within {seconds:g}s" +
                 ("; the receiver bootloader USB identity is not yet established"
                  if pid == "U2C_RECEIVER" else ""))
    return path


def enter_loader(pid, device, seconds):
    """Select by model and port, then enter through stock HID or our tool device."""
    found = matching_loaders(pid, device)
    if len(found) > 1:
        sys.exit("8bitdoboot: more than one boot loader; pick one with --device PORT")
    if found:
        return found[0][0]
    running = ours(pid, device)
    original = stock(pid, device)
    ports = {loc for _, loc, _ in running + original}
    if len(ports) > 1:
        sys.exit("8bitdoboot: more than one matching device; pick one with --device PORT")
    if not running and not original:
        return None
    if not running:
        path, port, _ = original[0]
        target = Stock(path)
        try:
            if target.identify() != struct.pack("<H", MODELS[pid]):
                raise OSError("stock device model changed before boot entry")
            print(f"8bitdoboot: {port}: stock {pid} into the boot loader")
            target.reboot_loader()
        finally:
            target.close()
        return wait_entered_loader(port, seconds, pid)
    path, port, _ = running[0]
    print(f"8bitdoboot: {port}: firmware into the boot loader")
    try:
        scsi(path, [OP, 0x11], 0, 2000)
    except OSError:
        pass                             # it may leave before the status gets back
    return wait_entered_loader(port, seconds, pid)


def flash(args):
    if args.pid != "U2C_WIRELESS":
        sys.exit("8bitdoboot: receiver flashing is not established; loader entry only")
    image = Path(args.image).read_bytes()
    check(image)
    end = args.start + len(image) + (-len(image) % SECTOR)
    if end > args.end:
        sys.exit(f"8bitdoboot: image ends at {end:#x}, past {args.end:#x}")
    also = []
    for spec in getattr(args, "also", []):
        address, _, name = spec.partition(":")
        data = Path(name).read_bytes()
        address = int(address, 0)
        if address % SECTOR or address < end:
            sys.exit(f"8bitdoboot: --also {spec}: a sector boundary past the image")
        also.append((address, data + b"\xff" * (-len(data) % SECTOR)))
    path = enter_loader(args.pid, args.device, args.wait)
    if not path:
        sys.exit("8bitdoboot: no matching boot loader, stock HID device, or our tool device" +
                 (f"\n{args.hint}" if args.hint else ""))
    loader = Loader(path)
    try:
        model = loader.identify()
        if model != struct.pack("<H", MODELS[args.pid]):
            raise OSError("boot loader model changed before flashing")
        print(f"8bitdoboot: boot loader {path}, model {model.hex()}")
        for address, data in also:
            for offset in range(0, len(data), SECTOR):
                print(f"\r8bitdoboot: {address + offset:#x}", end="", flush=True)
                loader.write_sector(address + offset, data[offset:offset + SECTOR])
        padded = image + b"\xff" * (end - args.start - len(image))
        sectors = list(range(args.start + SECTOR, end, SECTOR)) + [args.start]
        for i, address in enumerate(sectors, 1):
            offset = address - args.start
            print(f"\r8bitdoboot: sector {i}/{len(sectors)} at {address:#x}", end="", flush=True)
            loader.write_sector(address, padded[offset:offset + SECTOR])
        print("\n8bitdoboot: written and read back; starting it")
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
    p = sub.add_parser("main")
    p.add_argument("--pid", required=True, help="the image's PID")
    p.add_argument("--at", type=lambda s: int(s, 0), required=True, help="its offset above the updater")
    p.add_argument("--entry", type=lambda s: int(s, 0), required=True, help="the app's start (its APP_START)")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("app", help="the packed app (firmware.bin)")
    for name in ("flash", "loader"):
        p = sub.add_parser(name)
        p.add_argument("--pid", required=True, choices=MODELS, help="the product to select by model")
        p.add_argument("--device", help="the USB port (as scan lists it)")
        p.add_argument("--wait", type=float, default=15)
        p.add_argument("--hint", help="how to enter the boot loader by hand")
        if name == "flash":
            p.add_argument("--start", type=lambda s: int(s, 0), required=True)
            p.add_argument("--end", type=lambda s: int(s, 0), required=True)
            p.add_argument("--also", action="append", default=[], metavar="ADDR:FILE",
                           help="also write FILE at ADDR (before the image)")
            p.add_argument("image")
    sub.add_parser("scan")
    args = ap.parse_args()
    if args.cmd == "pack":
        stamp = bytes.fromhex(args.stamp)
        if len(stamp) != 6:
            sys.exit("8bitdoboot: the stamp is 6 bytes")
        Path(args.output).write_bytes(pack(Path(args.app).read_bytes(), stamp, args.max))
    elif args.cmd == "main":
        app = Path(args.app).read_bytes()
        check(app)
        Path(args.output).write_bytes(main_image(app, args.pid, args.at, args.entry))
    elif args.cmd == "flash":
        flash(args)
    elif args.cmd == "loader":
        path = enter_loader(args.pid, args.device, args.wait)
        if not path:
            sys.exit("8bitdoboot: no matching boot loader, stock HID device, or our tool device" +
                     (f"\n{args.hint}" if args.hint else ""))
        print(f"8bitdoboot: boot loader at {path}")
    else:
        for path, loc in loaders():
            loader = Loader(path)
            try:
                model = loader.identify()
                print(f"{loc}\tboot loader, model {model.hex()}\t{path}")
            finally:
                loader.close()
        for path, loc, pid in stock():
            print(f"{loc}\tstock {pid}\t{path}")
        for path, loc, identity in ours():
            name = identity[56:88].split(b"\0")[0].decode(errors="replace")
            pid = identity[40:56].split(b"\0")[0].decode(errors="replace")
            print(f"{loc}\t{name} ({pid})\t{path}")


if __name__ == "__main__":
    main()
