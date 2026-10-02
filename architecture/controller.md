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

What wakes it (`plat_wake()` from an interrupt or the bluetooth thread):

| Event | Raised by |
|---|---|
| `EV_BUTTONS` | the 250 µs tick interrupt, when a sampled button port changed (2 ms lockout after a change) |
| `EV_IMU` | the I2C interrupt (or the tick, as a fallback), when a sensor read finished |
| `EV_BT` | `bt.c`, when it queued an event |
| `EV_USB` | the mass-storage driver, when it parked a vendor command |
| `EV_DEADLINE` | the tick, when the deadline passed |

The bits only wake the loop: each pass does everything, in this order, and
each step costs little when there is nothing for it.

```
read_input       plat_input() (buttons, sticks drained from the UART, triggers),
                 in_map(), cal_apply(); then cal_step() or ux_step() -> request
bt_poll()        until empty: link events, bonds, host writes
usb_poll()       a parked vendor command -> request
sample_power     USB present (debounced), battery every second
timeouts         the state's timeout, retry, wake step
send_report      while CONNECTED and the report is due:
                 imu_take(), profile->report(), bt_send_input()
outputs          profile->host(), imu_poll(), plat_motors(); at an LED
                 deadline led_frame() and plat_leds() if the frame changed
set_poll         write dirty records once the radio is quiet
plat_wait(earliest of: LED frame, state deadline, gesture hold, IMU, calibration)
```

So reports are sent on time without a 1 ms tick, nothing runs while nothing
changes, and a button press reaches the gesture engine within 250 µs plus the
lockout.

### Timing stats

The loop measures itself all the time (`struct ctl_stats`, read with
`padtool stats [-w SECONDS] [--reset]`):

* each pass, wake to sleep, and each step's worst and total time;
* the interval between reports sent, and the age of a button change when its
  report is sent (from the tick that saw it);
* `bt.c`'s counters (reports sent, overwritten in the newest-wins slot,
  packets the controller reports as gone) and the platform's (button changes,
  stick frames, stick resyncs).

Times are histograms of 16 doubling buckets from 32 µs.  The counters are
reset with `--reset` (or every period with `-w`).  Measured idle, connected to
nothing: about 56 passes a second at ~0.9 ms each, 5 % of the CPU, most of it
parsing the 5 000 stick frames a second.

A later option, not part of the design yet: send a report as soon as a button
changes instead of at the next report deadline.  The newest-wins slot and
`IN_FLIGHT` already bound how much that can send.

A host write (`BT_EV_WRITE`) goes to the running profile on this thread:

```c
struct bt_bond bond;
if (profile->write(&ev.write, now, &bond)) {  /* 1: the write finished a pairing (Switch 2) */
    merge bond.ltk into the slot's bond; set_bond_store(); bt_update_bond();
}
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

The boot press goes through `ux_boot()`, so the gesture table decides
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
