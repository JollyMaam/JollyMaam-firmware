# Waking a sleeping Switch 1 from this pad

> The console command is now `debug.wake SLOT [CHANNEL] [SECONDS]` / `debug.wake stop`,
> and its state is `status list wake`; the text below keeps the names it was written with.

Status (2026-09-25): phase 1 runs on the pad and **the wake access code has
been verified on the air** with an ESP32 listener (see "Bench results" at the
end).  Not yet tried against a sleeping console.  Earlier status: `src/wake/`
exists and builds; nothing had been flashed or run.  The
parameter maths is checked on the host by `make wake-vectors` against the
reference's own worked examples.  Everything about the BR23 is read out of the
vendor bitcode in `cpu/br23/liba/btctrler.a`; everything about the wake
sequence is read out of `/shared/github.com-608-NS1-Wakeup-Demo`.

This note exists because `AGENTS.md` says "waking a sleeping Switch 1 needs a
raw-RF access code below HCI, which this SDK does not expose".  The first half
is right.  The second half is true of the *host* interface only: `btctrler.a`
exposes the same two register fields the ESP32 reaches through its CSB control
structure, and they are reachable from `src/`, the way `btstack_jl_sdk.c`
already calls `lmp_set_sniff_establish_by_remote()`.

## Why no HCI command can do this

The wake access code is a deliberately invalid BR/EDR access code: the
standard sync word `S` with its lower 32 bits inverted.

    P34_wakeup = (S AND 0x3FFFFFFFF) XOR 0xFFFFFFFF

`S` is a (64,30) BCH codeword of a 30-bit input built from the LAP, and that
mapping is injective over its 30 information bits.  A "forged BD_ADDR" whose
*standard* access code equals `P34_wakeup` therefore does not exist, so no
page, connect or inquiry command can be bent into producing it.  The demo
reaches it because the ESP32 CSB control structure takes the LAP and the
34-bit parity as **separate fields** and lets software write the parity.

## Is a pairing required first?

Not for the three phases themselves — they are unencrypted and
unauthenticated, and no key is involved anywhere in them.  What they need is
the **console's BD_ADDR**, because both the access code and the phase-1 body
are derived from its LAP.  In the demo that is explicit: `wake_cmd_start()`
refuses to run when `swpro_host_snapshot()` has no stored host address, and
the store it reads (`swpro_pairing.c`) holds a bare 6-byte address in NVS and
nothing else — no link key.

Two things still tie this to a pairing in practice:

* the only way to learn a console's address is to have connected to it once;
  a sleeping console does not answer inquiry, and the demo's own comment says
  it does not receive page requests either;
* waking it is only useful if you can then connect, and the console drops an
  authenticated link from a pad it does not recognise — this repo has already
  measured that as `link key rejected (reason=5)`.

So the wake feature inherits the bond problem in `docs/reconnect.md` rather
than sidestepping it.  One consequence is worth testing precisely because it
separates the two: if the console answers phase 1 on the strength of its own
address alone, then a radio that has *never* paired could still wake it.  The
console is asleep and cannot tell which pad sent the code, so this is likely,
but it has not been measured and should be treated as unknown.

## What "CSB" means here

Connectionless Slave Broadcast is the Bluetooth 4.0 (BR/EDR) feature for a
master broadcasting to unlimited slaves with no connection: a fixed access
code, a fixed channel, no ARQ, no encryption, and a schedule.  Its HCI surface
is `Set_CSB`, `Set_Connectionless_Slave_Broadcast_Receive`,
`Write/Receive_Synchronization_Train_Parameters` and friends.

The demo borrows the *transmit* half as a raw frame injector, because it is
the one standard interface that hands the baseband an access code, a channel,
a cadence and a payload under application control.  Its *receive* half is the
mirror: `Set_Connectionless_Slave_Broadcast_Receive` makes the controller
listen on a given access code and channel and hand whatever it hears up as
ACL data (`switch1_csb_rx_start` / `switch1_csb_rx_take`).  The demo uses that
for phase 2, listening for the sleeping console's 17-byte DM1 reply.

That receive half is also the cheapest way to test our side.  A second ESP32
put into CSB receive, listening on the *wake* access code for a chosen
address, will report whatever our BR23 transmits — access code, packet type
and payload — with no console anywhere near it.  Note the CSB interface is
the ESP32's side of the test only; the BR23 has no CSB at all and does not
need one, because it reaches the same registers directly.

## The BR23 baseband has the same two fields

The controller talks to the baseband through one `struct ctrl_frame`.
`bredr_bd_get_frame()` rents one.  The field names below come from the DWARF
in the archive (`bredr_frame.c.o`, source line 28 onwards); the writers are
the `__write_reg_*` helpers in the same object.

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

The LAP and the parity are two independent registers, exactly as on the ESP32.
Writing `bdaddr = LAP` and `bch = P34 XOR 0xFFFFFFFF` makes the hardware emit
the wake access code, preamble and trailer included.

Two independent confirmations that the parity convention matches:

* `__get_access_addr()` in `lmp_utils.c.o` uses `PN = 0x83848D96BBCC54FC` and
  `GENERATOR = 0x585713DA9` — the same constants as the demo's
  `wakeup_code.c` — and stores the result little-endian into 5 bytes.
* Reimplementing the demo's `switch1_sync_word()` from its own description
  reproduces all four `BD_ADDR -> W` rows in `wakeup-protocol.en.md`
  (`0x79F98210`, `0xFAD69689`, `0x0CB13E3F`, `0xAF3BC02D`).

So the wake parity is the standard parity with bytes 0..3 XOR 0xFF and byte 4
left alone.

One trap, recorded because it cost time: the reference's worked-example table
prints the **wake-up** access code in its fourth column, not the standard one.
Its upper 32 bits are unchanged and its lower 32 are already inverted, so it
reads like a normal access code and is not.  `make wake-vectors` now pins that
down: it rebuilds all four rows from `src/wake/` and fails if the port drifts.

## What already exists that can carry the transmission

`link_fix_tx_enable(addr, fre, packet_type, payload)` (`hci_lmp.h`) is a
fixed-frequency transmit that does not need a connection.  It is compiled in:
`lib_btctrler_config.c` sets `CONFIG_TEST_DUT_CODE = 1` and
`CONFIG_TEST_FCC_CODE = 1`.  Through `__fix_txrx_enable(role=2, ...)` it:

1. rents a `ctrl_frame` and zeroes it;
2. copies the peer address and calls `__get_access_addr()` + `__write_reg_bch`
   — i.e. it sets a *standard* access code;
3. sets `format`, `miscntl`, `linkcntl0/1`, `bufcntl`, `txphdr0/1`;
4. allocates a `bredr_tx_bulk` of the packet's maximum length and points the
   frame's `txptr0/1` at it;
5. registers a `link_task` (`run_slot = 32`, i.e. 20 ms) and calls
   `link_task_idle_disable()` / `link_task_add()`.

`packet_table` in `bredr_link.c.o` maps `packet_type` to a packet:

| `packet_type` | BR/EDR type | max payload |
|---:|---|---:|
| 0 | DH1 | 27 |
| 1 | DH3 | 183 |
| 2 | DH5 | 339 |
| 3, 4, 5 | 2-DH1, 2-DH3, 2-DH5 | 54, 367, 679 |
| 6, 7, 8 | 3-DH1, 3-DH3, 3-DH5 | 83, 552, 1021 |

`packet_table` holds `group<<4 | code`, and the code is looked up in
`iut_aclsco_table` (group 0, BR) or `iut_edracl_table` (group 2, EDR).  In the
BR table the codes we care about are `DM1 = 3` (17 bytes), `DH1 = 4`,
`DM3 = 10`, `DH3 = 11`, `DM5 = 14` and `DH5 = 15`.  **DM1 is present in the
table but has no `packet_type` index, so `link_fix` cannot be asked for it.**

`link_fix_tx_enable(addr, ch, 2, ...)` is therefore already a 339-byte DH5
transmitter — the same shape as phase 1.

The scheduler behind it is useful too: `struct link_task` has `run_slot` and
`default_run_slot` in slots of 625 us, plus a `link_task_ops {resume, suspend}`
pair that arms and disarms the frame.  Phase 1 wants 6 slots (3.75 ms), which
is inside its resolution.

## What link_fix cannot do

Three concrete gaps, all in the same function:

* it writes the **standard** parity, so the access code is wrong;
* the payload is `memset` with a single byte (the "single" test mode), so it
  cannot carry the repeated 4-byte wake code or phase 3's BD_ADDR;
* there is no `packet_type` index for DM1, so phases 2 and 3 have no way to
  ask for their packet type, and the cadence is a fixed 32 slots.

None of the three is a dead end, but the workaround is bounded by what the
archive exports: `fix_txrx` (the state object) has internal linkage, so the
app cannot reach its `ctrl_frame` pointer or its payload buffer afterwards.
`frame->txptr0` holds a radio-space address, not a plain pointer.

## Two routes, and the one taken

**Route A — cheapest experiment.**  Enable a `link_fix` transmit and then
patch it from the app: re-derive the frame with `bredr_bd_get_frame()`, rewrite
the parity with `__write_reg_bch()`, locate the payload buffer through
`txptr0`, and move the task's `run_slot`.  It proves the access code reaches
the air, but it fights the scaffold, and `link_fix` cannot be asked for DM1 or
for a one-shot transmit, so it can never carry phases 2 and 3.

**Route B — taken.**  An app-owned path modelled on `__fix_txrx_enable`,
renting its own `ctrl_frame` and registering its own `link_task` with its own
`link_task_ops`.  Every symbol it needs is already exported by the archive.
This is `src/wake/wake_radio.c`.

## What is implemented

| file | what | state |
|---|---|---|
| `src/wake/wakeup_code.c` | sync word, LAP, parity, wake-up code | done; `make wake-vectors` checks it against the reference's four worked examples |
| `src/wake/wake_payload.c` | phase-1 body, phase-3 body, whitening pre-cancel and the 32-state cycle | done, same check |
| `src/wake/wake_radio.c` | rents a frame, writes the wake-up parity, owns a scheduler task, repeats a burst | written for phase 1; **never run** |
| `src/wake/wake.c` | session: target slot, burst sweep, deadline, hand-off | written; never run |
| `src/controller.c` | `wake N` request, the `WAKING` radio state, teardown and reconnect around it | written; never run |
| `src/usb.c` | `wake N [channel] [seconds]`, `wake stop`, `wake status` | written; never run |

Console behaviour:

    wake 0            # wake slot 0's console on the default channel, 5 s
    wake 0 24 10      # channel 24 (2426 MHz), 10 s
    wake stop
    wake status

`wake N` tears the running profile down first, runs phase 1 for the requested
time, then reconnects slot N on its normal path - which is what actually
completes the wake, exactly as the reference's cycle pages after its bursts.
It refuses with a distinct message when the slot has no console address yet
and when the controller library has never been started.

Two things are deliberately *not* implemented yet: phase 2 (listening for the
console's 17-byte reply, which is what yields the clock offset for a direct
ACL initiation) and phase 3.  The reference says phase 3 alone does not wake
the console - the ACL initiation does - and its own cycle pages normally
afterwards, so phase 1 plus the existing reconnect is a coherent first
version.  Phases 2 and 3 are the optimisation to add if that is not reliable.

## State machine, mapped from the demo

| phase | direction | content | cadence | state |
|---|---|---|---|---|
| 1 | TX | DH3/DH5 body = `rotate_right(W, 10)` repeated | every 6 slots (3.75 ms) | implemented |
| 2 | RX | 17-byte DM1 on the same wake access code | expect a reply 1.6-2.4 ms after a burst; bursts ~320 ms apart for ~40 ms | not implemented |
| 3 | TX | 12-byte DM1: pad BD_ADDR little-endian + 6 zero bytes | once, 634 us after the last phase-2 response | body builder exists, transmit not implemented |

After phase 3 the demo drops the CSB link and pages normally; here the
equivalent is to stop the wake task, restore the frame, and let the existing
`connect N` path run.

## Values the app has to compute (all host-testable)

Straight ports of `wakeup_code.c` and `wake_payload.c`:

* `switch1_sync_word`, `switch1_parity34`, `switch1_wakeup_parity34`;
* the phase-1 body: `W = ~S AND 0xFFFFFFFF`, rotate right by 10, repeat;
* the phase-3 body: pad BD_ADDR least-significant byte first, then zeros;
* the whitening pre-cancel: a 7-bit LFSR, seed `CLK[6:1]`, and the demo's
  32-entry seed cycle with its skip counts (DH3/DH5 and DM1 differ).

The whitening step is the demo's own workaround and needs no register: the
payload is XORed before it is handed to the baseband, so what leaves the
antenna is unwhitened.

Where it differs from the reference is the seed.  The reference knows the
sequence the ESP32 will use because it is derived from the piconet clock and
it picks a correction for each burst.  The BR23 has a whitening control word
at `ctrl_frame + 16` (`wincntl`) that the vendor's own transmit path writes
from the clock, and what that word means to the hardware has not been
measured here.  So `wake_radio.c` writes the seed explicitly and `wake.c`
pre-cancels the body with the same value, and rather than commit to one seed
the session walks the reference's 32 states in turn, holding each for 400 ms.
One burst in 32 is then self-consistent whatever the baseband does with the
word.  Capture what actually leaves the antenna and this collapses to a single
state; until then it is a sweep, not a known value.

## What a working connection has to save

The wake-up access code is derived from the **console's** address, so a pad
that can pair with two different consoles needs that address per slot, not one
globally.  Both halves of that are already in place and this work only leans
on them:

| what | where | written when |
|---|---|---|
| the console each slot connects to | `CFG_EDR_ADDRESS_BEGIN + slot`, read and written through `cyclone2_peer_address_read/write` | on every connection, and now also on `BT_STATUS_ENCRY_COMPLETE`, where the link is proven rather than merely up |
| that console's link key | `CFG_REMOTE_DB_00..15`, through the BTstack link-key database | on pairing, by `src/bt/btstack_jl_sdk.c` |

The address is what the wake is aimed at; the key is what makes the reconnect
afterwards succeed.  `wake N` reads slot N's own record, so slot 0 waking a
Switch 1 and slot 1 waking a Switch 2 do not interfere - they do not even
produce the same access code, because the LAP comes from the console.

Entering pairing mode still clears the slot's address (that is the point of
it), and `wake N` then refuses that slot until it has connected once, which is
the right shape: there is nothing to wake until you know what to wake.

## Should we reimplement the controller instead?

No — not to get the wake, and probably not at all.

`btctrler.a` is 108 objects and covers far more than the baseband: LMP and
link control, hopping and AFH, sniff, SCO/eSCO codecs, the whole TWS feature
set, and a BLE link layer with CIS/BIS, extended and periodic advertising and
CTE.  Reimplementing that to reach two register writes is a far larger project
than the thing it unblocks, and it would put the working link at risk — the
link behaviour is exactly what this repo has spent its time getting right.

What makes reading the archive cheap is that it is unstripped LLVM bitcode
with full DWARF, symbol names and vendor source paths, so individual functions
can be printed as IR and read like source.  That is what this note did.  It is
a different order of magnitude from rewriting it.

Two smaller ideas are worth keeping in reserve:

* if the register path turns out to be blocked by some internal state we
  cannot reach, the next step is to *extend* the archive rather than replace
  it — add one exported entry point beside `link_fix_tx_enable` using
  `tools/patch_btctrler.py`, which already re-emits members.  `docs/reconnect.md`
  records that a bitcode round trip is lossless but that changing what the
  code does is where the SSP breakage came from, so treat any edit as a real
  change, not a build trick.
* the only part genuinely worth reverse-engineering in depth is the baseband
  and its scheduler: `bredr_frame.c.o`, `bredr_RF.c.o`, `bredr_link.c.o` and
  `link_task.c.o`.  Reading those four is most of the work and none of it has
  to be rewritten.

For context, `/work/jl-esp32` is a port of this same demo onto a BR23 that
went the other way: it ported the emulator and dropped the wake research,
recording that the CSB and baseband pieces "read and write ESP32 ROM and
shared memory and have no JieLi equivalent".  That verdict is right about the
*ESP32 interface* and wrong about the capability — the equivalent exists, it
is just `struct ctrl_frame` rather than a CSB command.  That gap is the whole
reason this note exists.

## Open questions, and what would settle them

| question | how to settle it |
|---|---|
| does a manual frame really emit the access code we put in `bch`? | write both fields on a live frame and capture the burst with an SDR, or with the ESP32 demo in CSB-receive mode |
| does a sleeping console answer phase 1 at all for a radio it has never paired with? | the ESP32 demo can drive the wake against a console this pad is not bonded to; it needs the console's address, not a key |
| which `format` value is a plain ACL packet? | `__fix_txrx_enable` uses 2 for its transmit role and 3 for its receive role; neither is proven for DM1/DH3 with an arbitrary payload |
| whitening seed for a scheduled slot | compare the demo's LFSR against a capture; `wincntl` may remove the need |
| which channel does a sleeping console listen on? | sweep; the demo treats channel as a runtime parameter and re-tries |
| can the raw task coexist with BTstack? | it takes the frame and calls `link_task_idle_disable()`, so it has to be exclusive — run it from the disconnected state, as the demo does |

## First bench step

The code is written but nothing in it has been on a radio, so the first
session should not involve a console at all.  In order:

1. `make wake-vectors` (host, no pad) - the parameters are right.
2. `connect 0` once, so the controller library is up and the slot has an
   address.  `wake status` should then report a stopped session and no error.
3. `wake 0` with a receive-only listener on channel 0 - an SDR, or the ESP32
   reference driven as a CSB receiver.  What matters is the **access code**:
   the standard one for that console's LAP with the low 32 bits of the sync
   word inverted, and a body of the wake-up code repeated.  If the access
   code is right, everything downstream is a matter of the whitening word and
   the sweep; if it is not, the frame was not set up the way
   `__fix_txrx_enable` sets one up and that is the thing to re-read.
4. Only then put a sleeping console in front of it, and expect the pad's own
   reconnect to do the rest.

The app_core chain measured with `tools/stack_report.py` after these changes
is 1856 bytes of the 2560 the task gets.  That number is not one this change
produced: built with `src/usb.c` reverted, the tool reports the same 680-byte
`execute_command` and a slightly *larger* poll chain, so the new command costs
nothing once its body and its status printer are `noinline`, which is why they
are.  The 528 quoted in `docs/stack-budget.md` for `execute_command` predates
other work on it; that document's table is worth refreshing.

## Reference

* `/shared/github.com-608-NS1-Wakeup-Demo` — protocol, timing, payloads,
  whitening; `docs/wakeup-protocol.en.md` first, then `src/wake/`.
* `/work/jl-esp32` — the same demo's emulator already ported to this chip;
  `src/port/swpro_link.c` lists what it kept and what it dropped, and
  `tools/patch_btstack.py --btctrler` is a working example of re-emitting a
  member of the vendor archive.
* `btctrler.a` — `lmp_utils.c.o` (access code), `bredr_frame.c.o` (register
  block), `bredr_link.c.o` (`link_fix`, packet tables, `link_task`),
  `link_task.c.o` (scheduler), `bredr_dut.c.o` / `bredr_fcc.c.o` (test modes).
  The extraction recipe is in `docs/reconnect.md`.

## Bench results, 2026-09-25

Branch `wakeup-test` (today's `btstack` + this work), pad on the flash bond
store with slot 0's console `c8:48:05:00:54:b1`.

**Two bugs fixed before anything reached the air:**

1. `wake N` reset the pad within a second.  Markers (`0x9Axx`, one per step of
   `wake_radio_start()`) put the death in `__write_reg_txinfo()`: that writer
   calls `cpu_reset()` unless the payload lies within 32 KB above the
   baseband's buffer base, which the hardware holds at `0x1CFC44`.  The base
   read **0**: the teardown before the wake shuts the controller library down
   (`btctrler_task_exit()`), which clears the baseband setup.
2. So `start_pending_wake()` now brings the controller library up on its own
   (`jieli_btctrler_open()`, no BTstack) before `wake_start()`, and
   `cyclone2_wake_finished()` closes it again before the normal reconnect.
   `wake_radio_start()` also checks the window itself and fails with `-4`
   instead of letting the writer reset the chip.  After the fix: base
   `0xa310`, payload `0xb3c8`, and a 20 s wake runs its 80 starts / 79
   whitening states and hands back to the reconnect, with no reset.

**On the air.**  `tools/esp32-wakelisten/` is a controller-only ESP32 app
(ESP32 rev 1.0, ESP-IDF 6.1) that holds a CSB receive open for the console
address with the demo's control-structure rewrites (wake parity, fixed channel
0, LT_ADDR 0, whitening off) and prints every access-code correlation the
hardware stamps into the receive control structure.

| listener parity | before `wake 0 0 20` | during | after |
|---|---:|---:|---:|
| wake (`0x086245c86`) | 0 in 23 s | **22** | 0 |
| standard (`0x079dba379`), control | 0 | **0** | 0 |

So the pad emits exactly the wake-up access code - the console's sync word
with the low 32 bits inverted - on channel 0, and never the standard one.
The correlations are sparse because the listener's receive window only
occasionally lines up with a burst; that is the listener, not the pad.

**Not verified yet:** the payload (the listener cannot store a DH5; its CSB
buffer is 20 bytes), the whitening word, and of course whether a sleeping
Switch 1 answers.  The next step is a sleeping console.  A Switch 2 is woken
differently in the reference - by replaying a BLE advertisement captured from
a real Switch 2 controller (`src/wake/ns2_wake.c` in the demo) - which belongs
with the Switch 2 / BLE work.

## Against a sleeping Switch 2, 2026-09-25

`wake 0 0 20` and `wake 0 0 30` against the user's sleeping Switch 2
(`c8:48:05:00:54:b1`, a console this pad is bonded to as a Classic Pro
Controller): the console did not wake, and the three pages after each wake
timed out (`status=0x04`).  With the ESP32 listener next to the console, the
40 wake-code correlations it caught during a wake all lie on one smooth
bit-offset drift line - one transmitter, the pad - and it recorded no DM1
reception at all, so there is no sign of a phase-2 reply.

Two readings, not separated:

* the implementation is incomplete - the reference says the console wakes
  only when the controller initiates an ACL link from the phase-2 clock, and
  "receiving Phase 3 alone does not complete the wake-up"; this code stops
  after phase 1 and pages normally, which a sleeping console does not answer;
* a Switch 2 may not wake for a Switch 1 controller at all.  User reports on
  the internet say it does not; the reference wakes a Switch 2 differently,
  with an LE advertisement from one of its paired Switch 2 controllers
  (`src/wake/ns2_wake.c`: manufacturer data
  `53 05 01 00 03 7e 05 <pid> 00 01 81 <console addr> 0f`, ADV_IND from the
  controller's own address).

The wake guard that required BTstack to have run once is gone: the wake opens
the controller library itself.  Phase 1 is left in as the best guess for a
Switch 1; a Switch 2 wake belongs with the Switch 2 controller profile.
