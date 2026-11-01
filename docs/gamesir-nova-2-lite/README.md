# GameSir Nova 2 Lite

Board facts for the GameSir Nova 2 Lite pad (GameSir's code T4N2), a Jieli
BR23 ([../jieli/](../jieli/README.md)), and what its stock firmware shows.
The inputs', motors' and Home light's pins are established.

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

The pairing button is on `PA8`: with the pin pulled up, it reads low while
pressed (found with a probe of `PA0`..`PA9` and `PB0`..`PB9`). With our
firmware flashed, the ROM loader is also entered by the `USB_KEY` pattern of
`cyclone2-esp32/jl-forced-usb` sent on D+/D- at power-up.

### BD19 differences that matter to our firmware

- GPIO groups are A, B, D and P (`gpio / 16` = 2 is port D); there is no
  port C, and reading `JL_PORTC` raises an exception.
- The heap is the RAM between the end of `.bss`/overlays and the retained
  area, about 18 KiB in our build; when `malloc` finds nothing it resets the
  chip (source `0x20`) instead of returning NULL.  `xPortGetFreeHeapSize()`
  reports about 66 KiB regardless.

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

The trigger stop switches are not wired to the chip. Flipping them changes
none of `PR0`..`PR3`, `PA2`, `PA5`, `PA9`, `PA11`, `PA15`, `PB4`, `PB6`,
`PB11`, `PC6`, `PD5`..`PD7` (each pulled up), and no pair of these pins or
the matrix rows (each of the pins driven low in turn). Pulled up, `PR1`,
`PD6` and `PD7` read low.

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

## LEDs

None of these 15 pins, driven high for 2 s and low for 2 s, lit an LED:
`PA0`, `PA2`, `PA9`, `PA11`, `PA12`, `PA15`, `PB0`, `PB4`, `PB6`, `PB9`,
`PB10`, `PB11`, `PC3`, `PC6`, `PC7`.

The stock image drives a serial chain (WS2812 type) from `PB0`, from code it
runs in RAM (RAM address `0x0a64`; the image's RAM code starts at file offset
`0x59408`, listing address `0x01e59528`). That function:

- turns interrupts off, saves timer 3's `CON`, `CNT` and `PRD`, and runs the
  timer for the bit timing;
- sends 18 bytes, most significant bit first: per bit it waits out the bit
  period, sets bit 0 of `JL_PORTB->OUT` (output in `DIR`), waits one of two
  times (the longer one for a 1), and clears it;
- restores timer 3 and turns interrupts back on.

The 18 bytes are six lights of three bytes each, kept at RAM `0x7ccc`
(`0x7650 + 1660`). The light code stores a colour `0xXXYYZZ` as the bytes
`XX`, `YY`, `ZZ` (code at `0x01e3448a`) and scales each by a per-light
brightness (`0x01e34584`, `0x01e3fa1e`). A frame goes out only when the 18
bytes differ from the last one sent (`0x01e42744`). The string
`app_rgb_init` is printed in the start-up code at `0x01e40bce`.

On the pad, driven from `PB0` with 1.7 us bits (high some 0.3 us for a 0,
some 1 us for a 1):

- Position 0 is the Home light, its bytes red, green, blue.
- Positions 1 to 5, each lit alone, lit nothing.
- The light keeps its colour while the pad has power, across resets and
  flashing: the stock image's white stayed on until the chain was sent a
  frame.

## Power

Stock sleep callbacks, wake tables and shutdown pin handling are described in
[re-stock-power.md](re-stock-power.md).

`PA0` is an active-high USB sense input: high with USB connected, low with
it disconnected, with both internal pulls disabled. It is electrically
connected to `PC7`.

The BR23 can enter soft power-off and wake on either Home (`PB1`, falling
edge with a pull-up) or USB insertion (`PA0`, rising edge without pulls).
A brief Home tap can be released before the app starts; the GPIO wake
flags remain available to identify it.

With USB connected, the BR23's LDOIN detectors read 0: `LDO5V_DET` (bit 5
of `P3_LDO5V_CON`) and `LVCMP_DET` (bit 5 of `P3_LVCMP_CON`, LDOIN against
VBAT), after `LDO5V_EN(1)`, `LVCMP_EN(1)`, `LVCMP_CMP_SEL(1)`. The chip's
charger, which charges from LDOIN, has no input. With it off, the battery
read 4064 mV, then 4152 mV about an hour later, USB connected throughout.

## Not established

Whether the pad has lights other than Home, the charger IC and USB sense
circuit; why each motor has two pins; what drives `PA2`; whether a button
combination enters the stock pad's loader (Home + Menu, GameSir's
instruction for its updater, did not).
