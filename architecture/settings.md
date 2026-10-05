# Settings

Two files:

* `settings.c` (prefix `set_`) – the values the user can change, the bonds,
  the calibration; their defaults and storage.
* `settings_ux.c` (prefix `ux_`) – fixed tables that decide how the pad
  feels; see [ux.md](ux.md).

## The model

* **Targets**: the eight slots (the same fields in each), the palette
  (global) and calibration (the unit's).
* **Fields** are rows of a table in `settings.c`: a never-reused id, a name,
  a type, the range or value names, the default per slot, a line of help.
  The tools read the table over USB, so a new row needs no tool update.
* **Each field is default or set.**  Only set fields are stored.  Reset
  clears them, so a better default in a new firmware reaches every field that
  was never set.

```c
{ .id = 7, .name = "rumble", .type = T_NUMBER, .max = 100, FIELD(rumble),
  .def = ALL(100), .help = "motor strength in percent; 0 turns it off" },
```

Per slot: `type`, `connect_colour`, `primary_colour`, `secondary_colour`,
`grips`, `players`, `rumble`, `mouse_layout`, `stick_speed`, `gyro_speed`,
`gyro_accel`, `address`, `bond` (`padtool fields` lists them with ranges and
defaults).  `set_slot(slot)` returns them as a plain `struct set_slot`.

**Colours** are `{palette entry, brightness}`: brightness 0-255 scales the
entry, which is an RGB at full brightness.  Sixteen entries; 0 and 1 are the
idle and battery roles, whose brightness is in `settings_ux.c`
(`ux_colours`).  The default palette and brightnesses give the colours the
pad had before.

**Changing `type` clears `address` and `bond`**: a new type is a new device
to the console.  Items apply in order, so a specific address goes after the
type.

## Changing settings: collect, then commit

From outside (USB) a change is items `{target, id, len, value}`; `len` 0 puts
a field back to its default, id 0 (len 0) the whole target.

1. `set_write()` checks each item as it arrives and collects the good ones
   (up to 1 KB); a bad one is refused with a code (unknown, bad length, out
   of range, full).
2. `set_apply()` applies what was collected, in order, and saves.
3. `set_drop()` throws it away; so does a reboot.

Nothing changes until commit, so pairing, which saves at once, only ever
saves settings that are already saved.  After commit, what the loop reads
every frame (colours, LED modes, rumble) shows at once; what is fixed when a
link starts (type, address, bond, mouse settings) on the next connect.

## Ephemeral mode

`set_ephemeral()` (USB): from then on nothing is written to flash, not
commits, not pairing, not calibration, until the next boot.  Commit still
applies.  What was waiting to be written is written first.  The controller
makes Home wink and, disconnected on battery, sleeps after `idle_sleep_ms`
instead of the short offline/unplug timeouts.  Any reboot or sleep ends it.

## Records

One record per target, syscfg user items 1-8 (slots), 9 (palette), 10
(calibration), each written at a fixed 160 bytes:

```
u16 magic 0x4332, u8 version, u8 used, then {u8 id, u8 len, value} per set field
```

An unknown id or a value out of range or of the wrong length is dropped on
load; a record with another magic or version is ignored (defaults).  Rows can
be added or removed without invalidating the rest.

Flash access is `plat_store_read(key, buf, len)` /
`plat_store_write(key, buf, len)` (on the Jieli SDK, syscfg VM items).

## Addresses

Each slot has its own Bluetooth address: derived from the flash UID, the slot
and its type (locally administered) while `address` is zero, and pinned there
when the slot bonds, so a later change to the derivation keeps the host's
pairing.  `set_slot_new_address()` makes a fresh one, for a pairing that must
look like a pad the host has never met.

## When records are written

Any change marks its record dirty.  `set_poll(now, radio_quiet)` writes dirty
records once nothing has changed for 100 ms and the radio is quiet: no pairing
exchange, link set-up or Switch 1 wake under way.  A commit and calibration
are written at the next quiet moment without the 100 ms wait.  Writes are
otherwise allowed while connected.  Sleep and reboot write what is pending
first (`set_flush()`).

## Interface

Called on the control thread: by the controller, and by `usb.c` for the
commands that move settings.

```c
void set_init(void);
void set_poll(u32 now, int radio_quiet);
void set_flush(void);                           /* write now: before sleep or reboot */

const struct set_slot *set_slot(u8 slot);
struct rgb set_colour(struct colour c);         /* through the palette */
void set_slot_address(u8 slot, u8 out[6]);      /* pinned, or derived from the flash UID */
void set_slot_new_address(u8 slot);

const struct bt_bond *set_bond(u8 slot);
void set_bond_store(u8 slot, const struct bt_bond *bond);   /* from BT_EV_BONDED, or a Switch 2 write() */
void set_bond_forget(u8 slot);

const struct set_cal *set_cal(void);            /* zero if missing: cal_apply() uses default ranges */
void set_cal_store(const struct set_cal *cal);  /* from the calibration routine */

/* usb.c */
u32 set_describe(u32 n, u8 *out, u32 max);      /* field n of all targets; 0 past the last */
u32 set_read(u8 target, u8 *out, u32 max);
u32 set_write(const u8 *items, u32 len, u8 *results, u32 max);
void set_apply(void);
void set_drop(void);
u32 set_staged(void);
void set_ephemeral(void);
int set_is_ephemeral(void);
```

The tables that decide how the pad feels are not settings; they are in
`settings_ux.c` ([ux.md](ux.md)).
