"""BR23 RAM-loader protocol and raw image flashing, shared by GUI and CLI.

Ported from our jlnewfw implementation. No external flashing executable is
needed. A supplied image must already be packed for the connected chip's key.
"""
import argparse
import binascii
import os
from pathlib import Path
import struct
import time

from padtransport import device_paths, scsi

PIDS = {"pad": "GS_C2_ADC_DEVICE", "dongle": "GS_C2_Dongle"}
FLASH_SIZES = {"pad": 0x100000, "dongle": 0x80000}
PACKAGED_LOADER = Path(__file__).resolve().parent / "loaders/br23loader.bin"
SDK_LOADER = Path(os.environ.get("JIELI_SDK", "/shared/github.com-Jieli-Tech/fw-AC63_BT_SDK")) / "cpu/br23/tools/br23loader.bin"
DEFAULT_LOADER = PACKAGED_LOADER if PACKAGED_LOADER.is_file() else SDK_LOADER


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


def image_identity(data, require_key=True):
    """Same boot-image selection as jlnewfw, including EOFFSET stale copies."""
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
    def __init__(self, path, report=print):
        self.path = path
        self.report = report
        self.io = 512

    def transfer(self, command, args=b"", length=0, out=None):
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
            raise ValueError("Empty BR23 loader")
        self.report(f"{self.path}: uploading BR23 loader to RAM")
        for offset in range(0, len(blob), 512):
            self.write(0xfb06, 0x12000 + offset, blob[offset:offset + 512])
        self.execute(0xfb08, struct.pack(">IH", 0x12000, 1))
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

    def read(self, address, size, progress=False):
        if address < 0 or size < 0 or address + size > self.flash_size:
            raise ValueError("Read exceeds flash capacity")
        data = bytearray()
        for offset in range(0, size, self.io):
            count = min(self.io, size - offset)
            data.extend(self.transfer(0xfd05, struct.pack(">IH", address + offset, count), count))
            if progress and (offset // 0x10000 != (offset + count) // 0x10000 or offset + count == size):
                self.report(f"Read {offset + count}/{size} bytes")
        return bytes(data)

    def pid(self):
        return image_identity(self.read(0, min(self.flash_size, 0x11000)), require_key=False)[0]

    def backup(self, file):
        data = self.read(0, self.flash_size, progress=True)
        with open(file, "wb") as stream:
            if stream.write(data) != len(data):
                raise OSError("Incomplete flash backup write")
            stream.flush()
            # A failed backup must abort before any erase.
            os.fsync(stream.fileno())
        self.report(f"Saved full flash backup: {file} ({len(data)} bytes)")

    def flash(self, data, board, backup=None):
        pid, key = image_identity(data)
        if pid != PIDS[board] or self.pid() != pid:
            raise ValueError("Firmware PID does not match the selected board and device")
        if key != self.key:
            raise ValueError(f"Image key 0x{key:04x} differs from device key 0x{self.key:04x}; pack a build for this device")
        if self.flash_size != FLASH_SIZES[board] or not data or len(data) > self.flash_size:
            raise ValueError("Image or device flash size does not match the selected board")
        if backup:
            self.backup(backup)
        top = (len(data) + 4095) & ~4095
        self.report(f"Erasing 0x0–0x{top:x}")
        at = 0
        while at < top:
            block = at % 0x10000 == 0 and top - at >= 0x10000
            self.execute(0xfb00 if block else 0xfb01, struct.pack(">I", at))
            at += 0x10000 if block else 0x1000
        for at in range(0, len(data), self.io):
            chunk = data[at:at + self.io]
            if any(byte != 255 for byte in chunk):
                self.write(0xfb04, at, chunk)
            if at // 0x10000 != (at + len(chunk)) // 0x10000 or at + len(chunk) == len(data):
                self.report(f"Written {at + len(chunk)}/{len(data)} bytes")
        self.report("Verifying flash readback")
        if self.read(0, len(data), progress=True) != data:
            raise OSError("Flash readback differs; device left in loader for recovery")
        self.report("Written and verified")
        try:
            self.execute(0xfc0c, struct.pack(">I", 1))
        except OSError:
            pass  # The successful reboot normally disconnects before replying.


def select_loader(board, blob, wait=30, report=print, device_path=None):
    """Enter loader mode and match PID, or restrict to an already-selected ROM path."""
    if board not in PIDS or not blob:
        raise ValueError("Select a supported board and a nonempty BR23 loader")
    deadline = time.monotonic() + wait
    seen = {}
    report(f"Looking for {board}; if needed, connect it while holding its loader buttons")
    while True:
        for path in device_paths():
            if device_path is not None and path != device_path:
                continue
            if path in seen:
                continue
            try:
                inquiry = scsi(path, [0x12, 0, 0, 0, 36], 36, 1000)
            except OSError:
                continue
            if len(inquiry) != 36 or inquiry[8:12] != b"BR23":
                continue
            if inquiry[16:21] != b"UBOOT":
                if device_path is not None:
                    raise OSError("Selected ROM loader changed mode; refresh devices before flashing")
                try:
                    ident = scsi(path, [0xf5, 0], 64, 1000)
                    if ident[:4] == b"C2PD" and len(ident) >= 7 and ident[6] != (board == "dongle"):
                        continue
                    report(f"{path}: requesting ROM loader")
                    scsi(path, [0xfc, 0x0a] + [255] * 14, 16, 1000)
                except OSError:
                    pass  # A reboot can disconnect without a reply.
                continue
            loader = Loader(path, report)
            loader.start(blob)
            try:
                pid = loader.pid()
            except ValueError:
                pid = None
            report(f"{path}: holds {pid or 'no recognized firmware header'}")
            seen[path] = (loader, pid)
        matching = [loader for loader, pid in seen.values() if pid == PIDS[board]]
        if len(matching) > 1:
            raise ValueError("Multiple matching ROM loaders; connect only the intended board")
        if matching:
            if matching[0].flash_size != FLASH_SIZES[board]:
                raise ValueError("Device flash capacity does not match selected board")
            return matching[0]
        if time.monotonic() >= deadline:
            raise OSError(f"No ROM loader with {board} firmware PID found")
        time.sleep(0.5)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("command", choices=["flash", "backup"])
    p.add_argument("file", type=Path)
    p.add_argument("--board", choices=PIDS, default="pad")
    p.add_argument("--loader", type=Path, default=DEFAULT_LOADER)
    p.add_argument("--backup", type=Path, help="optional full flash backup before flashing")
    args = p.parse_args()
    if args.command == "flash":
        if args.backup and args.backup.resolve() in (args.file.resolve(), args.loader.resolve()):
            p.error("--backup must be a different file from the image and loader")
        data = load_image(args.file)
        if image_identity(data)[0] != PIDS[args.board]:
            p.error("image does not match selected board")
    elif args.file.resolve() == args.loader.resolve():
        p.error("backup must not overwrite the loader")
    import padtool
    curve_backup = padtool.prepare_curve_upgrade(args.board, args.backup) if args.command == "flash" and args.backup else None
    loader = select_loader(args.board, args.loader.read_bytes())
    if args.command == "backup":
        loader.backup(args.file)
        print("Reconnect USB to leave the loader")
    else:
        loader.flash(data, args.board, args.backup)
        padtool.finish_curve_upgrade(curve_backup)


if __name__ == "__main__":
    main()
