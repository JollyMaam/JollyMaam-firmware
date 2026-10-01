# Bluetooth

Two files:

* `bt.c` – the Bluetooth host, written against upstream BTstack only.  It
  turns a profile's `struct bt_device` into SDP records, a GATT server and
  advertising; keeps the link; sends reports; stores bonds through
  `settings.c`.  Nothing in it is Jieli-specific.
* `btstack_hci_jieli.c` – the BTstack HCI transport and run loop on top of the
  Jieli controller library `btctrler.a`.  Moved here from
  `btstack-jl-port/port/jieli-br23`; the fork keeps only its patches to
  upstream BTstack sources.

```
profiles ──> bt.c ──> BTstack (upstream, patched fork) ──> btstack_hci_jieli.c ──> btctrler.a
                ^                                                   |
controller <────┘ ctl_bt_event()          run loop wake / time <────┘ plat_*
```

## `bt.c` interface

```c
void bt_start(const struct profile *p, u8 slot, int pairing);
int  bt_stop(void);                 /* begin stopping; 1 once the radio is down */
int  bt_linked(void);
void bt_disconnect(void);           /* drop the link, keep the session */
int  bt_reconnect(void);            /* Classic: page the bonded host again */
int  bt_send_input(u8 id, const u8 *data, u16 len);    /* newest-wins slot */
int  bt_send_message(u8 id, const u8 *data, u16 len);  /* queued, ordered */
void bt_forget(u8 slot);            /* delete the slot's bond everywhere */
int  bt_radio_quiet(void);          /* no pairing or link set-up under way */
```

Events go up through one function the controller provides:

```c
void ctl_bt_event(enum bt_event e);   /* BT_LINKED, BT_UNLINKED, BT_STOPPED */
```

and host writes/reads go to the running profile's `write`/`read` (see
[profiles.md](profiles.md)).  Everything runs on the bluetooth thread except
`bt_send_*`, `bt_linked` and `bt_radio_quiet`, which the control thread may
call.

What `bt.c` does for each transport:

| | Classic (switch, xbox, dualsense, dualshock4, mouse) | LE (switch2, xboxble, generic) |
|---|---|---|
| start | SDP HID record from `hid_descriptor`, Device ID record, class of device, page/inquiry scan when pairing | GATT db from `att_db`, advertising from `advertising()` |
| reconnect | the pad pages the bonded host | the pad advertises to the bonded host |
| bond | peer address + link key per slot (`set_bond_classic`) | peer + LTK per slot (`set_bond_le`, from the profile for Switch 2) |
| input | HID interrupt channel | notification on `input_handle` |

## Rules that must survive (measured)

These come from failures seen on hardware; see `AGENTS.md`.

* Classic reconnect is initiated by the pad.  "Change Grip/Order" on the
  console instead starts a new pairing.
* Outgoing HID channels are opened only after authentication: security level
  2 outgoing, level 0 incoming.  Opening before authentication caused reason-5
  failures.
* Input reports, Classic and LE, go through one newest-wins slot with at most
  `profile.#.in_flight` (default 2) packets in the controller.  Nothing is
  queued.
* LE links ask for 251-byte PDUs; at 27 bytes a Switch 2 report took three
  connection events.
* The run loop sleeps on the transport's wake semaphore, not a timer tick: a
  10 ms tick capped Classic reports at 100 per second.
* The controller advertises Read Encryption Key Size but never answers it; the
  transport answers it.
* The SDK detach queue can keep the last received ACL packet and delay a
  disconnect to the supervision timeout; the archive patch
  `patch_detach_drain_rx()` stays.
* Persistent bonds live in the app's settings, not in the controller's
  retention memory.

## `btstack_hci_jieli.c` interface

Used by `bt.c` only:

```c
const hci_transport_t *hci_transport(void);
const btstack_run_loop_t *hci_run_loop(void);
void hci_open(const u8 address[6]);
void hci_close(void);
void hci_wake(void);           /* wake the run loop from any thread or interrupt */
```

It is the only file that includes both BTstack and Jieli headers (the SDK's
`bool` conflicts with C99 `bool`; keep the include order the port uses).

## Porting

On another chip `bt.c` and the profiles stay.  `btstack_hci_jieli.c` is
replaced by that chip's BTstack transport (BTstack ships ports for several
nRF and other controllers), or `bt.c`'s interface is reimplemented on the
chip's own stack.  The `bt_` interface above has no BTstack types in it for
that reason.
