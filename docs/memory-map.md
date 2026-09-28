# Memory map

The BR23 CPU address space as this firmware uses it, and the two low-RAM
addresses the pad pokes by hand.  Numbers come from the generated linker script
(`build/sdk.ld`), the map file (`build/app.map`), and our own
`src/sdkconfig/app.ld`; the last section explains how to re-derive them.

## Address space

| range | size | what |
|---|---|---|
| `0x00000000..0x0002BF00` | ~176 KiB | `ram0` - application RAM (`.data`, `.bss`, stack, heap) |
| `0x0002BF00..0x0002C000` | 0x100 | `ISR_BASE` - interrupt vectors/entry area |
| `0x0002C000..0x0002FF80` | ~16 KiB | `ram1` - MMU TLB + second heap |
| `0x0002FF80..0x00030000` | 0x80 | `UPDATA_BEG` - update handoff slot |
| `0x1E00120..0x1F00120` | 1 MiB | `code0` - the application code window (`.text`) |
| `0x00800000..0x00A00000` | 2 MiB | `psram` - declared by the SDK, nothing placed in it |

The application is linked at `0x1E00120` inside `code0`, which is also the
entry point in `EXTRA_CFG_PARAM` (see `docs/flash-map.md`).  The flash itself is
not mapped at offset 0; that address is RAM.

## RAM as the linker lays it out (this build)

| symbol / section | address | size | notes |
|---|---|---|---|
| `.data` | `0x0000` | `0x46FC` | initialised data, copied out of flash at startup |
| `.irq_stack` | `0x4B80` | `0x848` | |
| `.bss` | `0x53E0` | `0x88E0` | zeroed at startup; ends `0xDCC0` |
| `NVRAM_DATA` | `0xD85C` | `0x194` | SDK saved-state blob, inside `.bss` |
| overlay | `0xDCE4` | 4 | overlay area, effectively unused here |
| `RAM_USED` / `HEAP_BEGIN` | `0xDCE8` | | end of all static RAM |
| main heap | `0xDCE8` | `0x12318` | `HEAP_END` is forced to `0x20000` by our ld change |
| `.reset_mailbox` | `0x20000` | 16 | ours; see below |
| `.bringup_log` | `0x20010` | 3700 | ours; ends `0x20E84` |
| `RAM_END` | `0x2BF00` | | |
| `.mmu_tlb` | `0x2C000` | `0x1200` | in `ram1` |
| `RAM1_USED` / `HEAP1_BEGIN` | `0x2D200` | | |
| second heap | `0x2D200` | `0x2D80` | `HEAP1_END = 0x2FF80` |

Total usable internal RAM is `0x0..0x30000` (192 KiB); `_MALLOC_SIZE` is the two
heaps together, `0x15098`.

## The reserved SRAM block

`src/sdkconfig/app.ld` places two `NOLOAD` sections at fixed addresses and
shrinks the heap to keep clear of them:

    RESET_MAILBOX_BEGIN = 0x20000   (16 bytes)
    BRINGUP_LOG_BEGIN   = 0x20010   (3700 bytes)
    ASSERT(HEAP_END <= RESET_MAILBOX_BEGIN)

The comment there explains the choice: the secondary bootloader is linked at
`0x4000` in a 20 KiB window, MaskROM's low BSS ends near `0x11424`, and
MaskROM's high workspace starts at `0x2BDC0` - so `0x20000` sits in the gap, and
SDK startup does not clear it.  `.bringup_log` holds the boot counter and the
bring-up ring (`status` prints them); the mailbox held the profile-switch
handoff and is now written by nothing.

## Two addresses used by hand

Neither is allocated by the linker.  Both are read and written directly by the
firmware, and both are **RAM**, not flash:

| address | size | what |
|---|---|---|
| `0x00010700` | `0x100` | the Bluetooth controller's pairing records (`btctrler.a`); a record shows up at `0x10760` as peer address + link key |
| `0x00010800` | 16 | MaskROM boot-state word / USB boot token (`nvram_set_boot_state()`) |

`src/usb.c` flushes the data cache after touching `0x10800`, which is the usual
signature of a cached RAM location.  The clean way to tell these apart from
flash is to compare the two: the flash at offset `0x10800` is simply part of
`app.bin`, while `status` reports a totally different `nvram=` value read from
the RAM at address `0x10800`.

This is why `bondstore` and `nvscan` describe memory that no flash tool can
reach, and why `clearkeys` zeroes `0x10700` and restarts rather than erasing
anything.

## What survives what

| event | SRAM (`0x10700`, mailbox, bring-up log) | flash (VM, BTIF, image) |
|---|---|---|
| profile switch / `disconnect`+`connect` | kept | untouched |
| `p33_soft_reset()` (`clearkeys`) | kept | untouched |
| power cycle / unplug | lost | kept |
| `make flash` | kept | image rewritten, reserved regions kept |

The persistent store's RAM copies (link-key mirror, peer addresses, settings,
calibration; see `docs/bond-store.md`) live in `.bss`, so changes held in RAM
mode (`store ram`) behave like the first column: they survive a profile
switch, and are gone at the next boot.

## Open question worth keeping in mind

`0x10700` and `0x10800` sit inside the linker's `RAM_USED..HEAP_END` window
(`0xDCE8..0x20000`), so the controller's record area and the malloc arena share
an address range.  Nothing here establishes how that is kept safe - the records
are known to persist in practice, and the app apparently never allocates that
high, but it is not an explicitly reserved region the way `0x20000` is.  If
bond corruption ever shows up under memory pressure, this is the first thing to
look at.

## Re-deriving the numbers

* `build/sdk.ld` - generated memory regions and the `HEAP_*` symbols;
* `build/app.map` - resolved addresses for every section and symbol
  (`nm build/app.elf` works too, with the Pi32 toolchain's `nm`);
* `src/sdkconfig/app.ld` - our fixed-address reservations and the assertions
  that keep the heap out of them.
