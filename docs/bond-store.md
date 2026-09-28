# The persistent store and `store ram`

Everything this firmware persists lives in syscfg.  It is all held in RAM
first and written to flash from there at safe points; the console's `store`
command decides whether those writes happen.

    store            show the mode, and whether anything is not written yet
    store ram        hold every write back: try a configuration without
                     touching flash
    store flash      write what changed (at the next safe point) and keep
                     writing from now on - i.e. commit
    store revert     drop what changed and reload everything from flash
                     (radio off: `disconnect` first)

The pad boots in flash mode, so a reset in RAM mode also discards anything
that was not committed.  This replaces the old `CYCLONE2_BOND_STORE_RAM` build
option, which made the same choice at compile time (and started every boot
with no bonds; RAM mode starts from what flash holds).

## What is stored

| what | item | area | written |
|---|---|---|---|
| link keys, one tagged record per slot and peer | `CFG_REMOTE_DB_00..15` (62..77) | VM | radio idle |
| the peer address each slot pages | `CFG_EDR_ADDRESS_BEGIN..+3` (34..37) | VM | radio idle |
| each slot's Switch 2 bond (console address, LTK) | `CFG_BLE_BONDING_REMOTE_INFO+slot` | VM | radio idle |
| the pad's own Bluetooth address | `CFG_BT_MAC_ADDR` (102) | BTIF | radio idle |
| profile settings (per-slot type, report period) | `CFG_CYCLONE2_PROFILE_SETTINGS` (48) | VM | connected or idle |
| stick and trigger calibration | `CFG_CYCLONE2_CALIBRATION` | VM | connected or idle |

"Radio idle" is 3 s with no radio session (`CYCLONE2_FLASH_IDLE_MS`,
`cyclone2_poll()`); "connected or idle" is any time the radio is not in a
transition.  A pad powering itself off writes whatever is still pending first.
The rules come from `docs/reboot-hunting.md`: a syscfg access while pairing,
connecting or switching profiles resets the chip, so nothing on those paths
touches flash - not even a read.  The link-key store is a RAM mirror loaded at
boot (`btstack_jl_link_key_load()`), and every other store is read at boot too
(`board_init()`), before the radio first runs.

The link-key records are `tag | peer address (6) | link key (16) | key type
(1)`, reached through the `btstack_link_key_db_t` callbacks that
`btstack_init()` installs - that is the database that answers the console's
`LINK_KEY_REQUEST` on a reconnect.  The tag is `0xC8 | slot` for the slot the
key belongs to (each slot has its own local address, so a host paired on two
slots holds two keys), or `0xC2` for records written before keys were per
slot, which any slot may use.  The tag is also what separates these records
from the 38-byte ones the vendor host library left in the same items.

Flash survives a reset *and* a reflash (`docs/flash-map.md`), which is why a
reflash cannot be used to clear a pairing: the controller's own records
(retention memory at `0x10700`, dumped by `bondstore` and zeroed by
`clearkeys`) and these survive it too.

## Using RAM mode

* Pairing, connecting, switching profiles, `report N`, `profile N type`,
  calibrating and `newaddr` all cost no flash write, so a test can churn them
  as often as it likes (`make itest` proves it on hardware; see
  `docs/testing.md`).
* `store` says `changes not written yet` once anything differs from flash.
  `store flash` writes it all at the next safe point; `store revert` or a reset
  throws it away.
* `newaddr` and `pair N fresh` in RAM mode give the pad an identity that is
  gone at the next reset, so a console paired to it has to be re-paired after
  a reboot.
* Bonds survive a profile switch, because a profile switch does not reset the
  chip (`docs/profile-switch.md`).

## It does not touch controller state

The controller's own pairing records at `0x10700` are controller state, not
host state, and are RAM anyway.  Clearing them still needs `clearkeys` (which
zeroes the region and restarts) or the per-peer `unkey` command.
