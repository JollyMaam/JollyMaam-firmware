#ifndef HCI_QUEUE_H
#define HCI_QUEUE_H

#include "pad.h"

/* The transport's receive queue (hci_queue.c): packets from the controller
 * for the Bluetooth thread.  Writers: the controller's task, and any thread
 * that holds plat_lock() (the controller's task is above it); one reader, the
 * Bluetooth thread.  A full queue drops the packet and counts it; `most`
 * counts the slot being handled. */

void hciq_clear(void);
u8 *hciq_put(u8 type, const u8 *packet, u16 size);   /* the copy, or 0 when dropped */
int hciq_peek(u8 *type, u8 **data, u16 *size);        /* the oldest; 0 when empty */
void hciq_pop(void);
u32 hciq_free(void);                                  /* slots free */
void hciq_held(void);                                 /* count an ACL packet kept back */
void hciq_stats(struct hci_queue_stats *out);

#endif
