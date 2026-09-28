#ifndef CYCLONE2_WAKE_RADIO_H
#define CYCLONE2_WAKE_RADIO_H

#include "generic/typedef.h"

struct wake_radio_config {
    u8  address[6];      /* the console, least significant byte first */
    u8  parity[5];
    u8  channel;
    u16 interval_slots;
    u8  llid;
    u8  flow;
    u8  edr;
    u8  whiten_seed;     /* written to the whitening control; the body byte
                          * that reaches this function must already be
                          * pre-cancelled with the same seed */
};

int wake_radio_start(const struct wake_radio_config *config,
                     const u8 *body, u16 length);

void wake_radio_stop(void);

#endif
