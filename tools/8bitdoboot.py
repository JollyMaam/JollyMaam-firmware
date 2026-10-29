#!/usr/bin/env python3
"""Telink app images and the 8BitDo boot loader (2dc8:3208, HID).

  pack   app.bin from objcopy -> the image the boot loader starts: the stamp
         in header bytes 2..7, the size at 0x18, padded to 16 bytes with
         0xff, then the CRC (CRC-32 without its final inversion) of it all
  main   a packed app above the updater (src/updater/): the updater's header
         ("MAIN", size, CRC16 of the app, entry, format 1, its offset, the
         PID, CRC16 of those fields; 0x120 bytes) and the app: update.bin
  full   the updater's firmware.bin and the app's update.bin as one image
         from the updater's start, update.bin at --at: firmware.img
  flash  into the boot loader (stock or our firmware),
         erase and program the image's sectors (the header's last), read
         them back, start the app; --also ADDR:FILE first puts FILE there
  loader into the boot loader and wait there
  start  leave the boot loader without flashing: it starts the app there
  scan   boot loaders, stock devices and our firmware (read-only identify)

The loader's protocol is in padtool/pad8bitdo.py.
"""
import argparse
import os
from pathlib import Path
import struct
import sys
import time
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "padtool"))
from padhid import dev_path  # noqa: E402
from padtransport import scsi, tool_paths, usb_location  # noqa: E402
from pad8bitdo import LOADER, MODELS, SECTOR, Loader, Stock, check, find_stock  # noqa: E402

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


def full_image(updater, update, at):
    """The updater, padded with 0xff to at, then update.bin."""
    check(updater)
    if len(updater) > at or at % SECTOR or update[:4] != b"MAIN":
        raise ValueError("the updater must end before --at, a sector boundary, and update.bin start with MAIN")
    return updater + b"\xff" * (at - len(updater)) + update


def hidraw_nodes():
    """(node, vid, pid, USB port) of each hidraw interface."""
    found = []
    for sys_node in sorted(Path("/sys/class/hidraw").glob("hidraw*")):
        path = dev_path(sys_node.name)
        if not os.access(path, os.R_OK | os.W_OK):
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


def stock(pid=None, port=None):
    """Identify before selecting: the stock pad and receiver can share USB PID 310a."""
    return [(path, loc, product) for path, loc, product in find_stock()
            if product and (pid is None or product == pid) and (port is None or port == loc)]


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
        loader.write_image(image, args.start, args.end,
                           lambda i, n: print(f"\r8bitdoboot: sector {i}/{n}", end="", flush=True))
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
    p = sub.add_parser("full")
    p.add_argument("--at", type=lambda s: int(s, 0), required=True, help="update.bin's offset from the updater's start")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("updater", help="the updater's firmware.bin")
    p.add_argument("update", help="the app's update.bin")
    for name in ("flash", "loader", "start"):
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
    elif args.cmd == "full":
        Path(args.output).write_bytes(full_image(Path(args.updater).read_bytes(), Path(args.update).read_bytes(), args.at))
    elif args.cmd == "flash":
        flash(args)
    elif args.cmd == "start":
        found = matching_loaders(args.pid, args.device)
        if len(found) != 1:
            sys.exit("8bitdoboot: " + ("no" if not found else "more than one") + " matching boot loader" +
                     ("" if not found else "; pick one with --device PORT"))
        with Loader(found[0][0]) as loader:
            loader.start_app()
        print(f"8bitdoboot: {found[0][1]}: started the app")
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
