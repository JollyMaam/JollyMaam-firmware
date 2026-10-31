#include <string.h>
#include "pad.h"
#include "yc3121.h"
#include "usb_yc3121.h"
#include "radio_yc3121.h"
#include "sdp_yc3121.h"

/* 8BitDo Ultimate 2C Bluetooth (YC3121) bring-up: own code on the pad, and
 * USB as the tool device (padtool identify, status, log, peek, reboot,
 * loader: the boot loader's mailbox keeps it there), and the Bluetooth core
 * by bring-up commands (0x3a start, 0x3b send, 0x3c up as a generic HID
 * gamepad); what it sends back goes to the log.  While a host has HID
 * connected, the raw key matrix goes out as that gamepad's buttons (key k
 * button k + 1, keys 16 on in the Z axis byte); key changes are logged.
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

static u8 reboot_pending, radio_start_pending, bt_up_pending, hid_connected;

/* The core's bond table (event 0x0d, 5 records of 34 bytes), given back at
 * start (command 0x26) as stock does; kept over a restart, not yet in flash. */
static struct {
    u32 magic;
    u8 table[170];
} bonds __attribute__((section(".noinit")));
#define BONDS_MAGIC 0x53444e42
static struct radio_packet packet;

/* profile_hid.c's generic gamepad, report 1: 16 buttons, hat, 6 axes. */
static const u8 generic_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x01,
    0x05, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x06,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34, 0x09, 0x32, 0x09, 0x35, 0x81, 0x02,
    0xc0,
};

static void show(unsigned mask)
{
    for (int i = 0; i < 4; i++)
        yc_pin(lights[i].group, lights[i].pin, mask >> i & 1 ? YC_PIN_HIGH : YC_PIN_LOW);
}

/* The keys down: bit 5 * row + column for the matrix, 20 and 21 for 2.11
 * and 2.12, 22 Home. */
static u32 scan(void)
{
    u32 down = 0;
    for (int r = 0; r < 4; r++) {
        yc_pin(rows[r].group, rows[r].pin, YC_PIN_LOW);
        for (u32 t0 = plat_us(); plat_us() - t0 < 5;)
            ;
        down |= (u32)((~YC_GPIO_IN(0) & COLUMNS_MASK) >> 9) << 5 * r;
        yc_pin(rows[r].group, rows[r].pin, YC_PIN_HIGH);
    }
    for (int k = 0; k < 3; k++)
        if (!yc_pin_in(keys[k].group, keys[k].pin))
            down |= 1u << (k ? 19 + k : 22);
    return down;
}

static void log_packet(const struct radio_packet *p)
{
    static const char digits[] = "0123456789abcdef";
    char hex[3 * 24 + 1];
    int k = p->len < 24 ? p->len : 24;

    for (int i = 0; i < k; i++) {
        hex[3 * i] = digits[p->data[i] >> 4];
        hex[3 * i + 1] = digits[p->data[i] & 15];
        hex[3 * i + 2] = ' ';
    }
    hex[3 * k] = 0;
    util_log("bt: %02x %02x [%u] %s", p->type, p->id, p->len, hex);
    if (p->id == 0x0d && p->len == sizeof(bonds.table)) {
        memcpy(bonds.table, p->data, sizeof(bonds.table));
        bonds.magic = BONDS_MAGIC;
    }
    if (p->id == 0xb0) hid_connected = 1;
    if (p->id == 0xb6) hid_connected = 0;
}

/* A command, then its acknowledgement: event 0x06 {id, status} (0x29 for
 * 0x77 and 0x78), within 500 ms; USB stays served meanwhile. */
static int bt_command(u8 id, const u8 *data, u16 len, int wide)
{
    u8 ack = id == 0x77 || id == 0x78 ? 0x29 : 0x06;

    if (!plat_radio_send(id, data, len, wide)) {
        util_log("bt: %02x: no room", id);
        return 0;
    }
    for (u32 t0 = plat_ms(); plat_ms() - t0 < 500;) {
        plat_usb_poll();
        if (!plat_radio_take(&packet)) continue;
        log_packet(&packet);
        if (packet.id == ack && (ack == 0x29 || packet.data[0] == id))
            return ack == 0x29 || packet.data[1] == 0;
    }
    util_log("bt: %02x: no answer", id);
    return 0;
}

/* Up as a generic gamepad, discoverable and connectable: stock v1.01's
 * start-up commands (meanings in the analysis notes), our name and SDP. */
static void bt_up(void)
{
    static const u8 address[6] = { 0x01, 0x02, 0x03, 0xd8, 0x17, 0xe4 };   /* bring-up only */
    static const u8 cmd04[14] = { '8', '0', 'K', 'A' };
    static const u8 cod[3] = { 0x08, 0x25, 0x00 };
    static const u8 on = 1, visible = 3, x42 = 2, x39[2] = { 200, 0 }, x79[2] = { 0, 4 };
    static const u8 service[17] = { 0x10, 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0, 0, 0x80, 0, 0x10, 0, 0, 0x10, 0xff, 0, 0 };
    static const u8 characteristic[20] = { 0x18, 0x10, 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0, 0, 0x80, 0, 0x10, 0, 0, 0x13, 0xff, 0, 0, 1, 0 };
    static u8 records[512];
    const struct yc_sdp_hid hid = {
        .name = board_name, .descriptor = generic_descriptor, .descriptor_len = sizeof(generic_descriptor),
        .subclass = 0x08, .vendor = USB_OWN_VID, .product = USB_OWN_JOYSTICK_PID, .version = 0x0100,
    };
    u16 n = yc_sdp_records(records, sizeof(records), &hid);
    int ok;

    util_log("bt: up, SDP %u bytes", n);
    ok = plat_radio_start() == 0;
    while (ok && plat_radio_take(&packet)) log_packet(&packet);
    ok = ok && bt_command(0x04, cmd04, sizeof(cmd04), 0);
    ok = ok && bt_command(0x01, address, 6, 0);
    ok = ok && bt_command(0x00, address, 6, 0);
    ok = ok && bt_command(0x0c, &on, 1, 0);
    ok = ok && bt_command(0x15, cod, 3, 0);
    if (bonds.magic != BONDS_MAGIC) memset(bonds.table, 0, sizeof(bonds.table));
    ok = ok && bt_command(0x26, bonds.table, sizeof(bonds.table), 0);
    ok = ok && bt_command(0x76, NULL, 0, 0);
    ok = ok && bt_command(0x77, service, sizeof(service), 0);
    ok = ok && bt_command(0x78, characteristic, sizeof(characteristic), 0);
    ok = ok && bt_command(0x42, &x42, 1, 0);
    ok = ok && bt_command(0x39, x39, 2, 0);
    ok = ok && bt_command(0x79, x79, 2, 0);
    ok = ok && bt_command(0xa0, records, n, 1);
    ok = ok && bt_command(0x03, (const u8 *)board_name, (u16)strlen(board_name), 0);
    ok = ok && bt_command(0x02, &visible, 1, 0);
    util_log("bt: up %s", ok ? "ok" : "failed");
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
    case 0x3c:                           /* bring-up: Bluetooth up as a generic gamepad */
        bt_up_pending = 1;
        return 0;
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
    u32 tick_ms = plat_ms(), held = 0, tick = 0, reboot_at = 0, last_keys = 0, sent_keys = ~0u;

    for (;;) {
        plat_usb_poll();
        if (ev->resets != resets) util_log("usb: bus reset (%u)", (unsigned)(resets = ev->resets));
        if (ev->configured != configured) util_log("usb: configured %u", configured = ev->configured);
        if (ev->odd_setups != odd) util_log("usb: SETUP of %u bytes (%u)", ev->odd_length, (unsigned)(odd = ev->odd_setups));
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
        if (bt_up_pending) {
            bt_up_pending = 0;
            bt_up();
        }
        while (plat_radio_take(&packet))
            log_packet(&packet);
        if (reboot_pending && !reboot_at) reboot_at = plat_ms() + 50;   /* the answer goes out first */
        if (reboot_at && (int)(plat_ms() - reboot_at) >= 0) plat_restart(reboot_pending == 2);
        if (plat_ms() - tick_ms < 10)
            continue;
        tick_ms += 10;
        tick++;
        u32 keys_down = scan();
        int down = !keys_down ? 0 : keys_down == 1u << 22 ? 1 : 2;
        if (keys_down != last_keys) util_log("keys %06x", (unsigned)(last_keys = keys_down));
        if (hid_connected && keys_down != sent_keys) {
            u8 report[11] = { 0xa1, 0x01, (u8)keys_down, (u8)(keys_down >> 8), 8, 0x80, 0x80, 0x80, 0x80,
                              (u8)(keys_down >> 16), 0 };
            if (plat_radio_send(0xb0, report, sizeof(report), 1)) sent_keys = keys_down;
        }
        if (!hid_connected) sent_keys = ~0u;
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
