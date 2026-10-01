# Controller

`controller.c` is the 1 ms loop.  It owns every decision about what the pad
is doing, and calls the other modules as step functions: it gives them what
they need for this millisecond and gets back what happened.  No module calls
the controller, and no module changes what the pad is doing on its own.

## Who owns which state

The rule: **if the controller can change it, the controller owns it.**  If
only a module's own logic changes it, the module keeps it.

| State | Owner | Why |
|---|---|---|
| connection state (below), slot, profile, attempts, timers | controller | every transition is the controller's decision |
| last slot used (retained RAM, never flash) | controller | set when the controller starts a session |
| rumble: motor levels, when they stop | controller | set from profile results; stopped by the controller at session end, during calibration, at sleep |
| LED scene and when it began; host lights (player number, lightbar) | controller | the scene follows the connection state; host lights come from profile results and are cleared at session end |
| calibrating or not | controller | the controller starts and cancels it, and suppresses buttons while it runs |
| IMU mode | controller | chosen from the profile at session start, off at session end |
| gesture tracking (Home down since, chord, rows fired) | gesture engine, [ux.md](ux.md) | changed only by the buttons; the controller can only tell it to ignore what is held |
| calibration routine (phase, samples, result) | `input_calibration.c` | the routine's own progress; the controller only starts or cancels it |
| IMU samples, the bus transfer in progress | `input_imu.c` | |
| profile session state (Switch input mode, sequence numbers, Switch 2 pairing exchange) | each profile | changed only by host writes and reports; reset by `begin()` |
| link state, the newest-wins input slot, the event queue | `bt.c` | the radio's own progress; the controller asks for start, stop, disconnect |
| settings records, what is unsaved | `settings.c` | |
| log ring buffer | `utils.c` | |

## The loop

`ctl_poll()`, every millisecond, on the control thread:

```
now = plat_ms()
plat_input(&raw)                       raw buttons, hat, axes
in_map(&raw, &pad)                     input.c: buttons as read, axes mapped
if calibrating:
    r = cal_step(&raw, now)            input_calibration.c runs itself
    if r != CAL_RUNNING: calibrating = 0, store the result if CAL_DONE
else:
    cal_apply(&raw, &pad)
imu_poll(now, &pad)                    input_imu.c: collect a finished read, start the next

handle(ux_step(&pad, now, state))      gestures -> a request; hides Home and chord keys in pad
if calibrating: clear all buttons in pad
handle(usb_poll(&status))              USB vendor commands -> a request
while bt_poll(&ev): handle(ev)         link events, host writes, new bonds
timeouts(now)                          ux_timeouts: connect, pair, idle, offline sleep

if CONNECTED and a report is due:
    n = profile->report(&pad, now, buf, &id)
    if n: bt_send_input(id, buf, n)

plat_motors(rumble levels, or 0 once the rumble time is over)
plat_leds(led_frame(&leds, now, &pad))    leds.c, stateless
set_poll(now, radio_quiet)             settings.c writes what is unsaved
```

A host write (`BT_EV_WRITE`) goes to the running profile on this thread:

```c
struct profile_effects fx = { 0 };
profile->write(&ev.write, &fx);
/* the controller applies fx: rumble, host lights, a new LE bond */
```

So profiles never run on the bluetooth thread, and everything that changes
the pad's outputs passes through the loop.

## Requests

Gestures and USB commands do not act; they return a request, and the loop
decides what it means in the current state.

```c
struct ctl_request {
    enum {
        REQ_NONE, REQ_CONNECT, REQ_PAIR, REQ_DISCONNECT, REQ_SLEEP,
        REQ_CALIBRATE, REQ_REBOOT, REQ_REBOOT_LOADER, REQ_HOME_TAP,
    } kind;
    u8 slot;                   /* SLOT_LAST (0xff): the slot used last */
    u8 generate_new_address;   /* REQ_PAIR */
};
```

`SLOT_LAST` with no slot used since power-up is dropped.  `REQ_HOME_TAP` is a
short Home press the host should see: the controller adds Home to the reports
for `home_tap_ms`.

## States

One tagged union; each kind carries only its own data.

| State | Meaning | Leaves for |
|---|---|---|
| `BOOT` | just powered on or reset; radio off; boot scene; Home+Start with USB enters the loader | `IDLE` after `uboot_window_ms`, or at once on a request (a boot chord) |
| `IDLE` | radio off | `CONNECTING`/`PAIRING` on a request; `SLEEPING` after `offline_sleep_ms` on battery |
| `CONNECTING` | the profile's connect: its wake step if it has one (Switch 1), then paging or advertising to the bonded host | `CONNECTED` on `BT_EV_LINKED`; after `connect_ms`: `STOPPING` → `IDLE` |
| `PAIRING` | discoverable for a new host | `CONNECTED` on `BT_EV_LINKED`; after `pair_ms`: `STOPPING` → `IDLE` |
| `CONNECTED` | a host has the link; reports flow | `BT_EV_UNLINKED`: lost signal → `CONNECTING` (a reconnect, same timeout); host ended it → `STOPPING` → `IDLE` |
| `STOPPING` | radio going down, then `next`: idle, or connect/pair a slot | `next` on `BT_EV_STOPPED`, or after `stop_max_ms` |
| `SLEEPING` | radio going down, unsaved settings written, then power off | off; USB present → `IDLE` |
| `REBOOTING` | radio going down, then reset; `to_loader`: into the ROM loader | reset |

* A request while `STOPPING` replaces its `next` (newest wins).  Requests
  while `SLEEPING` or `REBOOTING` are dropped.
* Every timeout is the controller's.  `bt.c` never gives up on its own; it
  reports what happened, and the controller decides when to stop.
* `REQ_SLEEP` on USB power only disconnects (`STOPPING` → `IDLE`): the pad
  stays awake to charge and for tools.
* `idle_sleep_ms` with no input activity while `CONNECTED` on battery →
  `SLEEPING`.
* Calibration is not a state: it runs alongside any state, keeps the link,
  and sends the host no buttons until it ends.

## Rumble

Rumble has one source: the host, through the profile.  A host write (Switch
output report, Xbox or PlayStation motor report, Switch 2 vibration packet)
is decoded by the profile into `fx.rumble = {left, right, ms}`, and the
controller applies it.  A USB hardware check, if ever added, would be one
more request.

The controller holds the motor state, not the profile, because the controller
is what has to stop it: at session end, while calibrating, when going to
sleep, when a timed rumble runs out.  Kept in the profiles, every one of them
would need a "stop" and the loop would poll them for a value only the host
changes.  The controller also applies the slot's `rumble` setting and caps
one-shot durations at `rumble_max_ms`.

## Interface

```c
void ctl_start(void);    /* once, from the platform, after it is up */
void ctl_poll(void);     /* every 1 ms, from the platform */
```

Nothing else.  Everything else the controller calls.
