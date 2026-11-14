# Radio core

The YC3121's Bluetooth is a separate core with its own ROM. It runs a
complete Classic HID device and LE GATT server inside itself; the ARM
talks to it with Yichip's own command set over byte rings, not HCI. Stock
loads a patch blob into it at start.

## The window

The ARM sees the core through `0xc0000..0xc9000`, byte-wide (a 32-bit read
returns one byte four times).

| Range | Contents | Mirrors |
|---|---|---|
| `0xc0000..0xc1000` | command/event rings | `0xc2000` |
| `0xc1000..0xc2000` | RAM (mostly power-up contents) | `0xc3000` |
| `0xc4000..0xc5000` | the core's data RAM (core address `0x4000` + offset) | `0xc5000`, `0xc6000`, `0xc7000` |
| `0xc8000..0xc9000` | the core's registers (LPM domain, ADC at `0xc8906`/`0xc897x`, charger from `0xc8129`) | |

No ROM is visible in the window. Unknown register reads in `0xc8000..` can
stall the CPU.

## Start-up

Stock `0x01020d58`:

1. SYSCTRL `0xf8520+0x1c` = `0x94`; `0xc8010` = 1; wait for `0xc8043` bit 7,
   clear it.
2. Two LPM register writes (`0x01028078`: 32-bit value to `0xc804c..0xc804f`,
   strobe `0xc8005` = `1 << n`): reg 1 |= `0xd00000`, reg 0 = `0x3084cf0f`.
3. Clear `0xc0000` and `0xc4000` (4 KiB each).
4. Load the patch blob (`0x010269e4`, below).
5. `0x1c` = `0x9c` releases the core.

After start the core sends event `0x09` with no data.

### The patch blob

Stock v1.01 at `0x01029314..0x0102b0c0` (7596 bytes): `00 02`, `aa 55`, u16
code length (6010), the code, then 69 records `{u16 0x55aa, u16 len, u16
addr, data}` into the core's data RAM.

- Code: 6008 bytes and a CRC-16/CCITT-FALSE (poly `0x1021`, init `0xffff`,
  not reflected) of them, big-endian. The ARM does not check it.
- The code goes through a port, not the window: `0xf8540+0x2c` bit 13
  cleared, `0xc8024` = 0, `0xc8022` = 0, `0xc8023` = `0x80`, `0xf8e00+0xc` =
  8, DMA channel 6 streams the bytes to `0xc8025`, then `0xc8023` = 0 and
  `0xf8e00+0xc` = 0.
- Records land at: `0x4000` hook bitmap (64 bytes), `0x40a0` BD address,
  `0x4341` LE advertising data, `0x4361` scan response, `0x4381` and
  `0x44e7` names (LE, Classic), `0x4683` SDP records (602 bytes), `0x4975`
  GATT table (202 bytes), `0x4b6f` radio register table, `0x4ff0` ring
  descriptors. The loader patches `0xc453d` (1 + 3 bytes from elsewhere) and
  `0xc4acf` (0) on the way.
- The blob's defaults: name "BT3.0 Keyboard" (Classic) / "BT4.2 Keyboard"
  (LE), class `0x002508`.

### The code port

The port at `0xc8025` also reads: the address is `0xc8022` (bits 16-23) :
`0xc8023` (bits 8-15) : `0xc8024` (bits 0-7). Reads need about 10 µs after
setting the address and 1 µs per byte; faster reads can stall the CPU.

- Patch RAM: 8 KiB at `0x8000` (mirrored at `0xa000`, `0xc000`, `0xe000`,
  `0x18000`, `0x1c000`, `0x28000`).
- ROM: 64 KiB in four 16 KiB blocks at `0x44000`, `0x54000`, `0x64000`,
  `0x74000` (each mirrored `0x8000` higher). In that order they end with
  zeros from `0xf756` and a CRC-16/CCITT-FALSE of the rest (`f1 b1`).
- Elsewhere: fill words.

## Rings and packets

Ring descriptors are pointed to by SRAM `0x20028` / `0x2002c` (stock's
variables): four LE16 fields (start, end, read, write), offsets from
`0xc0000`. Stock locks while touching them (`0xc4fe0+0xf` = 1 and
`0xf8520+0x1c` |= 4) when its flag at `0x20030` is 1.

- Command: `01 <opcode> <len8> <data>`, or `01 <opcode> <len16 LE>
  <data>` for the long ones (`0xa0`, `0xb0`).
- Event: `02 <id> <len> <data>`.
- Every command is acknowledged by event `0x06 {opcode, status}`, status
  0 = done; `0x77` and `0x78` answer `0x29 {u16 handle}` instead.

## Commands

Names in quotes are from stock's strings; others are what the command was
seen to do.

| Op | Data | Does |
|---|---|---|
| `0x00` | 6-byte address, LSB first | sets data `0x40a0` |
| `0x01` | 6-byte address, LSB first | sets data `0x4472` (the identity SMP sends) and `0x40a0`; not the LE advertising address |
| `0x02` | flags | visibility ("SetVisibility"): bit 0 discoverable, bit 1 connectable (Classic), bit 2 LE advertising (connectable). 0: a bonded host cannot connect |
| `0x04` | 14 bytes | LE name, NUL padded; also the scan response `0f 09 <name>` |
| `0x09` | u16 handle, data | LE notification |
| `0x0c` | `{1}` | "confirmkey mode" |
| `0x11` | — | disconnect; then event `0xb6` twice |
| `0x12` | — | refused while connected; stock sends it before entering the loader |
| `0x15` | 3 bytes | class of device ("COD") |
| `0x26` | 170 bytes | the bond table ("nvram"), at start or while running |
| `0x39` | u16 | largest MTU the core accepts in an exchange (stock 200; up to 512 works) |
| `0x42` | `{2}` | not established |
| `0x70` | | "Enable_WakeupNs" |
| `0x76` | — | start of the GATT definitions |
| `0x77` | `10 <uuid128>` | add a primary service |
| `0x78` | `<props> 10 <uuid128> <len> <value>` | add a characteristic; the value is what reads return; a CCC follows a notifying one |
| `0x79` | u16 | not established (stock `00 04`) |
| `0xa0` | long | SDP record ("SetSDP_SERVICE") |
| `0xa1` | | "BT_ResetFeatrues" |
| `0xb0` | long | HID input: the HIDP frame (`a1 <id> <report>`) |
| `0xb1` | address (6), u16 page timeout, link key (16) | page a bonded host ("BT_CreatBtReconnect"; stock timeout `0x12c0`) |
| `0xb2` | | "BT_SendHidControlData" |

Stock's start-up (`0x0101add8`): `0x04` (`"80KA"` + zeros), `0x01` and `0x00`
addresses, `0x0c {1}`, `0x15 {08 25 00}`, `0x26` (the table from
`0x0107f000`), `0x76`, `0x77`, `0x78`, `0x42 {2}`, `0x39 {c8 00}`, `0x79 {00
04}`, then `0x02 {flags}`.

The core's own command switch (ROM `0x4e5c`) takes `00 01 02 03 04 05 09
0b 0c 0d 10 11 12 26 28 29 2a 30 33 34 35 36 37 38 42 48 49 51 61 ff ee`;
the patch adds `11 15 27 0c b0 b2 09 a1 a2 b1 a0 39 76 77 78 70 79`.
`0x34` writes the advertising data, `0x35` the scan response. Unknown
opcodes mostly get no answer; `0x27` and some above `0x29` wedge the core
and stall the CPU.

## Events

| Id | Data | Meaning |
|---|---|---|
| `0x02` | `{0}` | LE connected |
| `0x05` | `{0}` | LE disconnected |
| `0x06` | opcode, status | command acknowledged |
| `0x08` | u16 handle, data | LE host write (CCCs too) |
| `0x09` | — | core started |
| `0x0d` | 170 bytes | bond table changed (stock writes it to `0x0107f000`) |
| `0x14` | `01 00` | before `0xb5` on pairing |
| `0x29` | u16 handle | answer to `0x77`/`0x78` |
| `0xb0` | | HID connected |
| `0xb2` | HIDP frame | host data from either channel: output `a2 <id> <data>`, SET_REPORT feature `53 <id> <data>` (the core answers the handshake) |
| `0xb3` | | "HCI_EVENT_BLE_HID_CONNECT" |
| `0xb4` | | "HCI_EVENT_BLE_ENC_INFO" |
| `0xb5` | 28 bytes | pairing: own address (6), host address (6), link key (16) |
| `0xb6` | | disconnected |
| `0xb7` | 2 bytes | not established (channels, likely) |
| `0xb9` | `{0}` | baseband connected |
| `0xba` / `0xbb` | | Switch wake timeout / fail (stock's names) |
| `0xbf` | | control-channel data |

Others handled by stock without names: `0x03 0x07 0x0e 0x11`.

## Classic

- The core is a complete HID device: it runs SDP, pairing, link keys and
  the HID channels itself. The app gives SDP records (`0xa0`) and sends
  HIDP frames (`0xb0`).
- SDP records must nest sequences with the u8-length form (`0x35`), as
  stock's do; with u16-length nesting (valid SDP) the core stops working
  once a host reads the records.
- GET_REPORT from the host: not established.
- No event asks the app for a link key. Restarting the core while a host is
  connected drops the link without telling the host (it waits for the
  supervision timeout); a `0x11` first ends it cleanly.

### Bond table

170 bytes: five 34-byte records; the core replaces the least recently used.

| Bytes | Field |
|---|---|
| 0 | use order, 0 = most recent (empty records keep 1..4) |
| 1 | `0x33` in every bond seen; meaning not established |
| 2-7 | host address, LSB first |
| 8-17 | zero in every bond seen |
| 18-33 | link key |

A table built by the app (records moved, order rebuilt) is accepted, at
start and at run time; a record removed makes that host's connect fail.

## LE

- Advertising (`0x02` bit 2) sends the name of `0x04` only, from the
  address in `0xc43b0`. The core keeps its advertising state in data RAM:
  `0xc4340` on/off, `0xc4341` data length, `0xc4342..0xc4361` data,
  `0xc4361` scan response length, `0xc4362..0xc4381` its data, `0xc43ae` the
  outgoing PDU buffer (rebuilt per event). Bytes written there by the ARM
  (length and 31 bytes) are kept across `0x02 {4}` and sent on air.
- The core can not scan: its ROM handles received advertising-channel PDUs
  only as SCAN_REQ and CONNECT_IND (`0x5ade`).
- GATT: the core serves GAP `0x1800` (`0x2a00`, `0x2a01`, `0x2a04`) and GATT
  `0x1801` (`0x2a05`, indicate) itself, then the app's services. Reads are
  answered from the value given at creation, with no event; notifications
  and host writes do not change it.
- Attribute table at `0xc4970..`: entries `{u16 handle (BE), 00 02, u16
  type, ...}`; a service 21 bytes, a characteristic 25 bytes with a 2-byte
  value, a CCC 8 more. Data follows at `0xc4b6f`, so about 11
  characteristics fit; past that the next command gets no answer.
- Handles are the table's lookup key: renumbered in RAM, reads, writes and
  notifications follow the new numbers; discovery (Find Information) walks
  the table in its order and does not.
- LE writes up to 244 bytes are taken; 251 gives ATT error `0x0e` and drops
  the link.
- Link-layer features: `01 00 00 00 00 00 00 00` (encryption only). The ROM's
  LL control switch (`0x6044`) handles CONN_UPDATE_IND, CHANNEL_MAP_IND,
  terminate and encryption PDUs, FEATURE_REQ, PAUSE_ENC_REQ, VERSION_IND and
  PING_REQ; LENGTH_REQ, PHY_REQ and CONN_PARAM_REQ get UNKNOWN_RSP. No data
  length extension and no 2M PHY; the patch does not hook that switch.
  Whether the radio could do LE 2M is not established.
- Consequence: a Switch 2 console connects to it and then sends nothing;
  the console proceeds only on a link running 2M PHY with data length
  extension.
- Switch 2 wake: the console wakes on its wake advert (Nintendo
  manufacturer data, `0x81`, the console's LE address) sent from an LE
  address the console has paired, written into `0xc4341` and `0xc43b0`.
- Encryption with an LTK supplied by the app: not established.

## The core's code

Instruction words are 4 bytes, big-endian, word-addressed: patch RAM words
`0x0000..0x07ff`, ROM words `0x4000..0x7fff`. Fields as far as known:
`[31:27]` major, `[26:21]` register, `[20:17]` size in bytes, `[15:0]`
address or immediate.

- `01101` load, `01100` store, `11011` register = imm16, `58 00 iiii`
  accumulator = immediate (`r3f` is a wide accumulator).
- `00100` branch: `[23:21]` 1 jump, 2 call, 3 return; `[20:16]` condition
  (0 always), bit 26 inverts; target the low 15 bits.
- `c0 vv tttt`: if acc == `vv << 1 | bit 15` goto target (switch case).
- `70 HH LL VV`: byte `[0xHHLL]` = VV.

Hooks: the ROM has 420 hook words; each tests bit (id - `0x100`) of the
bitmap at data `0x4000` and, when set, jumps to patch word 0, which
dispatches on the id. The v1.01 blob sets 42 bits: the radio set-up
(`0x8900..0x8906` PLL/VCO sequence, `0x894b`, `0x8955`/`56`/`58`, `0x896d`),
Classic LMP and L2CAP handlers, the host ring path and boot.

Radio registers sit at core address `0x8900..`. ROM `0x496a` writes {offset,
value} pairs from the table at data `0x40c3`'s pointer until offset `0xff`;
the blob's: `4b=ba 55=d0 56=e0 57=88 58=6c 59=10 07=ff`. Data `0x41df` selects
per-mode `0x8955`/`0x8956` values (TX power steps, likely).

SMP senders at ROM `0x5ee2..0x5f88` (Pairing Failed, Encryption
Information with the LTK from `0x44bc`, Master Identification, Identity
Address, Security Request); L2CAP signalling at `0x51f9..0x52ae`.

Yichip's SDK for this core is not public.
