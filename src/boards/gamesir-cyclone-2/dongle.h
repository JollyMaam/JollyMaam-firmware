#ifndef DONGLE_H
#define DONGLE_H

#include "pad.h"
#include "link.h"

void dongle_bt_start(void);
int dongle_tool_only(int on);             /* 0 off, 1 on (until power loss), else read; returns the mode */
int dongle_hub(int on);                     /* 0 off, 1 on (RAM), else read; returns the mode */
/* Per slot (below HUB_SLOTS); slot 0 is the dongle's own pad. */
void dongle_bt_forget(u8 slot);             /* and pair that slot */
void dongle_bt_slots(u8 n);                 /* slots that link: 1, or HUB_SLOTS in hub mode */
u8 dongle_bt_state(void);
int dongle_bt_linked(u8 slot);              /* our pad, encrypted and subscribed */
int dongle_bt_take_input(u8 slot, struct pad *p);    /* the newest state, once */
int dongle_bt_take_raw_input(u8 slot, u8 out[LINK_INPUT_SIZE]);   /* hub mode: the newest, as sent; takes the parsed one too */
int dongle_bt_take_config(u8 slot, struct link_config *c);
void dongle_bt_output(u8 slot, const u8 out[LINK_OUTPUT_SIZE]);
struct link_stats { u32 inputs, replaced, lost; };   /* counted on the Bluetooth thread */
void dongle_bt_stats(u8 slot, struct link_stats *out);
void hci_jieli_rx_stats(struct hci_queue_stats *out);         /* btstack_hci_jieli.c */
int dongle_store_write(u16 key, const void *buf, u16 len);

u16 dongle_interval(u8 linked, int waiting, int hub); /* connection interval, 1.25 ms units */
int dongle_address_valid(const u8 addr[6]);
void dongle_identity(u8 own[6]);            /* controller order */

#endif
