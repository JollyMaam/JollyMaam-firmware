#ifndef RADIO_YC3121_H
#define RADIO_YC3121_H

#include "pad.h"

/* A packet from the Bluetooth core: type 2, an event id, the data. */
struct radio_packet {
    u8 type, id, len;
    u8 data[255];
};

/* Load and start the core: 0, or < 0 (-1 it never signalled, -2 no blob). */
int plat_radio_start(void);
/* A command to the core, wide: with a u16 length; 0: no room in its ring. */
int plat_radio_send(u8 id, const u8 *data, u16 len, int wide);
/* The core's next packet; 0: none. */
int plat_radio_take(struct radio_packet *p);
/* The ADC, after plat_radio_start: pins as stock, then a channel's value
 * (0..0x7fe, even), 100 us each. */
void plat_adc_init(void);
u16 plat_adc_read(int channel);

#endif
