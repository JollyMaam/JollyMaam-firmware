# SoC: Jieli BR23 (AC635N)

## Identity

- Jieli BR23-family Bluetooth SoC. The packed image names the chip `AC635N`
  (`CONFIG_CHIP_NAME`). The package marking is not recorded.
- Bluetooth BR/EDR (Classic) and BLE, with the controller library in the SDK.
  This firmware runs its own controller policy on the SDK's radio ([radio.md](radio.md)).
- USB 2.0 full-speed device controller (USB0). The app exposes a mass-storage
  interface for vendor SCSI commands and a CDC serial interface.
- 192 KiB internal RAM (`0x00000`–`0x30000`).
- Serial NOR flash through the serial flash controller (SFC). 1 MiB on the pad,
  512 KiB on the dongle ([flash-map.md](flash-map.md)).
- The SDK declares a 2 MiB PSRAM window at `0x00800000`. Whether a PSRAM chip
  is populated on the pad is not established.
- 24 MHz oscillator (board configuration).

## CPU

- Core: PI32V2, a 32-bit core. The build uses clang target `pi32v2` with
  `-mcpu=r3`. The instruction set reference is in the toolchain, not in this
  repository.
- No hardware floating point. Float operations trap into the SDK's software
  emulator (`emu_fpu_*`). Use integer or fixed-point code in per-sample or
  per-millisecond paths.
- MMU enabled (`CONFIG_MMU_ENABLE`). The application's code window is mapped
  from flash ([memory-map.md](memory-map.md)).
- The application has a data cache. The LED driver flushes it before handing a
  buffer to DMA.

## Clocks

- System clock: from the PLL with the Bluetooth oscillator as input
  (`SYS_CLOCK_INPUT_PLL_BT_OSC`). Clock mode is adaptive
  (`CLOCK_MODE_ADAPTIVE`). The board configuration starts it at 24 MHz; the
  application raises it to 48 MHz at start-up, and `clk_get("sys")` reads
  48 MHz while running.
- Peripheral clock: the "lsb" clock, 24 MHz in this configuration. Peripheral
  dividers are computed from it (the I2C baud formula in
  [peripherals.md](peripherals.md) uses it).
- Runtime clock values can differ from the configured ones. The SDK sets the
  clock at boot.

## Power domains

- **P33**: always-on power and PMU block. Holds the charger registers
  (`P3_CHG_CON0`) and the reset-source register (`P33 0x12`). Base `0x1e3e00`.
- **RTC domain**: `R3_*` registers, the trigger stop-switch pins (`PR0`, `PR1`)
  and `R3_RST_SRC`.
- Low-power configuration (SDK, `board_config.h`): sleep enabled, LDO15 power
  selected, Bluetooth oscillator kept on, I/O rail levels VDDIOM 3.0 V and
  VDDIOW 2.8 V.

## Boot chain

1. **MaskROM** (on chip, not updatable). Reads a boot-state token from SRAM
   `0x10800` ([memory-map.md](memory-map.md)). Starts the second-stage loader.
2. **Second-stage loader** `uboot.boot`, stored in the flash BANKCB area
   ([re-application-image.md](re-application-image.md)). Linked to run at SRAM
   `0x4000`.
3. **Application**, stored in the JLFS application area (flash `0x3000`; the
   code image at `0x3120`). Entered at code-window address `0x01e00120`. The
   `.text` section runs from flash through the MMU. `.data` is copied into RAM
   by startup code.

The ROM USB loader identifies itself as `UBOOT1.00`. Entering it from the app is
described in [power.md](power.md). Image layout is in
[re-application-image.md](re-application-image.md).

## Peripheral register blocks

Register block base = `0x1e0000` (low-speed bus) or `0x1f0000` (high-speed
bus), plus `0x100 × group + 4 × index`, where group and index are the values
in the SDK's register definitions (`map_adr(group, index)`).

| Block | Base | Notes |
|---|---|---|
| CLOCK | `0x1e0000` | clock control |
| RST | `0x1e00c0` | reset control |
| MODE | `0x1e0100` | |
| SYSTEM | `0x1e0200` | |
| TIMER0 / 1 / 2 / 3 | `0x1e0400` / `0x500` / `0x600` / `0x700` | general timers; the period register counts to 65535 at most (a larger value is cut to its low 16 bits). On the osc input (CON bits 3:2 = 2), timer 3 has counted about 1 MHz, unsteadily, on some boots, with its registers and CLK_CON0-2 as on a healthy boot, while timer 2 on the same input kept 24 MHz; cause not established. On the lsb input (bits 3:2 = 0) it has not |
| PCNT | `0x1e1000` | pulse counter |
| SD0 | `0x1e1400` | SD controller |
| USB | `0x1e1800` | USB0 controller (`USB_CON0` at `0x1e1800`) |
| ANA | `0x1e1900` | analog block |
| SPI0 / SPI1 / SPI2 | `0x1e1c00` / `0x1d00` / `0x1e00` | SPI1 drives the LED chain |
| UART0 / UART1 / UART2 | `0x1e2000` / `0x2100` / `0x2200` | UART2 receives the stick |
| IIC | `0x1e2400` | I2C, route group C (PB4/PB6) |
| PAP | `0x1e2800` | |
| SS | `0x1e2b00` | |
| RDEC0 | `0x1e2c00` | rotary decoder |
| PLNK | `0x1e2d00` | |
| ALNK0 | `0x1e2e00` | |
| AUDIO | `0x1e2f00` | DAC and ADC ([audio.md](audio.md)) |
| MCPWM | `0x1e3000` | motor PWM |
| ADC | `0x1e3100` | SAR ADC: triggers, VBAT, jack detect |
| IR | `0x1e3200` | |
| CRC | `0x1e3500` | |
| LRCT | `0x1e3600` | |
| EFUSE | `0x1e3700` | chip key and other one-time values |
| RAND | `0x1e3b00` | |
| CTM | `0x1e3c00` | capacitive touch |
| P33 | `0x1e3e00` | power and reset ([power.md](power.md)) |
| DMA | `0x1e3f00` | UART2 receive ring |
| PERIENC | `0x1e4100` | |
| SBC | `0x1e4200` | SBC codec hardware |
| AES | `0x1e4300` | |
| PORTA / PORTB / PORTC / PORTD | `0x1e5000` / `0x1e5040` / `0x1e5080` / `0x1e50c0` | GPIO |
| USB_IO | `0x1e5100` | USB pin control; bit 11 selects GPIO mode |
| WAKEUP | `0x1e5108` | wake sources |
| IOMAP | `0x1e5118` | pin function map (`CON1`) |
| PLED | `0x1e5200` | |
| LCD | `0x1e5300` | |

High-speed bus, from the same definitions: SFC `0x1f0200`, SFCENC `0x1f0300`,
PSRAM `0x1f0500`, and CORDIC, DCP, EQ, SRC, FM, WL.
