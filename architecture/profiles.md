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
struct profile {
    const char *name;                 /* "switch", "dualsense", ... */
    u16 report_hz;                    /* default rate for profile.#.report_hz=auto */
    const struct bt_device *device;   /* what the host sees; see below */

    void (*begin)(u8 slot, int pairing);
    void (*linked)(void);
    void (*end)(void);

    int  (*report)(const struct pad *pad, u32 now_ms, u8 *out, u8 *id);
    void (*write)(const struct bt_write *w);
    int  (*read)(const struct bt_read *r, u8 *out, u16 max);
};

extern const struct profile *const profiles[PROFILE_TYPE_COUNT];
```

Every function pointer may be NULL except `report`.

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
    void (*advertising)(u8 slot, int pairing, u8 *data, u8 *len);  /* LE */
};
```

### Session: `begin`, `linked`, `end`

* `begin(slot, pairing)` – a session is starting for this slot.  Reset all
  per-host state (Switch input mode, PlayStation sequence numbers, the Switch 2
  pairing exchange).  `pairing` is true when the slot is waiting for a new
  host.
* `linked()` – the host is connected and can take reports (Classic: the HID
  interrupt channel is open; LE: the host enabled notifications on
  `input_handle`).
* `end()` – the session is over.  Undo anything the host had set; the
  controller also stops rumble and clears host lights.

### Pad to host: `report`

```c
int report(const struct pad *pad, u32 now_ms, u8 *out, u8 *id);
```

Called by the controller at the slot's report rate while linked.  `pad` is the
calibrated input (buttons, hat, sticks, triggers, latest IMU samples).  The
profile writes one input report into `out`, sets `*id` (the HID report ID, or
0 for LE), and returns its length.  Returning 0 means "nothing to send this
time" (Switch simple mode and Xbox send an unchanged state only as an
occasional keep-alive).

The controller hands the result to `bt_send_input()`, the newest-wins slot.
The profile does not send input itself.

### Host to pad: `write`

```c
struct bt_write {
    enum { BT_WRITE_OUTPUT, BT_WRITE_FEATURE, BT_WRITE_ATT } kind;
    u16 id;            /* HID report ID, or ATT handle */
    const u8 *data;
    u16 len;
};
```

Everything the host pushes to the pad arrives here: HID output reports on the
interrupt channel (`OUTPUT`), HID SET_REPORT on the control channel
(`FEATURE`), and writes to GATT characteristics (`ATT`).  Typical handling:

| Profile | Host writes | Profile does |
|---|---|---|
| switch | output report 0x01/0x10: rumble + subcommand | `ctl_rumble()`; answer the subcommand with `bt_send_message(0x21, ...)` |
| switch2 | ATT command channel writes | answer with `bt_send_message()` on the response handle; on pairing, `set_bond_le()` |
| xbox | rumble output report | `ctl_rumble()` |
| dualsense / dualshock4 | output report: motors, lightbar, player LEDs | `ctl_rumble()`, `led_host_lightbar()`, `led_host_players()` |

Runs on the bluetooth thread: see the thread rules in [README.md](README.md).

`bt_send_message(id, data, len)` is the second way to send to the host: an
answer or an event that must not be dropped (a subcommand reply, the Xbox Guide
button report, a Switch 2 response notification).  Messages are queued in
order, few and small; input reports never go this way.

### Host asks the pad: `read`

```c
struct bt_read {
    enum { BT_READ_FEATURE, BT_READ_ATT } kind;
    u16 id;            /* report ID, or ATT handle */
    u16 offset;        /* ATT long reads */
};
```

HID GET_REPORT (the PlayStation consoles read calibration and the MAC address
as feature reports) and GATT reads.  Returns the length written to `out`, or
-1 for "no such report/handle".

### What profiles may call

Only these, all safe from the bluetooth thread:

| Call | For |
|---|---|
| `bt_send_message(id, data, len)` | replies and events that must arrive |
| `ctl_rumble(left, right, ms)` | motors; `ms` 0 = until changed; the slot's `rumble` setting is applied there, not in the profile |
| `led_host_players(mask, flash)`, `led_host_lightbar(r, g, b)` | lights the host sets |
| `set_bond_le(slot, peer, ltk)` | Switch 2 pairing result |
| `set_slot(slot)` | read-only slot settings (`stick_speed`, `gyro_speed`, `variant`) |
| `util_*()` | AES, byte packing |
| `plat_mac_address()` | the pad's address, which some hosts expect in a report |

Classic bonds (peer address, link key) are handled by `bt.c` itself.

## Console wake: `profile_switch_wake.c`

Waking a sleeping Switch 1 is not a Bluetooth link: the pad transmits
raw packets for some seconds and then starts a normal `switch` session.  It sweeps the whitening states of the console's
wake packets on one channel for up to 30 s.  Emission is measured; a
console actually waking is not yet.  It is its own module with its own small interface,
used only by the controller:

```c
int  swake_check(u8 slot, u8 channel, u8 seconds);   /* 0 ok, <0 why not */
int  swake_start(u8 slot, u8 channel, u8 seconds);
int  swake_step(u32 now_ms);                         /* 1 when finished */
void swake_stop(void);
```

It contains the access-code and payload generation (pure, unit-tested against
the known vectors) and the radio register sequence (Jieli-specific; on another
chip this part would be rewritten or the feature left out).  Switch 2 wake is
just a flag in its advertisement and needs none of this.

**Open: what starts it.**  Today only the console's `wake` command does, and
the console is going.  Options: a Home+slot chord on a `switch` slot always
wakes first and then connects; or a separate gesture row (for example
Home+slot held 1 s); or the USB tool only, for now.
