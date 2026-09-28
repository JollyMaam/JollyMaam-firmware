# Config, status and the console

Status: agreed with the user 2026-09-28; being implemented.

Replaces the six ad-hoc persistent stores (profile settings, calibration, link
keys, peer addresses, Switch 2 bonds, the BTIF address), `store ram|flash|revert`
and the `strcmp` chain in `usb.c`.

## The model

* **`struct config config`** (`src/config.h`) is an ordinary typed struct.
  Application code reads and writes it directly: `config.report_ms`,
  `config.profile[slot].bond.peer`.
* **`config_items[]`** (`src/config_schema.c`) describes every leaf of it, in
  plain C designated initializers - no generator, no X-macros:

      { .uid = 2, .path = "report_ms", .type = CFG_ENUM, CFG_FIELD(report_ms),
        .choices = report_periods, .def = { .u = 8 }, .apply = CFG_LIVE,
        .help = "HID report period in ms" },

  * `uid` - a number that is never reused; a leaf whose type changes gets a
    new one.  Duplicates are caught by a self-check at boot (logged) and in
    the host test.
  * `path` - dotted; `#` is the array index (`profile.#.type`), one level.
  * the default is in the entry: `.def.u` (a scalar, or every element of a
    list), or `.def_ptr` (raw bytes: one value per array index when the leaf
    is in an array).  Unset means zero.
  * `apply` - when a change takes effect: `CFG_LIVE`, `CFG_NEXT_CONNECT`, or
    `CFG_REBOOT` (supported, used by nothing today).  Setting a value never
    restarts, reconnects or reboots anything; the console says which applies.
* **`status_items[]`** (`src/status.c`) - same entry type, a getter instead of
  storage, read-only.

## Storage

* Groups, one syscfg item each: `PREFS` (item 48), `CAL` (49), `BOND` (one per
  slot, items 34..37).  The old records in those items fail the format byte
  and read as defaults; no legacy loader (by agreement).
* An item is `0xC1` then records `key | len | value`, `key` a 7-bit varint of
  `uid << 3 | index` (arrays up to 8).  **Only non-default values are
  stored.**  So a new leaf costs nothing, a changed default reaches every pad
  that never set the value, an unknown uid is skipped (downgrade), and a
  record with a wrong length or out-of-range value is dropped with a log line.
* `committed` is a RAM copy of what flash holds.  A group is pending when any
  of its leaves differs from it.  Writing a group = encode it with interrupts
  masked, `syscfg_write`, copy the group's leaves into `committed`.
* The pad's own address is no longer stored at all (see Identity).

## When things are written

* `config.autocommit = true` (default): a pending group is written once
  * config has not changed for 100 ms (CRC over `config`, checked each poll),
  * and the radio is not mid-pairing: `radio_state` is OFF, CONNECTED, or
    CONNECTING with no ACL link up, and has been for 500 ms.  PAIRING,
    STOPPING and WAKING hold writes.  (Measured before: the resets were
    syscfg access inside the SSP exchange and during a profile start.)
* `config.autocommit = false`: **nothing is written**, not even that setting;
  `commit` writes everything pending (under the same timing rule).  No revert:
  reboot to drop changes.
* `factory-reset` - every leaf to its default (autocommit back to true), then
  committed.  `factory-reset --no-commit` = `config.autocommit = false` +
  `unset config`: nothing written; `commit` keeps it, reboot undoes it.
* `unset PATH` - to the default; `unset config` = everything except
  `autocommit`.  Both follow the autocommit mode.
* A pad powering itself off writes what is pending first (autocommit only).

## Identity (Bluetooth addresses)

* `seed` = `config.identity.seed` if set, else the SPI flash's factory unique
  ID (`get_norflash_uuid()`).  If the flash has none, a random seed is drawn
  once and stored in `identity.seed`.
* unbonded slot: `address = hash(seed, slot, type, profile.#.generation)`,
  forced locally administered unicast.  Nothing stored.
* bonded slot: `profile.#.bond.local`, recorded when the pairing completed.
  An exported bond therefore carries its address and restores on any pad.
* `pair N` clears the slot's bond; `pair N --fresh` also bumps `generation`
  (a new identity the console has never seen).

## One bond per profile

`profile.#.bond.{local, peer, link_key, key_type, le_peer, ltk}`.  The BTstack
link-key database callbacks become views of the running slot's bond; the
classic peer and the Switch 2 LE bond read/write the same struct.  Pairing
writes happen in the btstack task; the flush encodes under masked interrupts,
so it never sees half a bond.

## Console

One command table (`name`, `usage`, `help`, handler taking argc/argv).  The
dispatcher answers `help X`, `X -h`, `X --help` for every command, and
`help PATH` / `PATH --help` for every leaf (type, range/choices, default,
when it applies, help text).

    config[.path]              list (non-default: "# default X", pending: "# not committed")
    config.path = value        set (quotes optional); prints when it applies
    status[.path]              read-only state
    unset PATH                 back to the default (subtree allowed)
    export [PATH]              non-default leaves as importable lines, keys included
    import ... end             stage lines; all validated, applied together or not at all
    commit / pending           write / list what is not written
    factory-reset [--no-commit]

Actions stay commands (`connect`, `disconnect`, `pair`, `wake`, `reboot`,
`uboot`, `cal start|done|cancel|raw`, `inject`, `rumble`, `imu`, debugging
aids).  Removed because they are now config writes: `report`, `profile`,
`store`, `setaddr`, `newaddr`, `peer`, `setkey`, `bond`, `restore`, `keys`,
`unkey`, `cal reset|deadzone`.  The old `status` dump becomes `bringup`; `stat`
stays for the tools.

**Line editing and completion are in the firmware.**  Tab completes commands,
paths and enum values after `=`; a second Tab lists candidates.  When a log
line arrives while a line is being typed, the input line is erased, the log
printed and the prompt and partial input redrawn.

**Machine mode** (for padbus and scripts), entered with a control byte like
the existing ENQ ping: no echo, no prompt, every output line tagged -
`@ID command` in, `=ID ...` output, `.ID OK` / `.ID ERR msg` to finish, `!`
log lines, `#` events.  `complete LINE` gives the TUI the same completion.
A second CDC interface stays possible later without changing the protocol.

## Order

1. Config core + schema + storage + identity; move every store onto it; old
   console commands temporarily routed to it.  Hardware: boot, pair, reconnect,
   reboot, flash only written when expected.
2. New console: table, argv, help, config/status/unset/export/import/commit,
   completion, log redraw.
3. Machine mode; padbus/TUI and the scripts that parse console output.
4. Docs (`bond-store.md` rewritten as `config.md`, `usb-control.md`).

## To measure on hardware

* `get_norflash_uuid()` returns a real ID.
* The largest syscfg item the VM takes (the CAL group is ~180 bytes at most).
* Nothing in the SDK libraries reads items 34..37 or 48/49.
