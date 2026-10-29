#ifndef BOARD_H
#define BOARD_H

/* Between the Telink platform (platform_telink*.c: the chip, the SDK, USB,
 * time) and the board file linked with it (board_pad.c).  Not seen by the
 * rest of the firmware.  Include after sdk.h. */

/* ---- the board ---------------------------------------------------------- */

void board_init(void);           /* pins and the input scanner, before USB */
/* The input scanner, from the platform's wait loop: the matrix and the
 * sticks each in one go when due.  Returns EV_BUTTONS when the buttons
 * changed, EV_STICKS when the sticks moved.  sticks_on_demand: reports are
 * timed to the connection events and read the sticks for themselves
 * (board_sweep), the scanner only now and then. */
u32 board_poll(int sticks_on_demand);
void board_sweep(u32 max_age_us);       /* the sticks now, unless read within max_age_us */
void board_input(struct in_raw *raw);
int board_usb_present(void);    /* physical USB power, independent of enumeration */
void board_power_off(void);     /* outputs off, then release the battery power hold */

/* ---- the platform, for the board and between its files ------------------ */

void plat_usb_init(void);                /* platform_telink_usb.c */
u32 plat_usb_poll(void);                 /* from the wait loop: EV_USB when a command waits */
void plat_usb_settle(u32 ms);            /* answer the host for ms, nothing else: before a reset */
int plat_usb_configured(void);
void plat_bt_init(void);                 /* bt_telink.c: the stack, before board_init() */
/* bt_telink.c: reports are built a lead before the connection event they
 * are for (plat_bt_jit: while events come), JIT_LEAD_US at first, then as
 * late and early ones show; plat_bt_systimer() from the interrupt handler
 * keeps the next event's start. */
#ifndef JIT_LEAD_US
#define JIT_LEAD_US 3700
#endif
u32 plat_bt_jit(void);                   /* the lead now, us; 0: reports go when there is room */
int plat_bt_input_window(void);          /* a report would be wanted now (no side effects) */
void plat_bt_systimer(u32 fired);

/* The updater (src/updater/updater_telink.c) and analog register 0x3c,
 * kept through a reset: UPDATER_STAY asks it to stay rather than start the
 * app; UPDATER_STAY + n counts n starts of the app that never came up (the
 * updater stays after UPDATER_TRIES_MAX); UPDATER_UP, its power-on value,
 * is the app up.  The 8BitDo boot loader acts on 0x5a and 0x5b there. */
#define UPDATER_STAY_REG  0x3c
#define UPDATER_STAY      0xa0
#define UPDATER_TRIES_MAX 3
#define UPDATER_UP        0x0f
void updater_boot(void);                 /* first in main(): may start the app instead */
u32 plat_bt_poll(void);                  /* runs the stack; EV_BT for bt_poll() */

#endif
