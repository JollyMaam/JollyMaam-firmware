# CPU memory map

Addresses are CPU addresses unless explicitly called flash offsets. The map
comes from the BR23 linker script and recovered bootloader/MaskROM use.
Ranges below are half-open (end excluded).

| Range | Size | Use |
|---|---|---|
| `0x00000000..0x0002bf00` | `0x2bf00` | ram0: data, BSS, IRQ stack, heap |
| `0x0002bf00..0x0002c000` | `0x100` | interrupt vector/entry area |
| `0x0002c000..0x0002ff80` | `0x3f80` | ram1: MMU TLB and second heap |
| `0x0002ff80..0x00030000` | `0x80` | update handoff slot |
| `0x00800000..0x00a00000` | 2 MiB | SDK-declared PSRAM window; populated hardware unconfirmed |
| `0x01e00120..0x01f00120` | 1 MiB | application code window |

Total internal RAM address space is 192 KiB. The code window address is not a
raw flash offset: the packed `app.bin` begins at flash offset `0x3120`.
The MMU TLB occupies `0x2c000..0x2d200`; the second heap runs from `0x2d200`
to `0x2ff80`. `.data`, `.bss`, IRQ stack, overlays, `NVRAM_DATA` and the main
heap start change with the build; read `build/app.map` for their current values.

## Preserved SRAM

| Address | Reservation | Meaning |
|---|---|---|
| `0x20000` | 16 bytes | legacy reset mailbox reservation |
| `0x20010` | `0x2100` bytes maximum | bring-up log and boot counter |

The main heap is capped at `0x20000`. The bring-up log currently contains 2000
32-bit entries plus metadata; its actual linked size is below the reservation.
These `NOLOAD` sections are not cleared by normal SDK startup. The mailbox
has no current producer; its layout is a legacy app ABI, not a hardware register.

The chosen gap avoids known bootloader use: second-stage UBOOT is linked at
`0x4000` within a 20 KiB window; recovered MaskROM low BSS ends near `0x11424`,
and high workspace begins near `0x2bdc0`. These bounds come from reverse
engineering, not a guarantee for all ROM/loader revisions.

## Low SRAM aliases: do not treat as permanent bond storage

`0x10800` is the MaskROM boot-state/token area (16 bytes), written through
`nvram_set_boot_state()` with a cache flush. Earlier dumps showed address/key
records around `0x10760`; a legacy debug operation clears `0x10700..0x10800`.
These are SRAM addresses, **not NOR flash offsets**. The current app does not
reserve `0x10700` as an independent persistent store. Its relation to the SDK's
current allocation is not established; inspect the current map before using
old absolute-address observations. App bonds persist in flash VM, not here.

Preserved SRAM can survive software resets and a loader-mediated reflash when
power remains present. Complete loss of supply loses SRAM. Normal app BSS is
reinitialized on reset even if the underlying SRAM electrically retained bits.

Sources: `src/sdkconfig/app.ld`, generated `build/sdk.ld`, `build/app.map`,
`src/main.c`, `src/usb.c`.
