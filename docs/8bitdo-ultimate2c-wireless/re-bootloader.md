# Stock USB boot entry

These facts come from static inspection of the Ultimate 2C Wireless pad v1.06
flash backup and receiver v1.03 update image. The pad's manual boot entry and
bootloader protocol are also established on hardware. Receiver boot entry over
USB and its bootloader's USB identity are not established on hardware.

## Software entry

Both stock applications recognize the same 64-byte HID output report:

```
81 05 00 51 00 00 ... 00
```

`0x81` is the report ID, included in the 64 bytes. It does not take the extra
zero byte used for an unnumbered Linux hidraw output report.

The pad's handler at flash `0xf608` calls `0xf5ec`: write analog register
`0x3c = 0x5a`, wait 200000 us, reset. Its bootloader tests this retained flag
to stay in USB boot mode. The receiver's handler at `0xdf74` calls `0xdf50`:
clear the watchdog-enable bit, write the same analog flag and reset. This
establishes the receiver application's boot-entry request; how the receiver
bootloader interprets that flag is not established without its image.

The debug name `chanegBoot_Dinput` in the receiver does not establish a change
to DInput gamepad mode. The inspected handler sets the boot flag and resets.
Parameters 1 and 2 of the `0x51` command must not be substituted for parameter 0.

## Product identity

| Product | Model returned by identify | Stock USB identities in inspected images |
|---|---|---|
| Pad | `1b 30` (`0x301b`) | `2dc8:310a`, `2dc8:301b`, `2dc8:301d` |
| Receiver | `1c 30` (`0x301c`) | `2dc8:310a`, `2dc8:301c` |

**USB PID `0x310a` is shared.** Select by the returned model, then retain the
physical USB port across re-enumeration. A bootloader's USB PID is not its model.

The stock HID vendor interface has Output report ID `0x81`. Its read-only model
query is 64 bytes: report ID `81`, framing byte `05`, then the little-endian
16-byte command header, with command 8 and the other fields zero:

```
81 05 08 00 00 ... 00
```

The reply starts `02 05`, followed by u16 status, u16 echoed command, u16 payload
length, u16 CRC, u32 total and u32 address. Model bytes are at offset 18, with
payload length 2. This is a different envelope from the pad bootloader's
unnumbered reports, whose replies begin `00 05`.

The receiver's model-query handler is at flash `0xd940`, reached by dispatcher
`0xd998`. Its boot-entry handler is reached by the normal USB command dispatcher
at `0xe1fc`. Availability of the vendor HID interface in every stock USB state
is not established. A receiver with no accessible vendor interface cannot be
entered by a hidraw command through a gamepad interface alone.

## Manual entry and recovery

For the pad: unplug USB, hold L1 and R1, tap Home, then connect USB. The
bootloader drives PA0 low and checks PA7 and PC3 low together. Its USB identity
is `2dc8:3208`, product `8BitDo Boot`; identify returns `1b 30`.

A manual receiver boot-entry sequence is **not established**. The pad's shoulder
button sequence does not apply to a receiver. The receiver's bootloader USB
identity, report framing and physical recovery access are not established.
Use a model query to verify any receiver that appears under `2dc8:3208`; do not
assume it contains the pad model or accepts the pad's image.

## Receiver update distinction

The receiver's normal application updater stages firmware at `0x13000`, saves
metadata in sector `0x12000` and uses analog flag `0x3c = 0x5b`. This differs
from explicit boot entry (`0x5a`). Neither staging firmware nor finalizing
metadata is required by the inspected boot-entry handler. Receiver erase,
program, bootloader validation and recovery behavior are not established by the
pad's successful flashing history.

## Evidence identities

- Pad full flash backup SHA-256:
  `f4d925915a35f502080239ffa69649094ceb9a1c4615d4278d3a9c7671cfa974`.
- Receiver v1.03 package, updater type 76, payload PID `0x301c`, SHA-256:
  `e6dd2565e200e647e47982f42eb88ecae99d34ab1e1ab2018b49a2ce5f91a1d9`.
  Official legacy download path:
  `http://dl.8bitdo.com:8080/firmwareFile/upload/8c7285c9-e169-4f22-8e3b-928d08599151.dat`.
- Receiver offsets above are physical flash addresses for an application loaded
  at `0x6000`; its first `0x2400` bytes execute from SRAM at CPU address zero.
