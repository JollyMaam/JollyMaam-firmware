#ifndef CYCLONE2_BT_BOARD_H
#define CYCLONE2_BT_BOARD_H

#include "typedef.h"

void cyclone2_bt_address_init(void);
void cyclone2_bt_address_for_slot(u8 slot, u8 address[6]);
void cyclone2_bt_address_randomize(void);
void cyclone2_bt_address_set(const u8 address[6]);
void cyclone2_remote_db_init(void);

/* The console a slot connects back to.  The record is kept in syscfg flash
 * unless the build sets CYCLONE2_BOND_STORE_RAM, in which case it is RAM only
 * and every boot starts with no remembered peer. */
int cyclone2_peer_address_read(u8 slot, u8 address[6]);
void cyclone2_peer_address_write(u8 slot, const u8 address[6]);

/* Cached, flash-backed build only: read the store into RAM once (before the
 * radio is up) and write the dirty slots back from an idle application task.
 * Both are no-ops on the RAM-only build. */
void cyclone2_peer_address_cache_load(void);
void cyclone2_peer_address_flush(void);

#endif
