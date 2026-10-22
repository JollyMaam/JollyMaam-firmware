# SDK and toolchain

## Components

| Item | Value |
|---|---|
| Jieli SDK | `fw-AC63_BT_SDK` (BR23 platform). SDK version `0x240001`, dated 2024-12-31 |
| Compiler | clang for `pi32v2`, from toolchain release jieli-linux-toolchains-20250324.1 |
| Linker | `lto-wrapper` (LTO); `objcopy`; `llvm-ar` from the toolchain's `common/bin` |

## Downloads

Vendor package-manager links:

| Item | Link |
|---|---|
| Compiler toolchain, Linux | curl -LJO https://pkgman.jieliapp.com/s/linux-toolchain |
| Compiler toolchain, Windows | curl -LJO https://pkgman.jieliapp.com/s/win-toolchain |
| Post-build tools, Linux | curl -LJO https://pkgman.jieliapp.com/s/linux-postbuild |
| Post-build tools, Windows | curl -LJO https://pkgman.jieliapp.com/s/downtool |

## Compiler flags

The SDK's BR23 flags, as used by the build:

- `-target pi32v2 -mcpu=r3 -integrated-as -flto -Oz -g`
- `-Wframe-larger-than=256`, `-Wimplicit-function-declaration`,
  `-fms-extensions`. `-w` suppresses most other warnings.

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
([flash.md](flash.md)), not by the config value.

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
- **`REGISTER_LP_TARGET`** keeps the SDK out of low-power sleep. Without it USB
  did not enumerate.
