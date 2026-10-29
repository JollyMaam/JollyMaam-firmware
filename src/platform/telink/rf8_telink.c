#include "sdk.h"
#include "board.h"

/* Test build (RF8_TEST, make ... U2CW_RF8=d0,e8,76,02,cc): the pad talks to
 * the stock 8BitDo receiver on that receiver's address, the way the stock
 * pad does once paired (docs/8bitdo-ultimate2c-wireless/, research notes):
 * generic FSK at 1 Mbit/s, 1-byte preamble, the 5-byte address as the sync
 * word, variable payload with a 2-byte CRC, on channel (sum of the address
 * bytes) % 156 + 2 in half-MHz steps.  Each frame goes out and the radio
 * listens 350 us for the receiver's reply; a new frame when the input
 * changed, else every 30 ms.  No Bluetooth, no controller: the board's raw
 * input, and USB for the tools (peek rf8 for the counts). */

#ifdef RF8_TEST
#include "stack/2_4g/genfsk_ll/genfsk_ll.h"

#define RF8_FRAME     18
#define RF8_REPLY_US  350
#define RF8_KEEP_MS   30
#define RF8_SECOND_CH 50

static const u8 address[5] = { RF8_TEST };
static u8 tx[32] __attribute__((aligned(4)));
static u8 rx[40] __attribute__((aligned(4)));

/* Read with padtool peek: what went out and what came back. */
struct {
    u32 sent, tx_done, replies, crc_bad, timeouts;
    u8 channel, busy, reply_len, sweep_bit;
    u8 reply[12];
} rf8;

/* The receiver's XInput bits (its conversion of the frame). */
static u32 buttons_of(u32 b)
{
    static const struct { u32 pad; u8 bit; } map[] = {
        { PAD_START, 0 }, { PAD_L3, 1 }, { PAD_R3, 2 }, { PAD_SELECT, 3 },
        { PAD_X, 4 }, { PAD_Y, 5 }, { PAD_RIGHT, 6 }, { PAD_LEFT, 7 },
        { PAD_DOWN, 8 }, { PAD_UP, 9 }, { PAD_L1, 10 }, { PAD_R1, 11 },
        { PAD_B, 12 }, { PAD_A, 13 }, { PAD_HOME, 17 },
    };
    u32 i, out = 0;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (b & map[i].pad) out |= 1u << map[i].bit;
    return out;
}

/* An XInput axis as the frame carries it: the receiver reports 0x7fff - raw. */
static u16 axis_of(s32 v)
{
    return (u16)(0x7fff - v);
}

static s32 up(s16 v)                     /* XInput's Y grows upward, ours downward */
{
    return v == -32768 ? 32767 : -v;
}

/* The calibrated input; RF8_SWEEP adds each unassigned button bit in turn
 * (rf8.sweep_bit), half a second each, to find what the receiver does with it. */
static void frame(const struct in_raw *in, u8 *p)
{
    struct pad pad;
    u32 b, i, sum = 0;

    memset(&pad, 0, sizeof(pad));
    cal_apply(set_cal(), NULL, in, &pad);
    b = buttons_of(in->buttons);
#ifdef RF8_SWEEP
    {
        static const u8 bits[] = { 14, 15, 16, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
        u32 k = plat_ms() / 500;

        rf8.sweep_bit = k & 1 ? bits[(k / 2) % sizeof(bits)] : 0;
        if (rf8.sweep_bit) b |= 1u << rf8.sweep_bit;
    }
#endif
    p[0] = 0x90;
    p[1] = rf8.channel;
    p[2] = RF8_SECOND_CH;
    util_put32(p + 3, b);
    util_put16(p + 7, axis_of(pad.lx));
    util_put16(p + 9, axis_of(up(pad.ly)));
    util_put16(p + 11, axis_of(pad.rx));
    util_put16(p + 13, axis_of(up(pad.ry)));
    p[15] = (u8)(pad.l2 >> 8);
    p[16] = (u8)(pad.r2 >> 8);
    for (i = 0; i < 17; i++) sum += p[i];
    p[17] = (u8)sum;
}

static void radio_init(void)
{
    u32 sum = address[0] + address[1] + address[2] + address[3] + address[4];

    rf8.channel = (u8)(sum % 156 + 2);
    gen_fsk_datarate_set(GEN_FSK_DATARATE_1MBPS);   /* first, the SDK says */
    gen_fsk_preamble_len_set(1);
    gen_fsk_sync_word_len_set(SYNC_WORD_LEN_5BYTE);
    gen_fsk_sync_word_set(GEN_FSK_PIPE0, (u8 *)address);
    gen_fsk_pipe_open(GEN_FSK_PIPE0);
    write_reg8(0x800f15, read_reg8(0x800f15) & ~7);  /* TX on pipe 0 */
    gen_fsk_packet_format_set(GEN_FSK_PACKET_FORMAT_VARIABLE_PAYLOAD, 32);
    gen_fsk_rx_buffer_set(rx, 36);
    gen_fsk_radio_power_set(GEN_FSK_RADIO_POWER_P6p45dBm);
    gen_fsk_channel_set(rf8.channel);
    gen_fsk_radio_state_set(GEN_FSK_STATE_AUTO);
    gen_fsk_tx_settle_set(149);
    /* the status bits latch; no CPU interrupt (the SDK's handler is BLE's) */
    rf_irq_enable(FLD_RF_IRQ_TX | FLD_RF_IRQ_RX | FLD_RF_IRQ_RX_TIMEOUT);
}

static void send(const u8 *payload)
{
    util_put32(tx, RF8_FRAME + 1);
    tx[4] = RF8_FRAME;
    memcpy(tx + 5, payload, RF8_FRAME);
    rf_irq_clr_src(FLD_RF_IRQ_TX | FLD_RF_IRQ_RX | FLD_RF_IRQ_RX_TIMEOUT);
    gen_fsk_stx2rx_start(tx, clock_time(), RF8_REPLY_US);
    rf8.sent++;
    rf8.busy = 1;
}

/* The exchange is over: a reply, or the receive window timed out. */
static void poll_radio(void)
{
    u16 st = rf_irq_src_get();
    u8 len, *p;

    if (st & FLD_RF_IRQ_TX) {
        rf_irq_clr_src(FLD_RF_IRQ_TX);
        rf8.tx_done++;
    }
    if (st & FLD_RF_IRQ_RX) {
        rf_irq_clr_src(FLD_RF_IRQ_RX);
        if (gen_fsk_is_rx_crc_ok(rx)) {
            p = gen_fsk_rx_payload_get(rx, &len);
            rf8.replies++;
            rf8.reply_len = len;
            memcpy(rf8.reply, p, len < sizeof(rf8.reply) ? len : sizeof(rf8.reply));
        } else {
            rf8.crc_bad++;
        }
        rf8.busy = 0;
    }
    if (st & FLD_RF_IRQ_RX_TIMEOUT) {
        rf_irq_clr_src(FLD_RF_IRQ_RX_TIMEOUT);
        rf8.timeouts++;
        rf8.busy = 0;
    }
}

void rf8_test(void)
{
    static struct usb_status status;
    struct in_raw in;
    u8 now[RF8_FRAME], last[RF8_FRAME];
    u32 at = 0;
    struct ctl_request r;

    status.state = CTL_CONNECTED;
    status.slot = SLOT_NONE;
    radio_init();
    util_log("rf8: channel %u", rf8.channel);
    memset(last, 0, sizeof(last));
    for (;;) {
        plat_wait(0, plat_us() + 200);
        poll_radio();
        board_input(&in);
        frame(&in, now);
        if (!rf8.busy && (memcmp(now, last, RF8_FRAME) || plat_ms() - at >= RF8_KEEP_MS)) {
            send(now);
            memcpy(last, now, sizeof(last));
            at = plat_ms();
        }
        status.uptime_ms = plat_ms();
        r = usb_poll(&status);
        if (r.kind == REQ_REBOOT) plat_reboot();
        if (r.kind == REQ_REBOOT_LOADER) plat_reboot_loader();
    }
}
#endif
