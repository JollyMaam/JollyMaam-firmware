# BR23 USB RAM loader

`br23loader.bin` is not tracked here. Development tools read it directly from
Jieli's AC63 Bluetooth SDK (`JIELI_SDK/cpu/br23/tools/br23loader.bin`).
A distribution can place an unchanged copy in this directory; the tools prefer
that packaged copy when present. The license and provenance below are retained
for distribution packaging.

- Publisher: Jieli Technology
- Repository: https://github.com/Jieli-Tech/fw-AC63_BT_SDK
- SDK checkout commit: `7e1f8326b8235d31efa404c67681dec61404c89b`
- Upstream path: `cpu/br23/tools/br23loader.bin`
- Last file change in that history: `930c39e` (`update v0.8.0`)
- SHA-256: `2bf56d6d4692424aa4b3be197024dccd7356e9877ba13c8428f2dcdcc640be04`
- License: the SDK's root Apache License 2.0, reproduced in `LICENSE`.

No separate loader license or exclusion was found in this SDK checkout. The
repository license is taken to cover this unchanged binary; there is no
loader-specific license statement. No loader source is included here.

The host uploads this file to RAM at `0x12000` through BR23's UBOOT1.00 ROM
interface, then starts it to access SPI flash. It is separate from the
`uboot.boot` second-stage bootloader contained in a firmware image.
