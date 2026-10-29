"""The 8BitDo boot loader (USB 2dc8:3208, "8BitDo Boot", HID): find it,
flash our firmware through it, leave it (docs/8bitdo-ultimate2c-wireless/).

Its protocol: 64-byte reports without an ID, 05 then <u16 command, u16 0,
u16 length, u16 CRC-16/MODBUS of the data, u32 total, u32 address> and up to
46 data bytes; each reply echoes the header with a status in place of the 0.
Commands: 3 program, 4 erase a 4 KiB sector, 5 read, 7 start the app (what
is at 0x6000; no reply), 8 identify (Ultimate 2C: 1b 30).  Its erase skips
sectors below 0x6000 (the loader's own).

The image it takes from us is flash.bin: the updater (a packed Telink app,
KNLT header, its size at 0x18 and its CRC at the end), then, a sector
further on, update.bin ("MAIN" header naming the product, then the app), all
from 0x6000."""
import struct
import time

import padhid

LOADER = (0x2dc8, 0x3208)
MODELS = {"U2C_WIRELESS": 0x301b, "U2C_RECEIVER": 0x301c}
SECTOR = 4096
CHUNK = 46
START = 0x6000                   # where the boot loader starts an app
END = 0x6f000                    # the U2C's settings from here (its APP_END)


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def crc32(data):
    """CRC-32 without its final inversion, as the boot loader checks."""
    import zlib
    return zlib.crc32(data) ^ 0xFFFFFFFF


def check(image):
    """The checks the boot loader makes of the packed app at the image's
    start, as far as known; what follows it is the image's own."""
    if len(image) < 32 or image[8:12] != b"KNLT":
        raise ValueError("bad image header (no KNLT at 8)")
    size = struct.unpack_from("<I", image, 0x18)[0]
    if not 8 <= size <= len(image):
        raise ValueError("bad image size")
    if struct.unpack_from("<I", image, size - 4)[0] != crc32(image[:size - 4]):
        raise ValueError("bad image CRC")


def image_pid(image):
    """The product a flash.bin is for: check() the updater, then the PID in
    the MAIN header of the update.bin a sector boundary after it."""
    check(image)
    size = struct.unpack_from("<I", image, 0x18)[0]
    for at in range(-(-size // SECTOR) * SECTOR, len(image) - 40, SECTOR):
        if image[at:at + 4] == b"MAIN":
            return image[at + 24:at + 40].split(b"\0")[0].decode(errors="replace")
    raise ValueError("not a full image (no update.bin after the updater): use flash.bin")


def model_pid(model):
    return next((p for p, m in MODELS.items() if model == struct.pack("<H", m)), None)


class Loader:
    def __init__(self, path):
        self.dev = padhid.Device(path)

    def close(self):
        self.dev.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def request(self, command, address, data=b"", length=None, timeout=10):
        report = bytearray(64)
        report[0] = 5
        count = len(data) if length is None else length
        total = SECTOR if command in (3, 4) else count
        struct.pack_into("<HHHHII", report, 1, command, 0, count, crc16(data) if data else 0, total, address)
        report[17:17 + len(data)] = data
        self.dev.drain()
        self.dev.write(b"\0" + report)
        if command == 7:
            return b""
        deadline = time.monotonic() + timeout
        while True:
            reply = self.dev.read(deadline - time.monotonic())
            if reply is None:
                raise TimeoutError(f"no answer to command {command} at {address:#x}")
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
        """Leave the loader: it starts what is at 0x6000 (no reply)."""
        self.request(7, 0)

    def write_image(self, image, start=START, end=END, progress=None):
        """Every sector the image covers, read back (one that already holds
        its part is left as it is); its first last, so a write cut short
        leaves no header the loader would start."""
        stop = start + len(image) + (-len(image) % SECTOR)
        if stop > end:
            raise ValueError(f"image ends at {stop:#x}, past {end:#x}")
        padded = image + b"\xff" * (stop - start - len(image))
        sectors = list(range(start + SECTOR, stop, SECTOR)) + [start]
        for i, address in enumerate(sectors):
            if progress:
                progress(i, len(sectors))
            offset = address - start
            data = padded[offset:offset + SECTOR]
            if self.read(address, SECTOR) != data:
                self.write_sector(address, data)
        if progress:
            progress(len(sectors), len(sectors))


def find():
    """The boot loaders on USB: (path, USB port or None, product or None),
    asking each its model."""
    found = []
    for hid in padhid.devices():
        if (hid["vid"], hid["product"]) != LOADER:
            continue
        try:
            with Loader(hid["path"]) as loader:
                pid = model_pid(loader.identify())
        except OSError:
            continue
        found.append((hid["path"], hid["location"], pid))
    return found


def flash(path, image, progress=None):
    """flash.bin into the loader at path, for the product it names; then
    start it.  progress(done, total) per sector."""
    pid = image_pid(image)
    with Loader(path) as loader:
        model = model_pid(loader.identify())
        if model != pid:
            raise ValueError(f"the image is for {pid}, the boot loader's pad is {model or 'unknown'}")
        loader.write_image(image, progress=progress)
        loader.start_app()


def leave(path):
    """Out of the loader without flashing: start the app that is there."""
    with Loader(path) as loader:
        loader.identify()
        loader.start_app()
