#ifndef BOARD_H
#define BOARD_H

/* Between the Telink platform (platform_telink*.c: the chip, the SDK, USB,
 * time) and the board file linked with it (board_pad.c).  Not seen by the
 * rest of the firmware.  Include after sdk.h. */

/* ---- the board ---------------------------------------------------------- */

void board_init(void);           /* pins and the input scanner, before USB */
/* One step of the input scanner, from the platform's wait loop; it never
 * waits itself.  Returns EV_BUTTONS when the buttons changed or a full
 * set of sticks and triggers was read. */
u32 board_poll(void);
void board_input(struct in_raw *raw);

/* ---- the platform, for the board and between its files ------------------ */

void plat_usb_init(void);                /* platform_telink_usb.c */
u32 plat_usb_poll(void);                 /* from the wait loop: EV_USB when a command waits */
void plat_usb_settle(u32 ms);            /* answer the host for ms, nothing else: before a reset */
int plat_usb_configured(void);

#endif
