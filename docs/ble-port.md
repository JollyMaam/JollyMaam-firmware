# LE on the BTstack port (for the Switch 2 profile)

The BTstack port started Classic-only: the two LE profiles (generic HOGP and
the Switch 2 GATT controller, `src/profile_switch2.c`) were written against
the vendor host library's LE stack, which the port removed, and
`src/bt/btstack_jl_ble_stub.c` stubs them out.  This page tracks moving LE onto
upstream BTstack (`att_server`, `sm`, GATT) over the controller's own LE link
layer.

## Milestone 1 - LE advertising from BTstack: done (2026-09-25)

* `btctrler.a` already runs its LE link layer:
  `config_btctler_modules = BT_MODULE_CLASSIC | BT_MODULE_LE`, roles
  `LE_ADV | LE_SLAVE` (`src/sdkconfig/lib_btctrler_config.c`).  Nothing on the
  controller side had to change.
* `src/btstack/btstack_config.h`: `ENABLE_BLE`, `ENABLE_LE_PERIPHERAL`, LE
  device DB / SM lookup / whitelist sizes, `MAX_ATT_DB_SIZE 512`.  The Makefile
  builds `att_db`, `att_db_util`, `att_dispatch`, `att_server`,
  `le_device_db_memory` and `sm` from the BTstack checkout.
* `btstack_jl_core_le_advertise(data, len)` queues LE advertising (ADV_IND,
  30 ms, all channels; `len` 0 = off) onto the BTstack run loop, like the
  port's other requests.  Console: `bleadv on|off` advertises the Switch 2
  payload from `profile_switch2.c` while BTstack is up (any Classic profile).
* Verified with `tools/esp32-lescan` (controller-only ESP32 passive scanner):
  the pad's public address advertises the exact payload
  `0201061bff53050100037e05692000010000000000000f000000000000` while on, and
  stops on `bleadv off` (count frozen), alongside a running Classic profile.

Note: that payload declares its manufacturer field as 27 bytes (`0x1b`) but
carries 25, so the advertisement is 29 bytes.  It is what the old profile and
the emulator reference used; check it against a real Switch 2 controller's
advertisement before relying on it.

## Next

2. **GATT server**: the Switch 2 controller's services and characteristics as
   a BTstack ATT DB (the old profile's tables are in
   `src/switch2_profile_data.h`), `att_server_init`, `MAX_NR_HCI_CONNECTIONS`
   raised for Classic + LE, LE ACL routed through the bridge.  Test by
   connecting from the ESP32 (LE central) or Linux and reading the DB.
3. **Pairing / security**: the Switch 2's pairing is a custom exchange over
   GATT whose key is `A1 XOR B1`
   (`/shared/github.com-ndeadly-switch2_controller_research`), then LE
   encryption with that key; `sm` needs the key injected rather than an SMP
   pairing.
4. **Reports and the profile**: port `profile_switch2.c` off the vendor LE API
   onto the above; then a real Switch 2.
5. **Wake**: a Switch 2 is woken by an LE advertisement replayed from a real
   Switch 2 controller (NS1-Wakeup-Demo `src/wake/ns2_wake.c`), which this
   advertising path can already send once the packet is known.
