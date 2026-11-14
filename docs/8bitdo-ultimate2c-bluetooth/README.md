# 8BitDo Ultimate 2C Bluetooth

Board facts for the 8BitDo Ultimate 2C Bluetooth pad (U2CB), the
Switch-oriented 2C: a Yichip YC3121 (Cortex-M0) with a separate Bluetooth
radio core, and what its stock firmware v1.01 shows. Not to be confused
with the Ultimate 2C Wireless ([../8bitdo-ultimate2c-wireless/](../8bitdo-ultimate2c-wireless/)),
a Telink part.

[Boot loader](boot-loader.md): 8BitDo's USB loader, its commands, entry,
and how the app slot is ciphered.

[Radio core](radio-core.md): the Bluetooth core, its command rings,
commands and events, Classic and LE behaviour, its ROM and link layer.

Pins are written `group.pin` (Yichip GPIO group 0, 1, 2). Function
addresses `0x0101xxxx`/`0x0102xxxx` are in stock v1.01's app.

## Chip and memory

| CPU range | Contents |
|---|---|
| `0x00000..0x07000` | mask ROM (reading `0x7000..0x8000` resets the chip) |
| `0x20000..0x30000` | SRAM, 64 KiB |
| `0x01000000..0x01080000` | flash, 512 KiB (see below) |
| `0xc0000..0xc9000` | the radio core's window ([radio-core.md](radio-core.md)) |
| `0xf0000..` | peripherals |

- Mask ROM entries used by stock (Yichip `rom_api.h` names): `init_otp`
  `0x442c`, `read_otp` `0x449c`, `qspi_flash_read` `0x48f6`,
  `enc_write_flash` `0x51f0`, sector erase `0x48b5` (opcode `0x20`),
  `qspi_flash_write` `0x47f5` (256-byte page programs), block check
  `0x513c`, LPM register read `lpm_bt_read` `0x4d25`.
- The CPU runs at 48 MHz as the boot loader leaves it: HCLK_CON `0xf8560`
  & 15 = 2, 192 MHz / (2 + 2). SysTick counts the CPU clock. Code from flash
  runs a busy loop about five times slower than its cycle count.
- Soft reset: RST_EN `0xf8574` |= 1, then RESET `0xf857c` = `0x55`. It
  starts the boot loader, which starts the app. RAM survives it.
- Pins 2.0, 2.14 and 2.15 read 0 whatever drives them until LPM register 1
  bit 17 is cleared (stock does this first at boot, `0x010272f8`: read via
  ROM `0x4d25`, write back through `0xc804c..0xc804f` and strobe
  `0xc8005` = `1 << 1`; `0xa8f20000` before, `0xa8f00000` after). The
  setting survives a soft reset.
- Stock static RAM ends at `0x2296c` (Keil region table at `0x0102cd5c`:
  compressed initial data `0x0102cd7c` -> `0x20000`, `0x690` bytes; zero fill
  from `0x20690`, `0x22dc` bytes).

## Flash

The flash is ciphered as the CPU sees it: each 32-byte block is 36 bytes of
flash (data and a check word), blocks of each 32 KiB window shuffled by a
key. Details in [../firmware/updater.md](../firmware/updater.md) "The
Ultimate 2C Bluetooth"; the data pages are raw, unciphered flash.

| CPU address | Contents |
|---|---|
| `0x01000000..0x01017000` | 8BitDo boot loader (code to about `0x01007500`, zeros from `0x01008000`) |
| `0x01017000` | version page: version (`101`) at `0x01017040`, build date (`20250723`), pointers into the loader, calibration-like values, a chip-bound signature the loader checks. Never erase |
| `0x01018000..0x0107c000` | app slot (400 KiB); stock v1.01 uses 96 KiB from `0x01018000` |
| `0x0107c000..0x01080000` | data pages, raw flash |

Stock's data pages:

| Page | Use |
|---|---|
| `0x0107c000` | unused (blank) |
| `0x0107d000` | pairing data (from `+0x100`) |
| `0x0107e000` | user data, 4 × 28 bytes (`0x01022790`) |
| `0x0107f000` | the radio core's bond table, 170 bytes (`0x01022774`: erase + write) |

- A CPU (bus) read of a data page resets the chip; they are read through
  the ROM's QSPI routines. Stock's read `0x010286e8`, erase `0x01028724`,
  write `0x0102874c`: interrupts off and bit 7 of `0xf881c` cleared around
  each (`0x01028168`, restored by `0x01027414`); the read opcode comes from
  `0xf881c` bits 16-23.
- Reading `0x01010440` resets the chip.

## Inputs

Key matrix: rows 1.5, 0.6, 0.1, 2.4 (driven low in turn), columns
0.9..0.13 (pull-up, low = pressed).

| Row | col 0 | col 1 | col 2 | col 3 | col 4 |
|---|---|---|---|---|---|
| 0 (1.5) | D-pad up | X | Plus | R4 | L4 |
| 1 (0.6) | D-pad left | Y | Star | R1 | L1 |
| 2 (0.1) | D-pad down | A | Minus | right stick click | left stick click |
| 3 (2.4) | D-pad right | B | Capture | Sync | — |

On their own pins, pull-up, low = pressed: Home 1.10, L2 2.11, R2 2.12.
L2 and R2 are digital; the pad has no analog triggers. Labels are Nintendo
order: B south, A east, Y west, X north.

### Sticks and battery: the ADC

The ADC belongs to the radio core's register space, so it works only with
the core powered (stock's ADC init `0x01018bf8` starts the core first).

- Stock init `0x01018990`: pins 2.5, 2.6, 2.8, 2.9, 2.10 configured `0xc0`.
- A conversion: `0xc8971` = `mode << 4 | 0x8e`, `0xc8970` = `0x7f`,
  `0xc8972` = `0xaa`, `0xc8973` = `channel | 0x30`; 100 µs later the value
  is `*(u16 *)0xf850e & 0x7fe`.
- Channels: left X 4, left Y 5, right X 0, right Y 1; battery 3; channel 2
  reads a constant (1344).
- X falls to the right on both sticks; left Y falls downward, right Y rises
  downward. Rest about 1390, travel about ±270.
- Stock: sticks `0x01025ef0` (through `0x0101dce0`, which retries a raw value
  outside `0x400..0x7c0` and re-inits the ADC), centre calibration
  `0x01021700`, battery `0x01025fc4` (channel 3, × 370 / 100), a 0 V check on
  channel 3 with pin 2.8 driven low (`0x01018868`), 16 bytes of OTP
  calibration from offset 300 into `0x20fa4`. Battery scale in volts: not
  established.

### Motion sensor

I2C: SDA 2.0 (GPCFG `0x3b`), SCL 1.15 (GPCFG `0x3a`, external pull-up).
SDA needs LPM register 1 bit 17 cleared (above).

- This pad's sensor is an ICM-42670 type (TDK) at address `0xd0`:
  WHO_AM_I (`0x75`) `0x63`, register 0 `0x01`.
- Stock probes in turn: BMI160 at `0xd0` (register 0 = `0xd1`),
  InvenSense types at `0xd0` (`0x75` = `0x38`/`0x3a`, `0x67`/`0x63`,
  `0x20`), QMI8658 at `0xd6`/`0xd4` (register 0 = `0x05`, `0x010284a4`),
  first at slow then fast I2C timing (`0x01020036`).
- Stock's ICM-42670 variant: init `0x0102079a`; read `0x01020420`, 12 bytes
  from `0x0b`, big-endian, accelerometer then gyro; GYRO_CONFIG0 (`0x20`)
  ±2000 dps 800 Hz, ACCEL_CONFIG0 (`0x21`) ±8 g 800 Hz, PWR_MGMT0 (`0x1f`)
  low noise. Its report carries y, -x, z of both.
- I2C engine: DMA channel 5 at `0xf8d00`: out buffer `+0`, in buffer `+4`
  (16-bit SRAM offsets), lengths `+8` (`out << 16 | in`), `+0x20` bit 24 =
  read; pulse `+0xf` bit 5, then `0x80` starts; `+0x10` bit 0 = done.

## Outputs

| Pin | Use |
|---|---|
| 1.11, 1.6, 0.15, 2.3 | player lights 1-4, high = lit |
| 0.5 | Home light, high = lit |
| 0.4 | light below Home, high = lit |
| 2.2 | Sync light (red), high = lit; stock drives it from its battery/charge logic (on, off, 500 ms blink) |
| 1.2 | motor, PWM timer 1 (GPCFG `0x11`) |
| 1.1 | motor, PWM timer 2 (GPCFG `0x12`) |

Stock passes pin masks, e.g. `(2, 8)` is 2.3.

Motors: stock `0x01022940(100, a, b)` runs both at 100 Hz: period =
timer clock / 100, unit = period / 255; high and low counts `unit * duty`
and `unit * (255 - duty)`; duty 0 stops the timer. Timers at `0xf0c00`:
timer n's counts at `+8n` and `+8n + 4`, control at `0xf0c48`, 4 bits per
timer (bit 0 = on), counting the CPU clock. Which motor is which side and
which count is the on time: not established.

## Power

| Pin | Use |
|---|---|
| 0.7 | power hold, output: high keeps the pad on; low on battery switches it off. On battery the pad starts only while Home is held |
| 0.14 | USB power, floating input, 1 = present |
| 1.12 | floating input, goes 0 when USB is plugged in: charge status (active low), likely; not established |
| 0.8 | pull-up input, role not established |

Stock GPIO set-up `0x0101c9e0` (modes: 1 floating input, 2 pull-up input,
5 push-pull output).

## USB

- Stock app: Nintendo Pro Controller `057e:2009`. It has no `0x81` output
  report, so the loader cannot be entered from a host ([boot-loader.md](boot-loader.md)).
- Boot loader: `2dc8:3208`, identify model `1a 30` (the Ultimate 2C
  Wireless is `1b 30`).
- The USB device controller loses data on multi-packet OUT control
  transfers: status bit 0 comes while a packet is still arriving (FIFO count
  growing), the next packet is taken while the last is in the FIFO, and an
  event may show count 0 with nothing after. Single 64-byte packets are
  reliable. Stock's own feature reports are single packets.
- Status register `0xf6027`: bits 0-3 are the IN-done flags; bit 4 stays set.

## Bluetooth address

Stock's address: `E4:17:D8` and three bytes from the chip ID (this pad
`E4:17:D8:03:02:01`). The radio core takes the address from the app at
each start ([radio-core.md](radio-core.md)).
