# JieLi configuration blobs

`jlcfg` handles the two unrelated configuration formats stored alongside a
BR23 application. It has no dependency on Jieli's graphical configuration
tool or its proprietary Lua `cfg` runtime.

```sh
out/jlcfg isd-encode out/isd_config.ini out/isd_config.bin
out/jlcfg isd-decode out/isd_config.bin

out/jlcfg cfg-decode path/to/cfg_tool.bin cfg_tool.conf
out/jlcfg cfg-encode cfg_tool.conf rebuilt.bin
```

The decoder output accepted by the corresponding encoder is lossless, including
the chip key printed in the decoded `[ISD_CONFIG]` section. `isd-encode -k`
may be used to override that key. Known `cfg_tool.bin` item IDs use names from
the SDK's `syscfg_id.h`; unknown IDs stay
numeric. Decoded comments provide friendly interpretations for common records,
while the authoritative value remains an explicit byte string.

`cfg-encode` accepts `HEX:`, `U8:`, `U16:`, and `U32:` value lists. Bluetooth
name groups may use the more readable form:

```ini
[CFG_TOOL]
CFG_BT_NAME = BT_NAMES:1:Cyclone 2 | 0:Unused fallback
CFG_BT_RF_POWER_ID = U8:10
```

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

The AC695X Lua project describes which typed values are concatenated into each
record, but it cannot run under ordinary Lua: calls such as `cfg:group`,
`cfg:i32`, `cfg:addOutputGroups`, and the GUI widgets are supplied by Jieli's
configuration-tool host. `jlcfg` therefore implements the actual stable binary
format and does not attempt to emulate that GUI.

The GUI's `cfg_tool.c` and `cfg_tool.h` are a separate text-output path made
from the same Lua value objects. They contain compile-time macros/configuration;
the firmware's `cfg_bin.c` reads `cfg_tool.bin` directly at runtime. Reproducing
the binary therefore does not require generating the C and header files.
