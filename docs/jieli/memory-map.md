# CPU memory map

Addresses are CPU addresses unless marked as flash offsets. Ranges are half-open.
The layout comes from the SDK linker script and the MaskROM and loader layout.

## Address space

| Range | Size | Use |
|---|---|---|
| `0x00000000..0x0002bf00` | `0x2bf00` | ram0: `.data`, `.bss`, IRQ stack, heap |
| `0x0002bf00..0x0002c000` | `0x100` | interrupt vector and entry area |
| `0x0002c000..0x0002ff80` | `0x3f80` | ram1: MMU TLB, then second heap |
| `0x0002ff80..0x00030000` | `0x80` | update handoff slot |
| `0x00800000..0x00a00000` | 2 MiB | PSRAM window declared by the SDK; populated hardware not established |
| `0x01e00120..0x01f00120` | 1 MiB | application code window (flash, through the MMU) |

Internal RAM is 192 KiB in total.

The MMU TLB occupies `0x2c000..0x2d200`. The second heap runs from `0x2d200` to
`0x2ff80`.

The code window is a CPU address, not a flash offset. The application image
`app.bin` starts at flash offset `0x3120` ([flash-map.md](flash.md)).

`.data`, `.bss`, overlays, `NVRAM_DATA` and the main heap move with the build.
The linker map from the build lists current values.

## Retained SRAM

Sections linked `NOLOAD` are not cleared by startup code. Retained SRAM
survives software resets and loader-mediated reflashing while power stays on.
Loss of supply and soft power-off clear it. Ordinary `.bss` is reinitialized
at every application boot, even when the SRAM bits were retained.

## Low SRAM used by MaskROM

| Address | Size | Use |
|---|---|---|
| `0x10800..0x10810` | 16 bytes | MaskROM boot-state token, written with `nvram_set_boot_state()` and a cache flush |

These are SRAM addresses, not flash offsets. The Bluetooth controller's RAM must
not cover this token.

The bootloader's use of low SRAM and MaskROM's high workspace were found by
reverse engineering. Their bounds are: second-stage loader linked at `0x4000`
within a 20 KiB window, MaskROM low `.bss` ending near `0x11424`, and MaskROM
high workspace starting near `0x2bdc0`. These bounds may differ between ROM and
loader revisions.
