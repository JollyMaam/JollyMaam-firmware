# Capturing a live pairing, and what a replay does and does not fix

> Written when keeping bonds in RAM was a build option
> (`CYCLONE2_BOND_STORE_RAM` / `BOND_STORE_RAM=1`, "the RAM-only build").  It
> is now the console's `store ram` (`docs/bond-store.md`); the findings below
> are as recorded.

Written after a session against a real Switch 2 with a RAM-only build.  The
point of the exercise was to stop guessing about the "missing bond key": take a
pairing that works, lift the key off the pad, put it back on a later boot, and
see what a reconnect does with it.

## What the firmware can now do

| command | what it is for |
|---|---|
| `keys` | dump the **host's** link-key store (`CFG_REMOTE_DB_00..`), one line per record: slot, peer, key type, 16-byte key |
| `setkey AA:.. KEY32 [type]` | store one link key in that database |
| `restore N AA:.. KEY32 [type]` | point slot N at that peer, store the key, and page it |
| `setaddr AA:..` | give the pad a specific address (the address a bond is keyed to) |
| `hcidump on\|off` | trace every HCI command, event and ACL packet to the USB console |

`hcidump` is off by default but **sticky**: it survives the stack restart that
a profile start performs, so `hcidump on` then `pair N` captures the pairing
itself.  `bondstore`/`clearkeys` are unchanged and still act on the
controller's own retention memory, not on the host store.

`tools/bond_capture.py` drives the whole sequence and writes a `BOND` line;
`tools/bond_load.py` replays one over USB.  Saved keys live in
`/work/cyclone2-bonds/`.

## What a pairing looks like from the host

With the console in Change Grip/Order:

    HCI t4 < 32 09 <addr> 03 00 00     IO Capability Response: io_cap 3, auth_req 0
    HCI t1 > 2b 04 09 <addr> 03 00 00  our reply: io_cap 3, auth_req 0
    HCI t4 < 33 0a <addr> <value>      User Confirmation Request (numeric compare)
    HCI t1 > 2c 04 06 <addr>           we auto-accept it
    HCI t4 < 18 17 <addr> <key:16> 00  LINK KEY NOTIFICATION, type 0

Two things follow from `auth_req 0` on **both** sides: the console asks for no
bonding, and after dropping our own bonding flag we reply the same.  In stock
BlueKitchen that is exactly the case that discards the key, and it is why the
console's own behaviour is worth watching here.

## The key does get stored on this build

Immediately after a pairing, `keys` prints the key from the notification:

    HCI t4 < 1817 b154000548c8 ff0cb5ac1cdfda96f09cf7148e6f8809 00
    key slot=0 peer=c8:48:05:00:54:b1 type=0 key=ff0cb5ac1cdfda96f09cf7148e6f8809

So the `LINK_KEY_NOTIFICATION` reaches `put_link_key()` and lands in the store.
The reason is the key *type*: the console sends type 0 (a combination key), and
the bonding gate in `hci.c` only runs for other types:

    if (conn->link_key_type != COMBINATION_KEY) { ...drop if neither side bonds... }

## Replaying the key does not fix a reconnect, and why

Restoring the captured bond on a later boot (`setaddr` + `restore`) puts the
right key in the host store - `keys` confirms it - and a reconnect still ends
in `link key rejected (reason=5)`.  The HCI trace of that reconnect says why:

    HCI t4 < 03 0b ...            Connection Complete
    HCI t1 > 1b 04 02 30 00       Authentication Requested (host-initiated)
    HCI t4 < 20 07 <addr> 01      Page Scan Repetition Mode Change
    HCI t4 < 38 04 30 00 80 0c    Link Supervision Timeout Change
    HCI t4 < 05 04 00 30 00 05    Disconnection Complete, reason 5

There is no `HCI_EVENT_LINK_KEY_REQUEST` (0x17) and no
`hci_link_key_request_reply` (0x040b) anywhere in the exchange.  The controller
never asks the host for a key.  That request only happens when the *remote*
initiates authentication; when the **host** sends Authentication Requested, the
controller authenticates from its own store, and this one is empty (RAM-only,
and after a reset).  So the host database that `keys`/`setkey` maintain is not
consulted on the outgoing path at all.

The pad then does what `edr_vendor_keep_connect_for_keymiss(1)` asks: a
key-miss rejection falls back to SSP on the same link.  That is why a reconnect
*while the console is still in Change Grip/Order* can succeed - the console is
willing to re-pair - and produces a *new* key:

    HCI t4 < 1817 b154000548c8 32d42887c1e07d169565dab17bf89804 00

which is not the reconnect we want.  Against a console that is not looking for
controllers, that fallback has nothing to pair with and the link just drops.

## Where a fix has to go

The host store is necessary but not sufficient.  The controller needs the key
on the outgoing path, and there are three ways to get it there:

1. **Let the remote authenticate.**  If the pad pages the console and does not
   send Authentication Requested itself, the console drives the authentication
   and the controller raises `HCI_EVENT_LINK_KEY_REQUEST`, which btstack already
   answers from the store.  This is the path the vendor stack used and the one
   the earlier reconnect traces show.
2. **Answer the request directly.**  `lmp_hci_link_key_request_reply(addr, key)`
   is the controller library's entry point for handing it a key; it is the only
   such entry point that is exposed, and it expects a pending request.
3. **Keep the controller's own store populated.**  That is what the stock
   firmware relies on, and it is why clearing it (`clearkeys`) guarantees a
   reconnect cannot authenticate.

## Which direction the connection goes decides whether the store is used

Follow-up runs with the console *out* of Change Grip/Order, which is the real
reconnect case, split cleanly on who pages whom:

**Pad pages the console.**  `HCI_Create_Connection` from the pad, then
`Authentication Requested` from the host, and the host's key database is never
looked at:

    HCI t1 > 05 04 0d <addr> ...    Create Connection (pad -> console)
    HCI t4 < 03 0b ...              Connection Complete
    HCI t1 > 1b 04 02 30 00         Authentication Requested
    HCI t4 < 05 04 00 30 00 05      Disconnection Complete, reason 5

Four attempts with a valid key in the store, four reasons 5, and no
`HCI_EVENT_LINK_KEY_REQUEST` in any of them: the controller authenticates from
its own memory and the host store is irrelevant on this path.

**Console pages the pad.**  Then the controller *does* ask, and the stored key
is really used:

    HCI t4 < 17 06 <addr>                       Link Key Request
    HCI t1 > 0b 04 16 <addr> <key:16>           Link Key Request Reply, from `keys`
    HCI t4 < 05 04 00 30 00 05                  still reason 5

So the round trip through the host does reach the controller - the two captured
keys for the same address (`ff0cb5ac...` and `32d42887...`, from the fresh
pairing and the SSP fallback) were both offered this way, and the console
refused both.  That is the next thing to explain: the console is not holding
either key we saw a `LINK_KEY_NOTIFICATION` for, so either the console's record
for that address was replaced, or the key the pad reports and the key the
console keeps are not the same secret.

A reproducible loop for that question, with the console out of Change Grip/Order:

    tools/bond_load.py -p /dev/bwrap/ttyjlN --trace <candidate>.bond
    # then read the log for: t4 < 17 / t1 > 0b 04 / encrypted peer / reason 5

`setaddr` and `restore` do not themselves reset the pad (verified: uptime
advances across both), so a candidate can be swapped in without a reflash.

### Both captured keys were refused

Repeating the console-pages-pad test with the other key for the same address
(`ff0cb5ac...`, the one the *fresh* CGO pairing produced, rather than
`32d42887...`, the SSP fallback) gives the same exchange and the same answer:

    HCI t4 < 17 06 b154000548c8                 Link Key Request
    HCI t1 > 0b 04 16 b154000548c8 ff0cb5ac...  our reply
    HCI t4 < 05 04 00 30 00 05                  reason 5

So the console is not holding either key we watched it and the controller agree
on.  The pad's side of the exchange is demonstrably correct - it asks, it
answers with exactly the bytes from the `LINK_KEY_NOTIFICATION`, and it is
rejected - which moves the question to *when the console commits a pairing*.

The two pairings we captured were both made while the console was in Change
Grip/Order, and this repo already records that leaving that screen with a
freshly paired pad drops the link.  The obvious next experiment, with the
console's own state as the only variable:

    tools/bond_capture.py -p /dev/bwrap/ttyjlN -o /work/nslog/cgo-exit.txt \
        --slot 0 --fresh --reconnect --leave-cgo-wait 60

Pair in Change Grip/Order as before, then press Home when the tool says so, and
compare the `disconnect` / `connect 0` result with the run that never left the
screen.  If a proper exit is what commits the bond, the second run's key is the
one to save, and the fix on our side is to capture *after* that exit rather
than during pairing.

## 2026-09-23 (later): the console re-pairs, and rejects every key it is offered

Running that experiment produced a cleaner picture than expected.

**A reconnect where the console holds no key re-pairs and works.**  After the
pad left Change Grip/Order, `disconnect` + `connect 0` gave:

    HCI t1 > 05 04 0d <addr> ...     Create Connection (pad -> console)
    HCI t4 < 03 0b ...               Connection Complete
    HCI t1 > 2b 04 09 ...            IO Capability Request Reply
    HCI t4 < 33 0a ...               User Confirmation Request
    HCI t1 > 2c 04 06 ...            accepted
    HCI t4 < 36 07 ...               Simple Pairing Complete
    HCI t4 < 18 17 ... <key:16> 00   LINK KEY NOTIFICATION
    HCI t4 < 08 04 00 30 00 01       Encryption Change (enabled)

No `Link Key Request` anywhere, so the console had nothing to authenticate
with and ran a fresh pairing instead.  The session then came up normally.

**The next reconnect does ask, and the key is refused.**  Thirty seconds later,
the same pair of devices, same address, same host store:

    HCI t4 < 17 06 b154000548c8                 Link Key Request
    HCI t1 > 0b 04 16 b154000548c8 173bf2b3...  reply, byte-for-byte the
    HCI t4 < 05 04 00 30 00 05                  Notification's key -> reason 5

The key offered is exactly the one from the notification in the pairing above,
the pad read it back from its store with `keys` beforehand, and the console
still refuses it.  Three more variants were tried and all failed:

| what was offered | how it failed |
|---|---|
| `173bf2b3...`, the pairing above | `Link Key Request`, reply, reason 5 |
| `e222b603...`, the earlier CGO pairing | `Link Key Request`, reply, reason 5 |
| `...f23b17`, the same key byte-reversed | `Link Key Request`, reply, reason 5 |

So the byte order is not it either.  Note that a *fresh* pairing always
succeeds even outside Change Grip/Order - the pad can always get back on, one
pairing later.

## The pad's side of the key exchange checks out

Two things were verified in the vendor library's own bitcode (see
`tools/stack_report.py` for the same extraction trick):

* `f2_function_api()` writes the derived key straight into
  `conn->core.link_key`, and `lmp_send_event_link_key_notification()` reads the
  same field - so the notification carries the key the pairing actually used;
* the controller's HCI Link Key Request Reply handler
  (`lmp_hci_link_key_request_reply`, case 1035 in the parser) copies the 16
  bytes out of the command's parameter block at the right offset (skipping the
  6-byte address) and then computes the SRES with them, so the key the host
  supplies is the key the controller authenticates with.

The second point rules out the most attractive explanation for the rejections:
that the reply command was being mis-parsed.  (The vendor host calls
`lmp_hci_link_key_request_reply()` directly rather than sending the HCI
command; the port goes through the generic HCI path, but both land in the same
handler with the same buffer layout.)

What remains is that the console's record for this controller does not hold the
key the controller derives and reports, even for a pairing that was encrypted
seconds earlier.  Two readings are still open, and the experiments so far
cannot separate them:

* the console replaced or never committed that key, and is challenging with one
  from a pairing whose notification we never captured (several pairings
  happened during the earlier attempts, before a console were being logged);
* the key the controller reports and the key it authenticates with are not the
  same secret, despite the two bitcode findings above.

That needs a host we can inspect rather than a console we cannot.  `hci0` on
this machine is up, so the next step is to repeat the same sequence against it,
where the stored link key and the full HCI trace are both readable.  If the
same rejection appears there, it is a controller-side problem; if the Linux
host accepts the key, the console is the one being unusual and the answer is to
work out what it wants the pad to do differently.

## Two things to know about running this on a RAM-only build

* The pad resets on its own during a live session - `status` shows
  `p33=64 rtc=1` and the boot counter moves.  A reset that lands while a
  console is connected wipes the RAM bond, so a capture has to be running
  *before* the interesting moment or the evidence is gone.  This is the
  reboot-hunting problem (`docs/reboot-hunting.md`), not a bond problem.
* Because of that, read the store with `keys` while the session is still up;
  a `keys count=0` read can just mean the pad reset a moment earlier.
