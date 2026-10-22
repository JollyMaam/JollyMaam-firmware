/* The Classic TWS link between two earbuds (the SDK's tws_link.c): the
 * global `tws_link` state (read by retained and owned code even though this
 * controller never pairs a peer), the monitor frame set-up that follows the
 * peer's link, the master/slave hand-over, the eSCO relay link open and
 * close, and the link-state sync channel.  The relay's data path
 * (tws_esco_transmit_deal and friends), tws_esco_acl_communication and
 * tws_link_disconnection have no caller anywhere and go with the member.
 *
 * Quirks kept as the SDK has them: the eSCO relay block (bt_malloc(80)) is
 * not cleared, so its timers, toggle bits and tws_esco_tx_info are whatever
 * the heap held, and esco_dump_cnt too when the phone's rx list was not
 * empty; rx_remove_esco_req_flag is set to 80, not a flag value; when the
 * phone's eSCO still holds queued rx frames and the open is not a monitor
 * start, all but one of them are freed; a failed allocation asserts and
 * then carries on; tws_link_rxtx_state_reset trades the two queued tx
 * packets when the toggle bit was set; the tx sync request reads linkcntl0
 * twice.  The volatile register accesses and their order are the SDK's. */
#include <stddef.h>
#include <string.h>
#include "btctrler/glue.h"

#define TWS_CODE __attribute__((section(".classic_tws_code")))

#ifdef UNITTEST
#include <stdio.h>
#define putchar test_putchar
int test_putchar(int c);
#else
int putchar(int c);
#endif
int os_taskq_post_type(const char *name, int type, int argc, int *argv);
void log_print(int level, const char *tag, const char *format, ...);
extern BTCTRLER_CONFIG u8 log_tag_const_i_TWS_LMP;

#ifdef UNITTEST
/* Host tests: the baseband registers are plain memory. */
static volatile u32 test_regs[0x400];
#define REG32(a) (test_regs[((a) & 0xfff) / 4])
#else
#define REG32(a) (*(volatile u32 *)(a))
#endif

#define REG_WIN_BASE    0x1c0028u     /* + 20 gives the monitor frame's window */
#define REG_ESCO_CNTL   0x1c0150u     /* zeroed before the eSCO relay is programmed */
#define REG_ESCO_TXPTR  0x1c015cu     /* tx buffers: (second << 16) | first */
#define REG_ESCO_RXPTR  0x1c0160u     /* rx buffers: (second << 16) | first */
#define REG_BUF_BASE    0x1cfc44u     /* subtracted from buffer addresses */

#ifndef UNITTEST
_Static_assert(sizeof(struct tws_link_connection) == 80, "SDK tws_link_connection size");
_Static_assert(sizeof(struct tws_link_core_data) == 44, "SDK tws_link_core_data size");
_Static_assert(offsetof(struct tws_link_core_data, rx_enc) == 2, "rx_enc");
_Static_assert(offsetof(struct tws_link_core_data, tx_enc) == 3, "tx_enc");
_Static_assert(offsetof(struct tws_link_core_data, super_timeout_cnt) == 4, "super_timeout_cnt");
_Static_assert(offsetof(struct tws_link_core_data, rev) == 6, "rev");
_Static_assert(offsetof(struct tws_link_core_data, channel_map) == 8, "channel_map");
_Static_assert(offsetof(struct tws_link_core_data, bitoff) == 18, "bitoff");
_Static_assert(offsetof(struct tws_link_core_data, cryptk) == 20, "cryptk");
_Static_assert(offsetof(struct tws_link_core_data, afh_end) == 36, "afh_end");
_Static_assert(offsetof(struct tws_link_core_data, offset) == 40, "offset");
_Static_assert(offsetof(struct tws_link_connection, link) == 72, "link");
_Static_assert(offsetof(struct bredr_esco_link, tx_esco) == 24, "tx_esco");
_Static_assert(offsetof(struct bredr_esco_link, rx_esco) == 32, "rx_esco");
_Static_assert(offsetof(struct bredr_esco_link, rx_bulk) == 40, "rx_bulk");
_Static_assert(offsetof(struct bredr_esco_link, tx_bulk) == 48, "tx_bulk");
_Static_assert(offsetof(struct bredr_esco_link, pdu) == 60, "pdu");
_Static_assert(offsetof(struct bredr_rx_bulk, entry) == 16, "rx entry");
#endif

/* The SDK's state of the link to the peer earbud.  Nothing in this
 * controller stores to it: .bss, all zero. */
struct tws_link_connection tws_link;

static void __list_del_entry(struct list_head *entry)
{
    struct list_head *prev = entry->prev, *next = entry->next;
    next->prev = prev;
    prev->next = next;
}

static void list_add(struct list_head *new, struct list_head *head)
{
    struct list_head *next = head->next;
    next->prev = new;
    new->next = next;
    new->prev = head;
    head->next = new;
}

static void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

u32 tws_link_read_slot_clk() TWS_CODE;
u32 tws_link_read_slot_clk()
{
    if (!tws_link.link) return 0;
    return bredr_get_master_slot_clk(tws_link.link);
}

/* Monitor mode: follow the peer's link without transmitting.  en 0/1 and
 * role 0 (slave) / 1 (master) pick one of four baseband set-ups. */
void tws_monitor_enable(struct link_connection *link, u8 en, u8 role) TWS_CODE;
void tws_monitor_enable(struct link_connection *link, u8 en, u8 role)
{
    local_irq_disable();
    link->monitor_en = en;
    if (en) {
        if (role) tws_link_monitor_master_enable(link->frame);
        else tws_link_monitor_slave_enable(link->frame);
    } else {
        if (role) tws_link_monitor_master_init(link->frame);
        else tws_link_monitor_slave_init(link->frame);
    }
    local_irq_enable();
}

void tws_link_master_2_slave(struct link_connection *link) TWS_CODE;
void tws_link_master_2_slave(struct link_connection *link)
{
    local_irq_disable();
    /* SDK debug offsets place monitor at byte 46, bit 0 (the byte-level
     * OR below in the SDK IR is mask 1); keep the named bitfield access. */
    link->monitor = 1;
    tws_monitor_enable(link, link->monitor_en, 0);
    bredr_pwr_set(link, 0, 9);
    local_irq_enable();
    tws_conn_send_event(10, "1", 1);
}

/* Leave monitor mode: the phone link takes over with the sequence bits the
 * eSCO link last saw. */
void tws_link_slave_2_master(struct link_connection *link) TWS_CODE;
void tws_link_slave_2_master(struct link_connection *link)
{
    struct ctrl_frame *frame = link->frame;

    link_conn_tx_empty(link);
    local_irq_disable();
    link->monitor = 0;
    link->tx_bulk[0] = 0;
    link->tx_bulk[1] = 0;
    frame->txbuf0cntl |= 1;
    frame->txbuf1cntl |= 1;
    if (link->esco) {
        u8 txtog = frame->txtog;
        if (((link->esco->remote_txtog ^ txtog) & 1) == 0) {
            __write_reg_seqn(link->frame, 0, link->esco->remote_seqn & 1);
            __write_reg_seqn(link->frame, 1, link->esco->remote_seqn & 2);
        } else {
            __write_reg_seqn(link->frame, 1, link->esco->remote_seqn & 1);
            __write_reg_seqn(link->frame, 0, link->esco->remote_seqn & 2);
        }
    }
    tws_monitor_enable(link, link->monitor_en, 1);
    local_irq_enable();
    tws_conn_send_event(10, "1", 0);
}

/* A frame for the monitor link: the window follows the register at
 * REG_WIN_BASE plus 20, the buffers are armed, the link control words are
 * the monitor's. */
struct ctrl_frame *tws_link_monitor_frame_init() TWS_CODE;
struct ctrl_frame *tws_link_monitor_frame_init()
{
    struct ctrl_frame *frame = bredr_bd_get_frame();
    u16 bufcntl;

    frame->bufcntl &= 0xf0ff;
    bufcntl = frame->bufcntl;
    frame->bufcntl = bufcntl;
    frame->bufcntl &= 0xffdf;
    frame->bufcntl |= 3;
    frame->wincntl = (u16)(REG32(REG_WIN_BASE) + 20);
    frame->txbuf0cntl |= 1;
    frame->txbuf1cntl |= 1;
    __write_reg_linkcntl(frame, 0, 16400);
    __write_reg_linkcntl(frame, 1, 16384);
    __write_reg_txphdr(frame, 0, 2);
    __write_reg_txphdr(frame, 1, 2);
    return frame;
}

/* Copy the phone link's core data into the monitor frame, moving the clock
 * and bit offsets onto the peer link's timebase (625 us bit offsets; the
 * carry into the clock offset is the SDK's > 624 test). */
void tws_link_monitor_frame_setup(struct ctrl_frame *frame, struct bredr_frame_core_data *data) TWS_CODE;
void tws_link_monitor_frame_setup(struct ctrl_frame *frame, struct bredr_frame_core_data *data)
{
    struct ctrl_frame *peer = tws_link.link->frame;
    u32 clk_offset, tem_bitoff;
    u16 rx_enc, tx_enc, peer_lo, peer_hi;
    int i;

    frame->miscntl = data->miscntl | 16;
    frame->format = data->format;
    frame->bdaddr0 = data->bdaddr0;
    frame->bdaddr1 = data->bdaddr1;
    frame->bdaddr2 = data->bdaddr2;
    frame->bchl = data->bchl;
    frame->bchm = data->bchm;
    frame->bchh = data->bchh;

    peer_lo = peer->clk_offsetl;
    peer_hi = peer->clk_offseth;
    clk_offset = ((u32)peer_hi << 16 | peer_lo) + ((u32)data->clk_offseth << 16 | data->clk_offsetl);
    tem_bitoff = (peer->bitoff & 1023) + (u32)data->bitoff;
    if (tem_bitoff > 624) {
        tem_bitoff -= 625;
        clk_offset -= 1;
    }
    frame->bitoff = (u16)tem_bitoff;
    __write_reg_clkoffset(frame, (s32)clk_offset);

    for (i = 0; i < 8; i++)
        frame->cryptk[i] = data->cryptk[i];
    rx_enc = (u16)(data->rx_enc << 15);
    tx_enc = (u16)(data->tx_enc << 7);
    frame->linkcntl0 = frame->linkcntl0 | rx_enc | tx_enc;
    frame->linkcntl1 = frame->linkcntl1 | rx_enc | tx_enc;
    tws_link_monitor_slave_init(frame);
}

/* Sync channel 1 request: describe this link (packet toggles, encryption,
 * AFH map, supervision timeout, clock and bit offset to the peer) to the
 * other earbud.  Returns the request length when something changed (the
 * core data hash differs or 30 requests passed), else 0.  With an eSCO
 * link open nothing is sent; every fifth request only prints a 'Z'. */
static int tws_link_tx_sync_req(struct lmp_connection *conn, u8 *txbuffer, int prev_req_err) TWS_CODE;
static int tws_link_tx_sync_req(struct lmp_connection *conn, u8 *txbuffer, int prev_req_err)
{
    struct tws_link_core_data *core = (struct tws_link_core_data *)txbuffer;
    struct link_connection *link = conn->link;
    struct ctrl_frame *frame = link->frame;
    u32 super_timeout_begin, hash;
    s32 clkoffset, offset, bitoff, peer_clkoffset;
    int div;

    core->ptt = (frame->miscntl >> 3) & 1;
    core->rx_enc = frame->linkcntl0 >> 15;
    core->tx_enc = (u8)frame->linkcntl0 >> 7;
    core->afh_enable = (frame->format >> 6) & 1;
    core->afh_end = link->afh_slot_end;
    super_timeout_begin = link->super_timeout_begin;
    div = bredr_clkn2offset(super_timeout_begin, bredr_read_slot_clk()) / 800;
    core->super_timeout_cnt = (u16)div;
    memcpy(core->channel_map, link->channel_map, 10);
    memcpy(core->cryptk, (const void *)link->frame->cryptk, 16);

    if (tws_link.sync_clkn_offset) {
        tws_link.sync_clkn_offset = 0;
        clkoffset = __read_reg_clkoffset(link->frame);
        bitoff = link->frame->bitoff & 1023;
        peer_clkoffset = __read_reg_clkoffset(tws_link.link->frame);
        offset = (s32)((u32)clkoffset - (u32)peer_clkoffset);
        bitoff = bitoff - (tws_link.link->frame->bitoff & 1023);
        if (bitoff < 0) {
            offset = (s32)((u32)offset + 1);
            bitoff = bitoff + 625;
        }
        core->offset = offset;
    } else {
        core->offset = 0;
        bitoff = 0;
    }
    core->bitoff = (s16)bitoff;
    core->rev = 0;

    if (conn->esco_conn) {
        if (prev_req_err == 0 && tws_link.esco_tx_sync_req_cnt) {
            tws_link.esco_tx_sync_req_cnt--;
            if (tws_link.esco_tx_sync_req_cnt % 5 == 0) {
                putchar(90);
                return 44;
            }
        }
        return 0;
    }

    hash = JBHash(txbuffer, 44);
    if (tws_link.core_hash != (int)hash || tws_link.tx_req_cnt > 29) {
        tws_link.core_hash = hash;
        tws_link.wait_tx_suss = 1;
        tws_link.tx_req_cnt = 0;
        return 44;
    }
    tws_link.tx_req_cnt++;
    if (!tws_link.wait_tx_suss) return 0;
    if (prev_req_err) return 44;
    tws_link.wait_tx_suss = 0;
    return 0;
}

/* Sync channel 1 response: apply the peer's link description.  The
 * supervision timeout start only moves later; the AFH map is taken when it
 * ends later (or none is set); the clock and bit offsets are the peer's
 * plus our own, with the same > 624 carry as above. */
static int tws_link_rx_sync_req(struct lmp_connection *conn, u8 *rxbuffer, int len, u8 *txbuffer) TWS_CODE;
static int tws_link_rx_sync_req(struct lmp_connection *conn, u8 *rxbuffer, int len, u8 *txbuffer)
{
    const struct tws_link_core_data *core = (const struct tws_link_core_data *)rxbuffer;
    struct link_connection *link = conn->link;
    int clk, timeout;
    u32 offset;
    int bitoff;
    (void)len;
    (void)txbuffer;

    link_conn_set_ptt(link, core->ptt);
    link_conn_set_encrypt(link, core->rx_enc, core->tx_enc);
    memcpy((void *)link->frame->cryptk, core->cryptk, 16);

    clk = bredr_read_slot_clk();
    timeout = core->super_timeout_cnt * 800;
    if (!(clk < timeout)) {
        int begin = clk - timeout;
        if (bredr_clkn_after(begin, link->super_timeout_begin))
            link->super_timeout_begin = begin;
    }

    if (bredr_clkn_after(core->afh_end, link->afh_slot_end) || link->afh_slot_end == 0)
        bredr_link_set_afh(link, core->afh_end, core->afh_enable, (u8 *)core->channel_map);

    if (core->offset) {
        struct ctrl_frame *frame = link->frame;
        u16 unused;
        s32 peer_clkoffset;

        unused = frame->bitoff;
        (void)unused;
        __read_reg_clkoffset(frame);
        peer_clkoffset = __read_reg_clkoffset(tws_link.link->frame);
        offset = (u32)peer_clkoffset + core->offset;
        bitoff = (tws_link.link->frame->bitoff & 1023) + core->bitoff;
        if (bitoff > 624) {
            bitoff -= 625;
            offset -= 1;
        }
        link->frame->bitoff = (u16)bitoff;
        __write_reg_clkoffset(link->frame, (s32)offset);
    }
    return 0;
}

int tws_link_get_rxtx_state(struct link_connection *link, struct tws_link_rxtx_state *data) TWS_CODE;
int tws_link_get_rxtx_state(struct link_connection *link, struct tws_link_rxtx_state *data)
{
    struct ctrl_frame *frame = link->frame;

    data->rxtog = frame->rxtog;
    data->txtog = frame->txtog;
    data->rxstat0 = frame->rxstat0;
    data->rxstat1 = frame->rxstat1;
    data->seqn = 0;
    if (__read_reg_packet_seqn(frame, 0)) data->seqn |= 1;
    if (__read_reg_packet_seqn(link->frame, 1)) data->seqn |= 2;
    return 8;
}

int tws_link_set_rxtx_state(struct link_connection *link, struct tws_link_rxtx_state *data) TWS_CODE;
int tws_link_set_rxtx_state(struct link_connection *link, struct tws_link_rxtx_state *data)
{
    struct bredr_esco_link *esco = link->esco;

    if (esco) {
        esco->remote_txtog = data->txtog;
        esco->remote_seqn = data->seqn;
    } else {
        struct ctrl_frame *frame = link->frame;

        frame->rxtog = data->rxtog;
        frame->txtog = data->txtog;
        frame->rxstat0 = data->rxstat0;
        frame->rxstat1 = data->rxstat1;
        frame->txbuf0cntl |= 1;
        frame->txbuf1cntl |= 1;
        __write_reg_seqn(frame, 0, data->seqn & 1);
        __write_reg_seqn(link->frame, 1, data->seqn & 2);
    }
    return 8;
}

/* Put the phone link's packet state back to the beginning: pending rx
 * released, status words reset when nothing is pending, the (up to two)
 * queued tx packets returned to the head of the ACL queues with their
 * offsets rebased on the transmit pointers, and the toggle bits cleared.
 * When the toggle bit was set and both packets are queued they trade
 * places. */
void tws_link_rxtx_state_reset() TWS_CODE;
void tws_link_rxtx_state_reset()
{
    u16 txptr[3];
    struct bredr_tx_bulk *tx[3];
    struct link_connection *link = tws_link.link;
    struct ctrl_frame *frame = link->frame;
    u8 txtog = frame->txtog & 1;
    u8 role;
    int i;

    link_conn_rx_bulk_avaliable(link, 0);
    /* Both reads are intentional: the helper can change rx_pend, and the
     * task post can change it again before the status reset decision. */
    if (link->rx_pend)
        os_taskq_post_type("tws", 54, 1, (int *)&tws_link.link);
    if (!link->rx_pend) {
        u16 status = (u8)((tws_link.link->role << 4) ^ 0x90);
        frame->rxstat0 = status;
        frame->rxstat1 = status;
        frame->rxtog = 0;
    }

    for (i = 0; i < 2; i++) {
        tx[i] = link->tx_bulk[i];
        txptr[i] = (&frame->txptr0)[i];
    }
    if (tx[0] && (txtog != 0 && tx[1] != 0)) {
        struct bredr_tx_bulk *first = tx[0], *second = tx[1];
        u16 first_ptr, second_ptr;

        tx[2] = first;
        tx[0] = second;
        tx[1] = first;
        first_ptr = txptr[0];
        txptr[2] = first_ptr;
        second_ptr = txptr[1];
        txptr[0] = second_ptr;
        txptr[1] = first_ptr;
    }
    if (tx[0] == tx[1]) tx[1] = 0;

    for (i = 1; i >= 0; i--) {
        struct bredr_tx_bulk *bulk = tx[i];
        if (bulk) {
            if (bulk->offset != bulk->len) __list_del_entry(&bulk->entry);
            bulk->offset = (u16)(txptr[i] + REG32(REG_BUF_BASE) - (u32)(uintptr_t)bulk->data);
            if (bulk->llid == 3) list_add(&bulk->entry, &link->tx_acl_c);
            else list_add(&bulk->entry, &link->tx_acl_u);
        }
        link->tx_bulk[i] = 0;
    }

    role = (tws_link.link->role ^ 1) & 1;
    frame->txtog &= 0xf8;
    frame->bufcntl &= 0xf0ff;
    frame->bufcntl |= role << 8;
    frame->bufcntl &= 0xffdf;
    frame->bufcntl |= 3;
    frame->txbuf0cntl |= 1;
    frame->txbuf1cntl |= 1;
    __write_reg_txphdr(frame, 0, 2);
    __write_reg_txphdr(frame, 1, 2);
    __write_reg_linkcntl(frame, 0, role | 16400);
    __write_reg_linkcntl(frame, 1, role | 16384);
}

/* Open the eSCO relay on the peer link: a copy of the phone's eSCO
 * parameters in a new block, with its own rx/tx buffers, programmed into
 * the baseband eSCO registers.  Returns 1 when opened, 0 when there is no
 * phone eSCO or peer link, the phone's eSCO is a SCO link (type 0) or its
 * T_sco is below 12. */
u8 bredr_esco_tws_link_open(struct link_connection *lc, u8 monitor_start) TWS_CODE;
u8 bredr_esco_tws_link_open(struct link_connection *lc, u8 monitor_start)
{
    struct bredr_esco_link *esco = lc->esco;
    struct bredr_esco_link *tws_esco;
    u8 edr, type_m2s, type_s2m;
    u8 tag;
    int rx_esco_cnt;

    if (!(esco && tws_link.link)) return 0;
    tws_link.remove_esco_timeout = 0;
    if (esco->link_type == 0) return 0;
    if (esco->pdu.T_sco < 12) return 0;

    tws_esco = bt_malloc(80);
    if (!tws_esco) btctrler_assert_failed("tws_esco", 0);
    memcpy(&tws_esco->pdu, &esco->pdu, 14);
    tws_esco->slot = esco->slot;
    tws_esco->link_type = esco->link_type;
    tws_esco->rsvd_state = esco->rsvd_state;
    tws_esco->max_slot_cnt = esco->max_slot_cnt;
    tws_esco->rx_remove_esco_req_flag = 80;

    if (monitor_start) {
        tws_esco->esco_dump_cnt = esco->esco_dump_cnt + 1;
    } else {
        struct list_head *node;

        rx_esco_cnt = 0;
        for (node = esco->rx_esco.next; node != &esco->rx_esco; node = node->next)
            rx_esco_cnt++;
        if (rx_esco_cnt == 0) {
            tws_esco->esco_dump_cnt = esco->esco_dump_cnt + 1;
        } else {
            /* Free all queued phone rx frames but the last. */
            node = esco->rx_esco.next;
            while (node != &esco->rx_esco && rx_esco_cnt != 1) {
                struct list_head *next = node->next;
                __list_del_entry(node);
                bredr_rx_bulk_free((struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry)));
                rx_esco_cnt--;
                node = next;
            }
        }
    }

    tws_link.link->esco = tws_esco;
    INIT_LIST_HEAD(&tws_esco->rx_esco);
    INIT_LIST_HEAD(&tws_esco->tx_esco);

    edr = esco->pdu.packet_type_s2m > 13;
    type_m2s = bredr_get_esco_packet_type(esco->pdu.packet_type_m2s);
    type_s2m = bredr_get_esco_packet_type(esco->pdu.packet_type_s2m);
    tag = log_tag_const_i_TWS_LMP;
    if (tag)
        log_print(2, NULL, "[Info]: [TWS_LMP]pcket_type: %d, %d, %d, %d\n\r\n", type_m2s, type_s2m,
                  esco->pdu.packet_len_m2s, esco->pdu.packet_len_s2m);

    REG32(REG_ESCO_CNTL) = 0;
    __write_reg_escoltcntl(1, edr, esco->link_type == 0, esco->pdu.tws_lt_addr);
    tag = log_tag_const_i_TWS_LMP;
    if (tag)
        log_print(2, NULL, "[Info]: [TWS_LMP]tws_>lt_addr=0x%x,%x,role=%x,monitor=%x\n\r\n",
                  tws_link.link->frame->miscntl & 7, esco->pdu.tws_lt_addr, lc->role, lc->monitor);

    __write_reg_escotrcntl(1, type_m2s, (u16)(esco->pdu.packet_len_m2s + 8), type_s2m,
                           (u16)(esco->pdu.packet_len_s2m + 8));
    __write_reg_escoportcntl(1, esco->pdu.T_sco);
    _write_reg_esco_clkalign(tws_link.link->frame, 1);

    tws_esco->rx_bulk[0] = bredr_rx_bulk_alloc(0, esco->pdu.packet_len_m2s + 8);
    tws_esco->rx_bulk[1] = bredr_rx_bulk_alloc(0, esco->pdu.packet_len_m2s + 8);
    tws_esco->tx_bulk[0] = bredr_tx_bulk_alloc(esco->pdu.packet_len_s2m);
    tws_esco->tx_bulk[1] = bredr_tx_bulk_alloc(esco->pdu.packet_len_s2m);
    memset(tws_esco->tx_bulk[0]->data, 0xaa, esco->pdu.packet_len_s2m);
    memset(tws_esco->tx_bulk[1]->data, 0xaa, esco->pdu.packet_len_s2m);

    {
        u32 second = (u32)(uintptr_t)tws_esco->rx_bulk[1]->data - REG32(REG_BUF_BASE);
        u32 first = (u32)(uintptr_t)tws_esco->rx_bulk[0]->data - REG32(REG_BUF_BASE);
        REG32(REG_ESCO_RXPTR) = first | second << 16;
    }
    {
        u32 second = (u32)(uintptr_t)tws_esco->tx_bulk[1]->data - REG32(REG_BUF_BASE);
        u32 first = (u32)(uintptr_t)tws_esco->tx_bulk[0]->data - REG32(REG_BUF_BASE);
        REG32(REG_ESCO_TXPTR) = first | second << 16;
    }
    __pcm_out_enable(1);
    return 1;
}

/* Close the eSCO relay: on the TWS link itself when lc is one, else on the
 * stored peer link.  Frees the relay's buffers and lists, returns its slot
 * timer and the block.  0 when there is no relay to close, else 1. */
u8 bredr_esco_tws_link_close(struct link_connection *lc) TWS_CODE;
u8 bredr_esco_tws_link_close(struct link_connection *lc)
{
    struct link_connection *tws_lc = lc->tws_link ? lc : tws_link.link;
    struct list_head *node, *next;
    u8 tag, timer;
    int i;

    if (!tws_lc) return 0;
    if (!tws_lc->esco) return 0;

    local_irq_disable();
    tag = log_tag_const_i_TWS_LMP;
    if (tag)
        log_print(2, NULL, "[Info]: [TWS_LMP]clos_tws_lc->esco->pdu.tws_lt_addr=%d\n\r\n",
                  tws_lc->esco->pdu.tws_lt_addr);
    bredr_rx_bulk_free(tws_lc->esco->rx_bulk[0]);
    bredr_rx_bulk_free(tws_lc->esco->rx_bulk[1]);
    for (i = 0; i < 3; i++)
        if (tws_lc->esco->tx_bulk[i]) bredr_tx_bulk_free(tws_lc->esco->tx_bulk[i]);
    for (node = tws_lc->esco->tx_esco.next; node != &tws_lc->esco->tx_esco; node = next) {
        next = node->next;
        bredr_tx_bulk_free((struct bredr_tx_bulk *)((u8 *)node - offsetof(struct bredr_tx_bulk, entry)));
    }
    for (node = tws_lc->esco->rx_esco.next; node != &tws_lc->esco->rx_esco; node = next) {
        next = node->next;
        __list_del_entry(node);
        bredr_rx_bulk_free((struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry)));
    }
    timer = tws_link.slot_instant_timer;
    if (timer) {
        slot_timer_put(timer);
        tws_link.slot_instant_timer = 0;
    }
    bt_free(tws_lc->esco);
    tws_lc->esco = 0;
    local_irq_enable();
    if (tag) log_print(2, NULL, "[Info]: [TWS_LMP]%s\r\n", __func__);
    return 1;
}

/* Sync channel 1 of the TWS channel table: link state, both directions. */
const struct tws_sync_channel tws_link_sync __attribute__((section(".tws_sync_channel.1"))) = {
    .channel = 1,
    .attribute = 2,
    .tx_sync_req = tws_link_tx_sync_req,
    .rx_sync_req_and_res = tws_link_rx_sync_req,
};

#ifdef UNITTEST
#include "test.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>

BTCTRLER_CONFIG u8 log_tag_const_i_TWS_LMP;

/* ---- stubs: a trace of letters, and scripted results ---- */
static char trace[256];
static void tr(char c) { size_t n = strlen(trace); trace[n] = c; trace[n + 1] = 0; }
static int putchars, putchar_arg;
int test_putchar(int c) { putchars++; putchar_arg = c; return c; }

static u32 master_clk, slot_clk;
static struct link_connection *master_clk_arg;
u32 bredr_get_master_slot_clk(struct link_connection *lc) { master_clk_arg = lc; return master_clk; }
u32 bredr_read_slot_clk() { return slot_clk; }
void local_irq_disable(void) { tr('d'); }
void local_irq_enable(void) { tr('u'); }
static struct ctrl_frame *mon_frame;
void tws_link_monitor_slave_enable(struct ctrl_frame *f) { mon_frame = f; tr('e'); }
void tws_link_monitor_master_enable(struct ctrl_frame *f) { mon_frame = f; tr('E'); }
void tws_link_monitor_slave_init(struct ctrl_frame *f) { mon_frame = f; tr('i'); }
void tws_link_monitor_master_init(struct ctrl_frame *f) { mon_frame = f; tr('I'); }
static int pwr_sel, pwr_inc;
int bredr_pwr_set(struct link_connection *lc, char sel, char inc) { (void)lc; pwr_sel = sel; pwr_inc = inc; tr('P'); return 0; }
static int event_code, event_arg;
static const char *event_format;
void tws_conn_send_event(int code, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    event_code = code; event_format = format; event_arg = va_arg(ap, int);
    va_end(ap);
    tr('V');
}
int link_conn_tx_empty(struct link_connection *lc) { (void)lc; tr('T'); return 0; }

static struct { struct ctrl_frame *frame; int index; int value; } seqn_writes[8];
static int seqn_count;
void __write_reg_seqn(struct ctrl_frame *f, int index, u8 seqn)
{
    if (seqn_count < 8) { seqn_writes[seqn_count].frame = f; seqn_writes[seqn_count].index = index; seqn_writes[seqn_count].value = seqn; }
    seqn_count++;
}
static u16 packet_seqn[2];
u16 __read_reg_packet_seqn(struct ctrl_frame *f, int index) { (void)f; return packet_seqn[index]; }
static s32 clkoffset_of[3];            /* by frame_id() */
static struct ctrl_frame frame_a, frame_b, frame_c;
static int frame_id(struct ctrl_frame *f) { return f == &frame_a ? 0 : f == &frame_b ? 1 : 2; }
static int clk_reads;
static int mutate_core_offset_on_clk_read;
static struct tws_link_core_data *mutate_core_offset;
static s32 mutated_core_offset;
static int mutate_phone_bitoff_on_clk_read;
static u16 mutated_phone_bitoff;
s32 __read_reg_clkoffset(struct ctrl_frame *f)
{
    int read = ++clk_reads;
    if (read == mutate_core_offset_on_clk_read && mutate_core_offset)
        mutate_core_offset->offset = mutated_core_offset;
    if (read == mutate_phone_bitoff_on_clk_read)
        frame_a.bitoff = mutated_phone_bitoff;
    return clkoffset_of[frame_id(f)];
}
static struct ctrl_frame *clk_written_frame;
static s32 clk_written;
static int clk_writes;
void __write_reg_clkoffset(struct ctrl_frame *f, s32 v) { clk_written_frame = f; clk_written = v; clk_writes++; }
static int linkcntl_val[2], linkcntl_n, txphdr_val[2], txphdr_n;
void __write_reg_linkcntl(struct ctrl_frame *f, int index, int val) { (void)f; linkcntl_val[index] = val; linkcntl_n++; }
void __write_reg_txphdr(struct ctrl_frame *f, int index, u16 val) { (void)f; txphdr_val[index] = val; txphdr_n++; }
struct ctrl_frame *bredr_bd_get_frame() { return &frame_c; }

static int after_ret[4], after_n, after_arg[8];
int bredr_clkn_after(int a, int b)
{
    if (after_n < 4) { after_arg[after_n * 2] = a; after_arg[after_n * 2 + 1] = b; }
    return after_ret[after_n++ & 3];
}
static int off_begin, off_end, off_ret;
int bredr_clkn2offset(int begin, int end) { off_begin = begin; off_end = end; return off_ret; }
static u32 hash_ret;
static int hashes;
u32 JBHash(u8 *data, int len) { (void)data; CHECK(len == 44); hashes++; return hash_ret; }
static int ptt_arg, enc_rx, enc_tx;
void link_conn_set_ptt(struct link_connection *lc, u8 ptt) { (void)lc; ptt_arg = ptt; }
void link_conn_set_encrypt(struct link_connection *lc, u8 rx, u8 tx) { (void)lc; enc_rx = rx; enc_tx = tx; }
static int afh_calls; static u32 afh_end_arg; static int afh_en_arg; static u8 *afh_map_arg;
void bredr_link_set_afh(struct link_connection *lc, u32 end, u8 en, u8 *map)
{
    (void)lc; afh_calls++; afh_end_arg = end; afh_en_arg = en; afh_map_arg = map;
}
static int rx_avail_calls, rx_avail_set;
static struct link_connection *rx_avail_link;
void link_conn_rx_bulk_avaliable(struct link_connection *lc, struct bredr_rx_bulk *b)
{
    CHECK(b == NULL); rx_avail_calls++; lc->rx_pend = rx_avail_set;
}
static int posts, post_clears;
static int mutate_txtog_after_post;
static int *post_argv;
int os_taskq_post_type(const char *name, int type, int argc, int *argv)
{
    CHECK(!strcmp(name, "tws") && type == 54 && argc == 1);
    posts++; post_argv = argv; tr('Q');
    if (post_clears) rx_avail_link->rx_pend = 0;
    if (mutate_txtog_after_post >= 0) rx_avail_link->frame->txtog = (u8)mutate_txtog_after_post;
    return 0;
}

static jmp_buf escape;
static int asserts;
static char assert_what[64];
void btctrler_assert_failed(const char *what, int value)
{
    (void)value; asserts++; snprintf(assert_what, sizeof(assert_what), "%s", what);
    longjmp(escape, 1);
}
static void *malloc_result;
static int malloc_size;
void *bt_malloc(int size) { malloc_size = size; return malloc_result; }
static void *freed[16];
static int nfreed;
void bt_free(void *p) { if (nfreed < 16) freed[nfreed] = p; nfreed++; tr('f'); }
static void *bulk_freed[16];
static int nbulk_freed;
void bredr_rx_bulk_free(struct bredr_rx_bulk *b) { if (nbulk_freed < 16) bulk_freed[nbulk_freed] = b; nbulk_freed++; tr('r'); }
void bredr_tx_bulk_free(struct bredr_tx_bulk *b) { if (nbulk_freed < 16) bulk_freed[nbulk_freed] = b; nbulk_freed++; tr('t'); }
static struct bredr_rx_bulk *rx_allocs[4];
static int rx_alloc_n, rx_alloc_len[4], rx_alloc_id[4];
struct bredr_rx_bulk *bredr_rx_bulk_alloc(int id, int len)
{
    struct bredr_rx_bulk *b = calloc(1, sizeof(*b) + 400);
    rx_allocs[rx_alloc_n & 3] = b; rx_alloc_len[rx_alloc_n & 3] = len; rx_alloc_id[rx_alloc_n & 3] = id; rx_alloc_n++;
    return b;
}
static struct bredr_tx_bulk *tx_allocs[4];
static int tx_alloc_n, tx_alloc_len[4];
struct bredr_tx_bulk *bredr_tx_bulk_alloc(int size)
{
    struct bredr_tx_bulk *b = calloc(1, sizeof(*b) + 400);
    tx_allocs[tx_alloc_n & 3] = b; tx_alloc_len[tx_alloc_n & 3] = size; tx_alloc_n++;
    return b;
}
static int timer_put;
void slot_timer_put(u8 i) { timer_put = i; tr('s'); }
static int pcm_enable;
void __pcm_out_enable(u8 channel) { pcm_enable = channel; tr('p'); }
static struct ctrl_frame *align_frame; static int align_en;
void _write_reg_esco_clkalign(struct ctrl_frame *f, u8 en) { align_frame = f; align_en = en; }
static int ltc[4], trc[5], portc[2];
void __write_reg_escoltcntl(u8 ch, u8 edr, u8 type, u8 lt) { ltc[0] = ch; ltc[1] = edr; ltc[2] = type; ltc[3] = lt; }
static int trcnt_len_m2s, trcnt_len_s2m;
void __write_reg_escotrcntl(u8 ch, u8 tm, u16 lm, u8 ts, u16 ls)
{
    trc[0] = ch; trc[1] = tm; trc[2] = ts; trcnt_len_m2s = lm; trcnt_len_s2m = ls;
}
void __write_reg_escoportcntl(u8 ch, u8 t) { portc[0] = ch; portc[1] = t; }
static int type_calls[4], type_n;
static struct bredr_esco_link *mutate_esco_packet_type;
u8 bredr_get_esco_packet_type(u8 packet)
{
    type_calls[type_n++ & 3] = packet;
    if (type_n == 1 && mutate_esco_packet_type)
        mutate_esco_packet_type->pdu.packet_type_s2m = 14;
    return packet + 100;
}
static int logs;
static const char *log_format[4];
static int log_args[4][4];
void log_print(int level, const char *tag, const char *format, ...)
{
    va_list ap;
    int i;
    CHECK(level == 2 && tag == NULL);
    va_start(ap, format);
    if (logs < 4) {
        log_format[logs] = format;
        for (i = 0; i < 4; i++) log_args[logs][i] = va_arg(ap, int);
    }
    va_end(ap);
    logs++;
}

static struct link_connection phone, peer;
static struct lmp_connection conn;

static void reset(void)
{
    int i;
    memset(&tws_link, 0, sizeof(tws_link));
    memset(&phone, 0, sizeof(phone)); memset(&peer, 0, sizeof(peer)); memset(&conn, 0, sizeof(conn));
    memset(&frame_a, 0, sizeof(frame_a)); memset(&frame_b, 0, sizeof(frame_b)); memset(&frame_c, 0, sizeof(frame_c));
    for (i = 0; i < 0x400; i++) test_regs[i] = 0;
    trace[0] = 0; putchars = 0; hashes = 0; seqn_count = 0; clk_reads = clk_writes = 0; linkcntl_n = txphdr_n = 0;
    after_n = 0; memset(after_ret, 0, sizeof(after_ret)); afh_calls = 0; nfreed = nbulk_freed = 0;
    rx_alloc_n = tx_alloc_n = 0; type_n = 0; logs = 0; asserts = 0; posts = 0; post_clears = 0;
    rx_avail_calls = rx_avail_set = 0; mutate_txtog_after_post = -1;
    log_tag_const_i_TWS_LMP = 0; clkoffset_of[0] = clkoffset_of[1] = clkoffset_of[2] = 0;
    mutate_core_offset_on_clk_read = 0; mutate_core_offset = NULL; mutated_core_offset = 0;
    mutate_phone_bitoff_on_clk_read = 0; mutated_phone_bitoff = 0; mutate_esco_packet_type = NULL;
    packet_seqn[0] = packet_seqn[1] = 0; malloc_result = NULL; mon_frame = NULL;
    phone.frame = &frame_a; peer.frame = &frame_b; conn.link = &phone;
}

static int count(struct list_head *h) { int n = 0; for (struct list_head *e = h->next; e != h; e = e->next) n++; return n; }
static void list_init(struct list_head *h) { h->next = h->prev = h; }

int main(void)
{
    int i;

    _Static_assert(sizeof(struct tws_link_core_data) == 44, "core data");
    _Static_assert(offsetof(struct tws_link_core_data, offset) == 40, "offset");

    /* read_slot_clk: no peer link reads 0 without asking */
    reset(); master_clk = 0x1234; master_clk_arg = NULL;
    CHECK(tws_link_read_slot_clk() == 0 && master_clk_arg == NULL);
    tws_link.link = &peer;
    CHECK(tws_link_read_slot_clk() == 0x1234 && master_clk_arg == &peer);

    /* monitor_enable: en picks enable/init, role picks master/slave */
    reset(); tws_monitor_enable(&phone, 1, 1); CHECK(!strcmp(trace, "dEu") && mon_frame == &frame_a && phone.monitor_en == 1);
    reset(); tws_monitor_enable(&phone, 1, 0); CHECK(!strcmp(trace, "deu") && phone.monitor_en == 1);
    reset(); phone.monitor_en = 9; tws_monitor_enable(&phone, 0, 1); CHECK(!strcmp(trace, "dIu") && phone.monitor_en == 0);
    reset(); tws_monitor_enable(&phone, 0, 0); CHECK(!strcmp(trace, "diu") && phone.monitor_en == 0);
    reset(); tws_monitor_enable(&phone, 7, 200); CHECK(!strcmp(trace, "dEu") && phone.monitor_en == 7);   /* any nonzero role is master */

    /* master_2_slave: monitor flag set, monitor on as a slave, power -9, event "1" 1 */
    reset(); phone.monitor_en = 5; pwr_sel = pwr_inc = -1;
    tws_link_master_2_slave(&phone);
    CHECK(phone.monitor == 1 && !strcmp(trace, "ddeuPuV") && pwr_sel == 0 && pwr_inc == 9);
    CHECK(event_code == 10 && !strcmp(event_format, "1") && event_arg == 1);

    /* slave_2_master without an eSCO link */
    reset(); phone.monitor = 1; phone.monitor_en = 0; phone.tx_bulk[0] = (void *)1; phone.tx_bulk[1] = (void *)2;
    tws_link_slave_2_master(&phone);
    CHECK(phone.monitor == 0 && phone.tx_bulk[0] == 0 && phone.tx_bulk[1] == 0);
    CHECK(frame_a.txbuf0cntl == 1 && frame_a.txbuf1cntl == 1 && seqn_count == 0);
    CHECK(!strcmp(trace, "TddIuuV"));               /* monitor_en 0, role 1: master init, inside the outer irq-off */
    CHECK(event_code == 10 && event_arg == 0);
    /* with an eSCO link: equal toggle bits keep the sequence bits, unequal ones swap */
    {
        struct bredr_esco_link esco;
        memset(&esco, 0, sizeof(esco));
        reset(); phone.esco = &esco; esco.remote_seqn = 3; esco.remote_txtog = 1; frame_a.txtog = 0x81;   /* bit 0 equal */
        tws_link_slave_2_master(&phone);
        CHECK(seqn_count == 2 && seqn_writes[0].frame == &frame_a && seqn_writes[0].index == 0 && seqn_writes[0].value == 1);
        CHECK(seqn_writes[1].index == 1 && seqn_writes[1].value == 2);
        reset(); phone.esco = &esco; esco.remote_seqn = 3; esco.remote_txtog = 0; frame_a.txtog = 1;      /* bit 0 differs */
        tws_link_slave_2_master(&phone);
        CHECK(seqn_count == 2 && seqn_writes[0].index == 1 && seqn_writes[0].value == 1);
        CHECK(seqn_writes[1].index == 0 && seqn_writes[1].value == 2);
        reset(); phone.esco = &esco; esco.remote_seqn = 0xfc; esco.remote_txtog = 2; frame_a.txtog = 0;   /* only bit 0 counts */
        tws_link_slave_2_master(&phone);
        CHECK(seqn_writes[0].index == 0 && seqn_writes[0].value == 0 && seqn_writes[1].value == 0);
    }

    /* monitor_frame_init: 0x0f20 -> clear 0xf00 -> clear 0x20 -> set 3; window register + 20 */
    reset(); frame_c.bufcntl = 0x0f20; REG32(REG_WIN_BASE) = 100;
    CHECK(tws_link_monitor_frame_init() == &frame_c);
    CHECK(frame_c.bufcntl == 3 && frame_c.wincntl == 120 && frame_c.txbuf0cntl == 1 && frame_c.txbuf1cntl == 1);
    CHECK(linkcntl_n == 2 && linkcntl_val[0] == 16400 && linkcntl_val[1] == 16384 && txphdr_n == 2 && txphdr_val[0] == 2 && txphdr_val[1] == 2);
    reset(); frame_c.bufcntl = 0xffff; frame_c.txbuf0cntl = 0x10; REG32(REG_WIN_BASE) = 0xffff0000u | 65530;
    tws_link_monitor_frame_init();
    CHECK(frame_c.bufcntl == 0xf0df && frame_c.wincntl == (u16)(65530 + 20) && frame_c.txbuf0cntl == 0x11);

    /* monitor_frame_setup: offsets move onto the peer link; carry past 624 bit offsets */
    {
        struct bredr_frame_core_data d;
        reset(); tws_link.link = &peer;
        memset(&d, 0, sizeof(d));
        frame_b.clk_offsetl = 0x1111; frame_b.clk_offseth = 2; frame_b.bitoff = 0x0500 | 0x100;   /* 256 under the mask */
        d.miscntl = 0x21; d.format = 0x52; d.bdaddr0 = 0xa0; d.bdaddr1 = 0xa1; d.bdaddr2 = 0xa2;
        d.bchl = 0xb0; d.bchm = 0xb1; d.bchh = 0xb2; d.clk_offsetl = 1; d.clk_offseth = 0; d.bitoff = 100;
        d.rx_enc = 1; d.tx_enc = 1; for (i = 0; i < 8; i++) d.cryptk[i] = 0x100 + i;
        frame_c.linkcntl0 = 0x4010; frame_c.linkcntl1 = 0x4000;
        tws_link_monitor_frame_setup(&frame_c, &d);
        CHECK(frame_c.miscntl == 0x31 && frame_c.format == 0x52 && frame_c.bdaddr0 == 0xa0 && frame_c.bdaddr1 == 0xa1 && frame_c.bdaddr2 == 0xa2);
        CHECK(frame_c.bchl == 0xb0 && frame_c.bchm == 0xb1 && frame_c.bchh == 0xb2);
        CHECK(frame_c.bitoff == 356 && clk_written_frame == &frame_c && clk_written == 0x21112 && clk_writes == 1);
        for (i = 0; i < 8; i++) CHECK(frame_c.cryptk[i] == 0x100 + i);
        CHECK(frame_c.linkcntl0 == (0x4010 | 0x8000 | 0x80) && frame_c.linkcntl1 == (0x4000 | 0x8000 | 0x80));
        CHECK(!strcmp(trace, "i") && mon_frame == &frame_c);
        /* 256 + 400 = 656 > 624: bit offset 31, clock offset one less */
        reset(); tws_link.link = &peer; frame_b.clk_offsetl = 0x1111; frame_b.clk_offseth = 2; frame_b.bitoff = 0x100;
        d.bitoff = 400; d.rx_enc = 0; d.tx_enc = 0; frame_c.linkcntl0 = 5; frame_c.linkcntl1 = 6;
        tws_link_monitor_frame_setup(&frame_c, &d);
        CHECK(frame_c.bitoff == 31 && clk_written == 0x21112 - 1 && frame_c.linkcntl0 == 5 && frame_c.linkcntl1 == 6);
        /* exactly 624 does not carry */
        reset(); tws_link.link = &peer; frame_b.bitoff = 224; d.bitoff = 400;
        tws_link_monitor_frame_setup(&frame_c, &d);
        CHECK(frame_c.bitoff == 624 && clk_written == 1);
        /* clock offset words add as 32 bits */
        reset(); tws_link.link = &peer; frame_b.clk_offsetl = 0xffff; frame_b.clk_offseth = 0xffff; d.clk_offsetl = 2; d.clk_offseth = 0; d.bitoff = 0;
        tws_link_monitor_frame_setup(&frame_c, &d);
        CHECK(clk_written == 1);
    }

    /* tx sync request: description of the phone link */
    {
        u8 buf[64] __attribute__((aligned(4)));
        const struct tws_link_core_data *core = (const struct tws_link_core_data *)buf;
        struct lmp_esco esco_conn;

        reset(); frame_a.miscntl = 0x08; frame_a.linkcntl0 = 0x8040; frame_a.format = 0x40;
        phone.afh_slot_end = 0x1234; phone.super_timeout_begin = 5; slot_clk = 1000; off_ret = 2400;
        for (i = 0; i < 10; i++) phone.channel_map[i] = i + 1;
        for (i = 0; i < 8; i++) frame_a.cryptk[i] = 0x0100 * (i + 1) + i;
        memset(buf, 0xee, sizeof(buf)); hash_ret = 7;
        CHECK(tws_link_tx_sync_req(&conn, buf, 0) == 44);
        CHECK(core->ptt == 1 && core->afh_enable == 1 && core->rx_enc == 1 && core->tx_enc == 0);
        CHECK(core->afh_end == 0x1234 && core->super_timeout_cnt == 3 && off_begin == 5 && off_end == 1000);
        for (i = 0; i < 10; i++) CHECK(core->channel_map[i] == i + 1);
        for (i = 0; i < 8; i++) CHECK(core->cryptk[i] == 0x0100 * (i + 1) + i);
        CHECK(core->offset == 0 && core->bitoff == 0 && core->rev == 0);
        CHECK(buf[44] == 0xee);
        CHECK(tws_link.core_hash == 7 && tws_link.wait_tx_suss == 1 && tws_link.tx_req_cnt == 0 && hashes == 1);
        /* same hash: counts up and clears the wait on a successful previous request */
        CHECK(tws_link_tx_sync_req(&conn, buf, 0) == 0 && tws_link.tx_req_cnt == 1 && tws_link.wait_tx_suss == 0);
        CHECK(tws_link_tx_sync_req(&conn, buf, 0) == 0 && tws_link.tx_req_cnt == 2);
        /* waiting and the previous request failed: send again, keep waiting */
        tws_link.wait_tx_suss = 1;
        CHECK(tws_link_tx_sync_req(&conn, buf, 1) == 44 && tws_link.wait_tx_suss == 1 && tws_link.tx_req_cnt == 3);
        /* a changed hash and the 30th unchanged request both send and restart */
        hash_ret = 8; tws_link.wait_tx_suss = 0;
        CHECK(tws_link_tx_sync_req(&conn, buf, 0) == 44 && tws_link.core_hash == 8 && tws_link.wait_tx_suss == 1 && tws_link.tx_req_cnt == 0);
        tws_link.tx_req_cnt = 29; tws_link.wait_tx_suss = 0;
        CHECK(tws_link_tx_sync_req(&conn, buf, 0) == 0 && tws_link.tx_req_cnt == 30);
        CHECK(tws_link_tx_sync_req(&conn, buf, 0) == 44 && tws_link.tx_req_cnt == 0 && tws_link.wait_tx_suss == 1);
        /* the offset report: clock offset difference and bit offset difference */
        reset(); tws_link.link = &peer; tws_link.sync_clkn_offset = 1; hash_ret = 1;
        clkoffset_of[0] = 1000; clkoffset_of[1] = 400; frame_a.bitoff = 0x0400 | 300; frame_b.bitoff = 100;
        tws_link_tx_sync_req(&conn, buf, 0);
        CHECK(tws_link.sync_clkn_offset == 0 && core->offset == 600 && core->bitoff == 200);
        frame_a.bitoff = 100; frame_b.bitoff = 0x0800 | 300; tws_link.sync_clkn_offset = 9;
        tws_link_tx_sync_req(&conn, buf, 0);
        CHECK(core->offset == 601 && core->bitoff == 425);        /* negative bit offset borrows */
        frame_a.bitoff = 300; frame_b.bitoff = 300; tws_link.sync_clkn_offset = 1;
        tws_link_tx_sync_req(&conn, buf, 0);
        CHECK(core->offset == 600 && core->bitoff == 0);          /* zero does not */
        clkoffset_of[0] = 5; clkoffset_of[1] = 9; tws_link.sync_clkn_offset = 1;
        tws_link_tx_sync_req(&conn, buf, 0);
        CHECK(core->offset == (u32)-4);
        /* The phone bitoff load sits between the two clock reads. A
         * mutation during the second read must not change that value. */
        reset(); tws_link.link = &peer; tws_link.sync_clkn_offset = 1; hash_ret = 1;
        frame_a.bitoff = 300; frame_b.bitoff = 100;
        mutate_phone_bitoff_on_clk_read = 2; mutated_phone_bitoff = 500;
        tws_link_tx_sync_req(&conn, buf, 0);
        CHECK(clk_reads == 2 && core->bitoff == 200 && frame_a.bitoff == 500);
        /* with an eSCO link: only every fifth request is "sent" (a 'Z' on the console) */
        reset(); conn.esco_conn = &esco_conn; hash_ret = 1; tws_link.esco_tx_sync_req_cnt = 6;
        CHECK(tws_link_tx_sync_req(&conn, buf, 1) == 0 && tws_link.esco_tx_sync_req_cnt == 6 && putchars == 0);
        CHECK(tws_link_tx_sync_req(&conn, buf, 0) == 44 && tws_link.esco_tx_sync_req_cnt == 5 && putchars == 1 && putchar_arg == 90);
        CHECK(tws_link_tx_sync_req(&conn, buf, 0) == 0 && tws_link.esco_tx_sync_req_cnt == 4 && putchars == 1);
        tws_link.esco_tx_sync_req_cnt = 1;                    /* 0 % 5 == 0: the last one counts too */
        CHECK(tws_link_tx_sync_req(&conn, buf, 0) == 44 && tws_link.esco_tx_sync_req_cnt == 0 && putchars == 2);
        CHECK(tws_link_tx_sync_req(&conn, buf, 0) == 0 && tws_link.esco_tx_sync_req_cnt == 0 && putchars == 2);   /* then idle */
        tws_link.esco_tx_sync_req_cnt = 11;
        CHECK(tws_link_tx_sync_req(&conn, buf, 0) == 44 && tws_link.esco_tx_sync_req_cnt == 10 && putchars == 3);
        CHECK(hashes == 0 && tws_link.core_hash == 0 && tws_link.wait_tx_suss == 0);
    }

    /* rx sync request: apply the peer's description */
    {
        u8 buf[64] __attribute__((aligned(4)));
        struct tws_link_core_data *core = (struct tws_link_core_data *)buf;

        reset(); memset(buf, 0, sizeof(buf));
        core->ptt = 1; core->rx_enc = 1; core->tx_enc = 0; core->super_timeout_cnt = 2; core->afh_enable = 1; core->afh_end = 77;
        for (i = 0; i < 10; i++) core->channel_map[i] = 0x10 + i;
        for (i = 0; i < 8; i++) core->cryptk[i] = 0x200 + i;
        slot_clk = 5000; phone.super_timeout_begin = 100; phone.afh_slot_end = 50;
        after_ret[0] = 1; after_ret[1] = 0;
        CHECK(tws_link_rx_sync_req(&conn, buf, 44, NULL) == 0);
        CHECK(ptt_arg == 1 && enc_rx == 1 && enc_tx == 0);
        for (i = 0; i < 8; i++) CHECK(frame_a.cryptk[i] == 0x200 + i);
        CHECK(after_n == 2 && after_arg[0] == 3400 && after_arg[1] == 100 && phone.super_timeout_begin == 3400);   /* 5000 - 2 * 800 */
        CHECK(after_arg[2] == 77 && after_arg[3] == 50 && afh_calls == 0);                               /* not after, and a map is set */
        CHECK(clk_reads == 0 && clk_writes == 0);
        /* not later than the begin we hold: kept; later AFH end: map applied; offset 0: no frame update */
        reset(); phone.super_timeout_begin = 100; phone.afh_slot_end = 50; slot_clk = 5000; after_ret[0] = 0; after_ret[1] = 1;
        tws_link_rx_sync_req(&conn, buf, 44, NULL);
        CHECK(phone.super_timeout_begin == 100 && afh_calls == 1 && afh_end_arg == 77 && afh_en_arg == 1 && afh_map_arg == core->channel_map);
        /* clock before the timeout span: neither compare nor store; no AFH map held: applied */
        reset(); phone.super_timeout_begin = 100; phone.afh_slot_end = 0; slot_clk = 1599; after_ret[0] = 0;
        tws_link_rx_sync_req(&conn, buf, 44, NULL);
        CHECK(phone.super_timeout_begin == 100 && after_n == 1 && afh_calls == 1);
        reset(); slot_clk = 1600; after_ret[0] = 1; phone.super_timeout_begin = 100;                    /* equal: not before */
        tws_link_rx_sync_req(&conn, buf, 44, NULL);
        CHECK(phone.super_timeout_begin == 0 && after_n == 2);
        reset(); slot_clk = (u32)-5; after_ret[0] = 1;                                                    /* clock as a signed int */
        tws_link_rx_sync_req(&conn, buf, 44, NULL);
        CHECK(after_n == 1 && phone.super_timeout_begin == 0);
        /* offsets: peer clock offset + ours, peer bit offset (masked) + ours (signed) */
        reset(); tws_link.link = &peer; core->offset = 1000; core->bitoff = -50;
        clkoffset_of[1] = 400; frame_b.bitoff = 0x0400 | 200; frame_a.bitoff = 7;
        tws_link_rx_sync_req(&conn, buf, 44, NULL);
        CHECK(clk_writes == 1 && clk_written_frame == &frame_a && clk_written == 1400 && frame_a.bitoff == 150);
        CHECK(clk_reads == 2);                                   /* our own register is read and dropped */
        core->bitoff = -300; tws_link_rx_sync_req(&conn, buf, 44, NULL);
        CHECK(frame_a.bitoff == (u16)-100);                      /* negative stays negative */
        core->bitoff = 424; tws_link_rx_sync_req(&conn, buf, 44, NULL);
        CHECK(frame_a.bitoff == 624 && clk_written == 1400);     /* 624 does not carry */
        core->bitoff = 425; tws_link_rx_sync_req(&conn, buf, 44, NULL);
        CHECK(frame_a.bitoff == 0 && clk_written == 1399);
        core->offset = 0xfffffff0u; clkoffset_of[1] = 0x20; core->bitoff = 0;
        tws_link_rx_sync_req(&conn, buf, 44, NULL);
        CHECK(clk_written == 0x10);
        /* The guard uses the initial nonzero offset; the peer clock addition
         * reloads the buffer field after the second register-read call. */
        core->offset = 1000; core->bitoff = 0; clkoffset_of[1] = 400;
        clk_reads = 0;
        mutate_core_offset = core; mutate_core_offset_on_clk_read = 2; mutated_core_offset = 2000;
        tws_link_rx_sync_req(&conn, buf, 44, NULL);
        CHECK(clk_reads == 2 && clk_written == 2400 && core->offset == 2000);
    }

    /* rxtx state get/set */
    {
        struct tws_link_rxtx_state st;
        struct bredr_esco_link esco;
        reset(); frame_a.rxtog = 1; frame_a.txtog = 0x81; frame_a.rxstat0 = 0x1234; frame_a.rxstat1 = 0x5678;
        memset(&st, 0xee, sizeof(st));
        CHECK(tws_link_get_rxtx_state(&phone, &st) == 8);
        CHECK(st.rxtog == 1 && st.txtog == 0x81 && st.rxstat0 == 0x1234 && st.rxstat1 == 0x5678 && st.seqn == 0);
        packet_seqn[0] = 0x10; tws_link_get_rxtx_state(&phone, &st); CHECK(st.seqn == 1);
        packet_seqn[0] = 0; packet_seqn[1] = 1; tws_link_get_rxtx_state(&phone, &st); CHECK(st.seqn == 2);
        packet_seqn[0] = 1; tws_link_get_rxtx_state(&phone, &st); CHECK(st.seqn == 3);

        reset(); memset(&esco, 0, sizeof(esco)); phone.esco = &esco;
        st.rxtog = 1; st.txtog = 0x81; st.rxstat0 = 0x1111; st.rxstat1 = 0x2222; st.seqn = 3;
        CHECK(tws_link_set_rxtx_state(&phone, &st) == 8);
        CHECK(esco.remote_txtog == 0x81 && esco.remote_seqn == 3 && frame_a.rxtog == 0 && frame_a.txtog == 0 && seqn_count == 0);
        reset(); frame_a.txbuf0cntl = 0x20;
        CHECK(tws_link_set_rxtx_state(&phone, &st) == 8);
        CHECK(frame_a.rxtog == 1 && frame_a.txtog == 0x81 && frame_a.rxstat0 == 0x1111 && frame_a.rxstat1 == 0x2222);
        CHECK(frame_a.txbuf0cntl == 0x21 && frame_a.txbuf1cntl == 1);
        CHECK(seqn_count == 2 && seqn_writes[0].index == 0 && seqn_writes[0].value == 1 && seqn_writes[1].index == 1 && seqn_writes[1].value == 2);
        st.seqn = 0xfc; seqn_count = 0; tws_link_set_rxtx_state(&phone, &st);
        CHECK(seqn_writes[0].value == 0 && seqn_writes[1].value == 0x0 + (0xfc & 2));
    }

    /* rxtx state reset */
    {
        struct bredr_tx_bulk *t0, *t1;
        struct list_head dummy;
        u32 base = 0x100000;
#define OFFSET_OF(b, p) ((u16)((u16)(p) + base - (u32)(uintptr_t)(b)->data))

        for (i = 0; i < 6; i++) {
            reset(); list_init(&phone.tx_acl_c); list_init(&phone.tx_acl_u); tws_link.link = &phone;
            rx_avail_link = &phone; REG32(REG_BUF_BASE) = base; list_init(&dummy);
            t0 = calloc(1, sizeof(*t0) + 64); t1 = calloc(1, sizeof(*t1) + 64);
            t0->len = 10; t1->len = 20; t0->offset = 3; t1->offset = 4; t0->llid = 2; t1->llid = 3;
            list_add(&t0->entry, &dummy); list_add(&t1->entry, &dummy);        /* both in a list to leave */
            frame_a.txptr0 = 0x300; frame_a.txptr1 = 0x400; frame_a.txtog = 0xff; frame_a.bufcntl = 0xffff;
            frame_a.rxtog = 1; frame_a.rxstat0 = 0xabc; frame_a.rxstat1 = 0xdef;
            phone.tx_bulk[0] = t0; phone.tx_bulk[1] = t1;
            if (i == 0) {                       /* toggle bit set: the packets trade places */
                phone.role = 0;
                tws_link_rxtx_state_reset();
                CHECK(phone.tx_acl_c.next == &t1->entry && phone.tx_acl_c.next->next == &phone.tx_acl_c);   /* llid 3 -> control queue */
                CHECK(phone.tx_acl_u.next == &t0->entry && phone.tx_acl_u.next->next == &phone.tx_acl_u);
                CHECK(t0->offset == OFFSET_OF(t0, 0x300) && t1->offset == OFFSET_OF(t1, 0x400));   /* each with its own pointer */
                CHECK(count(&dummy) == 0);
                CHECK(frame_a.txtog == 0xf8 && frame_a.bufcntl == 0xf1df && frame_a.txbuf0cntl == 1 && frame_a.txbuf1cntl == 1);
                CHECK(linkcntl_val[0] == 16401 && linkcntl_val[1] == 16385 && txphdr_val[0] == 2 && txphdr_val[1] == 2);
                CHECK(frame_a.rxstat0 == 0x90 && frame_a.rxstat1 == 0x90 && frame_a.rxtog == 0 && rx_avail_calls == 1 && posts == 0);
                CHECK(phone.tx_bulk[0] == 0 && phone.tx_bulk[1] == 0);
            } else if (i == 1) {                /* toggle clear, same llid: the second is queued first, so the first leads */
                t0->llid = t1->llid = 2; frame_a.txtog = 0xfe; phone.role = 1;
                tws_link_rxtx_state_reset();
                CHECK(phone.tx_acl_u.next == &t0->entry && t0->entry.next == &t1->entry && t1->entry.next == &phone.tx_acl_u);
                CHECK(frame_a.bufcntl == 0xf0df && linkcntl_val[0] == 16400 && linkcntl_val[1] == 16384);
                CHECK(frame_a.rxstat0 == 0x80 && frame_a.rxstat1 == 0x80);
            } else if (i == 2) {                /* toggle set, same llid: the first of the pair is queued first, so the second leads */
                t0->llid = t1->llid = 2;
                tws_link_rxtx_state_reset();
                CHECK(phone.tx_acl_u.next == &t1->entry && t1->entry.next == &t0->entry && t0->entry.next == &phone.tx_acl_u);
            } else if (i == 3) {                /* the same packet in both slots is queued once; a finished packet is not unlinked */
                phone.tx_bulk[1] = t0; t0->offset = t0->len; t0->entry.next = t0->entry.prev = NULL;
                tws_link_rxtx_state_reset();
                CHECK(count(&phone.tx_acl_u) == 1 && phone.tx_acl_u.next == &t0->entry && count(&phone.tx_acl_c) == 0);
                CHECK(t0->offset == OFFSET_OF(t0, 0x400));      /* the toggle swap ran first: slot 0 now holds the second pointer */
            } else if (i == 4) {                /* only the second slot filled: it uses its own pointer */
                phone.tx_bulk[0] = NULL;
                tws_link_rxtx_state_reset();
                CHECK(count(&phone.tx_acl_c) == 1 && count(&phone.tx_acl_u) == 0 && t1->offset == OFFSET_OF(t1, 0x400) && t0->offset == 3);
            } else {                            /* only the first: the toggle does not swap with nothing */
                phone.tx_bulk[1] = NULL;
                tws_link_rxtx_state_reset();
                CHECK(count(&phone.tx_acl_u) == 1 && count(&phone.tx_acl_c) == 0 && t0->offset == OFFSET_OF(t0, 0x300) && t1->offset == 4);
            }
            free(t0); free(t1);
        }
        /* txtog is captured before callbacks, even if the task post changes
         * the frame field before the queued buffers move. */
        reset(); list_init(&phone.tx_acl_c); list_init(&phone.tx_acl_u); tws_link.link = &phone;
        rx_avail_link = &phone; rx_avail_set = 2; post_clears = 1; REG32(REG_BUF_BASE) = base;
        t0 = calloc(1, sizeof(*t0) + 64); t1 = calloc(1, sizeof(*t1) + 64);
        t0->len = t1->len = 10; t0->offset = t1->offset = 3; t0->llid = t1->llid = 2;
        list_init(&t0->entry); list_init(&t1->entry);
        phone.tx_bulk[0] = t0; phone.tx_bulk[1] = t1;
        frame_a.txptr0 = 0x300; frame_a.txptr1 = 0x400; frame_a.txtog = 1;
        mutate_txtog_after_post = 0;
        tws_link_rxtx_state_reset();
        CHECK(phone.tx_acl_u.next == &t1->entry && t1->entry.next == &t0->entry && t0->entry.next == &phone.tx_acl_u);
        CHECK(t0->offset == OFFSET_OF(t0, 0x300) && t1->offset == OFFSET_OF(t1, 0x400));
        CHECK(frame_a.txtog == 0);
        free(t0); free(t1);
        /* rx pending: the task is told with the link slot as its argument; status words are kept ... */
        reset(); list_init(&phone.tx_acl_c); list_init(&phone.tx_acl_u); tws_link.link = &phone; rx_avail_link = &phone;
        rx_avail_set = 2; frame_a.rxstat0 = 0xabc; frame_a.rxstat1 = 0xdef; frame_a.rxtog = 1; frame_a.txtog = 0;
        tws_link_rxtx_state_reset();
        CHECK(posts == 1 && post_argv == (int *)&tws_link.link && frame_a.rxstat0 == 0xabc && frame_a.rxtog == 1 && !strcmp(trace, "Q"));
        /* ... unless the task cleared it again */
        reset(); list_init(&phone.tx_acl_c); list_init(&phone.tx_acl_u); tws_link.link = &phone; rx_avail_link = &phone;
        rx_avail_set = 2; post_clears = 1; frame_a.rxtog = 1; phone.role = 1;
        tws_link_rxtx_state_reset();
        CHECK(posts == 1 && frame_a.rxstat0 == 0x80 && frame_a.rxstat1 == 0x80 && frame_a.rxtog == 0);
        CHECK(frame_a.txtog == 0 && frame_a.bufcntl == 3);                 /* role 1: the role bit is 0 */
#undef OFFSET_OF
    }

    /* eSCO relay open */
    {
        struct bredr_esco_link src;
        struct bredr_esco_link *dst;
        u32 base = 0x100000;
        struct bredr_rx_bulk *rxn[3];

#define OPEN_SETUP() do { \
        reset(); memset(&src, 0, sizeof(src)); tws_link.link = &peer; phone.esco = &src; REG32(REG_BUF_BASE) = base; \
        src.link_type = 2; src.slot = 5; src.rsvd_state = 6; src.max_slot_cnt = 7; src.esco_dump_cnt = 9; src.pdu.T_sco = 12; \
        src.pdu.handle = 1; src.pdu.lt_addr = 3; src.pdu.tws_lt_addr = 5; src.pdu.D_sco = 2; src.pdu.W_sco = 4; \
        src.pdu.packet_type_m2s = 0x0c; src.pdu.packet_type_s2m = 0x0d; src.pdu.air_mode = 3; src.pdu.negotiation_state = 8; \
        src.pdu.packet_len_m2s = 60; src.pdu.packet_len_s2m = 30; list_init(&src.rx_esco); list_init(&src.tx_esco); \
        malloc_result = calloc(1, 4096); memset(malloc_result, 0xa5, 4096); tws_link.remove_esco_timeout = 77; \
    } while (0)

        /* not opened: no phone eSCO, no peer link (the timeout is untouched); SCO link or T_sco 11 (it is zeroed) */
        OPEN_SETUP(); phone.esco = NULL;
        CHECK(bredr_esco_tws_link_open(&phone, 1) == 0 && tws_link.remove_esco_timeout == 77 && tws_link.link->esco == NULL);
        OPEN_SETUP(); tws_link.link = NULL;
        CHECK(bredr_esco_tws_link_open(&phone, 1) == 0 && tws_link.remove_esco_timeout == 77);
        OPEN_SETUP(); src.link_type = 0;
        CHECK(bredr_esco_tws_link_open(&phone, 1) == 0 && tws_link.remove_esco_timeout == 0 && malloc_size == 0);
        OPEN_SETUP(); src.pdu.T_sco = 11;
        CHECK(bredr_esco_tws_link_open(&phone, 1) == 0 && tws_link.remove_esco_timeout == 0 && malloc_size == 0);
        OPEN_SETUP(); src.pdu.T_sco = 255;
        CHECK(bredr_esco_tws_link_open(&phone, 1) == 1);

        /* a monitor-start open */
        OPEN_SETUP(); dst = malloc_result; log_tag_const_i_TWS_LMP = 0;
        CHECK(bredr_esco_tws_link_open(&phone, 1) == 1 && malloc_size == 80);
        CHECK(peer.esco == dst && tws_link.remove_esco_timeout == 0);
        CHECK(!memcmp(&dst->pdu, &src.pdu, sizeof(src.pdu)));
        CHECK(dst->slot == 5 && dst->link_type == 2 && dst->rsvd_state == 6 && dst->max_slot_cnt == 7 && dst->esco_dump_cnt == 10);
        CHECK(dst->rx_remove_esco_req_flag == 80);
        CHECK(dst->timer0 == 0xa5 && dst->timer1 == 0xa5 && dst->remote_txtog == 0xa5 && dst->acl_work_cnt == 0xa5);   /* block is not cleared */
        CHECK(dst->rx_esco.next == &dst->rx_esco && dst->rx_esco.prev == &dst->rx_esco && dst->tx_esco.next == &dst->tx_esco && dst->tx_esco.prev == &dst->tx_esco);
        CHECK(type_n == 2 && type_calls[0] == 0x0c && type_calls[1] == 0x0d);
        CHECK(ltc[0] == 1 && ltc[1] == 0 && ltc[2] == 0 && ltc[3] == 5);               /* 0x0d is not above 13; the link type is not 0 */
        CHECK(trc[0] == 1 && trc[1] == 0x0c + 100 && trc[2] == 0x0d + 100 && trcnt_len_m2s == 68 && trcnt_len_s2m == 38);
        CHECK(portc[0] == 1 && portc[1] == 12);
        CHECK(align_frame == &frame_b && align_en == 1 && logs == 0);
        CHECK(rx_alloc_n == 2 && rx_alloc_id[0] == 0 && rx_alloc_len[0] == 68 && rx_alloc_len[1] == 68);
        CHECK(tx_alloc_n == 2 && tx_alloc_len[0] == 30 && tx_alloc_len[1] == 30);
        CHECK(dst->rx_bulk[0] == rx_allocs[0] && dst->rx_bulk[1] == rx_allocs[1] && dst->tx_bulk[0] == tx_allocs[0] && dst->tx_bulk[1] == tx_allocs[1]);
        for (i = 0; i < 30; i++) CHECK(tx_allocs[0]->data[i] == 0xaa && tx_allocs[1]->data[i] == 0xaa);
        CHECK(tx_allocs[0]->data[30] == 0 && tx_allocs[1]->data[30] == 0);
        CHECK(REG32(REG_ESCO_CNTL) == 0);
        CHECK(REG32(REG_ESCO_RXPTR) == (((u32)(uintptr_t)rx_allocs[0]->data - base) | ((u32)(uintptr_t)rx_allocs[1]->data - base) << 16));
        CHECK(REG32(REG_ESCO_TXPTR) == (((u32)(uintptr_t)tx_allocs[0]->data - base) | ((u32)(uintptr_t)tx_allocs[1]->data - base) << 16));
        CHECK(pcm_enable == 1 && !strcmp(trace, "p"));
        /* packet types above 13 mean EDR; a SCO-type 0 would be rejected earlier, so the type argument is always 0 */
        OPEN_SETUP(); src.pdu.packet_type_s2m = 14; bredr_esco_tws_link_open(&phone, 1); CHECK(ltc[1] == 1 && ltc[2] == 0);
        OPEN_SETUP(); src.pdu.packet_type_s2m = 255; bredr_esco_tws_link_open(&phone, 1); CHECK(ltc[1] == 1);
        /* EDR is decided before conversion helpers; the second helper reads
         * the packet type after the first helper mutates it. */
        OPEN_SETUP(); src.pdu.packet_type_s2m = 13; mutate_esco_packet_type = &src;
        bredr_esco_tws_link_open(&phone, 1);
        CHECK(ltc[1] == 0 && type_calls[0] == 0x0c && type_calls[1] == 14);
        /* with logging on: both messages, arguments as the SDK passes them */
        OPEN_SETUP(); log_tag_const_i_TWS_LMP = 1; phone.role = 1; phone.monitor = 1; frame_b.miscntl = 0xfd;
        bredr_esco_tws_link_open(&phone, 1);
        CHECK(logs == 2 && !strcmp(log_format[0], "[Info]: [TWS_LMP]pcket_type: %d, %d, %d, %d\n\r\n"));
        CHECK(log_args[0][0] == 0x0c + 100 && log_args[0][1] == 0x0d + 100 && log_args[0][2] == 60 && log_args[0][3] == 30);
        CHECK(!strcmp(log_format[1], "[Info]: [TWS_LMP]tws_>lt_addr=0x%x,%x,role=%x,monitor=%x\n\r\n"));
        CHECK(log_args[1][0] == 5 && log_args[1][1] == 5 && log_args[1][2] == 1 && log_args[1][3] == 1);
        /* not a monitor start, nothing queued: the same as above */
        OPEN_SETUP(); dst = malloc_result; bredr_esco_tws_link_open(&phone, 0);
        CHECK(dst->esco_dump_cnt == 10 && nbulk_freed == 0);
        /* queued phone rx frames: all but the last are released, the dump count is left as the heap had it */
        OPEN_SETUP(); dst = malloc_result;
        for (i = 0; i < 3; i++) {
            rxn[i] = calloc(1, sizeof(*rxn[i]) + 16);
            list_add(&rxn[i]->entry, src.rx_esco.prev);
        }
        CHECK(count(&src.rx_esco) == 3);
        bredr_esco_tws_link_open(&phone, 0);
        CHECK(nbulk_freed == 2 && bulk_freed[0] == rxn[0] && bulk_freed[1] == rxn[1] && count(&src.rx_esco) == 1 && src.rx_esco.next == &rxn[2]->entry);
        CHECK(dst->esco_dump_cnt == 0xa5 && dst->slot == 5);
        OPEN_SETUP(); dst = malloc_result;
        rxn[0] = calloc(1, sizeof(*rxn[0]) + 16); list_add(&rxn[0]->entry, &src.rx_esco);
        bredr_esco_tws_link_open(&phone, 0);
        CHECK(nbulk_freed == 0 && count(&src.rx_esco) == 1 && dst->esco_dump_cnt == 0xa5);
        OPEN_SETUP(); dst = malloc_result;
        for (i = 0; i < 3; i++) { rxn[i] = calloc(1, sizeof(*rxn[i]) + 16); list_add(&rxn[i]->entry, src.rx_esco.prev); }
        bredr_esco_tws_link_open(&phone, 1);                      /* a monitor start leaves them alone */
        CHECK(nbulk_freed == 0 && count(&src.rx_esco) == 3 && dst->esco_dump_cnt == 10);
        /* a failed allocation is reported (and the SDK would carry on) */
        OPEN_SETUP(); malloc_result = NULL;
        if (setjmp(escape) == 0) bredr_esco_tws_link_open(&phone, 1);
        CHECK(asserts == 1 && !strcmp(assert_what, "tws_esco") && malloc_size == 80 && peer.esco == NULL);
#undef OPEN_SETUP
    }

    /* eSCO relay close */
    {
        struct bredr_esco_link *esco;
        struct bredr_tx_bulk *tb[3], *txn[2];
        struct bredr_rx_bulk *rb[2], *rxn[2];

#define CLOSE_SETUP() do { \
        reset(); esco = calloc(1, sizeof(*esco)); list_init(&esco->rx_esco); list_init(&esco->tx_esco); \
        rb[0] = calloc(1, sizeof(*rb[0])); rb[1] = calloc(1, sizeof(*rb[1])); esco->rx_bulk[0] = rb[0]; esco->rx_bulk[1] = rb[1]; \
        for (i = 0; i < 3; i++) tb[i] = calloc(1, sizeof(*tb[i])); \
        esco->tx_bulk[0] = tb[0]; esco->tx_bulk[1] = NULL; esco->tx_bulk[2] = tb[2]; \
        for (i = 0; i < 2; i++) { \
            txn[i] = calloc(1, sizeof(*txn[i])); rxn[i] = calloc(1, sizeof(*rxn[i])); \
            list_add(&txn[i]->entry, esco->tx_esco.prev); list_add(&rxn[i]->entry, esco->rx_esco.prev); \
        } \
        esco->pdu.tws_lt_addr = 6; tws_link.slot_instant_timer = 0x21; \
    } while (0)

        /* nothing to close */
        reset(); CHECK(bredr_esco_tws_link_close(&phone) == 0 && trace[0] == 0);      /* no peer link, not a TWS link */
        reset(); tws_link.link = &peer; CHECK(bredr_esco_tws_link_close(&phone) == 0 && trace[0] == 0);   /* peer without eSCO */
        reset(); phone.tws_link = 1; CHECK(bredr_esco_tws_link_close(&phone) == 0 && trace[0] == 0);       /* the link itself, without eSCO */
        /* on the peer link, in order: rx buffers, tx buffers, tx list, rx list (unlinked), timer, block */
        CLOSE_SETUP(); tws_link.link = &peer; peer.esco = esco; phone.esco = (void *)1;
        CHECK(bredr_esco_tws_link_close(&phone) == 1);
        CHECK(!strcmp(trace, "drrttttrrsfu"));
        CHECK(nbulk_freed == 8 && bulk_freed[0] == rb[0] && bulk_freed[1] == rb[1] && bulk_freed[2] == tb[0] && bulk_freed[3] == tb[2]);
        CHECK(bulk_freed[4] == txn[0] && bulk_freed[5] == txn[1] && bulk_freed[6] == rxn[0] && bulk_freed[7] == rxn[1]);
        CHECK(timer_put == 0x21 && tws_link.slot_instant_timer == 0 && nfreed == 1 && freed[0] == esco && peer.esco == NULL && phone.esco == (void *)1);
        CHECK(logs == 0);
        /* on the TWS link itself, with logging: the stored peer is not touched */
        CLOSE_SETUP(); log_tag_const_i_TWS_LMP = 1; phone.tws_link = 1; phone.esco = esco; tws_link.link = &peer; peer.esco = (void *)2;
        CHECK(bredr_esco_tws_link_close(&phone) == 1 && phone.esco == NULL && peer.esco == (void *)2);
        CHECK(logs == 2 && !strcmp(log_format[0], "[Info]: [TWS_LMP]clos_tws_lc->esco->pdu.tws_lt_addr=%d\n\r\n") && log_args[0][0] == 6);
        CHECK(!strcmp(log_format[1], "[Info]: [TWS_LMP]%s\r\n"));
        /* no timer held: none returned; logging is decided before the work */
        CLOSE_SETUP(); tws_link.slot_instant_timer = 0; tws_link.link = &peer; peer.esco = esco;
        bredr_esco_tws_link_close(&phone);
        CHECK(strchr(trace, 's') == NULL && nfreed == 1);
        /* the table registers the two sync callbacks on channel 1 with attribute 2 */
        CHECK(tws_link_sync.channel == 1 && tws_link_sync.attribute == 2);
        CHECK(tws_link_sync.tx_sync_req == tws_link_tx_sync_req && tws_link_sync.rx_sync_req_and_res == tws_link_rx_sync_req);
        CHECK(!tws_link_sync.tx_sync_req_suss && !tws_link_sync.tx_sync_res_suss && !tws_link_sync.rx_sync_res && !tws_link_sync.event_handler);
#undef CLOSE_SETUP
    }

    return TEST_DONE("btctrler_tws_link");
}
#endif
