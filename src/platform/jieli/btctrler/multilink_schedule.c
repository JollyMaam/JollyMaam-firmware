/* BLE multilink scheduling from the SDK. The 625 us slot arithmetic,
 * software timer cadence, and IRQ-protected hardware transitions are kept. */
#include "btctrler/glue.h"

#include <string.h>

#define BLE_CODE __attribute__((section(".ble_rf_code")))

#ifdef UNITTEST
static volatile u32 test_regs[6];
static u32 test_reg_trace[64];
static unsigned test_reg_trace_len;
static u32 fixture_anchor_values[5];
static unsigned fixture_anchor_pos;
static u8 fixture_anchor_sequence;
static volatile u32 *test_reg_ptr(u32 address)
{
    test_reg_trace[test_reg_trace_len++] = address;
    switch (address) {
    case 1867784u: return &test_regs[0];
    case 1867788u: return &test_regs[1];
    case 1867796u: return &test_regs[2];
    case 1867848u: return &test_regs[3];
    case 1867816u: return &test_regs[4];
    case 1867852u: return &test_regs[5];
    default: return &test_regs[1];
    }
}
#define REG32(address) (*test_reg_ptr(address))
#else
#define REG32(address) (*(volatile u32 *)(uintptr_t)(address))
#endif

extern u16 usr_timeout_add(void *priv, void (*func)(void *), u32 timeout, u8 reload);
extern void usr_timeout_del(u16 id);
extern BTCTRLER_CONFIG int config_btctler_le_master_multilink;
extern void log_print(int level, const char *file, const char *fmt, ...);
#ifdef UNITTEST
extern u8 log_tag_const_e_LL;
#else
extern const u8 log_tag_const_e_LL;
#endif
BLE_CODE static u8 mult_slave_or_master_idle_query(void);

static u8 Lead_hw_id __attribute__((section(".ble_rf_data"))) = 0xff;
static const union __connected_distance find_interspace_distance[8] = {
    { ._u32 = 0xffffffff }, { ._u32 = 0xffffffff },
    { ._u32 = 0xffffffff }, { ._u32 = 0xffffffff },
    { ._u32 = 0xffffffff }, { ._u32 = 0xffffffff },
    { ._u32 = 0xffffffff }, { ._u32 = 0xffffffff },
};
#ifdef CONFIG_CPU_BD19
const struct lp_target ble_slave_multlp_target __attribute__((section(".lp_target"), used)) = {
    (char *)"ble_slave_mult", NULL, mult_slave_or_master_idle_query
};
#else
const struct lp_target ble_slave_multlp_target __attribute__((section(".lp_target"), used)) = {
    (char *)"ble_slave_mult", mult_slave_or_master_idle_query
};
#endif
static struct mult_slave_or_master_manger m_s __attribute__((section(".ble_rf_data"))) = {
    .opt_id = 0xff, .opt_rx_irq_hw_id = 0xff, .opt_next_id = 0xff,
    .opt_scan_id = 0xff, .opt_init_id = 0xff,
    .opt_fist_con_event_temp = 0, .opt_is_idle = 1, .opt_event_ct = 0,
    .opt_con_id = { 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff },
    .opt_update_temp = { 0 }, .opt_adv_open_event = { 0 },
    .opt_adv_hw_id = { 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff },
    .opt_adv_interval = { 0 }, .opt_scan_interval = 0, .opt_scan_win = 0,
    .opt_init_scan_interval = 0, .opt_init_scan_win = 0,
    .opt_update_timer = { 0 }, .opt_adv_timer = { 0 },
    .opt_mult_adv_event_timer = 0, .opt_scan_timer = 0,
};

BLE_CODE static u8 mult_slave_or_master_idle_query(void) { return m_s.opt_is_idle; }

BLE_CODE static u32 __hw_distance_slots(struct ble_hw *hw) {
    u32 low = __get_ble_anchor_con(hw, 0);
    u32 high = __get_ble_anchor_con(hw, 14);
    return (((high << 16) & 0x00ff0000u) | (low & 0xffffu)) + 1;
}

BLE_CODE static u32 __get_assigned_hw_physical_distance_us(struct ble_hw *hw) {
    struct ble4_hw *ble = hw->ble4_hw;
    u8 state = ble->state;
    u32 counter = REG32(state == 6 ? 1867784u : 1867788u);
    u32 fine = REG32(1867796u);
    u32 add = (fine & 255u) + (counter & 1023u);
    u32 anchor = __get_ble_anchor_con(hw, 7);
    s32 cv = (s16)anchor;
    s32 offset = ((cv & 2048) ? (cv | (s32)0xfffff000u) : cv) + (625 - (s32)add);
    u32 shifted = (u32)offset << 16;
    s32 folded = (s16)(shifted >> 16);
    s32 pre = ((s32)shifted > 40894464) ? folded - 625 :
              ((s32)shifted < 0 ? folded + 625 : offset);
    u32 first_anchor = __hw_distance_slots(hw) * 625u;
    u16 first_fine = (u16)REG32(1867848u) & 1023u;
    u32 second_anchor = __hw_distance_slots(hw) * 625u;
    u16 second_fine = (u16)REG32(1867848u) & 1023u;
    u32 index = first_anchor != second_anchor;
    u32 chosen_anchor = index ? second_anchor : first_anchor;
    u32 chosen_fine = index ? second_fine : first_fine;
    return (u32)((s16)pre + (s32)add) + chosen_anchor - chosen_fine;
}

BLE_CODE static void __master_set_distance(struct ble_hw *hw, u32 distance_us) {
    u32 div = distance_us / 625u;
    u32 rem = distance_us % 625u;
    u32 clkn = REG32(1867784u);
    u32 finecnt = REG32(1867796u);
    u32 fine = (REG32(1867848u) & 1023u) + rem;
    u32 over = fine > 624u;
    u32 bitoffset = over ? fine - 625u : fine;
    u32 slot = (div - !over) +
        ((((625u - (clkn & 1023u)) - (finecnt & 255u)) + bitoffset) > 624u);
    __set_anchor_cnt(hw, slot);
    __set_bitoffset(hw, (int)bitoffset);
}

BLE_CODE void multilink_debug_trace_info(void) { }
/* SDK ABI is zeroext i8 although sdk.h declares C bool (i1). */
BLE_CODE u8 master_setup_connection_updatea_first_data_location_ir(struct ble_hw *conn_hw)
    __asm__("master_setup_connection_updatea_first_data_location");
BLE_CODE u8 master_setup_connection_updatea_first_data_location_ir(struct ble_hw *conn_hw)
{ (void)conn_hw; return 0; }

BLE_CODE void master_setup_first_data_location(struct ble_hw *conn_hw,
                                      struct ble_conn_param *conn_param) {
    if (!config_btctler_le_master_multilink) return;
    s32 min = conn_param->winoffset * 1250 + 1250;
    u8 lead = Lead_hw_id;
    if (lead == 0xff) return;
    s32 max = conn_param->winsize * 1250 + min;
    u32 distance = __get_assigned_hw_physical_distance_us(bb_hdl->hw_base.hw + lead);
    s32 adjusted = (s32)distance + 1250;
    __master_set_distance(conn_hw, (adjusted < min || adjusted > max) ? (u32)min : (u32)adjusted);
}

BLE_CODE void ajust_interspace_between_connected(void) { }

BLE_CODE static u8 rewrite_ConnReq_WinOffset_WinSize(struct ble4_hw *init_hw,
                                            u16 scan_interval_slots,
                                            u32 hw_distance_us) {
    u16 add = (u16)(scan_interval_slots + 3u);
    u32 lower = add * 625u + 1250u;
    u32 target = hw_distance_us + 1250u;
    if (target < lower) return 0;
    struct ble_tx **tx = init_hw->tx;
    tx[0]->data[20] = (u8)((target - lower) / 1250u);
    tx[1]->data[20] = tx[0]->data[20];
    tx[0]->data[19] = (u8)(((add > 2) ? add : 2) >> 1);
    tx[1]->data[19] = tx[0]->data[19];
    const struct ble_handler *handler = init_hw->handler;
    if (handler && handler->sync_param_handler)
        handler->sync_param_handler(init_hw->priv, &tx[0]->data[19], 3);
    return 1;
}

BLE_CODE void find_interspace_between_connected(struct ble4_hw *init_hw) {
    if (!config_btctler_le_master_multilink) return;
    union __connected_distance distance[8];
    memcpy(distance, find_interspace_distance, sizeof(distance));
    Lead_hw_id = 0xff;
    u32 count = 0;
    for (u32 id = 0; id < 8; id++) {
        struct ble_baseband_layer_handle *base = bb_hdl;
        if (!base->hw_base.inst[id]) continue;
        struct ble_hw *hw = base->hw_base.hw + id;
        struct ble4_hw *ble = hw->ble4_hw;
        if (!ble || ble->state != 6) continue;
        distance[count]._u32 = (distance[count]._u32 & 0xf0u) |
            ((u32)__get_assigned_hw_physical_distance_us(hw) << 8) | id;
        count++;
    }
    if (!count) return;
    for (u32 end = count; end > 1; end--)
        for (u32 i = 0; i + 1 < end; i++)
            if ((distance[i]._u32 >> 8) > (distance[i + 1]._u32 >> 8)) {
                u32 tmp = distance[i]._u32;
                distance[i]._u32 = distance[i + 1]._u32;
                distance[i + 1]._u32 = tmp;
            }
    u32 i;
    u32 hw_distance;
    for (i = 0; ; i++) {
        /* Keep the SDK's sentinel read before any count bound. With eight
         * occupied slots and no qualifying gap, this reads distance[8]. */
        u32 next = distance[i + 1]._u32;
        if (next == 0xffffffffu) { hw_distance = distance[i]._u32 >> 8; break; }
        hw_distance = distance[i]._u32 >> 8;
        if ((next >> 8) - hw_distance > 1874u) break;
    }
    Lead_hw_id = (u8)(distance[i]._u32 & 15u);
    (void)rewrite_ConnReq_WinOffset_WinSize(init_hw, init_hw->scan_interval, hw_distance);
}

BLE_CODE void suspend_init(struct ble4_hw *close_hw) {
    if (!config_btctler_le_master_multilink || close_hw->state != 6) return;
    for (u32 id = 0; id < 8; id++) {
        struct ble_baseband_layer_handle *base = bb_hdl;
        if (!base->hw_base.inst[id]) continue;
        struct ble_hw *hw = base->hw_base.hw + id;
        struct ble4_hw **pble = &hw->ble4_hw;
        if (!*pble) continue;
        local_irq_disable();
#ifndef UNITTEST
        __asm__ volatile("csync;");
#endif
        struct ble4_hw *ble = *pble;
        if (ble->state == 3) {
            ble->sub_state = 3;
            u32 v = REG32(1867816u);
            u32 hid = HW_ID(hw);
            u32 cleared = v & ~(1u << hid);
            u32 hid_again = HW_ID(hw);
            REG32(1867816u) = cleared;
            v = REG32(1867816u);
            hid = HW_ID(hw);
            cleared = v & ~(256u << hid);
            hid_again = HW_ID(hw);
            REG32(1867816u) = cleared;
            v = REG32(1867852u);
            hid = HW_ID(hw);
            cleared = v & ~(1u << hid);
            hid_again = HW_ID(hw);
            (void)hid_again;
            REG32(1867852u) = cleared;
            __set_anchor_cnt_disable(hw);
            local_irq_enable();
            return;
        }
        local_irq_enable();
    }
}

BLE_CODE void refind_interspace_between_connected(void) {
    if (!config_btctler_le_master_multilink) return;
    struct ble_baseband_layer_handle *base = bb_hdl;
    for (u32 id = 0; id < 8; id++) {
        if (!base->hw_base.inst[id]) continue;
        struct ble4_hw **pble = &(base->hw_base.hw + id)->ble4_hw;
        struct ble4_hw *ble = *pble;
        if (!ble || ble->state != 3) continue;
        find_interspace_between_connected(ble);
        (*pble)->sub_state = 1;
        return;
    }
}

BLE_CODE int ble_master_multilink_schedule(struct ble_hw *hw, u32 min_slots,
                                  u8 credits, u8 hw_number) {
    (void)hw_number;
    if (!config_btctler_le_master_multilink) return 0;
    struct ble4_hw *ble = hw->ble4_hw;
    u32 window = ble->scan_window;
    u32 idle_window = ble->scan_idle;
    if (min_slots <= 4) return 1;
    u8 state = ble->state;
    if (credits == 1 && state == 1) {
        ble->is_scan_multilink_state = 1;
        u32 sub = min_slots - 1;
        u32 interval = ble->scan_interval;
        u32 win, idle;
        if (sub < interval) {
            bool exceeds = sub > window;
            win = exceeds ? window : sub;
            idle = exceeds ? sub - window : 0;
        } else { win = window; idle = interval - window; }
        __set_scan_window(hw, win, interval + 48);
        le_hw_resume(hw, idle);
        return 1;
    }
    if (credits != 3 || state != 3) return 1;
    ble->is_scan_multilink_state = 0;
    u32 interval = ble->scan_interval;
    bool cap = interval > min_slots;
    u32 target = cap ? min_slots : interval;
    u8 lead = Lead_hw_id;
    if (lead != 0xff) {
        if (ble->sub_state == 3) return 1;
        u32 distance = __get_assigned_hw_physical_distance_us(bb_hdl->hw_base.hw + lead);
        if (!rewrite_ConnReq_WinOffset_WinSize(ble, (u16)target, distance)) return 1;
    }
    __set_scan_window(hw, cap ? min_slots - 4 : window, target);
    le_hw_resume(hw, cap ? 0 : idle_window);
    return 1;
}

BLE_CODE u8 mult_slave_or_master_check_up_point(void) {
    u8 ids[8]; u8 tids[8];
    memset(ids, 0xff, sizeof(ids)); memset(tids, 0xff, sizeof(tids));
    u32 n = 0, tn = 0;
    for (u32 i = 0; i < 8; i++) {
        if (!m_s.opt_update_temp[i]) continue;
        if (!m_s.opt_update_timer[i]) ids[n++] = (u8)i;
        else tids[tn++] = (u8)i;
    }
    if (n && tn) { btctrler_assert_failed("multilink update point", 2033); return 0xff; }
    if (tn > 1) { btctrler_assert_failed("multilink update timers", 2038); return 0xff; }
    if (tn == 1) return tids[0];
    if (n > 1) { if (log_tag_const_e_LL) log_print(4, 0, "<Error>: [LL]__________mult_done_conn_up________\r\n"); return 0xff; }
    return n == 1 ? ids[0] : 0xff;
}

BLE_CODE u8 mult_slave_or_master_get_con_num(void) { u8 n=0; for (u32 i=0;i<8;i++) n += m_s.opt_con_id[i] != 0xff; return n; }
BLE_CODE u8 mult_slave_or_master_get_adv_num(void) { u8 n=0; for (u32 i=0;i<8;i++) n += m_s.opt_adv_hw_id[i] != 0xff; return n; }
BLE_CODE void mult_slave_or_master_fist_con_event_set(u8 temp) { m_s.opt_fist_con_event_temp = temp; }

BLE_CODE void mult_slave_or_master_set_adv_interval(u8 id, u16 interval) {
    u8 adv_count = mult_slave_or_master_get_adv_num();
    if (adv_count) {
        u8 con_count = mult_slave_or_master_get_con_num();
        if (interval < 40 && con_count) {
            if (log_tag_const_e_LL) log_print(4, 0, "<Error>: [LL]mult_adv_intervali_too_small\r\n");
            btctrler_assert_failed("mult_adv_intervali_too_small", 2124);
        }
    }
    m_s.opt_adv_interval[id] = interval;
    m_s.opt_adv_open_event[id] = 1;
}
BLE_CODE void mult_slave_or_master_set_scan_param(u8 id,u16 interval,u16 win) { m_s.opt_scan_id=id; m_s.opt_scan_interval=interval; m_s.opt_scan_win=win; }
BLE_CODE void mult_slave_or_master_set_init_param(u8 id,u16 interval,u16 win) { m_s.opt_init_id=id; m_s.opt_init_scan_interval=interval; m_s.opt_init_scan_win=win; }
BLE_CODE void mult_slave_or_master_save_conn_id(u8 id) { for (u32 i=0;i<8;i++) if (m_s.opt_con_id[i]==0xff) { m_s.opt_con_id[i]=id; return; } }
BLE_CODE void mult_slave_or_master_get_rx_irq_hw(u8 id) { m_s.opt_rx_irq_hw_id=id; }

BLE_CODE static void mult_slave_or_master_open_hw_state(struct ble_hw *hw) {
    ble_hw_enable(hw, 0);
    ble_power_put(hw);
    ble_power_get(hw);
}

BLE_CODE static void mult_slave_or_master_hw_exit_latency(struct ble_hw *hw) {
    struct ble4_hw *ble = hw->ble4_hw;
    if (ble->latency_en) {
        if (!ble->mult_latency_temp) return;
        ble->mult_latency_temp = 0;
        if (ble->hold_cnt) __set_latency_disable(hw);
        return;
    }
    u32 a9 = __get_ble_anchor_con(hw, 9);
    u32 a2 = __get_ble_anchor_con(hw, 2);
    __set_ble_anchor_con(hw, 2, a2 & 62976u);
    __set_ble_anchor_con(hw, 9, (a9 & 65471u) | 64u);
}

BLE_CODE static void mult_slave_or_master_exit_latency_all_hw(void) {
    for (u32 id = 0; id < 8; id++) {
        struct ble_baseband_layer_handle *base = bb_hdl;
        if (!base->hw_base.inst[id]) continue;
        struct ble_hw *hw = base->hw_base.hw + id;
        struct ble4_hw *ble = hw->ble4_hw;
        if (ble && ((ble->state & 0xfe) == 6))
            mult_slave_or_master_hw_exit_latency(hw);
    }
}

BLE_CODE static void mult_slave_or_master_exit_latency_this_hw(u8 hw_id) {
    for (u32 id = 0; id < 8; id++) {
        struct ble_baseband_layer_handle *base = bb_hdl;
        if (!base->hw_base.inst[id]) continue;
        struct ble_hw *hw = base->hw_base.hw + id;
        struct ble4_hw **pble = &hw->ble4_hw;
        struct ble4_hw *ble = *pble;
        if (!ble || (ble->state & 0xfe) != 6) continue;
        if (HW_ID(hw) == hw_id) {
            mult_slave_or_master_hw_exit_latency(hw);
            continue;
        }
        ble = *pble;
        if (ble->latency_en) {
            if (ble->latency_substate == 1) {
                __set_latency_hold_cnt(1, ble, 0);
                (*pble)->mult_latency_temp = 1;
            }
            if (mult_slave_or_master_check_up_point() != 0xff)
                __set_latency_enable(hw);
        } else {
            u32 a9 = __get_ble_anchor_con(hw, 9);
            u32 a2 = __get_ble_anchor_con(hw, 2);
            __set_ble_anchor_con(hw, 2, (a2 & 62976u) | 2303u);
            __set_ble_anchor_con(hw, 9, (a9 & 65471u) | 64u);
        }
    }
}

BLE_CODE static void mult_slave_or_master_conn_update_timer(void *p) { m_s.opt_update_temp[*(u8 *)p] = 1; }

BLE_CODE static void mult_slave_or_master_mult_adv_event_end(void) { m_s.opt_mult_adv_event_timer = 0; m_s.opt_is_idle = 1; }

BLE_CODE static void mult_slave_or_master_adv_event_open(void *p) { m_s.opt_adv_open_event[*(u8 *)p] = 1; }

BLE_CODE static void mult_slave_or_master_adv_keep_open(void *p)
{
    local_irq_disable();
    u8 adv_lock=0;
    for (u32 i=0;i<8;i++) {
        u8 id=m_s.opt_adv_hw_id[i];
        if (id==0xff) continue;
        struct ble_baseband_layer_handle *base=bb_hdl;
        if (!base->hw_base.inst[id]) continue;
        struct ble_hw *hw=base->hw_base.hw+id;
        struct ble4_hw *ble=hw->ble4_hw;
        if (!ble || ble->state!=2 || !m_s.opt_adv_open_event[id]) continue;
        if (*(u8 *)p==id) {
            __set_anchor_cnt(hw,0);
            ble_hw_enable(hw,0);
            m_s.opt_is_idle=0;
            u16 timer=m_s.opt_mult_adv_event_timer;
            if (timer) { sys_timeout_del(timer); m_s.opt_mult_adv_event_timer=0; }
            m_s.opt_mult_adv_event_timer=usr_timeout_add(0,
                (void (*)(void *))mult_slave_or_master_mult_adv_event_end,6,1);
            m_s.opt_adv_open_event[id]=0;
        }
        u8 increment=(u8)(adv_lock+1);
        if (increment==2) {
            (void)usr_timeout_add(&m_s.opt_adv_hw_id[i],mult_slave_or_master_adv_keep_open,6,1);
            adv_lock=2;
        } else adv_lock=increment;
    }
    local_irq_enable();
}

BLE_CODE static void mult_slave_or_master_adv_sm_end(void *p) {
    (void)p;
    local_irq_disable();
    for (u32 id = 0; id < 8; id++) {
        struct ble_baseband_layer_handle *base = bb_hdl;
        if (!base->hw_base.inst[id]) continue;
        struct ble_hw *hw = base->hw_base.hw + id;
        struct ble4_hw *ble = hw->ble4_hw;
        if (ble && ble->state == 2) ble_hw_disable(hw);
    }
    local_irq_enable();
}

BLE_CODE static void mult_slave_or_master_scan_or_init_win_close(void *p) {
    local_irq_disable();
    u8 id = *(u8 *)p;
    if (id != 0xff) {
        struct ble_baseband_layer_handle *base = bb_hdl;
        if (base->hw_base.inst[id]) {
            struct ble_hw *hw = base->hw_base.hw + id;
            struct ble4_hw *ble = hw->ble4_hw;
            if (ble && mult_slave_or_master_get_con_num() &&
                (ble->state == 1 || ble->state == 3)) ble_hw_disable(hw);
        }
    }
    m_s.opt_is_idle = 1;
    local_irq_enable();
}

BLE_CODE void mult_slave_or_master_del_conn_id(struct ble_hw *hw, u8 del_id) {
    for (u32 i = 0; i < 8; i++) {
        if (m_s.opt_con_id[i] == del_id) {
            m_s.opt_con_id[i] = 0xff;
            m_s.opt_next_id = 0xff;
            break;
        }
    }
    if (!mult_slave_or_master_get_con_num()) {
        if (mult_slave_or_master_get_adv_num()) for (u32 i = 0; i < 8; i++) {
            u8 id = m_s.opt_adv_hw_id[i];
            if (id == 0xff) continue;
            struct ble_baseband_layer_handle *base = bb_hdl;
            if (!base->hw_base.inst[id]) continue;
            struct ble_hw *adv = base->hw_base.hw + id;
            if (adv->ble4_hw && adv->ble4_hw->state == 2) mult_slave_or_master_open_hw_state(adv);
        }
        u8 id = m_s.opt_scan_id;
        if (id != 0xff) {
            struct ble_baseband_layer_handle *base = bb_hdl;
            if (base->hw_base.inst[id]) {
                struct ble_hw *scan = base->hw_base.hw + id;
                if (scan->ble4_hw && scan->ble4_hw->state == 1) {
                    __set_scan_window(scan, m_s.opt_scan_win, m_s.opt_scan_interval);
                    mult_slave_or_master_open_hw_state(scan);
                }
            }
        }
        id = m_s.opt_init_id;
        if (id != 0xff) {
            struct ble_baseband_layer_handle *base = bb_hdl;
            if (base->hw_base.inst[id]) {
                struct ble_hw *init = base->hw_base.hw + id;
                if (init->ble4_hw && init->ble4_hw->state == 3) {
                    __set_scan_window(init, m_s.opt_init_scan_win, m_s.opt_init_scan_interval);
                    mult_slave_or_master_open_hw_state(init);
                }
            }
        }
    }
    struct ble4_hw *ble = hw->ble4_hw;
    if (ble) {
        if (ble->state == 2) {
            m_s.opt_adv_hw_id[del_id] = 0xff;
            u16 timer = m_s.opt_adv_timer[del_id];
            if (timer) { sys_timeout_del(timer); m_s.opt_adv_timer[del_id] = 0; }
        } else mult_slave_or_master_exit_latency_all_hw();
    }
    if (m_s.opt_scan_id == del_id) m_s.opt_scan_id = 0xff;
    if (m_s.opt_init_id == del_id) m_s.opt_init_id = 0xff;
    m_s.opt_adv_open_event[del_id] = 0;
    m_s.opt_is_idle = 1;
    m_s.opt_event_ct = 0;
    for (u32 i = 0; i < 8; i++) if (i == del_id) {
        m_s.opt_update_temp[del_id] = 0;
        u16 timer = m_s.opt_update_timer[del_id];
        if (timer) { usr_timeout_del(timer); m_s.opt_update_timer[del_id] = 0; }
    }
    if (mult_slave_or_master_get_con_num() == 1) {
        for (u32 i = 0; i < 8; i++) {
            m_s.opt_update_temp[i] = 0;
            u16 timer = m_s.opt_update_timer[i];
            if (timer) { usr_timeout_del(timer); m_s.opt_update_timer[i] = 0; }
        }
    }
}

BLE_CODE static void mult_slave_or_master_sort(u8 count, u16 *list) {
    if (count < 2) return;
    for (u32 i = 0; i < count; i++)
        for (u32 j = 0; j + 1 < (u32)count - i; j++)
            if (list[j] > list[j + 1]) { u16 t=list[j]; list[j]=list[j+1]; list[j+1]=t; }
}

BLE_CODE static void mult_slave_or_master_polling_mode(u8 event_id, const u8 *num_id)
{
    u8 count = mult_slave_or_master_get_con_num();
    u16 clocks[count], timeouts[count];
    u8 info = count;
    u16 next_clock;       /* intentionally undefined if opt_next_id is not present */
    u16 next_interval;    /* same SDK VLA/SSA behavior */
    if (m_s.opt_next_id == 0xff) m_s.opt_next_id = event_id;
    bool immediate = false;
    for (u32 i=0; i<8; i++) {
        struct ble_baseband_layer_handle *base = bb_hdl;
        if (!base->hw_base.inst[i]) continue;
        struct ble_hw *hw=base->hw_base.hw+i;
        struct ble4_hw **pble=&hw->ble4_hw;
        struct ble4_hw *ble=*pble;
        if (!ble || (ble->state & 0xfe) != 6) continue;
        u8 slot=--info;
        u16 clkn=(u16)__get_clkn(hw);
        clocks[slot]=clkn;
        timeouts[slot]=(*pble)->ll_timeout;
        u8 id=HW_ID(hw);
        if (id != m_s.opt_next_id) continue;
        next_clock=clkn;
        next_interval=(u16)(*pble)->interval;
        if (id == event_id || m_s.opt_rx_irq_hw_id == id) continue;
        if (!(*pble)->latency_en || (*pble)->latency_substate != 0) continue;
        m_s.opt_rx_irq_hw_id=id;
        immediate=true;
        break;
    }
    u8 candidate;
    if (immediate) candidate=m_s.opt_rx_irq_hw_id;
    else {
        mult_slave_or_master_sort(count, clocks);
        mult_slave_or_master_sort(count, timeouts);
        u8 next=m_s.opt_next_id;
        bool mismatch=next != event_id;
        bool irq_match=m_s.opt_rx_irq_hw_id == next;
        if (!mismatch && !irq_match) {
            if (m_s.opt_id == event_id) {
                u8 ct=++m_s.opt_event_ct;
                u16 interval_div=(u16)(next_interval / 1000u);
                u16 quotient=(u16)(timeouts[0] / interval_div);
                if ((s32)(u32)quotient - ct < 4) m_s.opt_rx_irq_hw_id=event_id;
            }
            candidate=next;
        } else if (mismatch && irq_match) {
            if (log_tag_const_e_LL) {
                log_print(4,0,"<Error>: [LL]rx_irq error\r\n");
                candidate=m_s.opt_next_id; /* SDK reload after the callback */
            } else candidate=next;
        } else if (next == event_id || irq_match) {
            candidate=next;
        } else {
            u8 pos=0;
            for (u32 i=0;i<count;i++) if (next_clock == clocks[i]) pos=(u8)i;
            u8 active=next;
            for (u32 i=0;i<pos;i++) {
                /* The operands are zero-extended u16 values before signed i32 subtraction. */
                if ((s32)((u32)next_clock - (u32)clocks[i]) < 5) {
                    mult_slave_or_master_exit_latency_this_hw(next);
                    active=m_s.opt_next_id;
                    m_s.opt_id=active;
                    break;
                }
            }
            u8 id_for_candidate=active;
            if (m_s.opt_id == event_id) {
                u8 ct=++m_s.opt_event_ct;
                u16 interval_div=(u16)(next_interval / 1000u);
                if (next_clock>240 && count>2 && ct>5 &&
                    (u32)interval_div > (u32)timeouts[0]+150u)
                    m_s.opt_rx_irq_hw_id=id_for_candidate;
            }
            candidate=id_for_candidate;
        }
    }
    u8 current_rx=m_s.opt_rx_irq_hw_id;
    if (candidate != current_rx) return;
    for (u32 i=0;i<count;i++) {
        if (num_id[i] != candidate) continue;
        m_s.opt_next_id=num_id[i == (u32)count-1 ? 0 : i+1];
        break;
    }
    m_s.opt_event_ct=0;
    mult_slave_or_master_exit_latency_all_hw();
}

BLE_CODE void mult_slave_or_master_event_enter(u8 cur_hw_id)
{
    if (m_s.opt_fist_con_event_temp) { m_s.opt_fist_con_event_temp=0; return; }
    u8 count=mult_slave_or_master_get_con_num();
    u8 log_enabled=log_tag_const_e_LL;
    u16 clocks[count];
    u8 event_ids[count];
    u8 info=count;
    for (u32 i=0;i<8;i++) {
        struct ble_baseband_layer_handle *base=bb_hdl;
        if (!base->hw_base.inst[i]) continue;
        struct ble_hw *hw=base->hw_base.hw+i;
        struct ble4_hw **pble=&hw->ble4_hw;
        struct ble4_hw *ble=*pble;
        if (!ble) continue;
        if (count>1) {
            u16 event_count=__get_conn_event_count(hw);
            u32 interval=__get_interval(hw);
            for (u32 j=0;j<8;j++) {
                u8 conn_id=m_s.opt_con_id[j];
                u8 hwid=HW_ID(hw);
                if (conn_id != hwid) continue;
                ble=*pble;
                if (ble->instant_valid != 1) continue;
                u32 instant=ble->instant;
                u32 before=instant-1u;
                bool later=(s32)before > (s32)event_count;
                bool is_current=(conn_id == cur_hw_id);
                if (is_current && later && interval>200 && !m_s.opt_update_temp[conn_id]) {
                    u16 timer=usr_timeout_add(&m_s.opt_con_id[j],
                        mult_slave_or_master_conn_update_timer,
                        (interval*625u*(before-event_count))/1000u-20u,1);
                    m_s.opt_update_timer[conn_id]=timer;
                } else if ((s32)event_count > (s32)(instant-4u) && later &&
                           !m_s.opt_update_temp[conn_id]) {
                    m_s.opt_update_temp[conn_id]=1;
                }
                ble=*pble;
                if (is_current && event_count == ble->instant-1u) {
                    m_s.opt_update_temp[conn_id]=0;
                    m_s.opt_update_timer[conn_id]=0;
                    m_s.opt_next_id=m_s.opt_rx_irq_hw_id;
                    ble=*pble;
                }
                ble=*pble;
                if (!ble->instant_valid && m_s.opt_update_temp[conn_id]) {
                    if (log_enabled)
                        log_print(4,0,"<Error>: [LL]__mult_slave_or_master_up_error__\r\n");
                    btctrler_assert_failed("multilink update instant",1951);
                }
            }
        }
        ble=*pble;
        if (ble->state==7 || ble->state==6) {
            u8 slot=--info;
            event_ids[slot]=HW_ID(hw);
            clocks[slot]=(u16)__get_clkn(hw);
            u8 id=HW_ID(hw);
            if (m_s.opt_adv_open_event[id]) {
                id=HW_ID(hw);
                m_s.opt_adv_open_event[id]=0;
            }
        } else if (ble->state==1) {
            ble_hw_disable(hw);
        } else if (ble->state==2) {
            u8 id=HW_ID(hw);
            if (m_s.opt_adv_timer[id]) ble_hw_disable(hw);
            u8 write_id=HW_ID(hw);
            u8 index=HW_ID(hw);
            m_s.opt_adv_hw_id[index]=write_id;
        }
    }
    if (count>1) {
        u8 point=mult_slave_or_master_check_up_point();
        if (point!=0xff) mult_slave_or_master_exit_latency_this_hw(point);
        if (mult_slave_or_master_check_up_point()==0xff)
            mult_slave_or_master_polling_mode(cur_hw_id,event_ids);
        mult_slave_or_master_sort(count,clocks);
    }
    u16 clkn=clocks[0];
    u8 open_count=0;
    for (u32 i=0;i<8;i++) open_count += m_s.opt_adv_open_event[i] != 0;
    bool adv_protocol=false;
    if (m_s.opt_init_id==0xff && open_count &&
        mult_slave_or_master_check_up_point()==0xff) {
        u8 adv_count=mult_slave_or_master_get_adv_num();
        if (clkn>=8 && adv_count) adv_protocol=true;
    }
    if (adv_protocol) {
        u8 adv_lock=0;
        for (u32 i=0;i<8;i++) {
            u8 adv_id=m_s.opt_adv_hw_id[i];
            if (adv_id==0xff) continue;
            struct ble_baseband_layer_handle *base=bb_hdl;
            if (!base->hw_base.inst[adv_id]) continue;
            struct ble_hw *hw=base->hw_base.hw+adv_id;
            struct ble4_hw *ble=hw->ble4_hw;
            if (!ble || ble->state!=2 || !m_s.opt_adv_open_event[adv_id]) continue;
            u16 *timer=&m_s.opt_adv_timer[adv_id];
            u8 lock_after=adv_lock;
            if (*timer) {
                if (!adv_lock) {
                    __set_anchor_cnt(hw,0);
                    ble_hw_enable(hw,0);
                    m_s.opt_is_idle=0;
                    u16 mult_timer=m_s.opt_mult_adv_event_timer;
                    if (mult_timer) { sys_timeout_del(mult_timer); m_s.opt_mult_adv_event_timer=0; }
                    m_s.opt_mult_adv_event_timer=usr_timeout_add(0,
                        (void (*)(void *))mult_slave_or_master_mult_adv_event_end,6,1);
                    lock_after=1;
                } else {
                    u8 increment=(u8)(adv_lock+1);
                    if (open_count>1 && (clkn>>3)>open_count && increment==2) {
                        (void)usr_timeout_add(&m_s.opt_adv_hw_id[i],mult_slave_or_master_adv_keep_open,6,1);
                        lock_after=2;
                    } else lock_after=increment;
                }
            }
            sys_timeout_del(*timer);
            *timer=0;
            u16 interval=m_s.opt_adv_interval[adv_id];
            *timer=sys_timeout_add(&m_s.opt_adv_hw_id[i],mult_slave_or_master_adv_event_open,
                                   ((u32)interval*625u)/1000u);
            u16 fresh_interval=m_s.opt_adv_interval[adv_id];
            if (fresh_interval<=clkn && clkn%fresh_interval<8) {
                u32 numerator=(u32)fresh_interval*625u-2500u;
                u32 product=numerator*(u32)(clkn/fresh_interval);
                s32 timeout=(s32)product/1000;
                (void)sys_timeout_add(0,mult_slave_or_master_adv_sm_end,(u32)timeout);
            }
            if (lock_after<2) m_s.opt_adv_open_event[adv_id]=0;
            adv_lock=lock_after;
        }
        return;
    }
    if (config_btctler_le_master_multilink) return;
    if (m_s.opt_scan_id!=0xff && m_s.opt_init_id!=0xff) {
        btctrler_assert_failed("multilink scan/init IDs",1848);
    }
    u8 scan_id=m_s.opt_scan_id, init_id=m_s.opt_init_id;
    u8 id=scan_id & init_id;
    if (id==0xff || clkn<5) return;
    struct ble_baseband_layer_handle *base=bb_hdl;
    if (!base->hw_base.inst[id]) return;
    struct ble_hw *hw=base->hw_base.hw+id;
    struct ble4_hw *ble=hw->ble4_hw;
    if (!ble) return;
    m_s.opt_is_idle=0;
    if (ble->state==1) {
        u16 win=m_s.opt_scan_win;
        u16 interval=m_s.opt_scan_interval;
        u32 next=(clkn > ((u32)interval<<1)) ? interval : (u16)(clkn<<1);
        u16 selected=(win<clkn) ? win : clkn;
        u16 scan_window=(u16)(selected-3u);
        __set_scan_window(hw,scan_window,next);
        u16 fresh_interval=m_s.opt_scan_interval;
        u32 delay;
        if (next==fresh_interval) {
            s32 slots=(s32)(u32)clkn+2-(s32)(u32)m_s.opt_scan_win;
            delay=(u32)((slots*625)/1000);
        } else delay=(u32)(((s32)(u32)clkn-1)*625/1000);
        (void)usr_timeout_add(&m_s.opt_scan_id,mult_slave_or_master_scan_or_init_win_close,delay,1);
        __set_anchor_cnt(hw,0);
        ble_hw_enable(hw,0);
    } else if (ble->state==3) {
        u32 scan_window=(u32)clkn-3u;
        u32 interval=(u32)clkn<<1;
        __set_scan_window(hw,scan_window,interval);
        (void)usr_timeout_add(&m_s.opt_init_id,mult_slave_or_master_scan_or_init_win_close,
                              (u32)(((s32)((u32)clkn*625u)-625)/1000),1);
        __set_anchor_cnt(hw,0);
        ble_hw_enable(hw,0);
    }
}

#ifdef UNITTEST
#include "test.h"
#include <stdlib.h>

struct ble_baseband_layer_handle *bb_hdl;
int config_btctler_le_master_multilink;
u8 log_tag_const_e_LL;
static struct ble_hw *fixture_hws;
static u32 fixture_enable_count, fixture_disable_count, fixture_anchor_set;
static u32 fixture_scan_calls, fixture_resume_count;
static u16 fixture_timer_id;
static void (*fixture_timer_cb)(void *);
static void (*fixture_usr_cb)(void *);
static u32 fixture_timer_delay, fixture_usr_delay;
static u32 fixture_clkn=20;
u8 HW_ID(struct ble_hw *hw) { return (u8)(hw-fixture_hws); }
u32 __get_clkn(struct ble_hw *hw) { return HW_ID(hw)==0 ? fixture_clkn : 0; }
u32 __get_interval(struct ble_hw *hw) { (void)hw; return 24; }
u16 __get_conn_event_count(struct ble_hw *hw) { (void)hw; return 1; }
void ble_hw_enable(struct ble_hw *hw, int slot) { (void)hw; (void)slot; fixture_enable_count++; }
void ble_hw_disable(struct ble_hw *hw) { (void)hw; fixture_disable_count++; }
static u32 fixture_scan_win, fixture_scan_interval, fixture_scan_mutate;
void __set_scan_window(struct ble_hw *hw, int win, int interval)
{ (void)hw; fixture_scan_win=(u32)win; fixture_scan_interval=(u32)interval;
  fixture_scan_calls++;
  if (fixture_scan_mutate) { m_s.opt_scan_win=40; m_s.opt_scan_interval=10; } }
void le_hw_resume(struct ble_hw *hw, int idle) { (void)hw; (void)idle; fixture_resume_count++; }
void __set_anchor_cnt_disable(struct ble_hw *hw) { (void)hw; }
void __set_latency_disable(struct ble_hw *hw) { (void)hw; }
void __set_latency_enable(struct ble_hw *hw) { (void)hw; }
void __set_latency_hold_cnt(u8 reset, struct ble4_hw *hw, u16 count)
{ (void)reset; (void)hw; (void)count; }
void local_irq_disable(void) { }
void local_irq_enable(void) { }
u16 usr_timeout_add(void *priv, void (*func)(void *), u32 delay, u8 reload)
{ (void)priv; (void)reload; fixture_usr_cb=func; fixture_usr_delay=delay; return ++fixture_timer_id; }
void usr_timeout_del(u16 id) { (void)id; }
u16 sys_timeout_add(void *priv, void (*func)(void *), u32 delay)
{ (void)priv; fixture_timer_cb=func; fixture_timer_delay=delay; return ++fixture_timer_id; }
void sys_timeout_del(u16 id) { (void)id; }
static u32 fixture_assert_count;
void btctrler_assert_failed(const char *what, int value)
{ (void)what; (void)value; fixture_assert_count++; }
void log_print(int level, const char *file, const char *fmt, ...)
{ (void)level; (void)file; (void)fmt; }

static u32 captured_anchor, captured_offset, set_order;
void __set_anchor_cnt(struct ble_hw *hw, u32 slot)
{ (void)hw; captured_anchor=slot; fixture_anchor_set=slot; set_order=set_order*10+1; }
void __set_bitoffset(struct ble_hw *hw, int bitoffset)
{ (void)hw; captured_offset=(u32)bitoffset; set_order=set_order*10+2; }
u32 __get_ble_anchor_con(struct ble_hw *hw, u8 index)
{
    (void)hw;
    if (fixture_anchor_sequence && fixture_anchor_pos < 5)
        return fixture_anchor_values[fixture_anchor_pos++];
    return index == 14 ? 0 : 0;
}
void __set_ble_anchor_con(struct ble_hw *hw, u8 index, u32 value)
{ (void)hw; (void)index; (void)value; }

static u8 callback_len;
static u8 *callback_data;
static void schedule_test_sync(void *priv, u8 *data, u16 len)
{
    (void)priv; callback_data=data; callback_len=(u8)len;
    CHECK(data[0] == 6 && data[1] == 1 && data[2] == 0);
    data[0] = 0xa5;
}

int main(void)
{
    static const u8 manager_init[100] = {
        0xff,0xff,0xff,0xff,0xff,0,1,0,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        [32]=0xff,[33]=0xff,[34]=0xff,[35]=0xff,
        [36]=0xff,[37]=0xff,[38]=0xff,[39]=0xff,
    };
    CHECK(sizeof(m_s) == sizeof(manager_init));
    CHECK(!memcmp(&m_s, manager_init, sizeof(manager_init)));
    for (u32 i=0;i<8;i++) CHECK(find_interspace_distance[i]._u32 == 0xffffffffu);
    CHECK(!strcmp(ble_slave_multlp_target.name, "ble_slave_mult"));
    CHECK(ble_slave_multlp_target.is_idle == mult_slave_or_master_idle_query);
    CHECK(mult_slave_or_master_get_con_num() == 0);
    CHECK(mult_slave_or_master_get_adv_num() == 0);
    for (u8 i=0;i<8;i++) mult_slave_or_master_save_conn_id(i);
    CHECK(mult_slave_or_master_get_con_num() == 8);
    mult_slave_or_master_save_conn_id(0);
    CHECK(mult_slave_or_master_get_con_num() == 8);
    struct ble_hw dummy_hw={0};
    mult_slave_or_master_del_conn_id(&dummy_hw, 3);
    CHECK(mult_slave_or_master_get_con_num() == 7);
    mult_slave_or_master_set_scan_param(2, 24, 19);
    CHECK(m_s.opt_scan_id == 2 && m_s.opt_scan_interval == 24 && m_s.opt_scan_win == 19);
    mult_slave_or_master_set_init_param(4, 30, 17);
    CHECK(m_s.opt_init_id == 4 && m_s.opt_init_scan_interval == 30 && m_s.opt_init_scan_win == 17);

    /* The raw IR reads 0x1c8008, 0x1c8014, then 0x1c8048 before setting
     * anchor and bit offset. For 1876 us: 3 slots, fine count wraps 625+1
     * to bit offset 0, and no extra anchor slot is needed. */
    test_regs[0]=100; test_regs[2]=10; test_regs[3]=624;
    test_reg_trace_len=0; set_order=0;
    __master_set_distance((struct ble_hw *)0, 1876);
    CHECK(test_reg_trace_len == 3);
    CHECK(test_reg_trace[0] == 1867784u && test_reg_trace[1] == 1867796u && test_reg_trace[2] == 1867848u);
    CHECK(captured_anchor == 3 && captured_offset == 0 && set_order == 12);
    test_regs[0]=100; test_regs[2]=10; test_regs[3]=1023;
    test_reg_trace_len=0; set_order=0;
    __master_set_distance((struct ble_hw *)0, 1877);
    CHECK(captured_anchor == 4 && captured_offset == 400);
    CHECK(test_reg_trace_len == 3 && set_order == 12);

    /* With zero fine count, 1875 us is exactly three slots. The SDK's
     * subtract-one path programs anchor 2 and offset 0. */
    test_regs[0]=100; test_regs[2]=10; test_regs[3]=0;
    __master_set_distance((struct ble_hw *)0, 1875);
    CHECK(captured_anchor == 2 && captured_offset == 0);

    struct ble4_hw distance_ble={0}; struct ble_hw distance_hw={0};
    distance_hw.ble4_hw=&distance_ble; distance_ble.state=6;
    test_regs[0]=100; test_regs[2]=10; test_regs[3]=20;
    u32 stable_samples[5]={50,10,0,10,0};
    memcpy(fixture_anchor_values,stable_samples,sizeof(stable_samples));
    fixture_anchor_pos=0; fixture_anchor_sequence=1;
    u32 stable_result=__get_assigned_hw_physical_distance_us(&distance_hw);
    CHECK(stable_result==7530);
    CHECK(fixture_anchor_pos==5);
    /* A changed anchor selects the second pair of anchor/fine samples. */
    u32 rollover_samples[5]={50,9,0,10,0};
    memcpy(fixture_anchor_values,rollover_samples,sizeof(rollover_samples));
    fixture_anchor_pos=0; test_regs[3]=30;
    u32 rollover_result=__get_assigned_hw_physical_distance_us(&distance_hw);
    CHECK(rollover_result==7520);
    CHECK(fixture_anchor_pos==5);
    /* A negative pre-offset folds through the signed 16-bit path, then adds
     * one slot before the selected hardware anchor is converted to time. */
    u32 folded_samples[5]={0,0,0,0,0};
    memcpy(fixture_anchor_values,folded_samples,sizeof(folded_samples));
    fixture_anchor_pos=0; test_regs[0]=1000; test_regs[2]=100; test_regs[3]=0;
    u32 folded_result=__get_assigned_hw_physical_distance_us(&distance_hw);
    CHECK(folded_result==1875);
    CHECK(fixture_anchor_pos==5);
    fixture_anchor_sequence=0;

    /* tx->data is a zero-length SDK tail array, so the buffers carry its
     * payload bytes. The callback mutates only packet 0 after both writes. */
    u8 payload0[24]={0}, payload1[24]={0};
    struct ble_tx *tx0=(struct ble_tx *)calloc(1, sizeof(struct ble_tx)+sizeof(payload0));
    struct ble_tx *tx1=(struct ble_tx *)calloc(1, sizeof(struct ble_tx)+sizeof(payload1));
    CHECK(tx0 && tx1);
    if (tx0 && tx1) {
        memcpy(tx0->data,payload0,sizeof(payload0)); memcpy(tx1->data,payload1,sizeof(payload1));
        struct ble_handler handler={0}; handler.sync_param_handler=schedule_test_sync;
        struct ble4_hw init={0}; init.tx[0]=tx0; init.tx[1]=tx1; init.handler=&handler;
        callback_data=0; callback_len=0;
        CHECK(rewrite_ConnReq_WinOffset_WinSize(&init,10,10000));
        CHECK(callback_data == &tx0->data[19] && callback_len == 3);
        CHECK(tx0->data[19] == 0xa5 && tx1->data[19] == 6);
        CHECK(tx0->data[20] == 1 && tx1->data[20] == 1);

        /* i16 add wraps 65534 + 3 to 1 before the unsigned offset math. */
        memset(tx0->data, 0, sizeof(payload0));
        memset(tx1->data, 0, sizeof(payload1));
        init.handler=NULL;
        CHECK(rewrite_ConnReq_WinOffset_WinSize(&init,65534,1250));
        CHECK(tx0->data[19] == 1 && tx1->data[19] == 1);
        CHECK(tx0->data[20] == 0 && tx1->data[20] == 0);
        free(tx0); free(tx1);
    }

    /* The configured path with a minimum window returns success before any
     * radio or parameter writes. */
    struct ble4_hw sched_ble={0}; struct ble_hw sched_hw={0};
    sched_hw.ble4_hw=&sched_ble; sched_ble.scan_window=11; sched_ble.scan_idle=7;
    config_btctler_le_master_multilink=1;
    u32 scan_before=fixture_scan_calls, resume_before=fixture_resume_count;
    CHECK(ble_master_multilink_schedule(&sched_hw,4,1,0)==1);
    CHECK(fixture_scan_calls==scan_before && fixture_resume_count==resume_before);
    CHECK(sched_ble.scan_window==11 && sched_ble.scan_idle==7 && !sched_ble.is_scan_multilink_state);
    CHECK(ble_master_multilink_schedule(&sched_hw,5,2,0)==1);
    CHECK(fixture_scan_calls==scan_before && fixture_resume_count==resume_before);
    CHECK(sched_ble.scan_window==11 && sched_ble.scan_idle==7 && !sched_ble.is_scan_multilink_state);
    config_btctler_le_master_multilink=0;
    CHECK(ble_master_multilink_schedule(&sched_hw,4,1,0)==0);
    CHECK(fixture_scan_calls==scan_before && fixture_resume_count==resume_before);
    config_btctler_le_master_multilink=1;

    /* Raw event-enter branch: for a connected clock at 20 with one eligible
     * advertisement, SDK disables the timed advertiser, enables it at the
     * event, replaces its 44 ms timer with 24*625/1000=15 ms, and clears the
     * open flag. Its callback sets that flag again from the pointed-to ID. */
    struct ble_hw hw_fixture[2]={{0}};
    struct ble4_hw ble_fixture[2]={0};
    struct ble_baseband_layer_handle base_fixture={0};
    fixture_hws=hw_fixture; bb_hdl=&base_fixture;
    base_fixture.hw_base.hw=hw_fixture;
    base_fixture.hw_base.inst[0]=base_fixture.hw_base.inst[1]=1;
    hw_fixture[0].ble4_hw=&ble_fixture[0]; hw_fixture[1].ble4_hw=&ble_fixture[1];
    ble_fixture[0].state=6; ble_fixture[1].state=2;
    memset(m_s.opt_con_id,0xff,sizeof(m_s.opt_con_id)); m_s.opt_con_id[0]=0;
    memset(m_s.opt_adv_hw_id,0xff,sizeof(m_s.opt_adv_hw_id)); m_s.opt_adv_hw_id[1]=1;
    memset(m_s.opt_adv_open_event,0,sizeof(m_s.opt_adv_open_event)); m_s.opt_adv_open_event[1]=1;
    memset(m_s.opt_adv_timer,0,sizeof(m_s.opt_adv_timer)); m_s.opt_adv_timer[1]=44;
    m_s.opt_adv_interval[1]=24; m_s.opt_init_id=0xff;
    m_s.opt_update_temp[0]=m_s.opt_update_timer[0]=0;
    m_s.opt_mult_adv_event_timer=0; m_s.opt_is_idle=1;
    config_btctler_le_master_multilink=1;
    fixture_enable_count=fixture_disable_count=fixture_anchor_set=0;
    fixture_timer_id=100; fixture_timer_cb=0; fixture_timer_delay=0;
    mult_slave_or_master_event_enter(0);
    CHECK(fixture_disable_count==1 && fixture_enable_count==1);
    CHECK(m_s.opt_mult_adv_event_timer==101 && m_s.opt_adv_timer[1]==102);
    CHECK(fixture_usr_cb==(void (*)(void *))mult_slave_or_master_mult_adv_event_end);
    CHECK(fixture_timer_cb==mult_slave_or_master_adv_event_open && fixture_timer_delay==15);
    CHECK(m_s.opt_adv_open_event[1]==0 && m_s.opt_is_idle==0);
    mult_slave_or_master_adv_event_open(&m_s.opt_adv_hw_id[1]);
    CHECK(m_s.opt_adv_open_event[1]==1);

    /* A sole update in either class returns its ID. Mixed classes and
     * multiple timed entries assert; multiple untimed entries log and return
     * no point. */
    memset(m_s.opt_update_temp,0,sizeof(m_s.opt_update_temp));
    memset(m_s.opt_update_timer,0,sizeof(m_s.opt_update_timer));
    CHECK(mult_slave_or_master_check_up_point()==0xff);
    m_s.opt_update_temp[2]=1;
    CHECK(mult_slave_or_master_check_up_point()==2);
    m_s.opt_update_temp[3]=1; m_s.opt_update_timer[3]=7;
    fixture_assert_count=0;
    CHECK(mult_slave_or_master_check_up_point()==0xff && fixture_assert_count==1);
    m_s.opt_update_temp[2]=0;
    CHECK(mult_slave_or_master_check_up_point()==3);
    m_s.opt_update_timer[3]=0; m_s.opt_update_temp[2]=m_s.opt_update_temp[4]=1;
    fixture_assert_count=0;
    CHECK(mult_slave_or_master_check_up_point()==0xff && fixture_assert_count==0);
    m_s.opt_update_temp[2]=m_s.opt_update_temp[4]=0;

    /* With scan_window writing parameters back through a callback, the true
     * state-1 delay branch rereads both fields. A negative eight-slot delta
     * is signed i32 arithmetic: (-8*625)/1000 truncates toward zero to -5. */
    memset(m_s.opt_con_id,0xff,sizeof(m_s.opt_con_id)); m_s.opt_con_id[0]=0;
    memset(m_s.opt_adv_open_event,0,sizeof(m_s.opt_adv_open_event));
    memset(m_s.opt_adv_hw_id,0xff,sizeof(m_s.opt_adv_hw_id));
    memset(m_s.opt_adv_timer,0,sizeof(m_s.opt_adv_timer));
    m_s.opt_scan_id=1; m_s.opt_init_id=0xff; m_s.opt_scan_interval=10; m_s.opt_scan_win=25;
    m_s.opt_update_temp[2]=m_s.opt_update_temp[3]=0;
    m_s.opt_update_timer[3]=0; config_btctler_le_master_multilink=0;
    ble_fixture[1].state=1; fixture_clkn=30; fixture_scan_mutate=1;
    fixture_timer_id=200; fixture_disable_count=0; fixture_usr_cb=0;
    mult_slave_or_master_event_enter(0);
    CHECK(fixture_scan_win==22 && fixture_scan_interval==10);
    CHECK(fixture_usr_cb==mult_slave_or_master_scan_or_init_win_close);
    CHECK(fixture_usr_delay==(u32)-5);
    fixture_scan_mutate=0;

    /* State 3 keeps the SDK's full i32 (clkn-3, clkn*2) window arguments,
     * including values beyond the signed 16-bit range. */
    m_s.opt_scan_id=0xff; m_s.opt_init_id=1; ble_fixture[1].state=3;
    fixture_clkn=40000; fixture_timer_id=300; fixture_usr_cb=0;
    mult_slave_or_master_event_enter(0);
    CHECK(fixture_scan_win==39997 && fixture_scan_interval==80000);
    CHECK(fixture_usr_delay==24999);

    /* The signed advertising-close product wraps as i32 before sdiv. */
    config_btctler_le_master_multilink=1; ble_fixture[1].state=2;
    m_s.opt_init_id=0xff; m_s.opt_adv_hw_id[1]=1; m_s.opt_adv_open_event[1]=1;
    m_s.opt_adv_timer[1]=44; m_s.opt_adv_interval[1]=2; m_s.opt_scan_id=0xff;
    fixture_clkn=8; fixture_timer_id=400; fixture_timer_cb=0;
    mult_slave_or_master_event_enter(0);
    CHECK(fixture_timer_cb==mult_slave_or_master_adv_sm_end);
    CHECK(fixture_timer_delay==(u32)-5);

    /* Two active links: mismatch plus RX match selects the RX candidate and
     * rotates next_id through the event IDs. */
    memset(m_s.opt_con_id,0xff,sizeof(m_s.opt_con_id));
    m_s.opt_con_id[0]=0; m_s.opt_con_id[1]=1;
    ble_fixture[0].state=ble_fixture[1].state=6;
    ble_fixture[0].interval=ble_fixture[1].interval=8000;
    ble_fixture[0].ll_timeout=ble_fixture[1].ll_timeout=20;
    ble_fixture[0].latency_en=ble_fixture[1].latency_en=0;
    m_s.opt_next_id=1; m_s.opt_rx_irq_hw_id=1; m_s.opt_event_ct=9;
    u8 two_ids[2]={0,1};
    mult_slave_or_master_polling_mode(0,two_ids);
    CHECK(m_s.opt_next_id==0 && m_s.opt_event_ct==0);

    /* The candidate still clears event_ct when it is absent from num_id. */
    m_s.opt_next_id=1; m_s.opt_rx_irq_hw_id=1; m_s.opt_event_ct=7;
    u8 no_candidate[2]={0,2};
    mult_slave_or_master_polling_mode(0,no_candidate);
    CHECK(m_s.opt_next_id==1 && m_s.opt_event_ct==0);

    return TEST_DONE("btctrler_multilink_schedule");
}
#endif
