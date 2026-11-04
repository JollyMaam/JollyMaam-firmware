#include <string.h>
#include "pad.h"
#include "yc3121.h"
#include "usb_yc3121.h"
#include "radio_yc3121.h"
#include "sdp_yc3121.h"
#include "flash_yc3121.h"

/* 8BitDo Ultimate 2C Bluetooth (YC3121) bring-up: own code on the pad, and
 * USB as the tool device (padtool identify, status, log, peek, reboot,
 * loader: the boot loader's mailbox keeps it there), and the Bluetooth core
 * by bring-up commands (0x3a start, 0x3b send, 0x3c up as a generic HID
 * gamepad); what it sends back goes to the log.  While a host has HID
 * connected, the raw key matrix goes out as that gamepad's buttons (key k
 * button k + 1, keys 16 on in the Z axis byte) and ADC channels 0, 1, 4, 5
 * as its X, Y, Rx, Ry (the ADC starts with the core; 0x3f reads channels
 * 0..5 raw); key changes are logged.
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
 *   Home        1.10 with pull-up, low = down; 2.11 (L2) and 2.12 (R2) likewise
 *   keys        row 0: up, X, Plus, R4, L4; row 1: left, Y, Star, R1, L1;
 *               row 2: down, A, Minus, R3, L3; row 3: right, B, Capture
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
u32 plat_log_fault(void);

static volatile u8 reboot_pending, radio_start_pending, bt_up_pending, raw_pending;
/* Bring-up flash commands for the main loop: 1 erase, 2 write. */
static volatile u8 flash_pending, bonds_send_pending;
static u32 flash_at, flash_len;
static u8 flash_data[64];
static u8 hid_connected, adc_ready;
static u32 inputs_refused, inputs_refused_logged;
/* Bring-up: stock's inputs of unknown role (0.8 pulled up, 0.14 and 1.12
 * floating), each change logged. */
static const struct pin probes[3] = { { 0, 8 }, { 0, 14 }, { 1, 12 } };
static u8 probe_levels = 0xff;   /* acks of 0xb0 with an error */
/* ADC channels 0..5, sampled each tick once the core is up (0x3f reads
 * them): stock's sticks on 0, 1, 4, 5, its battery on 3. */
static volatile u16 adc[6];
/* The sticks (proven 2026-11-03 by moving each): left X on channel 4, left Y
 * on 5, right X on 0, right Y on 1; raw values fall to the right on X, and
 * downward on left Y but upward on right Y.  Rest near 1390, travel about
 * +-270 of 0..2046.  Bring-up scaling: the centre is the first sample,
 * +-STICK_TRAVEL is full scale; as X, Y, Rx, Ry (X right, Y down). */
static const u8 stick_channel[4] = { 4, 5, 0, 1 };
static const int stick_sign[4] = { -1, -1, -1, 1 };
#define STICK_TRAVEL 260
static int centre[4];
static u8 raw[256];              /* 0x3b's packet, for the main loop */
/* Bring-up of the core's code (0x46..0x48): a RAM copy of the blob to edit,
 * and 0x47's request for the main loop (1 load stock's blob and hold the
 * core, 2 load the copy and hold, 3 release). */
static u8 blob_copy[8192];
static u32 blob_copy_len;
static volatile u8 core_load_pending;
static u16 raw_len;

/* The core's bond table (event 0x0d, 5 records of 34 bytes), given back at
 * start (command 0x26) as stock does; in the settings store (core/store.c)
 * under a bring-up key until the slots keep bonds, and in RAM over a restart. */
#define KEY_BRINGUP_BONDS 60
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
    char hex[3 * 16 + 1];

    if (p->id == 0x06 && p->data[0] == 0xb0) {               /* input reports' acks: counted */
        if (p->data[1]) inputs_refused++;
        goto logged;
    }
    for (int o = 0; o == 0 || (o < p->len && o < 64 && p->id != 0x0d); o += 16) {   /* 16 a line */
        int k = p->len - o < 16 ? p->len - o : 16;

        for (int i = 0; i < k; i++) {
            hex[3 * i] = digits[p->data[o + i] >> 4];
            hex[3 * i + 1] = digits[p->data[o + i] & 15];
            hex[3 * i + 2] = ' ';
        }
        hex[3 * (k > 0 ? k : 0)] = 0;
        if (!o) util_log("bt: %02x %02x [%u] %s", p->type, p->id, p->len, hex);
        else util_log("bt:   +%u %s", o, hex);
    }
logged:
    if (p->id == 0x0d && p->len == sizeof(bonds.table)) {
        u32 t0 = plat_us();
        int n;

        memcpy(bonds.table, p->data, sizeof(bonds.table));
        bonds.magic = BONDS_MAGIC;
        n = plat_store_write(KEY_BRINGUP_BONDS, bonds.table, sizeof(bonds.table));
        util_log("bonds: stored %d in %u us", n, (unsigned)(plat_us() - t0));
    }
    if (p->id == 0xb0) hid_connected = 1;
    if (p->id == 0xb6) hid_connected = 0;
}

/* A command, then its acknowledgement: event 0x06 {id, status} (0x29 for
 * 0x77 and 0x78), within 500 ms. */
static int bt_command(u8 id, const u8 *data, u16 len, int wide)
{
    u8 ack = id == 0x77 || id == 0x78 ? 0x29 : 0x06;

    if (!plat_radio_send(id, data, len, wide)) {
        util_log("bt: %02x: no room", id);
        return 0;
    }
    for (u32 t0 = plat_ms(); plat_ms() - t0 < 500;) {
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
    if (ok) plat_adc_init(), adc_ready = 1;
    while (ok && plat_radio_take(&packet)) log_packet(&packet);
    ok = ok && bt_command(0x04, cmd04, sizeof(cmd04), 0);
    ok = ok && bt_command(0x01, address, 6, 0);
    ok = ok && bt_command(0x00, address, 6, 0);
    ok = ok && bt_command(0x0c, &on, 1, 0);
    ok = ok && bt_command(0x15, cod, 3, 0);
    if (plat_store_read(KEY_BRINGUP_BONDS, bonds.table, sizeof(bonds.table)) == sizeof(bonds.table))
        bonds.magic = BONDS_MAGIC, util_log("bonds: from the store");
    else if (bonds.magic != BONDS_MAGIC)
        memset(bonds.table, 0, sizeof(bonds.table));
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
    case 0x3d: {                         /* bring-up: bytes from the address (the radio window) */
        u32 k = want < max ? want : max;

        for (u32 i = 0; i < k; i++)
            data[i] = *(volatile u8 *)(arg + i);
        return k;
    }
    case 0x45:                           /* bring-up: bytes into the radio window's RAM */
        if (arg < 0xc0000 || arg + n > 0xc5000) return 0;
        for (u32 i = 0; i < n; i++)
            *(volatile u8 *)(arg + i) = data[i];
        return 0;
    case 0x3e: {                         /* bring-up: the USB counters */
        const struct usb_events *e = plat_usb_events();
        u32 v[] = { e->resets, e->setups, e->odd_setups, e->stalls, e->commands,
                    e->set_reports, e->get_reports, e->out_packets, e->out_idle, e->out_last,
                    e->incomplete, e->incomplete_got, e->incomplete_want, e->loop_max_us };

        if (max < sizeof(v)) return 0;
        for (u32 i = 0; i < sizeof(v) / 4; i++) put32(data + 4 * i, v[i]);
        return sizeof(v);
    }
    case 0x39: {                         /* bring-up: the EP0 trace, oldest first */
        u32 k = sizeof(usb_trace) < max ? sizeof(usb_trace) : max;

        memcpy(data, &usb_trace, k);
        if (arg == 1) usb_trace.stop = 0, usb_trace.next = 0;   /* argument 1: and rearm */
        return k;
    }
    case 0x3f:                           /* bring-up: ADC channels 0..5 (u16 each) */
        if (max < 12) return 0;
        for (int i = 0; i < 6; i++) data[2 * i] = (u8)adc[i], data[2 * i + 1] = (u8)(adc[i] >> 8);
        return 12;
    case 0x40:                           /* bring-up: raw flash of the data pages */
        if (!want || want > max || !plat_flash_read(arg, data, want)) return 0;
        return want;
    case 0x41:                           /* bring-up: erase a data page's sector */
    case 0x42:                           /* bring-up: write bytes there */
        if (flash_pending || n > sizeof(flash_data)) return 0;
        memcpy(flash_data, data, n);
        flash_at = arg, flash_len = n;
        flash_pending = cdb[1] == 0x41 ? 1 : 2;
        return 0;
    case 0x43:                           /* bring-up: bytes into the bond table at offset arg */
        if (arg > sizeof(bonds.table) || n > sizeof(bonds.table) - arg) return 0;
        memcpy(bonds.table + arg, data, n);
        bonds.magic = BONDS_MAGIC;
        return 0;
    case 0x44:                           /* bring-up: the bond table to the running core (0x26) */
        bonds_send_pending = 1;
        return 0;
    case 0x46: {                         /* bring-up: bytes from the core's code port */
        u32 k = want < max ? want : max;

        plat_radio_code_read((u8)arg, (u8)(arg >> 8), (u8)(arg >> 16), data, k);
        return k;
    }
    case 0x47:                           /* bring-up: 1 load and hold, 2 load the copy and hold, 3 release */
        if (arg < 1 || arg > 3 || core_load_pending) return 0;
        core_load_pending = (u8)arg;
        return 0;
    case 0x48:                           /* bring-up: bytes into the blob copy at offset arg (from stock's at 0) */
        if (!blob_copy_len) {
            blob_copy_len = (u32)(radio_blob_end - radio_blob);
            if (blob_copy_len > sizeof(blob_copy)) blob_copy_len = 0;
            else memcpy(blob_copy, radio_blob, blob_copy_len);
        }
        if (arg > blob_copy_len || n > blob_copy_len - arg) return 0;
        memcpy(blob_copy + arg, data, n);
        return 0;
    case 0x3a:                           /* bring-up: start the Bluetooth core */
        radio_start_pending = 1;
        return 0;
    case 0x3b:                           /* bring-up: data {id, wide, payload} to the core */
        if (n < 2 || n > sizeof(raw) || raw_pending) return 0;
        memcpy(raw, data, n);
        raw_len = (u16)n;
        raw_pending = 1;
        return 0;
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
    yc_pin(0, 8, YC_PIN_PULLUP);
    yc_pin(0, 14, 0);
    yc_pin(1, 12, 0);
    util_log_init();
    util_log("bring-up %s, HCLK_CON %08x", BUILD_ID, (unsigned)YC_HCLK_CON);
    plat_log_fault();
    *(volatile u32 *)0xe000e180 = ~0u;  /* NVIC: what the loader left on, off */
    plat_usb_init();
    __asm__ volatile ("cpsie i" ::: "memory");
    YC_SYST_CSR = 7;                     /* the hang watch (platform_yc3121.c): a wedge restarts */

    const struct usb_events *ev = plat_usb_events();
    u32 resets = 0, odd = 0, stalls = 0;
    u8 configured = 0;
    u32 tick_ms = plat_ms(), held = 0, tick = 0, reboot_at = 0, last_keys = 0, sent_keys = ~0u, sent_sticks = 0;

    u32 pass_us = plat_us();
    for (;;) {
        u32 now_us = plat_us();
        plat_watch_kick();
        if (now_us - pass_us > ev->loop_max_us) ((struct usb_events *)ev)->loop_max_us = now_us - pass_us;
        pass_us = now_us;
        if (ev->resets != resets) util_log("usb: bus reset (%u)", (unsigned)(resets = ev->resets));
        if (ev->configured != configured) util_log("usb: configured %u", configured = ev->configured);
        if (ev->odd_setups != odd) util_log("usb: SETUP of %u bytes (%u)", ev->odd_length, (unsigned)(odd = ev->odd_setups));
        if (ev->stalls != stalls) {
            const u8 *q = ev->stalled;
            util_log("usb: stall %u: %02x %02x %02x%02x %02x%02x %02x%02x", (unsigned)(stalls = ev->stalls),
                     q[0], q[1], q[3], q[2], q[5], q[4], q[7], q[6]);
        }
        plat_usb_service();
        if (radio_start_pending) {
            radio_start_pending = 0;
            util_log("bt: start");
            int r = plat_radio_start();
            util_log("bt: started %d", r);
            if (!r) plat_adc_init(), adc_ready = 1;
        }
        if (core_load_pending) {
            u8 what = core_load_pending;
            int r = 0;

            if (what == 1) r = plat_radio_load(radio_blob, (u32)(radio_blob_end - radio_blob));
            else if (what == 2) r = blob_copy_len ? plat_radio_load(blob_copy, blob_copy_len) : -3;
            else plat_radio_release();
            util_log("bt: core %s %d", what == 3 ? "released" : "loaded, held", r);
            if (what == 3) plat_adc_init(), adc_ready = 1;
            core_load_pending = 0;
        }
        if (raw_pending) {
            if (!plat_radio_send(raw[0], raw + 2, (u16)(raw_len - 2), raw[1])) util_log("bt: %02x: no room", raw[0]);
            raw_pending = 0;
        }
        if (flash_pending) {
            u32 t0 = plat_us();
            int ok = flash_pending == 1 ? plat_flash_erase(flash_at) : plat_flash_write(flash_at, flash_data, flash_len);
            util_log("flash: %s %08x %u: %d in %u us", flash_pending == 1 ? "erase" : "write",
                     (unsigned)flash_at, (unsigned)flash_len, ok, (unsigned)(plat_us() - t0));
            flash_pending = 0;
        }
        if (bonds_send_pending) {
            bonds_send_pending = 0;
            util_log("bt: bonds to the core: %d", bt_command(0x26, bonds.table, sizeof(bonds.table), 0));
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
        u8 levels = 0;
        for (int k = 0; k < 3; k++) levels |= (u8)(yc_pin_in(probes[k].group, probes[k].pin) << k);
        if (levels != probe_levels)
            util_log("pins: 0.8 %u 0.14 %u 1.12 %u", levels & 1, levels >> 1 & 1, levels >> 2 & 1), probe_levels = levels;
        int down = !keys_down ? 0 : keys_down == 1u << 22 ? 1 : 2;
        if (keys_down != last_keys) util_log("keys %06x", (unsigned)(last_keys = keys_down));
        if (adc_ready) {
            for (int c = 0; c < 6; c++) adc[c] = plat_adc_read(c);
            if (!centre[0])
                for (int a = 0; a < 4; a++) centre[a] = adc[stick_channel[a]];
        }
        u8 axes[4] = { 0x80, 0x80, 0x80, 0x80 };
        if (centre[0])
            for (int a = 0; a < 4; a++) {
                int v = ((int)adc[stick_channel[a]] - centre[a]) * stick_sign[a];
                v = 128 + v * 128 / STICK_TRAVEL;
                axes[a] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        u32 sticks = (u32)axes[0] | (u32)axes[1] << 8 | (u32)axes[2] << 16 | (u32)axes[3] << 24;
        if (hid_connected && (keys_down != sent_keys || sticks != sent_sticks)) {
            u8 report[11] = { 0xa1, 0x01, (u8)keys_down, (u8)(keys_down >> 8), 8, axes[0], axes[1], axes[2], axes[3],
                              (u8)(keys_down >> 16), 0 };
            if (plat_radio_send(0xb0, report, sizeof(report), 1)) sent_keys = keys_down, sent_sticks = sticks;
        }
        if (!hid_connected) sent_keys = ~0u;
        if (tick % 100 == 0 && inputs_refused != inputs_refused_logged)
            util_log("bt: %u inputs refused", (unsigned)(inputs_refused_logged = inputs_refused));
        /* Bring-up: the Home light (0.5) high, the one below Home (0.4) low,
         * Sync's (2.2) high, to tell which level lights them; and every
         * input pin's change logged, to find the Sync button. */
        yc_pin(0, 4, YC_PIN_LOW);
        yc_pin(0, 5, YC_PIN_HIGH);
        yc_pin(2, 2, YC_PIN_HIGH);
        {
            static u16 last_in[3];
            static u8 seen;
            /* Not the outputs (lights, rows, power hold, motors, these
             * three) nor the matrix columns. */
            static const u16 mask[3] = {
                (u16)~(1u << 1 | 1u << 4 | 1u << 5 | 1u << 6 | 1u << 7 | 1u << 15 | COLUMNS_MASK),
                (u16)~(1u << 1 | 1u << 2 | 1u << 5 | 1u << 6 | 1u << 11),
                (u16)~(1u << 2 | 1u << 3 | 1u << 4),
            };

            for (int g = 0; g < 3; g++) {
                u16 v = YC_GPIO_IN(g) & mask[g];

                if (seen && v != last_in[g]) util_log("in: group %d %04x -> %04x", g, last_in[g], v);
                last_in[g] = v;
            }
            seen = 1;
        }
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
