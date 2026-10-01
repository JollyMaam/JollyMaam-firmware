# Controller

`controller.c` is the control loop.  It owns every decision about what the pad
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
| what the motors and host lights actually do | controller | worked out every poll from what the profile says the host wants, the slot's `rumble` setting, calibration and the state; forced off at session end and sleep |
| LED scene and when it began | controller | the scene follows the connection state |
| calibrating or not | controller | the controller starts and cancels it, and suppresses buttons while it runs |
| IMU mode | controller | chosen from the profile at session start, off at session end |
| gesture tracking (Home down since, chord, rows fired) | gesture engine, [ux.md](ux.md) | changed only by the buttons; the controller can only tell it to ignore what is held |
| calibration routine (phase, samples, result) | `input_calibration.c` | the routine's own progress; the controller only starts or cancels it |
| IMU samples, the bus transfer in progress | `input_imu.c` | |
| profile session state (what the host last asked for: rumble, lights, Switch input mode; sequence numbers; Switch 2 pairing exchange) | each profile | changed only by host writes and reports; reset by `begin()` |
| link state, the newest-wins input slot, the event queue | `bt.c` | the radio's own progress; the controller asks for start, stop, disconnect |
| settings records, what is unsaved | `settings.c` | |
| log ring buffer | `utils.c` | |

## The loop

The control thread sleeps until something happens, does the work for what
happened, and sleeps again.  It does not run on a fixed period.

```c
for (;;) {
    u32 ev = plat_wait(next_deadline);   /* a bit per source; 0 = the deadline */
    ...
}
```

What wakes it:

| Event | Raised by | Loop does |
|---|---|---|
| `EV_BUTTONS` | a timer interrupt that samples the button ports every 250 µs and sees a change | `in_map()`, gestures (`ux_step`), calibration step |
| `EV_STICKS` | the stick UART: a whole frame from the stick microcontroller arrived | map and calibrate the sticks |
| `EV_IMU` | the I2C interrupt: a sensor read finished with a new sample | add it to `pad.imu` |
| `EV_BT` | `bt.c` queued an event | `bt_poll()` until empty |
| `EV_USB` | the mass-storage driver parked a vendor command | `usb_poll()` |
| deadline | the platform timer | whichever of these is due: the next report, the next LED frame (every 10 ms while anything animates), the next state timeout, the settings write |

After each wake the loop works out the earliest deadline from its own state
(the slot's report time, the scene's next frame, the state's timeout) and
passes it to `plat_wait()`.  So reports are sent on time without a 1 ms tick,
nothing runs while nothing changes, and a button press reaches the gesture
engine within 250 µs.

On a report deadline, while `CONNECTED`:

```
read the triggers (ADC), build pad from the latest buttons, sticks, triggers, imu
n = profile->report(&pad, now, buf, &id)     with gestures/calibration hiding applied
if n: bt_send_input(id, buf, n)
```

On every wake, after the work for the event:

```
profile->host(now, &host)                    what the host wants: rumble, player lights, lightbar
plat_motors(motors(&host))                   0 unless connected, not calibrating, rumble allowed
```

and on an LED deadline `plat_leds(led_frame(&leds, &host, now, &pad))`.

Things to measure once it runs:

* how late a wake is.  The control task (`app_core`, priority 3) is below the
  Bluetooth controller's own task (`btctrler`, priority 4) in the task table,
  so a wake can wait for it.
* the interrupt load: about 4 000 button-sample interrupts and, with the
  sensor running, about 10 000 short I2C byte interrupts a second.

A later option, not part of the design yet: send a report as soon as a button
changes instead of at the next report deadline.  The newest-wins slot and
`in_flight` already bound how much that can send.

A host write (`BT_EV_WRITE`) goes to the running profile on this thread:

```c
struct bt_bond bond;
if (profile->write(&ev.write, now, &bond))   /* 1: the write finished a pairing (Switch 2) */
    set_bond_store(slot, &bond);
```

Profiles therefore never run on the bluetooth thread.  The rule for what
comes back from a profile: **states are polled, events are returned.**  What
the host wants (rumble, lights) is a state the profile keeps and the loop
reads after every wake with `host()`; a finished pairing is an event, returned once
by the `write()` that produced it.

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
| `BOOT` | powered on, woken from sleep, or reset; radio off; boot scene.  Works out why it is awake (below) | `IDLE`, `CONNECTING`, `PAIRING` or `REBOOTING` (loader); see Boot |
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

## Boot

`BOOT` is where every start lands: first power, a reset, and waking from
sleep (the pad's sleep is a power-off that Home or a USB plug-in ends, so a
wake is a boot).  The platform reports why the chip started and what was held
at the first instant, before the buttons could be let go:

```c
struct plat_boot {
    enum { BOOT_POWER_ON, BOOT_HOME, BOOT_USB, BOOT_RESET } cause;
    u16 buttons_held;        /* sampled at the earliest moment */
    u8 hat_held;
};
void plat_boot_info(struct plat_boot *out);
```

What `BOOT` does with it:

| Seen | Then |
|---|---|
| Home+Start held, and USB present now or within `uboot_window_ms` | `REBOOTING` into the ROM loader |
| woken by Home, with a slot key held | the gesture table's "at once" rows for that slot: connect it |
| woken by Home alone | the table's tap rows: connect the last slot (if one is known) |
| woken by USB | `IDLE`: charging, ready for tools |
| power-on or reset, nothing held | `IDLE` |

The boot press goes through `ux_boot_chord()`, so the gesture table decides
what it means, and a press still held when the loop starts continues as an
ordinary gesture.  `BOOT` lasts at most `uboot_window_ms`; any request ends it
early.  The last slot survives a reset (retained RAM), and whether it
survives sleep is a hardware question still to measure (if not, Home alone
after sleep does nothing).

## Rumble

Rumble has one source: the host, through the profile.  The profile decodes
the host's writes (Switch output report, Xbox or PlayStation motor report,
Switch 2 vibration packet) and keeps what the host asked for, including a
one-shot's end time, because only the host changes it and only the profile
understands the protocol.  Every poll the loop reads it:

```c
struct profile_host {
    u8 rumble_left, rumble_right;     /* what the host wants now (a one-shot already timed out reads 0) */
    u8 players, players_flash;        /* player lights the host set */
    u8 lightbar_set, lightbar[3];
};
profile->host(now, &host);
```

and decides what the motors actually do: the host's levels while
`CONNECTED`, with the slot's `rumble` setting on, and not calibrating; 0
otherwise.  So the controller owns the outcome and every reason to override
it, and nothing has to be told to stop: when a session ends the loop stops
asking.  Host lights work the same way and go to `led_frame()`.  A USB
hardware check, if ever added, would be one more input to the same decision.

## Interface

```c
void ctl_main(void);     /* the control thread's body; never returns */
```

Nothing else.  Everything else the controller calls.
