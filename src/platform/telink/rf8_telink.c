#include "sdk.h"
#include "board.h"
#include "store.h"
#include "stack/2_4g/genfsk_ll/genfsk_ll.h"

#ifdef UPDATER                   /* the updater runs BLE only */
void rf8_start(int pairing) { (void)pairing; }
void rf8_stop(void) {}
int plat_receiver_paired(void) { return 0; }
void plat_receiver_pair(const u8 address[5], int save) { (void)address; (void)save; }
void rf8_update_bond(const struct bt_bond *bond) { (void)bond; }
u32 rf8_run(void) { return 0; }
int rf8_poll(struct bt_ev *ev) { (void)ev; return 0; }
int rf8_input_wanted(void) { return 0; }
int rf8_send_input(const u8 *data, u16 len) { (void)data; (void)len; return 0; }
#else

/* The stock 8BitDo receiver's 2.4 GHz link, behind bt_*() while a session
 * of a BT_8BITDO profile runs (bt_telink.c hands over).  As the stock pad
 * has it: generic FSK at 1 Mbit/s, 1-byte preamble, a 5-byte sync word,
 * variable payload with a 2-byte CRC.  Each frame goes out and the radio
 * listens 350 us for the receiver's reply (0x91, with the rumble).
 *
 * Paired: the receiver's address is the sync word, on channel
 * sum(address) % 156 + 2 (half-MHz steps) or 50; HOP_MS without a reply
 * moves to the other (the stock pad's three misses in a row happen often at
 * our frame rate, and a linked receiver stays on its channel).  A frame
 * without a reply goes again at once.  After 4-6 ms without a frame the
 * receiver scans both channels, linked or not (and drops the pad after 1 s):
 * the last frame goes again every KEEP_MS.  Pairing: 92 and six zeros on
 * channel 50 with the fixed pairing sync, until the receiver (just plugged
 * in) answers 93, its address and a checksum.  The address is the pad's,
 * not a slot's (plat_receiver_pair): a flash record, written by a new
 * pairing only.
 *
 * The SDK's BLE radio set-up leaves 0x800401 bit 3 set (on the pad, every
 * frame then went unanswered; clearing it alone fixed it): cleared here.
 * The FSK set-up changes registers BLE's set-up does not write again (the
 * packet format, the radio interrupt mask, modem settings: BLE advertised
 * but no host could connect), so the radio's configuration is saved before
 * and put back after, then BLE's transmit power.  The radio interrupt and
 * the system timer's stay off meanwhile: the SDK's handler is BLE's.  The
 * status bits are polled. */

#define FRAME_SIZE    18
#define REPLY_US      350
#define KEEP_MS       3          /* under the receiver's 4-6 ms before it scans */
#define LOST_MS       1000
#define PAIR_GAP_MS   2
#define HOP_MS        8          /* no reply this long: the other channel (stock: 3 misses) */
#define SECOND_CH     50
#define RF_FLAGS      (FLD_RF_IRQ_TX | FLD_RF_IRQ_RX | FLD_RF_IRQ_RX_TIMEOUT)

enum { OFF, PAIR, RUN };

/* The radio configuration the FSK set-up changes, the radio's RX and TX DMA
 * (BLE sets its RX buffer once, at start-up) among it: { first register,
 * count }. */
static const struct { u32 reg; u8 n; } saved_regs[] = {
    { 0x800400, 16 }, { 0x800420, 4 }, { 0x800430, 1 }, { 0x800c08, 8 }, { 0x800c20, 1 },
    { 0x800c42, 2 }, { 0x800f00, 1 }, { 0x800f04, 8 }, { 0x800f15, 2 }, { 0x800f1c, 2 },
    { 0x801220, 96 }, { 0x8012c0, 4 }, { 0x8012d0, 4 }, { 0x80134d, 1 },
};
#define SAVED_BYTES (16 + 4 + 1 + 8 + 1 + 2 + 1 + 8 + 2 + 2 + 96 + 4 + 4 + 1)

static void radio_registers(u8 *buf, int restore)
{
    u32 i, k;

    for (i = 0; i < sizeof(saved_regs) / sizeof(saved_regs[0]); i++)
        for (k = 0; k < saved_regs[i].n; k++, buf++) {
            if (restore) write_reg8(saved_regs[i].reg + k, *buf);
            else *buf = read_reg8(saved_regs[i].reg + k);
        }
}

static const u8 pairing_sync[5] = { 0x31, 0xa7, 0xa3, 0xea, 0x1b };

/* In the BLE TX FIFO's memory (plat_bt_idle_memory): no BLE session runs. */
struct rf8 {
    u8 tx[32];                   /* DMA length, RF length, payload */
    u8 rx[40];
    u8 frame[FRAME_SIZE];        /* the newest input, sent next and repeated */
    u8 reply[8];
    u8 address[5], channel[2];
    u8 mode, busy, ch, linked, have_frame, fresh, reply_len;
    u8 linked_pending, unlinked_pending, paired_pending, stopped_pending, write_pending;
    u32 sent_ms, reply_ms, hop_ms, irq_mask;
    u8 saved[SAVED_BYTES];       /* the radio's configuration under BLE */
};
static struct rf8 *r;

static u8 sum8(const u8 *p, u32 n)
{
    u32 s = 0;

    while (n--) s += *p++;
    return (u8)s;
}

static void radio(const u8 *sync, u8 channel)
{
    gen_fsk_datarate_set(GEN_FSK_DATARATE_1MBPS);   /* first, the SDK says */
    write_reg8(0x800401, read_reg8(0x800401) & ~0x08);
    gen_fsk_preamble_len_set(1);
    gen_fsk_sync_word_len_set(SYNC_WORD_LEN_5BYTE);
    gen_fsk_sync_word_set(GEN_FSK_PIPE0, (u8 *)sync);
    gen_fsk_pipe_open(GEN_FSK_PIPE0);
    write_reg8(0x800f15, read_reg8(0x800f15) & ~7);  /* TX on pipe 0 */
    gen_fsk_packet_format_set(GEN_FSK_PACKET_FORMAT_VARIABLE_PAYLOAD, 32);
    gen_fsk_rx_buffer_set(r->rx, sizeof(r->rx) - 4);
    gen_fsk_radio_power_set(GEN_FSK_RADIO_POWER_P6p45dBm);
    gen_fsk_channel_set(channel);
    gen_fsk_radio_state_set(GEN_FSK_STATE_AUTO);
    gen_fsk_tx_settle_set(149);
    rf_irq_enable(RF_FLAGS);
}

static void run_on(const u8 address[5])
{
    memcpy(r->address, address, 5);
    r->channel[0] = (u8)((address[0] + address[1] + address[2] + address[3] + address[4]) % 156 + 2);
    r->channel[1] = SECOND_CH;
    r->ch = 0;
    r->mode = RUN;
    r->sent_ms = plat_ms() - KEEP_MS;
    radio(r->address, r->channel[0]);
}

/* ---- the receiver's address: a record in the settings' flash log -------- */

#define RECEIVER_KEY STORE_RECEIVER_KEY          /* settings use 1..28 */

static u8 receiver[5], receiver_known, receiver_loaded;

static u8 receiver_check(const u8 *a)
{
    return (u8)(sum8(a, 5) ^ 0x5a);
}

int plat_receiver_paired(void)
{
    u8 record[STORE_RECEIVER_SIZE];

    if (!receiver_loaded) {
        receiver_loaded = 1;
        if (plat_store_read(RECEIVER_KEY, record, sizeof(record)) == sizeof(record) &&
            record[0] == 'R' && record[1] == 1 && record[7] == receiver_check(record + 2)) {
            memcpy(receiver, record + 2, 5);
            receiver_known = 1;
        }
    }
    return receiver_known;
}

void plat_receiver_pair(const u8 address[5], int save)
{
    u8 record[STORE_RECEIVER_SIZE] = { 'R', 1 };

    memcpy(receiver, address, 5);
    receiver_known = receiver_loaded = 1;
    if (!save) return;
    memcpy(record + 2, address, 5);
    record[7] = receiver_check(address);
    if (plat_store_write(RECEIVER_KEY, record, sizeof(record)) != sizeof(record))
        util_log("rf8: could not save the receiver");
}

void rf8_start(int pairing)
{
    r = plat_bt_idle_memory(sizeof(*r));
    if (!r) {
        util_log("rf8: no memory");
        return;
    }
    memset(r, 0, sizeof(*r));
    r->irq_mask = reg_irq_mask & (FLD_IRQ_ZB_RT_EN | FLD_IRQ_SYSTEM_TIMER);
    reg_irq_mask &= ~(FLD_IRQ_ZB_RT_EN | FLD_IRQ_SYSTEM_TIMER);
    radio_registers(r->saved, 0);
    if (pairing || !plat_receiver_paired()) {
        r->mode = PAIR;
        radio(pairing_sync, SECOND_CH);
        util_log("rf8: pairing");
    } else {
        run_on(receiver);
        util_log("rf8: receiver %02x%02x%02x%02x%02x, ch %u", r->address[0], r->address[1],
                 r->address[2], r->address[3], r->address[4], r->channel[0]);
    }
}

/* The radio back to BLE; STOPPED comes through rf8_poll(). */
void rf8_stop(void)
{
    if (!r || r->mode == OFF) return;
    gen_fsk_radio_state_set(GEN_FSK_STATE_OFF);
    rf_irq_clr_src(0xffff);
    rf_drv_ble_init();
    radio_registers(r->saved, 1);
    rf_set_power_level_index(RF_POWER_P2p87dBm);   /* as plat_bt_init() */
    reg_irq_mask |= r->irq_mask;
    if (r->linked) r->unlinked_pending = 1;
    r->linked = 0;
    r->stopped_pending = 1;
    r->mode = OFF;
}

void rf8_update_bond(const struct bt_bond *bond)
{
    (void)bond;
    if (r && r->mode == PAIR && receiver_known) run_on(receiver);
}

static void send(const u8 *payload, u8 len)
{
    util_put32(r->tx, len + 1u);
    r->tx[4] = len;
    memcpy(r->tx + 5, payload, len);
    rf_irq_clr_src(RF_FLAGS);
    gen_fsk_stx2rx_start(r->tx, clock_time(), REPLY_US);
    r->busy = 1;
    r->sent_ms = plat_ms();
}

static void replied(const u8 *p, u8 len)
{
    if (r->mode == PAIR) {
        if (len < 7 || p[0] != 0x93 || sum8(p, 6) != p[6]) return;
        r->paired_pending = 1;
        util_log("rf8: paired with %02x%02x%02x%02x%02x", p[1], p[2], p[3], p[4], p[5]);
        run_on(p + 1);
        return;
    }
    if (len < 7 || p[0] != 0x91 || sum8(p, 6) != p[6]) return;
    r->reply_ms = plat_ms();
    if (!r->linked) {
        r->linked = r->linked_pending = 1;
        util_log("rf8: linked on ch %u", r->channel[r->ch]);
    }
    if (memcmp(r->reply, p, 7)) {
        memcpy(r->reply, p, 7);
        r->reply_len = 7;
        r->write_pending = 1;
    }
}

static void exchange(void)
{
    u16 st = rf_irq_src_get();
    u8 len, *p;

    if (st & FLD_RF_IRQ_RX) {
        rf_irq_clr_src(FLD_RF_IRQ_RX);
        r->busy = 0;
        if (gen_fsk_is_rx_crc_ok(r->rx)) {
            p = gen_fsk_rx_payload_get(r->rx, &len);
            replied(p, len);
        }
    } else if (st & FLD_RF_IRQ_RX_TIMEOUT) {
        rf_irq_clr_src(FLD_RF_IRQ_RX_TIMEOUT | FLD_RF_IRQ_TX);
        r->busy = 0;
        if (r->mode == RUN && r->have_frame) r->fresh = 1;    /* the newest again */
        if (r->mode == RUN && plat_ms() - r->reply_ms >= HOP_MS && plat_ms() - r->hop_ms >= HOP_MS) {
            r->hop_ms = plat_ms();
            r->ch ^= 1;
            gen_fsk_channel_set(r->channel[r->ch]);
        }
    }
}

static void next(void)
{
    if (r->mode == PAIR) {
        static const u8 request[7] = { 0x92 };

        if (plat_ms() - r->sent_ms >= PAIR_GAP_MS) send(request, sizeof(request));
        return;
    }
    if (!r->fresh && plat_ms() - r->sent_ms < KEEP_MS) return;
    if (!r->have_frame) {                    /* before the first report: centred, nothing held */
        memset(r->frame, 0, FRAME_SIZE);
        r->frame[0] = 0x90;
        util_put16(r->frame + 7, 0x7fff);
        util_put16(r->frame + 9, 0x7fff);
        util_put16(r->frame + 11, 0x7fff);
        util_put16(r->frame + 13, 0x7fff);
    }
    r->frame[1] = r->channel[0];
    r->frame[2] = r->channel[1];
    r->frame[17] = sum8(r->frame, 17);
    send(r->frame, FRAME_SIZE);
    r->fresh = 0;
}

/* From the wait loop: the exchange under way, then the next one; EV_BT
 * when rf8_poll() has news. */
u32 rf8_run(void)
{
    if (!r) return 0;
    if (r->mode != OFF) {
        if (r->busy) exchange();
        if (r->mode == RUN && r->linked && plat_ms() - r->reply_ms > LOST_MS) {
            r->linked = 0;
            r->unlinked_pending = 1;
            util_log("rf8: receiver lost");
        }
        if (!r->busy) next();
    }
    return r->linked_pending || r->unlinked_pending || r->paired_pending || r->stopped_pending ||
           r->write_pending ? EV_BT : 0;
}

int rf8_poll(struct bt_ev *ev)
{
    memset(ev, 0, sizeof(*ev));
    if (!r) return 0;
    if (r->paired_pending) {
        r->paired_pending = 0;
        ev->kind = BT_EV_PAIRED;
        memcpy(ev->bond.peer, r->address, 5);
        return 1;
    }
    if (r->linked_pending) {
        r->linked_pending = 0;
        ev->kind = BT_EV_LINKED;
        return 1;
    }
    if (r->write_pending) {
        r->write_pending = 0;
        ev->kind = BT_EV_WRITE;
        ev->write.kind = BT_WRITE_OUTPUT;
        ev->write.id = r->reply[0];
        ev->write.len = r->reply_len;
        memcpy(ev->write.data, r->reply, r->reply_len);
        return 1;
    }
    if (r->unlinked_pending) {
        r->unlinked_pending = 0;
        ev->kind = BT_EV_UNLINKED;
        ev->lost = r->mode != OFF;           /* not our own stop */
        return 1;
    }
    if (r->stopped_pending) {
        r->stopped_pending = 0;
        ev->kind = BT_EV_STOPPED;
        r = NULL;                        /* the memory is BLE's again */
        return 1;
    }
    return 0;
}

/* The report every pass; it goes when it differs from the last (bytes 1,
 * 2 and 17 are the link's).  Newest wins: a frame not yet sent is
 * replaced. */
int rf8_input_wanted(void)
{
    return r && r->mode == RUN && r->linked;
}

int rf8_send_input(const u8 *data, u16 len)
{
    if (!rf8_input_wanted() || len != FRAME_SIZE) return 0;
    if (r->have_frame && data[0] == r->frame[0] && !memcmp(data + 3, r->frame + 3, 14)) return 0;
    memcpy(r->frame, data, len);
    r->have_frame = r->fresh = 1;
    if (r->busy) exchange();             /* at once if the radio is free */
    if (!r->busy) next();
    return 1;
}
#endif
