# Settings

Two files:

* `settings.c` (prefix `set_`) – the values the user can change, the bonds,
  the calibration; their defaults and storage.
* `settings_ux.c` (prefix `ux_`) – fixed tables that decide how the pad
  feels: gestures, timeouts, LED scenes.  Changed by editing and rebuilding.

## Records

Everything persistent is one of three records, each a whole C struct written
to flash with a header:

```c
struct set_header { u16 magic; u16 version; u16 size; u16 crc; };
```

| Record | Contains | On a version or size mismatch |
|---|---|---|
| preferences | `led.*`, per slot: type, variant, report rate, in-flight, tx power, EDR3, colour, rumble, grips, pointer speeds | defaults |
| bonds | identity seed; per slot: pinned address, Classic peer + link key + key type, LE peer + LTK | kept as far as the old layout is understood; if not, empty: every host must pair again |
| calibration | stick min/centre/max/gate/dead zones, trigger rest/full/dead zones | defaults; the lights and the USB status say calibration is required |

The version is bumped by hand when a struct's layout changes.  There is no
per-field ID and no migration code: preferences are cheap to set again, bonds
and calibration are kept apart so a preferences change never costs a re-pair
or a recalibration.

Flash access is `plat_store_read(key, buf, len)` /
`plat_store_write(key, buf, len)` with one key per record (on the Jieli SDK,
syscfg VM items).

## When records are written

Any change marks its record dirty.  `set_poll(now, radio_quiet)` writes dirty
records once nothing has changed for 100 ms and the radio is quiet: no pairing exchange or link set-up
under way.  Writes are otherwise allowed while connected.  Sleep writes what is
pending before powering off.

## Changing settings

For now there is no way to change settings from outside: no console, and the
USB `SETTINGS_READ`/`SETTINGS_WRITE` commands ([usb.md](usb.md)) are
reserved but not implemented.  Values are changed by editing the defaults in
`settings.c`.  The records, the dirty/commit logic and the per-slot accessors
are kept so the later USB commands only have to move bytes.

When those commands come, the PC tool and the Android app need names, types
and ranges for each field.  Whether that table lives in the firmware (as
today's schema does) or in the tools is decided then.

## Interface

```c
extern const struct set_prefs  set;     /* read-only to everyone else */
void set_init(void);
void set_poll(u32 now, int radio_quiet);
int  set_commit(void);              /* write now: before sleep or reboot */
int  set_pending(void);

const struct set_slot *set_slot(u8 slot);
void set_slot_address(u8 slot, u8 out[6]);      /* pinned or derived */
void set_bond_classic(u8 slot, const u8 peer[6], const u8 key[16], u8 type);
void set_bond_le(u8 slot, const u8 peer[6], const u8 ltk[16]);
void set_bond_forget(u8 slot);
const struct set_bond *set_bond(u8 slot);
void set_calibration(const struct set_cal *cal);
const struct set_cal *set_cal(void);
int  set_cal_required(void);

```

Writers are safe from any thread; the bond setters are called from the
bluetooth thread.

## `settings_ux.c`

```c
extern const struct ux_gesture ux_gestures[];    /* Home chords */
extern const struct ux_timeouts ux_timeouts;     /* every fixed time */
extern const struct led_scene ux_scenes[];       /* what each state shows */
```

Gesture rows are as in today's `ux.c`: what is held with Home, when the row
applies (always, linked, not linked, offline), the hold time (0 at once,
`UX_TAP` on release), and the action.

`ux_timeouts` collects every fixed time that is now a `#define` scattered over
the controller:

| Field | Now |
|---|---|
| `home_grace_ms`, `home_tap_ms` | 150, 60 |
| `connect_ms`, `connect_retries` | 30 000, 2 |
| `pair_ms` | 180 000 |
| `idle_sleep_ms` | 300 000 (no input while linked, on battery) |
| `offline_sleep_ms`, `unplug_sleep_ms` | 15 000, 5 000 |
| `stop_max_ms` | 12 000 |
| `boot_scene_ms` | 500 |
| `rumble_max_ms` | 2 000 |
| `uboot_window_ms` | 3 000 |
