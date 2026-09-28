# Physical flash map

The tested pad has **1 MiB SPI NOR** (`0x100000` bytes), established from full
flash readback. Offsets here are on that physical device, not CPU addresses.
The chip's exact manufacturer/part number is not established in these notes.

## Current layout

| Offset/range | Use |
|---|---|
| `0x000000..0x000020` | 32-byte flash header |
| `0x000020..0x0000a0` | four 32-byte top-level JLFS entries |
| `0x0000a0` | BANKCB `uboot.boot` (recorded size `0x1fe8`) |
| `0x002088` | encoded `isd_config.ini` (recorded size `0x88`) |
| `0x003000` | application-area header and nested entries |
| `0x003120` | `app.bin`; length varies by build, then `cfg_tool.bin` |
| image end through `0x050000` | unused remainder of protected code region |
| `0x050000..0x052000` | VM/syscfg, 8 KiB |
| `0x052000..0x0fe000` | unused/erased area |
| `0x0fe000..0x0ff000` | BTIF: MAC and RF trim |
| `0x0ff000..0x100000` | key_mac |

UBOOT/config sizes are from the recorded loader artifact and can change with
its version. `CODE_LIMIT=0x50000` fixes the VM boundary; the image itself ends
at the next 4 KiB boundary after application data. Moving the code limit moves
VM and changes where settings are found. Older dumps used `0x40000` or
`0x48000`; those are historical layouts, not different physical capacities.

The header's declared flash length is `0xff000`: the key_mac sector occupies
the final `0x1000` above it. BTIF is `flash_size - 0x1000 - btif_len`.
Packing this board as 512 KiB incorrectly moved BTIF/key_mac to `0x7e000` /
`0x7f000`, while factory identity and the controller expected `0xfe000` /
`0xff000`. A smaller app does not imply a smaller flash chip.

## JLFS entries and address interpretation

The top-level `app_dir_head` entry's offset field is `0x01e00120`, the CPU code
entry point, with size `0xffffffff` and flags `0x81`. Application file offsets
are relative to `app_area_head` at `0x3000`; reserved-region offsets are physical
flash offsets. `app.bin` normally has nested offset `0x120` and flags `0x82`.

The VM entry's size spans from the code limit up to BTIF; the actual VM length
(**8 KiB**) is stored separately in the trailing field. Reading the size field
alone overstates the VM capacity. PRCT covers `0..CODE_LIMIT` even when the
transported image is shorter. BTIF and PRCT entries use flags `0x92`; VM and
key_mac use `0x12`. See [Image formats](image-formats.md) for byte layouts.

Application settings use VM. A normal image rewrite need not overwrite VM,
BTIF or key_mac; survival depends on the actual write/erase range used by the
flashing tool. Full-device backups must cover `0x100000`, not just 512 KiB.

Evidence: decoded pad flash dumps, `tools/jlnewfw.c`, `Makefile`,
`src/sdkconfig/board_config.h`, generated `build/isd_config.ini`.
