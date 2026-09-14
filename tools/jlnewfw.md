# JieLi new-firmware images

`jlnewfw` documents the format by reading and writing it. It is independent of
the proprietary `isd_download` program. It builds raw BR23 NOR flash images
(`jl_isd.bin`) and can inspect or unpack either raw images or `.fw`/UFW updater
wrappers.

## Commands

```sh
make -C firmware
firmware/jlnewfw list firmware.img
firmware/jlnewfw verify firmware.img
firmware/jlnewfw unpack firmware.img output-directory
firmware/jlnewfw pack -c isd_config.ini -u uboot.boot \
    -o jl_isd.bin app.bin cfg_tool.bin
```

`pack` consumes the same readable `isd_config.ini` used by the SDK. It encodes
all `[SYS_CFG_PARAM]` values, derives the entry point and identity fields from
`[EXTRA_CFG_PARAM]`, and implements the standard PRCT/VM/BTIF/key-mac layout
from `[RESERVED_CONFIG]`. Numeric system values become little-endian 32-bit
integers; other values become upper-case, NUL-terminated strings.

## Flash layout

All multibyte integers are little endian. The raw image consists of:

1. A 32-byte flash header and a consecutive list of 32-byte JLFS entries.
2. A BANKCB `uboot.boot` and the encoded system configuration.
3. At the next 4 KiB boundary, an SFC-scrambled `app_area_head` containing a
   nested JLFS list and the application files.
4. Reserved regions described by headers but normally omitted from the file.

Every JLFS entry contains a CRC16 of its remaining 30 bytes and a CRC16 of its
data. Its fields are `data_crc, offset, size, flags, reserved, index, name[16]`.
A nonzero `index` terminates a list. Top-level offsets are relative to the flash
header. Nested application-file offsets are relative to `app_area_head`.
Reserved-region addresses are flash offsets.

The generated image ends after the 4-KiB-aligned application data. Its declared
flash span is the smallest power of two that fits code, VM, BTIF, and key-mac,
with a minimum of 128 KiB. Erased/reserved bytes do not need to be transported.

## Scrambling

This is reversible scrambling, not cryptographic protection:

- Flash and ordinary JLFS headers use the JieLi `ENC` XOR stream with key
  `0xffff`. VID and PID remain readable by construction.
- Each BANKCB header and bank payload in `uboot.boot` uses that stream
  independently with key `0xffff`.
- The complete application-area block uses 32-byte ENC blocks. A block at byte
  offset `n` uses `chip_key ^ (n >> 2)`.
- CRC16 is non-reflected CRC-CCITT, polynomial `0x1021`, initial value zero.

The encoded `isd_config.ini` begins with the 32-byte chip-key encoding and its
CRC16. Each following item is `data_size:u8`, `key:NUL`, and `data[data_size]`.
String data is upper-case and does not include a terminating NUL; the key does.

## Present scope

The raw flash image is the safety-critical artifact and is implemented here.
Vendor `.fw`/`.ufw` files add a 512-byte transport manifest plus burner-only
files such as `wp_flash.bin`, a readable INI copy, and a tail record. That layer
does not change flash contents and will be added separately after its metadata
semantics are verified. Compressed BANKCB bootloaders can be preserved, but
this tool does not itself LZ4-compress a bootloader.

The reader does understand the UFW container. Its first 64 bytes decode as:

```c
uint16_t header_crc, entry_list_crc;
uint32_t image_size;
uint16_t entry_count, unknown;
uint32_t data_alignment;
char chip_name[48];
```

Each 80-byte UFW entry contains its type, index, data CRC, file offset, actual
and padded sizes, 44 bytes of type-specific data, and a 16-byte name. The main
header and each entry are independently ENC-scrambled with key `0xffff`; the
entry-list CRC covers the entries as stored (scrambled). Type zero is the raw
`flash.bin` consumed by `jlnewfw`.

## Cross-check against isd_download

The implementation was compared with `isd_download` from the 2026-09-08 Linux
post-build tools using 44,196-, 65,536-, 458,752-, and 520,000-byte app inputs.
In every case it selected the same app base, code boundary, reserved-region
addresses, and flash size. Each complete raw image differed in only 38 bytes:
32 bytes of deliberately variable chip-key carrier data and six bytes of CRC
fields which cover that data. The flash header, BANKCB ciphertext, entire SFC
application area, and padding were byte-identical.
