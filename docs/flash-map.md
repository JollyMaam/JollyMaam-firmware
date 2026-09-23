# Flash map

The 1 MiB SPI NOR as this build lays it out.  Everything below was read back
off the pad and decoded, not copied from a datasheet: the addresses come from
`tools/jlnewfw.c` (the packer) and the contents from a full flash dump.

There is no memory-mapped-in-place trick here.  The linker declares the `code0`
region at `0x1E00120` with a 1 MiB length, and links the application there (see
`docs/memory-map.md`); the SPI flash is a separate device that the loader and
the VM driver address by offset.

## The whole device

| offset | size | what |
|---|---|---|
| `0x000000` | `0x20` | flash header (32 bytes) |
| `0x000020` | `0x80` | four top-level JLFS entries |
| `0x0000A0` | `0x1FE8` | `uboot.boot`, the BANKCB second-stage bootloader |
| `0x002088` | `0x88` | `isd_config.ini`, the encoded system configuration |
| `0x003000` | `0x340D0` | application area, ends `0x370D0` (`app_area_head` + files + reserved entries) |
| `0x0370D0` | `0xF30` | padding to the 4 KiB image boundary |
| `0x038000` | `0x8000` | rest of PRCT; **not** transported by a reflash |
| `0x040000` | `0x2000` | **VM** - the syscfg store (8 KiB) |
| `0x042000` | `0xBC000` | unused / erased |
| `0x0FE000` | `0x1000` | **BTIF** - MAC + RF trim |
| `0x0FF000` | `0x1000` | **key_mac** |

`make flash` writes only `0x000000..0x038000` (the image).  Everything from
`0x040000` up survives a reflash, which is why bonds and the pad's identity do.

The packer's own summary line says the same thing:

    wrote build/firmware.img: 229376 bytes, app @0x3000, image end 0x38000,
    code limit 0x40000, declared flash 0xff000 (total 0x100000)

Note `declared flash 0xff000` is the value stored *in the header*, while the
chip is a full `0x100000`; `key_mac` occupies the last sector above it.  The
addresses follow `tools/jlnewfw.c`:

    vm   = code limit                      (0x40000 here)
    btif = flash_size - 0x1000 - btif_len   (0xfe000)
    keymac = flash_size - 0x1000            (0xff000)

with `VM_LEN = 8K` and `BTIF_LEN = 0x1000` from `[RESERVED_CONFIG]`.

## Header and top-level entries

The 32-byte header carries the chip-key carrier and identity fields; the chip
name is deliberately left readable (`AC635N` at `0x10`).  Each JLFS entry is
32 bytes: `crc16 @0`, `data_crc @2`, `offset @4`, `size @8`, `flags @12`,
`reserved @13`, `index @14`, `name[16] @16`.  The entries are ENC-scrambled
(XOR stream, key `0xffff`); decoding a dump gives exactly the packer's list:

| offset | name | offset field | size | flags | index |
|---|---|---|---|---|---|
| `0x020` | `uboot.boot` | `0xA0` | `0x1FE8` | `0x00` | `0` |
| `0x040` | `isd_config.ini` | `0x2088` | `0x88` | `0x02` | `0` |
| `0x060` | `app_dir_head` | `0x1E00120` | `0xFFFFFFFF` | `0x81` | `0` |
| `0x080` | `key_mac` | `0xFF000` | `0x1000` | `0x12` | `0xFFFF` |

`index` is the terminator: `0xFFFF` ends the list.  `app_dir_head` points at
`0x1E00120`, which is the entry point recorded in `EXTRA_CFG_PARAM ENTRY` - the
same number the linker links the application at.

## Application area (0x3000)

One 32-byte `app_area_head` (SFC-scrambled in 32-byte blocks, key
`0xffff ^ (offset >> 2)`) followed by a nested JLFS list whose offsets are
relative to `0x3000`:

| nested offset | name | offset field | size | flags | trailing field |
|---|---|---|---|---|---|
| `+0x020` | `app.bin` | `0x120` | `0x33C1C` | `0x82` | `FF` |
| `+0x040` | `cfg_tool.bin` | `0x33D3C` | `0x394` | `0x82` | `00FFFFFF` |
| `+0x060` | `VM` | `0x40000` | `0xBE000` | `0x12` | `00200000` = 8 KiB |
| `+0x080` | `PRCT` | `0` | `0x40000` | `0x92` | `"CODE"` |
| `+0x0A0` | `BTIF` | `0xFE000` | `0x1000` | `0x92` | `"AUTO"` |

The application binary itself therefore starts at `0x3000 + 0x120 = 0x3120`
and runs to `0x36D3C`; `cfg_tool.bin` follows to `0x370D0`, and the image is
padded to `0x38000`.

Two details worth knowing:

* The `VM` entry's *size* field spans `0x40000..0xFE000`, i.e. VM plus all the
  unused flash up to BTIF, but the real VM length (8 KiB) is stored in the
  entry's trailing field.  Read the size field alone and you will conclude the
  syscfg store is 760 KiB.
* `PRCT` is the code region: it is declared as `0x0..0x40000` even though only
  the sectors holding application data are transported.  That is what
  `--code-limit` is for; without it, a smaller build would let old application
  sectors become VM storage.

## What writes to flash, and when

Full detail is in `docs/bond-store.md`.  A normal build writes four items: the
link keys and peer addresses in VM, the pad's own address in BTIF at `0xFE004`,
and the profile settings in VM.

With `BOND_STORE_RAM=1` all four are RAM-only and no application code calls
`syscfg_write` at all, so nothing above `0x038000` changes no matter how many
times you pair, switch profile, change the report period or generate a new
address.  That is the property `make itest` checks.

## Reproducing this map

* addresses: `make V=... ` build output, or `tools/jlnewfw.c` directly;
* contents: `tools/itest.sh` dumps the full 1 MiB to
  `/work/cyclone2-flash-backups/`, and a small decoder over that dump
  reproduces both tables (CRC16/CCITT, poly `0x1021`, init 0).

One caveat while reading dumps: `tools/flash.sh` still uses a 512 KiB window
(`flash_size=0x80000`), so its `--backup` misses `0x80000..0x100000` and its
`--erase-persistent` (`0x40000..0x7FFFF`) does not reach the MAC at `0xFE004`.
`tools/itest.sh` covers the full megabyte.
