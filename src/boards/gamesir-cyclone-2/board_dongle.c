#include <system/includes.h>
#include "app_config.h"
#include "board.h"

/* The GameSir dongle (PID GS_C2_Dongle), on USB power only.  From the stock
 * dongle 1.21: one button on PA0, active low with a pull-up ("key scan port
 * init"); a LED on PD3 or PA13, by board revision (not driven here). */

#define PIN_BUTTON IO_PORTA_00

const struct line board_lines[] = { { PIN_BUTTON, 1 } };
const u8 board_line_count = 1;
const u32 board_keep_pins[] = { PIN_BUTTON };
const u8 board_keep_count = 1;
const struct wakeup_param *const board_wakeups = NULL;
const char board_name[] = "Cyclone 2 Dongle";
const u8 board_audio = 0;
const u32 board_amp_enable = 0;

int board_jack(void)
{
    return 0;
}

static struct plat_boot boot;

/* The button held at plug-in goes to the ROM loader before any of this
 * firmware can hang: the way back from a bad build. */
void board_latch(void)
{
    plat_pull_up_input(PIN_BUTTON);
    plat_mdelay(2);              /* the pull-up charging the pin */
    if (plat_read_lines() & 1) plat_enter_loader();
    boot.cause = BOOT_USB;
}

void plat_boot_info(struct plat_boot *out)
{
    *out = boot;
}

void board_start(void)
{
    plat_pull_up_input(PIN_BUTTON);
}

HOT void board_tick(void)
{
}

void board_to_loader(void)
{
}

int board_usb_sense(void)
{
    return 1;
}

/* No motion sensor. */
u32 imu_poll(u32 now, u8 mode)
{
    (void)now; (void)mode;
    return 0;
}
