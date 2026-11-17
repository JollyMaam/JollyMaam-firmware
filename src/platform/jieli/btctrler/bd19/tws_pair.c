/* BD19 tws_pair.c.o occurrence 1: TWS connection core (tws_protocol_new).
 * This SDK revision differs substantially from BR23: the LE pairing API is in
 * occurrence 2 (tws_pair_api.c), the TWS link task lives in tws_link.c, and
 * this member adds pure-monitor, low-latency and RSSI-driven role switching.
 * Preserve the SDK's ordering, reloads and quirks. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "btctrler/glue.h"

#define TWS_CODE __attribute__((section(".classic_tws_code")))
#define TWS_WEAK __attribute__((weak, section(".classic_tws_code")))
#define TWS_IRQ __attribute__((section(".tws_irq_code")))
#define TWS_FILE "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/tws/tws_protocol_new/tws_pair.c"
#define memcpy __builtin_memcpy
#define memset __builtin_memset

void log_print(int level, const char *tag, const char *format, ...);
int printf(const char *format, ...);
int putchar(int c);
void p33_soft_reset(void);
int os_taskq_post_type(const char *name, int type, int argc, int *argv);

extern BTCTRLER_CONFIG u8 log_tag_const_i_TWS, log_tag_const_d_TWS, log_tag_const_e_TWS, log_tag_const_v_TWS;
extern BTCTRLER_CONFIG u8 log_tag_const_i_TWS_LMP, log_tag_const_d_TWS_LMP;
extern BTCTRLER_CONFIG int CONFIG_TWS_WORK_MODE, CONFIG_LOW_LATENCY_ENABLE, CONFIG_TWS_RUN_SLOT, CONFIG_PHONE_RUN_SLOT;
extern BTCTRLER_CONFIG int CONFIG_PHONE_LOW_LATENCY_RUN_SLOT, CONFIG_TWS_LOW_LATENCY_RUN_SLOT;
extern BTCTRLER_CONFIG int CONFIG_TWS_SUPER_TIMEOUT, CONFIG_BTCTLER_TWS_ENABLE, CONFIG_ESCO_MUX_RX_BULK_ENABLE;
extern BTCTRLER_CONFIG int CONFIG_A2DP_DELAY_TIME, CONFIG_TWS_POWER_BALANCE_ENABLE, CONFIG_MASTER_MAX_RX_BULK_PERSENT;

#define INFO(...) do { if (log_tag_const_i_TWS) log_print(2, NULL, __VA_ARGS__); } while (0)
#define DEBUG(...) do { if (log_tag_const_d_TWS) log_print(1, NULL, __VA_ARGS__); } while (0)
#define ERROR(...) do { if (log_tag_const_e_TWS) log_print(4, NULL, __VA_ARGS__); } while (0)
#define VERBOSE(...) do { if (log_tag_const_v_TWS) log_print(0, NULL, __VA_ARGS__); } while (0)
#define LMP_INFO(...) do { if (log_tag_const_i_TWS_LMP) log_print(2, NULL, __VA_ARGS__); } while (0)
#define LMP_DEBUG(...) do { if (log_tag_const_d_TWS_LMP) log_print(1, NULL, __VA_ARGS__); } while (0)

/* The SDK evaluates the condition once, then branches on config_asser. */
#define SDK_ASSERT(p, line, text) do { _Bool ok_ = (p); if (config_asser) { if (!ok_) { \
    printf("file:%s, line:%d", TWS_FILE, line); printf("ASSERT-FAILD: " text " "); cpu_assert_debug(); \
    } } else if (!ok_) cpu_reset(); } while (0)

#define SDK_ASSERT_FMT(p, line, text, arg) do { _Bool ok_ = (p); if (config_asser) { if (!ok_) { \
    printf("file:%s, line:%d", TWS_FILE, line); printf("ASSERT-FAILD: " text, arg); cpu_assert_debug(); \
    } } else if (!ok_) cpu_reset(); } while (0)

extern struct tws_sync_channel tws_sync_channel_begin[], tws_sync_channel_end[];
extern const struct tws_func_stub tws_func_stub_begin[], tws_func_stub_end[];

/* ---- Globals -------------------------------------------------------- */

char tws_local_channel = 85;
u8 tws_auto_role_switch = 1;
u8 tws_common_addr_en = 1;
u8 tws_pair_all_way;
u8 low_latency_enable;
bool test_esco_role_switch_flag;
int g_spp_printf;
struct tws_connection tws_conn __attribute__((common));

static u8 pure_monitor_enable;
static _Bool tws_role_switch_test __attribute__((aligned(4)));
static u16 tws_detach_timer;
static u16 rs_timer;

static inline void cpu_reset(void) { p33_soft_reset(); }

static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

static inline void __list_del_entry(struct list_head *entry)
{
    struct list_head *prev = entry->prev, *next = entry->next;

    next->prev = prev;
    prev->next = next;
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
    struct list_head *prev = head->prev;

    head->prev = new;
    new->next = head;
    new->prev = prev;
    prev->next = new;
}

/* ---- Host hooks the application may override -------------------------- */

int TWS_WEAK tws_host_get_local_role(void) { return 0; }
int TWS_WEAK tws_host_channel_match(char channel) { return 1; }
int TWS_WEAK tws_host_addr_match(u8 *addr) { return 1; }
char TWS_WEAK tws_host_get_local_channel(void) { return tws_local_channel; }
void TWS_WEAK tws_host_get_common_addr(u8 *local_addr, u8 *remote_addr, char channel) { }
void TWS_WEAK tws_host_event_packet_handler(u8 *packet, int size) { }
int TWS_WEAK tws_host_get_core_data(u8 *packet, int size) { return 0; }
int TWS_WEAK tws_host_set_core_data(u8 *packet, int size) { return 0; }
int TWS_WEAK tws_host_get_core_data_tick(u8 *packet, int size) { return 0; }
int TWS_WEAK tws_host_set_core_data_tick(u8 *packet, int size) { return 0; }
int TWS_WEAK tws_host_fliter_out_acl_data(u16 handle, u8 llid, u8 *data) { return 0; }
void TWS_WEAK tws_user_data_handler(u8 *data, u16 len) { }

/* ---- Bulk and history helpers ----------------------------------------- */

struct bredr_tx_bulk *TWS_IRQ tws_tx_bulk_alloc(int size)
{
    struct bredr_tx_bulk *tx = bredr_tx_bulk_alloc(size);

    if (tx)
        tx->llid = 2;
    return tx;
}

void TWS_IRQ tws_conn_history_hash_add(int hash)
{
    u8 index = tws_conn.history_hash_index;

    tws_conn.history_hash[index] = hash;
    index++;
    tws_conn.history_hash_index = index == 16 ? 0 : index;
}

void TWS_CODE tws_conn_history_hash_clear(void)
{
    tws_conn.history_hash_index = 0;
    memset(tws_conn.history_hash, 0, sizeof(tws_conn.history_hash));
}

u8 TWS_CODE get_esco_wait_start_monitor(void)
{
    if (tws_conn.lmp_conn && tws_link.link) {
        if (tws_link.link->esco == NULL && (tws_conn.monitor_start & ~1) != 2)
            return 1;
    }
    return 0;
}

void TWS_IRQ tws_conn_send_event_check(void)
{
    if (tws_conn.send_event == 0 && list_empty(&tws_conn.lmp_conn->link->rx_head))
        tws_conn.send_event = 1;
}

void TWS_IRQ tws_conn_try_send_event_to_lmp(void)
{
    if (tws_conn.send_event == 2) {
        if (bredr_link_event(tws_conn.lmp_conn->link, 1, 5) == 0)
            tws_conn.send_event = 0;
    }
}

void TWS_CODE tws_link_conn_start_in_irq(struct link_connection *link, int role)
{
    struct lmp_connection *conn = lmp_conn_for_address(NULL);

    tws_pair_conntion_suss_irq(role);
    tws_link_conn_start(link);
    memset(&tws_conn, 0, sizeof(tws_conn));
    tws_conn.role = role;
    tws_conn.lmp_conn = conn;
    tws_conn.run_slot = 0x80;
    tws_conn.tx_tick_res = 1;
    tws_conn.slot_timer = slot_timer_get();
    tws_conn.sync_timer = 0xff;
    INIT_LIST_HEAD(&tws_conn.rx_pending);
    INIT_LIST_HEAD(&tws_conn.tx_pending);
    if (CONFIG_TWS_WORK_MODE != 1)
        pure_monitor_enable = 1;
    os_taskq_post_type("tws", 22, 1, &role);
    if (conn && conn->link)
        link_conn_qos_disable(conn->link);
    DEBUG("[Debug]: [TWS]tws_connection_start: role = %d\n\r\n", role);
}

void TWS_CODE tws_link_init(void)
{
    /* One 32-bit clear: esco_status, esco_setup_flag, esco_wait_monitor_start. */
    memset(&tws_conn.esco_status, 0, 4);
}

void TWS_CODE tws_conn_role_switch_test(void)
{
    tws_role_switch_test = 1;
}

int TWS_WEAK tws_role_switch_sync_profile_core_data_for_send(u8 *data, u16 len) { return 0; }
void TWS_WEAK tws_role_switch_sync_profile_core_data_for_set(u8 *data, u16 len) { }

void TWS_IRQ tws_conn_master_tx_start_req(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct bredr_tx_bulk *tx = tws_tx_bulk_alloc(367);
    struct tws_sync_channel *channel;
    u8 *data;
    int len;

    if (tx == NULL) {
        DEBUG("[Debug]: [TWS]master_tx_NULL\n\r\n");
        return;
    }
    tx->event_post = 43;
    tx->data[2] = 0;
    tx->data[3] = 0x80;
    tx->data[8] = 0;
    *(u32 *)&tx->data[4] = 0;
    if (tws_link.switch_packet_type == 1)
        tx->data[8] = 16;
    if (conn && conn->link) {
        switch (tws_conn.role_switch) {
        case 1:
            if (tws_conn.phone_link_detach & 1) {
                tws_conn.role_switch = 0;
                DEBUG("[Debug]: [TWS]role_switch: cancel\n\r\n");
            } else if (tws_conn.role_switch_lock == 0 && tws_conn.monitor_start == 2) {
                tws_conn.role_switch = 2;
                tx->data[8] |= 4;
                DEBUG("[Debug]: [TWS]role_switch: step1: %d, %d\n\r\n",
                      tws_link.link->core.max_slot, is_tws_esco_working());
            }
            break;
        case 0:
            if (tws_conn.phone_link_detach & 1) {
                tx->data[8] |= 1;
                tx->data[9] = tws_conn.detach_code;
                DEBUG("[Debug]: [TWS]phone_link_detach: step2\n\r\n");
            }
            break;
        }
        if (CONFIG_LOW_LATENCY_ENABLE && tws_conn.pure_monitor && tws_conn.exit_pure_monitor == 1) {
            int slots;

            tws_conn.exit_pure_monitor = 2;
            slots = tws_conn.tws_run_slot * 2 + tws_conn.phone_run_slot;
            *(u32 *)&tx->data[4] = bredr_offset2clkn(tws_link_read_slot_clk(), slots);
            tws_link.link->task.run_slot = slots;
        }
    }
    data = &tx->data[12];
    for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
        if (tws_conn.monitor_start != 2 && (channel->attribute & 2) == 0)
            continue;
        if (is_tws_esco_working() && (channel->attribute & 4) == 0)
            continue;
        if (tws_link.link->core.max_slot != 5 && (channel->attribute & 8) == 0)
            continue;
        if (channel->tx_sync_req == NULL)
            continue;
        len = channel->tx_sync_req(conn, data + 4, tws_conn.prev_req_err);
        if (len == 0)
            continue;
        *data = channel->channel;
        btctler_little_endian_store_16(data, 1, len);
        data += ((len + 7) / 4) << 2;
    }
    data += tws_host_get_core_data_tick(data, tx->data + 367 - data);
    tx->len = data - tx->data;
    *(u16 *)tx->data = tx->len - 4;
    SDK_ASSERT(tx->len <= 367, 1015, "tx->len <= EDR2DH3_SIZE");
    tws_conn.prev_req_err = 1;
    tws_conn.tx_tick_res++;
    link_conn_tx_bulk_avaiable(tws_link.link, tx);
}

u8 TWS_CODE check_tws_esco_resume_acl(void)
{
    if (tws_conn.lmp_conn && tws_conn.role == 1 && tws_conn.detach)
        return 1;
    return 0;
}

void TWS_CODE test_esco_role_switch(u8 flag)
{
    if (tws_conn.lmp_conn && tws_link.link && tws_conn.role == 0)
        *(u8 *)&test_esco_role_switch_flag = flag; /* SDK bool is a u8 typedef */
}

u8 TWS_CODE tws_esco_check_role_switch(struct bredr_esco_link *esco, u8 master_esco_num, u8 role, u16 handle)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;

    if (tws_conn.role != 0)
        return 0;
    if (conn && tws_conn.detach && (conn->link->monitor & 1) == 0) {
        INFO("[Info]: [TWS]esco_detch_role_switch\r\n");
        tws_conn_switch_role();
        esco->esco_try_switch = 0;
        return 1;
    }
    if (test_esco_role_switch_flag) {
        INFO("[Info]: [TWS]test_esco_role_switch_flag\n\r\n");
        tws_conn_switch_role();
        test_esco_role_switch_flag = 0;
        return 1;
    }
    return 0;
}

void TWS_CODE tws_conn_switch_role(void)
{
    local_irq_disable();
    if (tws_conn.role == 0 && tws_conn.role_switch == 0)
        tws_conn.role_switch = 1;
    local_irq_enable();
}

static void tws_master_role_switch_check(int packet_num, char rssi) TWS_CODE;
static int role_switch_by_power_check(void) TWS_CODE;
static void tws_conn_slave_rx_esco_monitor_start_req(void) TWS_CODE;
static inline __attribute__((always_inline)) int tws_pure_monitor_slots(void);
static inline __attribute__((always_inline)) void tws_exit_pure_monitor(void);
void sys_event_notify(struct sys_event *event);

void TWS_IRQ tws_core_tws_link_resume(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct tws_sync_channel *channel;
    int working;
    u8 res;

    tws_conn.tws_wait_tx = 0;
    if (conn && tws_conn.monitor_start == 2) {
        working = is_tws_esco_working();
        if (tws_link.post_instant == 0 && working == 0) {
            if (tws_conn.role == 0) {
                if (tws_conn.detach) {
                    if (tws_conn.pure_monitor) {
                        tws_conn.exit_pure_monitor = 1;
                        pure_monitor_enable = 0;
                    } else if (tws_conn.role_switch == 0) {
                        tws_conn.role_switch = 1;
                    }
                }
            } else if (tws_conn.detach == 1) {
                tws_conn.detach = 2;
                tws_conn_send_cmd(14, "V1", 47, tws_conn.detach_code);
                DEBUG("[Debug]: [TWS]detach_step2: tx_detach\n\r\n");
            }
            if (CONFIG_LOW_LATENCY_ENABLE && tws_conn.pure_monitor)
                tws_link.link->task.run_slot = tws_pure_monitor_slots();
            goto tick;
        }
        if (tws_conn.role == 0) {
            if (working == 0)
                goto tick;
            if (tws_link.acl_communication == 0)
                goto channels;
            tws_link.acl_communication = 0;
            if (tws_conn.role_switch == 7) {
                INFO("[Info]: [TWS]not tx esco_role_switching\r\n");
                goto channels;
            }
            if (link_conn_tx_empty(tws_link.link))
                goto tick;
            putchar('N');
            goto channels;
        }
        if (tws_conn.detach == 1) {
            if (tws_link.esco_wait_role_swtich) {
                tws_link.esco_wait_role_swtich--;
            } else {
                tws_conn.detach = 2;
                tws_conn_send_cmd(14, "V1", 47, tws_conn.detach_code);
                DEBUG("[Debug]: [TWS]detach_step2: tx_detach\n\r\n");
            }
        }
    } else if (conn && conn->esco_conn && tws_link.link) {
        if (tws_link.acl_communication == 0 || link_conn_tx_empty(tws_link.link) == 0)
            goto channels;
        tws_link.acl_communication = 0;
    }
tick:
    tws_conn.slave_rx_packet_num = 0xff;
    res = tws_conn.tx_tick_res;
    if (res == 0) {
        tws_conn.tick_req_pending = 0;
        tws_conn_master_tx_start_req();
    } else if (tws_conn.role == 0) {
        tws_conn.tick_req_pending = 1;
        if (conn && conn->sbc_open) {
            INFO("[Info]: [TWS]tx: %d\n\r\n", res);
            tws_conn.try_switch = 0;
        }
    }
channels:
    for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
        if (channel->event_handler)
            channel->event_handler(conn, 20, 0);
    }
}

int TWS_CODE tws_conn_send_cmd(u8 opcode, const char *format, ...)
{
    va_list argptr;
    struct bredr_tx_bulk *tx;
    u16 length = 0;

    tx = tws_tx_bulk_alloc(24);
    if (tx == NULL) {
        ERROR("<Error>: [TWS]tws_send_cmd: nomem\n\r\n");
        return -12;
    }
    tx->len = 4;
    tx->data[2] = 0;
    tx->data[3] = opcode;
    if (format) {
        va_start(argptr, format);
        for (;; format++) {
            if (*format == 'v')
                tx->event_prob = va_arg(argptr, int);
            else if (*format == 'V')
                tx->event_post = va_arg(argptr, int);
            else
                break;
        }
        tx->len += __vsprintf(&tx->data[4], format, argptr);
        va_end(argptr);
        length = tx->len - 4;
    }
    *(u16 *)tx->data = length;
    link_conn_tx_bulk_avaiable(tws_link.link, tx);
    return 0;
}

void TWS_IRQ tws_core_tws_link_suspend(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct tws_sync_channel *channel;
    int period, offset;

    if (conn && tws_conn.monitor_start == 2) {
        if (tws_conn.role == 0) {
            tws_conn.channels = 0;
            if (conn->clear_a2dp_packet_num &&
                bredr_clkn2offset(tws_link.link->super_timeout_begin, READ_SLOT_CLK()) > 299)
                __lmp_private_clear_a2dp_packet(&conn->rejust_esco_req, conn->clear_a2dp_packet_num);
            tws_master_role_switch_check(tws_conn.slave_rx_packet_num, tws_conn.slave_plink_rssi);
        }
        if (CONFIG_LOW_LATENCY_ENABLE && tws_conn.pure_monitor) {
            period = tws_conn.phone_run_slot + tws_conn.tws_run_slot;
            offset = tws_conn.pure_monitor_mod - tws_link_read_slot_clk() % period;
            if (offset < 0)
                offset += period;
            conn->link->task.run_slot = offset > 4 ? offset : 4;
        }
    }
    for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
        if (channel->event_handler)
            channel->event_handler(conn, 21, 0);
    }
}

static void tws_master_role_switch_check(int packet_num, char rssi)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct link_connection *plink;
    int tws_rssi, plink_rssi, remain, hi, low;
    u8 count;

    if (!tws_auto_role_switch || tws_conn.monitor_start != 2 || tws_conn.role_switch ||
        tws_conn.phone_link_detach || low_latency_enable || conn->esco_conn ||
        tws_link.switch_add_pkg || conn->sbc_open == 0) {
        tws_conn.try_switch = 0;
        return;
    }
    if (tws_role_switch_test) {
        tws_role_switch_test = 0;
        tws_conn.role_switch = 1;
        return;
    }
    if (packet_num == 255)
        goto reset;
    tws_rssi = tws_link.link->rx_rssi;
    plink = tws_link.plink;
    plink_rssi = plink->rx_rssi;
    remain = lmp_private_get_sbc_remain_time(&conn->rejust_esco_req, 0);
    hi = lmp_get_a2dp_data_cache_hi(conn);
    low = lmp_get_a2dp_data_cache_low(conn);
    if (tws_rssi < -84)
        goto reset;
    if (remain > CONFIG_A2DP_DELAY_TIME + 70)
        goto reset;
    if (CONFIG_TWS_WORK_MODE == 1) {
        if (conn->link->rx_state == 71) {
            role_switch_by_power_check();
            goto rssi_check;
        }
    } else {
        if (tws_conn.pure_monitor == 0)
            goto reset;
        if (tws_link.ingore_add_pkg == 0 && role_switch_by_power_check())
            goto reset;
    }
    if (remain < 60 || remain >= (low + hi) / 2) {
        tws_conn.try_switch = 0;
    } else {
        count = tws_conn.try_switch;
        if (packet_num > 4 && plink->rx_packet_num + 2 < packet_num) {
            tws_conn.try_switch = ++count;
            if (count > 2) {
                tws_conn.role_switch = 1;
                goto reset;
            }
        } else if (count) {
            tws_conn.try_switch = count - 1;
        }
    }
rssi_check:
    if (rssi > -70 && plink_rssi < -58 && plink_rssi + 15 <= rssi) {
        if (++tws_conn.try_switch_2 > 3) {
            tws_conn.role_switch = 1;
            goto reset;
        }
        return;
    }
    if (tws_conn.try_switch_2)
        tws_conn.try_switch_2--;
    return;
reset:
    tws_conn.try_switch = 0;
    tws_conn.try_switch_2 = 0;
}

void TWS_CODE start_monitor_creat_esco(void *p)
{
    struct lmp_connection *conn;
    struct link_connection *link;

    INFO("[Info]: [TWS]start_monitor_creat_esco\n\r\n");
    conn = tws_conn.lmp_conn;
    if (conn == NULL || conn->core.esco_state == 4)
        return;
    conn->core.esco_state = 4;
    link = conn->link;
    if (link->monitor & 1) {
        if (conn->esco_conn)
            tws_post_open_esco_link(&conn->rejust_esco_req);
    } else if (link->esco) {
        tws_post_open_after_tws_esco_link(&conn->rejust_esco_req);
    }
}

void TWS_CODE tws_post_open_esco_link(void *p)
{
    struct lmp_connection *conn = p;
    u8 timer;

    if (conn == NULL || conn->esco_conn == NULL || tws_link.link == NULL)
        return;
    tws_conn.esco_setup_flag = 1;
    tws_link.post_step = LMP_TX_NULL;
    tws_link.post_instant = 0;
    tws_conn.role_switch = 0;
    timer = tws_link.slot_instant_timer;
    if (timer) {
        slot_timer_put(timer);
        tws_link.slot_instant_timer = 0;
    }
    if (CONFIG_LOW_LATENCY_ENABLE && tws_conn.pure_monitor) {
        INFO("[Info]: [TWS]esco_exit_pure_monitor\n\r\n");
        tws_exit_pure_monitor();
        tws_esco_monitor_init(tws_conn.lmp_conn->link);
    }
    link_task_run_slots(&tws_conn.lmp_conn->link->task, tws_conn.role == 0 ? 12 : 18);
    if (conn->core.esco_state == 4) {
        lmp_event_esco_link_setup(conn);
        tws_conn.lmp_conn->link->adjust_esco_winsize = 1;
    } else {
        bredr_link_event(conn->link, 1, 9);
    }
    INFO("[Info]: [TWS]tws_post_open_esco_link\n\r\n");
}

void TWS_CODE tws_post_open_after_tws_esco_link(void *p)
{
    struct lmp_connection *conn = p;
    struct lmp_esco *esco;
    u8 timer;

    if (conn == NULL)
        return;
    esco = conn->esco_conn;
    if (esco == NULL || tws_link.link == NULL)
        return;
    tws_conn.esco_setup_flag = 1;
    tws_link.post_step = LMP_TX_NULL;
    tws_link.post_instant = 0;
    timer = tws_link.slot_instant_timer;
    if (timer) {
        slot_timer_put(timer);
        tws_link.slot_instant_timer = 0;
    }
    conn->link->esco->pdu.tws_lt_addr = esco->pdu.tws_lt_addr;
    bredr_link_event(conn->link, 1, 10);
    tws_conn_send_event(13, "11", tws_conn.role, 1);
    INFO("[Info]: [TWS]after_bredr_esco_tws_link_open =0x%x,%x\n\r\n", conn, esco->pdu.tws_lt_addr);
}

/* Inlined into tws_master_start_monitor in the SDK (lines 1563, 1587). */
static inline __attribute__((always_inline)) void __start_monitor_by_instant(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;

    if (conn->esco_conn && conn->core.esco_state)
        return;
    if (conn->sniff && (conn->sniff->state & 1))
        return;
    if (is_tws_esco_working() || tws_link.post_instant)
        return;
    if (tws_link.link->state == 2)
        link_task_run(&conn->link->task);
}

static inline __attribute__((always_inline)) void tws_master_tx_start_esco_monitor_req(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;

    if (conn == NULL)
        return;
    conn->core.esco_state = 3;
    start_monitor_creat_esco(NULL);
}

void TWS_CODE tws_master_start_monitor(void *p)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;

    slot_timer_disable(tws_conn.slot_timer);
    switch (tws_conn.monitor_start) {
    case 1:
        DEBUG("[Debug]: [TWS]master_start_monitor_faild\n\r\n");
        slot_timer_set(tws_conn.slot_timer, tws_master_start_monitor, NULL, 128);
        break;
    case 3:
        tws_conn.monitor_start = 2;
        tws_link.link->task.run_slot = CONFIG_TWS_RUN_SLOT;
        conn->link->task.run_slot = CONFIG_PHONE_RUN_SLOT;
        tws_link_master_start_monitor();
        if (conn->core.esco_state == 1)
            tws_master_tx_start_esco_monitor_req();
        os_taskq_post_type("tws", 15, 0, NULL);
        tws_conn_send_event(15, "1", 0);
        INFO("[Info]: [TWS]master_start_monitor: %x\n\r\n", tws_link_read_slot_clk());
        break;
    case 2:
        __start_monitor_by_instant();
        break;
    }
}

void TWS_CODE tws_conn_send_event(int code, const char *format, ...)
{
    va_list argptr;
    struct sys_event event;

    event.type = 16;
    event.arg = (void *)1415009024;
    event.u.bt.event = code;
    if (format) {
        va_start(argptr, format);
        __vsprintf(event.u.bt.args, format, argptr);
        va_end(argptr);
    }
    sys_event_notify(&event);
}

void TWS_CODE tws_conn_slave_start_monitor(void *p)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct link_connection *link = conn->link;

    slot_timer_disable(tws_conn.slot_timer);
    if (tws_conn.monitor_start != 1)
        return;
    tws_conn.monitor_start = 2;
    tws_link.link->task.run_slot = CONFIG_TWS_RUN_SLOT;
    link->task.run_slot = CONFIG_PHONE_RUN_SLOT;
    link->task.sta_attribute = 5;
    link_task_add(&link->task);
    link_task_idle_disable();
    tws_link_slave_start_monitor();
    os_taskq_post_type("tws", 15, 0, NULL);
    tws_conn_send_event(15, "1", 1);
    if (conn->core.esco_state == 1)
        tws_conn_slave_rx_esco_monitor_start_req();
}

static void tws_conn_slave_rx_esco_monitor_start_req(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;

    if (conn == NULL)
        return;
    conn->core.esco_state = 3;
    start_monitor_creat_esco(NULL);
}

/* TWS slots before the next pure-monitor phone window; the SDK repeats this
 * calculation in several functions. */
static inline __attribute__((always_inline)) int tws_pure_monitor_slots(void)
{
    int period = tws_conn.phone_run_slot + tws_conn.tws_run_slot;
    int offset = tws_conn.pure_monitor_mod - tws_link_read_slot_clk() % period;
    int slot;

    if (offset < 0) {
        slot = offset + period - tws_conn.phone_run_slot;
        return slot > 4 ? slot : 4;
    }
    return offset > tws_conn.tws_run_slot ? 4 : tws_conn.tws_run_slot + offset;
}

static inline __attribute__((always_inline)) void tws_set_low_latency_slots(u8 enable)
{
    if (enable) {
        tws_conn.tws_run_slot = CONFIG_TWS_LOW_LATENCY_RUN_SLOT;
        tws_conn.phone_run_slot = CONFIG_PHONE_LOW_LATENCY_RUN_SLOT;
    } else {
        tws_conn.tws_run_slot = 32;
        tws_conn.phone_run_slot = CONFIG_PHONE_LOW_LATENCY_RUN_SLOT + CONFIG_TWS_LOW_LATENCY_RUN_SLOT - tws_conn.tws_run_slot;
    }
}

void TWS_CODE tws_master_tx_enter_pure_monitor_req(void)
{
    int instant = bredr_offset2clkn(tws_link_read_slot_clk(), 32);
    u8 enable = low_latency_enable;

    tws_set_low_latency_slots(enable);
    tws_conn_send_cmd(22, "V411", 65, instant, enable, tws_link.plink->frame->rxtog & 2);
    tws_link.plink->task.run_slot = tws_conn.phone_run_slot;
    tws_link.link->task.run_slot = tws_conn.tws_run_slot;
    link_task_run_slots(&tws_link.link->task, 200);
    tws_conn.pure_monitor = 1;
    tws_conn.pure_monitor_mod = instant % (tws_conn.tws_run_slot + tws_conn.phone_run_slot);
    INFO("[Info]: [TWS]tx_enter_pure_monitor_req: %x, %d, %d\n\r\n",
         instant, tws_conn.pure_monitor_mod, low_latency_enable);
}

void TWS_CODE tws_master_enter_pure_monitor(void)
{
    if (CONFIG_TWS_WORK_MODE != 2 && tws_link.link->bt_modulaion_mode == 0)
        link_conn_set_ptt(tws_link.link, 0);
    tws_link_master_pure_monitor_init();
    link_task_run_slots(&tws_link.link->task, tws_pure_monitor_slots());
    INFO("[Info]: [TWS]enter_pure_monitor: %d\n\r\n", tws_link_read_slot_clk());
}

void TWS_CODE tws_slave_enter_pure_monitor_mode(u8 *data)
{
    int instant = *(int *)data;
    int period, offset;

    low_latency_enable = data[4];
    tws_set_low_latency_slots(data[4]);
    period = tws_conn.tws_run_slot + tws_conn.phone_run_slot;
    tws_conn.pure_monitor_mod = instant % period;
    offset = bredr_clkn2offset(tws_link_read_slot_clk(), instant);
    while (offset <= 2) {
        instant = bredr_offset2clkn(instant, period);
        offset = bredr_clkn2offset(tws_link_read_slot_clk(), instant);
    }
    if (data[5])
        tws_link.plink->frame->rxtog |= 2;
    else
        tws_link.plink->frame->rxtog &= ~2;
    if (CONFIG_TWS_WORK_MODE != 2 && tws_link.link->bt_modulaion_mode == 0)
        link_conn_set_ptt(tws_link.link, 0);
    pure_monitor_enable = 1;
    tws_conn.pure_monitor = 1;
    tws_link_slave_pure_monitor_init();
    link_task_run_slots(&tws_link.link->task, offset);
    tws_link.plink->task.run_slot = tws_conn.phone_run_slot;
    INFO("[Info]: [TWS]rx_enter_pure_monitor_req: %x, %x, %d, %d, %d\n\r\n",
         *(int *)data, instant, offset, low_latency_enable, tws_conn.pure_monitor_mod);
}

void TWS_CODE tws_master_tx_exit_pure_monitor_req(void)
{
    struct ctrl_frame *frame = tws_link.plink->frame;

    tws_conn_send_cmd(23, "42211", tws_conn.history_hash[0], frame->rxstat0, frame->rxstat1,
                      frame->rxtog, pure_monitor_enable);
    DEBUG("[Debug]: [TWS]tx_exit_pure_monitor_req: %x\n\r\n", tws_conn.history_hash[0]);
}

void TWS_CODE tws_slave_rx_exit_pure_monitor_req(u8 *data)
{
    int hash = *(int *)data;
    int slots;

    if (hash == 0 || tws_conn.history_hash[0] == hash) {
        tws_conn_send_cmd(23, "V41", 67, bredr_offset2clkn(tws_link_read_slot_clk(), 64), 1);
        tws_conn.exit_pure_monitor = 0;
        tws_conn.pure_monitor = 0;
        pure_monitor_enable = data[9];
        tws_link_slave_exit_pure_monitor();
        slots = 64;
    } else {
        tws_conn_send_cmd(23, "41", bredr_offset2clkn(tws_link_read_slot_clk(), 32), 0);
        slots = 32;
    }
    link_task_run_slots(&tws_link.link->task, slots);
    DEBUG("[Debug]: [TWS]rx_exit_pure_monitor_req: %x, %x\n\r\n", tws_conn.history_hash[0], hash);
}

void TWS_CODE tws_master_rx_exit_pure_monitor_req(u8 *data)
{
    int offset = bredr_clkn2offset(tws_link_read_slot_clk(), *(int *)data);

    if (data[4] == 1) {
        tws_conn.exit_pure_monitor = 0;
        tws_conn.pure_monitor = 0;
        if (CONFIG_TWS_WORK_MODE != 2 && tws_link.link->bt_modulaion_mode == 0)
            link_conn_set_ptt(tws_link.link, 1);
        tws_link_master_exit_pure_monitor();
        if (tws_conn.detach)
            tws_conn_switch_role();
        if (tws_conn.role_switch == 1)
            tws_conn.role_switch = 2;
    } else {
        tws_conn.history_hash[0] = 0;
        tws_conn.exit_pure_monitor = 1;
    }
    link_task_run_slots(&tws_link.link->task, offset);
    DEBUG("[Debug]: [TWS]rx_exit_pure_monitor_req: %d\n\r\n", offset);
}

int TWS_CODE tws_core_enter_low_latency_mode(void)
{
    local_irq_disable();
    if (tws_conn.pure_monitor == 0)
        pure_monitor_enable = 1;
    low_latency_enable = 1;
    tws_conn.tws_run_slot = CONFIG_TWS_LOW_LATENCY_RUN_SLOT;
    tws_conn.phone_run_slot = CONFIG_PHONE_LOW_LATENCY_RUN_SLOT;
    local_irq_enable();
    return 0;
}

int TWS_CODE tws_core_exit_low_latency_mode(void)
{
    local_irq_disable();
    low_latency_enable = 0;
    tws_conn.tws_run_slot = 32;
    tws_conn.phone_run_slot = CONFIG_PHONE_LOW_LATENCY_RUN_SLOT + CONFIG_TWS_LOW_LATENCY_RUN_SLOT - tws_conn.tws_run_slot;
    if (CONFIG_TWS_WORK_MODE == 1) {
        pure_monitor_enable = 0;
        if (tws_conn.monitor_start == 2 && tws_conn.pure_monitor)
            tws_conn.exit_pure_monitor = 1;
    }
    local_irq_enable();
    return 0;
}

int TWS_CODE tws_core_low_latency_enable(void)
{
    return low_latency_enable;
}

void tws_host_timeout_set(void);
void tws_host_timeout_del_event(void);
u8 cheack_tws_connect_state();
int os_sem_create(OS_SEM *sem, int count);
int os_sem_pend(OS_SEM *sem, int timeout);
static void check_for_setup_monitor_link(void *p) TWS_CODE;

void TWS_CODE tws_phone_link_state_change(struct lmp_connection *conn, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    if (conn->core.role == 0 && rx && conn->core.stage == LMP_STAGE_CONN &&
        (rx->data[0] & ~1) == 6 && rx->data[1] == 51)
        conn->core.role_switch = 1;
    if (tws_conn.monitor_start == 2)
        tws_conn.lmp_core_data_sync = 1;
}

void TWS_CODE tws_phone_link_connection(struct lmp_connection *conn)
{
    INFO("[Info]: [TWS]tws_phone_link_connection\n\r\n");
    local_irq_disable();
    if (tws_conn.detach || tws_link.link == NULL) {
        local_irq_enable();
        return;
    }
    tws_conn.lmp_conn = conn;
    INFO("[Info]: [TWS]check_for_setup_monitor_0\n\r\n");
    if (tws_conn.monitor_start == 0) {
        tws_host_timeout_set();
        slot_timer_set(tws_conn.slot_timer, check_for_setup_monitor_link, &conn->rejust_esco_req, 4);
    }
    local_irq_enable();
}

/* The SDK's static tws_tx_setup_monitor_link_req (line 2277), inlined into its
 * only caller: send the peer the phone link's state so it can monitor it.
 * Returns nonzero when no buffer was available. */
static inline __attribute__((always_inline)) int tws_tx_setup_monitor_link_req(struct lmp_connection *conn)
{
    struct bredr_tx_bulk *tx = tws_tx_bulk_alloc(1358);
    struct lmp_esco *esco;

    if (tx == NULL)
        return 1;
    tx->data[2] = 0;
    tx->data[3] = 6;
    tx->len = 104;
    if (conn->esco_conn && conn->link->esco) {
        INFO("[Info]: [TWS]tws_tx_setup_monitor_get_esco_pdu=0x%x\n\r\n", conn);
        esco = conn->esco_conn;
        conn->core.esco_state = 1;
        esco->pdu.tws_lt_addr = esco_creart_lt_addr();
        bredr_link_event(conn->link, 2, 6, 1);
        if (log_tag_const_i_TWS) {
            log_print(2, NULL, "[Info]: [TWS]pdu->tws_lt_addr=0x%x\n\r\n", esco->pdu.tws_lt_addr);
            if (esco->pdu.tws_lt_addr > 7)
                log_print(2, NULL, "[Info]: [TWS]<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<pdu->tws_lt_addr err>>>>>>>>>>>>>>>>>>>\n\r\n");
        }
        memcpy(&tx->data[88], &esco->pdu.handle, 14);
    } else {
        conn->core.esco_state = 0;
    }
    memcpy(&tx->data[4], &conn->core, 44);
    memcpy(&tx->data[48], &conn->core_s, 40);
    conn->core.operation_mode = 0;
    conn->core.operation_mode_sync = 0;
    tws_link_set_plink(conn->link);
    if (conn->core.esco_state == 0)
        tws_link_monitor_master_init(conn->link->frame);
    tx->len += tws_link_master_get_plink_core_data(&tx->data[104]);
    tx->len += tws_host_get_core_data(&tx->data[tx->len], 1358 - tx->len);
    if (config_asser) {
        if (tx->len > 1358) {
            printf("file:%s, line:%d", TWS_FILE, 2341);
            printf("ASSERT-FAILD: tx->len <= max_len %x\n", tx->len);
            cpu_assert_debug();
        }
    } else if (tx->len > 1358) {
        cpu_reset();
    }
    *(u16 *)tx->data = tx->len - 4;
    link_conn_tx_bulk_avaiable(tws_link.link, tx);
    return 0;
}

/* Slot timer callback that waits for the phone link to be idle before the
 * monitor link is requested from the peer. */
static void check_for_setup_monitor_link(void *p)
{
    struct lmp_connection *conn = p;
    int slots;

    if (tws_conn.monitor_start != 0 || (conn->core.state & 8) != 0) {
        INFO("[Info]: [TWS]check_disable: %x\n\r\n", conn->core.state);
        slot_timer_disable(tws_conn.slot_timer);
        return;
    }
    if (conn->sniff && conn->core.operation_mode == 2)
        lmp_tx_unsniff_req(conn);
    if (cheack_tws_connect_state() && conn->core.operation_mode == 0) {
        if (lmp_conn_idle(conn) == 0) {
            slots = 8;
            goto reset;
        }
        tws_host_timeout_del_event();
        tws_conn.monitor_start = 1;
        conn->link->task.run_slot = 64;
        tws_conn.instant = bredr_offset2clkn(tws_link_read_slot_clk(), 64);
        if (tws_tx_setup_monitor_link_req(conn)) {
            tws_conn.monitor_start = 0;
            slot_timer_reset(tws_conn.slot_timer, 16);
            return;
        }
        link_conn_set_max_rx_bulk_persent(conn->link, CONFIG_MASTER_MAX_RX_BULK_PERSENT);
        if (conn->core.esco_state == 0) {
            if (low_latency_enable) {
                link_task_run_slots(&tws_link.link->task, 16);
                tws_link.link->task.run_slot = 16;
            } else {
                link_task_run_slots(&tws_link.link->task, 64);
            }
        }
        slot_timer_disable(tws_conn.slot_timer);
        INFO("[Info]: [TWS]master: tx_setup_monitor: %x, %x, %d\n\r\n",
             tws_conn.instant, conn->core.esco_state, conn->sbc_channel);
        return;
    }
    putchar('w');
    if (conn->core.operation_mode)
        putchar('M');
    slots = 64;
reset:
    slot_timer_reset(tws_conn.slot_timer, slots);
}

void TWS_CODE tws_phone_link_detach(struct lmp_connection *conn, u8 code)
{
    OS_SEM *sem;

    if (tws_link.link == NULL || tws_conn.monitor_start == 0)
        return;
    DEBUG("[Debug]: [TWS]lmp_phone_link_detach: %x\n\r\n", tws_conn.phone_link_detach);
    sem = malloc(80);
    SDK_ASSERT(sem != NULL, 2065, "sem");
    os_sem_create(sem, 0);
    local_irq_disable();
    tws_conn.sem_phone_detach = sem;
    tws_conn.phone_link_detach |= 1;
    tws_conn.detach_code = code;
    local_irq_enable();
    if (tws_link.link && tws_conn.monitor_start == 2) {
        os_sem_pend(sem, 0);
        tws_conn.sem_phone_detach = NULL;
    }
    free(sem);
}

void TWS_CODE tws_phone_link_del(u8 code)
{
    tws_conn.lmp_conn = NULL;
    if (tws_link.link)
        tws_conn_send_event(6, "111", tws_conn.role, 0, code);
}

int TWS_CODE tws_phone_link_sbc_data_len(int channel)
{
    return 0;
}

int TWS_CODE tws_phone_link_abandon_sbc_data(struct lmp_connection *conn, u16 seqn_number)
{
    return -1;
}

int TWS_CODE tws_phone_link_rx_pend_empty(void)
{
    if (tws_conn.monitor_start == 0)
        return 1;
    return list_empty(&tws_conn.rx_pending);
}

/* Drop every queued phone-link packet (open-coded twice in the SDK). */
static inline __attribute__((always_inline)) void tws_free_rx_pending(void)
{
    struct list_head *node, *next;

    for (node = tws_conn.rx_pending.next; node != &tws_conn.rx_pending; node = next) {
        next = node->next;
        __list_del_entry(node);
        bredr_rx_bulk_free((struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry)));
    }
}

/* Leave pure-monitor mode on both sides of the shared phone link. */
static inline __attribute__((always_inline)) void tws_exit_pure_monitor(void)
{
    tws_conn.pure_monitor = 0;
    if (CONFIG_TWS_WORK_MODE != 2 && tws_link.link->bt_modulaion_mode == 0)
        link_conn_set_ptt(tws_link.link, 1);
    if (tws_conn.role == 0)
        tws_link_master_exit_pure_monitor();
    else
        tws_link_slave_exit_pure_monitor();
}

void TWS_CODE do_phone_link_detach(void *priv)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;

    DEBUG("[Debug]: [TWS]do_phone_link_detach: %x\n\r\n", tws_conn.phone_link_detach);
    slot_timer_put((u8)(int)priv);
    slot_timer_disable(tws_conn.slot_timer);
    if (CONFIG_LOW_LATENCY_ENABLE && tws_conn.pure_monitor)
        tws_exit_pure_monitor();
    if (tws_conn.phone_link_detach & 1) {
        if (tws_conn.sem_phone_detach)
            os_sem_post(tws_conn.sem_phone_detach);
    } else if (conn && conn->link) {
        bredr_link_event(conn->link, 2, 14, tws_conn.detach_code);
        lmp_private_clear_a2dp_packet(&conn->rejust_esco_req, 0);
    }
    tws_free_rx_pending();
    tws_conn.monitor_start = 0;
    tws_conn.phone_link_detach = 0;
    tws_conn.phone_link_detach_instant = 0;
    tws_conn.esco_super_timeout = 0;
    tws_link_plink_detach();
}

int syscfg_read(u16 key, void *data, u16 len);
static void __tws_conn_role_switch() TWS_CODE;

void TWS_CODE tws_detach_timer_del(void)
{
    u16 timer = tws_detach_timer;

    if (timer) {
        sys_timeout_del(timer);
        tws_detach_timer = 0;
    }
}

void TWS_CODE reset_local_channle(void)
{
    u8 channel = 0;

    if (syscfg_read(98, &channel, 1) < 0)
        tws_api_set_local_channel(85);
    else
        tws_api_set_local_channel(channel);
}

int TWS_CODE tws_api_remove_pairs_internal(void)
{
    if (tws_link.link == NULL)
        return -22;
    if (tws_conn.monitor_start == 0)
        tws_conn_send_cmd(8, "V", 48);
    return 0;
}

void TWS_CODE tws_esco_remove_clear_state(void)
{
    memset(&tws_conn.esco_status, 0, 4);
    tws_link.post_step = LMP_TX_NULL;
    tws_link.post_instant = 0;
    tws_link.esco_rx_lmp_slot_clk = 0;
    tws_link.esco_rx_lmp_cmd = 0;
}

void TWS_CODE tws_api_role_switch(void)
{
    local_irq_disable();
    if (tws_link.link == NULL || tws_conn.role == 1 || tws_conn.role_switch) {
        local_irq_enable();
        return;
    }
    if (lmp_conn_for_address(NULL)) {
        tws_conn.role_switch = 1;
    } else {
        link_inquiry_disable();
        link_inquiry_scan_disable();
        link_page_disable(254);
        link_page_scan_disable();
        DEBUG("[Debug]: [TWS]tx_role_switch_req\n\r\n");
        link_task_set_sleep_slot(16);
        tws_conn_send_cmd(4, "4", bredr_offset2clkn(tws_link_read_slot_clk(), 128));
        slot_timer_set(tws_conn.slot_timer, __tws_conn_role_switch, NULL, 128);
    }
    local_irq_enable();
}

static void tws_rx_role_switch_req(u8 *data) TWS_CODE;
static void tws_conn_master_2_slave(void *p) TWS_CODE;
static void tws_conn_slave_2_master(void *p) TWS_CODE;
static void tws_conn_adjust_clkoffset_enable(void *p) TWS_CODE;
static void tws_cmd_role_switch(u8 *data) TWS_CODE;
static void tws_rx_setup_monitor_link_req(u8 *data, int len) TWS_CODE;
static void tws_conn_slave_rx_monitor_start_req(u8 *data) TWS_CODE;
static void tws_slave_rx_role_switch_req(u8 *data) TWS_CODE;
static void tws_master_rx_role_switch_res(u8 *data) TWS_CODE;
static inline void tws_function_call_by_id(int id, u8 *data, u16 len, u8 rx);
static void tws_slave_esco_step_start(void *p) TWS_CODE;
void tws_esco_wait_post_role_switch(void *p) TWS_CODE;
static void tws_core_rx_jiffies_offset(u32 offset) TWS_CODE;
static void tws_core_tx_jiffies_offset(void) TWS_CODE;
static void tws_esco_tx_role_switch_req(void) TWS_CODE;
static void tws_master_tx_start_monitor_req(void) TWS_CODE;
int tws_core_rx_bulk_full_handler(int size) TWS_IRQ;

/* Outside TWS sniff, a TWS link that has the role bit set goes back to
 * 5-slot packets (repeated in both tick handlers in the SDK). */
static inline __attribute__((always_inline)) void tws_resume_max_slot(void)
{
    if (!tws_sniff.enable && tws_link.link && tws_link.link->role) {
        if (tws_link.link->core.max_slot != 5) {
            INFO("[Info]: [TWS]-resume_max_slot\n\r\n");
            sniff_test_tone = 0;
        }
        tws_link.link->core.max_slot = 5;
    }
}

/* The SDK's static tws_conn_slave_rx_tick_req (line 1025): answer the
 * master's tick with this side's sync channel data. */
static inline __attribute__((always_inline)) void tws_conn_slave_rx_tick_req(u8 *data, int len)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct bredr_tx_bulk *tx;
    struct tws_sync_channel *channel;
    struct link_connection *link;
    u8 *rx, *out;
    u16 rx_len;
    int ret, offset;
    u8 flags;

    tws_resume_max_slot();
    while ((tx = tws_tx_bulk_alloc(367)) == NULL) {
        if (tws_core_rx_bulk_full_handler(367)) {
            ERROR("<Error>: [TWS]no_bulk_for_tick_res: %d\n\r\n", tws_link.link->state);
            return;
        }
    }
    tx->event_post = 44;
    tx->data[2] = 0;
    tx->data[3] = 0x81;
    tx->data[4] = 0;
    if (tws_conn.monitor_start == 2) {
        link = conn->link;
        tx->data[7] = link->rx_state;
        tx->data[6] = link->rx_packet_num;
        tx->data[8] = link->rx_rssi;
        tws_conn.role = 1;
        if (CONFIG_LOW_LATENCY_ENABLE && *(u32 *)(data + 4)) {
            offset = bredr_clkn2offset(tws_link_read_slot_clk(), *(u32 *)(data + 4));
            link_task_run_slots(&tws_link.link->task, offset > 2 ? offset : 2);
        }
    }
    flags = data[8];
    if (flags & 16)
        tws_link.switch_packet_type = 1;
    if (flags & 4) {
        tws_conn.role_switch = 5;
        if (is_tws_esco_working() && conn->link->esco)
            conn->link->esco->esco_acl_resume_t = 16;
        link_task_try_run_slots(&tws_link.link->task, 64);
        DEBUG("[Debug]: [TWS]role_switch: step2: %d, %d\n\r\n",
              tws_link.link->core.max_slot, is_tws_esco_working());
    } else if (!is_tws_esco_working()) {
        tws_conn.role_switch = 0;
    }
    if (data[8] & 1)
        tws_conn.detach_code = data[9];
    if (tws_conn.role_switch == 0 && tws_conn.phone_link_detach == 1) {
        tx->data[5] = tws_conn.detach_code;
        tx->data[4] = 1;
    }
    rx = data + 12;
    out = &tx->data[12];
    for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
        if (is_tws_esco_working() && (channel->attribute & 4) == 0)
            continue;
        if (tws_link.link->core.max_slot != 5 && (channel->attribute & 8) == 0)
            continue;
        if (rx - data < len && channel->channel == *rx) {
            rx_len = btctler_little_endian_read_16(rx, 1);
            SDK_ASSERT_FMT(channel->rx_sync_req_and_res != NULL, 1137,
                           "sync_ch->rx_sync_req_and_res channel: %d\n", channel->channel);
            ret = channel->rx_sync_req_and_res(conn, rx + 4, rx_len, out + 4);
            rx += (rx_len + 7) & ~3;
        } else {
            if ((channel->attribute & 1) == 0)
                continue;
            if ((channel->attribute & 2) == 0 && tws_conn.monitor_start != 2)
                continue;
            ret = channel->rx_sync_req_and_res(conn, NULL, 0, out + 4);
        }
        if (ret == 0)
            continue;
        *out = channel->channel;
        btctler_little_endian_store_16(out, 1, ret);
        out += ((ret + 7) / 4) << 2;
    }
    len -= rx - data;
    if (len)
        tws_host_set_core_data_tick(rx, len);
    tx->len = out - tx->data;
    SDK_ASSERT_FMT(tx->len <= 367, 1161, "tx->len <= EDR2DH3_SIZE %x\n", tx->len);
    *(u16 *)tx->data = tx->len - 4;
    bredr_tx_bulk_realloc(tx, tx->len);
    link_conn_tx_bulk_avaiable(tws_link.link, tx);
}

/* The SDK's static tws_conn_master_rx_tick_res (line 1212). */
static inline __attribute__((always_inline)) void tws_conn_master_rx_tick_res(u8 *data, int len)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct tws_sync_channel *channel;
    struct link_connection *link;
    u8 *rx, old;
    u16 rx_len;

    if (CONFIG_LOW_LATENCY_ENABLE && tws_conn.exit_pure_monitor)
        link_task_run_slots(&tws_link.link->task, tws_conn.tws_run_slot * 2 + tws_conn.phone_run_slot);
    tws_resume_max_slot();
    if ((data[4] & 1) && tws_conn.monitor_start) {
        tws_conn.detach_code = data[5];
        old = tws_conn.phone_link_detach;
        tws_conn.phone_link_detach = old | 2;
        if (old & 1)
            bredr_link_event(conn->link, 2, 14, data[5]);
    }
    if (is_tws_esco_working()) {
        tws_esco_tx_role_switch_req();
    } else {
        tws_conn.slave_rx_packet_num = data[6];
        tws_conn.slave_plink_rssi = data[8];
        if (conn && (link = conn->link)) {
            link->slave_rx_state = data[7];
            if (data[7] == 66 && link->rx_state == 71)
                link_conn_qos_disable(link);
        }
    }
    rx = data + 12;
    for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
        if (rx - data >= len)
            break;
        if (channel->channel != *rx)
            continue;
        rx_len = btctler_little_endian_read_16(rx, 1);
        if (channel->rx_sync_res == NULL)
            continue;
        channel->rx_sync_res(conn, rx + 4, rx_len);
        rx += (rx_len + 7) & ~3;
    }
    if (tws_conn.tick_req_pending) {
        tws_conn.tick_req_pending = 0;
        tws_conn_master_tx_start_req();
    }
}

int TWS_IRQ tws_conn_rx_handler_in_irq(struct bredr_rx_bulk *rx, u8 *data, int len)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    u8 *param = data + 4;
    u8 state;
    u32 instant;
    int offset;

    switch (data[3]) {
    case 1:
        tws_rx_role_switch_req(param);
        break;
    case 2:
        switch (param[0]) {
        case 1:
            slot_timer_disable(tws_conn.slot_timer);
            tws_link.link->super_timeout = CONFIG_TWS_SUPER_TIMEOUT * 1000 / 625;
            INFO("[Info]: [TWS]accepted_switch_req: %d\n\r\n", data[5]);
            if (conn) {
                state = conn->esco_conn ? 24 : 8;
                if (conn->sbc_open)
                    state |= 32;
                if (tws_conn.monitor_start == 0) {
                    tws_host_timeout_set();
                    slot_timer_set(tws_conn.slot_timer, check_for_setup_monitor_link,
                                   &conn->rejust_esco_req, 4);
                    INFO("[Info]: [TWS]check_for_setup_monitor_1\n\r\n");
                }
            } else {
                state = 0;
                if (data[5] == 1) {
                    instant = data[6] | data[7] << 8 | data[8] << 16 | data[9] << 24;
                    offset = bredr_clkn2offset(tws_link_read_slot_clk(), instant);
                    INFO("[Info]: [TWS]instant: %x, %d\n\r\n", instant, offset);
                    slot_timer_set(tws_conn.slot_timer, tws_conn_master_2_slave, NULL, offset);
                    break;
                }
            }
            tws_conn.connect_suss = 1;
            tws_conn.tx_tick_res = 0;
            if (tws_conn.sem_role_req)
                os_sem_post(tws_conn.sem_role_req);
            tws_conn_send_event(2, "111", tws_conn.role, conn != NULL, state);
            tws_core_tx_jiffies_offset();
            break;
        case 6:
            tws_master_tx_start_monitor_req();
            break;
        case 17:
            tws_rx_unsniff_accept(data + 5);
            break;
        case 15:
            tws_rx_sniff_accept(data + 5);
            break;
        case 25:
            tws_master_tx_ble_start_monitor_req();
            break;
        }
        break;
    case 4:
        tws_cmd_role_switch(param);
        break;
    case 5:
        if (conn) {
            INFO("[Info]: [TWS]check_for_setup_monitor_2: %d, %d\n\r\n",
                 tws_conn.monitor_start, tws_conn.slot_timer);
            if (tws_conn.monitor_start == 0) {
                tws_host_timeout_set();
                slot_timer_set(tws_conn.slot_timer, check_for_setup_monitor_link,
                               &conn->rejust_esco_req, 4);
            }
        }
        break;
    case 6:
        tws_rx_setup_monitor_link_req(param, len - 4);
        break;
    case 7:
        tws_conn_slave_rx_monitor_start_req(param);
        break;
    case 26:
        tws_conn_slave_rx_ble_monitor_start_req(param);
        break;
    case 20:
        tws_conn_slave_rx_esco_monitor_start_req();
        break;
    case 25:
        tws_rx_ble_setup_monitor_link_req(param, len);
        break;
    case 21:
        if (conn)
            tws_slave_rx_start_esco_req(param, conn->link);
        break;
    case 0x80:
        tws_conn_slave_rx_tick_req(data, len);
        break;
    case 0x81:
        tws_conn_master_rx_tick_res(data, len);
        break;
    case 0x82:
        tws_slave_rx_role_switch_req(data);
        break;
    case 0x83:
        tws_master_rx_role_switch_res(data);
        break;
    case 0x84:
        if (tws_conn.role == 0) {
            tws_conn.role = 1;
            tws_conn.tx_tick_res = 1;
            tws_conn.role_switch = 0;
            tws_link.link->send_acl_enable = 1;
            tws_link.link->adjust_clkoffset_enable = 1;
            link_task_run(&tws_link.link->task);
        }
        tws_conn_send_event(10, "1", 1);
        DEBUG("[Debug]: [TWS]role_switch_suss: %x\n\r\n", tws_link_read_slot_clk());
        break;
    case 14:
        DEBUG("[Debug]: [TWS]tws_rx_detach_req: %d %d\n\r\n", tws_conn.detach, param[0]);
        if (tws_conn.detach == 0)
            tws_conn.detach_code = param[0] == 13 ? 13 : 10;
        tws_conn.role_switch = 0;
        os_taskq_post_type("tws", 46, 0, NULL);
        break;
    case 8:
        os_taskq_post_type("tws", 5, 0, NULL);
        break;
    case 10:
        tws_sync_call_rx_events_by_link(param);
        break;
    case 13:
        tws_slave_rx_notice_step_fun(param);
        break;
    case 15:
        tws_rx_sniff_req(param);
        break;
    case 16:
        tws_rx_sniff_info(param);
        break;
    case 17:
        tws_rx_unsniff_req(param);
        break;
    case 22:
        if (CONFIG_LOW_LATENCY_ENABLE)
            tws_slave_enter_pure_monitor_mode(param);
        break;
    case 23:
        if (CONFIG_LOW_LATENCY_ENABLE) {
            if (tws_conn.role == 0)
                tws_master_rx_exit_pure_monitor_req(param);
            else
                tws_slave_rx_exit_pure_monitor_req(param);
        }
        break;
    case 18:
        tws_function_call_by_id(*(u32 *)param, data + 8, *(u16 *)data - 4, 1);
        break;
    case 24:
        tws_core_rx_jiffies_offset(*(u32 *)param);
        break;
    case 0:
        INFO("[Info]: [TWS]master_cancle_tx\n\r\n");
        break;
    }
    return 0;
}

static void tws_rx_role_switch_req(u8 *data)
{
    u8 mode, request;

    INFO("[Info]: [TWS]tws_rx_switch_req: %d, %d\n\r\n", data[0], data[1]);
    mode = data[1];
    tws_local_channel = data[2];
    request = data[3];
    memcpy(hci_param.addr, data + 4, 6);
    pure_monitor_enable = 0;
    if (mode & 16)
        tws_link.link->core.max_slot = 1;
    tws_conn.conn_work_mode = mode;
    tws_link.link->super_timeout = CONFIG_TWS_SUPER_TIMEOUT * 1000 / 625;
    tws_conn_send_cmd(2, "114", 1, request, bredr_offset2clkn(tws_link_read_slot_clk(), 64));
    if (mode == 0 && request == 1) {
        slot_timer_set(tws_conn.slot_timer, tws_conn_slave_2_master, NULL, 64);
        return;
    }
    tws_conn.role = 1;
    tws_conn_send_event(2, "111", 1, mode != 0, mode);
    INFO("[Info]: [TWS]accepted_switch_req: %x\n\r\n", mode);
    if (tws_conn.sem_role_req)
        os_sem_post(tws_conn.sem_role_req);
}

static void tws_conn_master_2_slave(void *p)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    u8 timer;

    INFO("[Info]: [TWS]master2slave: %x\n\r\n", tws_link_read_slot_clk());
    slot_timer_disable(tws_conn.slot_timer);
    if (conn) {
        tws_link_master_2_slave(conn->link);
        if (conn->esco_conn) {
            tws_conn.role = 1;
            tws_conn.tx_tick_res = 1;
            tws_conn.role_switch = 0;
            tws_link.link->send_acl_enable = 1;
            tws_link.link->adjust_clkoffset_enable = 1;
        } else if (tws_conn.pure_monitor) {
            tws_conn.role = 1;
            tws_conn.tx_tick_res = 1;
            tws_conn.role_switch = 0;
            tws_link.link->send_acl_enable = 1;
            link_task_run_slots(&tws_link.link->task, 8);
            timer = slot_timer_get();
            slot_timer_set(timer, tws_conn_adjust_clkoffset_enable, (void *)(int)timer, 10);
            tws_conn_send_event(10, "1", 1);
        } else {
            link_task_try_run_slots(&tws_link.link->task, 100);
        }
        link_conn_set_max_rx_bulk_persent(conn->link, CONFIG_MASTER_MAX_RX_BULK_PERSENT + 10);
    } else {
        bredr_bd_frame_disable(tws_link.link->frame);
        tws_conn.role = 1;
        tws_link.link->role = 1;
        tws_link.link->adjust_clkoffset_enable = 1;
        bredr_frame_role_switch_2_slave(tws_link.link->frame);
        bredr_bd_frame_enable(tws_link.link->frame, 0, 0);
        if (tws_conn.sem_role_req)
            os_sem_post(tws_conn.sem_role_req);
        tws_conn_send_event(2, "111", tws_conn.role, 0, 0);
    }
    tws_conn.try_switch = 0;
    tws_conn.try_switch_2 = 0;
}

static void tws_cmd_role_switch(u8 *data)
{
    int offset = bredr_clkn2offset(tws_link_read_slot_clk(), *(u32 *)data);

    offset = offset > 2 ? offset : 2;
    slot_timer_set(tws_conn.slot_timer, __tws_conn_role_switch, NULL, offset);
    DEBUG("[Debug]: [TWS]rx_role_switch: %d\n\r\n", offset);
}

/* The SDK's static __disable_all_inquiry_and_scan (line 2268). */
static inline __attribute__((always_inline)) void __disable_all_inquiry_and_scan(void)
{
    link_inquiry_disable();
    link_inquiry_scan_disable();
    link_page_disable(254);
    link_page_scan_disable();
}

static void tws_rx_setup_monitor_link_req(u8 *data, int len)
{
    struct lmp_connection *conn;
    int used;

    INFO("[Info]: [TWS]rx_setup_monitor_link_req: in_clkn = %x\n\r\n", tws_link_read_slot_clk());
    if (tws_conn.lmp_conn)
        return;
    tws_conn.role = 1;
    tws_conn.monitor_start = 1;
    __disable_all_inquiry_and_scan();
    conn = lmp_create_conn();
    SDK_ASSERT(conn != NULL, 2367, "conn != NULL");
    memcpy(&conn->core, data, 44);
    memcpy(&conn->core_s, data + 44, 40);
    conn->link = tws_link_get_plink();
    used = tws_link_slave_set_plink_core_data(data + 100);
    link_conn_set_max_rx_bulk_persent(conn->link, CONFIG_MASTER_MAX_RX_BULK_PERSENT + 10);
    tws_conn.lmp_conn = conn;
    lmp_role_machine_handler_register(conn, lmp_slave_machine);
    INFO("[Info]: [TWS]conn->core.esco_state=0x%x\n\r\n", conn->core.esco_state);
    if (conn->core.esco_state == 1) {
        INFO("[Info]: [TWS]tws_rx_setup_monitor_esco_pdu\n\r\n");
        tws_fast_setup_esco_link(conn, data + 84, 0);
    }
    conn->core.operation_mode = 0;
    conn->core.operation_mode_sync = 0;
    used += 100;
    tws_host_set_core_data(data + used, len - used);
    if (tws_common_addr_en)
        lmp_send_event_link_key_notification(conn, 0);
    tws_conn_send_cmd(2, "1", 6);
    INFO("[Info]: [TWS]rx_setup_monitor_link_req: out_clkn = %x\n\r\n", tws_link_read_slot_clk());
}

static void tws_conn_slave_rx_monitor_start_req(u8 *data)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct link_connection *link;
    u32 instant, offset;

    if (conn == NULL || tws_conn.monitor_start != 1)
        return;
    low_latency_enable = data[4];
    instant = data[0] | data[1] << 8 | data[2] << 16 | data[3] << 24;
    offset = bredr_clkn2offset(tws_link_read_slot_clk(), instant);
    slot_timer_set(tws_conn.slot_timer, tws_conn_slave_start_monitor, NULL, offset > 2 ? offset : 2);
    link = conn->link;
    link->super_timeout_begin = bredr_read_slot_clk();
    link->super_timeout = gbredr_local_dev.super_timeout_value * 1000 / 625;
    INFO("[Info]: [TWS]start_monitor_req: %x, %d\n\r\n", instant, offset);
}


static void tws_slave_rx_role_switch_req(u8 *data)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    int offset, slots;
    u16 period;

    INFO("[Info]: [TWS]tws_slave_rx_role_switch_req: %d\n\r\n", tws_conn.role_switch_lock);
    if (tws_conn.role_switch_lock)
        goto reject;
    if (!is_tws_esco_working() && tws_link.link->state != 2) {
        INFO("[Info]: [TWS]role LINK_STA_OPEN err\n\r\n");
        goto reject;
    }
    tws_free_rx_pending();
    if (link_conn_rx_empty(conn->link) == 0) {
        INFO("[Info]: [TWS]<<<<<<<<<<<<<<<<<role_switch rx_not_empty>>>>>>>>>>>>>>>>>\n\r\n");
        goto reject;
    }
    tws_link_set_rxtx_state(conn->link, (struct tws_link_rxtx_state *)(data + 12));
    tws_role_switch_sync_profile_core_data_for_set(data + 20, *(u16 *)data - 16);
    if (is_tws_esco_working()) {
        INFO("[Info]: [TWS]esco_tws_slave_rx_role_switch_req\n\r\n");
        tws_master_notice_slave_step_fun(3, 0, 0);
        return;
    }
    offset = bredr_clkn2offset(tws_link_read_slot_clk(), *(u32 *)(data + 8));
    period = *(u16 *)(data + 4);
    if (offset < period >> 2) {
        INFO("[Info]: [TWS]offset < 1 / 4: %d\n\r\n", offset);
        goto reject;
    }
    slots = (period - offset) * 3;
    slots = slots > 16 ? slots : 16;
    slots = slots > offset ? offset : slots;
    tws_conn.rs_instant = bredr_offset2clkn(tws_link_read_slot_clk(), slots);
    tws_conn_send_cmd(0x83, "V4", 41, tws_conn.rs_instant);
    tws_conn.role_switch = 6;
    link_task_try_run_slots(&tws_link.link->task, slots + 64);
    return;
reject:
    tws_conn_send_cmd(0x83, "4", 0);
}

static void tws_master_rx_role_switch_res(u8 *data)
{
    u32 instant = *(u32 *)(data + 4);
    int offset;

    if (tws_conn.role_switch != 3)
        return;
    if (instant == 0) {
        tws_conn.role_switch = 1;
        return;
    }
    offset = bredr_clkn2offset(tws_link_read_slot_clk(), instant);
    offset = offset > 16 ? offset : 16;
    tws_link.link->send_acl_enable = 0;
    tws_link.link->adjust_clkoffset_enable = 0;
    tws_conn.role_switch = 4;
    slot_timer_set(tws_conn.slot_timer, tws_conn_master_2_slave, NULL, offset);
    DEBUG("[Debug]: [TWS]tws_master_rx_role_switch_res: %x, %d\n\r\n", instant, offset);
}

u8 TWS_CODE tws_slave_rx_notice_step_fun(u8 *rx)
{
    u8 event = rx[0], step = rx[1];
    u32 instant = rx[2] | rx[3] << 8 | rx[4] << 16 | rx[5] << 24;
    struct lmp_connection *conn = tws_conn.lmp_conn;
    int slots;
    u8 timer;

    if (tws_link.link == NULL || conn == NULL)
        return 0;
    if (tws_conn.monitor_start != 2) {
        tws_conn.esco_wait_monitor_start = 0;
        return 0;
    }
    switch (event) {
    case 1:
        if (step >= 2)
            break;
        tws_link.post_step = LMP_TX_VERSION_REQ;
        tws_fast_setup_esco_link(conn, (struct lmp_esco_pdu *)(rx + 6), 0);
        slots = tws_step_fun_start(tws_post_open_esco_link, instant);
        tws_link.post_instant = instant;
        tws_link.remove_esco_timeout = 0;
        if (CONFIG_ESCO_MUX_RX_BULK_ENABLE)
            wait_enter_esco_rx_bulk_change(tws_conn.lmp_conn);
        if (step == 1) {
            timer = tws_link.slot_instant_timer;
            if (timer == 0) {
                INFO("[Info]: [TWS]slot_timer_get\n\r\n");
                timer = tws_link.slot_instant_timer = slot_timer_get();
            }
            slot_timer_set(timer, tws_slave_esco_step_start, tws_conn.lmp_conn, 4);
        } else {
            if (tws_conn.pure_monitor == 0)
                tws_esco_monitor_init(tws_conn.lmp_conn->link);
            tws_link.plink->adjust_winsize_exit_pure = 5;
        }
        link_task_run_slots(&tws_link.link->task, slots);
        INFO("[Info]: [TWS]slave_tws_post_open_esco_link_post:  %d,%d, %d,%d\n\r\n",
             tws_link.slot_instant_timer, slots, instant, tws_link.post_step);
        break;
    case 2:
        if (step >= 2)
            break;
        tws_conn.esco_super_timeout = rx[6];
        if (tws_slave_remove_esco_deal(instant, 0))
            tws_link.post_instant = instant;
        break;
    case 3:
        if (step == 0 && tws_conn.role_switch == 3) {
            tws_conn.role_switch = 7;
            slots = tws_step_fun_start(tws_esco_wait_post_role_switch, instant);
            INFO("[Info]: [TWS]tws_post_esco_role_switch_link_post:  %d,%d, %d,%d,%d\n\r\n",
                 tws_link.slot_instant_timer, slots, instant, tws_link.post_step, tws_conn.role_switch);
        }
        break;
    }
    return 1;
}

u32 jiffies_msec();
int log_get_time_offset();
void log_set_time_offset(int offset);

static inline void tws_function_call_by_id(int id, u8 *data, u16 len, u8 rx)
{
    const struct tws_func_stub *p;

    for (p = tws_func_stub_begin; p < tws_func_stub_end; p++) {
        if (p->func_id == id) {
            p->func(data, len, rx);
            break;
        }
    }
}

static void tws_core_rx_jiffies_offset(u32 offset)
{
    u32 jiffies = (tws_link_read_slot_clk() >> 4) + offset;

    if (jiffies < jiffies_msec() + log_get_time_offset())
        tws_core_tx_jiffies_offset();
    else
        log_set_time_offset(jiffies - jiffies_msec());
    VERBOSE("[Verb]: [TWS]jiffies: %d, %d\n\r\n", jiffies, jiffies_msec() + log_get_time_offset());
}

int TWS_IRQ is_tws_esco_working(void)
{
    if (tws_link.link && tws_conn.lmp_conn && tws_link.link->esco)
        return tws_conn.esco_status;
    return 0;
}

void TWS_CODE tws_esco_role_switch_post(void *p)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    int working = is_tws_esco_working();

    if (conn == NULL || !working || tws_conn.monitor_start != 2 || tws_conn.role_switch != 8)
        return;
    if (tws_conn.role == 0) {
        tws_conn_master_2_slave(&conn->rejust_esco_req);
        tws_conn_send_event(10, "1", 1);
        tws_link.esco_wait_role_swtich = 10;
        bredr_hw_write_lt_addr(conn->link->frame, tws_link.phone_link_acl_lt_addr);
    } else {
        tws_conn_slave_2_master(&conn->rejust_esco_req);
    }
}

static void tws_conn_slave_2_master(void *p)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;

    tws_conn.role = 0;
    tws_conn.role_switch = 0;
    tws_conn.tx_tick_res = 0;
    tws_link.link->send_acl_enable = 1;
    slot_timer_disable(tws_conn.slot_timer);
    if (conn) {
        tws_link_slave_2_master(conn->link);
        if (conn->esco_conn == NULL) {
            if (tws_conn.pure_monitor) {
                link_task_run_slots(&tws_link.link->task, 8);
            } else {
                tws_conn_send_cmd(0x84, "V", 42);
                link_task_try_run_slots(&tws_link.link->task, 100);
            }
        }
        link_conn_set_max_rx_bulk_persent(conn->link, CONFIG_MASTER_MAX_RX_BULK_PERSENT);
        tws_conn_send_event(10, "1", 0);
        return;
    }
    bredr_bd_frame_disable(tws_link.link->frame);
    tws_link.link->role = 0;
    bredr_frame_role_switch_2_master(tws_link.link->frame);
    bredr_bd_frame_enable(tws_link.link->frame, 0, 0);
    tws_conn_send_event(2, "111", tws_conn.role, 0, 0);
    INFO("[Info]: [TWS]accepted_as_master: %x\n\r\n", tws_link_read_slot_clk());
    if (tws_conn.sem_role_req)
        os_sem_post(tws_conn.sem_role_req);
}

u8 TWS_CODE tws_lmp_esco_link_setup(u8 en)
{
    if (tws_conn.esco_setup_flag == 0 || tws_conn.lmp_conn->rx_remove_esco_req_before_esco_setup == 0)
        return 0;
    INFO("[Info]: [TWS]rx_remove_esco_req_before_esco_setup 2\n\r\n");
    tws_master_notice_slave_step_fun(2, 0, 0);
    tws_conn.lmp_conn->rx_remove_esco_req_before_esco_setup = 0;
    return 1;
}

void tws_post_remove_esco_link(void *p) TWS_CODE;
void tws_esco_wait_role_switch_timeout(void *p) TWS_CODE;

u8 TWS_CODE tws_master_notice_slave_step_fun(u8 event, u8 reason, u8 step)
{
    struct link_connection *link = tws_link.link;
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct lmp_esco *esco;
    int ret, timeout, offset;
    u8 timer;

    if (link == NULL || conn == NULL)
        return 0;
    switch (event) {
    case 1:
        if (tws_conn.role != 0)
            break;
        switch (step) {
        case 0:
            if (conn->esco_conn == NULL) {
                INFO("[Info]: [TWS]esco_conn null\n\r\n");
                return 0;
            }
            tws_link.post_step = LMP_TX_VERSION_REQ;
            if (tws_conn.monitor_start != 2 || tws_sniff.enable ||
                !list_empty(&tws_conn.rx_pending) || !list_empty(&link->tx_acl_u)) {
                timer = tws_link.slot_instant_timer;
                if (timer == 0)
                    timer = tws_link.slot_instant_timer = slot_timer_get();
                INFO("[Info]: [TWS]tws_esco_wait_monitor=%d,tws_conn.pure_monitor=%d\n\r\n",
                     timer, tws_conn.pure_monitor);
                local_irq_disable();
                slot_timer_set(tws_link.slot_instant_timer, tws_esco_wait_monitor, tws_conn.lmp_conn, 10);
                local_irq_enable();
                break;
            }
            if (&link->task != get_curr_task()) {
                timer = tws_link.slot_instant_timer;
                if (timer == 0)
                    timer = tws_link.slot_instant_timer = slot_timer_get();
                INFO("[Info]: [TWS]tws_esco_wait_tws_resume=%d\n\r\n", timer);
                local_irq_disable();
                slot_timer_set(tws_link.slot_instant_timer, tws_esco_wait_tws_resume, tws_conn.lmp_conn, 4);
                local_irq_enable();
                break;
            }
            esco = tws_conn.lmp_conn->esco_conn;
            esco->pdu.tws_lt_addr = esco_creart_lt_addr();
            if (esco->pdu.tws_lt_addr > 7)
                INFO("[Info]: [TWS]<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<pdu->tws_lt_addr err>>>>>>>>>>>>>>>>>>>\n\r\n");
            ret = tws_send_esco_step_fun(1, tws_post_open_esco_link, 0, 49, 300);
            INFO("[Info]: [TWS]TWS_OPEN_ESCO_LINK_POST=%d,%d,%d,%d,pdu->tws_lt_addr=%d\n\r\n",
                 tws_link.post_instant, 300, reason, ret, esco->pdu.tws_lt_addr);
            break;
        case 1:
            timeout = CONFIG_ESCO_MUX_RX_BULK_ENABLE ? 100 : 40;
            if (tws_conn.esco_setup_flag) {
                INFO("[Info]: [TWS]esco_setup_flag ==1\n\r\n");
                break;
            }
            if (bredr_clkn2offset(tws_link_read_slot_clk(), tws_link.post_instant) < timeout) {
                tws_link.post_step = LMP_TX_FEATURES_REQ_EXT;
                ret = tws_step_fun_start(tws_post_open_esco_link, tws_link.post_instant);
                link_task_run_slots(&tws_link.link->task, ret + 20);
                INFO("[Info]: [TWS]NOTICE_SLAVE_STEP1_over =%x,%x,%x,%x\n\r\n",
                     tws_link.post_instant, timeout, reason, ret);
            } else {
                ret = tws_send_esco_step_fun(1, tws_post_open_esco_link, 1, 50, timeout);
                tws_link.post_step = LMP_WAIT_VERSION_RES;
                link_task_run_slots(&tws_link.link->task, ret);
                INFO("[Info]: [TWS]NOTICE_SLAVE_STEP1 =%x,%x,%x,%x\n\r\n",
                     tws_link.post_instant, timeout, reason, ret);
            }
            if (tws_conn.pure_monitor == 0)
                tws_esco_monitor_init(tws_conn.lmp_conn->link);
            tws_link.plink->adjust_winsize_exit_pure = 5;
            break;
        case 2:
            ret = tws_step_fun_start(tws_post_open_esco_link, tws_link.post_instant);
            link_task_run_slots(&tws_conn.lmp_conn->link->task, ret);
            tws_link.post_step = LMP_TX_FEATURES_REQ_EXT;
            INFO("[Info]: [TWS]NOTICE_SLAVE_STEP2 =%x,%x,%x,%x\n\r\n", tws_link.post_instant, 160, reason, ret);
            break;
        }
        break;
    case 2:
        INFO("[Info]: [TWS]TWS_REMOVE_ESCO_LINK_STEP_FUN\n\r\n");
        conn = tws_conn.lmp_conn;
        if (conn->rx_esco_req_after_remove_esco) {
            printf("rx_esco_req_after_remove_esco=%d\n", conn->rx_esco_req_after_remove_esco);
            return 0;
        }
        if (tws_conn.monitor_start != 2) {
            tws_conn.esco_wait_monitor_start = 0;
            if (tws_conn.role || tws_conn.esco_setup_flag || conn->esco_conn == NULL)
                return 0;
            INFO("[Info]: [TWS]<<<<<<<<<<<<<<<<<<<<<<wait_monitor_rx_remove_esco_req_before_esco\n\r\n");
            tws_send_esco_step_fun(2, NULL, 0, 51, 24);
            tws_post_remove_esco_link(tws_conn.lmp_conn);
            break;
        }
        link_conn_qos_esco_disable(conn->link);
        if (tws_conn.esco_setup_flag && tws_conn.role == 0) {
            switch (step) {
            case 0:
                if (tws_link.link->esco)
                    tws_link.link->esco->rx_remove_esco_req_flag = 60;
                conn = tws_conn.lmp_conn;
                esco = conn->esco_conn;
                if (esco)
                    esco->reason = reason;
                timeout = conn->rx_remove_esco_req_before_esco_setup ? 800 : 400;
                esco->state = 5;    /* SDK quirk: unguarded when esco_conn is NULL */
                ret = tws_send_esco_step_fun(2, tws_post_remove_esco_link, 0, 51, timeout);
                INFO("[Info]: [TWS]NOTICE_SLAVE_STEP_START=%x,%x,%x,%x\n\r\n",
                     tws_link.post_instant, timeout, reason, ret);
                tws_link.post_step = LMP_WAIT_FEATURES_RES_EXT;
                break;
            case 1:
                offset = bredr_clkn2offset(tws_link_read_slot_clk(), tws_link.post_instant);
                if (offset < 200) {
                    ret = tws_step_fun_start(tws_post_remove_esco_link, tws_link.post_instant);
                    tws_link.post_step = LMP_WAIT_NAME_RES;
                    INFO("[Info]: [TWS]NOTICE_SLAVE_STEP2 over =%x,%x,%x,%x\n\r\n",
                         tws_link.post_instant, 200, reason, ret);
                } else {
                    bredr_esco_close_slot_time(tws_conn.lmp_conn->link);
                    ret = tws_send_esco_step_fun(2, tws_post_remove_esco_link, 1, 52, 200);
                    tws_link.post_step = LMP_TX_NAME_REQ;
                    INFO("[Info]: [TWS]NOTICE_SLAVE_STEP1 =%x,%x,%x,%x\n\r\n",
                         tws_link.post_instant, 200, reason, ret);
                }
                break;
            case 2:
                tws_link.post_step = LMP_WAIT_NAME_RES;
                ret = tws_step_fun_start(tws_post_remove_esco_link, tws_link.post_instant);
                INFO("[Info]: [TWS]NOTICE_SLAVE_STEP2 =%x,%x,%x,%x\n\r\n", tws_link.post_instant, 160, reason, ret);
                break;
            }
        } else if (tws_conn.role == 0 && tws_conn.esco_setup_flag == 0) {
            if ((u8)(tws_link.post_step - 3) < 3) {
                INFO("[Info]: [TWS]rx_remove_esco_req_before_esco_setup 1\n\r\n");
                tws_conn.lmp_conn->rx_remove_esco_req_before_esco_setup = 30;
            } else if (tws_conn.lmp_conn->esco_conn) {
                INFO("[Info]: [TWS]rx_remove_esco_req_before_esco\n\r\n");
                conn = tws_conn.lmp_conn;
                if (conn->core.esco_state)
                    conn->rx_remove_esco_req_before_esco_setup = 1;
                else
                    tws_post_remove_esco_link(&conn->rejust_esco_req);
            }
        } else if (tws_conn.esco_setup_flag && tws_conn.role == 1) {
            INFO("[Info]: [TWS]slave_bredr_esco_close_slot_time\n\r\n");
            if (tws_link.link)
                tws_link.remove_esco_timeout = 400;
        }
        break;
    case 3:
        if (tws_conn.esco_setup_flag == 0 || tws_conn.role != 1)
            break;
        switch (step) {
        case 0:
            tws_link.esco_tx_sync_req_cnt = 20;
            ret = tws_send_esco_step_fun(3, tws_esco_wait_role_switch_timeout, 0, 53, 100);
            INFO("[Info]: [TWS]NOTICE_ROLE_SWITCH_STEP_START=%x,%x,%x,%x\n\r\n",
                 tws_link.post_instant, 100, reason, ret);
            break;
        case 1:
            tws_conn.role_switch = 7;
            ret = tws_step_fun_start(tws_esco_wait_post_role_switch, tws_link.post_instant);
            INFO("[Info]: [TWS]_ROLE_SWITCH_STEP_post=%x,%x,%x,%x\n\r\n", tws_link.post_instant, 160, reason, ret);
            break;
        }
        break;
    }
    return 1;
}

int puts(const char *s);

int TWS_CODE tws_post_remove_esco_link_super_timeout(void *p)
{
    struct lmp_connection *conn = p;
    OS_SEM *sem;

    if (conn == NULL || conn->esco_conn == NULL || tws_link.link == NULL)
        return 0;
    INFO("[Info]: [TWS]tws_post_remove_esco_link_super_timeout start \n\r\n");
    sem = malloc(80);
    SDK_ASSERT(sem != NULL, 3067, "sem");
    os_sem_create(sem, 0);
    local_irq_disable();
    tws_conn.sem_phone_detach = sem;
    local_irq_enable();
    tws_conn.esco_super_timeout = 1;
    if (CONFIG_BTCTLER_TWS_ENABLE)
        tws_master_notice_slave_step_fun(2, 19, 0);
    if (tws_link.link) {
        os_sem_pend(sem, 50);
        tws_conn.sem_phone_detach = NULL;
    }
    free(sem);
    INFO("[Info]: [TWS]tws_post_remove_esco_link_super_timeout over \n\r\n");
    return 1;
}

void TWS_CODE tws_post_remove_esco_link(void *p)
{
    struct lmp_connection *conn = p;
    u8 id, timer;

    if (conn == NULL || conn->esco_conn == NULL || tws_link.link == NULL)
        return;
    INFO("[Info]: [TWS]tws_post_remove_esco_link\n\r\n");
    if (tws_link.post_step == LMP_TX_NULL)
        INFO("[Info]: [TWS]remove_esco_link resson 0\n\r\n");
    id = conn->esco_conn->rx_esco_tran_id;
    if (id != conn->tran_id)
        conn->tran_id = id;
    remove_esco_link(conn, 19);
    memset(&tws_conn.esco_status, 0, 4);
    tws_link.post_step = LMP_TX_NULL;
    tws_link.post_instant = 0;
    timer = tws_link.slot_instant_timer;
    if (timer) {
        slot_timer_put(timer);
        tws_link.slot_instant_timer = 0;
    }
    tws_conn.lmp_conn->link->adjust_esco_winsize = 0;
    tws_conn.lmp_conn->rx_remove_esco_req_before_esco_setup = 0;
    if (tws_conn.role == 0 && tws_conn.esco_super_timeout) {
        os_sem_post(tws_conn.sem_phone_detach);
    } else {
        lmp_response(conn, "b04", 254, 1, 127, 13);
        INFO("[Info]: [TWS]lmp_response remove_esco\n\r\n");
    }
    if (tws_conn.role == 0) {
        if (tws_conn.tx_tick_res)
            INFO("[Info]: [TWS]waring tx_tick_res != 0\n\r\n");
        tws_conn.tx_tick_res = 0;
    }
}

void TWS_CODE tws_esco_wait_tws_resume(void *p)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;

    if (conn == NULL || conn->esco_conn == NULL) {
        INFO("[Info]: [TWS]return wait_esco_monitor\n\r\n");
        return;
    }
    if (&tws_link.link->task == get_curr_task()) {
        INFO("[Info]: [TWS]curr_task tws\n\r\n");
        tws_master_notice_slave_step_fun(1, 0, 0);
    } else {
        putchar('W');
        slot_timer_reset(tws_link.slot_instant_timer, 4);
    }
}

void TWS_CODE tws_esco_wait_monitor(void *p)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    u8 *pending;

    if (conn == NULL || conn->esco_conn == NULL) {
        INFO("[Info]: [TWS]return wait_esco_monitor\n\r\n");
        return;
    }
    putchar('w');
    if (tws_conn.monitor_start == 2 && !tws_sniff.enable && list_empty(&tws_conn.rx_pending) &&
        list_empty(&tws_link.link->tx_acl_u) && conn->sbc_open == 0) {
        tws_master_notice_slave_step_fun(1, 0, 0);
        return;
    }
    if (!list_empty(&tws_link.link->tx_acl_u))
        putchar('a');
    if (tws_sniff.state & 1)
        putchar('b');
    if (tws_conn.monitor_start != 2) {
        putchar('c');
        if (tws_conn.esco_wait_monitor_start) {
            if (--tws_conn.esco_wait_monitor_start < 10)
                INFO("[Info]: [TWS]esco_wait_monitor_start timeout\n\r\n");
        }
    }
    if (conn->sbc_open)
        putchar('s');
    if (!list_empty(&tws_conn.rx_pending)) {
        putchar('d');
        pending = &tws_conn.lmp_conn->rx_remove_esco_req_before_esco_setup;
        if (*pending >= 2 && --*pending == 1) {
            puts("rx_remove_esco_req_before_esco_setup timeout\n");
            tws_post_remove_esco_link(tws_conn.lmp_conn);
            return;
        }
    }
    slot_timer_reset(tws_link.slot_instant_timer, 4);
}

void TWS_CODE tws_esco_wait_post_role_switch(void *p)
{
    struct bredr_esco_link *esco;
    u8 timer;

    if (tws_link.link == NULL)
        return;
    tws_link.post_instant = 0;
    timer = tws_link.slot_instant_timer;
    if (timer) {
        slot_timer_put(timer);
        tws_link.slot_instant_timer = 0;
    }
    INFO("[Info]: [TWS]tws_esco_wait_post_role_switch=%d\n\r\n", tws_conn.role_switch);
    tws_link.esco_wait_role_swtich = 0;
    if (tws_conn.role_switch == 7)
        tws_conn.role_switch = 8;
    else
        INFO("[Info]: [TWS]tws_esco_wait_post_role_switch timeout\n\r\n");
    tws_link.link->adjust_clkoffset_enable = 0;
    esco = tws_conn.lmp_conn->link->esco;
    if (esco)
        esco->esco_acl_resume_t = 0;
}

void TWS_CODE tws_esco_wait_role_switch_timeout(void *p)
{
    u8 timer;

    INFO("[Info]: [TWS]tws_esco_wait_role_switch_timeout\n\r\n");
    timer = tws_link.slot_instant_timer;
    if (timer) {
        slot_timer_put(timer);
        tws_link.slot_instant_timer = 0;
    }
    tws_conn.role_switch = 0;
}

int TWS_CODE tws_step_fun_start(void (*tws_post_caback_fun)(void *), int post_instant)
{
    int offset;
    u8 timer;

    if (tws_conn.lmp_conn == NULL)
        return 4;
    offset = bredr_clkn2offset(tws_link_read_slot_clk(), post_instant);
    if ((u32)(offset - 4) > 69119996)
        offset = 4;
    local_irq_disable();
    timer = tws_link.slot_instant_timer;
    if (timer == 0) {
        INFO("[Info]: [TWS]slot_timer_get\n\r\n");
        timer = tws_link.slot_instant_timer = slot_timer_get();
    }
    slot_timer_set(timer, tws_post_caback_fun, tws_conn.lmp_conn, offset);
    local_irq_enable();
    INFO("[Info]: [TWS]tws_link.slot_instant_timer=%d\n\r\n", tws_link.slot_instant_timer);
    return offset;
}

int TWS_CODE tws_send_esco_step_fun(u8 event, void (*tws_post_caback_fun)(void *), u8 step, u8 post_step, int instant)
{
    struct lmp_esco_pdu *pdu;

    if (tws_link.link == NULL || tws_conn.lmp_conn == NULL)
        return 0;
    tws_link.post_instant = bredr_offset2clkn(tws_link_read_slot_clk(), instant);
    switch (event) {
    case 1:
        pdu = &tws_conn.lmp_conn->esco_conn->pdu;
        INFO("[Info]: [TWS]tws_send_esco_step_fun end esco setup\n\r\n");
        tws_conn_send_cmd(13, "V114c14", post_step, event, step, tws_link.post_instant, pdu);
        break;
    case 2:
        tws_conn_send_cmd(13, "V1141", post_step, event, step, tws_link.post_instant,
                          tws_conn.esco_super_timeout);
        break;
    case 3:
        tws_conn_send_cmd(13, "V114", post_step, event, step, tws_link.post_instant);
        break;
    }
    return tws_step_fun_start(tws_post_caback_fun, tws_link.post_instant);
}

u8 TWS_CODE tws_slave_remove_esco_deal(int instant, u8 timeout)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    int ret;

    if (tws_link.link == NULL || conn == NULL || conn->esco_conn == NULL)
        return 0;
    if (tws_link.link->esco) {
        tws_link.link->esco->rx_remove_esco_req_flag = 60;
        bredr_esco_close_slot_time(conn->link);
    }
    if (tws_conn.lmp_conn->esco_conn)
        tws_conn.lmp_conn->esco_conn->reason = 19;
    tws_link.remove_esco_timeout = 0;
    tws_link.post_step = LMP_WAIT_FEATURES_RES_EXT;
    if (timeout) {
        INFO("[Info]: [TWS]waring_remove_esco_timeout\n\r\n");
        tws_post_remove_esco_link(tws_conn.lmp_conn);
        ret = 0;
    } else {
        instant += 8;
        ret = tws_step_fun_start(tws_post_remove_esco_link, instant);
    }
    INFO("[Info]: [TWS]slave_tws_post_remove_esco_link_post:%d, %d,%d, %d,%d\n\r\n",
         timeout, tws_link.slot_instant_timer, ret, instant, tws_link.post_step);
    return 1;
}

static void tws_slave_esco_step_start(void *p)
{
    struct lmp_connection *conn = p;

    if (conn == NULL || conn->esco_conn == NULL || tws_link.link == NULL)
        return;
    link_task_run_slots(&tws_conn.lmp_conn->link->task,
                        tws_step_fun_start(tws_post_open_esco_link, tws_link.post_instant));
}

int TWS_CODE tws_check_can_setup_esco(void)
{
    INFO("[Info]: [TWS]%s %d %d %d\r\n", __func__, tws_conn.role, tws_conn.esco_setup_flag,
         tws_conn.monitor_start);
    if (tws_link.link == NULL || tws_conn.lmp_conn == NULL)
        return 1;
    if (tws_conn.monitor_start != 2) {
        tws_conn.esco_wait_monitor_start = 2000;
        return 0;
    }
    switch (tws_conn.role) {
    case 0:
        if (tws_conn.esco_setup_flag)
            return 1;
        tws_conn.esco_wait_monitor_start = 0;
        return 0;
    case 1:
        return tws_conn.esco_setup_flag != 0;
    }
    return 1;
}

int TWS_CODE tws_conn_phone_link_rx_lmp_add(struct link_connection *link, u16 lmp_value, u8 *data)
{
    if (is_tws_esco_working()) {
        tws_link.esco_rx_lmp_slot_clk = tws_link_read_slot_clk();
        tws_link.esco_rx_lmp_cmd = lmp_value;
        if (tws_conn.role_switch)
            INFO("[Info]: [TWS]is_tws_esco_working reject esco_role_switch 3=%d\n\r\n", tws_conn.role_switch);
    } else {
        tws_link.esco_rx_lmp_slot_clk = 0;
        tws_link.esco_rx_lmp_cmd = 0;
    }
    return 0;
}

void tws_host_timeout_del(void);
static void do_tws_conn_detach(int reason) TWS_CODE;
void tws_role_switch_lock_timeout(void *p) TWS_CODE;

static void TWS_CODE tws_accept_role_switch_timeout(void *p)
{
    INFO("[Info]: [TWS]tws_accept_role_switch_timeout\n\r\n");
    tws_link_detach();
    tws_conn_send_event(4, "1111", tws_conn.role, tws_conn.lmp_conn != NULL, 8, 0);
    tws_conn.role = 0;
    tws_conn.lmp_conn = NULL;
    tws_conn.run_slot = 0;
    tws_conn.tx_tick_res = 0;
    slot_timer_put(tws_conn.slot_timer);
}

void TWS_CODE tws_conn_event_handler(int *msg)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct tws_sync_channel *channel;
    OS_SEM *sem;
    u8 state;
    char local_channel;
    int i, role, err, offset, packets;

    switch (msg[0]) {
    case 22:
        if (msg[1] == 0) {
            state = 0;
            if (conn) {
                if (conn->esco_conn) {
                    tws_link.link->core.max_slot = 1;
                    state = 24;
                } else {
                    state = 8;
                }
                if (conn->sbc_open)
                    state |= 32;
            }
            local_channel = tws_host_get_local_channel();
            tws_local_channel = local_channel;
            if (tws_common_addr_en) {
                for (i = 0; i < 6; i++)
                    hci_param.addr[i] = gbredr_local_dev.internal_addr_remote[i] +
                                        gbredr_local_dev.internal_addr_local[i];
            }
            tws_host_get_common_addr(tws_link.link->remote_mac_addr, hci_param.addr, local_channel);
            os_time_dly(2);
            role = tws_host_get_local_role();
            tws_conn.conn_work_mode = state;
            tws_conn_send_cmd(1, "1111A", conn != NULL, state, local_channel == 76 ? 82 : 76, role,
                              hci_param.addr);
            slot_timer_set(tws_conn.slot_timer, tws_accept_role_switch_timeout, NULL, 500);
            if (conn)
                break;
        }
        sem = malloc(80);
        SDK_ASSERT(sem != NULL, 3854, "sem");
        os_sem_create(sem, 0);
        tws_conn.sem_role_req = sem;
        INFO("[Info]: [TWS]pend_in\n\r\n");
        err = os_sem_pend(sem, 30);
        free(sem);
        tws_conn.sem_role_req = NULL;
        INFO("[Info]: [TWS]pend_exit: err = %d\n\r\n", err);
        tws_conn.connect_suss = 1;
        if (tws_conn.role == 0)
            tws_core_tx_jiffies_offset();
        break;
    case 59:
        tws_host_timeout_del();
        break;
    case 15:
        if (tws_conn.role == 0)
            tws_conn_role_switch_lock_ms(3000);
        break;
    case 45:
        DEBUG("[Debug]: [TWS]tws_event_super_timeout\n\r\n");
        if (tws_conn.pure_monitor && tws_conn.role == 1) {
            offset = bredr_clkn2offset(tws_link.link->super_timeout_begin, bredr_read_slot_clk());
            if (conn && offset < 9600) {
                packets = __lmp_get_a2dp_packet_num(conn);
                /* The SDK computes and discards the phone link's offset. */
                bredr_clkn2offset(tws_link.plink->super_timeout_begin, bredr_read_slot_clk());
                if (packets > 2) {
                    tws_link.link->super_timeout = CONFIG_TWS_SUPER_TIMEOUT * 1000 / 625;
                    slot_timer_reset(tws_link.link->super_timer, 400);
                    break;
                }
            }
        }
        do_tws_conn_detach(8);
        break;
    case 46:
        os_time_dly(2);
        do_tws_conn_detach(tws_conn.detach_code);
        break;
    case 5:
        os_time_dly(2);
        memset(gbredr_local_dev.internal_addr_remote, 0xff, 6);
        tws_conn_send_event(5, "111", 0, 0, 0);
        do_tws_conn_detach(12);
        break;
    case 54:
        link_conn_rx_bulk_avaliable((struct link_connection *)msg[1], NULL);
        break;
    case 66:
        if (tws_conn.role == 0)
            tws_core_tx_jiffies_offset();
        break;
    }
    for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
        if (channel->event_handler)
            channel->event_handler(conn, msg[0], msg + 1);
    }
}

void TWS_CODE tws_conn_role_switch_lock_ms(int timeout)
{
    local_irq_disable();
    if (tws_conn.role == 1 || tws_conn.monitor_start != 2 || tws_conn.role_switch) {
        local_irq_enable();
        return;
    }
    tws_conn.role_switch_lock |= 2;
    local_irq_enable();
    if (rs_timer)
        sys_timer_modify(rs_timer, timeout);
    else
        rs_timer = sys_timeout_add(NULL, tws_role_switch_lock_timeout, timeout);
    VERBOSE("[Verb]: [TWS]rs_lock_ms: %d\n\r\n", timeout);
}

static void do_tws_conn_detach(int reason)
{
    struct lmp_connection *conn, *reported;
    struct tws_sync_channel *channel;
    struct list_head *node, *next;
    struct link_connection *link;
    int has_conn;
    int *role;
    u8 state;

    DEBUG("[Debug]: [TWS]detach_end: %d, %d\n\r\n", reason, tws_conn.monitor_start);
    tws_sniff_detach();
    tws_detach_timer_del();
    local_irq_disable();
    conn = tws_conn.lmp_conn;
    tws_lmp_sync_timer_disable();
    slot_timer_put(tws_conn.slot_timer);
    if (CONFIG_LOW_LATENCY_ENABLE && tws_conn.pure_monitor) {
        tws_conn.pure_monitor = 0;
        if (tws_conn.role == 0)
            tws_link_master_exit_pure_monitor();
        else
            tws_link_slave_exit_pure_monitor();
    }
    if (tws_conn.monitor_start) {
        tws_conn.monitor_start = 0;
        for (node = tws_conn.rx_pending.next; node != &tws_conn.rx_pending; node = next) {
            next = node->next;
            __list_del_entry(node);
            if (conn && (conn->link->monitor & 1) == 0)
                list_add_tail(node, &conn->link->rx_head);
            else
                bredr_rx_bulk_free((struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry)));
        }
        /* SDK quirk: conn is not checked here. */
        bredr_link_event(conn->link, 1, 5);
    }
    tws_link_detach();
    has_conn = conn != NULL;
    if (conn && conn->link && (conn->link->monitor & 1) == 0 && conn->esco_conn &&
        conn->esco_conn->state == 5)
        remove_esco_link(conn, 19);
    if (tws_link.slot_instant_timer) {
        INFO("[Info]: [TWS]put_tws_link.slot_instant_timer\n\r\n");
        slot_timer_put(tws_link.slot_instant_timer);
        tws_link.slot_instant_timer = 0;
        if (reason == 8 && has_conn && conn->esco_conn) {
            bredr_link_event(conn->link, 1, 9);
            local_irq_enable();
            goto state;
        }
    }
    local_irq_enable();
    if (!has_conn) {
        state = reason == 8 ? tws_conn.conn_work_mode : 0;
        INFO("[Info]: [TWS]lmp_no_exist\n\r\n");
        reported = conn;
        goto check;
    }
state:
    state = conn->esco_conn ? 24 : 8;
    if (conn->sbc_open)
        state |= 32;
    reported = NULL;
    if (conn->core.state & 8)
        goto check;
    link = conn->link;
    if ((link->monitor & 1) == 0 && (tws_conn.detach == 0 || tws_conn.detach_code == 13)) {
        INFO("[Info]: [TWS]send_detach_to_host\n\r\n");
        bredr_link_event(conn->link, 1, 5);
        reported = conn;
        goto check;
    }
    INFO("[Info]: [TWS]send_detach_to_lmp\n\r\n");
    if (reason == 8) {
        conn->err_code = 8;
        conn->core.state |= 8;
        bredr_link_event(conn->link, 2, 14, 8);
        goto report;
    }
    conn->err_code = 22;
    link = conn->link;
    if (link->monitor & 1) {
        conn->core.state |= 8;
        bredr_link_event(link, 2, 14, 22);
    }
check:
    if (reason != 11) {
report:
        tws_host_timeout_del_event();
        tws_conn_send_event(4, "1111", tws_conn.role, reported != NULL, reason, state);
    }
    if (tws_conn.phone_link_detach & 1)
        os_sem_post(tws_conn.sem_phone_detach);
    if (tws_conn.sem_tws_detach)
        os_sem_post(tws_conn.sem_tws_detach);
    role = (int *)(int)tws_conn.role;   /* the SDK passes the role as the pointer argument */
    for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
        if (channel->event_handler)
            channel->event_handler(reported, 46, role);
    }
    tws_conn.role = 0;
}

u8 TWS_CODE tws_conn_switch_is_master(void)
{
    if (tws_link.link)
        return tws_conn.role == 0 && tws_conn.role_switch == 0;
    return 1;
}

int TWS_CODE tws_conn_switch_role_lock(void)
{
    int ret = -22;

    local_irq_disable();
    if (tws_conn.role_switch == 0) {
        tws_conn.role_switch_lock |= 1;
        ret = 0;
    }
    local_irq_enable();
    return ret;
}

void TWS_CODE tws_conn_switch_role_unlock(void)
{
    tws_conn.role_switch_lock &= ~1;
}

void TWS_CODE tws_role_switch_lock_timeout(void *p)
{
    rs_timer = 0;
    tws_conn.role_switch_lock &= ~2;
    VERBOSE("[Verb]: [TWS]rs_unlock\n\r\n");
}

static void tws_conn_detach_timeout(void *p) TWS_CODE;
static void tws_master_tx_role_switch_req(void) TWS_CODE;

void TWS_CODE tws_conn_detach_internal(int reason)
{
    DEBUG("[Debug]: [TWS]detach_step1: reason = %d, %d\n\r\n", reason, tws_conn.monitor_start);
    local_irq_disable();
    if (tws_link.link == NULL) {
        local_irq_enable();
        if (tws_conn.sem_tws_detach)
            os_sem_post(tws_conn.sem_tws_detach);
        return;
    }
    tws_conn.detach = 1;
    tws_conn.detach_code = reason;
    switch (tws_conn.monitor_start) {
    case 0:
        local_irq_enable();
        DEBUG("[Debug]: [TWS]detach_step2: tx_detach\n\r\n");
        tws_conn_send_cmd(14, "V1", 47, tws_conn.detach_code);
        break;
    case 2:
        if (tws_conn.role == 0) {
            if (tws_conn.pure_monitor == 0)
                tws_conn_switch_role();
        } else {
            tws_conn.role_switch_lock = 1;
        }
        local_irq_enable();
        break;
    default:
        local_irq_enable();
        if (tws_conn.role == 1) {
            DEBUG("[Debug]: [TWS]detach_step2: tx_detach\n\r\n");
            tws_conn_send_cmd(14, "V1", 47, tws_conn.detach_code);
        }
        break;
    }
    tws_detach_timer = sys_timeout_add(NULL, tws_conn_detach_timeout, 5000);
}

static void tws_conn_detach_timeout(void *p)
{
    tws_detach_timer = 0;
    do_tws_conn_detach(8);
}

void TWS_CODE tws_conn_channel_open(u8 channel)
{
    tws_conn.channels |= 1 << channel;
}

void TWS_CODE tws_conn_channel_close(u8 channel)
{
    tws_conn.channels &= ~(1 << channel);
    if (tws_conn.channels == 0 && tws_conn.role == 1)
        link_task_run_slots(&tws_link.link->task, 24);
}

/* Every received phone-link ACL packet is complete (ready and not lost). */
static inline __attribute__((always_inline)) int tws_rx_acl_all_ready(struct lmp_connection *conn)
{
    struct list_head *node;

    for (node = conn->rx_acl_u.next; node != &conn->rx_acl_u; node = node->next) {
        struct bredr_rx_bulk *rx = (struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry));

        if (!rx->ready || rx->lost)
            return 0;
    }
    return 1;
}

void TWS_IRQ tws_communication_ending(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct link_connection *link;
    int low_latency;
    u8 role_switch;

    if (tws_conn.role == 1)
        return;
    link = tws_link.link;
    if (tws_conn.monitor_start != 2) {
        if (link_conn_tx_empty(link) == 0 && (conn == NULL || conn->sbc_open == 0) &&
            bredr_clkn2offset(tws_link.link->resume_clkn, READ_SLOT_CLK()) <= 99)
            link_task_try_run_slots(&tws_link.link->task, 16);
        return;
    }
    if (link->state == 3)
        return;
    if (is_tws_esco_working() || conn->link->esco)
        return;
    if (link_conn_tx_empty(link) == 0)
        return;
    if (tws_conn.tick_req_pending || tws_conn.tx_tick_res)
        return;
    if (tws_conn.channels && bredr_clkn2offset(tws_link.link->resume_clkn, READ_SLOT_CLK()) <= 31)
        return;
    low_latency = CONFIG_LOW_LATENCY_ENABLE != 0;
    if (low_latency && tws_conn.pure_monitor) {
        if (tws_conn.exit_pure_monitor == 2) {
            tws_conn.exit_pure_monitor = 3;
            tws_master_tx_exit_pure_monitor_req();
            return;
        }
        if (tws_conn.role_switch == 0)
            return;
    }
    if (tws_sniff.enable)
        return;
    role_switch = tws_conn.role_switch;
    switch (role_switch) {
    case 0:
        if (!low_latency || pure_monitor_enable != 1)
            break;
        if (tws_conn.phone_link_detach)
            return;
        if ((conn->sbc_open == 0 || conn->esco_conn) && conn->link->wait_exit_sniff_rx) {
            putchar('Y');
            return;
        }
        if (bredr_clkn2offset(tws_link.link->resume_clkn, READ_SLOT_CLK()) >= 32)
            break;
        if (!list_empty(&tws_conn.rx_pending))
            break;
        putchar('D');
        if (conn->esco_conn || link_conn_rx_empty(conn->link) == 0)
            return;
        putchar('E');
        if (!tws_rx_acl_all_ready(conn))
            return;
        tws_master_tx_enter_pure_monitor_req();
        return;
    case 2:
        putchar('C');
        if (tws_conn.phone_link_detach) {
            tws_master_tx_role_switch_req();
            return;
        }
        if (list_empty(&tws_conn.rx_pending)) {
            putchar('D');
            if (lmp_conn_idle(conn)) {
                putchar('E');
                if (list_empty(&tws_conn.tx_pending) && link_conn_rx_empty(conn->link)) {
                    putchar('F');
                    if (link_conn_tx_empty(conn->link)) {
                        putchar('G');
                        if (!tws_rx_acl_all_ready(conn))
                            return;
                        tws_master_tx_role_switch_req();
                        return;
                    }
                }
            }
        }
        os_taskq_post_type("tws", 54, 1, (int *)&conn->link);
        break;
    default:
        if (role_switch > 2)
            return;
        break;
    }
    link_task_run_slots(&conn->link->task, conn->sbc_open ? CONFIG_PHONE_RUN_SLOT : 64);
}

u32 TWS_CODE JBHash(u8 *data, int len)
{
    u32 hash = 5381;

    while (len != 0) {
        len--;
        hash = hash * 33 + *data++;
    }
    return hash;
}

#define RX_CHANNEL(d) ((d)[3] << 8 | (d)[2])
#define RX_SEQN(d) ((d)[6] << 8 | (d)[7])

int TWS_IRQ tws_core_plink_rx_irq_handler(struct link_connection *link, struct bredr_rx_bulk *rx)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct list_head *node;
    u8 pure_monitor, llid;

    switch (tws_conn.monitor_start) {
    case 0:
        return 0;
    case 2:
        if (tws_conn.esco_setup_flag == 0)
            break;
        if (rx->llid == 3 && (rx->data[0] & ~1) == 92)
            tws_conn_phone_link_rx_lmp_add(link, 92, rx->data);
        return 0;
    default:
        if (tws_conn.role == 0) {
            switch (rx->llid) {
            case 2:
                if (conn->sbc_channel == RX_CHANNEL(rx->data))
                    return 0;
                break;
            case 1:
                return 0;
            }
        }
        break;
    }
    pure_monitor = tws_conn.pure_monitor;
    if (CONFIG_LOW_LATENCY_ENABLE && pure_monitor) {
        if (low_latency_enable)
            tws_conn.history_hash[0] = (rx->llid == 2 && conn->sbc_channel == RX_CHANNEL(rx->data)) ? 0 : rx->hash;
        else
            tws_conn.history_hash[0] = rx->hash;
    }
    llid = rx->llid;
    if (llid == 3) {
        if (tws_conn.role == 1)
            goto drop;
        return 0;
    }
    if (pure_monitor && low_latency_enable && list_empty(&tws_conn.rx_pending))
        return 0;
    if (llid != 2)
        return -22;
    if (conn->sbc_channel == RX_CHANNEL(rx->data)) {
        if (conn->busy == 0)
            bredr_link_event(conn->link, 1, 5);
        for (node = conn->rx_acl_u.next; node != &conn->rx_acl_u; node = node->next) {
            struct bredr_rx_bulk *queued = (struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry));

            if (RX_CHANNEL(queued->data) == conn->sbc_channel && RX_SEQN(queued->data) == RX_SEQN(rx->data))
                goto drop;
        }
        list_add_tail(&rx->entry, &conn->rx_acl_u);
        return -22;
    }
    if (tws_conn.role == 1)
        goto drop;
    list_add_tail(&rx->entry, &tws_conn.rx_pending);
    return -22;
drop:
    bredr_rx_bulk_free(rx);
    os_taskq_post_type("tws", 54, 1, (int *)&conn->link);
    return -22;
}

int TWS_IRQ tws_core_rx_irq_handler_probe(u8 *data, int len)
{
    struct tws_sync_channel *channel;
    u8 id = data[2];

    if (id == 0)
        return tws_conn_rx_handler_in_irq(NULL, data, len);
    for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
        if (channel->channel == id) {
            if (channel->event_handler)
                return channel->event_handler(NULL, 25, (int *)data);
            return 0;
        }
    }
    return 0;
}

void TWS_IRQ tws_core_rx_irq_handler_post(struct link_connection *link, struct bredr_rx_bulk *rx)
{
    struct tws_sync_channel *channel;
    u8 id = rx->data[2];

    if (id == 0) {
        tws_conn_rx_handler_in_irq(NULL, rx->data, rx->len);
        bredr_rx_bulk_free(rx);
    } else {
        for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
            if (channel->channel == id) {
                if (channel->event_handler)
                    channel->event_handler(NULL, 26, (int *)rx);
                break;
            }
        }
    }
    os_taskq_post_type("tws", 54, 1, (int *)&link);
}

int TWS_IRQ tws_core_get_buf_len_for_packet(u8 *data)
{
    struct tws_sync_channel *channel;
    u8 id = data[2];

    if (id == 0)
        return *(u16 *)data + 4;
    for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
        if (channel->channel == id) {
            if (channel->event_handler)
                return channel->event_handler(tws_conn.lmp_conn, 27, (int *)data);
            break;
        }
    }
    return *(u16 *)data + 4;
}

int TWS_IRQ tws_core_rx_bulk_full_handler(int size)
{
    struct lmp_connection *conn = lmp_conn_for_address(NULL);
    struct list_head *node;
    struct bredr_rx_bulk *rx;

    if (conn == NULL)
        return -12;
    for (node = conn->rx_acl_u.next; node != &conn->rx_acl_u; node = node->next) {
        rx = (struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry));
        if (RX_CHANNEL(rx->data) == conn->sbc_channel)
            printf("sbc: %d, %d\n", RX_SEQN(rx->data), rx->sbc_flag);
        else
            printf("acl_u_packet: %d\n", rx->len);
    }
again:
    for (node = conn->rx_acl_u.prev; node != &conn->rx_acl_u; node = node->prev) {
        rx = (struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry));
        if (RX_CHANNEL(rx->data) != conn->sbc_channel)
            continue;
        if (rx->sbc_flag == 2)
            return -12;
        size -= rx->len;
        __list_del_entry(node);
        bredr_rx_bulk_free(rx);
        if (size < 1)
            return 0;
        goto again;
    }
    return -12;
}

void TWS_CODE tws_core_tws_wait_tx(void)
{
    if (tws_conn.role == 0)
        tws_conn.tws_wait_tx = 1;
}

void TWS_CODE tws_core_a2dp_packet_go(int packet)
{
}

int TWS_IRQ tws_conn_tx_event_handler(int event, struct bredr_tx_bulk *tx)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct tws_sync_channel *channel;
    int offset;

    switch (event) {
    case 23:
        INFO("[Info]: [TWS]========connect_post\n\r\n");
        break;
    case 28:
        tws_conn.monitor_start = 3;
        link_task_run(&conn->link->task);
        break;
    case 47:
        os_taskq_post_type("tws", 46, 0, NULL);
        break;
    case 48:
        os_taskq_post_type("tws", 5, 0, NULL);
        break;
    case 43:
        if (tws_conn.tx_tick_res)
            tws_conn.tx_tick_res--;
        tws_conn.tick_req_pending = 0;
        if (tws_conn.pure_monitor == 0 && tws_conn.monitor_start == 2 && !is_tws_esco_working())
            link_task_try_run_slots(&tws_link.link->task, CONFIG_TWS_RUN_SLOT);
        /* fall through */
    case 44:
        tws_conn.prev_req_err = 0;
        if (!tws_sniff.enable && tws_link.link && !tws_link.link->role) {
            if (tws_link.link->core.max_slot != 5) {
                INFO("[Info]: [TWS]+resume_max_slot\n\r\n");
                sniff_test_tone = 0;
            }
            tws_link.link->core.max_slot = 5;
        }
        if (event == 43) {
            for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
                if (channel->tx_sync_req_suss)
                    channel->tx_sync_req_suss();
            }
        } else {
            for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
                if (channel->tx_sync_res_suss)
                    channel->tx_sync_res_suss();
            }
        }
        break;
    case 41:
        if (tws_conn.role_switch != 6)
            break;
        offset = bredr_clkn2offset(tws_link_read_slot_clk(), tws_conn.rs_instant);
        if (offset < 2) {
            tws_conn_slave_2_master(NULL);
        } else {
            tws_conn.role_switch = 7;
            tws_link.link->send_acl_enable = 0;
            slot_timer_set(tws_conn.slot_timer, tws_conn_slave_2_master, NULL, offset);
        }
        break;
    case 42:
        link_task_run(&conn->link->task);
        DEBUG("[Debug]: [TWS]role_switch_suss: %x\n\r\n", tws_link_read_slot_clk());
        break;
    case 65:
        if (CONFIG_LOW_LATENCY_ENABLE)
            tws_master_enter_pure_monitor();
        break;
    case 67:
        if (CONFIG_TWS_WORK_MODE != 2 && tws_link.link->bt_modulaion_mode == 0)
            link_conn_set_ptt(tws_link.link, 1);
        break;
    case 49:
    case 50:
        tws_master_notice_slave_step_fun(1, 0, event == 49 ? 1 : 2);
        break;
    case 51:
    case 52:
        DEBUG("[Debug]: [TWS]TWS_EVENT_ESCO_REMOVE\n\r\n");
        tws_master_notice_slave_step_fun(2, 0, event == 51 ? 1 : 2);
        break;
    case 53:
        tws_master_notice_slave_step_fun(3, 0, 1);
        break;
    case 63:
        tws_function_call_by_id(*(u32 *)&tx->data[4], &tx->data[8], tx->len - 8, 0);
        break;
    }
    for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
        if (channel->channel == tx->data[2]) {
            if (channel->event_handler)
                return channel->event_handler(conn, event, (int *)tx);
            return 0;
        }
    }
    return 0;
}

static int role_switch_by_power_check(void)
{
    int remain;

    if (!CONFIG_TWS_POWER_BALANCE_ENABLE || !tws_conn.role_switch_by_power)
        return 0;
    if (tws_link.link->rx_rssi < -75)
        return 0;
    remain = lmp_private_get_sbc_remain_time(tws_conn.lmp_conn, 0);
    if (remain < CONFIG_A2DP_DELAY_TIME && remain > CONFIG_A2DP_DELAY_TIME - 70) {
        tws_conn.role_switch_by_power = 0;
        tws_conn.try_switch = 0;
        tws_conn.role_switch = 1;
        DEBUG("[Debug]: [TWS]try_switch_by_power: %d\n\r\n", remain);
        return 1;
    }
    return 0;
}

static void __tws_conn_role_switch()
{
    bredr_bd_frame_disable(tws_link.link->frame);
    if (tws_conn.role == 0) {
        tws_conn.role = 1;
        tws_conn.tx_tick_res = 1;
        tws_link.link->role = 1;
        tws_link.link->adjust_clkoffset_enable = 1;
        bredr_frame_role_switch_2_slave(tws_link.link->frame);
        link_task_set_sleep_slot(0);
    } else {
        tws_conn.role = 0;
        tws_conn.tx_tick_res = 0;
        tws_link.link->role = 0;
        bredr_frame_role_switch_2_master(tws_link.link->frame);
    }
    bredr_bd_frame_enable(tws_link.link->frame, 0, 0);
    tws_conn_send_event(10, "1", tws_conn.role);
}

static void tws_conn_adjust_clkoffset_enable(void *p)
{
    slot_timer_put((u8)(int)p);
    if (tws_link.link)
        tws_link.link->adjust_clkoffset_enable = 1;
}

static void tws_core_tx_jiffies_offset(void)
{
    tws_conn_send_cmd(24, "4", jiffies_msec() + log_get_time_offset() - (tws_link_read_slot_clk() >> 4));
}

static void tws_esco_tx_role_switch_req(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;

    if (conn == NULL)
        return;
    if (!is_tws_esco_working() || tws_conn.role != 0 || tws_conn.role_switch != 2)
        return;
    INFO("[Info]: [TWS]wait_esco_role_sw\n\r\n");
    if (!list_empty(&conn->link->rx_head)) {
        INFO("[Info]: [TWS]rx_head not list_empty\n\r\n");
        bredr_link_event(conn->link, 1, 5);
    }
    putchar('L');
    if (!list_empty(&conn->link->rx_head) || lmp_conn_idle(conn) == 0)
        return;
    putchar('E');
    if (!list_empty(&tws_conn.tx_pending) || link_conn_rx_empty(conn->link) == 0)
        return;
    putchar('F');
    if (link_conn_tx_empty(conn->link) == 0)
        return;
    putchar('G');
    if (link_conn_tx_empty(tws_link.link) == 0)
        return;
    putchar('H');
    tws_master_tx_role_switch_req();
}

static void tws_master_start_monitor_probe(void *p) TWS_CODE;

static void tws_master_tx_start_monitor_req(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    int slots = conn && conn->esco_conn ? 64 : 128;

    tws_conn.instant = bredr_offset2clkn(tws_link_read_slot_clk(), slots);
    tws_conn_send_cmd(7, "V41", 28, tws_conn.instant, low_latency_enable);
    slot_timer_set(tws_conn.slot_timer, tws_master_start_monitor_probe, NULL, slots - 16);
    INFO("[Info]: [TWS]tx_start_monitor_req: %x, %d\n\r\n", tws_conn.instant, slots);
}

static void tws_master_start_monitor_probe(void *p)
{
    slot_timer_set(tws_conn.slot_timer, tws_master_start_monitor, NULL, 16);
    link_task_run(&tws_link.link->task);
    bredr_bd_frame_disable(tws_link.link->frame);
}

/* Static inline in the SDK's frame header. */
static inline u8 bredr_hw_read_lt_addr(struct ctrl_frame *frame)
{
    return (frame->miscntl >> 4) & 7;
}

static void tws_master_tx_role_switch_req(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct bredr_tx_bulk *tx = tws_tx_bulk_alloc(54);
    struct lmp_esco *esco;
    u8 phone_acl, tws_acl, phone_esco, tws_esco;
    int tem;

    if (tx == NULL)
        return;
    link_task_try_run_slots(&tws_link.link->task, 232);
    if (is_tws_esco_working()) {
        esco = conn->esco_conn;
        phone_acl = tws_link.phone_link_acl_lt_addr;
        tws_acl = bredr_hw_read_lt_addr(tws_link.link->frame);
        phone_esco = esco->pdu.lt_addr;
        tws_esco = esco->pdu.tws_lt_addr;
        for (tem = 7; tem > 0; tem--) {
            if (tem != tws_acl && tem != phone_acl && tem != phone_esco && tem != tws_esco)
                break;
        }
        INFO("[Info]: [TWS]phone_esco_lt_addr=%x,tws_acl_addr=0x%x,phone_acl_addr%x,tws_esco_lt_addr%x,tem%x\n\r\n",
             phone_esco, tws_acl, phone_acl, tws_esco, tem);
        bredr_hw_write_lt_addr(conn->link->frame, tem);
    }
    tx->data[2] = 0;
    tx->data[3] = 0x82;
    tx->data[4] = 200;
    *(u32 *)&tx->data[8] = bredr_offset2clkn(tws_link_read_slot_clk(), 200);
    tx->len = 12;
    tx->len += tws_link_get_rxtx_state(conn->link, (struct tws_link_rxtx_state *)&tx->data[12]);
    tx->len += tws_role_switch_sync_profile_core_data_for_send(&tx->data[tx->len], 54 - tx->len);
    *(u16 *)tx->data = tx->len - 4;
    tws_conn.instant = *(u32 *)&tx->data[8];
    tws_conn.role_switch = 3;
    tws_conn_history_hash_clear();
    link_conn_tx_bulk_avaiable(tws_link.link, tx);
    DEBUG("[Debug]: [TWS]tws_master_tx_role_switch_req\n\r\n");
}
