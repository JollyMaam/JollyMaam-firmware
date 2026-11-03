#ifndef YC_BOARD_H
#define YC_BOARD_H

#include "pad.h"

/* Between the YC3121 platform (main_yc3121.c: the chip, time, the wait
 * loop; bt_yc3121.c; usb_yc3121.c) and the board file linked with it
 * (board_pad.c).  Not seen by the rest of the firmware. */

/* ---- the board ---------------------------------------------------------- */

void board_init(void);           /* pins, the power hold, before USB */
/* From the platform's wait loop: the key matrix and the sticks, each in
 * one go when due.  EV_BUTTONS when the buttons changed, EV_STICKS when the
 * sticks moved. */
u32 board_poll(void);
void board_input(struct in_raw *raw);
void board_battery(struct battery_status *out);
int board_usb_present(void);     /* USB power on the connector */
void board_power_off(void);      /* outputs off, then the power hold released */
u32 board_held(void);            /* PAD_* down now, scanned at once */

/* ---- the platform, for the board and between its files ------------------ */

void plat_bt_init(void);         /* bt_yc3121.c */
u32 plat_bt_poll(void);          /* from the wait loop: EV_BT for bt_poll() */
/* The ADC, while the radio core is up (radio_yc3121.c). */
u16 plat_adc_read(int channel);
int plat_adc_ready(void);

#endif
