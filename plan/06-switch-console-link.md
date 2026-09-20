# Real Switch console link: what blocks it

Status: blocked on the console side, not on the Nintendo protocol.

## Symptom

With the console sitting on **Change Grip/Order**, the controller sees a
connection roughly every 0.8 s:

```
HCI_EVENT_CONNECTION_COMPLETE status 0        (ACL is up)
~180 ms later: connections-delete, status 0x05 (authentication failure)
~180 ms later: HCI_EVENT_DISCONNECTION_COMPLETE reason 0x05
```

The console never sends an SDP query, never opens the HID control/interrupt
channels, and never sends an IO capability request, a user confirmation
request or a Nintendo subcommand. In the earlier logs (before the identity
work) the HID channels did open and report 0x3f was transmitted, and the
console still terminated the link after exactly 260 ms without sending
anything. So two separate things have been true at different times:

- the link can come up far enough for HID (`hid connect` twice, 0x3f sent);
- the console then drops it without starting the Nintendo handshake.

## Peer

`c8:48:05:00:54:b1`, OUI `C8:48:05` = **Nintendo Co., Ltd**. The peer is the
console, confirmed by an OUI lookup; nothing on the host PC is connecting (no
bluetoothd, no bonds, no active ACLs, and the host adapter is a different
address).

## Ruled out on the controller side

| Lever | Result |
| --- | --- |
| VM/syscfg erased (`0x40000..0x7dfff`) | no change |
| BTIF erased (`0x7e000`, holds the MAC and RF trim) | app generates a new address |
| key/mac region erased (`0x7f000..0x7ffff`) | no change |
| new advertised address (`13:11:db:58:28:18` → `22:25:…` → `c2:b6:…` → `6b:6a:…` → `ec:58:88:91:d0:1e`) | no change |
| `delete_link_key()` for every slot index (`unkey`) | no change |
| zeroing `0x10700..0x107ff` (`clearkeys`) | no change - `nvscan` shows this region is the controller's live working RAM, not a persistent store, so this was a no-op |
| reports in 0x3f and 0x30, sparse and continuous at 8 ms | the console drops before any report matters |
| pairing mode entered (`bt_wait_phone_connect_control(1)`) | matches the SDK reference app, which also never sends a proactive `USER_CTRL_PAIR` |

The controller therefore has no stored key anywhere reachable, and it still
advertises an identity the console has never seen. The console still answers
with an authentication failure, which only happens when it authenticates
against a record it already holds.

## What is left

1. Delete the controller record on the console (System Settings -> Controllers
   and Sensors -> Disconnect Controllers, X) and power-cycle the console.
   Then open Change Grip/Order again and run `pair 0 fresh`.
2. If that still fails, capture the air interface (Ubertooth/nRF sniffer) to
   see the LMP exchange: who sends the authentication request, and what the
   console does after it fails.

## Evidence from the console screen

After deleting the console's pairings, power-cycling it and opening
Change Grip/Order, the screen stays **empty** while the controller sees
connections at ~1.2/s that fail authentication. The console never shows a
controller tile, never asks for input and never starts an SSP exchange.

An empty Change Grip/Order screen is not what pairing looks like: a controller
in pairing mode appears there. So the console is not running a pairing flow
with this controller at all - it is retrying something silently and the
attempts happen to reach us. That is consistent with a record the console
still holds for this controller, and inconsistent with "the console sees a new
device and pairs".

The comparison that separates the two possibilities is a known-good emulator:
put the ESP32 from github.com-601-NS1-Wakeup-Demo on the same console. If it
pairs, the console is healthy and the fault is in this firmware's BR/EDR
pairing behaviour; if the ESP32 cannot pair either, the console is the problem
and the air interface has to be captured.

To run this side against the console again after such a test:

```sh
tools/flash.sh                      # write and verify the current build
tools/c2log.py -t 90 -o /work/nslog/run.txt --command 'pair 0 fresh'
```

`pair 0 fresh` forgets every remembered device, adopts a new local address and
waits to be paired. `disconnect` takes the radio down completely, which is what
to do before testing another controller against the same console so the two
do not compete.

## Instruments built along the way

- `tools/flash.sh [--erase-persistent]` - backup, write, read-back compare,
  reset, wait for the console; `--erase-persistent` is the factory reset of
  VM/BTIF/key-mac.
- `tools/c2log.py` - timed capture that survives the profile-reset
  re-enumeration.
- console commands: `pair N [fresh]`, `unkey AA:BB:..`, `clearkeys`,
  `bondstore`, `nvscan`, plus peer-address logging on connection events.
- full-flash backups in `/work/cyclone2-flash-backups/`, logs in
  `/work/nslog/` (`log1` console before changes, `log2` 15 connect cycles,
  `log11`+ after the identity work).
