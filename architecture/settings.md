# Settings

Two files:

* `settings.c` (prefix `set_`) – the values the user can change, the bonds,
  the calibration; their defaults and storage.
* `settings_ux.c` (prefix `ux_`) – fixed tables that decide how the pad
  feels; see [ux.md](ux.md).

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

Called by the controller only, on the control thread.

```c
void set_init(void);
void set_poll(u32 now, int radio_quiet);
void set_commit(void);                          /* write now: before sleep or reboot */

const struct set_prefs *set_prefs(void);        /* led colours */
const struct set_slot *set_slot(u8 slot);       /* type, rate, colour, speeds, ... */
void set_slot_address(u8 slot, u8 out[6]);      /* pinned, or derived from the flash UID */

const struct bt_bond *set_bond(u8 slot);
void set_bond_store(u8 slot, const struct bt_bond *bond);   /* from BT_EV_BONDED or profile effects */
void set_bond_forget(u8 slot);

const struct set_cal *set_cal(void);            /* defaults if the record is missing or old */
int  set_cal_required(void);
void set_cal_store(const struct set_cal *cal);  /* from the calibration routine */
```

The records and what is unsaved are this module's own state.  What goes into
them is always the controller's decision: it stores a bond when `bt.c` or a
profile reports one, and stores calibration when the routine reports done.

The tables that decide how the pad feels are not settings; they are in
`settings_ux.c` ([ux.md](ux.md)).
