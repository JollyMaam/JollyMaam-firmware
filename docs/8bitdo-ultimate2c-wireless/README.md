# 8BitDo Ultimate 2C Wireless

Board facts for the 8BitDo Ultimate 2C Wireless pad, a Telink TLSR8208
(B80): its pins, flash, boot loader and what its stock firmware shows.
Stock firmware uses PB6 for battery sensing. Charging control is not established.

[Stock USB boot entry](re-bootloader.md) describes the pad and receiver command,
their model query, shared USB PID and the receiver's unestablished recovery path.

## Chip and flash

- Chip ID `0x5591` (TLSR8208, B80 family) over SWS. 16 KiB SRAM at
  `0x840000..0x844000`.
- Flash JEDEC ID `85 60 13` (PUYA, 512 KiB). Its UID reads with command
  `0x4b`.
- Stock USB ID `2dc8:310a`. The stock app is built on Telink's
  `telink_b80_ble_2.4g_dual_mode_sdk_V3.0.0`.

| Range (flash offset) | Contents |
|---|---|
| `0x0000..0x6000` | 8BitDo boot loader, `KNLT` header at 0 |
| `0x6000..` | the app: `KNLT` header at `0x6008`, started by the loader |
| `0x73000..0x80000` | stock data in sectors `0x73000`-`0x77000`, `0x79000`, `0x7c000`, `0x7f000`; roles not established |

- The stock status register protects the low 256 KiB (SR1 `0x2c`); the boot
  loader's own writes work regardless.
- The OTP (16 KiB, `0x0000..0x4000`, read with the SDK's `otp_read`) is
  blank (`0xffffffff`) but for seven words near its end; their meaning is
  not established:

| OTP address | Word |
|---|---|
| `0x3fb4` | `10000000` |
| `0x3fc4` | `feffaf9f` |
| `0x3fd0`-`0x3fdc` | `33b62ab6 37712e15 5a5c4c82 573433d6` |
| `0x3fe0` | `00553031` |

- The BLE SDK's `cpu_wakeup_init()` (in `liblt_8208.a`, through
  `cpu_wakeup_no_deepretn_back_init()`) reads OTP word `0x3fb4` and resets
  the chip (`0x6f` <- `0x20`) unless it is `0xffffffff`. The stock boot
  loader and firmware run with it as it is. The B80 platform SDK's driver
  library has no such read.

## App image

The boot loader copies the vectors and RAM code (length at header `0x0c`,
in 16-byte units in the low half) to SRAM at `0x840000` and starts at 0;
the rest of the app runs from flash at its physical address. Header:

| Offset | Field |
|---|---|
| `0x00` | `tj` to the reset code |
| `0x02..0x08` | stock: `6a 00 00 00 5d 02` (meaning not established) |
| `0x08` | `KNLT` |
| `0x0c` | `0x0088xxxx`: the RAM copy |
| `0x18` | the image's size, CRC included |
| end - 4 | CRC-32 of everything before it, without the final inversion, after padding to 16 bytes with `0xff` |

### Starting the app

The boot loader's launch (its code at `0x3000`, called from `0x302c`) reads
the low half of header word `0x0c`, multiplied by 16, as a byte count (at
most `0x3200`), clears register `0x800643`, copies that many bytes as words
from the image's start (flash at its address) to SRAM at `0x840000`, then
writes `0x88` to register `0x800602`, which restarts the CPU from SRAM. Code
doing the same from flash with interrupts off starts an image at another
offset.

Analog register `0x3c` set to `0x55` before a reset reads back `0x55` in the
image the boot loader then starts.

## Boot loader

USB `2dc8:3208`, product "8BitDo Boot", one HID interface with 64-byte
reports, no report IDs. A request: `05`, then little-endian u16 command,
u16 0, u16 length, u16 CRC-16/MODBUS of the data, u32 total, u32 address,
then up to 46 data bytes. A reply echoes the header with a status (0: done)
in the second field and carries up to 46 bytes at offset 18.

| Command | Does |
|---|---|
| 3 | program `length` bytes (total 4096) |
| 4 | erase the 4 KiB sector at the address (total 4096); sectors below `0x6000` are skipped |
| 5 | read `length` bytes (total the same) |
| 7 | start the app (no reply) |
| 8 | identify: `1b 30` |

Entry:

- Analog register `0x3c` = `0x5a`, then a reset (`0x6f` <- `0x20`): the loader
  stays in USB mode. The register keeps its value across the reset.
- By hand: with USB unplugged, hold L1 and R1 and tap Home; plug in USB.
- The loader's start (`0x3208`-`0x3266` in its code) drives row PA0 low and
  stays in the loader when PA7 and PC3 read low together: L1 and R1.
- In the loader, PF1 (the light under Home) blinks: the loader toggles
  `0x80052b` bit 1 (`reg_gpio_pf_out`) in its service loop.

## Pins

Button matrix: a row driven low, the others high; columns pulled up, active
low.

| Row \ column | PA5 | PC1 | PA6 | PC3 | PA7 |
|---|---|---|---|---|---|
| PD6 | Up | Y | Menu | R4 | L4 |
| PA0 | Left | X | Turbo | R1 | L1 |
| PC0 | Down | B | View | R3 | L3 |
| PA3 | Right | A | Capture | Sync | mode switch (low: Bluetooth, high: 2.4G) |

Home: PA4, active low, on its own.

Outputs (each found by driving the pin as a GPIO output):

| Pin | Drives | Stock |
|---|---|---|
| PD3 | left motor (the deep one) | non-GPIO function |
| PD2 | right motor (the high one) | non-GPIO function |
| PB7 | the Home ring | output, high |
| PF1 | the light under Home | output, low |
| PD5 | the red light beside the USB port | output, low |

PD4 drives nothing visible; its role is not established. Which level lights
the lights is not established.

## Power

- PD7 is the battery power hold, active high. Home supplies power while
  held; driving PD7 high keeps the pad running after Home is released and
  when USB is unplugged. Stock startup sets PD7 high.
- PD0 indicates USB power, high while plugged in and low while unplugged.
  This is separate from USB enumeration and configuration. Stock refuses
  to release the power hold while PD0 is high.
- The board's current consumption with the power hold released, and its
  charging behavior, are not established.

Analog (ADC channel = PB pin number + 1; 14-bit differential against ground,
1.2 V reference, input prescaler 1 as stock, analog `0xfa` = `0x3d`):

| Control | Pin | Rest | Travel (codes) |
|---|---|---|---|
| Left stick X | PB0 | ~4050 | left 1340, right 6380 |
| Left stick Y | PB1 | ~4360 | up 1730, down 6582 |
| Right stick X | PB4 | ~3960 | left 1258, right 6594 |
| Right stick Y | PB5 | ~4000 | up 6401, down 1416 |
| L2 | PB2 | ~3656 | pressed 2102 |
| R2 | PB3 | ~3719 | pressed 2136 |

PB6 is the stock firmware's battery-sense input (ADC channel 7). Its v1.06
routine at `0xb748` converts a raw code to millivolts as
`((code * 1175) >> 13) * 3700 / 1000`, with integer truncation at both steps.
A raw reading around 7909 converts to about 4195 mV. The scale's absolute
accuracy and the charging-complete indication are not established.

## Debug wire

SWS is on USB D+ (PA2), D- held high, while the app keeps
`dp_through_swire` on (`usb_set_pin(1)`).
