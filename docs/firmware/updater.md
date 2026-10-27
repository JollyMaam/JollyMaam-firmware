# Updater: the app rewritten in place

A board built as `NAME-split` (the Cyclone 2 pad: `gamesir-cyclone-2-pad-split`)
has two programs in its code region. The updater (`src/updater/updater.c`,
board `gamesir-cyclone-2-pad-updater`) is the image `uboot.boot` starts. The
app is linked `MAIN_AT` (`0x18000`) above it in the same code window. The
updater rewrites the app over the tool device and is never rewritten itself.
There is no second copy of the app.

## Flash layout (the pad)

| Flash offset | Contents |
|---|---|
| `0x000000..0x003000` | flash header, `uboot.boot`, `isd_config` |
| `0x003000..` | app area: its directory, the updater (`app.bin`), `cfg_tool.bin` |
| `0x01b000` | the app's header (`0x120` bytes), then the app |
| `0x0fa000..` | VM, BTIF, key_mac, as before |

The app's header is written by `jlnewfw pack --main`:

| Offset | Field |
|---|---|
| `0x00` | `MAIN` (`0x4e49414d`), the commit word |
| `0x04` | the app's size |
| `0x08` | CRC16 of the app (non-reflected CCITT, initial value 0) |
| `0x0c` | the app's entry, `0x1e00000 + MAIN_AT + 0x120` |

The app area's cipher runs on over the header and the app: a 32-byte block
`n` bytes from the app area's start uses key `chip_key ^ (n >> 2)`, as in the
app area itself.

## The app above the updater

The app is the normal build with the code window's origin moved up by
`MAIN_AT` (`code0` in the SDK linker script). It reads `cfg_tool.bin`, VM and
BTIF through the app area's directory, which is the updater's and lists the
same file and regions. A normal build and a split build's app differ only in
link address.

## Boot

`updater_entry()` is linked ahead of the SDK's start-up, at the entry
`uboot.boot` jumps to. Before any SDK code runs:

1. If the reset mailbox holds the updater's request (`UPDATER_STAY` and its
   complement at `0x20000`), it clears the request and continues into the
   SDK's start-up.
2. Otherwise, if the app's header has the commit word and the expected entry,
   it calls the app's entry with `uboot.boot`'s boot information (`r0`)
   unchanged. No hardware has been touched.
3. Otherwise it continues into the SDK's start-up: the updater runs.

The updater is the SDK and this firmware's platform layer without the radio
(`NAME_NO_BT`): the tool device, the hang watch, the board's lights. It powers
off after 60 s without a USB host.

The app's `UPDATER` command (`0x40`) sets the request and reboots. In a build
without an updater the request is ignored and the app starts again.

## Update commands

On the tool device (vendor SCSI `0xf5`), answered by the updater:

| Sub-command | Does |
|---|---|
| `0x41` UPDATE_INFO | state, last error, whether the app is valid, its flash offset, room, bytes written, size begun, chip key |
| `0x42` UPDATE_BEGIN | argument: the image's size (header and app); clears block protection, erases the header's sector |
| `0x43` UPDATE_DATA | argument: offset (the bytes written so far); up to 512 bytes of the image in the clear |
| `0x44` UPDATE_COMMIT | checks the header and the app's CRC through the code window, then programs the commit word |

The updater erases each 4 KiB sector as data reaches it, encrypts the data
for the chip, programs it, and reads it back through the code window. The
commit word is held back from the first block and programmed last, without
an erase. From BEGIN until COMMIT the app has no commit word, so a reset or
power loss comes back to the updater. An error stays until the next BEGIN.

The chip key does not read back from the flash controller (see
[../jieli/re-application-image.md](../jieli/re-application-image.md)). The
updater finds it from the header sector it has just erased: the one key whose
keystream the erased block shows through the code window.

`padtool update FILE` sends a split build's `update.bin` (the header and app
in the clear, from `jlnewfw pack --main-out`). It sends `UPDATER` first if the
app is running, and reboots into the new app after the commit. The updater's
IDENTIFY has byte 7 set to 1.

The updater also answers IDENTIFY, STATUS, LOG, REBOOT and the ROM loader's
opcodes, so `jlnewfw flash` and `padtool loader` reach the ROM loader from it.
