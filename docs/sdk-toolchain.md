# SDK and toolchain

## Components

| Item | Value |
|---|---|
| Jieli SDK | `fw-AC63_BT_SDK` (BR23 platform); the linker script is patched at build time. SDK version `0x240001`, dated 2024-12-31 |
| Compiler | clang for `pi32v2`, toolchain release 2025-03-24 (`/shared/jieli-linux-toolchains-20250324.1/pi32v2/bin`) |
| Linker | `lto-wrapper` (LTO); `objcopy`; `llvm-ar` from the toolchain's `common/bin` |
| Host tools | `gcc -std=c11`, for host unit tests and the image tool only |
| BTstack | upstream master, unmodified, built from source. Some symbols are renamed at build time to avoid clashing with the SDK's controller. |
| Controller | the project's own C controller policy. No SDK controller archive is linked. |

The SDK tree is outside this repository. Its path is fixed by the build.

## Compiler flags

The SDK's BR23 flags, as used by the build:

- `-target pi32v2 -mcpu=r3 -integrated-as -flto -Oz -g`
- `-Wframe-larger-than=256`, `-Wimplicit-function-declaration`,
  `-fms-extensions`. `-w` suppresses most other warnings.
- The project's controller C also builds with
  `-Werror=implicit-function-declaration`.

The linker prints stack-size warnings from `-warn-stack-size=256`. These are
vendor lint, not build failures.

## Libraries

Linked from the SDK's `cpu/br23/liba/`:

- `cpu.a`, `system.a`, `update.a`, `crypto_toolbox_Osize.a`

Runtime libraries from the toolchain: `libc.a`, `libm.a`, `libcompiler-rt.a`
(`lib/r3`).

SDK sources compiled into the application: debug, the USB device stack and mass
storage, `adc_api.c`, `charge.c`, `clock_manager.c`, `led_spi.c`, `mcpwm.c`,
`setup.c`.

SDK defines in the build include `CONFIG_CPU_BR23`, `CONFIG_FREE_RTOS_ENABLE`,
`CONFIG_MMU_ENABLE`, `CONFIG_SBC_CODEC_HW`, `CONFIG_MSBC_CODEC_HW`,
`CONFIG_AUDIO_ONCHIP`, `CONFIG_ITEM_FORMAT_VM`, `VM_ITEM_MAX_NUM=256`, and
`VM_MAX_SIZE_CONFIG=16*1024`. The VM region in flash is set by the flash map
([flash-map.md](flash-map.md)), not by the config value.

Board configuration values:

- Chip name and PID: `AC635N`
- Reset: `PB01`, active low (`CONFIG_RESET_LEVEL 0`), `CONFIG_RESET_TIME 8`
- Oscillator and system clock: both 24,000,000 Hz
- Low power: `PWR_LDO15`, sleep enabled
- USB: mass-storage class in the SDK default

`CONFIG_FLASH_SIZE` is set per board: 1 MiB for the pad, 512 KiB for the dongle.

## Linker script

The SDK linker script is patched at build time:

- The heap end moves from `RAM_END` to `RESET_MAILBOX_BEGIN` (`0x20000`).
- The Bluetooth controller's `.bss` is placed so that it does not overlap the
  MaskROM boot-state token at `0x10800`.

The application script places:

- `.text` in the flash code window (`code0`)
- `.data` and `.bss` in `ram0`
- `.reset_mailbox` at `0x20000` (16 bytes, `NOLOAD`)
- `.retained` at `0x20010` (`0x2100` bytes maximum, `NOLOAD`)

Asserts enforce the sizes and the non-overlap. See [memory-map.md](memory-map.md).

## Build outputs

| Step | Output |
|---|---|
| link | ELF image, linker map, linker script as built |
| `objcopy` of `.text` and `.data`, concatenated | application binary (`.text` followed by `.data`) |
| `jlnewfw pack` with `uboot.boot`, `cfg_tool.bin` and `isd_config.ini` | firmware image for flashing |

`isd_config.ini` is generated from the SDK's `isd_config_rule.c` and holds the
PID. The loader used for flashing is the SDK's `br23loader.bin`. Flashing tools
and their options are not described in these pages.

The flashing tool writes only to a device whose flash holds the image's PID, and
encrypts the image for that chip's key.

## Toolchain and SDK pitfalls

- **Floating point is emulated** ([soc.md](soc.md)). Per-sample float maths is
  too slow for a control loop. Use fixed point.
- **Do not use `__attribute__((packed))` on state that the main loop writes.**
  With LTO, unaligned stores to packed fields have corrupted neighbouring globals,
  including the millisecond tick. Lay structs out with natural alignment.
- **`uxTaskGetStackHighWaterMark()` resets the pad.** It links, but its OS
  thunk is not wired. A linkable SDK symbol does not mean its runtime path works.
- **Local names are limited to 31 bytes** by the SDK's local-name handling.
- **Call `bt_max_pwr_set()` at boot.** Without it the radio transmits well below
  stock power. See [radio.md](radio.md).
- **The BTstack run loop sleeps on a semaphore.** A one-tick delay in it capped
  Classic reports at 100 per second.
- **The control loop must run above BTstack's priority.** BTstack takes about
  1 ms of CPU per packet.
- **`REGISTER_LP_TARGET`** keeps the SDK out of low-power sleep. Without it USB
  did not enumerate.
