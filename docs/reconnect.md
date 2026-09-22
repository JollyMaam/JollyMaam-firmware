# Why reconnecting was flaky, and what fixed it

## Symptom

Pairing works: with the console on **Change Grip/Order**, `pair N` or the
Home+face chord completes a fresh pairing and the controller streams reports.
Reconnecting to the same console afterwards does not:

* hold Home for five seconds to drop the link, or power-cycle the controller;
* press the chord again to reconnect;
* the console looks untouched and the controller never comes back.

The host Linux PC behaves the same way, so this is not console-specific.

## What the console side of a reconnect looks like

Captured from the ESP32 port that does work
(`/work/jl-esp32-logs/controller/l2cap/console-verified-clean-rebuild.log`),
a reconnect is driven by the *controller* paging the console it already knows:

    esp_hidd: paging host for HID connect
    HCI evt 0x03 (CONNECTION_COMPLETE)      status 0
    HCI evt 0x20 (page scan mode change)
    HCI evt 0x17 (LINK_KEY_REQUEST)         <- the host stack must answer
    HCI evt 0x06 (AUTHENTICATION_COMPLETE)  status 0
    HID control + interrupt channels open

So a reconnect needs the controller to consume its stored link key and
authenticate.  Pairing does not: there the console drives SSP because it is the
side initiating the connection.

## The regression: `config_btctler_hci_standard = 1`

Commit `8d363c6` ("Use standard HCI mode for Switch sniffing") replaced the
controller-archive sniff workaround with `config_btctler_hci_standard = 1`.
That fixed the sniff negotiation but broke reconnection.  On this hardware,
`connect 0` against the paired console produced:

    [8.060] ...status_event_handler: 5          (START_CONNECTED)
    [8.100] HCI_EVENT_CONNECTION_COMPLETE       status 0
    [8.100] hci peer c8:48:05:00:54:b1          (the console)
    [30.250] Bluetooth timeout; staying awake for USB console

Nothing else: no link-key request, no authentication, no HID channels.  The
ACL stayed up and `connect status` reported `state=connecting` forever, so
every later chord press started another attempt that hung the same way.  The
link-key exchange is exactly the piece a *reconnect* needs and a *pairing*
does not - with SSP the console creates the key itself - and it is consistent
with standard-HCI mode leaving link-key handling to a host stack that never
answers a link-key request.

With the vendor setting restored (`config_btctler_hci_standard = 0`) the same
`connect 0` reaches authentication instead:

    [8.230] HCI_EVENT_CONNECTION_COMPLETE       status 0
    [8.490] EDR_HID: hid disconnect             (HID state was reached)
    [8.500] HCI_EVENT_DISCONNECTION_COMPLETE    reason 0x05

Reason `0x05` is `AUTHENTICATION_FAILURE`, i.e. the console challenged us and
the key did not match.  That is a *stale bond*, not a broken reconnect path:
the console is still holding a record for this address from an earlier
pairing, while the controller's records were replaced by the many `pair N`,
`pair N fresh` and `delall` cycles used while working on the console link.
The dump of `0x42000` shows half a dozen different keys stored for the same
console address (see "Reading the bond store" below).  A single fresh pairing
clears it.

The sniff fix is not lost by the revert: `make PATCH_BTCTLER=1` was the
configuration that worked before `8d363c6`, and it is the default again.

## Second fix: a bounded retry

The old timeout handler only re-armed its timer while USB was present, so an
attempt that came up as a bare ACL left the controller stuck in `connecting`
with no way out except a reset.  `cyclone2_poll()` now asks the profile for a
fresh attempt (`cyclone2_profile_reconnect()`) when a connect attempt times
out with USB present:

* the Switch profile drops whatever link is left
  (`USER_CTRL_DISCONNECTION_HCI`) and pages the saved peer again after 300 ms;
* the retry is bounded to `CYCLONE2_CONNECT_RETRIES` (2) attempts and then
  stops, so an unreachable console cannot become a retry storm;
* `connect status` reports the attempt number as `attempt=N`.

The BLE profiles return 0 from their reconnect hook: their reconnection is
advertising-driven, with no outgoing page to re-arm.

## Also fixed: the generic profile's dispatch table was shifted

`profiles[]` in `src/profile.c` used positional initializers and the generic
entry was missing its `disconnect` slot, so every field after `start` was one
place to the left (`send` and `handle_event` were both wrong).  The table now
uses designated initializers.

## Reading the bond store

`tools/flash.sh --backup` writes the whole 512 KiB flash.  EDR pairing records
live in the region starting at `0x42000`: for each record a local address, the
peer address (least significant byte first) and a 16-byte link key.  On this
unit:

    $ python3 - <<'PY'
    data = open('flash.bin','rb').read()
    pat = bytes.fromhex('b154000548c8')          # c8:48:05:00:54:b1 reversed
    print([hex(i) for i in range(len(data)) if data.startswith(pat, i)])
    PY
    ['0x42008', '0x42298', '0x42317', '0x42331', '0x4234b', ...]

Several records for one console is the fingerprint of repeated pairing
attempts, and it is how the stale-bond failure above was identified.

## Recovery procedure

A link that the two sides disagree about cannot be repaired by the controller
alone, because the console owns the other half.  Either:

1. On the console: **System Settings -> Controllers and Sensors -> Disconnect
   Controllers**, then power-cycle it.  Then hold the Home+face chord for five
   seconds (`pair 0` over the console works too) and pair again.
2. Or make the controller a device the console has never seen with
   `pair 0 fresh`, which regenerates the local address as well as the keys.

After that, `disconnect` followed by the chord must reconnect without pairing.

## Update: the remaining failure is a key the console rejects

### Root cause found: the flash was packed for 512 KiB on a 1 MiB part

`src/sdkconfig/board_config.h` sets `CONFIG_FLASH_SIZE = 0x100000` and the
chip is 1 MiB (reading at `0x100000` wraps to `0x0`), but the Makefile packed
with `--flash-size 0x80000`.  The packer derives the reserved-region addresses
from that value, so the image *declared* BTIF at `0x7e000` and key/mac at
`0x7f000`, while the controller, the SDK configuration and the factory data
all use the 1 MiB positions `0xfe000` and `0xff000`:

    $ python3 -c "d=open('flash-1mib.bin','rb').read(); ..."
    0xfe000  aa 66 60 00 80 c0 be 43 b7 1f     <- factory BTIF: MAC + RF trim
    0x7e000  f0 66 60 00 00 54 ef 1c b7 5d     <- where the app had been writing

Consequences, all of which match the symptoms:

* the address the controller actually used was the one the app wrote into the
  wrong BTIF (`5d:b7:1c:ef:54:00`), not the identity in the real BTIF, so the
  console's record for this controller could never line up;
* `pair fresh` regenerated and stored an address in a place the controller
  never reads, which is why "be a new device" never actually happened;
* the factory record area above `0x80000` (`0xf6000`+) held pairing records the
  documented `--erase-persistent` recovery (which erases `0x40000..0x7ffff`)
  never touched.

Fix: `FLASH_SIZE := 0x100000` in the Makefile.  `jlnewfw` then reports

    VM    off=0x00040000 size=0x000be000
    BTIF  off=0x000fe000 size=0x00001000
    key_mac off=0x000ff000 size=0x00001000

and after flashing, `connect status` reports the address from the real BTIF
(`1f:b7:43:be:c0:80`) instead of the app-written one.

### What the failure looked like with the wrong layout

With the reconnect path fixed, the failure moved: the controller now reaches
link-key authentication and is turned away there.  The controller's own LMP
trace (`make DEBUG_LOG=1`, see `tools/patch_debug.py`) shows

    [2.050] SDK: same local addr, same phone,same key   <- a stored key is used
    [2.070] [LMP]LMP_TX_AU_RAND                          <- we authenticate
    [2.140] [LMP]LMP_DETACH: 5                           <- reason 0x05

Things measured since, so nobody repeats them:

* **Not record accumulation.**  Erasing the controller's whole store
  (`erase 0x40000 0x4000`, verified all `0xff`) and doing exactly one pairing
  still fails on the first reconnect.  The stack persists that one key
  (`vm_write idx:77`, 42 bytes: local address, peer address, 16-byte key) and
  still gets `reason=5`.
* **Not a missing peer address.**  The console's address is stored and the
  page succeeds; the ACL comes up and the console stays on the link until the
  authentication fails.
* **The console can do this.**  `USER_CTRL_START_CONNEC_VIA_ADDR` on this same
  controller and stack reconnects to the same console successfully in
  `/work/jl-esp32` (its HCI trace shows `LINK_KEY_REQUEST` then
  `AUTHENTICATION_COMPLETE`), so the peer accepts controller-initiated
  authenticated reconnects.
* **CGO masks it.**  Reconnecting from the console's Change Grip/Order screen
  works because that screen re-runs pairing and creates a fresh key.
* **`pair fresh` does not change the address on this unit.**  `CFG_BT_MAC_ADDR`
  is a BTIF item (`src/settings.c`, `btif_table[]`) stored at `0x7e004`;
  the syscfg write the firmware does is not what the controller reads back, so
  the address stays the same across a `pair fresh` and a soft reset.  The
  documented recovery that depends on a *new* identity therefore has to go
  through `tools/flash.sh --erase-persistent` (which erases `0x7e000` too), or
  this write path needs fixing.

So the open question is why the two sides hold different keys after a pairing
whose encryption succeeded, when the controller does write a key record
(`vm_write idx:77`) for that peer.  The answer is inside the prebuilt
`btctrler.a`/`btstack.a` link-key store, which has no source in this tree.

## Console commands added while chasing this

* `peer N aa:bb:cc:dd:ee:ff` - make slot N page that peer and clear the slot's
  stored key; lets a slot be restored or re-pointed without a pairing round.
* `connect status` now prints `attempt=N`.
* `make DEBUG_LOG=1` builds a copy of the SDK's `debug.c` that forwards the
  controller library's `log_print`/`printf` to the USB console, so the LMP/HCI
  trace is readable.  `make DEBUG_LOG=verbose` also sets the verbose tags.

## Dead ends, recorded so they are not retried

## The vendor's own key-miss fallback (the big one)

`btctrler.a` contains a hook the application is meant to call:

```c
void edr_vendor_keep_connect_for_keymiss(u8 enable);   /* sets key_special_bind */
```

and `lmp_rx_not_accept_au_rand()` uses that flag when a peer rejects our
authentication with reason 6 (PIN or key missing):

```c
case 6:
    if (key_special_bind) {
        conn->step = 27; conn->stage = 5;
        lmp_stage_auth_with_simple_pair_by_local(...);   /* re-pair on this link */
    } else {
        lmp_send_event_auth_complete(conn, 6);           /* report failure, drop */
    }
```

We left the flag at 0, so a console that has no usable key for this controller
made us report the failure and drop the link with reason 5 - which is the
reconnect symptom in one line.  `cyclone2_switch_start()` now calls
`edr_vendor_keep_connect_for_keymiss(1)`.

Effect on hardware: the reconnect now runs a fresh SSP on the same link and
reaches `AUTHENTICATION_COMPLETE: 0` and `hid send connect` - far past the
old failure.  It still ends in a detach with reason 5, and the trace shows
why: both ends challenge within 30 ms (`LMP_TX_AU_RAND` ours, `LMP_AU_RAND`
theirs) and our SRES answer is computed about 40 ms *before* the new key is
stored (`vm_write idx:77` lands after `send_sres_now`), so the console is
answered with the stale key and rejects it.  Wiping our store first does not
help: the console then never answers our public key and the link times out.

* **The report flood was real but is not the cause.**  Sending 0x30 every
  5-10 ms filled the controller's HID transmit ring, and in one full pairing
  capture the log held 1207 `hid send fail!!! 30` lines while the console
  visibly retried its setup subcommands (`0x03`, `0x04` and every SPI read
  appeared twice) because the replies were being dropped with them.  Moving to
  a real Pro Controller cadence (16 ms, 60 Hz) cut that to 207 failures and the
  console's setup subcommands are now answered first time, every time - but the
  reconnect still fails identically, so dropped input replies were not what
  stops the bond being kept.
* **`edr_hid_tx_buff_is_ok()` is not safe here.**  Gating report generation on
  it removes the flood (0 failures) but the firmware then resets in a loop as
  soon as a link comes up; `lmp_private_get_tx_remain_buffer()` must not be
  called at that point.  Reverted.
* **The console never re-keys the link.**  No `LMP_TX_COMB_KEY`,
  `LMP_WAIT_COMB_KEY` or encryption-key-refresh message appears in any capture,
  so "the console stored a changed key" is ruled out.

* **The console is willing to re-pair outside CGO.**  With the controller's
  store wiped, a controller-initiated page gets a normal `LMP_au_rand` from the
  console and a full SSP exchange (`LMP_SIMPLE_PAIRING_NUMBER`,
  `HCI_EVENT_USER_CONFIRMATION_REQUEST`, `LMP_DHKEY_CHECK`).  So "the console
  will not bond outside Change Grip/Order" is wrong; it tries, and the
  authentication step is what fails.
* **The failure is in the stack's *local* authentication path.**  From the
  linked image:

        lmp_stage_auth_with_link_key_by_local
        lmp_stage_auth_with_link_key_by_remote
        temp_link_key_flag
        lmp_hci_authentication_requested

  On a console-initiated connection (Change Grip/Order) the console challenges
  first and the stack takes the *remote* path: it logs `no need send au rand`
  and answers with `send_sres_now`, and the link comes up.  On a
  controller-initiated page it takes the *local* path: it sends
  `LMP_TX_AU_RAND` about 20 ms before the console sends its own `LMP_AU_RAND`,
  i.e. both ends authenticate at once.  With a stored key the console's SRES
  answers ours and is rejected; without a key the simultaneous exchange after
  SSP fails the same way.  Both ends then detach with reason 5.
* **Neither a retry nor a key wipe works around it.**  A second page after a
  failed one fails identically, and deleting the stored key just moves the
  failure to the SSP path.

## Experiment in the current build: never claim the authenticator role

The library already has the escape hatch.  In
`lmp_stage_auth_with_link_key_by_local` the `LMP_TX_AU_RAND` stage checks
`conn->role_switch_state` first and, when a role switch is pending, logs
`LMP_TX_AU_RAND_delay` and returns without sending.  On a console-initiated
connection that is exactly what happens in practice: the console's challenge
arrives first, the stack logs `no need send au rand`, and the *remote* path
authenticates the link.

`tools/patch_btctrler.py` now also forces that branch: the guard

    %cmp = icmp eq i8 %3, 1
    br i1 %cmp, label %if.then3, label %if.end7

becomes `%cmp = icmp eq i8 %3, %3`, so the stage always delays and this
firmware never starts an authentication of its own.  Responses to a peer's
challenge (`lmp_stage_auth_with_link_key_by_remote`) are untouched, so the
Change Grip/Order path is unchanged.  Because `role_switch_state` is not
normally set on an outgoing connection, this is a behavioural change to the
vendor library and is easy to back out: restore the guard and rebuild.

Tried on hardware and **reverted**: with the branch forced the stack retries
`LMP_TX_AU_RAND_delay` and never sends, and the console - which does expect the
new master to authenticate - waits about 3.5 s and then detaches with
`LMP_DETACH: 19`.  So the timing collision is a *consequence*, not the cause:
the console wants our challenge.  `tools/patch_btctrler.py` is back to the sniff
patch only, and the build is the earlier `4fde31d6...` image.

* **Retrying a refused HID report blocks the link.**  `user_data_try_send()`
  drops a report when `USER_CTRL_HID_SEND_DATA` returns busy.  That looks like
  a bug and was "fixed" by keeping the report and skipping new samples until it
  went out.  On hardware that is much worse: while the stack is refusing sends,
  the one stuck report blocks every later one, so the console sees no input at
  all (buttons dead) and drops the link.  Continuous input wants lossy-but-fresh
  - drop and take the next sample, as the SDK's code already does.  The
  `hid send fail!!! 30` bursts are a symptom of sending faster than the sniff
  interval, not a cause of the reconnect failure.
* **The uplink cannot explain the reconnect failure.**  The console detaches at
  `LMP_DETACH: 5` about 100 ms after our `LMP_TX_AU_RAND`, i.e. during
  authentication and before any HID traffic exists, so no amount of fixing
  report transmission changes it.
* **`pair fresh` does not survive a power cycle.**  `CFG_BT_MAC_ADDR` is a BTIF
  item: writing it takes effect only while it is unset, so a later `pair fresh`
  changes the address in RAM (survives a soft reset, not a power cycle) while
  ordinary VM settings do persist.  A genuinely new identity needs a factory
  erase of `0x40000..0xfffff`, not `pair fresh`.
* **`jl-esp32` is not a useful oracle.**  It fails the same way (it never gets
  past `connecting`) and its README's verification only ever covered pairing
  from Change Grip/Order, never a reconnect.

## 2026-09-21: the archive patch was breaking *pairing*, and the key never
## reaches the host

### `PATCH_BTCTLER=1` breaks SSP (this was self-inflicted)

Re-emitting `lmp.c.o` from extracted LLVM bitcode to neutralise
`lmp_esco_sniff_check()` also breaks the pairing handshake.  Measured on
hardware, with that archive the console reaches
`HCI_EVENT_USER_CONFIRMATION_REQUEST`, the chip then re-enumerates USB, the
pairing never completes, and the pad falls back to idle.  Rebuilding the exact
same source tree against the untouched vendor archive pairs and keeps a full
HID session up (`state=connected`, reports flowing, `t_sniff=15000 us` --
the console's own 24-slot request accepted).

The sniff workaround the patch existed for is not needed: `src/profile_switch.c`
hands the profile the console's 24-slot parameters and
`btstack_edr_start_after_init()` calls `lmp_set_sniff_establish_by_remote(1)`,
which is the flag `lmp_esco_sniff_check()` tests.  `PATCH_BTCTLER ?= 0` is now
the default and the Makefile explains why.

Also removed: the `delete_link_key()` call that ran from
`HCI_EVENT_USER_CONFIRMATION_REQUEST`.  That handler runs between the SSP
numeric comparison and the DHKey exchange, and each call erases VM records -
a flash write in the middle of an active pairing.  Forgetting a peer belongs
in the pairing *start* path, and that path now deletes every stored record for
the peer rather than the single slot the stack reports.

### The reconnect failure is a missing link key on the host side

With the vendor archive the pad pairs, works from Change Grip/Order, and then
every reconnect - controller-initiated page, or the console reconnecting after
the user leaves CGO - ends in `reason=5` (authentication failure).  What the
captures and flash dumps show:

* The controller's own record area (VM offset `0x22db` onwards) is written on
  every pairing: **four** records with four different 16-byte keys appear for
  one console after a *single* fresh pairing from an erased store.
* The host stack's key area (VM offset `0x00ad..0x02cf`, the 13 x 42-byte slots
  the SDK initialises at boot) is **completely empty** (`0xff`) even after a
  pairing that produced a working HID session.  The host never persists the
  key.
* On a reconnect the controller asks its host for the key and gets
  `HCI_LINK_KEY_REQUEST_NEGATIVE_REPLY PIN_OR_KEY_MISSING`.  With
  `edr_vendor_keep_connect_for_keymiss(1)` set, the library then re-pairs over
  SSP; the console already holds a key, so it drops the link with `reason=5`.
* Writing each of the controller's four recorded keys back into the flash
  store by hand - as the only record, and as every record - does **not** make
  the reconnect succeed, so the console's key is not one of the values the
  controller kept.
* Sniff is not involved: the negotiation reaches `t_sniff=15000 us` and the
  link stays up until the user leaves CGO.

The linked image has `get_link_key`, `put_link_key` and `delete_link_key` in the
host stack, and `put_link_key` is called from the host's `hci_event_handler` -
so the store path exists.  The next thing to find out is why it leaves the
database empty on this build.  The two candidate directions:

1. Compare against one of the SDK's own HID examples on the same chip (the
   `apps/hid/examples/mouse_single` flow) and see whether *its* key database is
   populated after a pairing - that separates "this app is missing a step" from
   "this SDK build cannot store EDR keys".
2. Make the controller use its own store on an outgoing reconnect instead of
   asking the host (`lmp_stage_auth_with_link_key_by_local` is entered through
   `lmp_master_stage_conn`, which sets the authentication stage; the
   host-request path is `lmp_stage_auth_with_link_key_by_remote`).

### Root cause: the host's remote-device database is a no-op

Both questions above are answered by reading the SDK's own libraries.

`put_link_key()` (in `btstack.a(remote_device_db.c.o)`) fills a 38-byte
`struct _remote_database` - local address at `+8`, peer address at `+9`,
16-byte link key at `+10` - and hands it to `put_database()`, which writes it
through the `database_file` abstraction in `btstack.a(database_file.c.o)`.
That layer is driven by two globals:

    @dbf_syscfg_remote_db_addr = internal global i32 0     ; VM index base
    @dbf_bt_rw_file            = internal global ptr null  ; syscfg interface

and they are set by exactly one function:

    void set_bt_vm_interface(u32 vm_index, void *interface);

**Nothing calls it.**  `grep` over the whole SDK finds only its declaration in
`include_lib/btstack/avctp_user.h`.  Both `db_file_read()` and `db_file_write()`
contain

    if (dbf_bt_rw_file == NULL) goto if.end27;   /* silently do nothing */

so with the interface left NULL the entire remote-device database - link keys
*and* their lookups - is a silent no-op.  That is exactly what the flash shows:
the database region the SDK reserves (VM offset `0x00ad`, 13..16 records of 42
bytes, i.e. `0x400ad..0x402cf` for this part) stays `0xff` after a pairing that
produced a working HID session, and a reconnect asks for the key, gets
`PIN_OR_KEY_MISSING`, and then re-pairs over SSP against a console that already
holds a key (`reason=5`).

It also explains the shape of the whole problem: pairing works because the key
only has to survive in RAM (the host keeps a copy in
`last_device_connect_linkkey` for the session), and every reconnect that matters
goes through a chip reset or a power cycle, where that RAM copy is gone.  The
stock GameSir firmware does not use this path at all - it keeps its own
(peer, key) records in its own VM area at `0xf6000` (see the layout notes
earlier), which is why it reconnects.  `jl-esp32`, which shares this SDK, has
the same gap and the same failure, which is why it was a poor oracle.

### The fix to make

Register the database once during Bluetooth init:

    set_bt_vm_interface(<base index>, <syscfg_file interface>);

The `syscfg_file` vtable is not declared in any SDK header; its shape has to be
taken from `database_file.c.o` (the write path calls `open(index, 38)`,
`write(buf, len)`, `seek(offset, whence)` and `getlen()`).  The index is a
syscfg item id used as a base: `put_database()` computes
`base + dbf_entry_info[db].offset + record*38`, so the base has to have a
contiguous range of free VM ids reserved for the remote database (16 devices
plus the two small tables, per `g_remote_device_number`).  `dbf_entry_info` is
`{0,16}, {16,4}, {20,1}`.

Once the database persists, `put_link_key()` stores the key the controller
derives and `get_link_key()` returns it on the next reconnect, which is the
step the whole reconnect path is missing.

### Tried: registering that database (and what it showed)

`src/settings.c` now implements the vtable `database_file.c.o` expects
(`open/read/write/seek/close`, with each 38-byte record mapped to one syscfg
item) and registers it with
`set_bt_vm_interface(CFG_REMOTE_DB_INFO, &remote_db_interface)`, first from
`board_init()` and then again just before `btstack_edr_start_before_init()`
(the stack's own init clears it if it is registered earlier - a first attempt
registered only at board init and the callbacks were never reached).

With tracing in every callback, a full pairing + reconnect cycle produced:

* pairing: ACL up, `encrypted`, `hid connect`, reports - i.e. a good session;
* **not one `remote db open/read/write` line**, neither during the pairing nor
  during the failed reconnect;
* the flash database region stayed empty, and the reconnect again ended in
  `link key rejected (reason=5)`.

So on this path the stack never calls `put_link_key()`/`get_link_key()` at all,
which means the database registration is necessary but not sufficient: the link
key the controller derives is never handed to the host in the first place.  The
controller library only raises that event from its own authentication stages
(`lmp_stage_calc_link_key`, `lmp_stage_auth_with_link_key_by_local/remote`,
the PIN paths - see `lmp_send_event_link_key_notification` callers), and the
console-initiated SSP path we use (`lmp_stage_auth_with_simple_pair_by_remote`
-> stage 12/step 36) does not go through any of them.

### Where to go next

1. Instrument the controller side of the pairing to see whether
   `lmp_send_event_link_key_notification()` (HCI event 0x18) is ever called on
   the console-initiated SSP path, and if it is, why the host does not act on
   it.  That needs either a patch to the vendor archive (which broke SSP when
   done with the full trace set - keep any patch minimal) or a breakpoint-style
   trace from the app side.
2. Alternatively, make the controller authenticate from its *own* record store
   on an outgoing reconnect (`lmp_stage_auth_with_link_key_by_local`, reached
   through `lmp_master_stage_conn` setting stage 1) instead of the
   host-request path (`lmp_stage_auth_with_link_key_by_remote`, which produced
   `HCI_LINK_KEY_REQUEST_NEGATIVE_REPLY PIN_OR_KEY_MISSING`).
3. The stock GameSir 3.52 firmware does exactly (2) in effect: it keeps its own
   `(peer, key)` records in its own VM area at `0xf6000` and never asks the SDK
   database.  Comparing its stored record with what our controller keeps is the
   cheapest way to learn which key the console actually holds.

Unrelated to the key: leaving Change Grip/Order with a freshly paired pad drops
the link (`hid disconnect`, then the reconnect attempts above).  That has been
seen with the vendor archive and a working HID session, so it is worth checking
from the console side once reconnection works.

### Flash tooling bugs found while chasing this

* `tools/flash.sh --erase-persistent` still erased `0x40000..0x7ffff`, the VM
  range of the old 512 KiB layout, so a "fresh identity" never touched the BTIF
  at `0xfe000`.  It now erases `0x40000..0xfffff`.
* `src/settings.c` generated a random address without clearing the group bit:
  one draw produced `01:28:82:29:be:c9`, which is a *group* address.  Both the
  first-boot and the `pair fresh` paths now normalise to a locally administered
  individual address.
* Capturing the console around a reset needs DTR/RTS re-asserted after the
  device re-enumerates; `tools/probe_reconnect.py` now does that (and reopens
  the port on I/O errors), which is what made post-reset traces readable.

## 2026-09-21 (later): the missing notification, found in the vendor bitcode

### The archive can be read, and re-emitting it is lossless

The vendor controller archive is LLVM bitcode, so `lmp.c.o` can be printed as
LLVM IR with the vendor's own compiler and read like source:

    clang -target pi32v2 -x ir -S -emit-llvm lmp.c.o -o lmp.ll

Doing that to a pristine member and to the same member after a bitcode
round trip (`-x ir -c -emit-llvm` and back) leaves the IR identical apart from
the `ModuleID` comment, and the re-emitted object is the same size.  So the
SSP breakage the earlier session measured was caused by *what* the diagnostic
patch did - replacing `lmp_esco_sniff_check` with `ret i1 1` and adding three
`printf` traces - not by re-emitting bitcode as such.  `PATCH_BTCTLER=1` now
builds a one-line patch instead; the old variant is still available as
`PATCH_BTCTLER=diag`.

### The key is computed on the pairing path and then dropped

`lmp_send_event_link_key_notification()` - the only thing that makes the host
see an EDR link key - has five call sites, all in
`lmp_stage_auth_with_link_key_by_local` (through `lmp_stage_calc_link_key`),
`lmp_stage_auth_with_link_key_by_remote`, and the two PIN paths.

`lmp_stage_auth_with_simple_pair_by_remote`, which is what a console-driven
pairing lands in, is not one of them.  It ends its DHKey exchange by deriving
the link key **straight into `conn->core.link_key`** with `f2_function_api()`,
stores `step 36` / `stage 12`, frees the simple-pair context, and returns - the
key exists, in the right place, and nobody is told.  That is the whole reason
the host's database stayed empty after a pairing that produced a working HID
session, and why the database registration in `src/settings.c` never saw a
callback: not a registration problem, a missing event.

### The host stack would store it, and answer with it

Read the same way out of `btstack.a` and the linked image:

* the `HCI_EVENT_LINK_KEY_NOTIFICATION` case of `hci_event_handler` calls
  `put_link_key(addr, packet + 8, 0, index)` and caches the key in
  `last_device_connect_linkkey`, with no bonding gate of any kind;
* `put_link_key()` skips only when the remote-test flag is set, when
  `temp_link_key_flag` is set (nothing in this image calls
  `set_temp_link_key()`, and it initialises to 0), or when
  `CONFIG_BTCTLER_FAST_CONNECT_ENABLE` is non-zero (compiled to a constant 0
  here), so on this build it goes on to `put_database()` ->
  `database_file.c.o` -> the vtable registered by `src/settings.c`;
* the `HCI_EVENT_LINK_KEY_REQUEST` case calls `get_link_key()` and answers with
  `lmp_hci_link_key_request_reply()` or `..._negative_reply()`.

So both ends of the loop exist and only the notification was missing.

### Third-party confirmation

`/shared/btstack-hci-dump` is the HCI dump attached to
https://github.com/bluekitchen/btstack/issues/422 - bluepad32 + btstack + a USB
dongle as the HID *host* pairing with a genuine Switch Pro Controller.  It
shows the controller asking its host for the link key, getting a negative
reply, running SSP with `io_cap` NoInputNoOutput and `auth_req` **"no
bonding"**, and then raising `LINK_KEY_NOTIFICATION` (type 4, unauthenticated
combination key from P-192) anyway, so that the host can store it.  The issue
itself is titled *"Allow to store link keys even if the auth_req is 'not
required'"*: btstack had to be changed to store the key for Nintendo
controllers that declare no bonding but still expect to reconnect.

### The patch

`tools/patch_btctrler.py --mode notify` (now `make PATCH_BTCTLER=1`) adds one
call at the point where `f2_function_api()` has just written the key:

    call void @lmp_send_event_link_key_notification(%conn, i32 0)

The diff against the pristine member is that single line; the linked image
carries six call sites to the notification instead of five, and the call is
inside `lmp_stage_auth_with_simple_pair_by_remote` in the optimised module.
`ret` is 0, as all five existing call sites pass it.

The image was built, verified and flashed on the pad (address
`fa:09:3a:f3:ed:ed`, persistent data left alone), but **the pairing test has
not been run yet**: with the console on Change Grip/Order, pair, then
`disconnect`, then press the chord and see whether the reconnect survives.
With `debug on` the `remote db open/write` lines from `src/settings.c` are the
direct evidence that the notification arrived and the key was persisted - they
have never appeared before.
