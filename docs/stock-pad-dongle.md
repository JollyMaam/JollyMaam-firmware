# Stock pad and dongle interaction

This note records what the custom dongle observed while connecting to a stock
Cyclone 2 pad, and what we recovered from the stock dongle 1.21 firmware. Packet
lengths below are HID payload lengths delivered by the interrupt L2CAP channel.
They do not include Bluetooth or L2CAP headers. “Observed” means measured in a
successful hardware session; “inferred” marks interpretations that still need
validation.

## Link establishment

The successful test began with the custom dongle paging the pad. The classic
Bluetooth exchange completed link-key authentication and encryption, then opened
the HID control and interrupt channels. The dongle remained the master. It did
not need a role switch, and normal AFH remained enabled.

During name exchange the pad reported the Bluetooth name “Game Pair Mode.” This
is the remote device name observed over LMP; we do not know that it represents a
separate HID negotiation mode. SSP user-confirmation activity also occurred,
but the connection completed without a button-confirmation workflow. These
details did not identify a special pairing handshake beyond normal classic
Bluetooth authentication, encryption and HID channel setup.

The important behavior came after HID opened. Early attempts authenticated and
opened HID, then the pad disconnected after roughly eight seconds. The stable
configuration began sending a stock-format HID output report immediately after
both HID channels opened and repeated it every 100 ms. The pad then stayed
connected for more than 85 seconds, delivered controls, and did not disconnect.
This strongly implicates the output report or its cadence, but does not prove
which fields are required or the minimum acceptable interval.

The stock dongle’s output is not the standard 79-byte DualShock 4 output report
we first tried. Its 30-byte HID payload is:

| Offset | Length | Contents |
| --- | ---: | --- |
| 0 | 1 | `A2`, HID output transaction prefix |
| 1 | 1 | `11`, report ID |
| 2–4 | 3 | Reserved bytes |
| 5–29 | 25 | Control block |

The final two bytes of the control block contain the low 16 bits of IEEE
CRC-32 over the preceding 23 control-block bytes, little-endian. The minimal
keepalive used by our dongle leaves motor, control and USB-mode fields zero.
The first send is scheduled through L2CAP CAN_SEND_NOW so it is not lost when
the controller has no transmit buffer available. The input/output scheduling
and recovered stock builder are also described in
[the dongle architecture note](../architecture/dongle.md).

One pad LED turned blue during the stable session, then went dark while the
Bluetooth link and input packets continued. The LED therefore did not reliably
indicate link loss in this test; the reason for its transition is unknown.

## Input reports

While idle, the pad sent two report types: 88-byte report `0x11` and 22-byte
report `0x31`. Together they arrived at about 160 HID packets per second. The
meaning of report `0x31` has not been identified. During stick movement, report
`0x11` packets grew in eight-byte increments, consistent with batched records.

For report `0x11`, the following offsets were validated across complete packet
captures. Offsets count from the first HID payload byte (`A1`):

| Bytes | Observed meaning |
| --- | --- |
| 0 | `A1`, HID input transaction prefix |
| 1 | Report ID `0x11` |
| 74–75 | Batch length in bytes, big-endian |
| 76–77 | Low 16 bits of IEEE CRC-32 over the batch, big-endian |
| 78–79 | Auxiliary-data length, big-endian |
| 82–83 | Low 16 bits of IEEE CRC-32 over bytes 1–81, little-endian |
| 84 onward | Batch, auxiliary data, then four trailing bytes |

The total packet length is `88 + batch_length + auxiliary_length`. The four
trailing bytes are part of the fixed 88-byte section, outside the batch and
auxiliary areas; their purpose is unknown. They are not always zero. The
observed payload structure includes
other fields in the fixed header whose meaning has not been established.

Each batched record is eight bytes: four little-endian 16-bit values. During
one-stick movement, the first two values changed smoothly and the last two
remained zero. They are consistent with stick axes, but their exact mapping and
scale are unverified.

In four complete one-second movement windows, the pad delivered 998–1006
records per second inside roughly 200 HID packets per second. This is evidence
for about 1 kHz delivery of stick records through batching. It does not mean
the radio sent 1,000 packets per second, and it does not establish the pad’s
ADC sampling rate or button-to-report latency. Packet captures and decoding
notes are retained outside this merge tree under
`/work/analysis/cyclone2-live-input/`; temporary packet-level debug logging is
not part of the merge build.

## Pairing and test notes

The custom dongle stores a single bond. Re-pairing can replace the saved link
key, so repeating pairing during a connection test can change the state being
measured. The dongle’s `ephemeral` mode keeps app bond changes in RAM until
reboot and is off by default; enable it before experiments where the existing
saved bond must remain intact. SDK radio trim writes remain enabled.

The custom dongle was tested with the experimental controller changes and
radio trace hooks removed. Those changes were not prerequisites for this stable
stock-pad link. The dongle currently receives the stock reports but does not
decode them into USB gamepad reports; its USB interface remains the firmware’s
vendor control interface.
