#ifndef CYCLONE2_IDENTITY_H
#define CYCLONE2_IDENTITY_H

#include "typedef.h"

/* The Bluetooth identity the running session presents: its address (from
 * config_slot_address(), set by the profile start) and its local name.  The
 * SDK's user_cfg.h hooks are implemented here. */

void cyclone2_remote_db_init(void);

#endif
