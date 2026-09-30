# Bluetooth radio and raw wake transmission

The app uses the Jieli BR23 controller library. Evidence below distinguishes
SDK binary observations from radio measurements. Exported functions are SDK
interfaces, not direct proof of over-the-air behavior.

## Established limitations and behavior

- Classic and LE peripheral operation are available. The SDK exposes LE data
  length extension and 2M PHY configuration. A 1M ESP32 central was measured
  exchanging MTU 247, reading the GATT database and receiving notifications.
- One early Switch 2 run stalled after a negotiated 2M PHY transition even
  though the app saw/sent ATT traffic. That historical run does not establish
  that BR23 cannot use 2M; no later capability conclusion follows from it.
- `Read Encryption Key Size` is advertised as supported but did not return a
  response. The app's transport supplies that completion as a workaround.
- SDK `Write/Delete Stored Link Key` handlers are stubs. The controller retains
  a key for a connection; the app's flash VM supplies persistent bonds.
- Read Buffer Size reports 10 ACL buffers of 640 bytes. The host may hand
  over up to ten packets; one waiting in them cannot be replaced by a newer
  report (measured 2026-09-29: unlimited hand-off gave a 63 ms median
  button-to-host latency against a Linux host, two in flight 23.5 ms).
- QoS Setup (guaranteed, 1250 µs latency) completes with the values asked for,
  reported with event code 0x01. A Linux host (master after its role switch)
  still delivered the reports in clumps about 25 ms apart.
- LE Read Buffer Size reports 4 LE buffers of 251 bytes.
- A Switch 2 sets a 5 ms connection interval (4 units), peripheral latency 0
  and a 2 s supervision timeout, then moves the link to the 2M PHY itself.
  Neither side starts a data length update: until LE Set Data Length asks for
  251 bytes the PDUs stay at 27, and the controller sent one per connection
  event, so a 63-byte report went out every third event. The console accepts
  251 bytes / 2120 µs both ways (measured 2026-09-29: reports acknowledged
  14 ms after hand-off on average instead of 47 ms).
- BTstack's own per-connection data length task (`gap_le_set_data_length`)
  never issued the command here; the app sends it directly.
- A default link policy without role switch (Write Default Link Policy 0x0004)
  did not keep the pad master: the Linux host's role switch still completed.
- SDK local-name handling appears limited to 31 bytes: a longer name caused a
  startup reset in a measured 2026-09-27 run. This is an SDK limitation inferred
  from the failure, not a Bluetooth protocol limit.

## Raw BR/EDR frame fields

Recovered from DWARF and register writers in SDK `bredr_frame.c.o`:

| field | offset | writer | meaning |
|---|---:|---|---|
| `bdaddr0/1/2` | 0..6 | `__write_reg_bdaddr` | the 24-bit LAP |
| `bchl/bchm/bchh` | 6..11 | `__write_reg_bch` | the 34-bit BCH parity `P34` |
| `miscntl` | 12 | `__write_reg_miscntl` | `[2:0]` LT_ADDR, `[3]` EDR flag |
| `format` | 13 | `__write_reg_format` | `[6:5]` kept, `[4:0]` frame format, `[7]` train |
| `wincntl` | 16 | direct | whitening control (semantics not established) |
| `txfreq`/`rxfreq` | 21/23 | direct | channel |
| `txptr0/1`, `rxptr0/1` | 24..31 | `__write_reg_txptr`, `__write_reg_rxptr` | packet buffers |
| `linkcntl0/1` | 32/34 | `__write_reg_linkcntl` | link control words |
| `txphdr0/1` | 36/38 | `__write_reg_txphdr` | payload headers |
| `rx_clknl/h` | 52/54 | — | clock the last packet was received at |
| `bitoff` | 56 | — | sub-slot position of that reception, in us |


`bredr_bd_get_frame()` allocates a frame. LAP and BCH parity are separately
writable, enabling an access code that ordinary HCI paging cannot generate.
`__get_access_addr()` uses PN `0x83848d96bbcc54fc` and generator `0x585713da9`;
its 34-bit parity is stored little endian in five bytes.

The transmit buffer window base is held at **`0x1cfc44`**. The SDK's
`__write_reg_txinfo()` resets the chip if the payload is outside the 32 KiB
window above that base. Shutting down the controller cleared the base to zero
in a measured wake attempt. Reopening the controller before raw transmission
fixed that reset; a recorded working base/payload pair was `0xa310 / 0xb3c8`.
These example addresses depend on allocation.

The SDK raw-frame scheduler uses 625 µs slots. Its fixed-transmit packet types
0/1/2 are DH1/DH3/DH5, with 27/183/339-byte payloads; types 3–5 are 2-DH1/3/5
(54/367/679 bytes), types 6–8 are 3-DH1/3/5 (83/552/1021 bytes).
DM1 exists in the radio packet table but has no `link_fix` packet-type index.
Raw transmission owns the scheduler exclusively; it cannot overlap a normal
profile session in the current implementation.

## Wake access-code measurement

The Switch 1 reference uses the console's normal sync word with its lower
32 bits inverted. Parity bytes 0–3 are XORed with `ff`; byte 4 is unchanged.
The phase-1 body repeats a rotated 32-bit wake code in DH3/DH5, every six slots
(3.75 ms). BR23 whitening-word semantics are **not verified**; the app sweeps
32 seeds with 250 ms dwell and pre-cancels the payload using the same seed.

On 2026-09-25, an ESP32 listener on channel 0 saw **22 correlations** with the
wake parity during a 20 s burst and none before/after. The standard-parity
control saw zero. This verifies emission of the wake access code, **not the
payload, whitening or a successful console wake**. The listener's 20-byte CSB
buffer could not capture DH5. The 20 s run completed without resetting.

Attempts against a sleeping Switch 2 did not wake it and produced no observed
DM1 reply. That does not settle Switch 1 behavior. Phase 2 reception and
clock extraction, and phase 3 transmission, are not implemented. A Switch 2
profile instead uses its own LE wake advertisement with wake byte `0x81` and
the paired console address. Classic raw wake must not be substituted for it.

Reference artifacts: SDK `btctrler.a` (`bredr_frame.c.o`, `bredr_link.c.o`,
`lmp_utils.c.o`, `link_task.c.o`),
`/shared/github.com-608-NS1-Wakeup-Demo`,
`/shared/github.com-ndeadly-switch2_controller_research`.
App implementation: `src/profile_switch_wake*.c`, `src/profile_switch2.c`.
