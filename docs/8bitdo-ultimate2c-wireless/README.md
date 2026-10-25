# 8BitDo Ultimate 2C Wireless

Board facts for the 8BitDo Ultimate 2C Wireless pad, a Telink TLSR8208
(B80): its pins, flash, boot loader and what its stock firmware shows.
LEDs, motors and the battery are not established.

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
- OTP word `0x3fb4` reads `0x10000000`. The B80 SDK's `cpu_wakeup_init()`
  wants `0xffffffff` there and resets the chip (`0x6f` <- `0x20`) otherwise;
  the stock boot loader starts flash firmware with it as it is.

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
- By hand: the loader's start reads two buttons (`0x3208`-`0x3250` in its
  code) and stays when they are held. Holding L1+R1 while the pad starts,
  after holding Home for some 8 s, entered it; the exact sequence is not
  established.

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

PB6 reads about 7909; its function is not established.

## Debug wire

SWS is on USB D+ (PA2), D- held high, while the app keeps
`dp_through_swire` on (`usb_set_pin(1)`).
