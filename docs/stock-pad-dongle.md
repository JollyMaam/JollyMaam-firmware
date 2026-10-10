# Stock pad and dongle protocol

How a stock Cyclone 2 pad connects to a Bluetooth host, and the HID traffic it
exchanges, as observed on the air and recovered from the stock dongle 1.21
firmware. Packet lengths are HID payload lengths on the interrupt L2CAP channel,
without Bluetooth or L2CAP headers.

Items marked "not established" have no confirmed explanation.

## Link establishment

- Classic Bluetooth. Link-key authentication and encryption complete, then the
  HID control and interrupt channels open.
- The dongle is the master. No role switch is needed. Normal AFH stays enabled.
- The pad reports its remote name as `Game Pair Mode` during name exchange. Whether
  this selects a separate HID mode is not established.
- SSP user-confirmation activity occurs, but the connection completes without a
  button-confirmation step.

## Output report that keeps the link up

After both HID channels open, the host must send a stock-format output report
right away and repeat it every 100 ms. Without it the pad disconnects after about
eight seconds of an open HID session.

The stock output is not the 79-byte DualShock 4 output report. Its 30-byte HID
payload:

| Offset | Length | Contents |
|---|---:|---|
| 0 | 1 | `A2`, HID output transaction prefix |
| 1 | 1 | `11`, report ID |
| 2–4 | 3 | reserved |
| 5–29 | 25 | control block |

The last two bytes of the control block hold the low 16 bits of IEEE CRC-32
over the preceding 23 control-block bytes, little-endian.

A minimal output with motor, control and USB-mode fields zero held a link for
more than 85 seconds with controls delivered. Which fields are required, and
the minimum interval, are not established.

The first send should wait for the L2CAP "can send now" event. Otherwise
controller buffer pressure can drop it.

The LED on the pad turned blue during a stable session, then went dark while
input kept arriving. It is not a reliable link indicator. Its cause is not
established.

## Input reports

While idle the pad sends two report types, together about 160 HID packets per
second:

- report `0x11`, 88 bytes plus batch and auxiliary data
- report `0x31`, 22 bytes. Its meaning is not established.

Stick movement grows report `0x11` in eight-byte steps, so the pad batches its
records.

### Report 0x11 header

Offsets count from the first HID payload byte (`A1`):

| Bytes | Meaning |
|---|---|
| 0 | `A1`, HID input transaction prefix |
| 1 | report ID `0x11` |
| 74–75 | batch length in bytes, big-endian |
| 76–77 | low 16 bits of IEEE CRC-32 over the batch, big-endian |
| 78–79 | auxiliary data length, big-endian |
| 82–83 | low 16 bits of IEEE CRC-32 over bytes 1–81, little-endian |
| 84 onward | batch, then auxiliary data, then four trailing bytes |

Total length is `88 + batch length + auxiliary length`. The four trailing bytes
are inside the fixed 88-byte section. They are not always zero. Their purpose is
not established. Other header fields are not established.

### Batch records

Each batch record is eight bytes: four little-endian 16-bit values. During
one-stick movement the first two values change smoothly and the last two stay
zero. They match stick axes, but the mapping and scale are not verified.

In complete one-second movement windows the pad delivered 998–1006 records per
second, inside roughly 200 HID packets per second. This shows stick data at about
1 kHz through batching. It does not mean the radio sent 1000 packets per second,
and it says nothing about the pad's ADC rate or button-to-report latency.
