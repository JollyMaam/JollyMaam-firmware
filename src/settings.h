#ifndef CYCLONE2_BT_BOARD_H
#define CYCLONE2_BT_BOARD_H

#include "typedef.h"

void cyclone2_bt_address_init(void);
void cyclone2_bt_address_for_slot(u8 slot, u8 address[6]);
void cyclone2_bt_address_randomize(void);

#endif
