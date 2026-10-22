# Application image: format, layout and tools

The BR23 flash image the loader and boot code read: its container format,
where the parts sit in flash, how they are scrambled, and the configuration
records inside it. Multi-byte integers are little endian
unless stated. Flash regions outside the image (VM, BTIF, key_mac) are in
[flash.md](flash.md).

## Image contents

1. A 32-byte flash header, then a list of 32-byte JLFS entries.
2. The BANKCB `uboot.boot` (second-stage loader) and the encoded system
   configuration (`isd_config`).
3. At the next 4 KiB boundary, the application area: `app_area_head`, a nested
   JLFS list, and the application files (`app.bin`, then `cfg_tool.bin`).
4. Reserved regions (VM, BTIF, key_mac). Their headers are described in the
   image, but their contents are normally not transported.

Layout with the SDK's tool (seen on a BR23 pad with a 4 KiB boot area):

| Offset | Use |
|---|---|
| `0x000000..0x000020` | 32-byte flash header |
| `0x000020..0x0000a0` | four 32-byte top-level JLFS entries |
| `0x0000a0` | BANKCB `uboot.boot` (size set by the SDK's tool; `0x1fe8` and `0xea4` bytes seen) |
| after `uboot.boot` | encoded `isd_config.ini` (`0x88` bytes seen) |
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

Entry values seen in BR23 images:

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

## Resource area

An image can have a resource area after its app area (the `tone` files). It
is built like the app area: a header with flags `0x83`, the contents from the
offset field `0x20` onward, a size that includes the header, a CRC16 over the
contents, and index 1 on the last. It is a small directory of its own. It is
encrypted with the app area, the cipher continuing from the app area's start.

An area with flags `0x82` in the same place holds one file. The AC632N (BD19)
image of the GameSir Nova 2 Lite dongle has `p11_code.bin` there: 4096 bytes,
code for the chip's P11 core.

### AC632N (BD19) images

The AC632N image read from that dongle differs from a BR23 image in these
fields: flash-header byte 12 is 11 (BR23: 2); the `uboot.boot` entry's flags
are `0x40` (BR23: `0x00`); the app area's reserved list is VM, BTIF, EXIF and
USERIF, with no PRCT. The AC695X image of the GameSir Nova 2 Lite pad also has
EXIF and USERIF after BTIF. Neither the meaning of byte 12 nor flag `0x40` is
established.

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

## Images and placement on devices

A device's own placement, its stock images and the image tool are in the
pages for that device and for this firmware.
