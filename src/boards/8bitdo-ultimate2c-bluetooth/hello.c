#include <string.h>
#include "pad.h"
#include "yc3121.h"
#include "usb_yc3121.h"
#include "radio_yc3121.h"

/* 8BitDo Ultimate 2C Bluetooth (YC3121) bring-up: own code on the pad, and
 * USB as the tool device (padtool identify, status, log, peek, reboot,
 * loader: the boot loader's mailbox keeps it there), and the Bluetooth core
 * by two bring-up commands (0x3a start, 0x3b send); what it sends back goes
 * to the log.
 *
 * Lights: with no button down and USB seen, they show its progress: light 1
 * a bus reset, 2 an address set, 3 configured, 4 toggling with each tool
 * command; with USB quiet, the chase.  While a button is down they all light
 * (Home: the outer two).  Any button held 3 s lets go of the power hold,
 * then resets the chip (on USB power the pad stays on: the boot loader
 * starts this again, or stays when L1+R1 are held).
 *
 * Pins as stock v1.01 drives them, each proven here on 2026-10-31:
 *   power hold  0.7 high keeps the pad on (stock drops it to switch off)
 *   lights      1.11, 1.6, 0.15, 2.3, high = lit (its player-light chase)
 *   matrix      rows 1.5, 0.6, 0.1, 2.4 driven low in turn; columns
 *               0.9..0.13 with pull-ups, low = down
 *   Home        1.10 with pull-up, low = down; 2.11 and 2.12 likewise
 * The loader leaves the CPU at 48 MHz (HCLK_CON divider 2, seen on the
 * lights); a busy loop runs about five times slower than its cycle count
 * from flash, so time comes from SysTick (platform_yc3121.c). */

const char board_name[] = "Ultimate 2C Bluetooth";

struct pin { uint8_t group, pin; };

static const struct pin lights[4] = { { 1, 11 }, { 1, 6 }, { 0, 15 }, { 2, 3 } };
static const struct pin rows[4] = { { 1, 5 }, { 0, 6 }, { 0, 1 }, { 2, 4 } };
static const struct pin keys[3] = { { 1, 10 }, { 2, 11 }, { 2, 12 } };   /* Home first */
#define COLUMNS_MASK 0x3e00             /* 0.9..0.13 */
#define POWER_GROUP 0
#define POWER_PIN 7

void plat_clock_init(void);
void plat_log_fault(void);

static u8 reboot_pending, radio_start_pending;
static struct radio_packet packet;

static void show(unsigned mask)
{
    for (int i = 0; i < 4; i++)
        yc_pin(lights[i].group, lights[i].pin, mask >> i & 1 ? YC_PIN_HIGH : YC_PIN_LOW);
}

/* 0 none, 1 Home, 2 anything else. */
static int scan(void)
{
    int down = 0;
    for (int r = 0; r < 4; r++) {
        yc_pin(rows[r].group, rows[r].pin, YC_PIN_LOW);
        for (u32 t0 = plat_us(); plat_us() - t0 < 5;)
            ;
        if (~YC_GPIO_IN(0) & COLUMNS_MASK)
            down = 2;
        yc_pin(rows[r].group, rows[r].pin, YC_PIN_HIGH);
    }
    for (int k = 2; k > 0; k--)
        if (!yc_pin_in(keys[k].group, keys[k].pin))
            down = 2;
    if (!down && !yc_pin_in(keys[0].group, keys[0].pin))
        down = 1;
    return down;
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24);
}

/* usb.c's commands, the few this build has. */
u32 usb_tool_command(const u8 *report, u8 *data, u32 n, u32 max)
{
    const u8 *cdb = report;
    u32 arg = (u32)(cdb[2] | cdb[3] << 8 | cdb[4] << 16 | (u32)cdb[5] << 24);
    u32 want = (u32)(report[16] | report[17] << 8);

    if (cdb[0] != 0xf5) return 0;
    switch (cdb[1]) {
    case 0x00:                           /* IDENTIFY */
        if (max < 88) return 0;
        memset(data, 0, 88);
        memcpy(data, "C2PD", 4);
        data[4] = 2;                     /* protocol */
        strncpy((char *)data + 8, BUILD_ID, 16);
        strncpy((char *)data + 40, BOARD_PID, 16);
        strncpy((char *)data + 56, board_name, 32);
        return 88;
    case 0x01:                           /* STATUS */
        if (max < 16) return 0;
        memset(data, 0, 16);
        data[1] = 0xff;                  /* no slot */
        data[2] = 1;                     /* on USB */
        put32(data + 8, plat_ms());
        return 16;
    case 0x02: {                         /* LOG */
        u32 seq = arg, k;

        if (max < 8) return 0;
        k = util_log_read(&seq, (char *)data + 8, max - 8);
        put32(data, seq);
        put32(data + 4, k);
        return k + 8;
    }
    case 0x15: {                         /* PEEK: words from the address */
        u32 k = (want < max ? want : max) & ~3u;

        for (u32 i = 0; i < k; i += 4)
            put32(data + i, *(volatile u32 *)(arg + i));
        return k;
    }
    case 0x3a:                           /* bring-up: start the Bluetooth core */
        radio_start_pending = 1;
        return 0;
    case 0x3b:                           /* bring-up: data {id, wide, payload} to the core */
        if (n < 2) return 0;
        data[0] = (u8)plat_radio_send(data[0], data + 2, (u16)(n - 2), data[1]);
        return 1;
    case 0x10:                           /* REBOOT */
        reboot_pending = 1;
        return 0;
    case 0x11:                           /* REBOOT_LOADER */
        reboot_pending = 2;
        return 0;
    }
    return 0;
}

int main(void)
{
    plat_clock_init();
    yc_pin(POWER_GROUP, POWER_PIN, YC_PIN_HIGH);
    show(0);
    for (int r = 0; r < 4; r++)
        yc_pin(rows[r].group, rows[r].pin, YC_PIN_HIGH);
    for (int c = 9; c <= 13; c++)
        yc_pin(0, c, YC_PIN_PULLUP);
    for (int k = 0; k < 3; k++)
        yc_pin(keys[k].group, keys[k].pin, YC_PIN_PULLUP);
    util_log_init();
    util_log("bring-up %s, HCLK_CON %08x", BUILD_ID, (unsigned)YC_HCLK_CON);
    plat_log_fault();
    plat_usb_init();

    const struct usb_events *ev = plat_usb_events();
    u32 resets = 0, odd = 0, stalls = 0;
    u8 configured = 0;
    u32 tick_ms = plat_ms(), held = 0, tick = 0, reboot_at = 0;

    for (;;) {
        plat_usb_poll();
        if (ev->resets != resets) util_log("usb: bus reset (%u)", (unsigned)(resets = ev->resets));
        if (ev->configured != configured) util_log("usb: configured %u", configured = ev->configured);
        if (ev->odd_setups != odd) util_log("usb: SETUP not 8 bytes (%u)", (unsigned)(odd = ev->odd_setups));
        if (ev->stalls != stalls) {
            const u8 *q = ev->stalled;
            util_log("usb: stall %u: %02x %02x %02x%02x %02x%02x %02x%02x", (unsigned)(stalls = ev->stalls),
                     q[0], q[1], q[3], q[2], q[5], q[4], q[7], q[6]);
        }
        if (radio_start_pending) {
            radio_start_pending = 0;
            util_log("bt: start");
            util_log("bt: started %d", plat_radio_start());
        }
        while (plat_radio_take(&packet)) {
            char hex[3 * 24 + 1];
            int k = packet.len < 24 ? packet.len : 24;
            for (int i = 0; i < k; i++) {
                static const char digits[] = "0123456789abcdef";
                hex[3 * i] = digits[packet.data[i] >> 4];
                hex[3 * i + 1] = digits[packet.data[i] & 15];
                hex[3 * i + 2] = ' ';
            }
            hex[3 * k] = 0;
            util_log("bt: %02x %02x [%u] %s", packet.type, packet.id, packet.len, hex);
        }
        if (reboot_pending && !reboot_at) reboot_at = plat_ms() + 50;   /* the answer goes out first */
        if (reboot_at && (int)(plat_ms() - reboot_at) >= 0) plat_restart(reboot_pending == 2);
        if (plat_ms() - tick_ms < 10)
            continue;
        tick_ms += 10;
        tick++;
        int down = scan();
        held = down ? held + 1 : 0;
        if (held >= 300)
            break;
        if (down)
            show(down == 1 ? 0x9 : 0xf);
        else if (ev->resets)
            show(1u | ev->addressed << 1 | ev->configured << 2 | (ev->commands & 1) << 3);
        else
            show(1u << (tick / 25 % 4));
    }
    show(0);
    yc_pin(POWER_GROUP, POWER_PIN, YC_PIN_LOW);
    plat_delay_ms(1000);
    yc_reset();
}
