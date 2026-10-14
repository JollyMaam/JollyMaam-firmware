#ifndef DONGLE_H
#define DONGLE_H

#include "pad.h"
#include "link.h"

void dongle_bt_start(void);
void dongle_bt_target(const u8 addr[6]);   /* display order */
void dongle_bt_forget(void);
u8 dongle_bt_state(void);
int dongle_bt_linked(void);                 /* our pad, its HID channels open */
int dongle_bt_take_input(struct pad *p);    /* the newest state, once */
int dongle_bt_take_config(struct link_config *c);
void dongle_bt_output(const u8 out[LINK_OUTPUT_SIZE]);
struct link_stats { u32 inputs, replaced, lost; };   /* counted on the Bluetooth thread */
void dongle_bt_stats(struct link_stats *out);
int dongle_store_write(u16 key, const void *buf, u16 len);

#define DONGLE_OUTPUT_SIZE 30
void dongle_output(u8 out[DONGLE_OUTPUT_SIZE]);
int dongle_address_valid(const u8 addr[6]);
void dongle_identity(u8 own[6], u8 peer[6]); /* own: controller order; peer: display order */

struct dongle_btn {
    u8 down;
    u8 hold_fired;
    u32 press_ms;
};

enum {
    DONGLE_BTN_NONE = 0,
    DONGLE_BTN_DOWN,
    DONGLE_BTN_TAP,
    DONGLE_BTN_HOLD,
};

#define DONGLE_BTN_HOLD_PAIR_MS 1500

int dongle_btn_update(struct dongle_btn *b, u32 now, int is_down, u32 hold_threshold_ms);

#endif
