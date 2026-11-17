# jollymaam

The project's tool for its firmware, built into `build/host/` on
libjollymaam (`jollymaam help` lists the commands, `jollymaam help COMMAND`
explains one). The build makes its images with it (`pack`) and the package
(`package`). On a pad: `list`, `install` (a package, a backup or a vendor
image, through the vendor loader, after a whole-flash backup), `update`
(through our updater), `backup` and `reboot`.

## Images

`pack jieli` builds a Jieli flash image
([../jieli/re-application-image.md](../jieli/re-application-image.md)) from
an `isd_config.ini` (`-c`), `uboot.boot` (`-u`) and the app files.
`--flash-size` and `--code-limit` fix the layout so reserved regions do not
move when code size changes; `--main OFFSET:FILE` puts a pad's app above the
app area behind its header, and `--main-out` writes header and app in the
clear ([updater.md](updater.md)). `pack telink` makes a Telink app the 8BitDo
boot loader starts; `pack join` puts the Telink updater and update.bin into
one image. `install` re-encrypts a Jieli image for the connected chip's key.

Unpack followed by pack, with `--at 0`, reproduces this project's build byte
for byte.

`unpack` writes into the `[JLNEWFW]` section what `pack` cannot derive: the
files and resource areas (`RES`, `name:82` for an area with flags `0x82`),
`UBOOT_FLAGS`, `HEADER_FS` (flash-header byte 12), and `AREAS`, the app
area's reserved list as found (`offset:size:flags:reserved:crc:name field`).
With `AREAS` given, `pack` writes that list instead of computing VM, PRCT and
BTIF. The GameSir Nova 2 Lite's stock pad and dongle images unpack and pack
back byte for byte, except the 32 bytes before VM that GameSir's updater
stamps (see the Cyclone 2's flash map).

## Telling devices apart

| Class | Known by | Product from | Into the vendor loader |
|---|---|---|---|
| Jieli ROM loader | INQUIRY vendor `BR23` or `BD19`, product `UBOOT1.00` | the flash, once the RAM loader runs | — |
| 8BitDo boot loader | USB `2dc8:3208` | the model its identify answers | — |
| this firmware's tool device | HID usage page `0xffc2`, IDENTIFY answers `C2PD` | IDENTIFY | a loader command |
| other Jieli SDK firmware | INQUIRY `BR23`, no IDENTIFY | the flash, once in the loader | — |
| stock firmware | VID, PID, `bcdDevice`, strings, and the interface taking the command | libjollymaam's table, or (8BitDo) the model it answers | the table's report, in the modes that have one |

The stock entries are the Cyclone 2's and the Nova 2 Lite's modes from
[../gamesir-cyclone2/stock-pad-dongle.md](../gamesir-cyclone2/stock-pad-dongle.md)
and [../gamesir-nova-2-lite/](../gamesir-nova-2-lite/README.md), and the
8BitDo pads' vendor interface (output report `0x81`). A pad in a mode
without a command is named, with how to enter its loader by hand.

A device is identified by its USB port: Linux's sysfs port name ("5-2.1"),
Windows' location information ("Port_#0002.Hub_#0003"); `--device` (make:
`DEVICE=`) takes one. A device restarted into its loader is found again on
the same port. A Jieli ROM loader writes only the image for the product its
flash already names.

## Writing a Jieli pad

Each product has a protected range, its factory data (`products.c`): the
Cyclone 2 pad `0xfe000..0x100000` and dongle `0x7e000..0x80000` (BTIF and
key_mac), the Nova 2 Lite pad `0x78000..0x80000` (BTIF, EXIF, USERIF,
key_mac and the sector before them). `install` never erases it, except with
`--raw`:

| File | Written | The rest |
|---|---|---|
| a package | its install image, for the chip's key | erased (settings start empty) |
| a whole-flash backup of this pad | all of it | — |
| a vendor image (raw or UFW) | at 0 | erased |
| `--raw`: a whole-flash backup of any pad | all of it, the protected range included | — |

A backup whose protected range differs from the pad's is of another pad and
is refused without `--raw`. The Nova 2 Lite dongle has no protected range
known: a package's image is written alone, and a backup is restored only
raw.

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
- with the 64-byte report (the YC3121's), a command's data may run past one
  report: further sets carry the same head (the length the whole's) and the
  next part of the data, up to 512 bytes, and the device runs the command
  once all of it is in.  Another program's command meanwhile is refused; the
  same program's next command, or a second without a part, drops the half;
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
