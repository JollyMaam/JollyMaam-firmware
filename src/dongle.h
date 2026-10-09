#ifndef DONGLE_H
#define DONGLE_H

#include "pad.h"

void dongle_bt_start(void);
void dongle_bt_target(const u8 addr[6]);   /* display order */
void dongle_bt_forget(void);
u8 dongle_bt_state(void);
int dongle_store_write(u16 key, const void *buf, u16 len);

#define DONGLE_OUTPUT_SIZE 30
void dongle_output(u8 out[DONGLE_OUTPUT_SIZE]);
int dongle_address_valid(const u8 addr[6]);
void dongle_identity(u8 own[6], u8 peer[6]); /* own: controller order; peer: display order */

#endif
