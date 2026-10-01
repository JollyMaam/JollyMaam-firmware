# Bluetooth

Two files:

* `bt.c` (prefix `bt_`) – the Bluetooth host, written against upstream
  BTstack only.  It turns a profile's `struct bt_device` into SDP records, a
  GATT server and advertising; keeps the link; carries reports and host
  writes.  It knows nothing about settings, storage or the controller's
  states.
* `btstack_hci_jieli.c` (prefix `hci_`) – the BTstack HCI transport and run
  loop on top of the Jieli controller library `btctrler.a`.  Moved here from
  `btstack-jl-port/port/jieli-br23`; the fork keeps only its patches to
  upstream BTstack sources.

```
controller ──bt_start/stop/send──> bt.c ──> BTstack (patched fork) ──> btstack_hci_jieli.c ──> btctrler.a
     ^                              |
     └──────── bt_poll(&ev) ────────┘   events, host writes, new bonds (queued, polled every 1 ms)
```

## Interface

All of it is called from the control thread.

```c
struct bt_bond {                       /* what the slot remembers about its host */
    u8 peer[6];                        /* all zero: none */
    u8 link_key[16]; u8 key_type;      /* Classic */
    u8 ltk[16];                        /* LE */
};

void bt_start(const struct profile *p, const u8 address[6],
              const struct bt_bond *bond, int pairing);
void bt_stop(void);                    /* BT_EV_STOPPED once the radio is down */
void bt_disconnect(void);              /* drop the link, keep the session */
void bt_connect(void);                 /* one attempt to reach the bonded host */
int  bt_send_input(u8 id, const u8 *data, u16 len);    /* newest-wins slot */
int  bt_send_message(u8 id, const u8 *data, u16 len);  /* queued, in order */
int  bt_poll(struct bt_event *ev);     /* 1 while there are events */
int  bt_radio_quiet(void);             /* no pairing exchange or link set-up under way */
```

```c
struct bt_event {
    enum {
        BT_EV_LINKED,      /* the host can take reports */
        BT_EV_UNLINKED,    /* .reason: BT_LOST or BT_ENDED */
        BT_EV_FAILED,      /* a connect attempt ended without a link */
        BT_EV_STOPPED,     /* the radio is down; bt_start may be called again */
        BT_EV_BONDED,      /* .bond: a new or changed bond to store for the slot */
        BT_EV_WRITE,       /* .write: data the host sent (profiles.md) */
    } kind;
    ...
};
```

### Bonds

`bt.c` never stores anything.  `bt_start()` gets the slot's bond (or none),
and BTstack's link-key lookups are answered from that copy for the session.
When pairing produces a key, `bt.c` reports `BT_EV_BONDED`; the controller
writes it to settings.  Forgetting a host is the controller clearing the
slot's bond in settings and starting with `pairing` set.  (Switch 2 pairs
inside its own GATT protocol, so its LTK comes from the profile's write
result instead; see [profiles.md](profiles.md).)

### Why a link ended

`BT_EV_UNLINKED` carries the reason from the HCI disconnection (the same codes
for Classic and LE), sorted into two:

| HCI reason | Code | Sorted as |
|---|---|---|
| Connection Timeout | `0x08` | `BT_LOST`: signal |
| LMP/LL Response Timeout | `0x22` | `BT_LOST` |
| Remote User Terminated Connection | `0x13` | `BT_ENDED`: the host meant it |
| Remote Device Terminated, Power Off | `0x15` | `BT_ENDED` |
| Remote Low Resources | `0x14` | `BT_ENDED` |
| the host closed the HID channels (link up or not) | – | `BT_ENDED` |
| anything else | | `BT_ENDED` |

The controller reconnects after `BT_LOST` and stops after `BT_ENDED`.  Which
codes a Switch, a Switch 2, a PlayStation and a PC actually send when they
sleep, when the user removes the pad, and when the signal goes, is to be
checked in the logs; the table is the one place to adjust.

### Threads

BTstack runs on the bluetooth thread.  Across to the control thread there is
exactly: the newest-wins input slot, the ordered message queue (control →
bluetooth), and the event queue (bluetooth → control).  Host writes are copied
into the event queue and handled by the profile on the control thread.

Host reads (HID GET_REPORT, GATT reads) must be answered at once, on the
bluetooth thread, so `bt.c` calls the profile's `read()` there.  For that
reason `read()` may only return constant data: descriptors, calibration
tables, the pad's address.  No profile has needed more.

## State machine

The controller decides what to try; `bt.c` follows the radio through it and
reports.

```
          bt_start(pairing=0)                       bt_start(pairing=1)
OFF ───────────────────────────> CONNECTING        OFF ─────────────> DISCOVERABLE
                                   │  Classic: page the bonded host       │ Classic: page scan + inquiry scan
                                   │  LE: advertise to the bonded host    │ LE: advertise to anyone
                                   v                                      v
                                 LINK UP (ACL, encrypting)  <─────────────┘
                                   │  Classic: open HID control + interrupt (after authentication)
                                   │  LE: wait for notifications on input_handle
                                   v
                                 LINKED ──── BT_EV_LINKED
                                   │  link goes ── BT_EV_UNLINKED(reason) ──> OFF... see below
any state ── bt_stop() ──> STOPPING ── BT_EV_STOPPED ──> OFF
```

* A Classic page that times out (the controller's page timeout, a few
  seconds) reports `BT_EV_FAILED` and leaves the radio connectable; the
  controller calls `bt_connect()` again while its own `connect_ms` runs.
  LE advertising has no attempts: it continues until the link or `bt_stop()`.
* After `BT_EV_UNLINKED` the session stays up in `CONNECTING`; the controller
  either calls `bt_connect()` (reconnect) or `bt_stop()`.

What `bt.c` does for each transport:

| | Classic (switch, xbox, dualsense, dualshock4, mouse) | LE (switch2, xboxble, generic) |
|---|---|---|
| start | SDP HID record from `hid_descriptor`, Device ID record, class of device | GATT db from `att_db`, advertising from `advertising()` |
| connect | page the bonded host | advertise to the bonded host |
| pairing | page scan + inquiry scan | advertise to anyone |
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
