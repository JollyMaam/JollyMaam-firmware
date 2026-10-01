# Firmware architecture

This folder describes how the application firmware is divided into modules and
how those modules talk to each other.  Hardware facts (pins, radio, power,
flash layout) live in [../docs/](../docs/README.md); this folder is about code.

Status: **target design**.  The code is being moved onto it in the steps listed
at the end; each module page says what it replaces.

| Page | Covers |
|---|---|
| [controller.md](controller.md) | the 1 ms loop, who owns which state, the states, requests, rumble |
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
controller: it polls the modules each millisecond and gets back what happened
(a request, an event, a result).  The modules below it do not call each
other, with two exceptions: a profile may send a message to the host
(`bt_send_message`), and `bt.c` asks the running profile to answer a host
read.

```
                              controller.c
               the 1 ms loop and every decision; owns the pad's state
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

| File | Prefix | Replaces (today's files) |
|---|---|---|
| `pad.h` | – | every project header |
| `settings_ux.c` | `ux_` | `ux.c` (table and engine), timeouts in `controller.c`/`controller_state.c`, scene table in `lights_render.c` |
| `controller.c` | `ctl_` | `controller.c`, `controller_state.c`, `rumble.c` (timing), the LED state from `lights.c` |
| `profile_switch.c` | `sw_` | `profile_switch.c`, `profile_switch2.c`, `profile_switch2_att_db.h`, the two `.bin` files |
| `profile_switch_wake.c` | `swake_` | `profile_switch_wake*.c/h` (4 + 4 files); the wake step of a `switch` connect |
| `profile_xbox.c` | `xbox_` | `profile_xbox.c`, `profile_xbox_ble.c`, its att_db header and `.gatt` |
| `profile_playstation.c` | `ps_` | `profile_playstation.c`, `profile_dualsense.c`, `profile_dualshock4.c` |
| `profile_hid.c` | `hid_` | `profile_generic.c`, `profile_mouse.c` (stick and gyro mouse become one `mouse` profile) |
| `bt.c` | `bt_` | `bt/btstack_jl_core.c`, `bt/btstack_jl_le.c`, `bt/btstack_jl_sdk.c`, `bt/btstack_jl_ble_stub.c`, `bt/hci_dump_console.c`, `profile.c`, `profile_classic.c`, `profile_le.c`, `identity.c` |
| `btstack_hci_jieli.c` | `hci_` | `btstack-jl-port/port/jieli-br23/*` |
| `input.c` | `in_` | `input.c` |
| `input_calibration.c` | `cal_` | `calibration.c`, `calibration_math.c` |
| `input_imu.c` | `imu_` | `imu.c` (no task), the offset learning from `gyro.c`; pointer maths moves to the mouse profile |
| `leds.c` | `led_` | `lights_render.c` (renderer, now stateless) |
| `settings.c` | `set_` | `config.c`, `config_schema.c` |
| `usb.c` | `usb_` | `usb.c`; `console.c`, `commands.c` and `status.c` are removed (no serial console) |
| `utils.c` | `util_` | `aes128.c`, byte packing helpers repeated in the profiles, the log ring buffer (`util_log`) |
| `platform_jieli.c` | `plat_` | `init.c`, `main.c`, `usb.c`, `led.c`, `rumble.c` (motors), `rom.c`, `timing.c` (clock) |
| `platform_jieli_sdk.c` | – | `sdkconfig/*.c` |

Deleted without replacement: `bounce.c`, `ps.c`, the probe, injection and
statistics parts of `timing.c` and `controller.c`, the bring-up log, the HCI
dump, and the report "air" statistics in the Bluetooth layer.  Each was a
measuring tool for a question that has been answered.

## Naming

* A public function, type or constant starts with its module's prefix:
  `bt_send_input()`, `struct led_scene`, `UX_TAP`.  The prefix tells you which
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
| control | `ctl_poll()` every 1 ms, and through it every module except `bt.c` | highest app priority |
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
compiler (`cc -DUNITTEST -Isrc`) and runs it.  `test.h` supplies host
typedefs and stubs for the few `plat_`/`bt_` calls a module makes.  Tested:
gestures and the controller state machine, LED frames, settings records, calibration maths, each profile's report bytes, the Switch wake
code generation.  Not tested on the host: `platform_jieli*.c`,
`btstack_hci_jieli.c`, `bt.c`.

## Migration steps

Each step builds, flashes and is checked on hardware (Switch, Switch 2, one
other profile) before the next.

1. Delete instrumentation and unused tools; strip comments.
2. `platform_jieli.c`, `platform_jieli_sdk.c`; trim the SDK source list.
3. `bt.c` and `btstack_hci_jieli.c` with the `bt_` API; delete the imitation of
   the vendor host API.  Highest risk: the Classic reconnect rules in
   [bluetooth.md](bluetooth.md) must survive.
4. Profiles onto `struct profile`; merge files.
5. `controller.c`, `settings_ux.c`, `leds.c`.
6. `input*.c`, then `settings.c`.
   `usb.c` and the PC tool `padtool` come before the CDC console is removed,
   so there is always a way to read logs and reach the loader; that may
   mean doing them first.
7. `pad.h`, `UNITTEST` blocks, `make test`, update `AGENTS.md`.
