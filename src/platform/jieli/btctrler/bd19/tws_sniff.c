/* BD19 tws_sniff.c.o occurrence 1, recovered from pinned SDK IR. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "btctrler/glue.h"
#ifdef UNITTEST
extern u8 log_tag_const_i_TWS, log_tag_const_d_TWS, log_tag_const_e_TWS;
#else
extern const u8 log_tag_const_i_TWS, log_tag_const_d_TWS, log_tag_const_e_TWS;
#endif
extern void log_print(int, const char *, const char *, ...);
extern u8 sniff_idle_detect(void) __asm__("link_idle_task_enable_detect");
extern int os_taskq_post_type(const char *, int, int, int *);
#define CODE __attribute__((section(".classic_tws_code")))
#define INFO(...) do { if (log_tag_const_i_TWS) log_print(2, 0, __VA_ARGS__); } while (0)
#define ERROR(...) do { if (log_tag_const_e_TWS) log_print(4, 0, __VA_ARGS__); } while (0)
#define FILE_NAME "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/tws/tws_protocol_new/tws_sniff.c"
u8 sniff_test_tone;
struct lmp_tws_sniff tws_sniff __attribute__((common));
static void cpu_reset(void) { p33_soft_reset(); }
static void CODE tws_sniff_without_phone(void);
static void CODE tws_unsniff_without_phone(void);
static void CODE tws_sniff_first_anchor_point(void *);
static void CODE tws_sniff_pre_anchor_point(void *);
static void CODE tws_sniff_anchor_point_timeout(void *);
static void CODE tws_sniff_slave_start_monitor(void *);
static void CODE tws_master_rx_unsniff_accept_check(void *);
static void CODE tws_sniff_master_start_monitor_probe(void *);
static void CODE tws_sniff_master_start_monitor(void *);

void CODE tws_rx_sniff_info(u8 *rx)
{
    u32 local;
    if (!tws_link.link->role) {
        if (config_asser) {
            printf("file:%s, line:%d", FILE_NAME, 82);
            printf("ASSERT-FAILD: tws_link.link->role == ROLE_SLAVE ");
            cpu_assert_debug();
        } else cpu_reset();
    }
    if (log_tag_const_i_TWS) {
        u16 d = tws_sniff.D_sniff; u32 cnt = tws_sniff.cnt;
        u32 a = btctler_little_endian_read_32(rx, 2);
        u32 b = btctler_little_endian_read_32(rx, 6);
        log_print(2, 0, "[Info]: [TWS]D_update:%d,%d,%d\r\n", d, cnt, a, b);
    }
    if (!tws_sniff.enable) return;
    tws_sniff.D_sniff = btctler_little_endian_read_16(rx, 0);
    tws_sniff.cnt = btctler_little_endian_read_32(rx, 2);
    u32 reference = btctler_little_endian_read_32(rx, 6);
    u32 off = __read_reg_clkoffset(tws_link.link->frame);
    u32 now = get_mclkn(off, tws_link.link->frame->bitoff, &local);
    u32 delta = 0;
    if (now >= reference) delta = now - reference;
    else if (reference - now > 69120000u) delta = 134217727u - reference + now;
    u32 period = tws_sniff.T_sniff, half = period >> 1;
    if (delta >= half) { u32 n = half + delta; tws_sniff.cnt += n - n % period; }
}
void CODE tws_rx_sniff_req(u8 *rx)
{
    tws_conn_send_cmd(2, "11", 15, *rx);
    INFO("[Info]: [TWS]%s:%d\r\n", __FUNCTION__, *rx);
    if (*rx == 0x55) { tws_lmp_rx_sniff_req(tws_conn.lmp_conn, rx + 1); tws_sniff_with_phone(); }
    else tws_sniff_without_phone();
}
void CODE tws_lmp_rx_sniff_req(struct lmp_connection *conn, u8 *rx)
{
    if (conn->core.operation_mode == 2) return;
    struct lmp_sniff sniff;
    sniff.timing_control_flags = rx[0];
    sniff.D_sniff = rx[1] | (rx[2] << 8);
    sniff.T_sniff = rx[3] | (rx[4] << 8);
    sniff.attempt = rx[5] | (rx[6] << 8);
    sniff.timeout = rx[7] | (rx[8] << 8);
    tws_lmp_rx_sniff_sync(conn, &sniff);
}
void CODE tws_sniff_with_phone(void)
{
    if (tws_link.link && tws_conn.monitor_start != 2) return;
    local_irq_disable();
    if (tws_link.link) {
        struct lmp_sniff *sniff = tws_conn.lmp_conn->sniff;
        if (sniff && !sniff->wake_instant) { local_irq_enable(); return; }
        if (!tws_sniff.enable) {
            INFO("[Info]: [TWS]%s\r\n", __FUNCTION__);
            tws_sniff.state = 0; tws_sniff.enable = 1; tws_sniff.sync = 0;
            tws_sniff.unsniff_retry = 0; tws_sniff.cnt = 0;
            tws_sniff.timer = slot_timer_get();
            tws_sniff_fap_set(tws_conn.lmp_conn->sniff);
        }
    }
    local_irq_enable();
}
static void CODE tws_sniff_without_phone(void)
{
    local_irq_disable();
    if (tws_link.link && !tws_sniff.enable) {
        INFO("[Info]: [TWS]%s\r\n", __FUNCTION__);
        tws_sniff.state = 0; tws_sniff.enable = 1; tws_sniff.sync = 0;
        tws_sniff.unsniff_retry = 0; tws_sniff.cnt = 0;
        tws_sniff.timer = slot_timer_get();
        tws_sniff.D_sniff = 0; tws_sniff.T_sniff = 800;
        tws_sniff.attempt = 8; tws_sniff.timeout = 1; tws_sniff.D_sniff_bk = 0;
        tws_sniff_fap_set(NULL);
        tws_conn_send_event(20, "1", tws_sniff.enable);
    }
    local_irq_enable();
}
void CODE tws_rx_unsniff_req(u8 *rx)
{
    switch (*rx) {
    case 0x55:
        tws_conn_send_cmd(2, "11", 17, 0x55); tws_sniff.state |= 8;
        local_irq_disable();
        if (tws_link.link->state == 2) link_task_run_slots(&tws_link.link->task, 128);
        else ERROR("<Error>: [TWS]rx_unsniff_BUT_task_suspend??\r\n");
        local_irq_enable(); sniff_test_tone = 6; break;
    case 0x56:
        tws_lmp_rx_unsniff_req(tws_conn.lmp_conn, rx + 1); sniff_test_tone = 7; break;
    case 0xac:
        tws_unsniff_without_phone(); tws_conn_send_cmd(2, "11", 17, *rx); break;
    }
    INFO("[Info]: [TWS]sync:rx unsniff\r\n");
}
void CODE tws_lmp_rx_unsniff_req(struct lmp_connection *conn, u8 *rx)
{
    if (*rx) {
        sniff_test_tone = 5;
        u32 instant = btctler_little_endian_read_32(rx, 1);
        tws_sniff.pclkoffset = btctler_little_endian_read_32(rx, 5);
        tws_sniff.pbitoffset = btctler_little_endian_read_16(rx, 9);
        tws_sniff.tbitoffset = btctler_little_endian_read_16(rx, 11);
        int delta = bredr_clkn2offset(tws_link_read_slot_clk(), instant);
        local_irq_disable();
        if (tws_link.link->state == 2) link_task_run_slots(&tws_link.link->task, 128);
        else ERROR("<Error>: [TWS]rx_unsniff_but_task_suspend??\r\n");
        local_irq_enable();
        slot_timer_set(tws_conn.slot_timer, tws_sniff_slave_start_monitor, conn, delta > 2 ? delta : 2);
        INFO("[Info]: [TWS]rx:clk:0x%x,%d,%d,%d\r\n", tws_sniff.pclkoffset, tws_sniff.pbitoffset, tws_sniff.tbitoffset, delta);
    } else {
        tws_unsniff_with_phone();
        INFO("[Info]: [TWS]phone_detach already, rx_tws_detch now\n\r\n");
    }
}
static void CODE tws_unsniff_without_phone(void)
{
    if (tws_sniff.enable) {
        link_task_idle_disable();
        if (tws_conn.role == 0 && lmp_conn_for_address(NULL) == NULL) link_task_set_sleep_slot(16);
        else link_task_set_sleep_slot(0);
    }
    local_irq_disable();
    if (tws_link.link && tws_sniff.enable) {
        INFO("[Info]: [TWS]%s\r\n", __FUNCTION__);
        tws_sniff.state = 0; tws_sniff.enable = 0; tws_sniff.sync = 0;
        tws_sniff.wakeup_offset = 0; tws_sniff.wake_instant = 0;
        slot_timer_put(tws_sniff.timer); link_task_run_set(&tws_link.link->task, 1);
        lc_sniff_ctrl(tws_link.link, 6, NULL);
        tws_conn_send_event(20, "1", tws_sniff.enable);
    }
    local_irq_enable();
}
void CODE tws_rx_unsniff_accept(u8 *rx)
{
    INFO("[Info]: [TWS]%s:%d\r\n", __FUNCTION__, *rx);
    tws_sniff.state |= 32; if (*rx == 0xac) tws_unsniff_without_phone();
}
void CODE tws_rx_sniff_accept(u8 *rx)
{
    INFO("[Info]: [TWS]%s:%d\r\n", __FUNCTION__, *rx);
    if (*rx == 0x55) tws_sniff_with_phone(); else tws_sniff_without_phone();
}
void CODE tws_sniff_ap_exit(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    local_irq_disable();
    if (tws_sniff.enable && tws_sniff.sync && (tws_sniff.state & 1)) {
        tws_sniff.sync = 0; tws_sniff_ap_set(conn);
    }
    local_irq_enable();
}
void CODE tws_sniff_ap_set(struct lmp_connection *conn)
{
    u32 master, local;
    lc_sniff_ctrl(tws_link.link, 2, NULL);
    u32 args[2] = {0, 0};
    lc_sniff_ctrl(tws_link.link, 3, args);
    s16 fine = args[1];
    bredr_get_link_slot_clk(tws_link.link, &master, &local);
    int is_master = !tws_link.link->role;
    u32 delta;
    if (is_master && tws_link.plink) {
        struct lmp_sniff *sniff = conn ? conn->sniff : NULL;
        if (sniff) {
            u32 wake = sniff->wake_instant;
            if (wake < local) wake += 134217728u;
            delta = wake - local + (s32)sniff->wakeup_offset + 18;
            if ((s32)delta < 10) delta += sniff->T_sniff;
        } else {
            u32 wake = tws_sniff.wake_instant + tws_sniff.T_sniff;
            if (wake < local) wake += 134217728u;
            delta = wake - local + (s32)tws_sniff.wakeup_offset;
        }
    } else {
        delta = tws_sniff.D_sniff - master % tws_sniff.T_sniff;
        if ((s32)delta < 0) delta += tws_sniff.T_sniff;
    }
    u32 wake = local + delta;
    tws_sniff.wake_instant = wake & 134217727u;
    if (is_master) tws_sniff.D_sniff = (master + delta) % tws_sniff.T_sniff;
    slot_timer_set_finecnt(tws_sniff.timer, tws_sniff_pre_anchor_point, conn, wake, fine);
    if (tws_conn.role == 1) {
        if (conn && conn->sniff && !(conn->sniff->state & 24)) link_task_set_period(tws_sniff.wake_instant - 4);
    } else if (conn && conn->sniff) {
        if (!(conn->sniff->state & 24)) {
            u32 p = conn->sniff->wake_instant, t = tws_sniff.wake_instant;
            link_task_set_period((p > t ? t : p) - 4);
        }
    } else link_task_set_period(tws_sniff.wake_instant - 4);
    INFO("[Info]: [TWS](r:%d, m:0x%x. l:0x%x)\r\n", tws_link.link->role, master, local);
}
void CODE tws_sniff_ap_enter(void) { tws_conn_sync_tx_operation_mode(); }
void CODE tws_conn_sync_tx_operation_mode(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    if (!conn || tws_conn.monitor_start != 2 || tws_conn.role == 1) return;
    struct lmp_sniff *sniff = conn->sniff;
    u8 mode = conn->core.operation_mode;
    if (mode == conn->core.operation_mode_sync) return;
    if (mode == 2) {
        if (!sniff) return;
        INFO("[Info]: [TWS]sync:tx sniff\r\n");
        conn->core.operation_mode_sync = conn->core.operation_mode;
        tws_conn_send_cmd(15, "112222", 85, sniff->timing_control_flags, sniff->D_sniff, sniff->T_sniff, sniff->attempt, sniff->timeout);
        if (tws_conn.pure_monitor) tws_conn.exit_pure_monitor = 1;
    } else {
        conn->core.operation_mode_sync = mode; tws_sniff.state |= 16;
        tws_conn_send_cmd(17, "1", 85);
        slot_timer_set(tws_sniff.timer, tws_master_rx_unsniff_accept_check, NULL, 16);
        INFO("[Info]: [TWS]sync:tx_unsniff_1\r\n"); sniff_test_tone = 1;
    }
}
void CODE __attribute__((weak)) sniff_achor_point_hook(u32 cnt) { (void)cnt; }
void CODE tws_sniff_cal_D_for_slave(u32 master, u32 offset)
{ tws_sniff.D_sniff = (master + offset) % tws_sniff.T_sniff; }
void CODE tws_sniff_update_D_for_slave(void)
{
    u32 local;
    if (tws_link.link->role || tws_sniff.D_sniff_bk == tws_sniff.D_sniff) return;
    INFO("[Info]: [TWS]D_update:%d,%d,%d\r\n", tws_sniff.D_sniff_bk, tws_sniff.D_sniff, tws_sniff.cnt);
    tws_sniff.D_sniff_bk = tws_sniff.D_sniff;
    u32 off = __read_reg_clkoffset(tws_link.link->frame);
    u32 master = get_mclkn(off, tws_link.link->frame->bitoff, &local);
    tws_conn_send_cmd(16, "244", tws_sniff.D_sniff, tws_sniff.cnt, master);
}
void CODE tws_sniff_fap_set(struct lmp_sniff *sniff)
{
    u32 local;
    if (log_tag_const_d_TWS) log_print(1, 0, "[Debug]: [TWS]%s\r\n", __FUNCTION__);
    u32 off = __read_reg_clkoffset(tws_link.link->frame);
    u32 master = get_mclkn(off, tws_link.link->frame->bitoff, &local);
    local_irq_disable();
    u32 delta;
    if (sniff) {
        u32 wake = sniff->wake_instant;
        if (wake < local) wake += 134217728u;
        delta = wake - local + (s32)sniff->wakeup_offset + 18;
        u32 period = sniff->T_sniff;
        while ((s32)delta < 10) delta += period;
        tws_sniff.wakeup_offset = 0; tws_sniff.wake_instant = local + delta;
        tws_sniff.T_sniff = period;
        tws_sniff.D_sniff = (delta + master) % period;
        tws_sniff.D_sniff_bk = tws_sniff.D_sniff;
    } else {
        u32 period = tws_sniff.T_sniff;
        delta = period - master % period + tws_sniff.D_sniff;
        while ((s32)delta < 10) delta += period;
        tws_sniff.wake_instant = local + delta;
    }
    slot_timer_set_ext(tws_sniff.timer, tws_sniff_first_anchor_point, sniff, delta, local);
    local_irq_enable();
    INFO("[Info]: [TWS]tws_fap :%d, %x, %x, %d\r\n", delta, master, local, tws_sniff.D_sniff);
}
static void CODE tws_sniff_first_anchor_point(void *p)
{
    u8 map[10];
    INFO("[Info]: [TWS]%s\r\n", __FUNCTION__);
    __builtin_memset(map, 255, 10); bdhw_set_afh(tws_link.link->frame, 1, map);
    struct link_connection *link = tws_link.link;
    link->frame->miscntl &= (u16)~512;
    link->core.max_slot = 1;
    if (tws_sniff.state & 1) {
        if (config_asser) {
            printf("file:%s, line:%d", FILE_NAME, 331);
            printf("ASSERT-FAILD: (tws_sniff.state & LMP_STA_SNIFF_EN) == 0 "); cpu_assert_debug();
        } else cpu_reset();
    }
    tws_sniff.state |= 1;
    lc_sniff_ctrl(tws_link.link, 5, NULL); sniff_idle_detect();
    link_task_run_set(&tws_link.link->task, 0);
    if (tws_link.link->role) lc_sniff_ctrl(tws_link.link, 3, NULL);
    tws_sniff_pre_anchor_point(p);
}
static void CODE tws_sniff_pre_anchor_point(void *p)
{
    u32 local;
    lc_sniff_ctrl(tws_link.link, 1, NULL); tws_sniff.sync = 1;
    int duration = tws_conn.role == 1 ? 16 : 8;
    tws_sniff.ap_check_retry = 0;
    tws_sniff.cnt += tws_sniff.T_sniff; sniff_achor_point_hook(tws_sniff.cnt);
    slot_timer_set(tws_sniff.timer, tws_sniff_anchor_point_timeout, p, (s32)tws_sniff.wakeup_offset + duration);
    link_task_run(&tws_link.link->task); link_task_try_run_slots(&tws_link.link->task, 64);
    if (tws_sniff.state & 16) slot_timer_set(tws_sniff.timer, tws_master_rx_unsniff_accept_check, NULL, 16);
    u32 off = __read_reg_clkoffset(tws_link.link->frame);
    get_mclkn(off, tws_link.link->frame->bitoff, &local);
    tws_sniff_update_D_for_slave();
}
void CODE tws_sniff_detach(void)
{
    if (tws_sniff.enable) link_task_idle_disable();
    local_irq_disable();
    if (tws_sniff.enable) {
        INFO("[Info]: [TWS]%s\r\n", __FUNCTION__);
        tws_sniff.state = 0; tws_sniff.enable = 0; tws_sniff.sync = 0;
        tws_sniff.unsniff_retry = 0; tws_sniff.wakeup_offset = 0;
        __builtin_memset(&tws_sniff.wake_instant, 0, 12);
        slot_timer_put(tws_sniff.timer); link_task_run_set(&tws_link.link->task, 1);
        lc_sniff_ctrl(tws_link.link, 6, NULL);
    }
    if (tws_conn.lmp_conn) tws_conn.lmp_conn->core.operation_mode_sync = 0;
    if (tws_link.link) tws_link.link->core.max_slot = 5;
    local_irq_enable();
}
void CODE tws_unsniff_with_phone(void)
{
    if (tws_sniff.enable) link_task_idle_disable();
    local_irq_disable();
    if (tws_link.link && tws_sniff.enable) {
        INFO("[Info]: [TWS]%s\r\n", __FUNCTION__);
        tws_sniff.state = 0; tws_sniff.enable = 0; tws_sniff.sync = 0;
        tws_sniff.unsniff_retry = 0; tws_sniff.wakeup_offset = 0;
        __builtin_memset(&tws_sniff.wake_instant, 0, 12);
        slot_timer_put(tws_sniff.timer); link_task_run_set(&tws_link.link->task, 1);
        lc_sniff_ctrl(tws_link.link, 6, NULL);
    }
    local_irq_enable();
}
static void CODE tws_sniff_slave_start_monitor(void *p)
{
    struct lmp_connection *conn = p;
    struct ctrl_frame *phone = tws_link.plink->frame, *peer = tws_link.link->frame;
    u32 pclk = tws_sniff.pclkoffset, clk;
    u16 pbit = tws_sniff.pbitoffset, bit, tbit = tws_sniff.tbitoffset;
    if (conn) {
        conn->core.operation_mode = 0; conn->core.operation_mode_sync = 0;
        lmp_send_event_mode_change(conn, 0); lmp_sniff_misc_free(conn, 0);
    }
    if (phone) s_sniffer_offset_cal_start(peer, phone, &pclk, &pbit, 0, 0);
    if (peer) {
        tws_forward_offset_cal(peer, &clk, &bit, tbit, 0);
        peer->bitoff = bit; __write_reg_clkoffset(peer, clk);
    }
    if (phone) {
        phone->twsclknoffl = clk; phone->twsclknoffh = clk >> 16;
        phone->twscon = (((u32)clkn0_phase(bit) << 5) - 32) | 9;
    }
    if (peer) {
        peer->twsclknoffl = pclk; peer->twsclknoffh = pclk >> 16;
        peer->twscon = (((u32)clkn0_phase(pbit) << 5) - 32) | 15;
        bredr_bd_frame_enable(peer, 0, 0);
    }
    sniff_test_tone = 8; tws_unsniff_with_phone();
    tws_link.last_phone_bitoff = tws_link.plink->frame->bitoff;
    tws_link.last_tws_bitoff = tws_link.link->frame->bitoff;
    INFO("[Info]: [TWS]%s\r\n", __FUNCTION__);
}
void CODE tws_lmp_rx_sniff_sync(struct lmp_connection *conn, struct lmp_sniff *source)
{
    struct lmp_sniff *sniff = lmp_sniff_misc_alloc(conn);
    sniff->timing_control_flags = source->timing_control_flags;
    sniff->D_sniff = source->D_sniff; sniff->T_sniff = source->T_sniff;
    sniff->attempt = source->attempt; sniff->timeout = source->timeout;
    INFO("[Info]: [TWS]conn:0x%x, timing_control_flags:0x%x, D:0x%x, T:0x%x, A:0x%x, TO:0x%x\r\n", conn, sniff->timing_control_flags, sniff->D_sniff, sniff->T_sniff, sniff->attempt, sniff->timeout);
    lmp_sniff_anchor_point_preset(conn); conn->core.operation_mode = 2;
    lmp_send_event_mode_change(conn, sniff->T_sniff);
}
static void CODE tws_master_rx_unsniff_accept_check(void *p)
{
    (void)p;
    int logging = log_tag_const_i_TWS;
    if (logging) log_print(2, 0, "[Info]: [TWS]%s\r\n", __FUNCTION__);
    if (tws_sniff.state & 32) {
        u16 tbit = 0, pbit = 0; u32 pclk = 0, instant = 0; int connected = 0;
        sniff_test_tone = 2;
        struct link_connection *phone = tws_link.plink;
        if (phone) {
            struct ctrl_frame *pf = phone->frame, *tf = tws_link.link->frame;
            tws_link_cacl_bitoff(pf, tf, &tbit); tws_sniff.tbitoffset = tbit;
            tws_link_cacl_clkoff_for_slave(pf, tf, &pclk, &pbit);
            instant = bredr_offset2clkn(tws_link_read_slot_clk(), 32); connected = 1;
        }
        tws_conn_send_cmd(17, "114422", 86, connected, instant, pclk, pbit, tbit);
        link_task_run_slots(&tws_link.link->task, 64);
        slot_timer_set(tws_sniff.timer, tws_sniff_master_start_monitor_probe, NULL, 28);
        if (logging) log_print(2, 0, "[Info]: [TWS]sync:tx_unsniff_2:%d,%d,0x%x,%d,%d\r\n", connected, instant, pclk, pbit, tbit);
    } else if (tws_sniff.unsniff_retry < 3) {
        ++tws_sniff.unsniff_retry;
        if (logging) log_print(2, 0, "[Info]: [TWS]unsniff_retry %d\r\n", tws_sniff.unsniff_retry);
        link_task_run_slots(&tws_link.link->task, 32);
        slot_timer_set(tws_sniff.timer, tws_master_rx_unsniff_accept_check, NULL, 16);
    } else { tws_sniff.unsniff_retry = 0; link_task_run(NULL); }
}
int CODE tws_in_sniff_state(void) { return tws_link.link && !tws_link.plink ? tws_sniff.enable : 0; }
int CODE tws_sniff_state_check(void) { return tws_link.link ? tws_sniff.enable : 0; }
void CODE tws_tx_unsniff_req(void)
{
    if (!tws_link.link || tws_link.plink || tws_conn.role == 1) return;
    if (tws_sniff.enable && !(tws_sniff.state & 16)) {
        INFO("[Info]: [TWS]tx_unsniff_withoutphone : %x\r\n", tws_sniff.enable);
        tws_sniff.state |= 16; tws_conn_send_cmd(17, "1", 172);
    }
}
void CODE tws_tx_sniff_req(void)
{
    if (!tws_link.link || tws_link.plink) return;
    int phones = link_conn_num_phone();
    if (phones || tws_conn.role == 1 || tws_sniff.enable) return;
    INFO("[Info]: [TWS]tx_sniff_withoutphone : %x\r\n", tws_sniff.enable);
    tws_conn_send_cmd(15, "1", 172);
}
void CODE tws_sniff_toggle(void) { if (tws_sniff.enable) tws_tx_unsniff_req(); else tws_tx_sniff_req(); }
static void CODE tws_sniff_anchor_point_timeout(void *p)
{
    if (tws_sniff.state & 24) return;
    if (!tws_link.link->sniff_rx_flag) link_task_run(NULL);
    else if (tws_sniff.ap_check_retry < 3) {
        ++tws_sniff.ap_check_retry;
        slot_timer_set(tws_sniff.timer, tws_sniff_anchor_point_timeout, p, 8);
    } else link_task_run(NULL);
}
static void CODE tws_sniff_master_start_monitor_probe(void *p)
{
    (void)p; INFO("[Info]: [TWS]%s\r\n", __FUNCTION__);
    if (tws_link.link) {
        link_task_run_slots(&tws_link.link->task, 32);
        bredr_bd_frame_disable(tws_link.link->frame);
        slot_timer_set(tws_sniff.timer, tws_sniff_master_start_monitor, NULL, 8);
        sniff_test_tone = 3;
    }
}
static void CODE tws_sniff_master_start_monitor(void *p)
{
    (void)p;
    int logging = log_tag_const_i_TWS;
    if (logging && (tws_sniff.state & 32)) log_print(2, 0, "[Info]: [TWS]%s, err:0x%x\r\n", __FUNCTION__, tws_sniff.state);
    u32 clk; u16 bit;
    tws_forward_offset_cal(tws_link.link->frame, &clk, &bit, tws_sniff.tbitoffset, 0);
    struct ctrl_frame *frame = tws_link.link->frame;
    frame->bitoff = bit; __write_reg_clkoffset(frame, clk);
    if (tws_link.plink) {
        struct ctrl_frame *phone = tws_link.plink->frame;
        phone->twscon = 6; tws_link.last_phone_bitoff = phone->bitoff;
    }
    bredr_bd_frame_enable(tws_link.link->frame, 0, 0);
    link_task_run_slots(&tws_link.link->task, 32);
    tws_unsniff_with_phone(); os_taskq_post_type("tws", 66, 0, NULL);
    tws_link.last_tws_bitoff = tws_link.link->frame->bitoff;
    if (logging) log_print(2, 0, "[Info]: [TWS]%s\r\n", __FUNCTION__);
    sniff_test_tone = 4;
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
int config_asser;
u8 log_tag_const_i_TWS, log_tag_const_d_TWS, log_tag_const_e_TWS;
struct tws_connection tws_conn;
struct tws_link_connection tws_link;
static struct link_connection peer, phone;
static struct ctrl_frame peer_frame, phone_frame;
static struct lmp_connection connection;
static struct lmp_sniff phone_sniff, allocated_sniff;
static u32 master_clock, local_clock, written_clock;
static int irq, max_irq, timer_gets, timer_puts, timer_calls, timer_slot, timer_ext, timer_fine;
static void (*timer_cb)(void *);
static void *timer_priv;
static int command_count, opcode, command_args[8], event_code, event_value;
static int run_slots, run_null, run_set, sleep_slot, period_slot, mode_calls, mode_interval;
static int phones, idle_calls, frame_enables, frame_disables, resets, asserts, task_posts;
static int mutate_role, mutate_pure, mutate_post, mutate_idle, mutate_clock;
static u32 *escaped_args;
static struct lmp_connection *address_result;
void local_irq_disable(void) { ++irq; if (irq>max_irq) max_irq=irq; }
void local_irq_enable(void) { CHECK(irq>0); --irq; }
void log_print(int level, const char *file, const char *fmt, ...) { (void)level; (void)file; (void)fmt; }
void cpu_assert_debug(void) { ++asserts; }
void p33_soft_reset(void) { ++resets; }
u16 btctler_little_endian_read_16(const u8 *p, int n) { return p[n] | ((u16)p[n+1]<<8); }
u32 btctler_little_endian_read_32(const u8 *p, int n) { return (u32)btctler_little_endian_read_16(p,n) | ((u32)btctler_little_endian_read_16(p,n+2)<<16); }
s32 __read_reg_clkoffset(struct ctrl_frame *f) { CHECK(f==&peer_frame); return 123; }
unsigned long get_mclkn(unsigned long off, unsigned short bit, u32 *local)
{ (void)bit; CHECK(off==123); *local=local_clock; return master_clock; }
void __write_reg_clkoffset(struct ctrl_frame *f,u32 clk) { CHECK(f==&peer_frame); written_clock=clk; }
void bredr_get_link_slot_clk(struct link_connection *l,u32 *m,u32 *c)
{ CHECK(l==&peer); *m=master_clock; *c=local_clock; if(mutate_clock && escaped_args) escaped_args[1]=99; }
u8 slot_timer_get(void) { ++timer_gets; return 7; }
void slot_timer_put(u8 id) { CHECK(id==7); ++timer_puts; }
void slot_timer_set(u8 id,void (*cb)(void *),void *p,int slot)
{ CHECK(id==7 || id==9); ++timer_calls; timer_cb=cb; timer_priv=p; timer_slot=slot; }
void slot_timer_set_ext(u8 id,void (*cb)(void *),void *p,int slot,int ext)
{ slot_timer_set(id,cb,p,slot); timer_ext=ext; }
void slot_timer_set_finecnt(u8 id,void (*cb)(void *),void *p,int slot,short fine)
{ slot_timer_set(id,cb,p,slot); timer_fine=fine; }
s32 lc_sniff_ctrl(struct link_connection *l,SNIFF_CTRL_CMD cmd,u32 *args)
{ CHECK(l==&peer); if(cmd==3 && args) { args[1]=0xfffe; escaped_args=args; } return -1; }
u32 link_task_idle_disable(void) { ++idle_calls; if(mutate_idle) tws_sniff.enable=0; return 0; }
u8 sniff_idle_detect(void) { if(mutate_role) tws_conn.role=1; return 0xa5; }
void link_task_run_set(struct link_task *t,u8 enable) { CHECK(t==&peer.task); run_set=enable; }
void link_task_run(struct link_task *t) { if(!t) ++run_null; else CHECK(t==&peer.task); }
void link_task_run_slots(struct link_task *t,int slots) { CHECK(t==&peer.task); run_slots=slots; }
void link_task_try_run_slots(struct link_task *t,int slots) { CHECK(t==&peer.task && slots==64); }
void link_task_set_sleep_slot(int n) { sleep_slot=n; }
void link_task_set_period(int n) { period_slot=n; }
struct lmp_connection *lmp_conn_for_address(u8 *a) { CHECK(!a); return address_result; }
int link_conn_num_phone(void) { return phones; }
u32 tws_link_read_slot_clk(void) { return 100; }
int bredr_clkn2offset(int a,int b) { return (u32)b-(u32)a; }
int bredr_offset2clkn(int a,int b) { return (u32)a+(u32)b; }
int tws_conn_send_cmd(u8 cmd,const char *fmt,...)
{
    ++command_count; opcode=cmd; va_list ap; va_start(ap,fmt);
    for(unsigned i=0; i<strlen(fmt); ++i) command_args[i]=va_arg(ap,int);
    va_end(ap); if(mutate_pure) tws_conn.pure_monitor=1;
    return -9;
}
void tws_conn_send_event(int code,const char *fmt,...)
{ event_code=code; va_list ap; va_start(ap,fmt); event_value=va_arg(ap,int); va_end(ap); }
struct lmp_sniff *lmp_sniff_misc_alloc(struct lmp_connection *c)
{ CHECK(c==&connection); c->sniff=&allocated_sniff; return &allocated_sniff; }
void lmp_sniff_anchor_point_preset(struct lmp_connection *c) { CHECK(c==&connection); c->sniff->T_sniff=456; }
void lmp_send_event_mode_change(struct lmp_connection *c,u16 n) { CHECK(c==&connection); ++mode_calls; mode_interval=n; }
void lmp_sniff_misc_free(struct lmp_connection *c,u8 run) { CHECK(c==&connection && !run); }
void bdhw_set_afh(struct ctrl_frame *f,u8 i,u8 *map)
{ CHECK(f==&peer_frame && i==1); for(int n=0;n<10;++n) CHECK(map[n]==255); }
void s_sniffer_offset_cal_start(struct ctrl_frame *f,struct ctrl_frame *p,u32 *c,u16 *b,u32 off,u16 bit)
{ CHECK(f==&peer_frame && p==&phone_frame && !off && !bit); *c=0x12345678; *b=45; }
void tws_forward_offset_cal(struct ctrl_frame *f,u32 *c,u16 *b,u16 t,u8 mode)
{ CHECK(f==&peer_frame && !mode); (void)t; *c=0xabcdef01; *b=67; }
unsigned char clkn0_phase(short bit) { return bit==67 ? 2 : 3; }
void bredr_bd_frame_enable(struct ctrl_frame *f,u8 a,u8 b) { CHECK(f==&peer_frame && !a && !b); ++frame_enables; }
void bredr_bd_frame_disable(struct ctrl_frame *f) { CHECK(f==&peer_frame); ++frame_disables; }
void tws_link_cacl_bitoff(struct ctrl_frame *p,struct ctrl_frame *f,u16 *b)
{ CHECK(p==&phone_frame && f==&peer_frame); *b=11; }
void tws_link_cacl_clkoff_for_slave(struct ctrl_frame *p,struct ctrl_frame *f,u32 *c,u16 *b)
{ CHECK(p==&phone_frame && f==&peer_frame); *c=0x11223344; *b=22; }
int os_taskq_post_type(const char *name,int type,int argc,int *args)
{ CHECK(!strcmp(name,"tws") && type==66 && !argc && !args); ++task_posts; if(mutate_post) peer_frame.bitoff=123; return -2; }
static void fixture(void)
{
    memset(&tws_sniff,0,sizeof(tws_sniff)); memset(&tws_link,0,sizeof(tws_link)); memset(&tws_conn,0,sizeof(tws_conn));
    memset(&peer,0,sizeof(peer)); memset(&phone,0,sizeof(phone)); memset(&peer_frame,0,sizeof(peer_frame)); memset(&phone_frame,0,sizeof(phone_frame));
    memset(&connection,0,sizeof(connection)); memset(&phone_sniff,0,sizeof(phone_sniff)); memset(&allocated_sniff,0,sizeof(allocated_sniff));
    peer.frame=&peer_frame; phone.frame=&phone_frame; peer.state=2; tws_link.link=&peer;
    tws_conn.lmp_conn=&connection; connection.sniff=&phone_sniff; tws_conn.slot_timer=9; tws_sniff.timer=7;
    master_clock=100; local_clock=200; tws_sniff.T_sniff=40; tws_sniff.D_sniff=20;
    irq=max_irq=timer_gets=timer_puts=timer_calls=timer_slot=timer_ext=timer_fine=0;
    timer_cb=NULL; timer_priv=NULL; command_count=opcode=event_code=event_value=0;
    memset(command_args,0,sizeof(command_args)); run_slots=run_null=run_set=sleep_slot=period_slot=0;
    mode_calls=mode_interval=phones=idle_calls=frame_enables=frame_disables=resets=asserts=task_posts=0;
    mutate_role=mutate_pure=mutate_post=mutate_idle=mutate_clock=0; escaped_args=NULL; address_result=NULL;
    sniff_test_tone=0; config_asser=log_tag_const_i_TWS=log_tag_const_d_TWS=log_tag_const_e_TWS=0;
}
int main(void)
{
    /* Unsigned phase arithmetic, exact-zero anchor, unmasked timer and fine snapshot. */
    fixture(); tws_sniff_cal_D_for_slave(0xfffffff0u,32); CHECK(tws_sniff.D_sniff==16);
    tws_sniff.D_sniff=20; mutate_clock=1; tws_sniff_ap_set(NULL);
    CHECK(tws_sniff.wake_instant==200 && timer_slot==200 && timer_fine==-2 && period_slot==196);
    tws_sniff.D_sniff=19; tws_sniff_ap_set(NULL); CHECK(timer_slot==239);
    fixture(); peer.role=1; local_clock=134217720; tws_sniff.D_sniff=30;
    tws_sniff_ap_set(NULL); CHECK(tws_sniff.wake_instant==2 && (u32)timer_slot==134217730u);
    /* First-anchor scheduling loops to ten slots, ap_set adds a period only once. */
    fixture(); phone_sniff.wake_instant=200; phone_sniff.wakeup_offset=-30; phone_sniff.T_sniff=8;
    tws_sniff_fap_set(&phone_sniff);
    CHECK(timer_slot==12 && timer_ext==200 && tws_sniff.wake_instant==212 && tws_sniff.D_sniff==0);
    tws_link.plink=&phone; tws_sniff_ap_set(&connection); CHECK(timer_slot==196);
    fixture(); tws_sniff.T_sniff=4; tws_sniff.D_sniff=0; master_clock=100;
    tws_sniff_fap_set(NULL); CHECK(timer_slot==12 && tws_sniff.wake_instant==212);
    /* Without-phone reception is functional and retains nested IRQ behavior. */
    fixture(); u8 rx[16]={0xac}; tws_rx_sniff_accept(rx);
    CHECK(tws_sniff.enable && !tws_sniff.sync && tws_sniff.T_sniff==800 && tws_sniff.attempt==8 && tws_sniff.timeout==1);
    CHECK(timer_gets==1 && max_irq==2 && !irq && event_code==20 && event_value==1);
    tws_rx_unsniff_accept(rx); CHECK(!tws_sniff.enable && timer_puts==1 && event_value==0 && sleep_slot==16);
    /* Phone path gate, uninitialized wake and mode-change command marshaling. */
    fixture(); tws_sniff_with_phone(); CHECK(!tws_sniff.enable);
    tws_conn.monitor_start=2; tws_sniff_with_phone(); CHECK(!tws_sniff.enable);
    phone_sniff.wake_instant=220; phone_sniff.T_sniff=40; tws_sniff_with_phone(); CHECK(tws_sniff.enable && !irq);
    fixture(); tws_conn.monitor_start=2; connection.core.operation_mode=2;
    phone_sniff.T_sniff=80; phone_sniff.D_sniff=17; phone_sniff.attempt=3; phone_sniff.timeout=4; phone_sniff.timing_control_flags=5;
    mutate_pure=1; tws_conn_sync_tx_operation_mode();
    CHECK(command_count==1 && opcode==15 && command_args[0]==85 && command_args[1]==5 && command_args[2]==17 && command_args[3]==80 && command_args[4]==3 && command_args[5]==4);
    CHECK(connection.core.operation_mode_sync==2 && tws_conn.exit_pure_monitor==1);
    connection.core.operation_mode=0; tws_conn_sync_tx_operation_mode();
    CHECK(opcode==17 && (tws_sniff.state&16) && timer_slot==16 && timer_cb==tws_master_rx_unsniff_accept_check && sniff_test_tone==1);
    /* Retry limit and successful master handoff packet/callback chain. */
    fixture(); for(int i=0;i<3;i++) { tws_master_rx_unsniff_accept_check(NULL); CHECK(tws_sniff.unsniff_retry==i+1 && timer_slot==16); }
    tws_master_rx_unsniff_accept_check(NULL); CHECK(!tws_sniff.unsniff_retry && run_null==1);
    tws_sniff.state=32; tws_master_rx_unsniff_accept_check(NULL);
    CHECK(command_args[0]==86 && command_args[1]==0 && command_args[2]==0 && timer_slot==28);
    tws_link.plink=&phone; tws_master_rx_unsniff_accept_check(NULL);
    CHECK(command_args[1]==1 && command_args[2]==132 && (u32)command_args[3]==0x11223344 && command_args[4]==22 && command_args[5]==11);
    tws_sniff_master_start_monitor_probe(NULL); CHECK(frame_disables==1 && timer_slot==8 && sniff_test_tone==3);
    mutate_post=1; tws_sniff_master_start_monitor(NULL);
    CHECK(written_clock==0xabcdef01 && phone_frame.twscon==6 && frame_enables==1 && task_posts==1 && tws_link.last_tws_bitoff==123 && sniff_test_tone==4);
    /* Slave handoff snapshots frames, clears modes and programs volatile halfwords. */
    fixture(); tws_link.plink=&phone; tws_sniff.pclkoffset=5; tws_sniff.pbitoffset=6; tws_sniff.tbitoffset=7;
    connection.core.operation_mode=connection.core.operation_mode_sync=2;
    tws_sniff_slave_start_monitor(&connection);
    CHECK(!connection.core.operation_mode && !connection.core.operation_mode_sync && mode_interval==0);
    CHECK(phone_frame.twsclknoffl==0xef01 && phone_frame.twsclknoffh==0xabcd && phone_frame.twscon==41);
    CHECK(peer_frame.twsclknoffl==0x5678 && peer_frame.twsclknoffh==0x1234 && peer_frame.twscon==79);
    CHECK(frame_enables==1 && peer_frame.bitoff==67 && sniff_test_tone==8);
    /* Imported preset can change T before mode-event reads it. */
    fixture(); u8 packet[9]={3,0x34,0x12,0x78,0x56,8,0,9,0};
    tws_lmp_rx_sniff_req(&connection,packet);
    CHECK(allocated_sniff.D_sniff==0x1234 && allocated_sniff.attempt==8 && allocated_sniff.timeout==9 && mode_interval==456 && connection.core.operation_mode==2);
    /* Timeout retry, frame setup, and returning assert paths. */
    fixture(); peer.sniff_rx_flag=1;
    for(int i=0;i<3;i++) { tws_sniff_anchor_point_timeout(NULL); CHECK(tws_sniff.ap_check_retry==i+1 && timer_slot==8); }
    tws_sniff_anchor_point_timeout(NULL); CHECK(run_null==1);
    tws_sniff.state=8; tws_sniff_anchor_point_timeout(NULL); CHECK(run_null==1);
    fixture(); peer_frame.miscntl=0xffff; peer.role=1; tws_sniff.state=1;
    tws_sniff_first_anchor_point(NULL);
    CHECK(resets==1 && peer_frame.miscntl==0xfdff && peer.core.max_slot==1 && tws_sniff.sync && tws_sniff.cnt==40 && timer_slot==8);
    config_asser=1; tws_sniff_first_anchor_point(NULL); CHECK(asserts==1);
    /* Unsigned delta/wrap and ignored calls continue after role assertion. */
    fixture(); u8 info[10]={5,0,10,0,0,0,70,0,0,0};
    tws_sniff.enable=1; master_clock=100; tws_rx_sniff_info(info); CHECK(resets==1 && tws_sniff.D_sniff==5 && tws_sniff.cnt==50);
    peer.role=1; master_clock=50; tws_rx_sniff_info(info); CHECK(tws_sniff.cnt==10);
    master_clock=10; info[6]=0; info[7]=0; info[8]=0; info[9]=8; tws_rx_sniff_info(info); CHECK(tws_sniff.cnt==10);
    /* Detach clears twelve bytes but keeps cnt and upper flag bits. */
    fixture(); tws_sniff.enable=1; tws_sniff.sync=1; tws_sniff.witchout_phone=1;
    tws_sniff.cnt=123; tws_sniff.wake_instant=tws_sniff.pclkoffset=99; tws_sniff.pbitoffset=tws_sniff.tbitoffset=42;
    connection.core.operation_mode_sync=2; tws_sniff_detach();
    CHECK(!tws_sniff.enable && !tws_sniff.sync && tws_sniff.witchout_phone && tws_sniff.cnt==123 && !tws_sniff.pclkoffset && !tws_sniff.pbitoffset && !tws_sniff.tbitoffset && peer.core.max_slot==5 && !connection.core.operation_mode_sync && !irq);
    fixture(); tws_sniff.enable=1; mutate_idle=1; tws_sniff_detach(); CHECK(timer_puts==0);
    fixture(); tws_sniff_toggle(); CHECK(opcode==15 && command_args[0]==172);
    tws_sniff.enable=1; tws_sniff_toggle(); CHECK(opcode==17 && command_args[0]==172 && (tws_sniff.state&16));
    int count=command_count; tws_sniff_toggle(); CHECK(command_count==count);
    CHECK(tws_in_sniff_state()==1 && tws_sniff_state_check()==1); tws_link.plink=&phone; CHECK(!tws_in_sniff_state() && tws_sniff_state_check());
    CHECK(!irq);
    return TEST_DONE("btctrler_bd19_tws_sniff");
}
#endif
