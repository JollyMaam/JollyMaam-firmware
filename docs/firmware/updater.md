# Updater: the app rewritten in place

Every pad build (`gamesir-cyclone-2-pad`, `gamesir-nova-2-lite-pad`,
`8bitdo-ultimate2c-wireless-pad`) has two programs in its code region. The updater (`src/updater/updater.c`,
board `gamesir-cyclone-2-pad-updater`) is the image `uboot.boot` starts. The
app is linked `MAIN_AT` (`0x20000`) above it in the same code window. The
updater rewrites the app over the tool device and is never rewritten itself.
There is no second copy of the app.

## Flash layout (the pad)

| Flash offset | Contents |
|---|---|
| `0x000000..0x003000` | flash header, `uboot.boot`, `isd_config` |
| `0x003000..` | app area: its directory, the updater (`app.bin`), `cfg_tool.bin` |
| `0x023000` | the app's header (`0x120` bytes), then the app |
| `0x0fa000..` | VM, BTIF, key_mac, as before |

The app's header is written by `jlnewfw pack --main`:

| Offset | Field |
|---|---|
| `0x00` | `MAIN` (`0x4e49414d`), the commit word |
| `0x04` | the app's size |
| `0x08` | CRC16 of the app (non-reflected CCITT, initial value 0) |
| `0x0c` | the app's entry, `0x1e00000 + MAIN_AT + 0x120` |
| `0x10` | format version, 1 |
| `0x14` | `MAIN_AT`, the offset the app is linked for |
| `0x18` | the image's PID, 16 bytes, NUL-padded (`isd_config.ini`'s, as `jlnewfw flash` checks) |
| `0x28` | CRC16 of `0x04..0x28` |

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
   (format version 1 too)
   unchanged. No hardware has been touched.
3. Otherwise it continues into the SDK's start-up: the updater runs.

The updater is the SDK and this firmware's platform layer, the tool device,
the hang watch, the board's lights, and Bluetooth LE: our controller built
LE-only (`BT_LE_ONLY`: no Classic HCI command handlers either), BTstack
without Classic or SM, no P-256 thread, and the USB tool device without
the app's personalities (`USB_TOOL_ONLY`). The SDK linker script's forced
symbols are cut to `sdfile_vfs_ops`. It powers off after 60 s
without a host (USB or LE) and outside a write.

The app's `UPDATER` command (`0x40`) sets the request and reboots. In a build
without an updater the request is ignored and the app starts again.

By hand: in the app, Home+M while not connected
restarts into the updater on release. In the updater, Home pressed and
released goes back to the app when it is valid. While connected, Home+M is
still remap config mode.

## Update commands

On the tool device (command `0xf5`), answered by the updater:

| Sub-command | Does |
|---|---|
| `0x41` UPDATE_INFO | state, last error, whether the app is valid, its flash offset, room, bytes written, size begun, chip key |
| `0x42` UPDATE_BEGIN | argument: the image's size (header and app); data: the header's first `0x2c` bytes. Refuses, with nothing changed, a header that is not whole, for another PID, or for another format, offset or size; then clears block protection and erases the header's sector |
| `0x43` UPDATE_DATA | argument: offset (the bytes written so far); up to 512 bytes of the image in the clear; at any other offset it is ignored, and the host resends from UPDATE_INFO's count |
| `0x44` UPDATE_COMMIT | checks the header and the app's CRC through the code window, then programs the commit word |

The first data must carry the header BEGIN checked. The updater erases each
4 KiB sector as data reaches it, encrypts the data
for the chip, programs it, and reads it back through the code window. The
commit word is held back from the first block and programmed last, without
an erase. From BEGIN until COMMIT the app has no commit word, so a reset or
power loss comes back to the updater. An error stays until the next BEGIN.

The chip key does not read back from the flash controller (see
[../jieli/re-application-image.md](../jieli/re-application-image.md)). The
updater finds it from the header sector it has just erased: the one key whose
keystream the erased block shows through the code window.

`padtool update FILE` sends a pad build's `update.bin` (the header and app
in the clear, from `jlnewfw pack --main-out`). It sends `UPDATER` first if the
app is running, and reboots into the new app after the commit. The updater's
IDENTIFY has byte 7 set to 1.

The updater also answers IDENTIFY, STATUS, LOG, REBOOT and the ROM loader's
opcodes, so `jlnewfw flash` and `padtool loader` reach the ROM loader from it.

## Over Bluetooth LE

The updater advertises as "<board> Update" (connectable, the service UUID in
the advertising data, the name in the scan response). Its address is
derived from the flash UID, apart from the app's slot addresses. No pairing.

One GATT service, `c2550000-3e2d-66b1-8a4f-d5037c2b419e`:

| Characteristic | UUID | Does |
|---|---|---|
| request | `c2550001-…` | write: u8 sub-command, u32 argument, then a host-to-pad command's data |
| reply | `c2550002-…` | notify: u8 sub-command, then the answer (IDENTIFY, STATUS, LOG, UPDATE_INFO), at most the ATT MTU − 4 bytes |

The sub-commands are the tool device's. Each write is a write request: the
pad answers it once the command has run, so one write is in flight. With
the 512-byte ATT MTU an UPDATE_DATA write carries 504 bytes.

Writes without response do not work for UPDATE_DATA: while the updater
erases a sector, BTstack's thread does not take packets, the HCI receive
queue (`btstack_hci_jieli.c`, 16 entries) fills, and the queue drops what
does not fit. A write arrived with its first fragment and the next write's
second, which BTstack accepted as one packet. The updater's read-back then
matched what it had received; the commit's CRC caught it.

BTstack's ATT server asks SM before every request whether an address
lookup or re-encryption is still running. The updater links no SM: its
`sm_none.c` answers the few SM calls BTstack makes, and says no.

`padtool update FILE --ble [NAME|ADDRESS]` (needs `bleak`) sends the same
image to a pad in its updater.

## The Ultimate 2C Wireless (B80)

`8bitdo-ultimate2c-wireless-pad`: the updater
(`8bitdo-ultimate2c-wireless-pad-updater`, about 52 KB) is the image the 8BitDo
boot loader starts; the protocol is `updater.c`'s, the chip's side
`updater_telink.c`.

| Flash offset | Contents |
|---|---|
| `0x00000..0x06000` | the 8BitDo boot loader |
| `0x06000..0x16000` | the updater (a `KNLT` image) |
| `0x16000` | the app's header (`MAIN_AT` `0x10000`) |
| `0x16120..0x6f000` | the app (a `KNLT` image linked to run there) |
| `0x6f000..` | settings and bonds, as before |

`tools/8bitdoboot.py main` writes the header and the packed app
(`update.bin`); `NAME-flash` writes the updater and, with `--also`, the region.

Boot: `updater_boot()` runs first in `main()`. Unless analog register `0x3c`
holds `0x55` (the app's request, which it then clears), and if the region's
header has the commit word, the entry, format 1 and the app's `KNLT`, it
starts the app the way the boot loader starts its image (see
[../8bitdo-ultimate2c-wireless/README.md](../8bitdo-ultimate2c-wireless/README.md)):
the app's first bytes into SRAM, then the CPU restarts there.

The flash is not enciphered. The updater reads back through the SPI
(`flash_read_page`), not the cache. The flash's protection (`0x002c`, the
low 256 KiB, the region among it) is lifted at UPDATE_BEGIN and set back
after the commit.

LE: the service as a profile on `bt_telink.c`. Its writes are handled in the
stack's callback (`write_now` in `struct bt_device`), and the write response
goes after, so one write is in flight. An UPDATE_DATA write carries 152
bytes (a 160-byte ATT write, the most the LE side takes in fragments);
UPDATE_INFO reports it. Replies are notifications of at most 144 bytes and
need the reply characteristic's notifications on.

By hand: Home+Turbo (the board's M) while not connected.

## The Nova 2 Lite (BR23)

`gamesir-nova-2-lite-pad`: as the Cyclone 2, with the app's region at
flash `0x80000` (`MAIN_AT` `0x7d000`), so that stock's data at
`0x64000..0x80000` stays; `NAME_KEEP` makes `NAME-flash` write that range
back as the device held it.

## The dongles

`gamesir-cyclone-2-dongle` (BR23) and `gamesir-nova-2-lite-dongle` (BD19)
have an updater too (`-dongle-updater`): USB only (`NO_BT`), in the app area
with the dongle's app `MAIN_AT` `0x10000` above it. Its IDENTIFY has byte 6
set to 1, as the dongle's. The dongle's button, pressed and released, goes
back to a valid app; the updater does not power off. `padtool --dongle
update FILE` sends a dongle build's `update.bin`.

On the BD19 the start-up is the BR23's in shape (the boot information in
`r0`, `CODE_BEG` the code window's origin), and the reset mailbox is at
`0xf330`, below the retained log (`sdkconfig/bd19_app.ld`).
