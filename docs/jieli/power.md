# Power, reset and loader entry

## Internal charger

The BR23 PMU has an internal charger. Its control register is `P3_CHG_CON0`
(P33 address `0x15`):

| Bit | Name | Meaning |
|---|---|---|
| 0 | `CHARGE_EN` | charger enable |
| 1 | `CHGBG_EN` | charger background enable |

The SDK's VBAT reading (`AD_CH_VBAT`) is scaled by 4 in the driver. A calibrated
state-of-charge reading is not established.

## Reset

### Reset sources

Reset causes are read from:

- `P33 RST_SRC` (P33 register `0x12`)
- `R3_RST_SRC` (RTC domain)

A bootloader or earlier code may read and clear these first. The app samples
them early in `app_main`.

SDK labels for `P33 RST_SRC` bits:

| Bit | Label |
|---|---|
| `0x01` | P33 reset |
| `0x02` | DVDD power-on reset |
| `0x20` | software reset |
| `0x40` | PLPM |

The register does not identify the cause alone. A reset from the loader, a
deliberate `p33_soft_reset()` and an unexpected fault all read `P33 = 0x40`.
A pin reset read `P33 = 0x01`.

### Retained memory

Retained SRAM (`NOLOAD` sections) survives software resets and loader resets
while power is on. See [memory-map.md](memory-map.md). Ordinary `.bss` is cleared at
every application boot.

Soft power-off (`power_set_soft_poweroff()`) clears retained SRAM. Across a
soft power-off and wake:

| Storage | Result |
|---|---|
| Retained SRAM | not kept |
| P33 `0x20`, `0x21`, `0x2f` (reserved range) | read `0x00` |
| `R3_RTC_DAT0`, `R3_RTC_DAT4` | written values kept |

The RTC does not run in this configuration, so its data registers hold what
was written. Whether they survive a loss of battery supply is not established.

### Input lines after soft power-off

Pins not on the keep list are parked as inputs with digital input disabled
during soft power-off. At the next boot, until `power_init()`, those lines
read low (pressed) even with their pull-ups enabled; a 1 ms wait does not
change this. After `power_init()` they read correctly. Lines on the keep list
read correctly before `power_init()`.

## USB pin control and loader entry

### USB registers

| Address | Register | Function |
|---|---|---|
| `0x001e1800` | `USB_CON0` | PHY enable, USB core reset, bus mode and interrupt controls |
| `0x001e1804` | `USB_CON1` | access to the MUSB core registers |
| `0x001e5100` | `USB_IO_CON0` | D+ and D− GPIO mode, directions, levels and pulls |

The SDK's BR23 `asm/usb.h` names the USB pin-control bits as follows:

| Bits | Names | Function |
|---|---|---|
| 0, 1 | `DPOUT`, `DMOUT` | GPIO output levels |
| 2, 3 | `DPIE`, `DMIE` | GPIO direction: 1 input, 0 output |
| 4, 5 | `DPPD`, `DMPD` | pull-downs |
| 6, 7 | `DPPU`, `DMPU` | pull-ups |
| 8 | `IO_PU_MODE` | GPIO pull mode |
| 11 | `IO_MODE` | GPIO mode |

The SDK's GPIO routines confirm the output, direction and pull bit assignments.
`IO_MODE` is not a dedicated SE0 control. The exact pull-control behavior when
`IO_PU_MODE` is clear is not established.

### Disconnecting from the host

The D+ pull-up signals a full-speed device's attachment. A reset of the USB
core and removal of that attachment signal are separate controls. Before
handing the bus to a fresh firmware instance, the host needs to observe a
disconnect so it can enumerate the device again.

GPIO mode allows explicit control of both USB lines and their pulls. Selecting
`IO_MODE` and `IO_PU_MODE`, clearing `DPPU` and `DMPU`, and clearing the direction
and output-level bits drives both lines low without pull-ups. That line state
is SE0. USB interrupt handlers must not reconfigure the pins during handover.
The minimum disconnect interval required by the connected host and hub is not
established.

### MaskROM loader

The MaskROM USB loader identifies itself as `UBOOT1.00`. Its boot-state token
occupies 16 bytes of SRAM at `0x00010800`. `nvram_set_boot_state(2)` selects
loader entry; the token must reach SRAM through the data cache before reset.
A watchdog reset can transfer control to the loader.

The Bluetooth controller must be stopped before loader entry. Stopping
advertising alone is not enough.

#### Crystal measurement before enumeration

The loader does not know the crystal frequency. Before it answers on USB, it
attaches with the D+ GPIO pull-up (`USB_IO_CON0` `0xe4c`: `IO_MODE`,
`DMDIE`, `DPDIE`, `DPPU`, `DPIE`, `DMIE`) and does not respond to the host.
Timer 3 (`CON` `0x1a`: rising-edge capture, the osc input, prescale 4)
captures D+ through input channel 2 (`IOMAP_CON2` bits 16–21 = `0x3d`, D+).
Its interrupt (MaskROM `0x112368`) takes the 16-bit difference between
captures and accepts the measurement after four differences in a row, each
within 260 counts of the one before. With a 24 MHz crystal a full-speed frame
is 6000 counts. The loader then programs the PLL, removes the pull-up and
attaches as `UBOOT1.00`. Each attempt waits up to about 1 s, then the pull-up
is removed for about 200 ms before the next attempt.

Host requests during the measurement fail (STALL on the first descriptor
read, no response to SET_ADDRESS). The loader configures the capture input
itself, so no state left before the reset changes the measurement.

Any other packet on the port is also a rising edge on D+. A hub with a single
transaction translator repeats all full-speed traffic to every enabled
full-speed port. Behind such a hub, other full-speed devices polled every
1 ms put extra edges in every frame, and the measurement never completes;
enumeration traffic can also complete it on a period other than the frame.
A loader entry from button combinations in a firmware (the stock Cyclone 2's
Home+Start) behaves the same way, because the
measurement is in the MaskROM.
