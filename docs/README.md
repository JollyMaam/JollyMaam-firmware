# Documentation

| Folder | Covers |
|---|---|
| [jieli/](jieli/README.md) | the Jieli BR23 SoC and SDK: chip, memory, flash and VM format, image format, radio, reset and loader. True of any BR23 board |
| [gamesir-cyclone2/](gamesir-cyclone2/README.md) | the GameSir Cyclone 2 pad and the GameSir dongle: pins, peripherals, audio wiring, flash map, the stock firmware and its protocol |
| [firmware/](firmware/README.md) | this firmware: build, image tool, what it keeps in RAM and flash, power policy, console wake |

A fact about the chip or SDK goes in `jieli/`; a fact about one board goes in
that board's folder; a choice this firmware makes goes in `firmware/`. Board
and firmware pages link to `jieli/`; `jieli/` does not depend on a board.

## Conventions

- Addresses are CPU addresses unless the text says "flash offset". Ranges are
  half-open: `0x0..0x2c000` ends before `0x2c000`.
- `PAn`, `PBn`, `PCn`, `PDn` are GPIO ports. `PR0`, `PR1` are RTC-domain pins.
- Build-specific addresses (`.data`, `.bss`, heap, overlays) change with the
  build. The linker map from the build lists the current values.
- Where a value comes from stock firmware or a binary rather than the SDK, the
  text says so.
- "Not established" marks a point that no source confirms.
