#!/usr/bin/env python3
"""dongle_place: lay a GameSir dongle firmware out on flash as GameSir's tool does.

    dongle_place.py IMAGE OUT [--key KEY] [--keep DEVICE_DUMP]

IMAGE is a dongle firmware as distributed (donglefw_1.21.bin: built for flash
address 0, its app area encrypted with the chip key it was made for).  The
dongle boots it from 0x10000 (isd_config EOFFSET 16, in 4 KiB units), so the
tool, as read back from a dongle it upgraded:

  * moves the image to 0x10000 and points the app area's PRCT and VM entries
    at real flash addresses (PRCT 0x10000, VM from the image's end to BTIF);
  * puts the image's first 4 KiB (header, uboot.boot, isd_config) at 0x0000
    and 0x1000, and image[0x1000:0x8000] at 0x2000;
  * leaves BTIF and key_mac (the last 8 KiB) as they were;
  * stamps the last 32 bytes of PRCT (0x60fe0) with a record of its own:
    "GS_C2_Dongle" and 16 bytes that differ per device (6 at 0x60ff8).  The
    app does not contain the string; an older tool left another record
    there.  It is not understood, so --keep carries the device's over.

OUT is the whole flash.  --key re-encrypts the app area for another chip key
(the chip-key record in isd_config is not rewritten, so use it only to
compare with a dump: flash a device only with the key the image was made
for).  --keep copies BTIF, key_mac and the PRCT tail record from a dump of
the device; without it they are erased.

Checked against a dongle GameSir's tool upgraded to 1.21: with its key and
its dump, the output differs only in the isd_config chip-key record and in
VM (formatted by the running firmware).
"""
import argparse, sys

FLASH = 0x80000
BASE = 0x10000
APP = 0x1000          # app_dir_head, relative to the image
BTIF = 0x7e000


def crc16(p, crc=0):
    for b in p:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (0x1021 if crc & 0x8000 else 0)) & 0xffff
    return crc


def enc(p, key):
    out = bytearray(p)
    for i in range(len(out)):
        out[i] ^= key & 0xff
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xffff
    return out


def sfc(p, key):
    """The flash controller's cipher over an area, 32-byte blocks keyed by offset."""
    out = bytearray()
    for i in range(0, len(p), 32):
        out += enc(p[i:i + 32], key ^ (i >> 2))
    return out


def chip_key(image):
    """From the chip-key record at the start of isd_config.ini (jlnewfw's rule)."""
    d = image[0xf44:0xf44 + 34]
    if crc16(d[:32]) != d[32] | d[33] << 8:
        sys.exit("dongle_place: chip-key record CRC mismatch")
    s = sum(d[:16]) & 0xff
    t = 0xaa if s >= 0xe0 else 0x55 if s <= 0x10 else s
    return sum(1 << j for j in range(16) if (d[16 + j] ^ d[15 - j]) < t)


def entry(p):
    if crc16(p[2:32]) != p[0] | p[1] << 8:
        sys.exit("dongle_place: header CRC mismatch")
    return {"offset": int.from_bytes(p[4:8], "little"), "size": int.from_bytes(p[8:12], "little"),
            "name": bytes(p[16:32]).split(b"\0")[0].decode("ascii", "replace"), "index": p[14] | p[15] << 8}


def seal(p):
    p[0:2] = crc16(p[2:32]).to_bytes(2, "little")


def relocate(image, key, new_key):
    """Everything after the first 4 KiB (app area, then resources, then raw
    0xff padding up to the end of PRCT), plain, with PRCT and VM at flash
    addresses; and that re-encrypted.  The cipher counts from the app area's start, so the
    bytes do not depend on where on flash they sit."""
    head = sfc(image[APP:APP + 32], key)
    size = entry(head)["size"]
    plain = sfc(image[APP:APP + size], key)
    if crc16(plain[32:size]) != plain[2] | plain[3] << 8:
        sys.exit("dongle_place: app area CRC mismatch (wrong key?)")
    off = 32
    while True:
        e = entry(plain[off:off + 32])
        if e["name"] == "PRCT":
            prct = off
        if e["name"] == "VM":
            vm = off
        if e["index"]:
            break
        off += 32
    code = int.from_bytes(plain[prct + 8:prct + 12], "little")   # PRCT length: the image's code
    raw = bytes(image[APP:code]).ljust(code - APP, b"\xff")
    plain = sfc(raw, key)
    used = len(raw.rstrip(b"\xff"))      # then unencrypted padding
    plain[used:] = b"\xff" * (len(raw) - used)
    plain[prct + 4:prct + 8] = BASE.to_bytes(4, "little")
    plain[vm + 4:vm + 8] = (BASE + code).to_bytes(4, "little")
    plain[vm + 8:vm + 12] = (BTIF - BASE - code).to_bytes(4, "little")
    for o in (prct, vm):
        e = plain[o:o + 32]
        seal(e)
        plain[o:o + 32] = e
    plain[2:4] = crc16(plain[32:size]).to_bytes(2, "little")
    h = plain[0:32]
    seal(h)
    plain[0:32] = h
    area = sfc(plain, new_key)
    area[used:] = b"\xff" * (len(raw) - used)     # padding stays raw, as GameSir's tool leaves it
    return plain, area, code


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument("image")
    a.add_argument("out")
    a.add_argument("--key", type=lambda s: int(s, 0), help="chip key of the target (default: the image's)")
    a.add_argument("--keep", help="a dump of the device: BTIF and key_mac are taken from it")
    a.add_argument("--plain", help="also write the relocated app area, decrypted, here")
    args = a.parse_args()
    image = bytearray(open(args.image, "rb").read())
    key = chip_key(image)
    new_key = key if args.key is None else args.key
    plain, area, code = relocate(image, key, new_key)
    image = image[:APP] + area
    flash = bytearray(b"\xff" * FLASH)
    flash[BASE:BASE + len(image)] = image
    flash[0:0x1000] = image[0:0x1000]
    flash[0x1000:0x2000] = image[0:0x1000]
    flash[0x2000:0x9000] = image[0x1000:0x8000]
    if args.keep:
        dump = open(args.keep, "rb").read()
        flash[BTIF:] = dump[BTIF:FLASH]
        tail = BASE + code - 32
        flash[tail:tail + 32] = dump[tail:tail + 32]
    open(args.out, "wb").write(flash)
    if args.plain:
        open(args.plain, "wb").write(plain)
    print(f"key 0x{key:04x} -> 0x{new_key:04x}, image 0x{len(image):x} bytes at 0x{BASE:x}, "
          f"VM 0x{BASE + code:x}-0x{BTIF:x}")


if __name__ == "__main__":
    main()
