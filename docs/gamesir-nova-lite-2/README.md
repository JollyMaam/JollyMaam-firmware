# GameSir Nova 2 Lite

Board facts for the GameSir Nova 2 Lite pad (GameSir's code T4N2), a Jieli
BR23 ([../jieli/](../jieli/README.md)), and what its stock firmware shows.
The inputs' and motors' pins are established; the LEDs' are not.

## Chip and flash

- Stock image PID `AC695X_TRANS`, VID field `N2LT`.
- Flash ID `0x856014`: 1 MiB SPI NOR. Chip key `0x3496`.
- The stock image declares 512 KiB (`FLASH_SIZE 0x80000`, code limit
  `0x63000`, image at `EOFFSET` 1). Its other entries, as flash offsets:

| Range | Entry |
|---|---|
| `0x64000..0x78000` | VM (magic `55 aa aa 55`, records appended at boot) |
| `0x79000..0x7a000` | BTIF |
| `0x7a000..0x7b000` | EXIF |
| `0x7b000..0x7f000` | USERIF |
| `0x7f000..0x80000` | key_mac |
| `0x80000..0x100000` | erased |

- The stock `isd_config` sets the loader's UART TX to `PA5` at 1000000 baud
  and the reset input to `PB11` (`PB11_01_0`). The Cyclone 2's stock sets
  `PB04` and `PB01_00_0`.

## USB, stock firmware

Wired, `3537:100f`, product "XBOX 360 For Windows", manufacturer "2.4G":

| Interface | Function |
|---|---|
| 0 | XInput |
| 1 | HID keyboard (report 1) and consumer control (report 3) |
| 2 | vendor HID, usage page `0xff7a`, 64-byte input and output, no report IDs |

Its three wired modes, by LED colour. Only the Xbox 360 mode has the vendor
interface.

| Mode | VID:PID | bcdDevice | Manufacturer / product / serial | Interfaces |
|---|---|---|---|---|
| Xbox 360 (green) | `3537:100f` | `0x0100` | "2.4G" / "XBOX 360 For Windows" / — | as above |
| Switch (red) | `057e:2009` | `0x0200` | "Zikway" / "Pro Controller" / `314838353800c9c4` | one HID: the Pro Controller's 203-byte descriptor |
| DS4 (blue) | `054c:09cc` | `0x0100` | "Sony Computer Entertainment" / "Wireless Controller" / — | one HID: 507-byte DS4 descriptor |

### Loader entry

In Xbox 360 mode, the output report `f2 00 04 f6` on interface 2 restarts
the pad into the MaskROM loader: `4c4a:2342` `BR23UBOOT1.00`, one
mass-storage interface (`08/06/50`, full speed), as in
[../jieli/power.md](../jieli/power.md). GameSir Connect for Windows sends
this, then writes a `.ufw` through that loader.

## Dongle

A Jieli AC632N (BD19). Stock image PID `AC632N_TRANS`, VID field `N2DG`,
chip key `0x3496` (the pad's), flash ID `0xcd7113`: 512 KiB, of which the
image declares 256 KiB (`0x3f000`, key_mac at `0x3f000`).

Running, it is `3537:2106`, manufacturer "Zikway", product "HID zk",
`bcdDevice` `0x0100`, 400 mA: one HID interface, usage page `0xff7a`, 64-byte
input and output reports, no report IDs. Frames of GameSir's configuration
protocol (`a5`, length, `c0`, command, data, checksum: the low byte of the
sum) are answered `a5 05 ff c0 69` with no pad linked.

### Loader entry

Held while USB is connected, and on after, the pairing button restarts the
dongle into the MaskROM loader: `4c4a:3442`, strings
`BR34UBOOT1.00`, INQUIRY vendor `BD19`, product `UBOOT1.00`, one
mass-storage interface (`08/06/50`). The SDK's `cpu/bd19/tools/bd19loader.bin`,
written to RAM at `0x2000` with the loader commands of
[../jieli/power.md](../jieli/power.md) and run with argument 1, reads,
erases and writes the flash with the BR23 loader's commands; the run command
afterwards starts the written firmware.

## Inputs

Found with a probe firmware (`pinprobe.c`, removed after commit 7607b19)
while each control was pressed. The stock image's ADC scan table (file
offset `0x53bd4`, address `0x01e53cf4`: `0x0050000f` (VBAT), then
`gpio, 0, channel` triples) lists the same six analog pins.

### Buttons

Fifteen buttons are a matrix of three column pins, `PC0`..`PC2`, and five row
pins. A pressed key connects its row and column. `PA4` and `PA5` are
connected to each other with no key pressed, so they act as one row.

| Row | `PC0` | `PC1` | `PC2` |
|---|---|---|---|
| `PA3` | D-pad up | View | Y |
| `PA4` / `PA5` | D-pad right | Circle (under Home) | X |
| `PA6` | D-pad left | Menu | A |
| `PA7` | D-pad down | right paddle | B |
| `PA8` | R3 | left paddle | L3 |

Four buttons connect a pin to ground; pulled up, the pin reads low while
pressed:

| Pin | Button |
|---|---|
| `PB1` | Home |
| `PB2` | M (bottom) |
| `PB5` | RB |
| `PB7` | LB |

### Sticks and triggers

Analog, one SAR channel each. Raw 10-bit values (`adc_get_value`) at rest
are near the middle, between 520 and 600.

| Pin | Channel | Control | Full travel |
|---|---|---|---|
| `PA1` | `AD_CH_PA1` | left stick X | right: higher (788 seen) |
| `PB8` | `AD_CH_PB8` | left stick Y | up: lower (264) |
| `PC4` | `AD_CH_PC4` | right stick X | right: lower (254) |
| `PC5` | `AD_CH_PC5` | right stick Y | up: higher (937) |
| `PA10` | `AD_CH_PA10` | LT | pulled: lower (96) |
| `PB3` | `AD_CH_PB3` | RT | pulled: lower (175) |

`PA1` at rest steps between about 540, 575 and 637 every few seconds.

### Other pins

With every free pin of ports A..C pulled down, then up (port D and the flash
pins `PA13`, `PA14` left alone; the flash is on SPI0 port B, 2-bit):

- `PA2` reads high either way.
- `PB12`..`PB15` and `PC8`..`PC15` read low either way.
- `PA0` follows `PC7` when `PC7` is driven, and the other way round.

## Motors

Two motors. Each runs while either of two pins is driven high (one pin at a
time, others floating), felt in the grips:

| Motor | Pins |
|---|---|
| left | `PC3`, `PB9` |
| right | `PB10`, `PA12` |

The stock image's motor table at `0x01e4ffa8` holds two 12-byte records,
`gpio, 2, 2` for `PC3` and `gpio, 3, 3` for `PB10`; the code at `0x01e3485e`
reads byte 4 of a record and scales a 0..255 level to 0..10000 (the SDK's
duty units). `PB9` is MCPWM channel 3's H pin and `PB10` its L pin
(`pwm_hw_h_pin`, `pwm_hw_l_pin` in the SDK's `mcpwm.c`); `PC3` and `PA12` are
no channel's own pin.

None of these 15 pins, driven high for 2 s and low for 2 s, lit an LED:
`PA0`, `PA2`, `PA9`, `PA11`, `PA12`, `PA15`, `PB0`, `PB4`, `PB6`, `PB9`,
`PB10`, `PB11`, `PC3`, `PC6`, `PC7`. The stock image has the string
`app_rgb_init` and no code that references it.

## Not established

Pins of the LEDs, USB presence and the charger; why each motor has two pins; what drives
`PA2` and links `PA0` with `PC7`; whether a button combination enters the
pad's loader (Home + Menu, GameSir's instruction for its updater, did not).
