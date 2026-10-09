/* BLE link-layer state and control-procedure support.  Keeps the SDK's
 * procedure FIFO wrap, short FIFO clear, access-address filters, and
 * allocation/assert paths, including their unchecked allocation continuation. */
#include "btctrler/glue.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LL_CODE __attribute__((section(".ble_ll_code")))
#define LL_CONST __attribute__((section(".ble_ll_const")))

/* Raw IR signatures differ from the recovered header's bool/int types. */
u8 __hci_emit_le_meta_event_ir(u8, const char *, ...) __asm__("__hci_emit_le_meta_event");
u8 ll_control_procedure_is_exist_ir(struct le_link *, u32) __asm__("ll_control_procedure_is_exist");
u8 ll_control_procedure_push_ir(struct le_link *, u8) __asm__("ll_control_procedure_push");
u8 device_addr_verify_ir(const u8 *, u8, u8) __asm__("device_addr_verify");
u8 __instant_link_lost_ir(u16, u16) __asm__("__instant_link_lost");
u8 hook_ll_receive_connection_param_req_ir(u16, const u8 *) __asm__("hook_ll_receive_connection_param_req");
u8 ll_ext_rx_post_process_ir(void) __asm__("ll_ext_rx_post_process");
u16 ll_get_update_handle_ir(void) __asm__("ll_get_update_handle");

extern BTCTRLER_CONFIG int config_btctler_le_roles;
extern int config_vendor_le_bb;
extern BTCTRLER_CONFIG int config_btctler_le_master_multilink;
extern BTCTRLER_CONFIG int config_btctler_le_acl_total_nums;
extern BTCTRLER_CONFIG int config_btctler_le_acl_packet_length;
extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern BTCTRLER_CONFIG u8 log_tag_const_i_LL, log_tag_const_e_LL;
extern BTCTRLER_CONFIG u8 log_tag_const_d_LL;
extern void log_print(int level, const char *tag, const char *format, ...);
extern int os_mutex_pend(OS_MUTEX *mutex, int timeout);
extern int os_mutex_post(OS_MUTEX *mutex);
extern int os_mutex_create(OS_MUTEX *mutex);
extern int os_mutex_del(OS_MUTEX *mutex, int timeout);
extern void printf_buf(const void *data, int len);
extern u8 power_is_poweroff_post(void);
extern void local_irq_disable(void);
extern void local_irq_enable(void);

static u16 ll_vendor_offset0 __attribute__((section(".ble_ll_data"))) = 9;
static u8 ll_vendor_instant_cnt __attribute__((section(".ble_ll_data"))) = 61;
struct link_layer_handle ll_hdl __attribute__((section(".ble_ll_bss")));
static u8 pEvent_buf_static[512] __attribute__((section(".ble_ll_bss")));
u8 ll_error_code __attribute__((section(".ble_ll_bss")));
static const u16 master_channel_map_request_steps[3] LL_CONST = {1, (u16)0x8000, 1535};
static const u16 slave_features_exchange_steps[3] LL_CONST = {14, 16393, 2047};
static const u16 master_features_exchange_steps[3] LL_CONST = {8, 16393, 1791};
static const u16 disconnect_steps[3] LL_CONST = {2, (u16)0x8000, 3839};
static const u16 slave_connection_parameter_request_steps[3] LL_CONST = {15, 16384, 255};
static const u16 master_connection_parameter_request_steps[5] LL_CONST = {15, 16400, 0, (u16)0x8000, 511};
static const u16 master_connection_update_steps[3] LL_CONST = {0, (u16)0x8000, 767};
static const u16 version_ind_steps[3] LL_CONST = {12, 16396, 2303};
static const u16 restart_encryption_steps[10] LL_CONST = {10, 16395, 11, (u16)0x8000, 3, 16388, 16389, 6, 16390, 2815};
static const u16 start_encryption_steps[6] LL_CONST = {3, 16388, 16389, 6, 16390, 2559};
static const u16 restart_encryption_req_steps[4] LL_CONST = {5, 16390, 6, 3327};
static const u16 start_encryption_req_steps[4] LL_CONST = {5, 16390, 6, 3071};
static const u16 slave_connection_parameter_respond_steps[3] LL_CONST = {16, 16384, 1023};
static const u16 data_length_update_steps[3] LL_CONST = {20, 16405, 4095};
static const u8 init_fixed_channel[5] LL_CONST = {255, 255, 255, 255, 31};
const struct ble_conn_param sample_conn_param LL_CONST = {
    .aa = {0x8e, 0x89, 0xbe, 0xd6}, .crcinit = {0xae, 0x83, 0x19},
    .winsize = 2, .winoffset = 0, .interval = 39, .latency = 0,
    .timeout = 50, .channel = {255, 255, 255, 255, 31}, .hop = 0,
    .sca = 0, .widening = 0, .widening_offset = 0, .afh_param = NULL,
};
static const u8 lctrComputeAccessAddr_upperSixBits[32] LL_CONST = {
    0x08,0x10,0x14,0x18,0x20,0x30,0x34,0x38,0x40,0x44,0x4c,0x5c,0x58,0x60,0x70,0x78,
    0xf4,0xec,0xe8,0xe4,0xdc,0xcc,0xc8,0xc4,0xbc,0xb8,0xb0,0xa0,0xa4,0x9c,0x8c,0x84,
};
static const u8 lctrComputeSeedAccessAddr_upperSixBits[32] LL_CONST = {
    0x82,0x86,0x8a,0x8e,0xa2,0xa6,0xaa,0xae,0xc2,0xc6,0xca,0xce,0xe2,0xe6,0xea,0xee,
    0x11,0x15,0x19,0x1d,0x31,0x35,0x39,0x3d,0x51,0x55,0x59,0x5d,0x71,0x75,0x79,0x7d,
};

static void LL_CODE ll_control_procedure_finish(struct le_link *, struct ble_rx *, int);
static void LL_CODE ll_conn_supervision_timer_handler(void *);
static void LL_CODE ll_response_timeout_handler(void *);
static void LL_CODE __master_ll_send_conn_update_done(struct le_link *);
static void __master_ll_send_conn_update_req_callback(void *);
static void __master_ll_send_channel_map_req_callback(void *);
static void __master_ll_send_enc_req_callback(void *);
static void LL_CODE __slave_ll_send_start_enc_req(void);
static void LL_CODE __slave_ll_send_start_enc_req_callback(void *);
static void atomic_add_return(atomic_t *);
static void atomic_sub_return(atomic_t *);
static void LL_CODE conn_rx_probe_handler(void *, struct ble_rx *);
static void LL_CODE conn_tx_probe_handler(void *, struct ble_tx *);
static void LL_CODE conn_evt_probe_handler(void *, int, int);
static void LL_CODE conn_echo_req_handler(void *, int, u8 *, u16);
static void LL_CODE ll_thread_process(struct thread *);

const struct ble_handler conn_handler LL_CONST = {
    .rx_probe_handler = conn_rx_probe_handler,
    .tws_rx_probe_handler = NULL,
    .tx_probe_handler = conn_tx_probe_handler,
    .event_probe_handler = conn_evt_probe_handler,
    .sync_param_handler = NULL,
    .echo_req_handler = conn_echo_req_handler,
};

const u16 master_sca[8] LL_CONST = {500, 250, 150, 100, 75, 50, 30, 20};

struct event_oneshot {
    struct list_head entry;
    void (*func)(struct le_link *);
    u32 instant;
};

struct rx_oneshot {
    struct list_head entry;
    void (*func)(struct le_link *);
};

#ifndef UNITTEST
_Static_assert(offsetof(struct le_link, entry) == 52, "SDK link list offset");
_Static_assert(sizeof(struct le_link) == 336, "SDK link allocation size");
#endif

static void cpu_reset(void)
{
    p33_soft_reset();
}

static void list_add_tail(struct list_head *entry, struct list_head *head)
{
    struct list_head *prev = head->prev;
    head->prev = entry;
    entry->next = head;
    entry->prev = prev;
    prev->next = entry;
}

static void INIT_LIST_HEAD(struct list_head *head)
{
    head->next = head;
    head->prev = head;
}

static void list_del(struct list_head *entry)
{
    struct list_head *prev = entry->prev;
    struct list_head *next = entry->next;
    next->prev = prev;
    prev->next = next;
    entry->next = entry;
    entry->prev = entry;
}

static int list_empty(const struct list_head *head)
{
    return head->next == head;
}

static void LL_CODE ll_supervision_timeout_stop(struct le_link *link)
{
    if (link->supervision_timeout) {
        sys_timer_remove(link->supervision_timeout);
        link->supervision_timeout = 0;
    }
}

static void ll_assert_or_reset(const char *what, int value)
{
    if (config_asser)
        btctrler_assert_failed(what, value);
    else
        cpu_reset();
}

u16 LL_CODE lctrCalcTotalAccuracy(u8 mstScaIdx)
{
    return master_sca[mstScaIdx < 7 ? mstScaIdx : 7];
}

void LL_CODE __event_oneshot_add(struct le_link *link,
                                 void (*func)(struct le_link *), int instant)
{
    struct event_oneshot *entry = __bt_malloc(16);
    if (!entry)
        ll_assert_or_reset("__event_oneshot_add: p != NULL", 0);
    local_irq_disable();
    if (entry) {
        entry->func = func;
        entry->instant = instant;
        list_add_tail(&entry->entry, &link->event_oneshot_head);
    }
    local_irq_enable();
}

void LL_CODE __rx_oneshot_add(struct le_link *link,
                              void (*func)(struct le_link *))
{
    struct rx_oneshot *entry = __bt_malloc(12);
    if (!entry)
        ll_assert_or_reset("__rx_oneshot_add: p != NULL", 0);
    local_irq_disable();
    if (entry) {
        entry->func = func;
        list_add_tail(&entry->entry, &link->rx_oneshot_head);
    }
    local_irq_enable();
}

void LL_CODE __event_oneshot_post_add(struct le_link *link,
                                      void (*func)(struct le_link *), int instant)
{
    struct event_oneshot *entry = __bt_malloc(16);
    if (!entry)
        ll_assert_or_reset("__event_oneshot_post_add: p != NULL", 0);
    local_irq_disable();
    if (entry) {
        entry->func = func;
        entry->instant = instant;
        list_add_tail(&entry->entry, &link->event_oneshot_post_head);
    }
    local_irq_enable();
}

u8 LL_CODE ll_control_procedure_is_exist_ir(struct le_link *link, u32 procedure_bits)
{
    return (link->current_procedure & procedure_bits) != 0;
}

u8 LL_CODE __hci_emit_le_meta_event_ir(u8 subevent_code, const char *format, ...)
{
    if (subevent_code < 0xf0) {
        bool enabled = (hci_param.event_mask[7] & 32) != 0;
        if (enabled) {
            int sub = (int)subevent_code - 1;
            enabled = (le_param.event_mask[sub / 8] & (1 << (sub & 7))) != 0;
        }
        if (!enabled) {
            if (log_tag_const_e_LL)
                log_print(4, NULL, "<Error>: [LL]LE event mask disable\r\n");
            return false;
        }
    }

    va_list ap;
    va_start(ap, format);
    int format_len = __vsprintf_len(format, ap);
    va_end(ap);
    int len = format_len + 1;
    struct le_event *event = lbuf_alloc_btctrler(
        (struct lbuff_head_btctrler *)ll_hdl.pEVT, (u32)(format_len + 3));
    if (!event) {
        ll_assert_or_reset("event != NULL %d, %s", len);
        ll_assert_or_reset("event != NULL", 0);
    } else {
        event->len = (u8)len;
    }
    event->event = 62;
    event->data[0] = subevent_code;
    va_start(ap, format);
    (void)__vsprintf((u8 *)&event->data[1], format, ap);
    va_end(ap);
    lbuf_push_btctrler(&event->event);
    thread_resume(&ll_hdl.thread);
    return true;
}

u8 LL_CODE ll_control_procedure_push_ir(struct le_link *link, u8 procedure)
{
    if (log_tag_const_i_LL)
        log_print(2, NULL, "[Info]: [LL]>push %d\r\n", procedure);
    if (link->current_procedure != 0) {
        if (log_tag_const_e_LL)
            log_print(4, NULL, "<Error>: [LL]conflict...%02x\r\n", link->current_procedure);
        return false;
    }
    link->current_procedure = 1U << procedure;
    return true;
}

void LL_CODE ll_control_procedure_pop(struct le_link *link, u8 procedure)
{
    u32 bit = 1U << procedure;
    if (link->current_procedure & bit) {
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]<pop %d\r\n", procedure);
        link->current_procedure &= ~bit;
    }
}

void LL_CODE ll_response_timeout_stop(struct le_link *link)
{
    if (link->response_timeout) {
        sys_timer_remove(link->response_timeout);
        link->response_timeout = 0;
    }
}

static int LL_CODE __attribute__((noinline)) ll_control_procedure_case_remap(u16 error_case)
{
    if (error_case == 0)
        return 0;
    if (error_case == 16391)
        return 3;
    if (error_case == 16401)
        return 2;
    return 1;
}

static void LL_CODE ll_control_procedure_finish(struct le_link *link, struct ble_rx *rx, int status)
{
    u8 rd = link->fifo_rd;
    u16 *steps = (u16 *)link->procedure_fifo[rd].steps_ptr;
    u8 counter = link->procedure_fifo[rd].counter;
    u16 step = steps[counter];
    if ((step & 255) != 255) {
        do {
            counter++;
            step = steps[counter];
        } while ((step & 255) != 255);
        (__ble_ops->ioctrl)(link->hw, 17, 0);
        (__ble_ops->ioctrl)(link->hw, 18, 0);
    }

    switch ((u8)(step >> 8)) {
    case 0:
        if (config_btctler_le_features & 2)
            ll_control_procedure_pop(link, 15);
        le_connection_update_complete_event(link, rx, status);
        break;
    case 1:
        le_connection_update_complete_event(link, rx, status);
        break;
    case 2: {
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]MASTER_CONNECTION_UPDATE_STEPS\r\n");
        u16 instant = link->instant;
        (__ble_ops->ioctrl)(link->hw, 4, instant, 0);
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]----update----  :%d\r\n", instant);
        __event_oneshot_add(link, __connection_upadte, instant - 1);
        __event_oneshot_post_add(link, __master_ll_send_conn_update_done, instant);
        break;
    }
    case 5: {
        u16 instant = link->instant;
        (__ble_ops->ioctrl)(link->hw, 4, instant, 1);
        __event_oneshot_add(link, __channel_map_upadte, instant - 1);
        break;
    }
    case 6:
    case 7:
        le_read_remote_used_features_complete_event(link, rx, status);
        break;
    case 8:
        hci_read_remote_version_information_complete(link, rx, status);
        break;
    case 9:
        if ((config_btctler_le_features & 1) == 0)
            return;
        {
            (__ble_ops->ioctrl)(link->hw, 17, 0);
            hci_event_encryption_change(link, rx, (u8)status);
        }
        break;
    case 10:
        if ((config_btctler_le_features & 1) == 0)
            return;
        {
            (__ble_ops->ioctrl)(link->hw, 17, 0);
            hci_event_encryption_refresh_complete(link, rx, (u8)status);
        }
        break;
    case 11:
        if ((config_btctler_le_features & 1) == 0)
            return;
        {
            (__ble_ops->ioctrl)(link->hw, 17, 0);
            hci_event_encryption_change(link, rx, (u8)status);
        }
        break;
    case 12:
        if ((config_btctler_le_features & 1) == 0)
            return;
        {
            (__ble_ops->ioctrl)(link->hw, 17, 0);
            hci_event_encryption_refresh_complete(link, rx, (u8)status);
        }
        break;
    case 14:
        le_disconnect_complete_event(link, status);
        break;
    case 17:
    case 18:
        __master_ll_phy_update_done(link, rx);
        break;
    case 19:
    case 20:
        __slave_ll_phy_update_done(link, rx);
        break;
    case 16:
        __slave_vendor_wifi_detect_done(link);
        break;
    default:
        break;
    }

    link->procedure_fifo[rd].steps_ptr = NULL;
    rd = link->fifo_rd;
    rd = rd == 4 ? 0 : (u8)(rd + 1);
    link->fifo_rd = rd;
    if (log_tag_const_i_LL)
        log_print(2, NULL, "[Info]: [LL]procedure end : %d - %d\r\n", link->fifo_wr, rd);
}

void LL_CODE ll_control_step_verify(u16 step_ex, struct le_link *link, struct ble_rx *rx)
{
    u8 rd = link->fifo_rd;
    u16 *steps = (u16 *)link->procedure_fifo[rd].steps_ptr;
    if (!steps)
        return;

    if (step_ex == 16397) {
        ll_response_timeout_stop(link);
        ll_control_procedure_finish(link, rx, ll_control_procedure_case_remap(16397));
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]##Procedure break 1: 0x%x\r\n", step_ex);
        return;
    }

    u8 counter = link->procedure_fifo[rd].counter;
    u16 current = steps[counter];
    if (step_ex == 16401 || step_ex == 16391) {
        u8 byte = rx->data[1];
        counter--;
        link->procedure_fifo[rd].counter = counter;
        current = steps[counter];
        if (byte == (u8)current) {
            ll_response_timeout_stop(link);
            ll_control_procedure_finish(link, rx, ll_control_procedure_case_remap(step_ex));
            if (log_tag_const_i_LL)
                log_print(2, NULL, "[Info]: [LL]##Procedure break 2: 0x%x 0x%x\r\n", byte, byte);
            return;
        }
    }

    if (current == step_ex) {
        counter++;
        link->procedure_fifo[rd].counter = counter;
        if ((steps[counter] & 255) == 255) {
            ll_response_timeout_stop(link);
            ll_control_procedure_finish(link, rx, 0);
            if (log_tag_const_i_LL)
                log_print(2, NULL, "[Info]: [LL]##Procedure Finish\r\n");
        }
    }
}

void LL_CODE ll_send_control_data_state_machine(struct le_link *link, struct ble_rx *rx)
{
    u8 rd = link->fifo_rd;
    u16 *steps = (u16 *)link->procedure_fifo[rd].steps_ptr;
    if (!steps)
        return;
    u16 word = steps[link->procedure_fifo[rd].counter];
    if (word >= 16384)
        return;
    u8 opcode = (u8)word;

    switch (opcode) {
    case 0: { /* LL_CONNECTION_UPDATE_REQ */
        if ((config_btctler_le_roles & 1) == 0)
            break;
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_CONNECTION_UPDATE_REQ\r\n");
        struct le_link *target = ll_link_for_handle(le_param.conn_update_param.connection_handle);
        if (!target)
            break;
        u16 latency = target->ll_data.latency;
        u32 delay;
        if (latency) {
            delay = (u32)latency * 6 + 6;
        } else {
            u32 timing = *(volatile u32 *)0x1e3b00;
            delay = (timing & 3) | 8;
        }
        u16 event_count = target->event_cnt;
        u32 instant = (u32)event_count + delay;
        bool vendor = config_vendor_le_bb != 0;
        if (vendor)
            instant += 7;
        u16 instant16 = (u16)instant;
        u16 interval, winoffset;
        u8 winsize;
        u16 new_latency, timeout;
        if ((s16)target->current_procedure < 0) {
            u16 old_interval = target->ll_data.interval;
            u16 min = target->conn_param_req.interval_min;
            u16 max = target->conn_param_req.interval_max;
            if (max == min) {
                interval = max;
            } else {
                unsigned i;
                for (i = 1; i != 255; i++) {
                    unsigned candidate = i * target->conn_param_req.PreferredPeriodicity;
                    if (candidate >= min && candidate <= max)
                        break;
                }
                interval = (u16)(i == 255 ? old_interval : i * target->conn_param_req.PreferredPeriodicity);
            }
            target->ll_data.interval = interval;
            u16 reference = target->conn_param_req.ReferenceConnEventCount;
            u16 offset = target->conn_param_req.offset[0];
            s32 phase;
            if (reference > instant16)
                phase = (((s32)reference - instant16) * old_interval + offset) % interval;
            else
                phase = interval - ((((s32)instant16 - reference) * old_interval - offset) % interval);
            if (((u32)phase & 65532) > 3) {
                winoffset = (u16)(phase - 1);
                winsize = 2;
            } else {
                winoffset = 0;
                winsize = 4;
            }
            target->ll_data.winoffset = winoffset;
            target->ll_data.winsize = winsize;
            new_latency = target->conn_param_req.latency;
            target->ll_data.latency = new_latency;
            timeout = target->conn_param_req.timeout;
            target->ll_data.timeout = timeout;
        } else {
            new_latency = le_param.conn_update_param.Conn_Latency;
            target->ll_data.latency = new_latency;
            interval = le_param.conn_update_param.Conn_Interval_Min;
            target->ll_data.interval = interval;
            timeout = le_param.conn_update_param.Supervision_Timeout;
            target->ll_data.timeout = timeout;
            winsize = 1;
            target->ll_data.winsize = winsize;
            winoffset = interval >> 1;
            target->ll_data.winoffset = winoffset;
        }
        target->instant = instant16;

        if (config_btctler_le_master_multilink) {
            if (target->echo_req_type == 2) {
                winoffset = (u16)target->echo_req_param;
                target->ll_data.winoffset = winoffset;
                target->echo_req_type = 0;
                if (winsize < 3) {
                    winsize = 3;
                    target->ll_data.winsize = 3;
                }
            } else {
                target->ll_data.winoffset = 0;
                winoffset = 0;
            }
        }
        if (vendor) {
            winsize = 1;
            target->ll_data.winsize = 1;
        }
        ll_send_control_data_callback(target, __master_ll_send_conn_update_req_callback,
                                      0, "122222", winsize, winoffset, interval,
                                      new_latency, timeout, instant16);
        break;
    }
    case 1: { /* LL_CHANNEL_MAP_REQ */
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_CHANNEL_MAP_REQ\r\n");
        if ((config_btctler_le_roles & 1) == 0)
            break;
        u16 latency = link->ll_data.latency;
        u32 instant = link->event_cnt + 6 + (latency ? latency * 6 : (*(volatile u32 *)0x1e3b00 & 3));
        if (config_vendor_le_bb)
            instant += 10;
        link->instant = (u16)instant;
        ll_send_control_data_callback(link, __master_ll_send_channel_map_req_callback,
                                      1, "c052", link->ll_data.channel, (u16)instant);
        break;
    }
    case 2: {
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_TERMINATE_IND\r\n");
        struct le_link *target = ll_link_for_handle(hci_param.disconn_param.connection_handle);
        if (!target) {
            if (log_tag_const_e_LL)
                log_print(4, NULL, "<Error>: [LL]No link\r\n");
            break;
        }
        ll_send_control_data(target, 2, "1", hci_param.disconn_param.reason);
        break;
    }
    case 3: { /* LL_ENC_REQ */
        if ((config_btctler_le_features & 1) == 0)
            break;
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_ENC_REQ\r\n");
        if ((config_btctler_le_roles & 1) == 0)
            break;
        struct le_link *target = ll_link_for_handle(le_param.start_encryption_param.connection_handle);
        if (!target) {
            if (log_tag_const_e_LL)
                log_print(4, NULL, "<Error>: [LL]No link\r\n");
            break;
        }
        bool slave = target->role == 1;
        if (config_asser && slave)
            btctrler_assert_failed("!ROLE_IS_SLAVE(link) __master_ll_send_enc_req", 0);
        else if (!config_asser && slave)
            cpu_reset();
        u32 ivm;
        u64 skdm;
        pseudo_random_genrate((u8 *)&ivm, 4);
        memcpy(target->ivm, &ivm, 4);
        pseudo_random_genrate((u8 *)&skdm, 8);
        memcpy(target->skdm, &skdm, 8);
        memcpy(target->long_term_key, le_param.start_encryption_param.long_term_key, 16);
        ll_send_control_data_callback(target, __master_ll_send_enc_req_callback, 3,
                                      "c08c02c08c04",
                                      le_param.start_encryption_param.random_number,
                                      le_param.start_encryption_param.encrypted_diversifier,
                                      target->skdm, target->ivm);
        break;
    }
    case 4:
        if ((config_btctler_le_features & 1) && log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_ENC_RSP\r\n");
        break;
    case 5:
        if (config_btctler_le_features & 1) {
            if (log_tag_const_i_LL)
                log_print(2, NULL, "[Info]: [LL]LL_START_ENC_REQ\r\n");
            __slave_ll_send_start_enc_req();
        }
        break;
    case 6:
        if ((config_btctler_le_features & 1) && log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_START_ENC_RSP\r\n");
        break;
    case 14:
    case 8: {
        if (opcode == 14 && (config_btctler_le_features & 8) == 0)
            break;
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_FEATURE_REQ\r\n");
        struct le_link *target = ll_link_for_handle(le_param.read_remote_used_features_param.connection_handle);
        if (!target)
            break;
        if (target->role == 1)
            ll_send_control_data(target, 14, "c08", le_param.features);
        else
            ll_send_control_data(target, 8, "c08", le_param.features);
        break;
    }
    case 9:
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_FEATURE_RSP\r\n");
        break;
    case 10: { /* LL_PAUSE_ENC_REQ */
        if ((config_btctler_le_features & 1) == 0)
            break;
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_PAUSE_ENC_REQ\r\n");
        if ((config_btctler_le_roles & 1) == 0)
            break;
        struct le_link *target = ll_link_for_handle(le_param.start_encryption_param.connection_handle);
        if (!target) {
            if (log_tag_const_e_LL)
                log_print(4, NULL, "<Error>: [LL]No link\r\n");
            break;
        }
        bool slave = target->role == 1;
        if (config_asser && slave)
            btctrler_assert_failed("!ROLE_IS_SLAVE(link) __master_ll_send_pause_enc_req", 0);
        else if (!config_asser && slave)
            cpu_reset();
        ll_send_control_data(target, 10, NULL);
        break;
    }
    case 11:
        if ((config_btctler_le_features & 1) && log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_PAUSE_ENC_RSP\r\n");
        break;
    case 12: {
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_VERSION_IND\r\n");
        struct le_link *target = ll_link_for_handle(hci_param.read_remote_version_param.connection_handle);
        if (!target) {
            if (log_tag_const_e_LL)
                log_print(4, NULL, "<Error>: [LL]No link\r\n");
            break;
        }
        ll_send_control_data(target, 12, "122", le_param.versnr,
                             le_param.compld, le_param.subversnr);
        break;
    }
    case 13:
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_REJECT_IND\r\n");
        break;
    case 15: { /* LL_CONNECTION_PARAM_REQ */
        if ((config_btctler_le_features & 2) == 0)
            break;
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_CONNECTION_PARAM_REQ\r\n");
        u16 min = le_param.conn_update_param.Conn_Interval_Min;
        u16 max = le_param.conn_update_param.Conn_Interval_Max;
        u16 latency = le_param.conn_update_param.Conn_Latency;
        u16 timeout = le_param.conn_update_param.Supervision_Timeout;
        if ((u32)timeout * 10 < ((u32)max << 1) * ((u32)latency + 1))
            ll_assert_or_reset("0 Conn Param Request err TimeOut in __ll_send_conn_param_req", 0);
        struct le_link *target = ll_link_for_handle(le_param.conn_update_param.connection_handle);
        if (!target)
            break;
        min = le_param.conn_update_param.Conn_Interval_Min;
        max = le_param.conn_update_param.Conn_Interval_Max;
        latency = le_param.conn_update_param.Conn_Latency;
        timeout = le_param.conn_update_param.Supervision_Timeout;
        target->conn_param_req.interval_min = min;
        target->conn_param_req.interval_max = max;
        target->conn_param_req.latency = latency;
        target->conn_param_req.timeout = timeout;
        u8 periodicity = min == max ? 0 : (u8)(max >> 1);
        target->conn_param_req.PreferredPeriodicity = periodicity;
        u16 reference = (u16)(ll_vendor_instant_cnt + target->event_cnt);
        target->conn_param_req.ReferenceConnEventCount = reference;
        memset(target->conn_param_req.offset, 0xff, 12);
        u16 offset0 = ll_vendor_offset0;
        target->conn_param_req.offset[0] = offset0;
        ll_send_control_data(target, 15, "222212222222", min, max, latency, timeout,
                             periodicity, reference, offset0, 65535, 65535, 65535, 65535, 65535);
        break;
    }
    case 16: { /* LL_CONNECTION_PARAM_RSP */
        if ((config_btctler_le_features & 2) == 0)
            break;
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_CONNECTION_PARAM_RSP\r\n");
        struct le_link *target = ll_link_for_handle(le_param.conn_update_param.connection_handle);
        if (!target)
            break;
        ll_send_control_data(target, 16, "222212222222",
                             le_param.conn_update_param.Conn_Interval_Min,
                             le_param.conn_update_param.Conn_Interval_Max,
                             le_param.conn_update_param.Conn_Latency,
                             le_param.conn_update_param.Supervision_Timeout,
                             target->conn_param_req.PreferredPeriodicity,
                             target->conn_param_req.ReferenceConnEventCount,
                             target->conn_param_req.offset[0], target->conn_param_req.offset[1],
                             target->conn_param_req.offset[2], target->conn_param_req.offset[3],
                             target->conn_param_req.offset[4], target->conn_param_req.offset[5]);
        break;
    }
    case 17:
        if ((config_btctler_le_features & 4) && log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_REJECT_IND_EXT\r\n");
        break;
    case 18:
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_PING_REQ\r\n");
        break;
    case 19:
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_PING_RSP\r\n");
        break;
    case 20: { /* LL_LENGTH_REQ */
        if ((config_btctler_le_features & 32) == 0)
            break;
        bool log = log_tag_const_i_LL != 0;
        if (log)
            log_print(2, NULL, "[Info]: [LL]LL_LENGTH_REQ\r\n");
        struct le_link *target = ll_link_for_handle(le_param.set_data_length_param.connection_handle);
        if (!target)
            break;
        u16 tx_octets = le_param.set_data_length_param.txoctets;
        u16 tx_time = le_param.set_data_length_param.txtime;
        target->pdu_len.connMaxTxOctets = tx_octets;
        target->pdu_len.connMaxTxTime = tx_time;
        u16 rx_octets = target->pdu_len.connMaxRxOctets;
        u16 rx_time = target->pdu_len.connMaxRxTime;
        ll_send_control_data(target, 20, "2222", rx_octets, rx_time, tx_octets, tx_time);
        if (log)
            log_print(2, NULL, "[Info]: [LL]send DLE req, rx:%d,%d, tx:%d,%d\n\r\n",
                      target->pdu_len.connMaxRxOctets, target->pdu_len.connMaxRxTime,
                      le_param.set_data_length_param.txoctets,
                      le_param.set_data_length_param.txtime);
        break;
    }
    case 22:
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]Tx - LL_PHY_REQ\r\n");
        __ll_send_phy_req(link);
        break;
    case 23:
        break;
    case 24:
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]Master Tx - LL_PHY_UPDATE_IND\r\n");
        __master_ll_send_phy_update_ind(link);
        break;
    case 72: {
        struct le_link *target = ll_link_for_handle(1);
        if (!target) {
            puts("get link err");
            break;
        }
        ll_send_control_data(target, 72, "111", get_jl_chip_id(), 1, 0);
        break;
    }
    case 81:
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]Slave Tx - LL_VENDOR_WIFI_DETECT_REQ\r\n");
        __slave_ll_send_vendor_wifi_detect_req(link);
        break;
    case 7:
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]LL_UNKNOWN_RSP\r\n");
        break;
    default:
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]Unknow-opcode: %x\r\n", opcode);
        break;
    }
    ll_control_step_verify(opcode, link, rx);
}

u8 LL_CODE ll_current_step(struct le_link *link)
{
    u8 rd = link->fifo_rd;
    return (u8)link->procedure_fifo[rd].steps_ptr[link->procedure_fifo[rd].counter];
}

u8 LL_CODE ll_current_no_step(struct le_link *link)
{
    return link->procedure_fifo[link->fifo_rd].steps_ptr == NULL;
}

void LL_CODE ll_control_data_step_start(struct le_link *link, const ll_step_extend *steps)
{
    u8 wr = link->fifo_wr;
    link->procedure_fifo[wr].steps_ptr = steps;
    link->procedure_fifo[wr].counter = 0;
    wr = wr == 4 ? 0 : (u8)(wr + 1);
    link->fifo_wr = wr;
    if (log_tag_const_i_LL)
        log_print(2, NULL, "[Info]: [LL]procedure start: %d - %d\r\n", wr, link->fifo_rd);
    ll_send_control_data_state_machine(link, NULL);
}

void LL_CODE ll_send_control_data(struct le_link *link, int opcode,
                                  const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int len = __vsprintf_len(format, ap);
    va_end(ap);
    if ((u32)len >= sizeof(link->send_buf))
        ll_assert_or_reset("__vsprintf_len(format, argptr) < sizeof(link->send_buf)", 0);

    link->send_buf[0] = (u8)opcode;
    int out_len = 1;
    if (format) {
        va_start(ap, format);
        out_len = __vsprintf(&link->send_buf[1], format, ap) + 1;
        va_end(ap);
    }
    (__ble_ops->send_packet_high)(link->hw, NULL, NULL, 3, link->send_buf, out_len);
}

void LL_CODE ll_send_control_data_callback(struct le_link *link,
                                           void (*callback)(void *), int opcode,
                                           const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int len = __vsprintf_len(format, ap);
    va_end(ap);
    if ((u32)len >= sizeof(link->send_buf))
        ll_assert_or_reset("__vsprintf_len(format, argptr) < sizeof(link->send_buf)", 0);

    link->send_buf[0] = (u8)opcode;
    int out_len = 1;
    if (format) {
        va_start(ap, format);
        out_len = __vsprintf(&link->send_buf[1], format, ap) + 1;
        va_end(ap);
    }
    (__ble_ops->send_packet_high)(link->hw, callback, NULL, 3, link->send_buf, out_len);
}

void LL_CODE ll_send_control_data_callback_extend(struct le_link *link,
                                                  void (*send_callback)(void *),
                                                  void (*ack_callback)(void *), int opcode,
                                                  const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int len = __vsprintf_len(format, ap);
    va_end(ap);
    if ((u32)len >= sizeof(link->send_buf))
        ll_assert_or_reset("__vsprintf_len(format, argptr) < sizeof(link->send_buf)", 0);

    link->send_buf[0] = (u8)opcode;
    int out_len = 1;
    if (format) {
        va_start(ap, format);
        out_len = __vsprintf(&link->send_buf[1], format, ap) + 1;
        va_end(ap);
    }
    (__ble_ops->send_packet_high)(link->hw, send_callback, ack_callback, 3,
                                  link->send_buf, out_len);
}

void LL_CODE ll_send_acl_packet(int handle, const u8 *packet,
                                int acl_fragmentation_total_size)
{
    os_mutex_pend(&ll_hdl.link_lock, 0);
    struct le_link *link = ll_link_for_handle(handle);
    if (!link) {
        if (log_tag_const_e_LL)
            log_print(4, NULL, "<Error>: [LL]TX link NULL\r\n");
        os_mutex_post(&ll_hdl.link_lock);
        return;
    }

    u8 flags = packet[1];
    const u8 *data = packet + 4;
    struct acl_fragment_packet *acl_packet = __bt_malloc(sizeof(*acl_packet));
    if (!acl_packet)
        ll_assert_or_reset("acl_packet != NULL", 0);
    acl_packet->num_acl_packets_sent = 0;

    u16 max_tx_octets = link->pdu_len.connEffectiveMaxTxOctets;
    if (max_tx_octets != link->pdu_len.PreConnEffectiveTxOctets) {
        while (!list_empty(&link->acl_packet_head))
            os_time_dly(1);
        link->pdu_len.PreConnEffectiveTxOctets = max_tx_octets;
    }

    int remaining = acl_fragmentation_total_size;
    for (;;) {
        int amount = remaining > max_tx_octets ? max_tx_octets : remaining;
        u8 sent_amount = (u8)amount;
        u8 count = acl_packet->num_acl_packets_sent;
        if ((flags & 16) == 0 && count == 0)
            (__ble_ops->send_packet)(link->hw, 2, data, sent_amount, max_tx_octets);
        else
            (__ble_ops->send_packet)(link->hw, 1, data, sent_amount, max_tx_octets);
        data += sent_amount;
        remaining -= sent_amount;
        acl_packet->num_acl_packets_sent++;
        if (remaining == 0)
            break;
    }
    local_irq_disable();
    list_add_tail(&acl_packet->entry, &link->acl_packet_head);
    local_irq_enable();
    os_mutex_post(&ll_hdl.link_lock);
}

u8 LL_CODE device_addr_verify_ir(const u8 *addr, u8 start, u8 len)
{
    u8 temp[6];
    memcpy(temp, addr, 6);
    unsigned end = (unsigned)start + len;
    u8 all_ones = 255;
    for (u8 i = start; (unsigned)i < end; i++) {
        u8 byte = temp[i];
        if (i == 5) {
            byte |= 0xc0;
            temp[i] = byte;
        }
        all_ones &= byte;
    }
    if (all_ones == 255) {
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]addr verify fail all 1!\r\n");
        return false;
    }

    u8 any_one = 0;
    for (u8 i = start; (unsigned)i < end; i++) {
        u8 byte = temp[i];
        if (i == 5) {
            byte &= 0x3f;
            temp[i] = byte;
        }
        any_one |= byte;
    }
    if (any_one == 0) {
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]addr verify fail all 0!\r\n");
        return false;
    }
    return true;
}

CONN_PARAM_RES LL_CODE __ll_receive_connection_param_req(struct le_link *link,
                                                        struct ble_rx *rx)
{
    const u8 *data = &rx->data[1];
    link->conn_param_req.interval_min = btctler_little_endian_read_16(data, 0);
    link->conn_param_req.interval_max = btctler_little_endian_read_16(data, 2);
    link->conn_param_req.latency = btctler_little_endian_read_16(data, 4);
    link->conn_param_req.timeout = btctler_little_endian_read_16(data, 6);
    link->conn_param_req.PreferredPeriodicity = rx->data[9];
    link->conn_param_req.ReferenceConnEventCount = btctler_little_endian_read_16(data, 9);
    for (int i = 0; i != 6; i++)
        link->conn_param_req.offset[i] = btctler_little_endian_read_16(data, (i << 1) + 11);
    return hook_ll_receive_connection_param_req_ir(link->handle, data) ? 0 : 2;
}

static void LL_CODE __set_conn_winsize(struct le_link *link)
{
    if (link->role == 1)
        (__ble_ops->ioctrl)(link->hw, 5, 50, (link->ll_data.widening << 1) + 50);
    else
        (__ble_ops->ioctrl)(link->hw, 5, 30, 0);
    __reset_CodedPHY_conn_winsize(link);
}

void LL_CODE __connection_upadte(struct le_link *link)
{
    (__ble_ops->ioctrl)(link->hw, 7, &link->ll_data);
    (__ble_ops->ioctrl)(link->hw, 18, 0);
    if ((config_btctler_le_features & 2) == 0)
        ll_control_procedure_pop(link, 0);
    else
        ll_control_procedure_pop(link, 15);
    __rx_oneshot_add(link, __set_conn_winsize);
    if (log_tag_const_i_LL)
        log_print(2, NULL, "[Info]: [LL]----update---- : \r\n");
    ll_supervision_timeout_changeperiod(link, link->ll_data.timeout * 10);
    if (log_tag_const_i_LL)
        log_print(2, NULL, "[Info]: [LL]>Done %d\r\n", link->event_cnt);
}

void LL_CODE __channel_map_upadte(struct le_link *link)
{
    (__ble_ops->ioctrl)(link->hw, 8, link->ll_data.channel);
    if (link->role != 1) {
        ll_control_procedure_pop(link, 1);
        (__ble_ops->ioctrl)(link->hw, 18, 0);
        ll_update_ch_done_afh_callback(link);
        ll_update_ch_done_wfdet_callback(link);
    }
}

u8 LL_CODE le_channel_map_update(const struct host_channel_classification *param)
{
    u8 status = 2;
    for (struct list_head *node = ll_hdl.le_links.next;
         node != &ll_hdl.le_links; node = node->next) {
        struct le_link *link = (struct le_link *)((u8 *)node - offsetof(struct le_link, entry));
        if (link->state == 7 && link->role != 1) {
            memcpy(link->ll_data.channel, param->channel_map, 5);
            ll_control_data_step_start(link, master_channel_map_request_steps);
            status = 0;
        }
    }
    return status;
}

void LL_CODE le_channel_map_update_from_controller(struct le_link *link)
{
    u32 current = link->current_procedure;
    if (current & 32771) {
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]map_req conflict,%04x\n\r\n", current);
        return;
    }
    if (ll_control_procedure_push_ir(link, 1))
        ll_control_data_step_start(link, master_channel_map_request_steps);
}

void LL_CODE ll_remote_feature(struct le_link *link)
{
    (void)link;
}

u8 LL_CODE le_features_exchange(const struct read_remote_used_features_paramter *param)
{
    u16 handle = param->connection_handle;
    struct le_link *link = ll_link_for_handle(handle);
    if (!link) {
        if (log_tag_const_e_LL)
            log_print(4, NULL, "<Error>: [LL]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
        return 2;
    }
    le_param.read_remote_used_features_param.connection_handle = handle;
    if (link->role == 1) {
        if (config_btctler_le_features & 8) {
            ll_control_data_step_start(link, slave_features_exchange_steps);
            return 0;
        }
        ll_assert_or_reset("0 le_features_exchange", 0);
        return 0;
    }
    ll_control_data_step_start(link, master_features_exchange_steps);
    return 0;
}

u8 LL_CODE le_disconnect(const struct disconnect_paramter *param)
{
    struct le_link *link = ll_link_for_handle(param->connection_handle);
    if (!link) {
        if (log_tag_const_e_LL)
            log_print(4, NULL, "<Error>: [LL]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
        return 2;
    }
    u8 state = link->state;
    if ((state & 0xfe) == 6) {
        ll_response_timeout_start(link);
        memcpy(&hci_param.disconn_param, param, 3);
        ll_control_data_step_start(link, disconnect_steps);
        return 0;
    }
    if (log_tag_const_e_LL)
        log_print(4, NULL, "<Error>: [LL]state:%d\r\n", state);
    return 0;
}

void LL_CODE __ll_send_cte_req(struct le_link *link,
                               struct set_cte_req_parameter *param)
{
    if ((config_btctler_le_features & 131072) == 0)
        return;
    u8 value = (u8)((param->MinCTELenReq & 31) | (param->CTETypeReq << 6));
    if (log_tag_const_i_LL)
        log_print(2, NULL, "[Info]: [LL]__ll_send_cte_req:%x %x\n\r\n",
                  value & 31, value >> 6);
    ll_send_control_data(link, 26, "1", value);
}

void LL_CODE __ll_send_cte_rsp(struct le_link *link,
                               struct set_cte_req_parameter *param)
{
    (void)param;
    if ((config_btctler_le_features & 131072) == 0)
        return;
    if (log_tag_const_i_LL)
        log_print(2, NULL, "[Info]: [LL]__ll_send_cte_rsp\n\r\n");
    ll_send_control_data(link, 27, NULL);
}

void LL_CODE __ll_send_length_rsp_auto(struct le_link *link)
{
    u16 rx_octets = link->pdu_len.connMaxRxOctets;
    u16 rx_time = link->pdu_len.connMaxRxTime;
    u16 tx_octets = link->pdu_len.connMaxTxOctets;
    u16 tx_time = link->pdu_len.connMaxTxTime;
    ll_send_control_data(link, 21, "2222", rx_octets, rx_time, tx_octets, tx_time);
    if (log_tag_const_i_LL)
        log_print(2, NULL, "[Info]: [LL]send DLE rsp, rx:%d,%d, tx:%d,%d\n\r\n",
                  link->pdu_len.connMaxRxOctets, link->pdu_len.connMaxRxTime,
                  link->pdu_len.connMaxTxOctets, link->pdu_len.connMaxTxTime);
}

void LL_CODE __ll_receive_length_rsp(struct le_link *link, struct ble_rx *rx)
{
    const u8 *data = &rx->data[1];
    link->pdu_len.connRemoteMaxRxOctets = btctler_little_endian_read_16(data, 0);
    link->pdu_len.connRemoteMaxRxTime = btctler_little_endian_read_16(data, 2);
    link->pdu_len.connRemoteMaxTxOctets = btctler_little_endian_read_16(data, 4);
    link->pdu_len.connRemoteMaxTxTime = btctler_little_endian_read_16(data, 6);
    if (log_tag_const_i_LL)
        log_print(2, NULL, "[Info]: [LL]recieve DLE rsp, rx:%d,%d, tx:%d,%d\n\r\n",
                  link->pdu_len.connRemoteMaxRxOctets, link->pdu_len.connRemoteMaxRxTime,
                  link->pdu_len.connRemoteMaxTxOctets, link->pdu_len.connRemoteMaxTxTime);
    __set_ll_effective_data_args(link);
}

u8 LL_CODE ll_push_control_data(u16 opcode, const u8 *param)
{
    u16 normalized = opcode & 1023;
    if ((opcode & 0xfc00) != 8192)
        normalized = opcode;
    if ((normalized & 0xfc00) == 0xf800)
        normalized &= 1023;

    switch (normalized) {
    case 19: { /* LL_CONNECTION_UPDATE */
        u16 handle;
        memcpy(&handle, param, sizeof(handle));
        struct le_link *link = ll_link_for_handle(handle);
        if (!link) {
            if (log_tag_const_e_LL)
                log_print(4, NULL, "<Error>: [LL]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
            return 2;
        }
        if ((link->state & 0xfe) != 6)
            return 12;
        memcpy(&le_param.conn_update_param, param, 14);
        if (config_btctler_le_features & 2) {
            if (link->role == 1) {
                if (ll_control_procedure_push_ir(link, 15))
                    ll_control_data_step_start(link, slave_connection_parameter_request_steps);
                return 0;
            }
            u32 current = link->current_procedure;
            if (current & 32771) {
                if (log_tag_const_e_LL)
                    log_print(4, NULL, "<Error>: [LL]update_param_req conflict,%04x\n\r\n", current);
                return 0;
            }
            if (ll_control_procedure_push_ir(link, 15))
                ll_control_data_step_start(link, master_connection_parameter_request_steps);
            return 0;
        }
        if (link->role == 1) {
            ll_assert_or_reset("0 le_connection_update", 0);
            return 0;
        }
        u32 current = link->current_procedure;
        if (current & 32771) {
            if (log_tag_const_e_LL)
                log_print(4, NULL, "<Error>: [LL]update_req conflict,%04x\n\r\n", current);
            return 0;
        }
        if (ll_control_procedure_push_ir(link, 0))
            ll_control_data_step_start(link, master_connection_update_steps);
        return 0;
    }
    case 20: /* LL_CHANNEL_MAP update */
        if ((config_btctler_le_roles & 1) == 0)
            return 1;
        memcpy(ll_hdl.fixed_channel_map, param, 5);
        return le_channel_map_update((const struct host_channel_classification *)param);
    case 22:
        return le_features_exchange((const struct read_remote_used_features_paramter *)param);
    case 1053: { /* HCI read remote version */
        u16 handle;
        memcpy(&handle, param, sizeof(handle));
        struct le_link *link = ll_link_for_handle(handle);
        if (!link) {
            if (log_tag_const_e_LL)
                log_print(4, NULL, "<Error>: [LL]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
            return 2;
        }
        ll_response_timeout_start(link);
        hci_param.read_remote_version_param.connection_handle = handle;
        ll_control_data_step_start(link, version_ind_steps);
        return 0;
    }
    case 25: { /* LL_ENC_REQ */
        if ((config_btctler_le_features & 1) == 0)
            return 1;
        u16 handle;
        memcpy(&handle, param, sizeof(handle));
        struct le_link *link = ll_link_for_handle(handle);
        if (!link) {
            if (log_tag_const_e_LL)
                log_print(4, NULL, "<Error>: [LL]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
            return 2;
        }
        bool slave = link->role == 1;
        if (config_asser && slave)
            btctrler_assert_failed("!ROLE_IS_SLAVE(link) le_start_encrypt", 0);
        else if (!config_asser && slave)
            cpu_reset();
        memcpy(&le_param.start_encryption_param, param, 28);
        ll_response_timeout_start(link);
        ll_control_data_step_start(link, link->encrypt_enable ? restart_encryption_steps : start_encryption_steps);
        return 0;
    }
    case 26: { /* HCI long-term key request reply */
        if ((config_btctler_le_features & 1) == 0)
            return 1;
        u16 handle;
        memcpy(&handle, param, sizeof(handle));
        struct le_link *link = ll_link_for_handle(handle);
        if (!link) {
            if (log_tag_const_e_LL)
                log_print(4, NULL, "<Error>: [LL]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
            return 2;
        }
        bool slave = link->role == 1;
        if (config_asser && !slave)
            btctrler_assert_failed("ROLE_IS_SLAVE(link) le_start_encrypt_req", 0);
        else if (!config_asser && !slave)
            cpu_reset();
        memcpy(&le_param.long_term_key_request_reply_param, param, 18);
        ll_control_data_step_start(link, link->encrypt_enable ?
                                   restart_encryption_req_steps : start_encryption_req_steps);
        return 0;
    }
    case 27: { /* HCI long-term key negative reply */
        if ((config_btctler_le_features & 1) == 0)
            return 1;
        u16 handle;
        memcpy(&handle, param, sizeof(handle));
        struct le_link *link = ll_link_for_handle(handle);
        if (!link) {
            if (log_tag_const_e_LL)
                log_print(4, NULL, "<Error>: [LL]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
            return 2;
        }
        bool slave = link->role == 1;
        if (config_asser && !slave)
            btctrler_assert_failed("ROLE_IS_SLAVE(link) __ll_send_reject_ind", 0);
        else if (!config_asser && !slave)
            cpu_reset();
        (__ble_ops->ioctrl)(link->hw, 17, 0);
        ll_send_control_data(link, 13, "1", 6);
        return 0;
    }
    case 1030:
        return le_disconnect((const struct disconnect_paramter *)param);
    case 32: { /* LL_CONNECTION_PARAM_REQ */
        if ((config_btctler_le_features & 2) == 0)
            return 1;
        u16 handle;
        memcpy(&handle, param, sizeof(handle));
        struct le_link *link = ll_link_for_handle(handle);
        if (!link) {
            if (log_tag_const_e_LL)
                log_print(4, NULL, "<Error>: [LL]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
            return 2;
        }
        memcpy(&le_param.conn_update_param, param, 14);
        ll_control_data_step_start(link, link->role == 1 ?
                                   slave_connection_parameter_respond_steps : master_connection_update_steps);
        return 1;
    }
    case 33: { /* LL_CONNECTION_PARAM_RSP */
        if ((config_btctler_le_features & 2) == 0)
            return 1;
        u16 handle;
        memcpy(&handle, param, sizeof(handle));
        struct le_link *link = ll_link_for_handle(handle);
        if (!link) {
            if (log_tag_const_e_LL)
                log_print(4, NULL, "<Error>: [LL]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
            return 2;
        }
        ll_send_control_data(link, 17, "11", link->last_opcode, param[2]);
        return 1;
    }
    case 34: { /* LL_LENGTH_REQ */
        if ((config_btctler_le_features & 32) == 0)
            return 1;
        u16 handle;
        memcpy(&handle, param, sizeof(handle));
        struct le_link *link = ll_link_for_handle(handle);
        if (!link)
            return 2;
        if ((link->state & 0xfe) == 6) {
            memcpy(&le_param.set_data_length_param, param, 6);
            ll_control_data_step_start(link, data_length_update_steps);
        } else {
            ll_assert_or_reset("0 le_data_length_update", 0);
        }
        return 0;
    }
    case 131: {
        if (!param)
            ll_assert_or_reset("param != NULL __ll_send_vendor_test_key_num", 0);
        bool log = log_tag_const_i_LL != 0;
        u16 handle;
        memcpy(&handle, param, sizeof(handle));
        if (log)
            log_print(2, NULL, "[Info]: [LL]ll_hdl:%x,key:%x\n\r\n", handle, param[2]);
        struct le_link *link = ll_link_for_handle(handle);
        if (!link)
            return 2;
        if (log)
            log_print(2, NULL, "[Info]: [LL]ll_key_num:%x\n\r\n", param[2]);
        ll_send_control_data(link, 68, "1", param[2]);
        return 1;
    }
    case 132: {
        u16 handle, hold;
        memcpy(&handle, param, sizeof(handle));
        memcpy(&hold, param + 2, sizeof(hold));
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]ll_hold_latency_cnt:handle %x,hold %x\n\r\n",
                      handle, hold);
        struct le_link *link = ll_link_for_handle(handle);
        if (!link)
            return 2;
        (__ble_ops->ioctrl)(link->hw, 23, hold);
        return 0;
    }
    default:
        return 1;
    }
}

struct le_link *LL_CODE ll_link_for_address(u8 addr_type, const u8 *addr)
{
    if (ll_hdl.number == 0)
        return NULL;
    for (struct list_head *node = ll_hdl.le_links.next;
         node != &ll_hdl.le_links; node = node->next) {
        struct le_link *link = (struct le_link *)((u8 *)node - offsetof(struct le_link, entry));
        if (link->peer.addr_type == addr_type && memcmp(link->peer.addr, addr, 6) == 0)
            return link;
    }
    return NULL;
}

struct le_link *LL_CODE ll_link_for_state(u8 state)
{
    if (ll_hdl.number == 0)
        return NULL;
    for (struct list_head *node = ll_hdl.le_links.next;
         node != &ll_hdl.le_links; node = node->next) {
        struct le_link *link = (struct le_link *)((u8 *)node - offsetof(struct le_link, entry));
        if (link->state == state)
            return link;
    }
    return NULL;
}

int LL_CODE ll_get_conn_link_num(void)
{
    int n = 0;
    for (struct list_head *node = ll_hdl.le_links.next;
         node != &ll_hdl.le_links; node = node->next)
        n += ((struct le_link *)((u8 *)node - offsetof(struct le_link, entry)))->state == 7;
    return n;
}

struct le_link *LL_CODE ll_link_for_handle(int handle)
{
    if (ll_hdl.number == 0)
        return NULL;
    for (struct list_head *node = ll_hdl.le_links.next;
         node != &ll_hdl.le_links; node = node->next) {
        struct le_link *link = (struct le_link *)((u8 *)node - offsetof(struct le_link, entry));
        if (link->handle == handle)
            return link;
    }
    return NULL;
}

struct le_link *LL_CODE get_ll_link_from_cur_number(void)
{
    return ll_link_for_handle(ll_hdl.number);
}

u16 LL_CODE get_ble_link_handle(void)
{
    return 80;
}

u16 LL_CODE alloc_link_handle(void)
{
    u16 bitmap = ll_hdl.handle;
    unsigned i;
    for (i = 0; i < 16; i++) {
        u16 bit = (u16)(1u << i);
        if ((bitmap & bit) == 0) {
            ll_hdl.handle = bitmap | bit;
            break;
        }
    }
    if (i == 16)
        ll_assert_or_reset("i < 16 alloc_link_handle error", 0);
    return (u16)(i | 80);
}

u8 LL_CODE hop_generarate(void)
{
    u32 counter = *(volatile u32 *)0x1e3b04;
    u32 rem = counter % 17;
    return (u8)(rem > 5 ? rem : 5);
}

struct le_link *LL_CODE ll_create_conn_link(void *hw, struct le_addr *local_addr,
                                            u8 role, u8 peer_addr_type,
                                            u8 *peer_addr, void *ll_data)
{
    if ((config_btctler_le_roles & 3) == 0) {
        if (log_tag_const_e_LL)
            log_print(4, NULL, "<Error>: [LL]No either LE_MASTER or LE_SLAVE support\r\n");
        return NULL;
    }
    struct le_link *link = __bt_malloc(336);
    if (!link && log_tag_const_e_LL)
        log_print(4, NULL, "<Error>: [LL]conn link NULL\r\n");
    link->hw = hw;
    memcpy(&link->local, &local_addr->addr_type, 7);
    link->peer.addr_type = peer_addr_type;
    memcpy(link->peer.addr, peer_addr, 6);
    link->role = role;
    if (role == 1) {
        memcpy(&link->ll_data, ll_data, 22);
        ll_slave_cal_widening(&link->ll_data);
    } else {
        memcpy(&link->ll_data, ll_data, 32);
    }
    (__ble_ops->ioctrl)(hw, 6, &link->ll_data);
    ll_wifi_detect_init(link);
    INIT_LIST_HEAD(&link->rx_oneshot_head);
    INIT_LIST_HEAD(&link->event_oneshot_head);
    INIT_LIST_HEAD(&link->acl_packet_head);
    INIT_LIST_HEAD(&link->event_oneshot_post_head);
    link->state = 6;
    (__ble_ops->handler_register)(hw, link, &conn_handler);
    __event_oneshot_post_add(link, ll_conn_fast_supervision_timer_handler, 6);
    return link;
}

void LL_CODE ll_conn_fast_supervision_timer_handler(struct le_link *link)
{
    ll_disconnect_process(link, 62);
}

void LL_CODE ll_create_conn_link_post(struct le_link *link)
{
    link->handle = alloc_link_handle();
    link_data_extension_init(link);
    ll_hdl.number++;
    if (link->role == 0)
        ll_afh_open(&link->ll_data);
    list_add_tail(&link->entry, &ll_hdl.le_links);
}

void LL_CODE ll_close_conn_link(struct le_link *link)
{
    struct list_head *head, *node;
    local_irq_disable();
    head = &link->rx_oneshot_head;
    for (node = head->next; node != head;) {
        struct list_head *next = node->next;
        list_del(node);
        __bt_free(node);
        node = next;
    }
    local_irq_enable();

    local_irq_disable();
    head = &link->event_oneshot_head;
    for (node = head->next; node != head;) {
        struct list_head *next = node->next;
        list_del(node);
        __bt_free(node);
        node = next;
    }
    local_irq_enable();

    local_irq_disable();
    head = &link->event_oneshot_post_head;
    for (node = head->next; node != head;) {
        struct list_head *next = node->next;
        list_del(node);
        __bt_free(node);
        node = next;
    }
    local_irq_enable();

    head = &link->acl_packet_head;
    for (node = head->next; node != head;) {
        struct list_head *next = node->next;
        list_del(node);
        __bt_free(node);
        node = next;
    }
    if (link->hw)
        (__ble_ops->close)(link->hw);
    ll_wifi_detect_ext(link);
    if (link->role == 0)
        ll_afh_close(link);
    list_del(&link->entry);
    ll_hdl.handle = (u16)(((1u << (link->handle & 15)) ^ 0xffff) & ll_hdl.handle);
    ll_hdl.number--;
    __bt_free(link);
}

void LL_CODE ll_delete_link(struct le_link *link)
{
    if ((config_btctler_le_roles & 3) == 0)
        return;
    if (link->handle == ll_get_update_handle_ir()) {
        ll_set_update_handle(0);
        link->rx_update_handle = 0;
        if (link->ll_delete_hook)
            link->ll_delete_hook(NULL);
    }
    os_mutex_pend(&ll_hdl.link_lock, 0);
    ll_response_timeout_stop(link);
    ll_supervision_timeout_stop(link);
    ll_resolution_disable();
    link->fifo_rd = 0;
    link->fifo_wr = 0;
    /* The SDK clears five bytes here, not five complete FIFO entries. */
    memset(link->procedure_fifo, 0, 5);
    ll_close_conn_link(link);
    os_mutex_post(&ll_hdl.link_lock);
}

void LL_CODE ll_disconnect_process(struct le_link *link, u8 reason)
{
    if (link->state == 8) {
        if (log_tag_const_e_LL)
            log_print(4, NULL, "<Error>: [LL]Already disconnect\r\n");
        return;
    }
    u16 handle = link->handle;
    link->state = 8;
    link->reason = reason;
    thread_resume(&ll_hdl.thread);
    hci_event_disconnection_complete((s8)handle, reason);
    tws_phone_ble_link_disconnection(link);
}

void LL_CODE ll_supervision_timeout_start(struct le_link *link, u32 timeout)
{
    link->supervision_timeout = sys_timer_register(timeout, ll_conn_supervision_timer_handler);
    if (!link->supervision_timeout)
        ll_assert_or_reset("link->supervision_timeout", 0);
    sys_timer_set_context(link->supervision_timeout, link);
}

void LL_CODE ll_supervision_timeout_changeperiod(struct le_link *link, u32 timeout)
{
    if (link->supervision_timeout) {
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]ll_supervision_timeout_changeperiod %d ms\r\n", timeout);
        sys_timer_change_period(link->supervision_timeout, timeout);
    }
}

static void LL_CODE ll_conn_supervision_timer_handler(void *priv)
{
    if (log_tag_const_e_LL)
        log_print(4, NULL, "<Error>: [LL]TO - ll_conn_supervision_timer_handler\r\n");
    if (!priv)
        return;
    struct le_link *link = priv;
    ll_supervision_timeout_stop(link);
    ll_disconnect_process(link, (link->state | 1) == 7 ? 8 : 62);
}

void LL_CODE ll_response_timeout_start(struct le_link *link)
{
    if (link->response_timeout) {
        if (log_tag_const_e_LL)
            log_print(4, NULL, "<Error>: [LL]repeat Reg_Timeout\r\n");
        ll_response_timeout_stop(link);
    }
    link->response_timeout = sys_timer_register(40000, ll_response_timeout_handler);
    if (!link->response_timeout)
        ll_assert_or_reset("link->response_timeout", 0);
    sys_timer_set_context(link->response_timeout, link);
}

static void LL_CODE ll_response_timeout_handler(void *priv)
{
    if (!priv)
        return;
    struct le_link *link = priv;
    if (log_tag_const_e_LL)
        log_print(4, NULL, "<Error>: [LL]TO - Response_Timeout\r\n");
    ll_response_timeout_stop(link);
    ll_control_procedure_finish(link, NULL, 4);
    ll_disconnect_process(link, 34);
}

void LL_CODE ll_conn_param_vendor_set(u16 offset0, u8 instant_cnt)
{
    ll_vendor_offset0 = offset0;
    ll_vendor_instant_cnt = instant_cnt;
}

u8 __attribute__((weak)) LL_CODE hook_ll_receive_connection_param_req_ir(u16 conn_handle, const u8 *data)
{
    u16 interval_min = btctler_little_endian_read_16(data, 0);
    u16 interval_max = btctler_little_endian_read_16(data, 2);
    u16 latency = btctler_little_endian_read_16(data, 4);
    u16 timeout = btctler_little_endian_read_16(data, 6);
    if (log_tag_const_i_LL)
        log_print(2, NULL, "[Info]: [LL]ll_recive_conn_param_req:interval %d~%d,latency %d, TimeOut %d,\n\r\n",
                  interval_min, interval_max, latency, timeout);
    (void)conn_handle;
    return 1;
}

void LL_CODE ll_debug_info(struct le_link *link)
{
    if (!log_tag_const_i_LL)
        return;
    log_print(2, NULL, "[Info]: [LL]>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\r\n");
    log_print(2, NULL, "[Info]: [LL]aa\r\n");
    printf_buf(link->ll_data.aa, 4);
    log_print(2, NULL, "[Info]: [LL]crcinit\r\n");
    printf_buf(link->ll_data.crcinit, 3);
    log_print(2, NULL, "[Info]: [LL]winsize : %02x\r\n", link->ll_data.winsize);
    log_print(2, NULL, "[Info]: [LL]winoffset : %04x\r\n", link->ll_data.winoffset);
    log_print(2, NULL, "[Info]: [LL]interval: %04x\r\n", link->ll_data.interval);
    log_print(2, NULL, "[Info]: [LL]latency : %04x\r\n", link->ll_data.latency);
    log_print(2, NULL, "[Info]: [LL]timeout : %04x\r\n", link->ll_data.timeout);
    log_print(2, NULL, "[Info]: [LL]channel map\r\n");
    printf_buf(link->ll_data.channel, 5);
    log_print(2, NULL, "[Info]: [LL]hop: %02x\r\n", link->ll_data.hop);
    log_print(2, NULL, "[Info]: [LL]sca: %02x\r\n", link->ll_data.sca);
    log_print(2, NULL, "[Info]: [LL]widening : %04x\r\n", link->ll_data.widening);
    log_print(2, NULL, "[Info]: [LL]>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\r\n");
}

void __attribute__((weak)) LL_CODE ll_conn_rx_acl_callback(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
}

void LL_CODE conn_rx_acl_u_post_handler(struct le_link *link, struct ble_rx *rx)
{
    if (!rx->len)
        return;
    u8 *packet = rx->head;
    packet[0] = (u8)link->handle;
    if (rx->llid == 1 || rx->llid == 2)
        packet[1] = (u8)(link->handle >> 8) | (rx->llid == 1 ? 16 : 32);
    packet[2] = rx->len;
    packet[3] = 0;
    if (log_tag_const_d_LL) {
        log_print(1, NULL, "[Debug]: [LL]ACL <\r\n");
        printf_buf(packet, rx->len + 4);
    }
    (void)h4_transport->send_packet(2, packet, rx->len + 4);
    if (config_vendor_le_bb)
        ll_conn_rx_acl_callback(packet, rx->len + 4);
}

u8 LL_CODE ll_read_channel_map(u8 *channel_map, const u8 *data)
{
    u16 handle = btctler_little_endian_read_16(data, 0);
    struct le_link *link = ll_link_for_handle(handle & 4095);
    if (!link)
        return 2;
    memcpy(channel_map, link->ll_data.channel, 5);
    return 0;
}

void LL_CODE ll_set_rf_debug(u16 debug_index)
{
    (__ble_ops->ioctrl)(NULL, 20, debug_index);
}

void LL_CODE ll_init(void)
{
    if (power_is_poweroff_post()) {
        ll_hdl.pEVT = (struct lbuff_head *)lbuf_init_btctrler(ll_hdl.pEvent_buf, 512);
        thread_create(&ll_hdl.thread, "link_layer", ll_thread_process, &bt_task_thread);
        return;
    }
    if (log_tag_const_i_LL)
        log_print(2, NULL, "[Info]: [LL]ll_init normal init\r\n");
    ll_hdl.pEvent_buf = pEvent_buf_static;
    memcpy(ll_hdl.fixed_channel_map, init_fixed_channel, 5);
    int mutex_result = os_mutex_create(&ll_hdl.link_lock);
    if (mutex_result && log_tag_const_e_LL)
        log_print(4, NULL, "<Error>: [LL]ll_init creat mutex err\r\n");
    ll_ext_handle_malloc();
    ll_set_default_prefers_phys();
    INIT_LIST_HEAD(&ll_hdl.le_links);
    ll_white_list_init();
    ll_resolving_list_init();
    ll_data_extension_init();
    ll_hdl.pEVT = (struct lbuff_head *)lbuf_init_btctrler(ll_hdl.pEvent_buf, 512);
    if (!ll_hdl.pEVT)
        ll_assert_or_reset("__this->pEVT", 0);
    __ble_ops->init();
    thread_create(&ll_hdl.thread, "link_layer", ll_thread_process, &bt_task_thread);
    memcpy(le_param.features, &config_btctler_le_features, 8);
    le_param.compld = hci_param.manufacturer_name;
    le_param.versnr = hci_param.hci_version;
    le_param.acl_packets_total_num = (u8)config_btctler_le_acl_total_nums;
    le_param.acl_data_packet_length = (u16)config_btctler_le_acl_packet_length;
}

void LL_CODE ll_destroy(void)
{
    if (power_is_poweroff_post())
        return;
    ll_adv_close();
    ll_scan_destroy();
    ll_init_close();
    bool have_links = ll_hdl.number != 0;
    struct list_head *node = ll_hdl.le_links.next;
    while (have_links && node != &ll_hdl.le_links) {
        struct list_head *next = node->next;
        struct le_link *link = (struct le_link *)((u8 *)node - offsetof(struct le_link, entry));
        if (log_tag_const_i_LL)
            log_print(2, NULL, "[Info]: [LL]ll_destory handle : %x\r\n", link->handle);
        ll_delete_link(link);
        node = next;
    }
    if (ll_hdl.thread.fun)
        thread_delete(&ll_hdl.thread);
    __ble_ops->destroy();
    if (ll_hdl.pEVT)
        lbuf_clear_btctrler((struct lbuff_head_btctrler *)ll_hdl.pEVT);
    os_mutex_del(&ll_hdl.link_lock, 0);
}

void LL_CODE ll_reset(void)
{
    ll_ext_destroy();
    ll_destroy();
    ll_init();
}

void LL_CODE ll_thread_resume(void)
{
    thread_resume(&ll_hdl.thread);
}

void LL_CODE __set_ll_local_addr_common(struct le_addr *addr, u8 own_addr_type)
{
    addr->addr_type = own_addr_type;
    switch (own_addr_type) {
    case 0:
    case 2:
        memcpy(addr->addr, le_param.public_address, 6);
        if (log_tag_const_i_LL) {
            log_print(2, NULL, "[Info]: [LL]LE public addr :\r\n");
            printf_buf(addr->addr, 6);
        }
        break;
    case 1:
    case 3:
        memcpy(addr->addr, le_param.random_address, 6);
        if (log_tag_const_i_LL) {
            log_print(2, NULL, "[Info]: [LL]LE random addr :\r\n");
            printf_buf(addr->addr, 6);
        }
        break;
    default:
        break;
    }
}

void LL_CODE __set_ll_local_addr(void *hw, struct le_addr *addr, u8 own_addr_type)
{
    __set_ll_local_addr_common(addr, own_addr_type);
    (__ble_ops->ioctrl)(hw, 2, addr->addr_type, addr->addr);
}

static void LL_CODE __slave_ll_send_start_enc_req_callback(void *priv)
{
    (__ble_ops->ioctrl)(*(void **)priv, 11, 1);
}

static void atomic_add_return(atomic_t *v)
{
    local_irq_disable();
#ifndef UNITTEST
    __asm__ volatile("csync;" ::: "memory");
#endif
    v->counter = v->counter + 1;
    local_irq_enable();
}

static void atomic_sub_return(atomic_t *v)
{
    local_irq_disable();
#ifndef UNITTEST
    __asm__ volatile("csync;" ::: "memory");
#endif
    v->counter = v->counter - 1;
    local_irq_enable();
}

static void LL_CODE __master_ll_send_conn_update_done(struct le_link *link)
{
    le_connection_update_complete_event(link, NULL, 0);
}

static void LL_CODE __master_ll_send_conn_update_req_callback(void *priv)
{
    (__ble_ops->ioctrl)(*(void **)priv, 18, 1);
}

static void LL_CODE __master_ll_send_channel_map_req_callback(void *priv)
{
    (__ble_ops->ioctrl)(*(void **)priv, 18, 1);
}

static void LL_CODE __master_ll_send_enc_req_callback(void *priv)
{
    (__ble_ops->ioctrl)(*(void **)priv, 17, 1);
}

static void LL_CODE __attribute__((noinline)) __slave_ll_send_start_enc_req(void)
{
    u8 iv[8];
    u8 session_key[16];
    u8 skd[16];
    struct le_link *link = ll_link_for_handle(
        le_param.long_term_key_request_reply_param.connection_handle);
    if (!link) {
        if (log_tag_const_e_LL)
            log_print(4, NULL, "<Error>: [LL]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
        return;
    }
    memcpy(iv, link->ivm, 4);
    memcpy(iv + 4, link->ivs, 4);
    bool log_info = log_tag_const_i_LL != 0;
    if (log_info) {
        log_print(2, NULL, "[Info]: [LL]IV :\r\n");
        printf_buf(iv, 8);
    }
    (__ble_ops->ioctrl)(link->hw, 0, iv);
    memcpy(skd, link->skdm, 8);
    memcpy(skd + 8, link->skds, 8);
    memcpy(link->long_term_key,
           le_param.long_term_key_request_reply_param.long_term_key, 16);
    aes128_start(link->long_term_key, skd, session_key);
    if (log_info) {
        log_print(2, NULL, "[Info]: [LL]STK : \r\n");
        printf_buf(link->long_term_key, 16);
    }
    btctler_reverse_128(session_key, skd);
    if (log_info) {
        log_print(2, NULL, "[Info]: [LL]SKD : \r\n");
        printf_buf(skd, 16);
    }
    (__ble_ops->ioctrl)(link->hw, 1, skd);
    ll_send_control_data_callback_extend(link, 0,
                                         __slave_ll_send_start_enc_req_callback,
                                         5, 0);
}

static void LL_CODE conn_rx_probe_handler(void *priv, struct ble_rx *rx)
{
    struct le_link *link = priv;
    if (link->state == 6) {
        link->state = 7;
        local_irq_disable();
        struct list_head *head = &link->event_oneshot_post_head;
        struct list_head *entry = head->next;
        while (entry != head) {
            struct event_oneshot *event = (struct event_oneshot *)entry;
            struct list_head *next = entry->next;
            if (event->func == ll_conn_fast_supervision_timer_handler &&
                event->instant == 6) {
                list_del(entry);
                __bt_free(event);
            }
            entry = next;
        }
        local_irq_enable();
        if (tws_link_get_ble_role_slave())
            tws_link_ble_slave_conn_establish_probe_handler();
        else
            __rx_oneshot_add(link, __set_conn_winsize);
    }
    if (link->supervision_timeout)
        sys_timer_reset(link->supervision_timeout);
    local_irq_disable();
    struct list_head *head = &link->rx_oneshot_head;
    struct list_head *entry = head->next;
    while (entry != head) {
        struct rx_oneshot *once = (struct rx_oneshot *)entry;
        struct list_head *next = entry->next;
        list_del(entry);
        once->func(link);
        __bt_free(once);
        entry = next;
    }
    local_irq_enable();
    if (!rx->invalid) {
        cyclone2_air_rx();
        link->rssi = rx->rssi;
        conn_rx_probe_channel_handler(link, rx);
    }
    if (rx->llid != 1 || rx->len != 0)
        thread_resume(&ll_hdl.thread);
}

static void LL_CODE conn_tx_probe_handler(void *priv, struct ble_tx *tx)
{
    struct le_link *link = priv;
    if (tx->llid == 3)
        atomic_add_return(&link->air_sent_acl_c_num);
    else if (tx->llid == 2 || tx->llid == 1)
        atomic_add_return(&link->air_sent_acl_u_num);
    thread_resume(&ll_hdl.thread);
}

static void LL_CODE conn_evt_probe_handler(void *priv, int type, int param)
{
    struct le_link *link = priv;
    if (type == 0) {
        if (!link)
            ll_assert_or_reset("link != NULL %s", 155);
        local_irq_disable();
        link->event_cnt = (u16)param;
        struct list_head *head = &link->event_oneshot_head;
        struct list_head *entry = head->next;
        while (entry != head) {
            struct event_oneshot *event = (struct event_oneshot *)entry;
            struct list_head *next = entry->next;
            if (__instant_link_lost_ir((u16)event->instant, (u16)param)) {
                list_del(entry);
                event->func(link);
                __bt_free(event);
            }
            entry = next;
        }
        local_irq_enable();
        ll_wifi_detect_probe_process(link);
        if ((link->wifi_detect_pend.counter & 15) != 0) {
            link->wifi_detect_pend.counter = (link->wifi_detect_pend.counter & 15) | 16;
            thread_resume(&ll_hdl.thread);
            return;
        }
        local_irq_disable();
        head = &link->event_oneshot_post_head;
        entry = head->next;
        while (entry != head) {
            struct event_oneshot *event = (struct event_oneshot *)entry;
            struct list_head *next = entry->next;
            if (__instant_link_lost_ir((u16)event->instant, (u16)param)) {
                local_irq_enable();
                if (log_tag_const_i_LL)
                    log_print(2, NULL, "[Info]: [LL]Post - Instant %d\r\n", param);
                thread_resume(&ll_hdl.thread);
                return;
            }
            entry = next;
        }
        local_irq_enable();
        if (link->echo_req_type == 1 &&
            (link->current_procedure & 32771) == 0)
            thread_resume(&ll_hdl.thread);
    } else if (type == 1) {
        conn_evt_probe_evtchannel_handler(link, param);
    }
}

static void LL_CODE conn_echo_req_handler(void *priv, int type, u8 *data, u16 size)
{
    (void)size;
    if ((config_btctler_le_roles & 1) == 0)
        return;
    struct le_link *link = priv;
    link->echo_req_type = (u8)type;
    link->echo_req_param = btctler_little_endian_read_32(data, 0);
    thread_resume(&ll_hdl.thread);
}

static void LL_CODE ll_thread_process(struct thread *th)
{
    int is_empty = 1;
    (void)th;
    tws_link_ble_slave_conn_establish_handler();

    struct le_event *event = lbuf_pop_btctrler(
        (struct lbuff_head_btctrler *)ll_hdl.pEVT);
    const bool log_info = log_tag_const_i_LL != 0;
    if (event) {
        if (log_info) {
            log_print(2, NULL, "[Info]: [LL]META EVT <\r\n");
            printf_buf(event, event->len + 2);
        }
        h4_transport->send_packet(4, (u8 *)event, event->len + 2);
        lbuf_free_btctrler(event);
    }

    int adv_empty = adv_rx_post_handler();
    int scan_empty = scan_rx_post_handler();
    int init_empty = init_rx_post_handler();
    is_empty &= adv_empty & scan_empty & init_empty;

    struct list_head *head = &ll_hdl.le_links;
    struct list_head *node = head->next;
    const int no_roles = (config_btctler_le_roles & 3) == 0;
    while (node != head) {
        struct list_head *next_node = node->next;
        struct le_link *link = (struct le_link *)((char *)node - offsetof(struct le_link, entry));
        if (!no_roles) {
            int sent_acl = 0;
            for (;;) {
#ifndef UNITTEST
                __asm__ volatile("csync;" ::: "memory");
#endif
                if (*(volatile int *)&link->air_sent_acl_u_num.counter == 0)
                    break;
                if (list_empty(&link->acl_packet_head))
                    break;
                atomic_sub_return(&link->air_sent_acl_u_num);
                if (!list_empty(&link->acl_packet_head)) {
                    struct acl_fragment_packet *fragment =
                        (struct acl_fragment_packet *)link->acl_packet_head.next;
                    fragment->num_acl_packets_sent--;
                    int freed = fragment->num_acl_packets_sent == 0;
                    if (freed) {
                        list_del(&fragment->entry);
                        __bt_free(fragment);
                    }
                    sent_acl += freed;
                }
            }
            if (sent_acl)
                hci_event_number_of_completed_packets(link, sent_acl);
            for (;;) {
#ifndef UNITTEST
                __asm__ volatile("csync;" ::: "memory");
#endif
                if (*(volatile int *)&link->air_sent_acl_c_num.counter == 0)
                    break;
                atomic_sub_return(&link->air_sent_acl_c_num);
                if (log_info)
                    log_print(2, NULL, "[Info]: [LL] - ack\r\n");
                ll_control_step_verify((u16)0x8000, link, 0);
                ll_send_control_data_state_machine(link, NULL);
            }
            if (link->role == 1)
                conn_slave_rx_post_handler(link);
            else
                conn_master_rx_post_handler(link);

            local_irq_disable();
            struct list_head *post_head = &link->event_oneshot_post_head;
            struct list_head *entry = post_head->next;
            while (entry != post_head) {
                struct event_oneshot *once = (struct event_oneshot *)entry;
                struct list_head *next = entry->next;
                if (__instant_link_lost_ir((u16)once->instant, link->event_cnt)) {
                    list_del(entry);
                    once->func(link);
                    __bt_free(once);
                }
                entry = next;
            }
            local_irq_enable();
        }

        if (link->state == 8) {
            if (log_tag_const_e_LL)
                log_print(4, NULL, "<Error>: [LL]thread LL_DISCONNECT : handle 0x%x\r\n", link->handle);
            ll_delete_link(link);
            hci_send_event(254, "11", 0, 0);
        } else {
            ll_update_channel_map_post_handler(link);
            if (link->echo_req_type == 1) {
                u32 procedure = link->current_procedure;
                if (procedure & 32771) {
                    if (log_info)
                        log_print(2, NULL, "[Info]: [LL]echo_update conflict,%04x\n\r\n", procedure);
                } else if (ll_control_procedure_push_ir(link, 0)) {
                    if (log_info)
                        log_print(2, NULL, "[Info]: [LL]ECHO_RF_REQ_CONN_UPDATE\r\n");
                    le_param.conn_update_param.connection_handle = link->handle;
                    le_param.conn_update_param.Conn_Interval_Min = link->ll_data.interval;
                    le_param.conn_update_param.Conn_Interval_Max = link->ll_data.interval;
                    le_param.conn_update_param.Conn_Latency = link->ll_data.latency;
                    le_param.conn_update_param.Supervision_Timeout = link->ll_data.timeout;
                    link->echo_req_type = 2;
                    ll_control_data_step_start(link, master_connection_update_steps);
                }
            }
            int link_empty;
            __ble_ops->ioctrl(link->hw, 19, &link_empty);
            is_empty &= link_empty;
            if (link->interval_event_send != 0 &&
                link->interval_event_enable != 0) {
                link->interval_event_send = 0;
                le_vendor_interval_complete_event(link, 0);
            }
        }
        node = next_node;
    }

    if (config_btctler_le_features & 4096)
        is_empty &= ll_ext_rx_post_process_ir();
    if (is_empty && lbuf_empty_btctrler((struct lbuff_head_btctrler *)ll_hdl.pEVT))
        thread_suspend(th, 0);
}

void LL_CODE ll_slave_cal_widening(struct ble_conn_param *param)
{
    u32 tick = (config_vendor_le_bb & 16) ? 625 : 1250;
    u32 sca = master_sca[param->sca];
    u32 widening = (((param->interval * tick * (sca + 500) * (param->latency + 1)) / 1000000) << 1) + 2;
    param->widening = (u16)widening;
}

u8 *LL_CODE ll_get_fixed_channel_map(void)
{
    return ll_hdl.fixed_channel_map;
}

static int LL_CODE check_transitions_bits(const u8 *pbuf, int len)
{
    int last = pbuf[0] & 1;
    int transitions = 0;
    for (int i = 1; i < len; i++) {
        int bit = (pbuf[i / 8] & (1 << (i % 8))) >> (i % 8);
        transitions += last != bit;
        last = bit;
    }
    return transitions;
}

u32 LL_CODE lctrComputeAccessAddr(void)
{
    u32 value;
    pseudo_random_genrate((u8 *)&value, 4);
    return (value & 0x017af5ca) |
           ((u32)lctrComputeAccessAddr_upperSixBits[value >> 27] << 24) |
           0x02040831;
}

u32 LL_CODE lctrComputeSeedAccessAddr(void)
{
    u32 value;
    pseudo_random_genrate((u8 *)&value, 4);
    return (value & 0xfd007fff) |
           ((u32)lctrComputeSeedAccessAddr_upperSixBits[value >> 27] << 15) |
           0x00800000;
}

u32 LL_CODE lctrComputeCrcInit(void)
{
    u32 value;
    pseudo_random_genrate((u8 *)&value, 3);
    return value & 0x00ffffff;
}

void LL_CODE access_addr_generate(u8 *aa)
{
    for (;;) {
        pseudo_random_genrate(aa, 4);
        int zeros = 0, max_zeros = 0;
        for (int byte = 0; byte != 4; byte++) {
            for (int bit = 0; bit != 8; bit++) {
                if ((aa[byte] & (1 << bit)) == 0) {
                    zeros++;
                } else {
                    if (zeros > max_zeros)
                        max_zeros = zeros;
                    zeros = 0;
                }
            }
        }
        if (max_zeros > 6)
            continue;

        int ones = 0, max_ones = 0;
        for (int byte = 0; byte != 4; byte++) {
            for (int bit = 0; bit != 8; bit++) {
                if (aa[byte] & (1 << bit)) {
                    ones++;
                } else {
                    if (ones > max_ones)
                        max_ones = ones;
                    ones = 0;
                }
            }
        }
        if (max_ones > 6 || memcmp(aa, sample_conn_param.aa, 4) == 0)
            continue;

        int differing = 0;
        for (int byte = 0; byte != 4; byte++) {
            u8 diff = aa[byte] ^ sample_conn_param.aa[byte];
            for (int bit = 0; bit != 8; bit++)
                differing += (diff & (1 << bit)) != 0;
        }
        if (differing < 2)
            continue;

        u8 xor_bytes = aa[0];
        for (int i = 1; i != 4; i++)
            xor_bytes ^= aa[i];
        if (xor_bytes == 0)
            continue;
        if (check_transitions_bits(aa, 32) > 24)
            continue;
        if (check_transitions_bits(aa + 3, 6) < 3)
            continue;
        return;
    }
}

u8 LL_CODE __instant_link_lost_ir(u16 instant, u16 eventcnt)
{
    return instant == eventcnt || (u16)(instant - eventcnt) > 32766;
}

#ifdef UNITTEST
#include "test.h"
#include <stdlib.h>
#include <setjmp.h>

int config_vendor_le_bb;
int config_asser;
int config_btctler_le_roles;
u64 config_btctler_le_features;
u8 log_tag_const_i_LL;
u8 log_tag_const_e_LL;
const struct ble_operation *__ble_ops;
void cyclone2_air_rx(void) {}
static u8 test_random_bytes[8];
static struct le_link *test_finish_link;
static int test_fail_alloc;
static int test_irq_disabled;
static int test_irq_disable_count;
static int test_irq_enable_count;
static int test_assert_count;
static int test_assert_saw_irq_disabled;
static const char *test_assert_text;
static int test_log_count;
static int test_log_tag_was_null;
static unsigned test_send_count;
static const u8 *test_send_first_data;
static const u8 *test_send_last_data;
static u8 test_send_first_len;
static u8 test_send_last_len;
static u8 test_send_first_llid;
static u8 test_send_last_llid;
static void *test_ioctrl_hw[2];
static int test_ioctrl_count;
static int test_ioctrl_num[2];
static int test_event_changes_rd;
static jmp_buf test_acl_abort;
static int test_acl_abort_enabled;
static void *test_last_alloc;
static int test_resume_count;
static int test_format_length;
static u8 test_event_storage[8];
struct le_parameter le_param;
struct hci_parameter hci_param;

void thread_resume(struct thread *th)
{
    (void)th;
    test_resume_count++;
}

int __vsprintf_len(const char *format, va_list ap)
{
    (void)format; (void)ap;
    return test_format_length;
}

int __vsprintf(u8 *data, const char *format, va_list ap)
{
    (void)data; (void)format; (void)ap;
    return 0;
}

void *lbuf_alloc_btctrler(struct lbuff_head_btctrler *head, u32 len)
{
    (void)head; (void)len;
    return test_event_storage;
}

void lbuf_push_btctrler(void *event)
{
    CHECK(event == test_event_storage);
}

static void test_send_packet_high(void *hw, void (*send_cb)(void *),
                                  void (*ack_cb)(void *), u8 llid,
                                  const u8 *data, int len)
{
    (void)hw; (void)send_cb; (void)ack_cb; (void)llid; (void)data; (void)len;
}


void *__bt_malloc(int size)
{
    test_last_alloc = test_fail_alloc ? NULL : calloc(1, (size_t)size);
    return test_last_alloc;
}

int os_mutex_pend(OS_MUTEX *mutex, int timeout)
{
    (void)mutex; (void)timeout;
    return 0;
}

int os_mutex_post(OS_MUTEX *mutex)
{
    (void)mutex;
    return 0;
}

void local_irq_disable(void)
{
    test_irq_disabled = 1;
    test_irq_disable_count++;
}

void local_irq_enable(void)
{
    test_irq_disabled = 0;
    test_irq_enable_count++;
}

void btctrler_assert_failed(const char *what, int value)
{
    (void)value;
    test_assert_count++;
    test_assert_text = what;
    test_assert_saw_irq_disabled = test_irq_disabled;
}

void log_print(int level, const char *tag, const char *format, ...)
{
    (void)level; (void)format;
    test_log_count++;
    test_log_tag_was_null = tag == NULL;
}

static void test_send_packet(void *hw, u8 llid, const u8 *data,
                             int len, int max_tx_octets)
{
    (void)hw; (void)max_tx_octets;
    if (!test_send_count) {
        test_send_first_data = data;
        test_send_first_len = (u8)len;
        test_send_first_llid = llid;
    }
    test_send_last_data = data;
    test_send_last_len = (u8)len;
    test_send_last_llid = llid;
    test_send_count++;
    if (test_acl_abort_enabled && test_send_count == 2)
        longjmp(test_acl_abort, 1);
}

static void test_ioctrl(void *hw, int command, ...)
{
    if (test_ioctrl_count < 2) {
        test_ioctrl_hw[test_ioctrl_count] = hw;
        test_ioctrl_num[test_ioctrl_count] = command;
    }
    if (test_ioctrl_count++ == 0)
        test_finish_link->hw = (void *)0x2222;
}

void le_connection_update_complete_event(struct le_link *link,
                                         struct ble_rx *rx,
                                         LL_CONTROL_CASE status)
{
    (void)rx; (void)status;
    if (test_event_changes_rd)
        link->fifo_rd = 3;
}

static const struct ble_operation test_ble_ops = {
    .send_packet = test_send_packet,
    .send_packet_high = test_send_packet_high,
    .ioctrl = test_ioctrl,
};

void pseudo_random_genrate(u8 *out, unsigned int size)
{
    memcpy(out, test_random_bytes, size);
}

int main(void)
{
    /* Allocation assertions precede the IRQ critical section in the SDK. */
    struct le_link oneshot_link = {0};
    test_fail_alloc = 1;
    config_asser = 1;
    test_assert_count = test_irq_disable_count = test_irq_enable_count = 0;
    __event_oneshot_add(&oneshot_link, NULL, 1);
    CHECK(test_assert_count == 1 &&
          strcmp(test_assert_text, "__event_oneshot_add: p != NULL") == 0);
    CHECK(!test_assert_saw_irq_disabled && test_irq_disable_count == 1 &&
          test_irq_enable_count == 1 && !test_irq_disabled);
    test_assert_count = test_irq_disable_count = test_irq_enable_count = 0;
    __rx_oneshot_add(&oneshot_link, NULL);
    CHECK(test_assert_count == 1 &&
          strcmp(test_assert_text, "__rx_oneshot_add: p != NULL") == 0);
    CHECK(!test_assert_saw_irq_disabled && test_irq_disable_count == 1 &&
          test_irq_enable_count == 1 && !test_irq_disabled);
    test_assert_count = test_irq_disable_count = test_irq_enable_count = 0;
    __event_oneshot_post_add(&oneshot_link, NULL, 1);
    CHECK(test_assert_count == 1 &&
          strcmp(test_assert_text, "__event_oneshot_post_add: p != NULL") == 0);
    CHECK(!test_assert_saw_irq_disabled && test_irq_disable_count == 1 &&
          test_irq_enable_count == 1 && !test_irq_disabled);
    test_fail_alloc = 0;
    config_asser = 0;

    /* An ioctrl callback mutates link->hw; raw IR reloads it for call two. */
    struct le_link finish_link = {0};
    const u16 finish_steps[] = {0x0001, 0x7fff};
    finish_link.hw = (void *)0x1111;
    finish_link.procedure_fifo[0].steps_ptr = finish_steps;
    finish_link.procedure_fifo[0].counter = 0;
    finish_link.fifo_rd = 0;
    test_finish_link = &finish_link;
    test_ioctrl_count = 0;
    config_btctler_le_features = 0;
    __ble_ops = &test_ble_ops;
    ll_control_procedure_finish(&finish_link, NULL, 0);
    CHECK(test_ioctrl_count == 2 && test_ioctrl_num[0] == 17 &&
          test_ioctrl_num[1] == 18);
    CHECK(test_ioctrl_hw[0] == (void *)0x1111 &&
          test_ioctrl_hw[1] == (void *)0x2222);

    /* Case zero pops procedure 15 when feature bit 1 is set (SDK condition). */
    struct le_link finish_case0 = {0};
    const u16 case0_steps[] = {0x00ff};
    finish_case0.procedure_fifo[0].steps_ptr = case0_steps;
    finish_case0.current_procedure = 1U << 15;
    finish_case0.fifo_rd = 0;
    config_btctler_le_features = 2;
    __ble_ops = &test_ble_ops;
    ll_control_procedure_finish(&finish_case0, NULL, 0);
    CHECK(finish_case0.current_procedure == 0);
    config_btctler_le_features = 0;

    /* Feature-disabled encryption completions return without dequeuing. */
    for (unsigned kind = 9; kind <= 12; kind++) {
        struct le_link disabled = {0};
        u16 disabled_steps[] = {(u16)((kind << 8) | 255)};
        disabled.procedure_fifo[0].steps_ptr = disabled_steps;
        ll_control_procedure_finish(&disabled, NULL, 0);
        CHECK(disabled.fifo_rd == 0 &&
              disabled.procedure_fifo[0].steps_ptr == disabled_steps);
    }

    /* Clear the original slot, but advance the index reloaded after callbacks. */
    finish_case0.procedure_fifo[0].steps_ptr = case0_steps;
    finish_case0.fifo_rd = 0;
    test_event_changes_rd = 1;
    ll_control_procedure_finish(&finish_case0, NULL, 0);
    CHECK(finish_case0.procedure_fifo[0].steps_ptr == NULL &&
          finish_case0.fifo_rd == 4);
    test_event_changes_rd = 0;

    /* ACL chunks advance by the SDK's truncated u8 send length, not by the
     * wider requested size: 257 wraps to one. Stop after two callbacks;
     * the raw loop would later stall when remaining reaches 256. */
    struct le_link acl_link = {0};
    u8 packet[264] = {0};
    acl_link.pdu_len.connEffectiveMaxTxOctets = 257;
    acl_link.pdu_len.PreConnEffectiveTxOctets = 257;
    acl_link.acl_packet_head.next = &acl_link.acl_packet_head;
    acl_link.acl_packet_head.prev = &acl_link.acl_packet_head;
    acl_link.handle = 0x123;
    INIT_LIST_HEAD(&acl_link.entry);
    INIT_LIST_HEAD(&ll_hdl.le_links);
    ll_hdl.number = 1;
    list_add_tail(&acl_link.entry, &ll_hdl.le_links);
    test_send_count = 0;
    __ble_ops = &test_ble_ops;
    test_acl_abort_enabled = 1;
    if (setjmp(test_acl_abort) == 0)
        ll_send_acl_packet(0x123, packet, 260);
    test_acl_abort_enabled = 0;
    CHECK(test_send_count == 2);
    CHECK(test_send_first_data == packet + 4 && test_send_first_len == 1 &&
          test_send_first_llid == 2);
    CHECK(test_send_last_data == packet + 5 && test_send_last_len == 1 &&
          test_send_last_llid == 1);
    CHECK(list_empty(&acl_link.acl_packet_head));
    free(test_last_alloc);
    list_del(&acl_link.entry);
    ll_hdl.number = 0;

    /* The raw diagnostic call uses a null tag. */
    test_log_count = 0;
    test_log_tag_was_null = 0;
    log_tag_const_e_LL = 1;
    ll_send_acl_packet(0x123, packet, 1);
    log_tag_const_e_LL = 0;
    CHECK(test_log_count == 1 && test_log_tag_was_null);

    /* subevent zero: signed (-1)/8 is zero, with mask bit seven. */
    hci_param.event_mask[7] = 32;
    le_param.event_mask[0] = 128;
    test_format_length = 0;
    CHECK(__hci_emit_le_meta_event_ir(0, NULL));
    CHECK(test_event_storage[0] == 62 && test_event_storage[1] == 1 &&
          test_event_storage[2] == 0);

    /* All three format bounds checks compare unsigned lengths. */
    test_format_length = -1;
    config_asser = 1;
    test_assert_count = 0;
    ll_send_control_data(&acl_link, 0, NULL);
    ll_send_control_data_callback(&acl_link, NULL, 0, NULL);
    ll_send_control_data_callback_extend(&acl_link, NULL, NULL, 0, NULL);
    CHECK(test_assert_count == 3);
    test_format_length = 0;
    config_asser = 0;

    /* The SDK wakes for a nonempty continuation or any non-continuation. */
    struct le_link rx_link = {0};
    struct ble_rx rx = {0};
    rx_link.state = 7;
    INIT_LIST_HEAD(&rx_link.rx_oneshot_head);
    rx.invalid = 1;
    rx.llid = 1;
    test_resume_count = 0;
    conn_rx_probe_handler(&rx_link, &rx);
    CHECK(test_resume_count == 0);
    rx.len = 1;
    conn_rx_probe_handler(&rx_link, &rx);
    CHECK(test_resume_count == 1);
    rx.llid = 2;
    rx.len = 0;
    conn_rx_probe_handler(&rx_link, &rx);
    CHECK(test_resume_count == 2);

    CHECK(lctrCalcTotalAccuracy(0) == 500);
    CHECK(lctrCalcTotalAccuracy(6) == 30);
    CHECK(lctrCalcTotalAccuracy(7) == 20);
    CHECK(lctrCalcTotalAccuracy(255) == 20);

    struct le_link link = {0};
    const u16 steps[] = {0x1234, 0x8000, 0x0200};
    link.procedure_fifo[0].steps_ptr = steps;
    link.procedure_fifo[0].counter = 0;
    CHECK(ll_current_step(&link) == 0x34);
    CHECK(!ll_current_no_step(&link));
    link.procedure_fifo[0].counter = 1;
    CHECK(ll_current_step(&link) == 0x00);
    link.procedure_fifo[0].steps_ptr = NULL;
    CHECK(ll_current_no_step(&link));

    CHECK(ll_control_procedure_case_remap(0) == 0);
    CHECK(ll_control_procedure_case_remap(16391) == 3);
    CHECK(ll_control_procedure_case_remap(16401) == 2);
    CHECK(ll_control_procedure_case_remap(0x1234) == 1);
    CHECK(__instant_link_lost_ir(7, 7));
    CHECK(!__instant_link_lost_ir(0, 65535));
    CHECK(!__instant_link_lost_ir(32766, 0));
    CHECK(__instant_link_lost_ir(32767, 0));

    const u8 transition_sample[] = {0x55, 0x00, 0x01, 0x80};
    CHECK(check_transitions_bits(transition_sample, 5) == 4);
    CHECK(check_transitions_bits(transition_sample, 1) == 0);
    const u8 fixed_map[] = {255, 255, 255, 255, 31};
    memcpy(ll_hdl.fixed_channel_map, fixed_map, sizeof(fixed_map));
    CHECK(memcmp(ll_get_fixed_channel_map(), fixed_map, sizeof(fixed_map)) == 0);

    struct ble_conn_param param = {0};
    param.sca = 7;
    param.interval = 39;
    param.latency = 0;
    config_vendor_le_bb = 0;
    ll_slave_cal_widening(&param);
    /* IR: ((master_sca[7] + 500) * 39 * 1250 / 1000000) * 2 + 2. */
    CHECK(param.widening == 52);
    config_vendor_le_bb = 16;
    ll_slave_cal_widening(&param);
    CHECK(param.widening == 26);
    config_vendor_le_bb = 0;

    /* Raw IR masks the random word and substitutes table entry word>>27. */
    memset(test_random_bytes, 0, sizeof(test_random_bytes));
    test_random_bytes[3] = 0xf0;
    CHECK(lctrComputeAccessAddr() == 0x8e040831);
    CHECK(lctrComputeSeedAccessAddr() == 0xf0bc8000);
    test_random_bytes[0] = 0x12;
    test_random_bytes[1] = 0x34;
    test_random_bytes[2] = 0x56;
    CHECK(lctrComputeCrcInit() == 0x00563412);

    return TEST_DONE("btctrler_link_layer");
}
#endif
