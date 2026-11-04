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
/* Bring-up: load a blob (radio_blob's format) and keep the core held (same
 * results as plat_radio_start); plat_radio_release() then starts it. */
int plat_radio_load(const u8 *blob, u32 len);
void plat_radio_release(void);
/* Bring-up: n bytes from the code port, the control bytes set first. */
void plat_radio_code_read(u8 c2, u8 c3, u8 c4, u8 *out, u32 n);
extern const u8 radio_blob[], radio_blob_end[];
/* A command to the core, wide: with a u16 length; 0: no room in its ring. */
int plat_radio_send(u8 id, const u8 *data, u16 len, int wide);
/* The core's next packet; 0: none. */
int plat_radio_take(struct radio_packet *p);
/* The ADC, after plat_radio_start: pins as stock, then a channel's value
 * (0..0x7fe, even), 100 us each. */
void plat_adc_init(void);
u16 plat_adc_read(int channel);
int plat_adc_ready(void);        /* plat_adc_init() since the core last started */

#endif
