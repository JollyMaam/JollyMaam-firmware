"""Jieli ROM/RAM-loader protocol (BR23, BD19) and raw image flashing, shared by GUI and CLI.

Ported from our jollymaam implementation. No external flashing executable is
needed. Images are converted in memory to the connected chip's key.
"""
import argparse
import binascii
import os
from pathlib import Path
import struct
import time

import padhid
from padknown import CHIPS, PRODUCTS, product_name, stock_devices
from padtransport import device_paths, scsi, usb_location, is_tool

# RAM loaders: a folder of <chip>loader.bin beside padtool, else the SDK.
PACKAGED_LOADERS = Path(__file__).resolve().parent
SDK = Path(os.environ.get("JIELI_SDK", Path(__file__).resolve().parent.parent / "external" / "jieli-fw-AC63_BT_SDK"))
DEFAULT_LOADER = (PACKAGED_LOADERS if any((PACKAGED_LOADERS / c.blob).is_file() for c in CHIPS.values())
                  else SDK)


def loader_blob(source, chip):
    """The RAM loader for chip: source is the bytes, the SDK (its
    cpu/<chip>/tools/), a folder of <chip>loader.bin, or one of the SDK's
    loader files (the others are found beside it)."""
    if isinstance(source, (bytes, bytearray)):
        return bytes(source)
    spec = CHIPS[chip]
    source = Path(source)
    if source.name == spec.blob and source.is_file():
        return source.read_bytes()
    folder = source if source.is_dir() else source.parent
    for path in (folder / "cpu" / spec.sdk_dir / "tools" / spec.blob, folder / spec.blob,
                 folder.parent.parent / spec.sdk_dir / "tools" / spec.blob):
        if path.is_file():
            return path.read_bytes()
    raise ValueError(f"No {spec.blob} for the {chip} under {source}; select the SDK or a folder of loaders")


def crc16(data, seed=0):
    return binascii.crc_hqx(data, seed)


def enc(data, key=0xffff):
    out = bytearray()
    for byte in data:
        out.append(byte ^ (key & 255))
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xffff
    return bytes(out)


def name(data):
    return data.split(b"\0", 1)[0].split(b"\xff", 1)[0].decode("ascii")


def u16(data, offset=0):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset=0):
    return struct.unpack_from("<I", data, offset)[0]


def valid_header(data, offset):
    header = enc(data[offset:offset + 32])
    return len(header) == 32 and u16(header) != 0 and crc16(header[2:]) == u16(header)


def entries(data, base):
    for offset in range(base + 32, min(len(data) - 31, base + 0x400), 32):
        h = enc(data[offset:offset + 32])
        if crc16(h[2:]) != u16(h):
            break
        yield name(h[16:32]), base + u32(h, 4), u32(h, 8), u16(h, 14)


def image_base(data):
    """Same boot-image selection as jollymaam, including EOFFSET stale copies."""
    base = next((o for o in (0, 0x1000, 0x10000) if valid_header(data, o)), None)
    if base is None:
        raise ValueError("No valid firmware flash header")
    for n, offset, size, index in entries(data, base):
        if n == "isd_config.ini" and offset + size <= len(data):
            config = data[offset:offset + size]
            at = config.find(b"EOFFSET\0")
            if at >= 0 and at + 12 <= len(config):
                alternate = u32(config, at + 8) * 0x1000
                if alternate and valid_header(data, alternate):
                    base = alternate
            break
        if index:
            break
    return base


def image_identity(data, require_key=True):
    base = image_base(data)
    pid = name(data[base + 16:base + 32])
    if not require_key:
        return pid, None
    for n, offset, size, index in entries(data, base):
        if n == "isd_config.ini" and size >= 34 and offset + 34 <= len(data):
            record = data[offset:offset + 34]
            if crc16(record[:32]) != u16(record, 32):
                raise ValueError("Chip-key record CRC mismatch")
            threshold = sum(record[:16]) & 255
            threshold = 0xaa if threshold >= 0xe0 else 0x55 if threshold <= 0x10 else threshold
            key = sum(1 << j for j in range(16) if (record[16 + j] ^ record[15 - j]) < threshold)
            return pid, key
    raise ValueError("Image has no valid chip-key record")


def sfc(data, key):
    """Application cipher; block offsets are relative to the application area."""
    return b"".join(enc(data[at:at + 32], key ^ (at >> 2)) for at in range(0, len(data), 32))


def rekey_image(data, key):
    """Convert a packed build without changing its layout or the input buffer.

    Validate the source directories and application/resource CRCs before
    changing any bytes. Reserved flash regions and image padding stay intact.
    """
    if not 0 <= key <= 0xffff:
        raise ValueError("Chip key must fit in 16 bits")
    pid, old_key = image_identity(data)
    if key == old_key:
        return bytes(data)

    def header(blob, at, scrambled=True):
        h = blob[at:at + 32]
        if scrambled:
            h = enc(h)
        if len(h) != 32 or crc16(h[2:]) != u16(h):
            raise ValueError("Image directory header CRC mismatch or truncated header")
        return bytearray(h)

    base = image_base(data)
    config = app = None
    for at in range(base + 32, base + 0x400, 32):
        h = header(data, at)
        n = name(h[16:32])
        if n == "isd_config.ini":
            if config is not None:
                raise ValueError("Multiple chip-key records in image")
            config = (at, h)
        elif n == "app_dir_head":
            if app is not None:
                raise ValueError("Multiple application directories in image")
            app = base + u32(h, 4)
        elif n == "app_dir_head2":
            raise ValueError("Chip-key conversion of secondary application directories is unsupported")
        if u16(h, 14):
            break
    else:
        raise ValueError("Unterminated image directory")
    if config is None or app is None:
        raise ValueError("Image lacks a chip-key record or application directory")
    config_at, config_header = config
    config_start, config_size = base + u32(config_header, 4), u32(config_header, 8)
    if not (at + 32 <= config_start and config_size >= 34 and
            config_start + config_size <= app < len(data)):
        raise ValueError("Image configuration or application lies outside its expected bounds")
    config_data = bytearray(data[config_start:config_start + config_size])
    if crc16(config_data) != u16(config_header, 2):
        raise ValueError("Image configuration CRC mismatch")
    # Stock pad check records and secondary directories need their own rules;
    # do not silently leave a key-dependent check unchanged.
    check = enc(data[app - 32:app])
    if crc16(check[2:]) == u16(check) and name(check[16:32]) == "check":
        raise ValueError("Chip-key conversion of stock application check records is unsupported")

    plain = sfc(data[app:], old_key)
    end = 0
    while True:
        h = header(plain, end, scrambled=False)
        if h[12] != 0x83 or (not end and name(h[16:32]) != "app_area_head"):
            raise ValueError("Invalid application or resource area")
        size = u32(h, 8)
        if size < 32 or end + size > len(plain):
            raise ValueError("Application or resource area is truncated")
        if crc16(plain[end + 32:end + size]) != u16(h, 2):
            raise ValueError("Application or resource data CRC mismatch")
        end += size
        if u16(h, 14):
            break

    # Preserve the carrier's random bytes and change only bits that need a
    # different classification, using the same threshold rule as jollymaam.
    threshold = sum(config_data[:16]) & 255
    threshold = 0xaa if threshold >= 0xe0 else 0x55 if threshold <= 0x10 else threshold
    for bit in range(16):
        value = config_data[16 + bit] ^ config_data[15 - bit]
        want = bool(key & (1 << bit))
        if (value < threshold) != want:
            value = value % threshold if want else threshold + value % (256 - threshold)
            config_data[16 + bit] = config_data[15 - bit] ^ value
    struct.pack_into("<H", config_data, 32, crc16(config_data[:32]))
    struct.pack_into("<H", config_header, 2, crc16(config_data))
    struct.pack_into("<H", config_header, 0, crc16(config_header[2:]))
    out = bytearray(data)
    out[config_start:config_start + config_size] = config_data
    out[config_at:config_at + 32] = enc(config_header)
    out[app:app + end] = sfc(plain[:end], key)
    if image_identity(out) != (pid, key):
        raise ValueError("Converted image identity does not match the requested key")
    return bytes(out)


def load_image(path):
    """Load a raw image, or unwrap a CRC-checked UFW flash.bin entry."""
    data = Path(path).read_bytes()
    if len(data) >= 64:
        h = enc(data[:64])
        if crc16(h[2:]) == u16(h):
            count = u16(h, 8)
            end = 64 + count * 80
            if not count or end > len(data) or crc16(data[64:end]) != u16(h, 2):
                raise ValueError("Invalid UFW entry list")
            for offset in range(64, end, 80):
                item = enc(data[offset:offset + 80])
                if u16(item) == 0:
                    at, size = u32(item, 8), u32(item, 12)
                    if at + size > len(data) or crc16(data[at:at + size]) != u16(item, 4):
                        raise ValueError("UFW flash.bin is truncated or has a CRC mismatch")
                    return data[at:at + size]
            raise ValueError("UFW contains no flash.bin")
    return data


def decode_chip_key(data):
    magic = bytes.fromhex("c3cfc0e8ced2b0aec4e3a3acd3f1c1d6")
    crc = crc16(b"\xff\xff", 0xffff)
    result = bytearray()
    for i, byte in enumerate(data):
        crc = crc16(magic[i % 16:i % 16 + 1], crc)
        result.append(byte ^ (crc & 255))
    return int.from_bytes(result, "little")


class Loader:
    def __init__(self, path, report=print, progress=None, chip="BR23"):
        self.path = path
        self.chip = chip                 # CHIPS key: the ROM loader's INQUIRY vendor
        self.location = usb_location(path)
        self.report = report
        self.progress = progress or (lambda stage, done=0, total=None: None)
        self.io = 512

    def transfer(self, command, args=b"", length=0, out=None):
        if self.location and usb_location(self.path) != self.location:
            raise OSError("ROM loader disconnected or its device path was reused; refresh before continuing")
        cdb = (struct.pack(">H", command) + args).ljust(16, b"\xff")
        if len(cdb) != 16:
            raise ValueError("Loader arguments exceed CDB length")
        data = scsi(self.path, cdb, length, timeout_ms=20000, out=out, raw_length=True)
        if out is None and len(data) != length:
            raise OSError(f"Short loader response: {len(data)} of {length} bytes")
        return data

    def execute(self, command, args=b""):
        data = self.transfer(command, args, 16)
        if data[:2] != struct.pack(">H", command):
            raise OSError(f"Loader did not acknowledge command {command:04x}")
        return data[2:]

    def write(self, command, address, data):
        args = struct.pack(">IH", address, len(data)) + b"\0" + struct.pack("<H", crc16(data))
        self.transfer(command, args, out=data)

    def start(self, blob):
        if not blob:
            raise ValueError(f"Empty {self.chip} loader")
        address = CHIPS[self.chip].address
        self.progress("Loading flash helper", 0, len(blob))
        self.report(f"{self.path}: uploading {self.chip} loader to RAM")
        for offset in range(0, len(blob), 512):
            self.write(0xfb06, address + offset, blob[offset:offset + 512])
            self.progress("Loading flash helper", min(offset + 512, len(blob)), len(blob))
        self.execute(0xfb08, struct.pack(">IH", address, 1))
        for attempt in range(20):
            try:
                info = self.execute(0xfc0a)
                break
            except OSError:
                if attempt == 19:
                    raise
                time.sleep(0.1)
        self.flash_id = int.from_bytes(info[2:5], "little")
        capacity = self.flash_id & 31
        if not 16 <= capacity <= 24:
            raise ValueError(f"Invalid flash capacity in ID {self.flash_id:06x}")
        self.flash_size = 1 << capacity
        try:
            limit = int.from_bytes(self.execute(0xfc14)[:4], "big")
            if 64 <= limit <= 0x8000:
                self.io = limit
        except OSError:
            pass
        record = self.execute(0xfc09, struct.pack(">I", 0xac6900))
        self.key = decode_chip_key(bytes([record[5], record[4]]))
        self.report(f"{self.path}: {self.flash_size // 1024} KiB flash, chip key 0x{self.key:04x}")

    def read(self, address, size, progress=False, stage="Reading flash"):
        if address < 0 or size < 0 or address + size > self.flash_size:
            raise ValueError("Read exceeds flash capacity")
        data = bytearray()
        if progress:
            self.progress(stage, 0, size)
        for offset in range(0, size, self.io):
            count = min(self.io, size - offset)
            data.extend(self.transfer(0xfd05, struct.pack(">IH", address + offset, count), count))
            if progress and (offset // 0x10000 != (offset + count) // 0x10000 or offset + count == size):
                self.report(f"Read {offset + count}/{size} bytes")
                self.progress(stage, offset + count, size)
        return bytes(data)

    def pid(self):
        return image_identity(self.read(0, min(self.flash_size, 0x11000)), require_key=False)[0]

    def backup(self, file):
        data = self.read(0, self.flash_size, progress=True, stage="Backing up flash")
        with open(file, "wb") as stream:
            if stream.write(data) != len(data):
                raise OSError("Incomplete flash backup write")
            stream.flush()
            # A failed backup must abort before any erase.
            os.fsync(stream.fileno())
        self.report(f"Saved full flash backup: {file} ({len(data)} bytes)")

    def flash(self, data, backup=None):
        pid, key = image_identity(data)
        if pid not in PRODUCTS:
            raise ValueError(f"Firmware PID {pid} is not a known pad or dongle")
        if self.pid() != pid:
            raise ValueError(f"Firmware PID {pid} does not match the device's flash")
        if self.flash_size != PRODUCTS[pid].flash_size or not data or len(data) > self.flash_size:
            raise ValueError(f"Image or device flash size does not match the {product_name(pid)}")
        if key != self.key:
            self.progress("Converting firmware chip key")
            self.report(f"Converting image key 0x{key:04x} to device key 0x{self.key:04x}")
            data = rekey_image(data, self.key)
        if backup:
            self.backup(backup)
        top = (len(data) + 4095) & ~4095
        self.progress("Erasing flash", 0, top)
        self.report(f"Erasing 0x0–0x{top:x}")
        at = 0
        while at < top:
            block = at % 0x10000 == 0 and top - at >= 0x10000
            self.execute(0xfb00 if block else 0xfb01, struct.pack(">I", at))
            at += 0x10000 if block else 0x1000
            self.progress("Erasing flash", at, top)
        self.progress("Writing firmware", 0, len(data))
        for at in range(0, len(data), self.io):
            chunk = data[at:at + self.io]
            if any(byte != 255 for byte in chunk):
                self.write(0xfb04, at, chunk)
            if at // 0x4000 != (at + len(chunk)) // 0x4000 or at + len(chunk) == len(data):
                self.report(f"Written {at + len(chunk)}/{len(data)} bytes")
                self.progress("Writing firmware", at + len(chunk), len(data))
        self.report("Verifying flash readback")
        if self.read(0, len(data), progress=True, stage="Verifying firmware") != data:
            raise OSError("Flash readback differs; device left in loader for recovery")
        self.report("Written and verified")
        self.progress("Restarting device")
        try:
            self.execute(0xfc0c, struct.pack(">I", 1))
        except OSError:
            pass  # The successful reboot normally disconnects before replying.


def inspect_loader(path, source, report=print, progress=None, expected_location=None):
    """Read the selected ROM loader's actual on-flash PID; never enter another device."""
    if expected_location and usb_location(path) != expected_location:
        raise OSError("Selected ROM loader moved or its device path was reused; refresh before continuing")
    inquiry = scsi(path, [0x12, 0, 0, 0, 36], 36, 1000)
    chip = inquiry[8:12].decode(errors="replace") if len(inquiry) == 36 else ""
    if chip not in CHIPS or inquiry[16:21] != b"UBOOT":
        raise OSError("Selected device is no longer a ROM loader; refresh before continuing")
    loader = Loader(path, report, progress, chip)
    if expected_location:
        loader.location = expected_location
    loader.start(loader_blob(source, chip))
    try:
        loader.product_id = loader.pid()
    except ValueError:
        loader.product_id = None
    report(f"{path}: product ID {loader.product_id or 'unavailable'}")
    return loader


def select_loader(pid, source, wait=30, report=print, device_path=None, progress=None, expected_location=None):
    """Enter loader mode and match PID, or restrict to an already-selected ROM path.
    Running firmware for pid is asked to restart: ours (IDENTIFY names its PID),
    other Jieli firmware, and stock pads in a USB mode with a loader command."""
    if pid not in PRODUCTS or not source:
        raise ValueError("Select a known product and the RAM loaders (the SDK or a folder)")
    deadline = time.monotonic() + wait
    seen, asked = {}, set()
    report(f"Looking for a {product_name(pid)}; {PRODUCTS[pid].by_hand}")
    while True:
        for path in device_paths():
            if device_path is not None and path != device_path:
                continue
            if device_path and expected_location and usb_location(path) != expected_location:
                raise OSError("Selected ROM loader moved or its device path was reused; refresh before continuing")
            if path in seen:
                continue
            if is_tool(path):        # our firmware: its IDENTIFY names its PID
                if device_path is not None:
                    raise OSError("Selected ROM loader changed mode; refresh devices before flashing")
                try:
                    ident = scsi(path, [0xf5, 0], 64, 1000)
                    if len(ident) < 56 or ident[:4] != b"C2PD" or \
                            ident[40:56].split(b"\0")[0].decode(errors="replace") != pid:
                        continue
                    report(f"{path}: requesting ROM loader")
                    scsi(path, [0xfc, 0x0a] + [255] * 14, 16, 1000)
                except OSError:
                    pass  # A reboot can disconnect without a reply.
                continue
            try:
                inquiry = scsi(path, [0x12, 0, 0, 0, 40], 40, 1000)
            except OSError:
                continue
            chip = inquiry[8:12].decode(errors="replace") if len(inquiry) >= 36 else ""
            if chip not in CHIPS:
                continue
            if inquiry[16:21] != b"UBOOT":   # other Jieli SDK firmware: any loader opcode
                if device_path is not None:
                    raise OSError("Selected ROM loader changed mode; refresh devices before flashing")
                try:
                    report(f"{path}: requesting ROM loader")
                    scsi(path, [0xfc, 0x0a] + [255] * 14, 16, 1000)
                except OSError:
                    pass  # A reboot can disconnect without a reply.
                continue
            loader = Loader(path, report, progress, chip)
            if expected_location:
                loader.location = expected_location
            loader.start(loader_blob(source, chip))
            try:
                held = loader.pid()
            except ValueError:
                held = None
            report(f"{path}: holds {held or 'no recognized firmware header'}")
            seen[path] = (loader, held)
        if device_path is None:
            for hid, entry in stock_devices(padhid.devices()):
                if entry.pid == pid and entry.usage_page and hid["path"] not in asked:
                    asked.add(hid["path"])
                    report(f"{hid['path']}: stock {product_name(pid)}, requesting ROM loader")
                    try:
                        padhid.write(hid["path"], entry.report.ljust(entry.length, b"\0"))
                    except OSError:
                        pass
        matching = [loader for loader, held in seen.values() if held == pid]
        if len(matching) > 1:
            raise ValueError("Multiple matching ROM loaders; connect only the intended device")
        if matching:
            if matching[0].flash_size != PRODUCTS[pid].flash_size:
                raise ValueError(f"Device flash capacity does not match the {product_name(pid)}")
            return matching[0]
        if time.monotonic() >= deadline:
            raise OSError(f"No ROM loader holding {pid} found")
        time.sleep(0.5)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("command", choices=["flash", "backup"])
    p.add_argument("file", type=Path)
    p.add_argument("--pid", choices=PRODUCTS, default="GS_C2_ADC_DEVICE")
    p.add_argument("--loader", type=Path, default=DEFAULT_LOADER)
    p.add_argument("--backup", type=Path, help="optional full flash backup before flashing")
    args = p.parse_args()
    if args.command == "flash":
        if args.backup and args.backup.resolve() in (args.file.resolve(), args.loader.resolve()):
            p.error("--backup must be a different file from the image and loader")
        data = load_image(args.file)
        if image_identity(data)[0] != args.pid:
            p.error("image does not match the selected PID")
    elif args.file.resolve() == args.loader.resolve():
        p.error("backup must not overwrite the loader")
    loader = select_loader(args.pid, args.loader)
    if args.command == "backup":
        loader.backup(args.file)
        print("Reconnect USB to leave the loader")
    else:
        loader.flash(data, args.backup)


if __name__ == "__main__":
    main()
