# Firmware image and configuration formats

Recovered formats used by BR23 loaders and SDK configuration readers.
Implementations: `tools/jlnewfw.c` and `tools/jlcfg.c`.

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

The generated image ends after the 4-KiB-aligned application data. By default,
its declared flash span is the smallest power of two that fits code, VM, BTIF,
and key-mac, with a minimum of 128 KiB. `--flash-size` fixes that span so those
reserved regions do not move when code size crosses a power-of-two boundary.
`--code-limit` similarly fixes the end of PRCT and start of VM without padding
the transported image to that address. Erased/reserved bytes do not need to be
transported.

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


## UFW transport wrapper

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


## isd_config.bin

This is the binary file named `isd_config.ini` inside the firmware image. It
contains only the readable INI's `[SYS_CFG_PARAM]` section:

```text
32 bytes  encoded chip-key carrier
u16       CRC16 of the carrier
repeat until end of file:
    u8    value length
    char  upper-case, NUL-terminated key[]
    u8    value[value length]
```

Plain integers are encoded as little-endian `u32`; other values are upper-case
ASCII without a terminating NUL. Decoder output uses explicit `U32:`, `STR:`,
and (for otherwise unrepresentable data) `HEX:` prefixes because the binary
format itself has no type tag.

## cfg_tool.bin

The file uses the SDK's read-only VM-item format:

```text
u32 payload_size                 # complete file size minus four
repeat:
    u32 header
    u8  data[data_length]

header = (data_length << 20) | (item_id << 8) | (CRC16(data) & 0xff)
```

IDs and lengths are 12 bits. This BR23 SDK rejects an individual item larger
than 800 bytes. Records have no padding or alignment. A zero-length item with
ID zero terminates the stream.

The stock 916-byte BR23 file contains key mappings, audio/charge/low-power
settings, UI/tone status mappings, 20 Bluetooth names, a Bluetooth MAC and RF
power, 17 floating-point AEC parameters, microphone bias settings, TWS pairing,
auto-off, and LRC calibration. `cfg-decode` recognizes and comments these
layouts while preserving their exact bytes. In particular, Bluetooth name ID
101 is an SDK-special repeated group of `u8 enabled; char name[32]`, not merely
a single 32-byte name as an older SDK note claims.
