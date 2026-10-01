# Firmware architecture

This folder describes how the application firmware is divided into modules and
how those modules talk to each other.  Hardware facts (pins, radio, power,
flash layout) live in [../docs/](../docs/README.md); this folder is about code.

Status: implemented on branch `rewrite` (the old firmware, kept for reference,
is branch `btstack`).

| Page | Covers |
|---|---|
| [controller.md](controller.md) | the event-driven loop, who owns which state, the states, requests, rumble |
| [ux.md](ux.md) | the gesture table and engine, the timeout table |
| [input.md](input.md) | buttons, sticks, triggers, calibration, IMU and gyro |
| [leds.md](leds.md) | what the lights show and how |
| [profiles.md](profiles.md) | the profile interface, and each console/host profile |
| [bluetooth.md](bluetooth.md) | `bt.c` over BTstack, and the BTstack to controller transport |
| [settings.md](settings.md) | settings, bonds and calibration storage |
| [usb.md](usb.md) | USB: stock-pad upgrades, firmware updates, logs, settings |
| [platform.md](platform.md) | the boundary to the Jieli SDK and board, and porting |

## Layers

The controller is on top and calls everything else.  Nothing calls the
controller: it wakes on events, calls the modules, and gets back what happened
(a request, an event, a result).  The modules below it do not call each
other, with two exceptions: a profile may send a message to the host
(`bt_send_message`), and `bt.c` asks the running profile to answer a host
read.

```
                              controller.c
        the event-driven loop and every decision; owns the pad's state
     │            │             │            │         │          │        │
settings_ux.c  input.c       profile_*.c   bt.c     leds.c   settings.c  usb.c
gestures,      input_         what each    BT host  frame     records    vendor
timeouts,      calibration.c  host expects   │      from      in flash   commands
scenes         input_imu.c        └─msg──>   │      state
                                             │
                                     btstack_hci_jieli.c
─────────────────────────────────────────────────────────────────────────────
utils.c               AES, byte packing, the log ring buffer (anyone)
platform_jieli.c      time, storage, USB, pins, I2C, LED chain, motors, power
platform_jieli_sdk.c  constants and hooks the Jieli libraries require
```

`pad.h` is the only project header.  It has one section per module, in the
order above, and each section lists everything that module offers to others.
If a function is not in `pad.h`, it is `static`.

## Files

| File | Exports | Holds |
|---|---|---|
| `pad.h` | – | every declaration shared between files |
| `controller.c` | `ctl_main` | the loop, the state machine, all state the pad decides |
| `settings_ux.c` | `ux_` | the gesture table and engine, the timeout table, the LED scene table |
| `settings.c` | `set_` | slot defaults, bond and calibration records, slot addresses |
| `input.c` | `in_` | raw input to `struct pad`, activity detection |
| `input_calibration.c` | `cal_` | stick and trigger calibration: maths and routine |
| `input_imu.c` | `imu_` | the motion sensor: polled reads, gyro bias, pointer maths |
| `leds.c` | `led_` | the stateless frame renderer |
| `profile_switch.c` | `profile_switch`, `profile_switch2` | Switch Pro (Classic), Switch 2 Pro (LE) |
| `profile_switch_wake.c` | `profile_switch_wake` | Switch 1 console wake: code generation and the raw radio |
| `profile_xbox.c` | `profile_xbox`, `profile_xbox_ble` | Xbox over Classic and LE |
| `profile_playstation.c` | `profile_dualsense`, `profile_dualshock4` | PlayStation pads |
| `profile_hid.c` | `profile_generic`, `profile_mouse` | generic gamepad, stick and gyro mouse |
| `bt.c` | `bt_` | the Bluetooth host over BTstack, Classic and LE |
| `btstack_hci_jieli.c` | `hci_jieli_` | BTstack's transport and run loop on the Jieli controller library |
| `usb.c` | `usb_` | vendor SCSI commands (identify, status, log, reboot) |
| `utils.c` | `util_` | the log ring, byte packing, AES-128 |
| `platform_jieli.c` | `plat_` | pins, buttons, sticks, I2C, LEDs, motors, USB, storage, power, tasks |
| `platform_jieli_sdk.c` | – | constants and hooks the Jieli libraries require |

## Naming

* A public function, type or constant starts with its module's prefix:
  `bt_send_input()`, `struct led_frame`, `IMU_GYRO`.  The prefix tells you which
  file it is in.
* `static` functions have no prefix.
* No `cyclone2_` prefix: the repository already says which project this is.
* Only `platform_jieli_sdk.c` and `platform_jieli.c` define names the Jieli
  libraries expect (`app_main`, `board_init`, `user_send_cmd_prepare`, ...).
  No other file uses a Jieli name, so a Jieli-looking name always means "the
  SDK calls this".
* One word per idea:
  * **slot** – the user's choice 0..7 (A B X Y up down left right).
  * **profile** – the protocol a slot speaks (`struct profile`).
  * **session** – one start of a profile on the radio, until it is stopped.
  * **link** – the radio connection to a host within a session.
  * **host** – the console or computer on the other end.
  * **report** – bytes from the pad to the host.  **write** – bytes from the
    host to the pad.  **read** – the host asking the pad for bytes.

## Configuration as data

Behaviour that someone might want to tune is a `const` table, not code:

* `settings_ux.c`: the Home gesture table, the timeout table, the LED scene
  table.
* each profile: a `const struct profile` (identity, descriptors, rates).
* `settings.c`: the default value of everything in the records.
* `platform_jieli.c`: the pin table.

## Threads

Two threads run application code.

| Thread | Runs | Priority |
|---|---|---|
| control | `ctl_main()`: sleeps in `plat_wait()`, wakes on events and deadlines, runs every module except `bt.c` | app priority (below the Bluetooth controller's task) |
| bluetooth | the BTstack run loop: `bt.c`, and the profiles' `read()` | below control |

What crosses between them, all inside `bt.c`:

* control → bluetooth: the newest-wins input slot, and the ordered message
  queue (`bt_send_message`).  Nothing else is queued: on a weak link a lost
  input is better than a late one.
* bluetooth → control: the event queue (`bt_poll`): link events, new bonds,
  host writes.

Host reads are the one thing answered on the bluetooth thread, because they
need an answer at once; `read()` may only return constant data.

There is no imu thread: the sensor is read without blocking from the loop
(see [input.md](input.md)).  On a single-threaded system (an nRF run loop)
the same design runs as is.

## Testing

Each module that has logic carries its own test at the end of the file:

```c
#ifdef UNITTEST
#include "test.h"
...
int main(void) { ... }
#endif
```

`make test` builds every `src/*.c` that contains `UNITTEST` with the host
compiler (`cc -DUNITTEST -Isrc`) and runs it.  `test.h` has `CHECK` and
`TEST_DONE`; each test stubs the few `plat_`/`bt_`/`util_` calls its module
makes.  Tested: gestures, LED frames, calibration maths and routine, the IMU
state machine and pointer, each profile's report and write handling, the
Switch wake code generation, USB commands, the log ring and AES.  Not tested
on the host: `controller.c`, `settings.c`, `bt.c`, `btstack_hci_jieli.c`,
`platform_jieli*.c`.
