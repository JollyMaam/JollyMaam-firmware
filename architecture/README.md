# Firmware architecture

This folder describes how the application firmware is divided into modules and
how those modules talk to each other.  Hardware facts (pins, radio, power,
flash layout) live in [../docs/](../docs/README.md); this folder is about code.

Status: **target design**.  The code is being moved onto it in the steps listed
at the end; each module page says what it replaces.

| Page | Covers |
|---|---|
| [controller.md](controller.md) | the main state machine and 1 ms loop, Home gestures, rumble policy |
| [input.md](input.md) | buttons, sticks, triggers, calibration, IMU and gyro |
| [leds.md](leds.md) | what the lights show and how |
| [profiles.md](profiles.md) | the profile interface, and each console/host profile |
| [bluetooth.md](bluetooth.md) | `bt.c` over BTstack, and the BTstack to controller transport |
| [settings.md](settings.md) | settings, bonds and calibration storage; the UX tables |
| [usb.md](usb.md) | USB: stock-pad upgrades, firmware updates, logs, settings |
| [platform.md](platform.md) | the boundary to the Jieli SDK and board, and porting |

## Layers

A module may call down, never up.  Calls up go through a callback that the
upper layer registered, or through a value the upper layer polls.

```
 settings_ux.c   const tables: gestures, timeouts, LED scenes
 ──────────────────────────────────────────────────────────────────────────
 controller.c    state machine, 1 ms loop, gestures, rumble policy
 ──────────────────────────────────────────────────────────────────────────
 profile_switch.c  profile_switch_wake.c  profile_xbox.c
 profile_playstation.c  profile_hid.c          what each host expects
 ──────────────────────────────────────────────────────────────────────────
 bt.c            Bluetooth host: Classic HID device, LE GATT server, bonds
 btstack_hci_jieli.c   BTstack <-> Jieli controller (btctrler.a)
 ──────────────────────────────────────────────────────────────────────────
 input.c  input_calibration.c  input_imu.c  leds.c  settings.c  usb.c
 utils.c
 ──────────────────────────────────────────────────────────────────────────
 platform_jieli.c       time, tasks, storage, USB CDC, pins, LED chain, motors
 platform_jieli_sdk.c   constants and hooks the Jieli libraries require
```

`pad.h` is the only project header.  It has one section per module, in the
order above, and each section lists everything that module offers to others.
If a function is not in `pad.h`, it is `static`.

## Files

| File | Prefix | Replaces (today's files) |
|---|---|---|
| `pad.h` | – | every project header |
| `settings_ux.c` | `ux_` | `ux.c` table, timeouts in `controller.c`/`controller_state.c`, scene table in `lights_render.c` |
| `controller.c` | `ctl_` | `controller.c`, `controller_state.c`, `ux.c` engine, `rumble.c` policy |
| `profile_switch.c` | `sw_` | `profile_switch.c`, `profile_switch2.c`, `profile_switch2_att_db.h`, the two `.bin` files |
| `profile_switch_wake.c` | `swake_` | `profile_switch_wake*.c/h` (4 + 4 files); the wake step of a `switch` connect |
| `profile_xbox.c` | `xbox_` | `profile_xbox.c`, `profile_xbox_ble.c`, its att_db header and `.gatt` |
| `profile_playstation.c` | `ps_` | `profile_playstation.c`, `profile_dualsense.c`, `profile_dualshock4.c` |
| `profile_hid.c` | `hid_` | `profile_generic.c`, `profile_mouse.c` (stick and gyro mouse become one `mouse` profile) |
| `bt.c` | `bt_` | `bt/btstack_jl_core.c`, `bt/btstack_jl_le.c`, `bt/btstack_jl_sdk.c`, `bt/btstack_jl_ble_stub.c`, `bt/hci_dump_console.c`, `profile.c`, `profile_classic.c`, `profile_le.c`, `identity.c` |
| `btstack_hci_jieli.c` | `hci_` | `btstack-jl-port/port/jieli-br23/*` |
| `input.c` | `in_` | `input.c` |
| `input_calibration.c` | `cal_` | `calibration.c`, `calibration_math.c` |
| `input_imu.c` | `imu_` | `imu.c`, `gyro.c` |
| `leds.c` | `led_` | `lights.c`, `lights_render.c` (renderer) |
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
* `settings.c`: the settings schema (path, type, default, range, help).
* `platform_jieli.c`: the pin table.

## Threads

Three threads touch application code.

| Thread | Runs | Priority |
|---|---|---|
| control | `ctl_poll()` every 1 ms, USB vendor commands, timers posted with `plat_after()` | highest app priority |
| bluetooth | BTstack run loop: `bt.c` and the profile `write`/`read` handlers | below control |
| imu | IMU sampling for `input_imu.c` | lowest |

Rules:

* Profile `write`/`read` handlers run on the bluetooth thread.  They may only
  call the "latch" functions documented as safe from any thread
  (`ctl_rumble()`, `led_host_*()`, `bt_send_message()`, `set_bond_*()`).  Each
  stores a value that the control thread picks up on its next poll.
* Input reports cross from control to bluetooth through one newest-wins slot
  in `bt.c`.  Nothing is queued: on a weak link a lost input is better than a
  late one.
* On a single-threaded system (an nRF run loop), the same rules hold
  trivially.

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
gestures and the controller state machine, LED scenes, settings parse/format
and records, calibration maths, each profile's report bytes, the Switch wake
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
