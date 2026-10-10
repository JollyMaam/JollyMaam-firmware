# Flash map

Offsets are flash offsets on the SPI NOR device, not CPU addresses.

## Devices

| Board | Flash | Declared flash length | Code limit |
|---|---|---|---|
| Pad | 1 MiB (`0x100000`) SPI NOR | `0xff000` + 4 KiB key_mac sector | `0x50000` |
| Dongle | 512 KiB (`0x80000`) SPI NOR | per its image | `0x50000` |

The chip's manufacturer and part number are not established.

## Pad regions

| Offset / range | Use |
|---|---|
| `0x000000..0x003000` | boot image: flash header, loader and configuration ([re-application-image.md](re-application-image.md)) |
| `0x003000..0x050000` | application image, then unused space in the protected code region |
| `0x050000..0x052000` | VM (settings store), 8 KiB |
| `0x052000..0x0fe000` | unused, erased |
| `0x0fe000..0x0ff000` | BTIF: MAC address and RF trim |
| `0x0ff000..0x100000` | key_mac sector |

`CODE_LIMIT` (`0x50000`) fixes the start of VM. Changing it moves VM, so the
settings are read from a different place.

The BTIF and key_mac region are placed relative to the declared flash length,
not to the physical size. Declaring the pad as 512 KiB moves them to `0x7e000`
and `0x7f000`, where the factory identity and the controller do not look for
them. A smaller application does not reduce the flash device size.

## Storage regions

| Region | Contents | Normal image write |
|---|---|---|
| VM | settings, bonds, calibration | not overwritten |
| BTIF | MAC address, RF trim | not overwritten |
| key_mac | key_mac sector (contents not described here) | not overwritten |

Whether a region survives depends on the write and erase range the flashing
tool actually uses. A full-device backup must cover `0x100000` on the pad, not
512 KiB.

## Image placement

The image format, its JLFS entries, the scrambling and the stock placement of
the pad and dongle images are in [re-application-image.md](re-application-image.md).
The pad's stock image boots when written raw at offset 0. The dongle's stock
image does not.
