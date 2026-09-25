# Working on this repository

GameSir Cyclone 2 firmware, reimplemented: the Jieli BR23 Bluetooth controller
library with BlueKitchen BTstack as the host stack.  The goal is a pad that
connects to a real Nintendo Switch 2 **and reconnects to it unaided**.

Everything below was measured on this pad and this console.  Where a claim
comes from reading a binary rather than from a live run, it says so.

## How to work with the user

* **The user operates the hardware.**  The Switch, its menus and the physical
  pad buttons are theirs; building, flashing and console commands are ours.
  Say what you want them to do in plain words and what you expect to see.
* **Do not leave the session dark.**  The shared log (`tools/padbus.py`) and
  the TUI exist so both sides watch the same thing.  Drive through the bus
  rather than opening the serial port directly.
* **Commit when you have reached a logical check point.**
* **Say "the app" or "the SDK", never a vague "the stack".**  See below.
* Prefer concrete evidence over plausibility.  This repo is full of
  previously-believed things that turned out to be wrong (`docs/reconnect.md`
  collects the dead ends so they are not retried).

## Two code bases, and only one of them is ours

| | means | where |
|---|---|---|
| **app** | every line we build | `src/`, `src/bt/` (the BlueKitchen port) |
| **SDK** | prebuilt Jieli archives, no source | `btctrler.a` (controller), other `.a` files |

The port **replaces** the vendor `btstack.a`; it is not linked.  `btctrler.a`
is still linked and is where link keys, sniff and the LMP state machine live.
Its LLVM bitcode can be read: `tools/stack_report.py` and the extraction
recipe in `docs/reconnect.md`.

## The bond problem, as far as it is proven

There are two places a link key can live, and they are different things:

| store | owner | lifetime | who reads it |
|---|---|---|---|
| app link-key store - `keys`, `setkey`, `restore` | app | `BOND_STORE_RAM=1`: RAM only, lost on any reset.  Otherwise syscfg flash | the app, when it answers a `Link Key Request` |
| one connection's key, `conn->core.link_key` at `conn + 0x9A` | SDK (`btctrler.a`) | one connection | the SDK's authentication maths |

There is **no persistent bond store inside `btctrler.a`**: `Write Stored Link
Key` (0x0C11) and `Delete Stored Link Key` (0x0C12) are stubs that log and
return success (read out of the bitcode).  `0x10700`, which older docs call
"the controller's pairing records", is referenced by nothing in the image
except our own `clearkeys`/`bondstore` commands; a dump of it is all zeros.
**There is exactly one persistent store, and it is the app's.**

### What a pairing actually looks like

In **Change Grip/Order the console re-pairs from scratch on every single
connection.**  Captured 2026-09-24 with `hcidump on`:

    IO Capability Request Reply          <- our answer, the console asked
    User Confirmation Request / accept
    Simple Pairing Complete
    LINK KEY NOTIFICATION  <new 16-byte key>
    Encryption Change (enabled)

and `keys` showed the key changing across that one connect
(`d16f0805...` -> `5c01d724...`).  So:

* "pair, disconnect, reconnect" **while in Change Grip/Order always works and
  proves nothing** - it is a fresh pairing every time, not a bond being used;
* a key captured in Change Grip/Order is not necessarily the key the console
  keeps, because the console is willing to just do it again;
* the failure to chase is the one **outside** Change Grip/Order, which ends in
  `link key rejected (reason=5)`.

Both sides negotiate **no bonding** (`auth_req` without the bonding bit), which
is normal for this console and is why `SSP_KEEP_BONDING_FLAG` exists in
`src/btstack/btstack_config.h`.  See
https://github.com/bluekitchen/btstack/issues/422 for the same shape of
problem in bluepad32.

### The other bug hiding behind it: the pad resets

The pad resets on its own, and on `BOND_STORE_RAM=1` a reset erases the bond -
so "it paired, and the next reconnect failed" can be a reset rather than a key
problem.  Reproduced 2026-09-24: `pair 0` -> console connects -> pairing
completes -> console starts its init subcommands (0x02, 0x08, 0x10) -> **the
pad dies** (`boots 5962ee38 -> 5962ee39`, `p33=0x40`).  Repeating it
immediately succeeded, so it is a race, not a fixed path.  It also dies right
after a `reason=5` rejection.

`p33=0x40` is what this chip reports for *any* requested reset, so it does not
by itself distinguish a fault from a deliberate reset - see
`docs/reboot-hunting.md`.

## Driving the pad

One daemon owns the serial port; everything else talks to it over a socket.

    tools/padbus.py serve -p /dev/bwrap/ttyjl0 --ping-ms 500   # once
    tools/pad_tui.py                                          # the live view
    tools/padbus.py request codex
    tools/padbus.py send codex "connect status"
    tools/padbus.py release codex
    tools/padbus.py tail -n 40

Control is exclusive and the holder is shown on the TUI status line; a client
that goes away hands control back.  **After restarting the daemon the user has
to reopen `pad_tui.py`.**  The port number changes when the pad re-enumerates
(`ttyjl0` -> `ttyjl6`); the daemon rescans `/dev/bwrap/ttyjl*` on its own.

The daemon pings with a bare ENQ and expects ACK.  A missed ACK is logged with
wall-clock time and means the application stopped answering - it is the
earliest sign of a reset, ahead of the boot counter.

Console commands worth knowing:

    keys                  dump the app link-key store (slot, peer, type, key)
    setkey AA:.. KEY32    store one link key
    restore N AA:.. K     set slot N's peer + key and page it
    setaddr AA:..         adopt a saved identity
    pair N [fresh]        forget slot N's bond and wait to be paired
    connect N             reconnect a saved slot
    disconnect            drop the link without sleeping
    reboot                reset the chip, come back as this build
    hcidump on|off        trace HCI to the console; survives the profile restart
    peek/poke             read and write controller memory (poke wants
                          contiguous hex: `deadbeef`, never `de ad be ef`)

`tools/bond_capture.py` and `tools/bond_load.py` move a bond on and off the pad
through `/work/cyclone2-bonds/`.  On a RAM-only build that round trip is the
only way a bond survives a reset.

## Build and flash

    make -j32                                    # uses build_config.h
    tools/flash.sh                               # flashes build/firmware.img

`BOND_STORE_RAM` (bond in RAM or syscfg flash) and `PATCH_BTCTLER` (how the
controller archive is patched) live in `build_config.h`.  Edit a value and
run plain `make`: only what it affects is rebuilt.  `make VAR=value` still
overrides a value for one run.

The stack-size warnings the compiler prints are a vendor lint, not a symptom;
the measured worst case is well inside the budget (`docs/stack-budget.md`).

## Where the time has gone

`docs/reconnect.md` is the long record of what was tried and disproved - read
its "dead ends" before retrying an idea.  `docs/live-bond-capture.md` is the
hunt for the key itself.  `docs/reboot-hunting.md` is the reset hunt.
`docs/session-bus.md` documents the padbus protocol.

Useful neighbours on this machine:

* `/shared/github.com-mart1nro-joyscontrol` - working Switch emulation.  Note
  that *reconnect* there is the device dialling the console (`connect(addr)`),
  and pairing only ever happens in Change Grip/Order.
* `/shared/github.com-ndeadly-switch2_controller_research` - Switch 2's own
  controllers are **Bluetooth LE**, not BR/EDR: the pad advertises and the
  console connects, and their pairing is a custom out-of-band exchange whose
  key is simply `A1 XOR B1`.  A different strategy, and much better documented.
* `/shared/github.com-608-NS1-Wakeup-Demo` - waking a sleeping Switch 1 needs
  a raw-RF access code below HCI, which this SDK does not expose.

## Traps

* The console **never** pages a classic pad by itself.  It is passive; the pad
  is what initiates.  Awaiting a console-initiated reconnect is waiting
  forever - except in Change Grip/Order, which re-pairs instead.
* `uxTaskGetStackHighWaterMark()` is unwired and calling it resets the pad.
* The log is a byte stream: strip control bytes before handing a line to
  curses (`tools/padbus.py: printable()`).
* No root, no `/dev/bus/usb` in this container: USB control transfers are not
  available, only the console serial port.
