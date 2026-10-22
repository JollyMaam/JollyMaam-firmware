# GameSir Nova 2 Lite

Board facts for the GameSir Nova 2 Lite pad (GameSir's code T4N2), a Jieli
BR23 ([../jieli/](../jieli/README.md)), and what its stock firmware shows.
The pins are not established.

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
[../jieli/power.md](../jieli/power.md) and run with argument 1, reads the
flash.

## Not established

Pins of every input, sticks and triggers, motors, LEDs, USB presence, the
charger; whether a button combination enters the pad's loader (Home + Menu,
GameSir's instruction for its updater, did not).
