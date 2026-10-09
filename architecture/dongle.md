# GameSir dongle firmware

The dongle build shares the BR23 platform and owned controller with the pad,
but uses `board_dongle.c`, `dongle.c` and `dongle_bt.c`. It maintains a stock-pad
Bluetooth HID connection. It does not yet decode input into USB gamepad
reports, drive the dongle LED, or implement audio or rumble forwarding.
USB provides the existing vendor SCSI control interface, not a gamepad.

## Connection and storage

Stock VM item 2 supplies the dongle address, least significant byte first;
item 3 supplies the pad's base address in display order. Neither is modified.
Without a usable stored identity, a local unicast address is derived from the
flash UID. Without a stored peer, the dongle listens until a pad connects or
`padtool --dongle target AA:BB:CC:DD:EE:FF` supplies an address.

The app keeps one bond in VM item 1. A saved bond is retried; without one, the
stored pad base and its five adjacent addresses are probed after an initial
15-second listening period. An explicit target selects only that address.
Failed pages are retried, with a two-second timer between attempts. Page
scanning stops while an ACL link is up and resumes after disconnection.

Outgoing HID channels require authentication/encryption before opening;
incoming channels use the registered LEVEL_0 policy. Once control and
interrupt channels are both open, the dongle sends the stock-format keepalive
and repeats it every 100 ms. Sending waits for L2CAP_CAN_SEND_NOW so controller
buffer pressure cannot silently discard the initial output. Normal AFH is
enabled. The app does not force a role switch.

`padtool --dongle pair` clears the bond and disconnects an existing link.
Target and pairing commands are queued onto the Bluetooth thread; USB callers
do not modify the live bond or call BTstack directly. `status` reports the
connection state. Logs contain connection transitions and errors, without
packet dumps, link-key bytes, LMP traces or periodic radio snapshots.

`padtool --dongle ephemeral` keeps subsequent app bond changes in RAM until
reboot. It is off at boot and must be enabled before pairing experiments.
SDK radio trim writes remain permitted, as on the pad. The dongle does not
implement settings or `keep`; reboot restores the last persisted bond.

## Keepalive format

The stock dongle 1.21 builder at 0x01e0c26a sends a 30-byte HID payload without
auxiliary data: A2 11, three reserved bytes, and a 25-byte control block.
The block ends with the low 16 bits of IEEE CRC-32 over its first 23 bytes,
little-endian. Our minimal keepalive leaves motors, control flags and USB
mode zero. This configuration sustained a stock-pad link; the necessity of
individual control fields and the minimum required interval are not known.

The stock pad also connected with all experimental controller fixes and
radio trace hooks removed: more than 85 seconds, no disconnects, controls
received, and four complete movement packets passing both checksum checks.
That test used normal AFH and remained in the master role. Controller fixes
are reviewed independently; they are not prerequisites for this link.

## Captured input format

Idle reception included 88-byte report 0x11 and 22-byte report 0x31, around
160 HID packets/s combined. Movement produced larger report-0x11 packets
containing batches of eight-byte records. Complete captures validated this
framing; offsets include A1 and the report ID:

| Bytes | Meaning |
| --- | --- |
| 74–75 | Batch byte length, big-endian |
| 76–77 | Low 16 bits of IEEE CRC-32 over the batch, big-endian |
| 78–79 | Auxiliary byte length, big-endian |
| 82–83 | Low 16 bits of IEEE CRC-32 over bytes 1–81, little-endian |
| 84 onward | Batch records, then auxiliary bytes and four trailing bytes |

Total HID length is 88 + batch length + auxiliary length. The trailing bytes
are not necessarily zero; their meaning is unverified.

Records decode as four little-endian 16-bit values. During one-stick movement,
the first two changed smoothly and the last two stayed zero, consistent with
four stick axes; exact scaling remains unverified. Sustained movement delivered
998–1006 records/s in complete one-second windows, carried in roughly 200 HID
packets/s. This supports approximately 1 kHz stick-record delivery by batching,
not 1,000 separate radio packets/s, ADC sampling frequency or button latency.

The detailed capture instrumentation is retained on
`archive-dongle-debug-20261009`. Local captures and decoded records are in
`/work/analysis/cyclone2-live-input/`. The merge build omits that instrumentation;
input decoding and USB forwarding are future work.
