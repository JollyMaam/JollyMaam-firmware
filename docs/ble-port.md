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

## 2026-09-25 evening: GATT server up; the console stalls on 2M

Done and verified with `tools/esp32-lecentral` (ESP32 as LE central, 1M PHY,
also with the console's 13-channel map `1fff000000`): the pad answers Exchange
MTU (247), Read By Type (the whole database), the CCC write, and a Switch 2
command on `0x0016` with a notification on `0x001e`.

* `tools/gen_switch2_gatt.py` -> `src/switch2_att_db.h`: the Pro Controller 2
  database from ndeadly's table in BTstack's format, handles checked (the old
  table was Jieli's variant of the format and did not load).
* `src/bt/btstack_jl_le.c`: one LE peripheral connection - ATT server,
  advertising, CCC state, report/response notifications, and the LE LTK
  request answered from the profile (no SMP).
* `src/bt/btstack_jl_sm_stub.c`: the five SM/GAP functions att_server needs,
  instead of `sm.c`.  Without SM, `gap_reconnect_security_setup_active()`
  reported an IRK lookup "pending" forever and att_server held every request
  (MTU exchange never answered); the stub says nothing is pending.
* `profile_switch2.c` on the new layer; AES from BTstack's rijndael.
* Controller: LE Data Length Extension and 2M PHY enabled
  (`lib_btctrler_config.c`), LE packets 251 bytes.
* `CODE_LIMIT` raised 0x40000 -> 0x48000: the image outgrew 256 KiB.  The VM
  (settings, bonds) starts at the code limit, so it moved; existing bonds were
  lost once.
* `PATCH_BTCTLER "markers..."` now also records LE link-layer traffic:
  `0x4e00|op` LL control PDU received, `0x4b00|op` sent, `0x4f01` data PDU
  received.  LE layer: `0x4c01` connected, `0x4c02` disconnected, `0x4c03`
  LTK answered, `0x4c04` refused, `0x4c05` encryption; profile `0x4d00|cmd`
  command answered.

**Open: the console never gets past the MTU exchange.**  With 2M offered it
runs LL_FEATURE, LL_PHY and PHY_UPDATE (-> 2M/2M), sends Exchange MTU, we
answer (HCI trace), and then it only sends LL_CHANNEL_MAP_IND over and over.
Without 2M it never sends a PHY request and never starts ATT at all.  The real
controller's capture has no channel map updates and moves on to Read By Type
15 ms after the MTU response.  Reading: our transmissions after the switch to
2M do not reach the console.  The ESP32 used here is Bluetooth 4.2 (no 2M), so
the next test needs a 2M-capable central (the Linux Intel adapter with LE Set
PHY, or an ESP32-C3/S3).
