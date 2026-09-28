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

## Reconnect from the Home screen: solved (2026-09-25)

A pad paired in Change Grip/Order now reconnects from the Home screen: `pair 0`
in CGO, leave to Home, `disconnect`, `connect 0` -> `Link Key Request` -> our
key -> `Authentication Complete` (success) -> encryption -> HID channels open,
and the pad works.  Measured with a fresh bond of this firmware and with a bond
lifted from the stock 3.52 firmware (`/work/cyclone2-bonds/*STOCK*`).

The reason-5 "link key rejected" of every earlier attempt was **not a key
problem**.  Two bugs, both on the reconnect path:

1. The port asked for the HID Control L2CAP channel the moment the ACL came up,
   before any authentication.  The console answers that by disconnecting with
   reason 5 without ever challenging the pad.  Fix: the outgoing HID channel is
   created at `LEVEL_2` (`src/bt/btstack_jl_core.c`), so L2CAP waits for
   authentication and encryption.  Incoming (pairing) channels stay `LEVEL_0`.
2. `btctrler.a` lists `Read Encryption Key Size` as supported but never answers
   it, which then held that secured channel forever.  Fix: the port's transport
   answers it (`btstack-jl-port`, `hci_transport_jl_br23.c`).

The console *does* keep the bond from a CGO pairing - the key captured from the
pairing's `Link Key Notification` is the one it accepts.  Things that turned
out not to matter (tested): the pad's address being locally administered, and
answering the pairing with General vs No Bonding.  `SSP_KEEP_BONDING_FLAG`
was never enabled and its hci.c patch is gone from `/work/btstack-jl-port`,
which is now upstream plus committed port changes only.  Not yet re-tested
since the fix: whether the `markers-role-guard` patch mode (changed during the
hunt) is needed at all.

There is exactly one persistent bond store, the app's: one bond per profile
slot, `profile.A.bond.*` (`config get --changed`, pasted back to restore; held in RAM only while
`global.autocommit` is off, see `docs/config.md`).  `btctrler.a` keeps a key only
for the life of a connection; `Write/Delete Stored Link Key` are stubs.

In Change Grip/Order the console *pages the pad* and re-pairs from scratch every
time; a pad-initiated connection there is treated like one from Home.

### The other bug hiding behind it: the pad resets

The pad resets on its own, and with autocommit off (`config global.autocommit=false`, formerly `store ram` and before that the `BOND_STORE_RAM=1` build) a
reset erases the bond -
so "it paired, and the next reconnect failed" can be a reset rather than a key
problem.  Reproduced 2026-09-24: `pair 0` -> console connects -> pairing
completes -> console starts its init subcommands (0x02, 0x08, 0x10) -> **the
pad dies** (`boots 5962ee38 -> 5962ee39`, `p33=0x40`).  Repeating it
immediately succeeded, so it is a race, not a fixed path.  It also dies right
after a `reason=5` rejection.  One cause is fixed: a received `LMP_DETACH`
stalled until the supervision timeout (`docs/reboot-hunting.md`, 2026-09-25);
no reset has been seen since, but the race above is not proven gone.

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

For a script, `tools/padbus.py cmd claude LINE` sends one command, waits
for it, prints exactly its output and exits 1 on `ERR` (the daemon runs the
console in its tagged machine mode, `docs/usb-control.md`).

Console commands worth knowing (`help`; every command and namespace also
takes `help`, `-h`, `--help`; Tab completes):

    config PATH[=VALUE]   show or change a setting (config help); a word
                          that is not a verb is a path
    config get [--changed]|reset|commit|pending
    status PATH           what the pad is doing, read-only (status help)
    pair SLOT [--fresh]   forget a slot's bond and wait to be paired
                          (--fresh: the slot's next address)
    connect SLOT          reconnect a saved slot
    disconnect            drop the link without sleeping
    reboot                reset the chip, come back as this build
    debug stat            one key=value line: uptime, boots, slot, state, ...
    debug bringup         reset mailbox and the bring-up marker ring
    debug hcidump on|off  HCI trace to the console; survives the profile restart
    debug peek/poke       read and write controller memory (poke wants
                          contiguous hex: `deadbeef`, never `de ad be ef`)
    debug help            the rest: log, wake, inject, rumble, imu, sensors, ...

Slots are A, B, X, Y - the Home+button chord that selects them; 0..3 work
too.

`tools/hostbt.py` is a Linux Bluetooth host to test the pad against.  The user
runs `sudo python3 tools/hostbt.py serve` on the host (bluetoothctl + btmon);
from here, `tools/hostbt.py run "pair AA:.." --wait 'successful|Failed'`,
`since SEQ [REGEX]` and friends drive it over 127.0.0.1:47807.

`tools/fuzz_links.py` fuzzes pair/connect/disconnect against a Switch left in
Change Grip/Order and dumps the marker ring on every reset
(`docs/testing.md`); `tools/ringdump.py` reads the whole ring on its own.

`tools/bond_capture.py` and `tools/bond_load.py` move a bond on and off the pad
through `/work/cyclone2-bonds/` (`config get --changed` lines; the old `BOND` lines still
load).  With autocommit off that round trip is the only way a bond survives a
reset.

## Build and flash

    make -j32                                    # uses build_config.h
    tools/flash.sh                               # flashes build/firmware.img

How the controller archive is patched (`CYCLONE2_PATCH_BTCTLER`) is set in
`src/build_config.h`, a normal committed header.  Edit it and run `make`.
Whether changes to bonds and settings reach flash is a runtime choice, not a
build option: `config global.autocommit=false` holds them in RAM, `config commit` writes
them, a reboot drops them (`docs/config.md`).  `make config-test` checks the
settings engine on the host.  Moving `CODE_LIMIT` moves the VM and starts the
pad from defaults.

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
* The log is a byte stream: strip control bytes before writing a line to a
  terminal (`tools/padbus.py: printable()`).
* No root, no `/dev/bus/usb` in this container: USB control transfers are not
  available, only the console serial port.
