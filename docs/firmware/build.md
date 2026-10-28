# Build

How this firmware is built on the Jieli SDK. The SDK, compiler and libraries
are in [../jieli/sdk-toolchain.md](../jieli/sdk-toolchain.md).

## Components

| Item | Value |
|---|---|
| Jieli SDK | unmodified; the linker script is patched at build time (below) |
| Host tools | `gcc -std=c11`, for host unit tests and the image tool only |
| BTstack | upstream master, unmodified, built from source. Some symbols are renamed at build time to avoid clashing with the SDK's controller. |
| Controller | the project's own C controller policy. No SDK controller archive is linked. |

The SDK tree is outside this repository. Its path is fixed by the build.

The project's controller C also builds with
`-Werror=implicit-function-declaration`.

## Board configuration

SDK board configuration values used by the build:

- Chip name and PID: `AC635N`
- Reset: `PB01`, active low (`CONFIG_RESET_LEVEL 0`), `CONFIG_RESET_TIME 8`
- Oscillator and system clock: both 24,000,000 Hz
- Low power: `PWR_LDO15`, sleep enabled
- USB: none of the SDK's classes (`USB_DEVICE_CLASS_CONFIG 0`); the firmware sets up its own

`CONFIG_FLASH_SIZE` is set per board: 1 MiB for the pad, 512 KiB for the dongle.

## Linker script

The SDK linker script is patched at build time:

- The heap end moves from `RAM_END` to `RESET_MAILBOX_BEGIN` (`0x20000`).
- The Bluetooth controller's `.bss` is placed so that it does not overlap the
  MaskROM boot-state token at `0x10800`.
- A split build's app: `code0` starts `MAIN_AT` higher. The updater: its entry
  goes ahead of `.startup.text` ([updater.md](updater.md)).

The application script places:

- `.text` in the flash code window (`code0`)
- `.data` and `.bss` in `ram0`
- `.reset_mailbox` at `0x20000` (16 bytes, `NOLOAD`)
- `.retained` at `0x20010` (`0x2100` bytes maximum, `NOLOAD`)

Asserts enforce the sizes and the non-overlap. See [runtime.md](runtime.md).

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

## BTstack on the SDK

- **The BTstack run loop sleeps on a semaphore.** A one-tick delay in it capped
  Classic reports at 100 per second.
- **The control loop must run above BTstack's priority.** BTstack takes about
  1 ms of CPU per packet.
- **The HCI receive queue drops packets when full.** BTstack can then join the
  fragments left of two ACL packets into one ([updater.md](updater.md)).
