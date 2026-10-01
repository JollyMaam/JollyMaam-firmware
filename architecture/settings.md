# Settings

Two files:

* `settings.c` (prefix `set_`) – the values the user can change, the bonds,
  the calibration; their defaults and storage.
* `settings_ux.c` (prefix `ux_`) – fixed tables that decide how the pad
  feels; see [ux.md](ux.md).

## Records

What is stored is a whole C struct per record, behind a header:

```c
struct head { u16 magic; u8 version; u8 size; };   /* magic 0x5032 */
```

| Record | Key | Contains | On a version or size mismatch |
|---|---|---|---|
| bond, one per slot | 34–37, 52–55 | Classic peer + link key + key type, LE peer + LTK, the pinned address | empty: that host must pair again |
| calibration | 49 | stick min/centre/max/gate/dead zones, trigger rest/full/dead zones | default ranges; logged as "calibration required" |

Preferences (slot types, colours, rumble, grips, pointer speeds, LED colours)
are not stored yet: they are the `const` tables at the top of `settings.c`.

The version is bumped by hand when a struct's layout changes.  There is no
per-field ID and no migration code.  Bonds and calibration are kept apart so
a change to one never costs the other.

Flash access is `plat_store_read(key, buf, len)` /
`plat_store_write(key, buf, len)` (on the Jieli SDK, syscfg VM items).

## Addresses

Each slot has its own Bluetooth address: derived from the flash UID, the slot
and its profile type (locally administered), and pinned in the bond record
once the slot bonds, so a later change to the derivation keeps the host's
pairing.  `set_slot_new_address()` makes a fresh one, for a pairing that must
look like a pad the host has never met.

## When records are written

Any change marks its record dirty.  `set_poll(now, radio_quiet)` writes dirty
records once nothing has changed for 100 ms and the radio is quiet: no pairing
exchange, link set-up or Switch 1 wake under way.  Calibration is written at
the next quiet moment without the 100 ms wait.  Writes are otherwise allowed while connected.  Sleep writes what is
pending before powering off.

## Changing settings

For now there is no way to change settings from outside: no console, and the
USB `SETTINGS_READ`/`SETTINGS_WRITE` commands ([usb.md](usb.md)) are
reserved but not implemented.  Values are changed by editing the defaults in
`settings.c`.  The records, the dirty/commit logic and the per-slot accessors
are kept so the later USB commands only have to move bytes.

When those commands come, the PC tool and the Android app need names, types
and ranges for each field.  Whether that table lives in the firmware (as
the old schema did) or in the tools is decided then.

## Interface

Called by the controller only, on the control thread.

```c
void set_init(void);
void set_poll(u32 now, int radio_quiet);
void set_commit(void);                          /* write now: before sleep or reboot */

const struct set_prefs *set_prefs(void);        /* led colours */
const struct set_slot *set_slot(u8 slot);       /* type, colour, rumble, grips, speeds */
void set_slot_address(u8 slot, u8 out[6]);      /* pinned, or derived from the flash UID */
void set_slot_new_address(u8 slot);

const struct bt_bond *set_bond(u8 slot);
void set_bond_store(u8 slot, const struct bt_bond *bond);   /* from BT_EV_BONDED, or a Switch 2 write() */
void set_bond_forget(u8 slot);

const struct set_cal *set_cal(void);            /* zero if missing or old: cal_apply() uses default ranges */
void set_cal_store(const struct set_cal *cal);  /* from the calibration routine */
```

The records and what is unsaved are this module's own state.  What goes into
them is always the controller's decision: it stores a bond when `bt.c` or a
profile reports one, and stores calibration when the routine reports done.

The tables that decide how the pad feels are not settings; they are in
`settings_ux.c` ([ux.md](ux.md)).
