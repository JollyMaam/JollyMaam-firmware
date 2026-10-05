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

## Where GameSir's flash tool puts an image

The distributed images (`fw_3.52.bin`, `donglefw_1.21.bin`) are built for flash
address 0, with PRCT at 0 and VM right after the code. Written raw at 0 they
verify, and the pad boots one (its stock backup here is exactly `fw_3.52.bin`
at 0). GameSir's Windows tool instead places the image at `EOFFSET` × 4 KiB
from `isd_config` (pad 1: `0x1000`; dongle 16: `0x10000`), as read back from
two dongles and a pad it upgraded:

| | pad (3.52) | dongle (1.21) |
|---|---|---|
| image | `0x1000` | `0x10000` |
| `0x0000` | erased but for the stamp (below) | copy of the image's first 4 KiB |
| `0x1000` | the image | copy of the image's first 4 KiB |
| `0x2000..0x9000` | the image | copy of `image[0x1000:0x8000]` |
| PRCT entry | `0x1000`, length unchanged | `0x10000`, length unchanged |
| VM entry | end of PRCT to BTIF (`0xf7000`, 28 KiB) | end of PRCT to BTIF (`0x61000`, 116 KiB) |
| BTIF, key_mac | kept | kept |

The PRCT and VM entries sit in the encrypted app area: the tool decrypts it
with the chip key, rewrites them, fixes the entry and area CRCs and
re-encrypts. The app area is encrypted per chip (`jl_sfc`, keyed by the chip
key and the offset from the app area's start, not by flash address), so the
same image is different bytes on chips with different keys. Seen: pads
`0xffff`; dongles `0x4317` and `0xffff`.

The tool also changes the top-level `app_dir_head` (and `app_dir_head2`)
entry's unused CRC field from `0xffff`, clearing bits: `0x0fff` on both
dongles, `0x00ff` on the pad. And it stamps the last 32 bytes of PRCT (and on
the pad also of the first 4 KiB) with the PID, 8 bytes the same on every
device (`66 75 01 04 2f ff 00 10`) and 6 that changed when the same dongle was
upgraded again; the firmware does not contain the PID string. Neither is
understood.

A dongle `donglefw_1.21.bin` written raw at 0 did not boot (the ROM loader
came up instead), where the pad's raw image does; why is not established.
`padtool flash` (and `padtool place`) reproduces the tool's dongle layout
(identical to the tool's output but for VM and the stamp).

The chip key is per chip (eFuses; `0xffff` unprogrammed) and readable through
the loader. The tool re-encrypts the app area (and the resources after it)
for the target's key and rewrites the chip-key record in `isd_config.ini`,
changing only the bytes whose bit must change, to values that look
arbitrary, then fixes the record's CRC and isd_config's in the top-level
table. `padtool flash` does the same for any image whose `EOFFSET` it
handles (16: GameSir dongle, 0: ours), so one image fits every chip; a full
dump goes back only to a chip with its key. Stock 1.21 laid out and
re-encrypted this way for a `0xffff` dongle booted. `jlnewfw` follows `EOFFSET` to the image a
device boots.

Evidence: `/shared/gamesir-firmware/dongleflash_*`,
`padflash_3.52_second_pad_official_full_1m.bin`, the stock pad backup.
