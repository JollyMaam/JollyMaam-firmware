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
/* Between the updater (src/updater/updater_yc3121.c) and its app, first in
 * the RAM kept over a restart through the boot loader (app.ld): the app's
 * base while it runs (start.S passes the entries on to it), and stay:
 * UPDATER_STAY asks the updater to stay, UPDATER_TRIES + n counts n starts
 * of the app that never reached its loop (the updater stays after
 * UPDATER_TRIES_MAX), 0 once the app's loop runs. */
struct yc_handover {
    volatile u32 app, stay;
};
extern struct yc_handover yc_handover;
#define UPDATER_STAY      0x54445055u    /* "UPDT" */
#define UPDATER_TRIES     0x54525900u    /* "\0YRT" */
#define UPDATER_TRIES_MAX 3
void updater_boot(void);         /* first in the updater's main(): may start the app */

/* The ADC, while the radio core is up (radio_yc3121.c). */
u16 plat_adc_read(int channel);
int plat_adc_ready(void);

#endif
