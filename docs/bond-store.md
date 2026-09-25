# The RAM-only build (`BOND_STORE_RAM`)

Everything this firmware persists lives in syscfg, so `BOND_STORE_RAM=1` turns
the whole of it into RAM.  The name comes from the first thing that moved (the
bond); the flag now covers the pad's identity and its profile settings too.

## What is stored, by default

| what | item | area | written when |
|---|---|---|---|
| link keys, one tagged record per peer | `CFG_REMOTE_DB_00..15` (62..77) | VM | on pairing; zeroed by `unkey`/`delall` |
| the peer address each slot pages | `CFG_EDR_ADDRESS_BEGIN..+3` (34..37) | VM | on **every** connection |
| the pad's own Bluetooth address | `CFG_BT_MAC_ADDR` (102) | BTIF | first boot if blank; `newaddr`, `pair N fresh`, `unkey` |
| profile settings (per-slot type, report period) | `CFG_CYCLONE2_PROFILE_SETTINGS` (48) | VM | the `report N` command |

The link-key records are `tag (0xC2) | peer address (6) | link key (16) |
key type (1)`, reached through the `btstack_link_key_db_t` callbacks that
`btstack_init()` installs - that is the database that answers the console's
`LINK_KEY_REQUEST` on a reconnect.  The tag byte is what separates these
records from the 38-byte ones the vendor host library left in the same items.

By default all of it is in flash, so bonds and identity survive a reset *and* a
reflash.  That is why a reflash cannot be used to clear a pairing: the
controller's own records (retention memory at `0x10700`, dumped by `bondstore`
and zeroed by `clearkeys`) and these survive it too.

## `BOND_STORE_RAM=1`

    # src/build_config.h: #define CYCLONE2_BOND_STORE_RAM 1
    make
    tools/flash.sh

With it on, every row of the table above is held in RAM.  Flash is still *read*
at boot, so the pad comes up with the identity and the slot types that are
already flashed; nothing is written back.

What that buys:

* every boot starts with no bonds at all - a reflash, a reset or a power-cycle
  is a clean slate;
* pairing, connecting, switching profiles, changing the report period and
  generating a new address all cost no flash write, so a test can churn them as
  often as it likes;
* a bond record that was never written reads back as empty (its tag byte is
  zero), so `jl_link_key_get()` and the iterator behave exactly as they do with
  blank syscfg items;
* bonds survive a profile switch, because a profile switch no longer resets the
  chip (see `docs/profile-switch.md`) and RAM is not touched by it.

What to expect from RAM-only state:

* `newaddr` and `pair N fresh` give the pad an identity that is gone at the next
  reset, so the console has to be re-paired after a reboot;
* on a device whose BTIF is blank there is no address to read, so the pad
  generates one per boot until it is flashed with a build that persists it;
* the report period reverts to whatever flash holds on the next boot.

`newaddr` exists for this: it generates a fresh random address and restarts the
active session so the controller picks it up, without changing the slot or the
pairing mode.  `pair N fresh` is the same thing plus entering pairing.

## It does not touch controller state

The controller's own pairing records at `0x10700` are controller state, not
host state, and are RAM anyway.  Clearing them still needs `clearkeys` (which
zeroes the region and restarts) or the per-peer `unkey` command.

## Verification

With the flag off, the application calls `syscfg_write` from five places.  With
it on, none: disassembling `build-ram/app.elf` and looking for calls to
`syscfg_write` leaves only four SDK-internal sites (crystal-offset trim and the
RTC time saves) that have no caller and no function pointer anywhere in the
loaded image.  `make itest` is what proves it on hardware; see
`docs/testing.md`.  For where the items physically land, see
`docs/flash-map.md`; for the RAM the firmware touches by hand, see
`docs/memory-map.md`.

The default (`BOND_STORE_RAM=0`) is unchanged: everything persists in flash.
