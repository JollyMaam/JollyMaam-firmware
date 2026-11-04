# Image commands (`jollymaam jieli`)

The project's tool for the BR23 image format
([../jieli/re-application-image.md](../jieli/re-application-image.md)), built
for each board. Commands:

| Command | Does |
|---|---|
| `list` | show the JLFS entries of an image |
| `unpack` | write an image's files and an `isd_config.ini` |
| `pack` | build an image from files and an `isd_config.ini` |
| `flash` | write an image through the ROM loader |
| `flashpack` | `pack`, then `flash`, using the connected chip's key |
| `backup` | read the whole flash through the ROM loader |
| `loader` | ask running firmware to enter the ROM loader, then wait for the loader holding `--pid` |
| `run` | a device in its ROM loader starts the firmware in its flash (the run command that ends a flash) |
| `scan` | list what is connected by USB port, classified as below; nothing is restarted |

Options used by the build:

- `--flash-size` and `--code-limit`: fix the layout so reserved regions do not
  move when code size changes.
- `--loader`: the SDK folder (each chip's RAM loader is `cpu/<chip>/tools/<chip>loader.bin`), a folder of `<chip>loader.bin` files, or one such file. The chip is the ROM loader's INQUIRY vendor: `BR23` (loader at `0x12000`) or `BD19` (at `0x2000`).
- `-c`: `isd_config.ini`. `-u`: `uboot.boot`.
- `--enter-loader --wait 30`: ask running firmware to enter the ROM loader,
  then wait for it (classes below).
- `-k`: re-encrypt for the target chip's key. `flashpack` does this by default.
- `--main OFFSET:FILE`, `--main-out FILE`: a pad's app above the app
  area, behind its header; `--main-out` also writes header and app in the
  clear ([updater.md](updater.md)).
- `--force-pid`: allow a write to a device whose PID differs (a blank device or a
  deliberate change).

`flash` writes only to a device whose flash holds the image's PID (the
Cyclone 2 PIDs are in [../gamesir-cyclone2/flash-map.md](../gamesir-cyclone2/flash-map.md)).

Unpack followed by pack, with `--at 0`, reproduces this project's build byte
for byte.

`unpack` writes into the `[JLNEWFW]` section what `pack` cannot derive: the
files and resource areas (`RES`, `name:82` for an area with flags `0x82`),
`UBOOT_FLAGS`, `HEADER_FS` (flash-header byte 12), and `AREAS`, the app
area's reserved list as found (`offset:size:flags:reserved:crc:name field`).
With `AREAS` given, `pack` writes that list instead of computing VM, PRCT and
BTIF. The GameSir Nova 2 Lite's stock pad and dongle images unpack and pack
back byte for byte, except the 32 bytes before VM that GameSir's updater
stamps (see the Cyclone 2's flash map); `flash --keep` carries them over.

## Telling devices apart

| Class | Known by | PID from | Into the loader |
|---|---|---|---|
| ROM loader | INQUIRY vendor `BR23`, product `UBOOT1.00` | the flash, once the RAM loader runs | — |
| this firmware's tool device | HID usage page `0xffc2`, IDENTIFY answers `C2PD` | IDENTIFY | a loader command; only for the wanted PID |
| other Jieli SDK firmware | INQUIRY `BR23`, no IDENTIFY | the flash, once in the loader | a loader command |
| stock pad, HID only | VID, PID, `bcdDevice`, strings, and the usage page of the interface taking the command | `jollymaam`'s table | the table's output report, in the modes that have one |

The stock pads' entries are the Cyclone 2's and the Nova 2 Lite's modes from
[../gamesir-cyclone2/stock-pad-dongle.md](../gamesir-cyclone2/stock-pad-dongle.md)
and [../gamesir-nova-2-lite/](../gamesir-nova-2-lite/README.md). A pad in a
mode without a command is named, with the mode that has one.

`flash`, `loader` and `backup` first survey everything connected. A
candidate is a device for the wanted PID: a ROM loader holding it (read
through the RAM loader), our firmware built for it, or a stock pad of it in
any USB mode; Jieli firmware of unknown PID only when nothing else is.
`--force-pid` takes any PID. More than one candidate is refused before any
device is restarted, listing them; `--device` (make: `DEVICE=`) keeps the
one on that USB port or at that node. The one left is restarted, and its ROM
loader is found again on the same USB port: Linux's sysfs port name
("5-2.1"), Windows' location information ("Port_#0002.Hub_#0003"). The
loader has no serial number.

## The tool device

This firmware's tool device, on every board, is a vendor HID interface:

- V-USB's shared ID for generic HID, `16c0:05df`; manufacturer `JollyMaam`,
  the model as product (e.g. "Nova 2 Lite"), the flash UID in hex as serial;
- one top-level collection, usage page `0xffc2`, with one feature report
  (512 bytes on the Jieli chips, 356 on the B80, 64 on the YC3121), one
  64-byte input report on an interrupt IN endpoint polled every 1 ms, and no
  report IDs;
- a command is a set of the feature report: the 16-byte CDB
  (`src/core/usb.c`), u16 length, flags (bit 0: data to the device; bit 1:
  the answer on input reports, which every command sets: one without it is
  dropped), a tag, a u32 client, then the data (at most the report less 24
  bytes); the device goes by the request's length, so a set may stop after
  the data;
- the answer comes in input reports: the client, the tag, a state (2 a
  piece and more, 3 the last, 4 refused: another command is running, send
  it again), u16 offset, then 56 bytes of the answer.  Every program with
  the device open receives every input report, so several programs share
  one device, each taking the reports with its own client.  A get of the
  feature report is refused;
- IDENTIFY (`0xf5 0x00`, 56 bytes) has the image's PID at byte 40.

Tools select candidates by the usage page and confirm running firmware by the
`C2PD` signature in IDENTIFY, rather than its display names. A command whose
first byte is not `0xf5` restarts a Jieli pad into its ROM loader. ROM loaders
are mass storage, with their chip vendor and `UBOOT1.00` inquiry product.
