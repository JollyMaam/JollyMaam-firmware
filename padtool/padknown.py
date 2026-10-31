"""The pads and dongles padtool knows: by image PID, and stock firmware by its
USB identity in each USB mode, with how each enters its ROM loader.

Mirrors jollymaam's table (tools/jollymaam_jieli.c) and the board pages under docs/.
"""
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Product:
    name: str
    kind: str                    # "pad" or "dongle"
    flash_size: int              # what our image for it declares
    by_hand: str = ""            # how to enter the ROM loader without a command


@dataclass(frozen=True)
class Chip:
    address: int                 # where its RAM loader runs
    blob: str                    # the SDK's RAM loader for it
    sdk_dir: str                 # under the SDK's cpu/


# By a ROM loader's INQUIRY vendor (the BD19's from kagaimiq's jl-uboot-tool
# data; docs/gamesir-nova-2-lite/).
CHIPS = {
    "BR23": Chip(0x12000, "br23loader.bin", "br23"),
    "BD19": Chip(0x2000, "bd19loader.bin", "bd19"),
}


# By the image PID: what the flash holds, what our firmware reports.
PRODUCTS = {
    "GS_C2_ADC_DEVICE": Product("Cyclone 2", "pad", 0x100000,
                                "Hold Home + Start while connecting USB."),
    "GS_C2_Dongle": Product("Cyclone 2 Dongle", "dongle", 0x80000,
                            "This firmware: hold the dongle's button while connecting USB."),
    "AC695X_TRANS": Product("Nova 2 Lite", "pad", 0x100000,
                            "Stock firmware: switch it to Xbox 360 mode (green) and use Enter flash loader."),
    "AC632N_TRANS": Product("Nova 2 Lite Dongle", "dongle", 0x80000,
                            "Hold its pairing button while connecting USB, and on after."),
}


@dataclass(frozen=True)
class StockMode:
    pid: str                     # PRODUCTS key; "" for a device with no known loader
    name: str                    # product name, for devices not in PRODUCTS
    mode: str
    vid: int
    product: int
    bcd: int
    manufacturer: str = None     # None: any
    product_name: str = None
    usage_page: int = 0          # the HID interface taking the command; 0: none
    report: bytes = b""          # report ID byte (0: none), then the command
    length: int = 0              # bytes written: the ID byte and the report
    note: str = ""


# Stock firmware that shows itself only as HID (docs/gamesir-cyclone2/
# stock-pad-dongle.md, docs/gamesir-nova-2-lite/README.md).
STOCK = [
    StockMode("GS_C2_ADC_DEVICE", "", "Xbox 360 (green)", 0x3537, 0x1053, 0x0352,
              usage_page=0xfff0, report=bytes([0x0f, 0x17, 0x55, 0x88]), length=64),
    StockMode("GS_C2_ADC_DEVICE", "", "Switch (red)", 0x057e, 0x2009, 0x0352, product_name="Gamepad"),
    StockMode("GS_C2_ADC_DEVICE", "", "DS4 (blue)", 0x054c, 0x09cc, 0x0352,
              manufacturer="Guangzhou Chicken Run Network Technology Co., Ltd."),
    StockMode("AC695X_TRANS", "", "Xbox 360 (green)", 0x3537, 0x100f, 0x0100,
              usage_page=0xff7a, report=bytes([0x00, 0xf2, 0x00, 0x04, 0xf6]), length=65),
    StockMode("AC695X_TRANS", "", "Switch (red)", 0x057e, 0x2009, 0x0200,
              manufacturer="Zikway", product_name="Pro Controller"),
    StockMode("AC695X_TRANS", "", "DS4 (blue)", 0x054c, 0x09cc, 0x0100,
              manufacturer="Sony Computer Entertainment", product_name="Wireless Controller"),
    StockMode("AC632N_TRANS", "", "", 0x3537, 0x2106, 0x0100,
              manufacturer="Zikway", product_name="HID zk"),
]


# Products with no ROM loader of this kind, by name only (the 8BitDo's boot
# loader: pad8bitdo).
NAMES = {"U2C_WIRELESS": "Ultimate 2C Wireless", "U2C_RECEIVER": "Ultimate 2C receiver"}


def product_name(pid):
    product = PRODUCTS.get(pid)
    return product.name if product else NAMES.get(pid) or pid or "Unknown"


def stock_name(entry):
    return entry.name or product_name(entry.pid)


def has_usage_page(descriptor, page):
    """Whether a HID report descriptor has a usage page item for page."""
    i = 0
    while i < len(descriptor):
        prefix = descriptor[i]
        if prefix == 0xfe:                      # long item: size in the next byte
            i += 3 + (descriptor[i + 1] if i + 1 < len(descriptor) else 0)
            continue
        size = (0, 1, 2, 4)[prefix & 3]
        value = int.from_bytes(descriptor[i + 1:i + 1 + size], "little")
        if prefix & 0xfc == 0x04 and value == page:
            return True
        i += 1 + size
    return False


def match_stock(hid):
    """The STOCK entry a HID interface is (padhid.devices() item), or None.
    An entry with a command matches only the interface that takes it."""
    for entry in STOCK:
        if (hid["vid"], hid["product"], hid["bcd"]) != (entry.vid, entry.product, entry.bcd):
            continue
        if entry.manufacturer is not None and hid["manufacturer"] != entry.manufacturer:
            continue
        if entry.product_name is not None and hid["product_name"] != entry.product_name:
            continue
        if entry.usage_page:
            if "usage_page" in hid:             # Windows: one collection per interface
                if hid["usage_page"] != entry.usage_page or hid.get("out_len") != entry.length:
                    continue
            elif not has_usage_page(hid.get("descriptor", b""), entry.usage_page):
                continue
        return entry
    return None


def command_mode(pid):
    """The mode of a stock product that takes the loader command."""
    return next((e.mode for e in STOCK if e.pid == pid and e.usage_page), None)


def stock_devices(hids):
    """One (hid, entry) per USB device: the interface taking the command first."""
    found = {}
    for hid in hids:
        entry = match_stock(hid)
        if entry is None:
            continue
        key = hid.get("location") or hid["path"]
        if key not in found or (entry.usage_page and not found[key][1].usage_page):
            found[key] = (hid, entry)
    return list(found.values())


# A product ID's build: its folder in the zip's firmware and in build/.
BUILDS = {"GS_C2_ADC_DEVICE": "gamesir-cyclone-2-pad", "GS_C2_Dongle": "gamesir-cyclone-2-dongle",
          "AC695X_TRANS": "gamesir-nova-2-lite-pad", "AC632N_TRANS": "gamesir-nova-2-lite-dongle",
          "U2C_WIRELESS": "8bitdo-ultimate2c-wireless-pad"}


def firmware_dir(pid=None):
    """Where firmware images are: the zip's firmware folder, else
    this tree's build; that product's own folder when known.  None: the
    dialog's own choice."""
    here = Path(__file__).resolve().parent.parent
    for folder in (here / "firmware", here / "build"):
        if folder.is_dir():
            board = folder / BUILDS.get(pid or "", "")
            return str(board if pid in BUILDS and board.is_dir() else folder)
    return None
