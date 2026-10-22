#include <system/includes.h>
#include <asm/power_interface.h>
#include "app_config.h"
#include "board.h"

/* GameSir Nova 2 Lite (PID AC695X_TRANS): a BR23 pad with Hall sticks and
 * triggers, two motors, no IMU.  Its pins are not known yet, so this board
 * drives none: no buttons, sticks, LEDs or motors, and USB taken as present.
 * From the stock image (docs/gamesir-nova-lite-2/): ISD log UART TX on PA5,
 * long-press reset on PB11. */

/* No lines yet: the platform reads none (board_line_count 0). */
const struct line board_lines[] = { { 0, 0 } };
const u8 board_line_count = 0;
const u32 board_keep_pins[] = { 0 };
const u8 board_keep_count = 0;
const struct wakeup_param *const board_wakeups = NULL;
const char board_name[] = "GameSir Nova 2 Lite pad";
const u8 board_audio = 0;
const u32 board_amp_enable = 0;

/* ---- boot: the way back from a bad build ------------------------------------
 * Without a known button combination, two boots in a row that do not run
 * BOOT_HEALTHY_MS (a hang-watch or watchdog reset, a crash) go to the ROM
 * loader on the third.  The count is in retained RAM, so a power cut starts
 * it over. */

#define BOOT_MAGIC      0x4e4c3242      /* "NL2B" */
#define BOOT_FAILS_MAX  2
#define BOOT_HEALTHY_MS 10000

/* pinprobe.c: logs what the pins do, until they are known */
void probe_start(void);
void probe_tick(void);
void probe_report(void);

static struct { u32 magic, fails; } guard RETAINED;
static struct plat_boot boot;

void board_latch(void)
{
    if (guard.magic != BOOT_MAGIC) {
        guard.magic = BOOT_MAGIC;
        guard.fails = 0;
    }
    if (guard.fails >= BOOT_FAILS_MAX) {
        guard.fails = 0;
        plat_flush_dcache(&guard, sizeof(guard));
        plat_enter_loader();
    }
    guard.fails++;
    plat_flush_dcache(&guard, sizeof(guard));
    boot.cause = BOOT_USB;
}

void plat_boot_info(struct plat_boot *out)
{
    *out = boot;
}

void board_start(void)
{
    probe_start();
}

HOT void board_tick(void)
{
    if (guard.fails && plat_now_ms >= BOOT_HEALTHY_MS) {
        guard.fails = 0;
        plat_flush_dcache(&guard, sizeof(guard));
    }
    probe_tick();
}

void board_to_loader(void)
{
}

/* No sense pin known: USB is taken as there, as on the dongle. */
int board_usb_sense(void)
{
    return 1;
}

int board_jack(void)
{
    return 0;
}

void plat_input(struct in_raw *raw)
{
    u32 changed_us;

    memset(raw, 0, sizeof(*raw));
    plat_lines(&changed_us);
    raw->changed_us = changed_us;
    probe_report();
}

void plat_leds(const struct led_frame *f)
{
    (void)f;
}

void plat_motors(u8 left, u8 right)
{
    (void)left; (void)right;
}

/* No wake-up source is set up yet, so soft power-off could not be left.
 * With USB taken as present the controller does not get here; restart if
 * it does. */
void plat_power_off(void)
{
    plat_reboot();
}

/* No IMU: input_imu.c links for the profiles' motion maths, and its I2C
 * reads fail. */
void plat_i2c_read(u8 dev, u8 reg, u8 *buf, u8 len)
{
    (void)dev; (void)reg; (void)buf; (void)len;
}

void plat_i2c_write(u8 dev, u8 reg, u8 value)
{
    (void)dev; (void)reg; (void)value;
}

int plat_i2c_done(void)
{
    return -1;
}
