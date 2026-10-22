# Bluetooth radio

The radio is the Jieli BR23 controller (Classic and LE baseband and link
control). This firmware replaces the SDK's controller policy with its own; the
baseband behaviour below is the SDK's and the chip's.

The statements here come from the SDK headers, the controller library, or
observed controller responses. Where a behaviour is a controller defect, it is
marked.

## Capabilities

- Classic (BR/EDR) and LE, peripheral role.
- LE Data Length Extension and the 2M PHY are configurable through the SDK.
- Read Buffer Size reports 10 ACL buffers of 640 bytes.
- LE Read Buffer Size reports 4 LE buffers of 251 bytes.
- Classic Read RSSI returns 0. Only LE gives absolute dBm values.
- Default LE data PDU length is 27 bytes until LE Set Data Length asks for more.

## Controller defects and gaps

- **Read Encryption Key Size** is advertised as supported and never answered.
  The transport layer must supply the completion

- **LE Read Local P-256 Public Key and LE Generate DHKey** are stubs that
  never answer, and Read Local Supported Commands does not list them
  (octet 34, bits 1 and 2). LE Secure Connections needs the host to supply
  P-256.
- **Write Stored Link Key and Delete Stored Link Key** are stubs. The controller
  holds link keys only for the life of a connection. Persistent bonds are kept
  in flash by the host.
- **QoS Setup**, in the unpatched SDK, echoes the requested values back in its
  completion event (SDK event code `0x01`) without sending an LMP QoS request.
  The LMP QoS sender exists and can be invoked explicitly.
- The SDK's local name handling accepts 31 bytes.

## Connection behaviour

- Classic reconnects are started by the pad. Outgoing HID channels open only
  after authentication: security level 2 outgoing, level 0 incoming. Opening
  before authentication fails with reason 5.
- A default link policy without role switch did not keep the pad as master: the
  host's role switch still completed.
- LE links ask for 251-byte PDUs. At 27 bytes a Switch 2 report took three
  connection events.
- A Switch 2 host sets a 5 ms interval, peripheral latency 0 and 2 s supervision,
  then moves the link to the 2M PHY. It does not request a data length update, so
  the pad must send LE Set Data Length itself.

## Transmit power

Transmit power is set by a table of TXSET and TXPWR values, one entry per step:

| Step | TXSET |
|---|---|
| 0 | `0xF010C2` |
| 4 | `0xFFFFC2` |

TXPWR holds a single value for all steps. The table's steps start at the value
stored by `bt_max_pwr_set()`. That value is 0 until the function is called.

In normal mode, `bt_get_pwr_max_level()` caps the step at 4. For LE, the step
used is the smallest of:

- that cap,
- the BLE value in `bt_max_pwr_set()`'s packed word (bits 12–15),
- the level fixed by `ble_set_fix_pwr()`.

The 0–9 dBm list in the SDK header is not the table this mode uses.

`bt_production_test(1)` selects a production power table, with top step TXSET
`0xFFFFF2` and TXPWR `0x70237F`. It also sets the radio supply VDC13 to 1.40 V
(step 7). `bt_production_test(0)` sets 1.30 V (step 5). The board's boot value
is step 7.

Call `bt_max_pwr_set()` at boot. See [sdk-toolchain.md](sdk-toolchain.md).

## Baseband frame registers

The SDK's raw baseband frame structure, recovered from the SDK's controller
library, with the register writers that set each field:

| Field | Offset | Writer | Meaning |
|---|---:|---|---|
| `bdaddr0/1/2` | 0..6 | `__write_reg_bdaddr` | 24-bit LAP |
| `bchl/bchm/bchh` | 6..11 | `__write_reg_bch` | 34-bit BCH parity |
| `miscntl` | 12 | `__write_reg_miscntl` | `[2:0]` LT_ADDR, `[3]` EDR flag |
| `format` | 13 | `__write_reg_format` | `[4:0]` frame format, `[6:5]` kept, `[7]` train |
| `wincntl` | 16 | direct | whitening control (semantics not established) |
| `txfreq` / `rxfreq` | 21 / 23 | direct | channel |
| `txptr0/1`, `rxptr0/1` | 24..31 | `__write_reg_txptr`, `__write_reg_rxptr` | packet buffers |
| `linkcntl0/1` | 32 / 34 | `__write_reg_linkcntl` | link control words |
| `txphdr0/1` | 36 / 38 | `__write_reg_txphdr` | payload headers |
| `rx_clknl/h` | 52 / 54 | — | clock of the last reception |
| `bitoff` | 56 | — | sub-slot position of that reception, in µs |

The access code's LAP and BCH parity are writable separately. This allows an
access code that ordinary HCI paging does not produce.

The transmit payload must lie within a 32 KiB window above a base the
controller holds in RAM. The SDK's `__write_reg_txinfo()` resets the chip if
the payload is outside it. The base value depends on the build's allocation.

### Slots and packet types

The SDK's raw scheduler uses 625 µs slots. Fixed-transmit packet types:

| Type | Packet | Payload (bytes) |
|---|---|---|
| 0, 1, 2 | DH1, DH3, DH5 | 27, 183, 339 |
| 3, 4, 5 | 2-DH1, 2-DH3, 2-DH5 | 54, 367, 679 |
| 6, 7, 8 | 3-DH1, 3-DH3, 3-DH5 | 83, 552, 1021 |

DM1 is in the radio packet table but has no `link_fix` packet-type index.

Raw transmission owns the scheduler. It cannot run alongside a normal profile
session.

Shutting the controller down clears the buffer base to zero. Reopen the
controller before raw transmission.
