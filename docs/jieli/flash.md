# Flash map

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
| `0x000000..0x003000` | boot image: flash header, loader and configuration ([re-application-image.md](re-application-image.md)) |
| `0x003000..0x0fa000` | application image, then unused space in the code region |
| `0x0fa000..0x0fe000` | VM (settings store), 16 KiB |
| `0x0fe000..0x0ff000` | BTIF: MAC address and RF trim |
| `0x0ff000..0x100000` | key_mac sector |

The code limit is the start of VM, and VM ends at BTIF. The `Makefile`
derives it from the flash size and `VM_KB`. Changing either moves VM, so the
settings are read from a different place. The dongle has the same layout,
with VM at `0x7a000..0x7e000`.

The image declares `0..CODE_LIMIT` as its protected region (PRCT). At boot
the SDK (`read_flash_id`) sets the flash chip's block-protect bits to the
largest region in its table that is no larger than a size from the boot
information. The SDK's software write check (`sdfile_check_write_protect`)
allows every address.

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

## VM format

VM (volatile memory, as the SDK's `isd_config_rule.c` labels its settings)
is the SDK's writable item store, read and written by `syscfg_read` and
`syscfg_write`. This is from the SDK's `vm_*` and `syscfg_vm_init` code in the
pad build.

### Areas

The region is split into two equal areas, used one at a time. With `VM_LEN`
of 16 KiB, each area is 8 KiB.

- The SDK caps the region at 16 KiB.
- Below 16 KiB, in one flash mode, the region is rounded down to 512 bytes,
  capped at 8 KiB, and erased in 256-byte pages.
- Otherwise it is rounded down to 8 KiB and erased in 4 KiB sectors.

Each area starts with a 32-bit magic word:

| Magic | Meaning |
|---|---|
| `0x55aaaa55` | the active area |
| `0xddeebb77` | being filled by a move |
| `0xddeeaa55` | filled by a move; the old area is not yet erased |
| `0xffffffff` | erased |

The three non-erased values can be programmed over each other in that
order without an erase.

### Items

Items follow the magic back to back, with no padding:

```text
u8  crc            low byte of CRC16(data)
u8  id             bits 0-7 of the item ID
u8  id_hi_len_lo   bits 8-11 of the ID (low nibble), bits 0-3 of the length (high nibble)
u8  len_hi         bits 4-11 of the length
u8  data[length]
```

As a little-endian `u32`, the header is
`(length << 20) | (id << 8) | (CRC16(data) & 0xff)`, the same layout as the
read-only items in [re-application-image.md](re-application-image.md).

- `syscfg_write` accepts IDs up to 255 and lengths up to 4095.
- An item costs its length plus 4 bytes.

### Writing

- A write whose data matches the stored item's is skipped.
- Otherwise the new item is appended to the active area. The newest copy of
  an ID is the one that counts. Older copies stay until the next move.
- No call deletes an item.

### Move

A move happens when an item does not fit in the rest of the active area:

1. The spare area is marked `0xddeebb77`.
2. The newest copy of each ID is copied into it, in ID order.
3. The spare area is marked `0xddeeaa55`.
4. The old area is erased.
5. The spare area is marked `0x55aaaa55`, and becomes the active area.

If the newest copies do not fit in one area, both areas are erased, and the
write fails.

### Start-up

- The active area is chosen from the two magics, which also lets an
  interrupted move be resolved.
- A spare area that is not blank is erased.
- The items are read from the start of the active area, up to the first
  header whose CRC does not match. That builds a table of the newest copy of
  each ID. The write position is the end of the last good item.
- If the used part is over 80% of the area, a move runs.

### Items in this firmware

| ID | Length | Owner |
|---|---|---|
| 1-28 | up to 224 | settings records (`src/settings.c`) |
| 92 | 8 | DAC trim (`CFG_DAC_TRIM_INFO`) |
| 93 | 29 | Bluetooth RF trim (`CFG_BT_TRIM_INFO`) |
| 110 | 6 | Bluetooth oscillator offset (`CFG_BT_FRE_OFFSET`) |
