#ifndef CYCLONE2_BT_BOARD_H
#define CYCLONE2_BT_BOARD_H

#include "typedef.h"

void cyclone2_bt_address_init(void);
void cyclone2_bt_address_for_slot(u8 slot, u8 address[6]);
void cyclone2_bt_address_randomize(void);
void cyclone2_bt_address_set(const u8 address[6]);
void cyclone2_remote_db_init(void);

/* Where persistent state is kept.
 *
 * Everything the firmware persists - the pad's Bluetooth address, each slot's
 * peer and Switch 2 bond (below), the link keys (btstack_jl_sdk.c), the
 * profile settings (controller.c) and the calibration (calibration.c) - lives
 * in RAM and is written to syscfg from there at safe points: bonds and
 * addresses once the radio has been idle for a while, settings and
 * calibration when the radio is not in a transition.
 *
 * RAM mode holds those writes back, so a configuration can be tried without
 * touching flash; switching back to flash mode writes everything that changed
 * at the next safe point, and a revert reloads what flash holds.  The pad
 * boots in flash mode. */
int cyclone2_store_ram_mode(void);
void cyclone2_store_set_ram_mode(int ram);

/* The pad's address, the slots' peers and Switch 2 bonds.  load reads them
 * from syscfg (at boot, before the radio; and for a revert, radio idle) and
 * drops pending changes; flush writes the ones that changed (idle radio,
 * not in RAM mode); pending says whether any changed. */
void cyclone2_settings_load(void);
void cyclone2_settings_flush(void);
int cyclone2_settings_pending(void);

/* The console a slot connects back to (all zeroes: none). */
int cyclone2_peer_address_read(u8 slot, u8 address[6]);
void cyclone2_peer_address_write(u8 slot, const u8 address[6]);

/* A slot's Switch 2 bond: the console's LE address (least significant byte
 * first) and the LTK the pairing derived, in the order the profile keeps it.
 * read returns 1 when the slot has one. */
int cyclone2_le_bond_read(u8 slot, u8 console[6], u8 ltk[16]);
void cyclone2_le_bond_write(u8 slot, const u8 console[6], const u8 ltk[16]);
void cyclone2_le_bond_clear(u8 slot);

#endif
