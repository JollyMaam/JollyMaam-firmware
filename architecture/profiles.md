# Profiles

A profile is what one kind of host expects the pad to be: a Switch Pro
Controller, a Switch 2 Pro Controller, an Xbox controller over Classic or LE, a
DualSense, a DualShock 4, a generic HID gamepad, a mouse.  Each slot is set to
one profile type (`profile.#.type`).

A profile knows **bytes and meanings**: what its reports look like, what the
host's writes mean, what to answer when the host reads.  It does not know
about radios, tasks, timers or storage formats.  That keeps the profiles the
same on any chip, and makes their report bytes testable on the host.

## Files

| File | Profiles | Transport |
|---|---|---|
| `profile_switch.c` | `switch` (Pro Controller), `switch2` | Classic, LE |
| `profile_switch_wake.c` | console wake for `switch` | raw radio |
| `profile_xbox.c` | `xbox`, `xboxble` | Classic, LE |
| `profile_playstation.c` | `dualsense`, `dualshock4` | Classic |
| `profile_hid.c` | `generic`, `mouse` | LE, Classic |

Profiles that share a file share their encoders: both Xbox transports send the
same state bytes; DualSense and DualShock 4 share the button, stick, motion and
touchpad packing.

`mouse` replaces today's `stickmouse` and `gyromouse`: the stick and the gyro
both move the pointer, each at its own speed (`stick_speed`, `gyro_speed`;
0 turns that source off, and with the gyro off the IMU stays off).  The
`desktop` variant still picks the button layout.

## The interface

```c
struct profile_session {          /* filled by the controller for begin() */
    u8 slot;
    u8 pairing;                       /* waiting for a new host */
    u8 address[6];                    /* the pad's address in this slot */
    const struct bt_bond *bond;       /* the bonded host, if any */
    const struct set_slot *settings;  /* read-only: speeds, variant, ... */
};

struct profile {
    const char *name;                 /* "switch", "dualsense", ... */
    u16 report_hz;                    /* default for profile.#.report_hz=auto */
    const struct bt_device *device;   /* what the host sees */
    const struct profile_wake *wake;  /* Switch 1 only; see below */

    enum imu_mode (*begin)(const struct profile_session *s);
    void (*end)(void);
    int  (*report)(const struct pad *pad, u32 now_ms, u8 *out, u8 *id);
    int  (*write)(const struct bt_write *w, u32 now_ms, struct bt_bond *bond);
    void (*host)(u32 now_ms, struct profile_host *out);
    int  (*read)(const struct bt_read *r, u8 *out, u16 max);   /* bluetooth thread */
};

extern const struct profile *const profiles[PROFILE_TYPE_COUNT];
```

Every function pointer may be NULL except `report`.

A profile keeps its own session state in `static` variables: what the host
last asked for (rumble, lights, Switch input mode), PlayStation sequence
numbers, the Switch 2 pairing exchange.  Only the host and the reports change
it, and `begin()` resets it.  Everything the controller decides (slot,
settings, bond) comes in through `profile_session`.  What the host wants of
the pad the controller reads with `host()` (states are polled); a finished
pairing comes back from `write()` (events are returned).  A profile calls
nothing else except `bt_send_message()` and `util_*()`.

### Identity: `device`

`struct bt_device` is plain data that `bt.c` turns into SDP records, GATT
services and advertising.  The profile never calls BTstack.

```c
struct bt_device {
    enum bt_transport transport;      /* BT_CLASSIC or BT_LE */
    const char *name;                 /* local name, e.g. "Pro Controller" */
    u32 class_of_device;              /* Classic */
    u16 vendor_id, product_id, version;   /* Device ID record / PnP ID */
    const u8 *hid_descriptor; u16 hid_descriptor_len;
    const u8 *att_db; u16 att_db_len; /* LE: compiled GATT database */
    u16 input_handle;                 /* LE: characteristic carrying reports */
    void (*advertising)(const struct profile_session *s, u8 *data, u8 *len);  /* LE */
};
```

### Session: `begin`, `end`

* `begin(s)` – a session is starting.  Reset the profile's state and return
  the IMU mode it needs (`IMU_OFF`, `IMU_MOTION`, `IMU_GYRO`); the controller
  sets it.  The mouse returns `IMU_GYRO` only if its gyro speed is not 0.
* `end()` – the session is over.  The controller stops asking `host()`, so
  rumble and host lights stop with it, and it turns the IMU off.

When the host is ready for reports the controller learns it from
`BT_EV_LINKED`; the profile does not need to be told.

### Pad to host: `report`

```c
int report(const struct pad *pad, u32 now_ms, u8 *out, u8 *id);
```

Called by the controller at the slot's report rate while connected.  `pad` is
the calibrated input (buttons, hat, sticks, triggers, latest IMU samples),
with whatever the gestures or calibration hide already removed.  The profile
writes one input report into `out`, sets `*id` (the HID report ID, or 0 for
LE), and returns its length.  0 means "nothing to send this time" (Switch
simple mode and Xbox send an unchanged state only as an occasional
keep-alive).

The controller hands the result to `bt_send_input()`, the newest-wins slot.

### Host to pad: `write`

```c
struct bt_write {
    enum { BT_WRITE_OUTPUT, BT_WRITE_FEATURE, BT_WRITE_ATT } kind;
    u16 id;            /* HID report ID, or ATT handle */
    const u8 *data;
    u16 len;
};
```

Everything the host pushes to the pad: HID output reports on the interrupt
channel (`OUTPUT`), HID SET_REPORT on the control channel (`FEATURE`), writes
to GATT characteristics (`ATT`).  `bt.c` queues them as `BT_EV_WRITE`; the
controller calls `write()` on the control thread.  It returns 1 when the write
finished a pairing and filled `bond` (Switch 2 only), else 0.

| Profile | Host writes | Profile keeps / does |
|---|---|---|
| switch | output report 0x01/0x10: rumble + subcommand | rumble levels; answers the subcommand with `bt_send_message(0x21, ...)` |
| switch2 | ATT command channel | rumble levels; answers with `bt_send_message()` on the response handle; returns the bond when pairing finishes |
| xbox | rumble output report | rumble levels and a one-shot's end time (capped) |
| dualsense / dualshock4 | output report: motors, lightbar, player LEDs | rumble levels, lightbar, player lights |

### What the host wants: `host`

```c
struct profile_host {
    u8 rumble_left, rumble_right;     /* now; a timed-out one-shot reads 0 */
    u8 players, players_flash;
    u8 lightbar_set, lightbar[3];
};
void host(u32 now_ms, struct profile_host *out);
```

Read by the controller every poll.  The profile answers from what the host
last wrote; it does not decide anything.  The controller decides what the
motors and lights actually do with it ([controller.md](controller.md)).

`bt_send_message(id, data, len)` is the way to send the host something that
must not be dropped: a subcommand reply, the Xbox Guide button report, a
Switch 2 response notification.  Messages are queued in order, few and small;
input reports never go this way.

### Host asks the pad: `read`

```c
struct bt_read {
    enum { BT_READ_FEATURE, BT_READ_ATT } kind;
    u16 id;            /* report ID, or ATT handle */
    u16 offset;        /* ATT long reads */
};
```

HID GET_REPORT (the PlayStation consoles read calibration and the pad's
address as feature reports) and GATT reads.  Returns the length written to
`out`, or -1 for "no such report or handle".  The answer has to be given at
once, so `read()` runs on the bluetooth thread, and may only return constant
data or data from `begin()` (the address).  It changes nothing.

## Console wake: `profile_switch_wake.c`

Waking a sleeping Switch 1 is not a Bluetooth link: the pad transmits raw
packets for some seconds, sweeping the whitening states of the console's wake
packets on one channel.  Emission is measured; a console actually waking is
not yet.

It is part of connecting a `switch` slot.  The profile table carries it as an
optional step that runs before the session starts:

```c
struct profile_wake {
    int  (*start)(const struct profile_session *s);  /* 0 started, <0 nothing to wake */
    int  (*step)(u32 now_ms);        /* 1 when finished */
    void (*stop)(void);
};
```

When the controller connects a slot (not pairing) whose profile has `wake`
and a bonded console, the first part of `CONNECTING` is the wake: the
controller calls `step()` every millisecond until it reports finished (or
`stop()` if the connect is abandoned), then calls `bt_start()`.  Whether the
console woke makes no difference to what happens next: the pad pages it
either way.  The wake keeps its own state (whitening state, burst timing);
the controller only knows it is still running.  The radio must be quiet for the wake, which is why it runs
before `bt_start()`, not inside the session.  Channel and duration are
constants in `profile_switch_wake.c` (today's defaults: channel 0, 8 s).
There is no separate wake command or gesture.

This puts the wake in front of every connect of a `switch` slot, so a console
that is already awake answers only after the wake is over (up to the full
duration).  If that is too slow: page first for a moment, and wake only if the
console does not answer.

The file contains the access-code and payload generation (pure, unit-tested
against the known vectors) and the radio register sequence (Jieli-specific; on
another chip this part would be rewritten or the feature left out).  Switch 2
wake is just a flag in its advertisement and needs none of this.
