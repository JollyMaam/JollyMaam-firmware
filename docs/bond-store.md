# The bond store, and keeping it in RAM

## What the bond store is

The host side of the port keeps one record per bonded peer in
`src/bt/btstack_jl_sdk.c`:

    tag (0xC2) | peer address (6) | link key (16) | key type (1)

Sixteen of those records live at syscfg items `CFG_REMOTE_DB_00..` (24 bytes
each).  BTstack reaches them through the `btstack_link_key_db_t` callbacks that
`btstack_init()` installs, so this is the database that answers the console's
`LINK_KEY_REQUEST` on a reconnect.  The tag byte is what separates these
records from the 38-byte ones the vendor host library used to leave in the same
items.

By default the records are written to syscfg flash, so they survive a reset and
a reflash.  That is why a reflash cannot be used to clear a pairing: the
controller's own records (retention memory at 0x10700, dumped by `bondstore`
and zeroed by `clearkeys`) and these still survive it.

## `BOND_STORE_RAM=1`

Building with `BOND_STORE_RAM=1` moves the whole host-side bond into internal
RAM and leaves syscfg untouched.  That is both halves:

* the sixteen link-key records above (`src/bt/btstack_jl_sdk.c`), and
* the paired peer address each slot pages (`src/settings.c`,
  `cyclone2_peer_address_read()/_write()`, syscfg `CFG_EDR_ADDRESS_BEGIN..`).

The peer address is the other half of the bond: the key alone does not say who
to page, and the address alone cannot authenticate.  It is also the record the
firmware rewrites on *every* connection, so it is the more frequent flash
write of the two.  Making both volatile is what lets a test start from "no
console has ever been seen" on every boot.

    make BOND_STORE_RAM=1 O=build-ram
    tools/flash.sh --image build-ram/firmware.img

With it on:

* every boot starts with no bonds at all -- a reflash, a reset, or a
  power-cycle is a clean slate;
* pairing costs no flash write;
* a record that was never written reads back as empty (its tag byte is zero),
  so `jl_link_key_get()`/the iterator behave exactly as they do with blank
  syscfg items;
* the store still survives a profile switch, because a profile switch no
  longer resets the chip (see `docs/profile-switch.md`) and the RAM is not
  touched by it.

It does **not** touch the controller's own pairing records at 0x10700.  Those
are controller state, not host state; clearing them still needs `clearkeys`
(which zeroes the region and restarts) or the per-peer `unkey` command.

`make itest` verifies the no-flash-write property end to end against a real
console; see `docs/testing.md`.  For where these items physically land, see
`docs/flash-map.md`; for the RAM the firmware touches by hand, see
`docs/memory-map.md`.

The default (`BOND_STORE_RAM=0`) is unchanged: bonds persist in flash.
