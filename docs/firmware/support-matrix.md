# What each pad and dongle runs

A slot has a radio side and a USB side:

| Setting | Values | Meaning |
|---|---|---|
| `radio` | `off`, `bluetooth`, `dongle` | the wireless host, if any |
| `radio_type` | a profile type | what the Bluetooth host sees; with `dongle`, what our dongle shows on its USB port |
| `usb_type` | a profile type, or `none` | what a USB host on the pad's cable sees |

A USB host on the cable (frames on the bus, not a charger) wins while
`usb_type` is set: the session is USB and the radio is off. Otherwise the
radio runs, also with a USB host present. With `radio` off and a `usb_type`,
the session waits for a USB host. A pairing always uses the radio. A slot
with neither side runs nothing.

A running session keeps the settings it started with: a change applies on
the next connect.

Changing `radio_type` with `radio` bluetooth, or moving `radio` between
bluetooth and dongle, gives the slot a new address and clears its bond.
Changing `usb_type`, a `radio_type` through the dongle, or turning the radio
off and on again keeps them.

Each board refuses a radio, type or pair it cannot run (the tools show
those choices empty). Through the dongle any type with a USB device runs.

Short names: C2 GameSir Cyclone 2, N2L GameSir Nova 2 Lite, U2CW 8BitDo
Ultimate 2C Wireless, U2CB 8BitDo Ultimate 2C Bluetooth.

## Radios

| | C2 | N2L | U2CW | U2CB |
|---|---|---|---|---|
| `radio` values | off, bluetooth, dongle | off, bluetooth, dongle | bluetooth | off, bluetooth |
| Bluetooth | Classic and LE (2M) | Classic and LE (2M) | LE (2M) | Classic and LE (1M, no data length extension) |
| USB game controller | yes | yes | no: USB is the tool device only | yes |

The U2CW's rear switch is above the slots: on 2.4G every session is the
stock 8BitDo receiver's (an Xbox pad through it), whatever slot is asked
for; on Bluetooth, the slots run.

## Types per side

✓ seen working on that pad (host in brackets); b built and allowed, not
established on that pad in this table; – refused by the board.

USB on the pad's cable (Linux enumerates the IDs shown):

| `usb_type` | USB ID | C2 | N2L | U2CW | U2CB |
|---|---|---|---|---|---|
| `switch` | 057e:2009 | ✓ | b | – | ✓ |
| `switch2` | 057e:2069 | ✓ | b | – | – (no bulk endpoints) |
| `xbox`, `xbox-ble` | 045e:028e | ✓ | b | – | ✓ (`xbox`) |
| `ds5` | 054c:0ce6 | ✓ | b | – | ✓ |
| `ds5edge`, `ds5edge-ble` | 054c:0df2 | ✓ | b | – | – |
| `ds4` | 054c:09cc | ✓ | b | – | ✓ |
| `generic`, `generic-ble` | 16c0:27dc | ✓ | b | – | ✓ (`generic`) |
| `mouse` | none | – | – | – | – |

`xbox` and `xbox-ble`, `ds5edge` and `ds5edge-ble`, `generic` and
`generic-ble` are one USB device each; they differ only over the air.

Bluetooth (`radio` bluetooth):

| `radio_type` | Air | C2 | N2L | U2CW | U2CB |
|---|---|---|---|---|---|
| `switch` | Classic | b | b | – | ✓ (Switch) |
| `switch2` | LE, needs 2M | ✓ (Switch 2) | b | ✓ (Switch 2) | – (no 2M) |
| `xbox` | Classic | b | b | – | b |
| `xbox-ble` | LE | b | b | b | – |
| `ds5` | Classic | b | b | – | b |
| `ds5edge` | Classic | b | b | – | – |
| `ds5edge-ble` | LE | b | b | b | – |
| `ds4` | Classic | ✓ (Linux) | b | – | ✓ (Linux) |
| `generic` | Classic | b | b | – | b |
| `generic-ble` | LE | b | b | ✓ (Linux) | – |
| `mouse` | Classic | b | b | – | b |

Through the C2 dongle (`radio` dongle; the dongle enumerates the type's USB
ID on its own port): `switch`, `switch2`, `xbox`, `xbox-ble`, `ds5`,
`ds5edge`, `ds5edge-ble`, `ds4`, `generic` and `generic-ble` ✓ from the C2;
the pad's bond stays across all of them. `mouse` is refused. The N2L's
dongle setting is built the same way; the N2L with either dongle is not
established. The U2CW and U2CB have no dongle radio.

## Combinations seen working

- A slot with `radio_type` and `usb_type` different (C2: Bluetooth
  `generic-ble` or `ds4`, every USB type above; U2CB: Bluetooth `ds5`, every
  USB type above): the cable gives the USB type, the radio the radio type.
- C2 and U2CB, Bluetooth `ds4` with `usb_type` `xbox`: pairing with a USB
  host on the cable went over Bluetooth (Linux paired it); the next connect
  with that host went to USB as the Xbox 360 pad; with `usb_type` none the
  pad reconnected to Linux over Bluetooth on the same bond.
- `usb_type` none with a USB host on the cable: the radio session (all
  three pads).
- C2, `radio` off: `switch2` and `xbox` on USB; with `usb_type` none,
  nothing runs.
- C2 dongle slot with `usb_type` `ds4`: a connect with a USB host on the
  pad's cable went to USB as the DS4.
- Slots stored as `type` and `transport` read as the same connection
  (stored `bluetooth` and default transports, on the U2CB and U2CW).

Not established: a radio session that moves to USB when a USB host arrives
(the cable plugged in during it); Switch 2 over USB with `radio` off waking
the console over Bluetooth on the new settings; the U2CW on the C2 dongle (no
dongle radio is built for it).
