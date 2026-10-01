# Controller

`controller.c` owns what the pad is doing and runs the 1 ms loop that ties
the other modules together.  Its tunable numbers and tables live in
`settings_ux.c` (see [settings.md](settings.md)).

## The loop

`ctl_poll()` runs every millisecond on the control thread:

```
in_read(&raw)                 input.c: debounced buttons, hat, raw axes
cal_step / cal_apply          input_calibration.c: calibrated pad, or the calibration routine
gestures(pad)                 Home chords from ux_gestures[]
timeouts                      ux_timeouts: connect, pair, idle, offline sleep
if report due and linked:
    n = profile->report(pad, now, buf, &id)
    if n: bt_send_input(id, buf, n)
rumble timer                  ctl_rumble() durations
led_poll(now, state, pad)     leds.c
set_poll(now, bt_radio_quiet())   settings.c writes pending records
usb_poll()                    usb.c: vendor commands
```

## States

The controller state is one tagged union; each kind carries only its own
data.

| Kind | Meaning | Left by |
|---|---|---|
| `IDLE` | no session (after boot, or after a stop) | a gesture |
| `CONNECTING` | session started, waiting for the bonded host | `BT_LINKED`; connect timeout → retry or sleep |
| `PAIRING` | session started, discoverable for a new host | `BT_LINKED`; pair timeout → sleep |
| `LINKED` | a host has the link; reports flow | `BT_UNLINKED` → `DISCONNECTED`; a gesture |
| `DISCONNECTED` | session up, link gone; the host may come back | `BT_LINKED`; offline sleep |
| `WAKING` | a connect's wake step (Switch 1) transmitting | wake done → `CONNECTING` |
| `STOPPING` | session going down, then `after`: idle, start slot, wake, sleep, reboot | `BT_STOPPED` or stop timeout |

A connect or pair requested while `STOPPING` towards another slot replaces
that target (newest wins).

## Interface

Called by gestures and by `usb.c` (control thread):

```c
int  ctl_connect(u8 slot);
int  ctl_pair(u8 slot, int fresh_address);
int  ctl_disconnect(void);
void ctl_sleep(void);               /* on USB power: disconnect only */
int  ctl_reboot(void);
const struct ctl_state *ctl_state(void);
u8   ctl_last_slot(void);           /* for Home: kept in RAM, not in flash */
```

Called from other threads (latches, picked up on the next poll):

```c
void ctl_bt_event(enum bt_event e); /* bt.c */
void ctl_rumble(u8 left, u8 right, u16 ms);   /* profiles; applies profile.#.rumble */
```

Called by the platform:

```c
void ctl_start(void);               /* once, after plat_init() */
void ctl_poll(void);                /* every 1 ms */
```

## Gestures

The engine is the one now in `ux.c`: a gesture starts when Home goes down and
ends when it comes up; the first thing held with Home is the chord; a chord
that changes ends the gesture; each row fires at most once per gesture.  The
rows themselves are the table `ux_gestures[]` in `settings_ux.c`.  The early
chord (Home held at power-on) is replayed through the same table.

Home+Start while USB is plugged in (or at a boot that finds USB) enters the
loader; it stays a fixed rule in the controller, not a table row.

## Rumble

`ctl_rumble(left, right, ms)` stores the request; the poll drives
`plat_motors()` and stops after `ms` (capped by `ux_timeouts.rumble_max_ms`).
The controller stops rumble when a session ends or the pad sleeps.
