# Application image: format, layout and tools

The flash image the loader and boot code read: its container format, where the
parts sit in flash, how they are scrambled, the configuration records inside
it, and the stock devices' placement. Multi-byte integers are little endian
unless stated. Flash regions outside the image (VM, BTIF, key_mac) are in
[flash-map.md](flash-map.md).

## Image contents

1. A 32-byte flash header, then a list of 32-byte JLFS entries.
2. The BANKCB `uboot.boot` (second-stage loader) and the encoded system
   configuration (`isd_config`).
3. At the next 4 KiB boundary, the application area: `app_area_head`, a nested
   JLFS list, and the application files (`app.bin`, then `cfg_tool.bin`).
4. Reserved regions (VM, BTIF, key_mac). Their headers are described in the
   image, but their contents are normally not transported.

Pad layout (stock pad 3.52 and this build, from the same offsets):

| Offset | Use |
|---|---|
| `0x000000..0x000020` | 32-byte flash header |
| `0x000020..0x0000a0` | four 32-byte top-level JLFS entries |
| `0x0000a0` | BANKCB `uboot.boot` (`0x1fe8` bytes on the stock pad; the size is set by the SDK's tool) |
| `0x002088` | encoded `isd_config.ini` (`0x88` bytes on the stock pad) |
| `0x003000` | application area: header and nested JLFS entries |
| `0x003120` | `app.bin` (length varies by build), then `cfg_tool.bin` |

The image ends at the next 4 KiB boundary after the application data. The
declared flash span is set by `--flash-size` and the code limit, so reserved
regions do not move when the code grows.

## JLFS entries

Each entry is 32 bytes. Its fields are `data_crc`, `offset`, `size`, `flags`,
`reserved`, `index` and `name[16]`. A CRC16 covers the entry's other 30 bytes,
and `data_crc` covers the data. A nonzero `index` ends the list.

- Top-level `offset` values are relative to the flash header.
- Nested `offset` values are relative to `app_area_head` (`0x3000`).
- Reserved-region offsets are always physical flash offsets.

Specific entries on the pad:

- The top-level `app_dir_head` entry's offset is `0x01e00120`, the CPU code
  entry point. Its size is `0xffffffff`, flags `0x81`.
- `app.bin` is at nested offset `0x120`, flags `0x82`.
- PRCT covers `0..CODE_LIMIT`, even when the transported image is shorter.
- BTIF and PRCT use flags `0x92`. VM and key_mac use `0x12`.
- The VM entry's size field spans from the code limit to BTIF. The real VM
  length (`0x2000`) is in a separate trailing field. Use that for capacity.

## Scrambling and encryption

Scrambling is reversible. It is not protection.

- Flash and top-level JLFS headers use the JieLi `ENC` XOR stream with key
  `0xffff`. VID and PID stay readable.
- Each BANKCB header and payload in `uboot.boot` uses the same stream with key
  `0xffff`.
- The application area uses 32-byte ENC blocks. The block at byte offset `n`
  uses key `chip_key ^ (n >> 2)`.
- The chip key is per chip, stored in eFuse (`0xffff` when unprogrammed). It
  is readable through the loader. Each chip's application area is encrypted
  with its own key. Two chips with different keys hold different bytes for the
  same image.

CRC16 is non-reflected CRC-CCITT: polynomial `0x1021`, initial value zero.

## isd_config.bin

The binary form of the `[SYS_CFG_PARAM]` section of `isd_config.ini`.

```text
32 bytes  encoded chip-key carrier
u16       CRC16 of the carrier
repeat until end of file:
    u8    value length
    char  upper-case key[], NUL-terminated
    u8    value[value length]
```

Integers are stored as little-endian `u32`. Other values are upper-case ASCII
without a NUL terminator. The format has no type tags. Decoded output prefixes
values with `U32:`, `STR:` or `HEX:` for clarity.

## cfg_tool.bin and VM items

The read-only VM item format used by the SDK:

```text
u32 payload_size              complete file size minus four
repeat:
    u32 header
    u8  data[data_length]

header = (data_length << 20) | (item_id << 8) | (CRC16(data) & 0xff)
```

- `item_id` and `data_length` are 12 bits.
- The BR23 SDK rejects an item larger than 800 bytes.
- Records have no padding or alignment.
- A zero-length item with ID zero ends the stream.

The image carries `cfg_tool.bin` after `app.bin`. It is the SDK's tool file.
In the SDK, Bluetooth name item 101 is a repeated group of
`u8 enabled; char name[32]`, not one 32-byte name.

The stock BR23 configuration file is 916 bytes. It holds key mappings,
audio, charge and low-power settings, UI and tone status mappings, 20 Bluetooth
names, a Bluetooth MAC address and RF power, 17 floating-point AEC parameters,
microphone bias settings, TWS pairing, auto-off and LRC calibration. `cfg-decode`
recognizes these layouts and preserves their exact bytes.

## Resource area (dongle)

The dongle image has a resource area after its app area (the `tone` files). It
is built like the app area: a header with flags `0x83`, the contents from the
offset field `0x20` onward, a size that includes the header, a CRC16 over the
contents, and index 1 on the last. It is a small directory of its own. It is
encrypted with the app area, the cipher continuing from the app area's start.

## UFW container

The reader understands the UFW transport container. Its first 64 bytes decode as:

```c
uint16_t header_crc, entry_list_crc;
uint32_t image_size;
uint16_t entry_count, unknown;
uint32_t data_alignment;
char chip_name[48];
```

Each 80-byte UFW entry holds the entry type, index, data CRC, file offset,
actual and padded sizes, 44 bytes of type-specific data, and a 16-byte name. The
main header and each entry are ENC-scrambled with key `0xffff`. The entry-list
CRC covers the entries as stored (scrambled). Type zero is the raw `flash.bin`
that `jlnewfw` consumes.

## Stock placement

The distributed images are `fw_3.52.bin` (pad) and `donglefw_1.21.bin` (dongle).
Both are built for flash address 0. The pad boots from 0 when written raw. The
dongle does not. GameSir's Windows tool writes each image at `EOFFSET` × 4 KiB
from `isd_config` (pad: `0x1000`; dongle: `0x10000`). Observed on two dongles
and an upgraded pad:

| | Pad (3.52) | Dongle (1.21) |
|---|---|---|
| image at | `0x1000` | `0x10000` |
| `0x0000` | erased, except the stamp below | copy of the image's first 4 KiB |
| `0x1000` | the image | copy of the image's first 4 KiB |
| `0x2000..0x9000` | the image | copy of `image[0x1000:0x8000]` |
| PRCT entry | `0x1000`, length unchanged | `0x10000`, length unchanged |
| VM entry | end of PRCT to BTIF (`0xf7000`, 28 KiB) | end of PRCT to BTIF (`0x61000`, 116 KiB) |
| BTIF, key_mac | kept | kept |

The tool decrypts the app area with the chip key, rewrites the PRCT and VM
entries, fixes the entry and area CRCs, and re-encrypts. The app area is
encrypted per chip (`jl_sfc`, keyed by the chip key and the offset from the
app area's start, not by flash address).

Chip keys seen: pads `0xffff`; dongles `0x4317` and `0xffff`.

The stock dongle image does not boot from offset 0. The `0x10000` layout boots.
The dongle image's `uboot.boot` (`0xea4` bytes) differs from the pad's
(`0x1fe8`). That it fixes the image at `0x10000` is a guess. Tried on a dongle
with erased flash up to BTIF, with the image re-encrypted for its key, none of
these ran: as distributed (`EOFFSET` 16); `EOFFSET` 0; `EOFFSET` 0 with the
`app_dir_head` mark. The ROM loader came up on replug.

### Stamps and unexplained fields

- The pad image has a check entry in the 32 bytes before its app area. Its
  data-CRC field is the CRC16 of the plain app area. Its offset field is the
  CRC16 of the encrypted one. Both change when the area is moved or
  re-encrypted. The dongle image has no check entry.
- The tool clears bits in the top-level `app_dir_head` (and `app_dir_head2`)
  CRC field: `0x0fff` on both dongles, `0x00ff` on the pad.
- The tool stamps the last 32 bytes of PRCT with the PID, and on the pad also
  the first 4 KiB. The stamp is 8 bytes that are the same on every device
  (`66 75 01 04 2f ff 00 10`) and 6 bytes that changed when the same dongle was
  upgraded again. The firmware does not contain the PID string. The stamp is not
  understood.

## The image tool (`jlnewfw`)

The project's `jlnewfw` tool, built for each board. Commands:

| Command | Does |
|---|---|
| `list` | show the JLFS entries of an image |
| `unpack` | write an image's files and an `isd_config.ini` |
| `pack` | build an image from files and an `isd_config.ini` |
| `flash` | write an image through the ROM loader |
| `flashpack` | `pack`, then `flash`, using the connected chip's key |

Options used by the build:

- `--flash-size` and `--code-limit`: fix the layout so reserved regions do not
  move when code size changes.
- `--loader`: the SDK's `br23loader.bin`.
- `-c`: `isd_config.ini`. `-u`: `uboot.boot`.
- `--enter-loader --wait 30`: ask running firmware to enter the ROM loader,
  then wait for it.
- `-k`: re-encrypt for the target chip's key. `flashpack` does this by default.
- `--force-pid`: allow a write to a device whose PID differs (a blank device or a
  deliberate change).

`flash` writes only to a device whose flash holds the image's PID. The PIDs are
`GS_C2_ADC_DEVICE` for the pad and `GS_C2_Dongle` for the dongle.

Unpack followed by pack, with `--at 0`, reproduces the stock dongle 1.21 image
byte for byte, and reproduces this project's build byte for byte. Pack does not
build the pad image's second application directory or its check entry, so a
packed pad image is not GameSir's layout. That has not been tested on a pad.

`pack` with `--mark 0x0fff --copy 0:0:0x1000 --copy 0:0x1000:0x1000 --copy
0x1000:0x2000:0x7000` reproduces the dongle's installed flash except for the VM
contents and the stamp. `flash --keep 0x60fe0:32` carries the stamp over.

## Stock images

The pad and dongle images analysed, with their application sizes and hashes,
and the code analysis, are in [re-stock-firmware.md](re-stock-firmware.md).
