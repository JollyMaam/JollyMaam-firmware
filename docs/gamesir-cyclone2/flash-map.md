# Flash map: pad and dongle

Offsets are flash offsets on the SPI NOR device, not CPU addresses.

## Devices

| Board | Flash | Declared flash length | Code limit |
|---|---|---|---|
| Pad | 1 MiB (`0x100000`) SPI NOR | `0xff000` + 4 KiB key_mac sector | `0xfa000` |
| Dongle | 512 KiB (`0x80000`) SPI NOR | per its image | `0x7a000` |

The chip's manufacturer and part number are not established.

## Pad regions

| Offset / range | Use |
|---|---|
| `0x000000..0x003000` | boot image: flash header, loader and configuration ([../jieli/re-application-image.md](../jieli/re-application-image.md)) |
| `0x003000..0x0fa000` | application image, then unused space in the code region |
| `0x0fa000..0x0fe000` | VM (settings store), 16 KiB |
| `0x0fe000..0x0ff000` | BTIF: MAC address and RF trim |
| `0x0ff000..0x100000` | key_mac sector |

The code limit is the start of VM, and VM ends at BTIF. The dongle has the
same layout, with VM at `0x7a000..0x7e000`. The region rules and the VM format
are the SDK's ([../jieli/flash.md](../jieli/flash.md)).

## Backups

A full-device backup must cover `0x100000` on the pad, not 512 KiB.

## Image PIDs

The flashing tool writes only to a device whose flash holds the image's PID.
The PIDs are `GS_C2_ADC_DEVICE` for the pad and `GS_C2_Dongle` for the dongle.

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

### Reproducing the stock layout

Unpack followed by pack (the image tool, [../firmware/image-tool.md](../firmware/image-tool.md)),
with `--at 0`, reproduces the stock dongle 1.21 image byte for byte. Pack does
not build the pad image's second application directory or its check entry, so
a packed pad image is not GameSir's layout. That has not been tested on a pad.

`pack` with `--mark 0x0fff --copy 0:0:0x1000 --copy 0:0x1000:0x1000 --copy
0x1000:0x2000:0x7000` reproduces the dongle's installed flash except for the VM
contents and the stamp. `flash --keep 0x60fe0:32` carries the stamp over.

The stock images analysed, with their hashes, and the code analysis, are in
[re-stock-firmware.md](re-stock-firmware.md).
