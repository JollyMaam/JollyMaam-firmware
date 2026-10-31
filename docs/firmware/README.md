# This firmware

Facts about this firmware's own software: how it is built, the tools, and the
choices it makes on the chip. Hardware is in [../jieli/](../jieli/README.md)
and the board folders.

| Page | Covers |
|---|---|
| [build.md](build.md) | components, board configuration, linker script patches, build outputs, BTstack on the SDK |
| [image-tool.md](image-tool.md) | `jollymaam jieli`: list, unpack, pack, flash, loader, scan; how it tells devices apart; the tool device's identity |
| [runtime.md](runtime.md) | retained SRAM sections, VM items, charger, sleep policy, boot counter |
| [updater.md](updater.md) | the pad build: the updater in the app area, the app above it, rewriting the app in place |
| [console-wake.md](console-wake.md) | the Switch 1 wake transmission and the Switch 2 wake flag |
| [telink-ble.md](telink-ble.md) | flash ATT databases, packet buffers, per-slot LE sessions and pairing on the Ultimate 2C |
