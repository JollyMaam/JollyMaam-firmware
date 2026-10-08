/* TWS pairing, link state and synchronization routines recovered from the
 * BR23 controller.  Keep the SDK's timeout, channel and role-switch quirks. */
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "btctrler/glue.h"

#define TWS_CODE __attribute__((section(".classic_tws_code")))
#define TWS_WEAK __attribute__((weak, section(".classic_tws_code")))

extern BTCTRLER_CONFIG u8 log_tag_const_i_TWS_LMP, log_tag_const_d_TWS_LMP;
extern BTCTRLER_CONFIG int config_btctler_le_tws, CONFIG_BTCTLER_FAST_CONNECT_ENABLE, CONFIG_BTCTLER_TWS_ENABLE;
extern BTCTRLER_CONFIG int CONFIG_TWS_AFH_ENABLE, CONFIG_TWS_POWER_BALANCE_ENABLE;
extern void log_print(int level, const char *tag, const char *format, ...);
extern int printf(const char *format, ...);
extern int putchar(int c);
extern void printf_buf(const void *buffer, int size);

#define LOG_I(...) do { if (log_tag_const_i_TWS_LMP) log_print(2, NULL, __VA_ARGS__); } while (0)
#define LOG_D(...) do { if (log_tag_const_d_TWS_LMP) log_print(1, NULL, __VA_ARGS__); } while (0)

/* SDK services this member calls that the generated header does not list. */
struct lbuff_head;
struct lbuff_head *lbuf_init(void *buf, u32 len, int align, int priv_head_len);
void *lbuf_alloc(struct lbuff_head *head, u32 len);
int lbuf_free_space(struct lbuff_head *head);
int os_taskq_post_type(const char *name, int type, int argc, int *argv);
int os_sem_create(OS_SEM *sem, int count);
int os_sem_pend(OS_SEM *sem, int timeout);
void sys_event_notify(struct sys_event *event);
void tws_host_timeout_set(void);
void tws_host_timeout_del(void);
void tws_host_timeout_del_event(void);
int syscfg_read(u16 key, void *data, int len);
u8 get_esco_coder_busy_flag();
void put_buf(const u8 *buf, int len);
u32 tws_le_get_connect_aa(void);
u32 tws_le_get_pair_aa(void);
u32 tws_le_get_search_aa(void);
u16 bt_get_tws_device_indicate(u8 *addr);
u8 crc8(const u8 *data, u16 len);
u8 cheack_tws_connect_state();

/* The TWS sync channels the SDK links by section. */
extern struct tws_sync_channel tws_sync_channel_begin[], tws_sync_channel_end[];
extern const struct tws_func_stub tws_func_stub_begin[], tws_func_stub_end[];
extern u32 tws_bulk_pool[], tws_bulk_pool_end[];

/* ---- Globals -------------------------------------------------------- */

char tws_local_channel = 85;
u8 tws_auto_role_switch = 1;
u8 tws_common_addr_en = 1;
u8 tws_pair_all_way;
struct tws_connection tws_conn;
bool test_esco_role_switch_flag;
u8 *fast_search_data;
void (*tws_fast_search_cb)(u8 *, u8 *);

/* File-static in the SDK (tws_sys_timer is "sys_timer" there; the generated
 * header already uses that name for a type). */
static u8 tws_conn_state;
static u16 tws_sys_timer;
static _Bool sys_timer_disable __attribute__((aligned(4)));
static u16 rs_timer;

static void role_switch_state_machine(struct lmp_connection *conn) TWS_CODE;
static void tws_conn_master_tx_start_req(void) TWS_CODE;
static void tws_master_start_monitor(void *p) TWS_CODE;
static void tws_conn_slave_start_monitor(void *p) TWS_CODE;

static void cpu_reset(void) { p33_soft_reset(); }

static void put16(void *p, u16 v) { memcpy(p, &v, 2); }
static void put32(void *p, u32 v) { memcpy(p, &v, 4); }

/* ---- Host hooks the application may override -------------------------- */

int tws_host_get_local_role(void) TWS_WEAK;
int tws_host_get_local_role(void)
{
    return 0;
}

int tws_host_channel_match(char channel) TWS_WEAK;
int tws_host_channel_match(char channel)
{
    return 1;
}

char tws_host_get_local_channel(void) TWS_WEAK;
char tws_host_get_local_channel(void)
{
    return tws_local_channel;
}

void tws_host_get_common_addr(u8 *local_addr, u8 *remote_addr, u8 *common_addr, char channel) TWS_WEAK;
void tws_host_get_common_addr(u8 *local_addr, u8 *remote_addr, u8 *common_addr, char channel)
{
}

int tws_host_get_core_data(u8 *packet, int size) TWS_WEAK;
int tws_host_get_core_data(u8 *packet, int size)
{
    return 0;
}

int tws_host_set_core_data(u8 *packet, int size) TWS_WEAK;
int tws_host_set_core_data(u8 *packet, int size)
{
    return 0;
}

int tws_host_fliter_out_acl_data(u16 handle, u8 llid, u8 *data) TWS_WEAK;
int tws_host_fliter_out_acl_data(u16 handle, u8 llid, u8 *data)
{
    return 0;
}

/* ---- Bulk buffers on the TWS pool ------------------------------------- */

/* Receive allocation for the peer's TWS channels: opcode 6 sub-opcode 1 is
 * ACL data forwarding, opcode 12 local media; anything else gets a plain
 * bulk of the length in its first two bytes (at most 1024). */
struct bredr_rx_bulk *tws_rx_bulk_alloc_in_irq(int size, u8 *data, int *err) TWS_CODE;
struct bredr_rx_bulk *tws_rx_bulk_alloc_in_irq(int size, u8 *data, int *err)
{
    struct bredr_rx_bulk *bulk;
    u16 len;

    switch (data[2]) {
    case 6:
        if (data[3] == 1)
            *err = tws_data_sync_rx_bulk_alloc(data, size);
        return NULL;
    case 12:
        return tws_local_media_sync_rx_bulk_alloc(data, size, err);
    default:
        memcpy(&len, data, 2);
        if (len > 1024)
            return NULL;
        bulk = __bredr_rx_bulk_alloc(tws_conn.bulk_pool, len + 4);
        if (!bulk)
            *err = -12;
        return bulk;
    }
}

int tws_tx_bulk_free_space(void) TWS_CODE;
int tws_tx_bulk_free_space(void)
{
    return lbuf_free_space((struct lbuff_head *)tws_conn.bulk_pool);
}

/* A zeroed tx bulk with llid 2 (start of an L2CAP message), or NULL. */
struct bredr_tx_bulk *tws_tx_bulk_alloc(int size) TWS_CODE;
struct bredr_tx_bulk *tws_tx_bulk_alloc(int size)
{
    struct bredr_tx_bulk *tx = lbuf_alloc((struct lbuff_head *)tws_conn.bulk_pool, size + 28);

    if (tx) {
        memset(tx, 0, 28);
        tx->llid = 2;
    }
    return tx;
}

/* Ring of the last 16 hashes of frames received from the peer. */
void tws_conn_history_hash_add(int hash) TWS_CODE;
void tws_conn_history_hash_add(int hash)
{
    int i = tws_conn.history_hash_index;

    tws_conn.history_hash[i] = hash;
    i++;
    tws_conn.history_hash_index = i == 16 ? 0 : i;
}

void tws_conn_history_hash_clear(void) TWS_CODE;
void tws_conn_history_hash_clear(void)
{
    tws_conn.history_hash_index = 0;
    memset(tws_conn.history_hash, 0, 64);
}

/* ---- Small list helpers (file-static in the SDK) --------------------- */

static int list_empty(const struct list_head *head)
{
    return head->next == head;
}

static void __list_del_entry(struct list_head *entry)
{
    struct list_head *prev = entry->prev, *next = entry->next;

    next->prev = prev;
    prev->next = next;
}

static void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

static void list_add_tail(struct list_head *new, struct list_head *head)
{
    struct list_head *prev = head->prev;

    head->prev = new;
    new->next = head;
    new->prev = prev;
    prev->next = new;
}

/* ---- Connection state ------------------------------------------------- */

/* 1 when the earbud link exists, is not an eSCO link and the monitor is not
 * in the states 2 or 3. */
u8 get_esco_wait_start_monitor(void) TWS_CODE;
u8 get_esco_wait_start_monitor(void)
{
    struct link_connection *link = tws_link.link;

    if (tws_conn.lmp_conn != NULL && link != NULL && link->esco == NULL &&
        (tws_conn.monitor_start & -2) != 2)
        return 1;
    return 0;
}

void tws_conn_send_event_check(void) TWS_CODE;
void tws_conn_send_event_check(void)
{
    if (tws_conn.send_event == 0 && list_empty(&tws_conn.lmp_conn->link->rx_head))
        tws_conn.send_event = 1;
}

void tws_conn_try_send_event_to_lmp(void) TWS_CODE;
void tws_conn_try_send_event_to_lmp(void)
{
    if (tws_conn.send_event == 2 && bredr_link_event(tws_conn.lmp_conn->link, 1, 5) == 0)
        tws_conn.send_event = 0;
}

/* Called from the radio interrupt when the TWS link starts: reset both link
 * and connection state and hand the role to the TWS task (event 22). */
void tws_link_conn_start_in_irq(struct link_connection *lc, int role) TWS_CODE;
void tws_link_conn_start_in_irq(struct link_connection *lc, int role)
{
    struct lmp_connection *conn;
    u8 box_mode;

    conn = lmp_conn_for_address(NULL);
    memset(&tws_link, 0, 80);
    tws_link.link = lc;
    tws_conn_state = 0;
    if (tws_sys_timer != 0)
        sys_timer_disable = 1;
    box_mode = tws_conn.tws_box_mode;
    memset(&tws_conn, 0, 176);
    tws_conn.tws_box_mode = box_mode;
    tws_conn.role = role;
    tws_conn.lmp_conn = conn;
    tws_conn.run_slot = 100;
    tws_conn.slot_timer = slot_timer_get();
    tws_conn.bulk_pool = tws_bulk_pool;
    INIT_LIST_HEAD(&tws_conn.rx_pending);
    INIT_LIST_HEAD(&tws_conn.tx_pending);
    lbuf_init(tws_conn.bulk_pool, (((char *)tws_bulk_pool_end - (char *)tws_bulk_pool) / 4) * 4, 4, 0);
    os_taskq_post_type("tws", 22, 1, &role);
}

void tws_link_init(void) TWS_CODE;
void tws_link_init(void)
{
    tws_conn.monitor_start = 0;
    tws_conn.tws_box_mode = 0;
    memset(&tws_conn.esco_status, 0, 4);
}

/* How many slots the master gives the TWS link, from how much media is
 * queued locally and at the peer; limited to 32..80. */
u8 local_tws_run_slots_adjust(u8 packet_cnt) TWS_CODE;
u8 local_tws_run_slots_adjust(u8 packet_cnt)
{
    u8 remote_media_packet, wait_send_packet;
    int call, div, slots;
    u8 slot;

    remote_media_packet = 0;
    wait_send_packet = 0;
    call = tws_api_local_media_packet_cnt(&remote_media_packet, &wait_send_packet);
    div = tws_api_local_media_trans_get_total_buffer_size() / 679;
    if (tws_conn.tws_box_mode == 2)
        slots = (((div - remote_media_packet) << 2) & 1020) + wait_send_packet * 3;
    else
        slots = ((div - call) & 255) * 6;
    if (packet_cnt != 0)
        slots += packet_cnt * 12;
    slot = slots;
    if (slot >= 80)
        slot = 80;
    return slot > 32 ? slot : 32;
}

u8 check_tws_esco_resume_acl(void) TWS_CODE;
u8 check_tws_esco_resume_acl(void)
{
    return !(tws_conn.lmp_conn == NULL || tws_conn.role != 1 || tws_conn.detach == 0);
}

/* Role switch decision while an eSCO link is up (called from the phone
 * link's eSCO path): returns 1 when a switch was started. */
u8 tws_esco_check_role_switch(struct bredr_esco_link *phone_esco, u8 master_esco_num, u8 remote_esco_num,
                              u16 limit_check_esco_cnt) TWS_CODE;
u8 tws_esco_check_role_switch(struct bredr_esco_link *phone_esco, u8 master_esco_num, u8 remote_esco_num,
                              u16 limit_check_esco_cnt)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    int inc;

    if ((tws_conn.role | tws_conn.pure_monitor) != 0)
        return 0;
    LOG_I("[Info]: [TWS_LMP]k=%d,%d\r\n", master_esco_num, remote_esco_num);
    if (master_esco_num > limit_check_esco_cnt && master_esco_num + 6 <= remote_esco_num &&
        remote_esco_num > limit_check_esco_cnt) {
        inc = phone_esco->esco_try_switch + 1;
        phone_esco->esco_try_switch = inc;
        if (inc > 3) {
            LOG_I("[Info]: [TWS_LMP]esco_tws_conn_switch_role\n\r\n");
            phone_esco->esco_try_switch = 0;
            return 0;
        }
    } else {
        phone_esco->esco_try_switch = 0;
    }
    if (conn != NULL && tws_conn.detach != 0 && !conn->link->monitor) {
        LOG_I("[Info]: [TWS_LMP]esco_detch_role_switch\r\n");
        tws_conn_switch_role();
        phone_esco->esco_try_switch = 0;
        return 1;
    }
    if (test_esco_role_switch_flag == 0)
        return 0;
    LOG_I("[Info]: [TWS_LMP]test_esco_role_switch_flag\n\r\n");
    tws_conn_switch_role();
    test_esco_role_switch_flag = 0;
    return 1;
}

/* ---- Link task: resume and suspend ------------------------------------ */

/* The SDK's tws_link_update_bitoff() (static inline in tws_pair.c): move the
 * TWS link's bit offset a small step towards the phone link's, carrying into
 * the clock offset when it crosses a slot (625 us) boundary. */
static void tws_link_update_bitoff(struct link_connection *lc)
{
    int phone_bitoff = tws_conn.lmp_conn->link->signed_bitoff_ali;
    struct ctrl_frame *frame = lc->frame;
    u16 clk_lo = frame->clk_offsetl;
    u32 clk_offset = clk_lo | (frame->clk_offseth << 16);
    int bitoff = frame->bitoff & 1023;
    int diff, step, new_bitoff;
    int flag;

    if ((clk_lo & 1) != 0)
        bitoff += 625;
    if (phone_bitoff > bitoff) {
        diff = phone_bitoff - bitoff;
        flag = diff < 626;
    } else {
        diff = bitoff - phone_bitoff;
        flag = diff > 625;
    }
    step = diff & 65535;
    if (step > 10)
        step = 10;
    else if (step > 7)
        step = 4;
    else
        step = step > 3;
    if (!flag) {
        if (bitoff < step) {
            new_bitoff = bitoff + 625 - step;
            clk_offset += 1;
        } else if (bitoff < 625) {
            new_bitoff = bitoff - step;
        } else if (bitoff < step + 625) {
            new_bitoff = bitoff - step;
            clk_offset += 1;
        } else {
            new_bitoff = bitoff - step - 625;
        }
    } else {
        if (bitoff < 625 - step) {
            new_bitoff = step + bitoff;
        } else if (bitoff < 625) {
            new_bitoff = bitoff - 625 + step;
            clk_offset -= 1;
        } else if (bitoff < 1250 - step) {
            new_bitoff = step + bitoff - 625;
        } else {
            new_bitoff = step + bitoff + 64286;
            clk_offset -= 1;
        }
    }
    frame->bitoff = new_bitoff;
    __write_reg_clkoffset(frame, clk_offset);
}

/* Resume of the TWS link task: bring queued peer frames and the monitor
 * state up to date, keep the two links' bit offsets aligned and drive the
 * role switch / master tx requests per slot. */
static int tws_link_connection_resume(struct link_task *task) TWS_CODE;
static int tws_link_connection_resume(struct link_task *task)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    u8 monitor_start;

    DEBUG_SINGAL_TWS_CONNECTION(1);
    tws_sniff_ap_enter();
    monitor_start = tws_conn.monitor_start;
    if (conn != NULL && monitor_start == 2) {
        if (conn->link->esco != NULL && (u8)(conn->core.esco_state - 1) > 2 && tws_conn.esco_status == 0) {
            struct list_head *node = tws_conn.rx_pending.next, *next;
            int moved = 0;

            for (;; node = next, moved = 1) {
                next = node->next;
                if (node == &tws_conn.rx_pending)
                    break;
                LOG_I("[Info]: [TWS_LMP]<<<<<<<<<<<<<<<<esco_tws_conn.rx_pending_waring>>>>>>>>>>>>>>>\n\r\n");
                __list_del_entry(node);
                list_add_tail(node, &conn->link->rx_head);
            }
            if (moved)
                bredr_link_event(conn->link, 1, 5);
            tws_conn.esco_status = 1;
            if (tws_conn.role == 0) {
                tws_link.esco_tx_sync_req_cnt = 20;
                tws_monitor_enable(conn->link, 1, 1);
            } else {
                tws_monitor_enable(conn->link, 1, 0);
            }
            tws_data_sync_clear_abandon_num();
        }
        if (tws_conn.link_reset != 0) {
            int esco = is_tws_esco_working();
            u32 post_instant = tws_link.post_instant;
            if ((post_instant | esco) == 0) {
                struct link_connection *lc;
                struct list_head *node, *next;

                tws_link_rxtx_state_reset();
                tws_data_sync_resume();
                tws_local_media_sync_resume();
                /* Drop the TWS link's queued tx frames of the forwarded kinds
                 * (channel 0 with the opcode's top bit set, channels 6 and 12). */
                lc = tws_link.link;
                for (node = lc->tx_acl_u.next; node != &lc->tx_acl_u; node = next) {
                    struct bredr_tx_bulk *tx = (struct bredr_tx_bulk *)((u8 *)node - offsetof(struct bredr_tx_bulk, entry));

                    next = node->next;
                    switch (tx->data[2]) {
                    case 0:
                        if ((s8)tx->data[3] >= 0)
                            continue;
                        break;
                    case 6:
                    case 12:
                        break;
                    default:
                        continue;
                    }
                    __list_del_entry(node);
                    bredr_tx_bulk_free(tx);
                    lc = tws_link.link;
                }
            }
        }
        if (is_tws_esco_working() == 0)
            role_switch_state_machine(conn);
        if (!tws_link.link->role)
            tws_link_update_bitoff(tws_link.link);
        if ((tws_conn.phone_link_detach & 4) == 0) {
            if (tws_conn.role == 0) {
                if (is_tws_esco_working() == 0 && tws_conn.detach != 0 &&
                    ((conn->link->monitor) | tws_conn.role_switch) == 0)
                    tws_conn.role_switch = 1;
                if (is_tws_esco_working() == 1 && conn->link->esco->esco_acl_resume_t != 0) {
                    u8 cnt = conn->link->esco->esco_master_tx_request_cnt;

                    if (cnt % 12 == 0) {
                        tws_conn_master_tx_start_req();
                        cnt = conn->link->esco->esco_master_tx_request_cnt;
                    }
                    conn->link->esco->esco_master_tx_request_cnt = cnt + 1;
                } else if (is_tws_esco_working() == 1) {
                    if (conn->link->esco->esco_master_tx_request_cnt != 0) {
                        if (tws_conn.phone_link_detach == 0)
                            role_switch_state_machine(conn);
                        conn->link->esco->esco_master_tx_request_cnt = 0;
                    } else if (tws_link.acl_communication != 0) {
                        tws_conn_master_tx_start_req();
                        tws_link.acl_communication = 0;
                    }
                } else {
                    tws_conn_master_tx_start_req();
                }
            } else if (tws_conn.detach == 1) {
                if (is_tws_esco_working() == 1 && tws_link.esco_wait_role_swtich != 0) {
                    tws_link.esco_wait_role_swtich--;
                } else {
                    tws_conn.detach = 2;
                    tws_conn_send_cmd(14, "V1", 47, tws_conn.detach_code);
                }
            }
        }
        tws_link_read_slot_clk();
    } else if (monitor_start == 0 && tws_link.link != NULL) {
        if (tws_conn.tws_box_mode != 0) {
            u8 slots = local_tws_run_slots_adjust(0);

            if (tws_tx_bulk_free_space() >= 1146)
                tws_conn_master_tx_start_req();
            if (tws_conn.role == 0)
                link_tws_task_adjust(slots);
        } else if (link_conn_tx_empty(tws_link.link) != 0) {
            tws_conn_master_tx_start_req();
        }
    }
    link_conn_task_resume(task);
    return 0;
}

/* Suspend of the TWS link task: a pending role switch (state 7) is carried
 * out here, the A2DP packets the phone link no longer needs are cleared and
 * the monitor is started in the new role. */
static int tws_link_connection_suspend(struct link_task *task) TWS_CODE;
static int tws_link_connection_suspend(struct link_task *task)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;

    link_conn_task_suspend(task);
    if (conn != NULL && tws_conn.monitor_start == 2) {
        if (is_tws_esco_working() == 0 && tws_conn.role_switch == 7) {
            if (tws_conn.role == 0) {
                tws_link_master_2_slave(conn->link);
                tws_conn.role = 1;
                link_conn_follow_ctrl_disable(conn->link);
            } else {
                tws_link_slave_2_master(conn->link);
                tws_conn.role = 0;
            }
            tws_conn.role_switch = 0;
        }
        if (tws_conn.role == 0) {
            if (conn->clear_a2dp_packet_num != 0) {
                u32 begin = tws_link.link->super_timeout_begin;
                int offset = bredr_clkn2offset(begin, READ_SLOT_CLK());

                if (offset > 299)
                    __lmp_private_clear_a2dp_packet(&conn->rejust_esco_req, conn->clear_a2dp_packet_num);
            }
            tws_master_start_monitor(0);
        } else {
            tws_conn_slave_start_monitor(0);
        }
    }
    tws_sniff_ap_exit();
    DEBUG_SINGAL_TWS_CONNECTION(0);
    return 0;
}

const struct link_task_ops tws_link_task_ops __attribute__((section(".classic_tws_const"))) = {
    tws_link_connection_resume,
    tws_link_connection_suspend,
};

/* ---- eSCO setup on the monitor link ----------------------------------- */

/* Monitor reached its instant: open the eSCO link on the TWS side (or tell
 * the link layer about the eSCO link the monitor already carries). */
void start_monitor_creat_esco(void *p) TWS_CODE;
void start_monitor_creat_esco(void *p)
{
    struct lmp_connection *conn;
    struct link_connection *link;
    struct lmp_esco *esco;
    int slot_timer;

    LOG_I("[Info]: [TWS_LMP]start_monitor_creat_esco\n\r\n");
    conn = tws_conn.lmp_conn;
    if (conn == NULL)
        return;
    if (conn->core.esco_state == 4)
        return;
    conn->core.esco_state = 4;
    link = conn->link;
    if (link->monitor) {
        if (conn->esco_conn == NULL)
            return;
        tws_post_open_esco_link(&conn->rejust_esco_req);
        return;
    }
    if (link->esco == NULL)
        return;
    esco = conn->esco_conn;
    LOG_I("[Info]: [TWS_LMP]after_bredr_esco_tws_link_open =0x%x,%x\n\r\n", conn, esco->pdu.tws_lt_addr);
    local_irq_disable();
    slot_timer = tws_link.slot_instant_timer;
    if (slot_timer != 0) {
        slot_timer_put(slot_timer);
        tws_link.slot_instant_timer = 0;
    }
    tws_conn.esco_setup_flag = 1;
    conn->link->esco->pdu.tws_lt_addr = esco->pdu.tws_lt_addr;
    bredr_esco_tws_link_open(conn->link, 0);
    local_irq_enable();
    tws_conn_send_event(13, "11", tws_conn.role, 1);
}

void tws_post_open_esco_link(void *p) TWS_CODE;
void tws_post_open_esco_link(void *p)
{
    struct lmp_connection *conn = p;
    int slot_timer;

    if (conn == NULL || conn->esco_conn == NULL || tws_link.link == NULL)
        return;
    tws_conn.esco_setup_flag = 1;
    tws_link.post_step = LMP_TX_NULL;
    tws_link.post_instant = 0;
    slot_timer = tws_link.slot_instant_timer;
    if (slot_timer != 0) {
        slot_timer_put(slot_timer);
        tws_link.slot_instant_timer = 0;
    }
    bredr_link_event(conn->link, 1, 9);
}

/* ---- Events to the TWS task and the peer ------------------------------ */

/* Post a system event (type 16, tagged for the TWS task) carrying the code
 * and the arguments formatted from the string like tws_conn_send_cmd's. */
void tws_conn_send_event(int code, const char *format, ...) TWS_CODE;
void tws_conn_send_event(int code, const char *format, ...)
{
    va_list argptr;
    struct sys_event event;

    event.type = 16;
    event.arg = (void *)1415009024;
    event.u.bt.event = code;
    if (format != NULL) {
        va_start(argptr, format);
        __vsprintf(event.u.bt.args, format, argptr);
        va_end(argptr);
    }
    sys_event_notify(&event);
}

/* The phone link saw a role-switch request from the phone (LMP opcode 6/7,
 * extended opcode 51): mark it, and tell the peer the core data changed. */
void tws_phone_link_state_change(struct lmp_connection *conn, struct bredr_rx_bulk *rx, int *hci_cmd) TWS_CODE;
void tws_phone_link_state_change(struct lmp_connection *conn, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    if (rx != NULL && conn->core.role == 0 && conn->core.stage == LMP_STAGE_CONN &&
        (rx->data[0] & -2) == 6 && rx->data[1] == 51)
        conn->core.role_switch = 1;
    if (tws_conn.monitor_start == 2)
        tws_conn.lmp_core_data_sync = 1;
}

/* Offsets in the monitor link request (tx->data) that the peer parses. */
#define MON_CORE        4       /* 44 bytes of the phone link's lmp_core_data */
#define MON_CORE_S      48      /* 40 bytes of lmp_core_data_static */
#define MON_POWER       88      /* 16 bytes from link core.pwr_set */
#define MON_LINKCNTL    104     /* linkcntl0 bit 15, bit 7 */
#define MON_MISCNTL     106
#define MON_FORMAT      107
#define MON_CHMAP       108     /* 4 + 4 + 2 bytes from the radio's channel map table */
#define MON_BCHH        118
#define MON_BCHL        120
#define MON_BCHM        122
#define MON_BITOFF      124
#define MON_CLKOFFSET_L 126
#define MON_CLKOFFSET_H 128
#define MON_BDADDR0     130
#define MON_BDADDR1     132
#define MON_BDADDR2     134
#define MON_CRYPTK      136
#define MON_SNIFF       152
#define MON_ESCO_PDU    192

/* The SDK's tws_tx_setup_monitor_link_req() (static inline in tws_pair.c):
 * send the peer everything it needs to follow the phone link as a monitor:
 * the phone link's state, the radio frame registers and (when one exists)
 * the eSCO link's PDU.  conn->core.esco_state is set to 1 when an eSCO PDU
 * was included, 0 when not; it is left alone when nothing could be sent. */
static void tws_tx_setup_monitor_link_req(struct lmp_connection *conn)
{
    struct ctrl_frame *frame = conn->link->frame;
    struct bredr_tx_bulk *tx;
    struct lmp_esco *esco;
    u32 len;
    int clk_diff, bit_diff;
    u32 map_base, chmap0, chmap1;

    if (tws_conn.tws_box_mode != 0 && tws_tx_bulk_free_space() <= 1045)
        return;
    tx = tws_tx_bulk_alloc(1046);
    if (!tx)
        btctrler_assert_failed("tx != NULL", 0);
    tx->data[2] = 0;
    tx->data[3] = 6;
    tx->len = 208;
    len = tx->len + tws_host_get_core_data(&tx->data[208], 838);
    tx->len = len;
    if ((len & 65535) >= 1047)
        btctrler_assert_failed("tx->len <= max_len", tx->len);
    if (conn->esco_conn != NULL && conn->link->esco != NULL) {
        LOG_D("[Debug]: [TWS_LMP]tws_tx_setup_monitor_get_esco_pdu=0x%x\n\r\n", conn);
        esco = conn->esco_conn;
        conn->core.esco_state = 1;
        esco->pdu.tws_lt_addr = esco_creart_lt_addr();
        if (log_tag_const_d_TWS_LMP) {
            log_print(1, NULL, "[Debug]: [TWS_LMP]pdu->tws_lt_addr=0x%x\n\r\n", esco->pdu.tws_lt_addr);
            if (((u8 *)&esco->pdu)[1] & 0x80)
                log_print(1, NULL, "[Debug]: [TWS_LMP]<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<pdu->tws_lt_addr err>>>>>>>>>>>>>>>>>>>\n\r\n");
        }
        memcpy(&tx->data[MON_ESCO_PDU], &esco->pdu.handle, 14);
    } else {
        conn->core.esco_state = 0;
    }
    memcpy(&tx->data[MON_CORE], &conn->core, 44);
    memcpy(&tx->data[MON_CORE_S], &conn->core_s, 40);
    memcpy(&tx->data[MON_POWER], &conn->link->core.pwr_set, 16);
    if (conn->sniff != NULL && conn->core.operation_mode == 2)
        memcpy(&tx->data[MON_SNIFF], conn->sniff, 40);

    clk_diff = __read_reg_clkoffset(frame);
    bit_diff = frame->bitoff & 1023;
    clk_diff -= __read_reg_clkoffset(tws_link.link->frame);
    bit_diff -= tws_link.link->frame->bitoff & 1023;
    if (bit_diff < 0) {
        bit_diff += 625;
        clk_diff += 1;
    }
    tx->data[MON_MISCNTL] = frame->miscntl;
    tx->data[MON_FORMAT] = frame->format;
    put16(&tx->data[MON_BITOFF], bit_diff);
    put16(&tx->data[MON_CLKOFFSET_L], clk_diff);
    put16(&tx->data[MON_CLKOFFSET_H], clk_diff >> 16);
    put16(&tx->data[MON_BDADDR0], frame->bdaddr0);
    put16(&tx->data[MON_BDADDR1], frame->bdaddr1);
    put16(&tx->data[MON_BDADDR2], frame->bdaddr2);
    put16(&tx->data[MON_BCHL], frame->bchl);
    put16(&tx->data[MON_BCHM], frame->bchm);
    tx->data[MON_BCHH] = frame->bchh;
    tx->data[MON_LINKCNTL] = frame->linkcntl0 >> 15;
    tx->data[MON_LINKCNTL + 1] = (frame->linkcntl0 >> 7) & 1;
    memcpy(&tx->data[MON_CRYPTK], (const void *)frame->cryptk, 16);
    /* The radio keeps a 12-byte channel map record per hopping mode, indexed
     * by miscntl bits 6 and up; its third word is read and not used. */
    map_base = (frame->miscntl >> 6) * 12 + 1835140;
    chmap0 = *(volatile u32 *)map_base;
    chmap1 = *(volatile u32 *)(map_base + 4);
    (void)*(volatile u32 *)(map_base + 8);
    put32(&tx->data[MON_CHMAP], chmap0);
    put32(&tx->data[MON_CHMAP + 4], chmap1);
    put16(&tx->data[MON_CHMAP + 8], chmap1);
    tws_link_monitor_master_init(frame);
    put16(tx->data, tx->len - 4);
    link_conn_tx_bulk_avaiable(tws_link.link, tx);
}

/* Slot timer callback that waits for the phone link to be idle before the
 * monitor link is requested from the peer. */
static void check_for_setup_monitor_link(void *p) TWS_CODE;
static void check_for_setup_monitor_link(void *p)
{
    struct lmp_connection *conn = p;
    int slots;

    if (tws_conn.monitor_start != 0 || (conn->core.state & 8) != 0) {
        slot_timer_disable(tws_conn.slot_timer);
        return;
    }
    if (cheack_tws_connect_state() == 0) {
        slots = 64;
    } else if (lmp_conn_idle(p) == 0) {
        slots = 6;
    } else {
        tws_host_timeout_del_event();
        tws_conn.monitor_start = 1;
        conn->link->task.run_slot = 64;
        tws_conn.instant = bredr_offset2clkn(tws_link_read_slot_clk(), 64);
        tws_tx_setup_monitor_link_req(conn);
        if (conn->core.esco_state != 1)
            link_task_run_slots(&tws_link.link->task, 64);
        slot_timer_disable(tws_conn.slot_timer);
        return;
    }
    slot_timer_reset(tws_conn.slot_timer, slots);
}

/* Send an LMP-style command to the peer over the TWS link: opcode, then the
 * arguments formatted from the string; 'v' and 'V' set the event hooks of
 * the tx bulk (before and after sending) instead of adding a byte. */
int tws_conn_send_cmd(u8 opcode, const char *format, ...) TWS_CODE;
int tws_conn_send_cmd(u8 opcode, const char *format, ...)
{
    va_list argptr;
    struct bredr_tx_bulk *tx;
    u16 length;

    tx = tws_tx_bulk_alloc(24);
    if (!tx)
        return -12;
    tx->len = 4;
    tx->data[2] = 0;
    tx->data[3] = opcode;
    length = 0;
    if (format != NULL) {
        va_start(argptr, format);
        for (;; format++) {
            if (*format == 'v')
                tx->event_prob = va_arg(argptr, int);
            else if (*format == 'V')
                tx->event_post = va_arg(argptr, int);
            else
                break;
        }
        tx->len = tx->len + __vsprintf(&tx->data[4], format, argptr);
        va_end(argptr);
        length = tx->len - 4;
    }
    put16(tx->data, length);
    link_conn_tx_bulk_avaiable(tws_link.link, tx);
    return 0;
}

/* ---- Pairing and connecting (called from the TWS task) ---------------- */

static void tws_sys_timeout_del(void) TWS_CODE;
static void __disable_all_inquiry_and_scan(void) TWS_CODE;
static int __tws_scan_search_rx_handler(struct ble_rx *rx, u8 role) TWS_CODE;
static void tws_ble_search_timeout(void *p) TWS_CODE;
static void __page_sibling_timeout(u8 *addr, int err) TWS_CODE;

/* Stop whatever the connect/wait/search in progress (tws_conn_state) started. */
int tws_cancle_create_connection_internal(void) TWS_CODE;
int tws_cancle_create_connection_internal(void)
{
    tws_sys_timeout_del();
    switch (tws_conn_state) {
    case 4:
    case 6:
        LOG_I("[Info]: [TWS_LMP]-------------disable ble en\n\r\n");
        if (config_btctler_le_tws != 0) {
            link_le_adv_try_start_disable(1);
            link_le_scan_try_start_disable(1);
        }
        break;
    case 3:
        link_page_disable(254);
        link_page_scan_disable();
        break;
    case 5:
        link_page_scan_disable();
        break;
    }
    tws_conn_state = 0;
    return 0;
}

int tws_wait_connection_internal_bredr(int timeout) TWS_CODE;
int tws_wait_connection_internal_bredr(int timeout)
{
    int err;

    LOG_I("[Info]: [TWS_LMP]tws wait by bredr\n\r\n");
    err = __page_scan_enable(0, gbredr_local_dev.internal_addr_local, 1, 0);
    if (err == 0)
        tws_conn_state = 5;
    return err;
}

static int __tws_adv_connect_rx_handler(struct ble_rx *rx, u8 role) TWS_CODE;
static int __tws_scan_connect_rx_handler(struct ble_rx *rx, u8 role) TWS_CODE;

static int tws_wait_connection_internal_ble(int timeout) TWS_CODE;
static int tws_wait_connection_internal_ble(int timeout)
{
    int err;

    link_le_adv_try_start_disable(1);
    link_le_scan_try_start_disable(1);
    LOG_I("[Info]: [TWS_LMP]tws wait by ble %d\r\n", timeout);
    err = link_le_adv_try_start_enable(tws_le_get_connect_aa(), 7, 100, 3, 0, 1, -128, __tws_adv_connect_rx_handler);
    tws_conn_state = 6;
    return err;
}

/* Wait for the sibling to connect (page scan, or BLE advertising); -22 when
 * a link exists or is being set up, or the eSCO coder is busy. */
int tws_wait_connection_internal(int timeout) TWS_CODE;
int tws_wait_connection_internal(int timeout)
{
    struct lmp_connection *conn = lmp_conn_for_address(NULL);

    if (conn == NULL || tws_link.link != NULL || (conn->core.state & 8) != 0)
        return -22;
    if (get_esco_coder_busy_flag() != 0)
        return -22;
    put_buf(gbredr_local_dev.internal_addr_local, 6);
    tws_link.tws_ble_timeout = timeout;
    __disable_all_inquiry_and_scan();
    if (config_btctler_le_tws != 0)
        return tws_wait_connection_internal_ble(timeout);
    /* The SDK passes an undefined timeout here; the callee ignores it. */
    return tws_wait_connection_internal_bredr(timeout);
}

int tws_le_connect_in_esco_internal(void) TWS_CODE;
int tws_le_connect_in_esco_internal(void)
{
    if (config_btctler_le_tws == 0)
        return 0;
    if (tws_link.link != NULL)
        return -22;
    link_le_adv_try_start_disable(1);
    link_le_scan_try_start_disable(1);
    LOG_I("[Info]: [TWS_LMP]ble set info\n\r\n");
    return link_le_adv_esco_try_start_enable(tws_le_get_connect_aa(), 7, 100, 2, 0, 1, -128,
                                             __tws_adv_connect_rx_handler);
}

/* Advertising connect handler: the SDK does nothing here. */
static int __tws_adv_connect_rx_handler(struct ble_rx *rx, u8 role)
{
    return 0;
}

int tws_le_cancle_connect_in_esco_internal(void) TWS_CODE;
int tws_le_cancle_connect_in_esco_internal(void)
{
    if (config_btctler_le_tws != 0) {
        LOG_I("[Info]: [TWS_LMP]ble close info\n\r\n");
        link_le_adv_try_start_disable(1);
        link_le_scan_try_start_disable(1);
    }
    return 0;
}

static void tws_create_conneciton_timeout(void *p) TWS_CODE;

int tws_create_connection_internal_bredr(int timeout) TWS_CODE;
int tws_create_connection_internal_bredr(int timeout)
{
    int err;

    LOG_I("[Info]: [TWS_LMP]tws create by bredr\n\r\n");
    err = __page_enable(gbredr_local_dev.internal_addr_remote, 0, 0, gbredr_local_dev.internal_addr_local, 0, 1, 0);
    if (err != 0)
        return err;
    err = __page_scan_enable(0, gbredr_local_dev.internal_addr_local, 1, 1);
    if (err != 0) {
        link_page_disable(254);
        return err;
    }
    if (timeout != 0 && tws_sys_timer == 0)
        tws_sys_timer = sys_timeout_add(NULL, tws_create_conneciton_timeout, timeout);
    tws_conn_state = 3;
    return 0;
}

/* Connect attempt timed out: cancel it and tell the TWS task (event 3). */
static void tws_create_conneciton_timeout(void *p)
{
    struct lmp_connection *conn;

    tws_sys_timer = 0;
    if (tws_link.link != NULL)
        return;
    conn = lmp_conn_for_address(NULL);
    tws_cancle_create_connection_internal();
    tws_conn_send_event(3, "111", 0, conn != NULL, tws_conn_state);
}

int tws_create_connection_internal_ble(int timeout) TWS_CODE;
int tws_create_connection_internal_ble(int timeout)
{
    int err;

    LOG_I("[Info]: [TWS_LMP]\n\n\ntws create by ble %d\r\n", timeout);
    link_le_adv_try_start_disable(1);
    link_le_scan_try_start_disable(1);
    err = link_le_adv_try_start_enable(tws_le_get_connect_aa(), 7, 100, 1, 0, 1, 16, __tws_adv_connect_rx_handler);
    if (err != 0)
        return err;
    err = link_le_scan_try_start_enable(tws_le_get_connect_aa(), 7, timeout, 1, 0, -128,
                                        __tws_scan_connect_rx_handler);
    if (err != 0) {
        link_le_adv_try_start_disable(1);
        return err;
    }
    if (timeout != 0 && tws_sys_timer == 0)
        tws_sys_timer = sys_timeout_add(NULL, tws_create_conneciton_timeout, timeout);
    tws_conn_state = 4;
    return 0;
}

/* Scan connect handler: remember the connect info the peer advertised. */
static int __tws_scan_connect_rx_handler(struct ble_rx *rx, u8 role)
{
    tws_scan_write_connect_info(rx->channel, rx->data, role);
    return 0;
}

int tws_create_connection_internal_ble_fast_conn(int role_type) TWS_CODE;
int tws_create_connection_internal_ble_fast_conn(int role_type)
{
    int err = 0;

    link_le_adv_try_start_disable(1);
    link_le_scan_try_start_disable(1);
    if (CONFIG_BTCTLER_FAST_CONNECT_ENABLE != 0) {
        u32 aa = tws_le_get_connect_aa();

        if (role_type == 2)
            err = link_le_scan_try_start_enable(aa, 7, 0, 4, 0, -128, __tws_scan_connect_rx_handler);
        else
            err = link_le_adv_try_start_enable(aa, 7, 100, 4, 1, 1, 16, __tws_adv_connect_rx_handler);
        tws_conn_state = 4;
    }
    return err;
}

/* Connect to the sibling: waits up to 10 x 4 ticks for a pending LMP
 * connection to finish, then pages (BR/EDR) or advertises and scans (BLE).
 * -14 when the LMP connection never settled, -22 when a link exists. */
int tws_create_connection_internal(int timeout) TWS_CODE;
int tws_create_connection_internal(int timeout)
{
    int count = 0;
    struct lmp_connection *conn;

    do {
        conn = lmp_conn_for_address(NULL);
        if (conn == NULL) {
            if (tws_link.link != NULL)
                return -22;
            tws_link.tws_ble_timeout = timeout;
            __disable_all_inquiry_and_scan();
            if (config_btctler_le_tws != 0)
                return tws_create_connection_internal_ble(timeout);
            return tws_create_connection_internal_bredr(timeout);
        }
        if ((conn->core.state & 8) == 0)
            return -14;
        os_time_dly(4);
        count++;
    } while (count < 10);
    return -14;
}

/* Ask the TWS task (event 56) to drop the search timeout. */
void tws_ble_search_timeout_clr(void) TWS_CODE;
void tws_ble_search_timeout_clr(void)
{
    if (tws_sys_timer != 0) {
        sys_timer_disable = 1;
        os_taskq_post_type("tws", 56, 0, 0);
    }
}

/* The search record the BLE advertiser puts on the air. */
void tws_ble_get_search_info(struct tws_search_info *search_info) TWS_CODE;
void tws_ble_get_search_info(struct tws_search_info *search_info)
{
    u32 access_addr;

    local_irq_disable();
    search_info->pair_code = bt_get_tws_device_indicate(NULL);
    search_info->pc_crc = btctler_crc8((u8 *)&search_info->pair_code, 2);
    search_info->channel = tws_local_channel;
    access_addr = tws_le_get_pair_aa();
    search_info->access_addr = access_addr;
    if (fast_search_data != NULL)
        memcpy(search_info->data, fast_search_data, 14);
    gbredr_local_dev.tws_aa_wait_connect = access_addr;
    memcpy(search_info->addr, gbredr_local_dev.internal_addr_local, 6);
    local_irq_enable();
}

static void __ble_sibling_timeout(void *p) TWS_CODE;

void tws_le_adv_search_open(void *p) TWS_CODE;
void tws_le_adv_search_open(void *p)
{
    LOG_I("[Info]: [TWS_LMP]\nle adv open\n\r\n");
    link_le_adv_try_start_disable(0);
    link_le_scan_try_start_disable(0);
    link_le_adv_try_start_enable(gbredr_local_dev.tws_aa_wait_connect, 7, 100, 1, 1, 0, 8, __tws_adv_connect_rx_handler);
    link_task_schedule_resume();
    if (tws_sys_timer == 0)
        tws_sys_timer = sys_timeout_add(NULL, __ble_sibling_timeout, 8000);
}

/* Search window over: unless the timer was cancelled meanwhile, stop
 * advertising and report event 3. */
static void __ble_sibling_timeout(void *p)
{
    tws_sys_timer = 0;
    if (sys_timer_disable) {
        sys_timer_disable = 0;
        return;
    }
    link_le_adv_try_start_disable(1);
    tws_conn_send_event(3, NULL);
}

static int __tws_inquiry_result_handler(struct link_inquiry_result *result) TWS_CODE;

/* Search for the sibling: by BLE scan (config_btctler_le_tws) or by
 * inquiry for the general LAP, matching the pair code. */
int tws_api_search_sibling_by_code_internal(int code, int timeout_ms) TWS_CODE;
int tws_api_search_sibling_by_code_internal(int code, int timeout_ms)
{
    u8 lap[3] = {0x33, 0x8b, 0x9e};

    if (tws_link.link != NULL)
        return -22;
    if (config_btctler_le_tws != 0) {
        if (check_tws_ble_connecting() != 0) {
            LOG_I("[Info]: [TWS_LMP]\n !!!tws connection when search start!!!\n\r\n");
            return -22;
        }
        tws_conn.code = code;
        tws_conn_state = 2;
        __disable_all_inquiry_and_scan();
        LOG_I("[Info]: [TWS_LMP]\n\n\ntws seach by ble %d\r\n", timeout_ms);
        link_le_adv_try_start_disable(0);
        link_le_scan_try_start_disable(0);
        link_le_scan_try_start_enable(tws_le_get_search_aa(), 7, timeout_ms, 0, 0, 8, __tws_scan_search_rx_handler);
        if (timeout_ms != 0 && tws_sys_timer == 0)
            tws_sys_timer = sys_timeout_add(NULL, tws_ble_search_timeout, timeout_ms);
        return 0;
    }
    tws_conn.code = code;
    tws_conn_state = 2;
    __disable_all_inquiry_and_scan();
    if (CONFIG_BREDR_INQUIRY != 0)
        link_inquiry_enable(lap, timeout_ms, 1, __tws_inquiry_result_handler);
    return 0;
}

static void __page_sibling_timeout(u8 *addr, int err);

/* Inquiry result (or NULL at the end of the inquiry, event 1): the
 * sibling advertises its pair code, the code's CRC8, its channel and its
 * address in manufacturer data (type 0xff) of the EIR.  On a match, page it. */
static int __tws_inquiry_result_handler(struct link_inquiry_result *result)
{
    u16 code;
    int len;
    u8 *eir;

    if (result == NULL) {
        link_le_scan_try_start_disable(0);
        tws_conn_send_event(1, NULL);
        return 0;
    }
    LOG_D("[Debug]: [TWS_LMP]inquiry_result: device = %x\n\r\n", result->remote.class_dev);
    if (log_tag_const_i_TWS_LMP) {
        printf_buf(result->remote.addr, 6);
        printf_buf(result->eir_data, result->eir_len);
    }
    len = result->eir_len;
    eir = result->eir_data;
    while (len != 0) {
        int field = eir[0];

        if (field == 0)
            break;
        if (eir[1] == 0xff) {
            code = eir[3] << 8 | eir[2];
            if (code == tws_conn.code && crc8((u8 *)&code, 2) == eir[4] && tws_host_channel_match(eir[5]) != 0) {
                memcpy(gbredr_local_dev.internal_addr_remote, eir + 6, 6);
                __disable_all_inquiry_and_scan();
                __page_enable(gbredr_local_dev.internal_addr_remote, result->remote.sr, 0,
                              gbredr_local_dev.internal_addr_local, 8000, 0, __page_sibling_timeout);
                return 0;
            }
        }
        len -= field + 1;
        eir += field + 1;
    }
    return 0;
}

static void tws_wait_pair_by_ble(void) TWS_CODE;
static void tws_wait_pair_by_ble(void)
{
    link_le_adv_try_start_disable(0);
    link_le_scan_try_start_disable(0);
    link_le_adv_try_start_enable(tws_le_get_search_aa(), 7, 100, 0, 0, 1, 2, NULL);
    link_le_scan_try_start_enable(tws_le_get_pair_aa(), 7, 0, 1, 1, -128, __tws_scan_connect_rx_handler);
}

int tws_api_wait_pair_by_ble_internal(int code, const char *name) TWS_CODE;
int tws_api_wait_pair_by_ble_internal(int code, const char *name)
{
    if (config_btctler_le_tws != 0 && tws_link.link == NULL)
        tws_wait_pair_by_ble();
    tws_conn_state = 1;
    return 0;
}

/* Our EIR for pair discovery: the device name (type 9, when given) and
 * manufacturer data (type 0xff) with the pair code, its CRC8, our channel
 * and our address.  The EIR type is the packet type that fits its length. */
static void tws_build_pair_eir(int *code_addr, const char *name, u8 *eir_data, struct bredr_local_eir *eir) TWS_CODE;
static void tws_build_pair_eir(int *code_addr, const char *name, u8 *eir_data, struct bredr_local_eir *eir)
{
    u8 *data;
    int code;

    eir->sr = 1;
    eir->enable = 1;
    eir->type = 4;
    eir->data = eir_data;
    if (name == NULL) {
        data = eir_data;
    } else {
        u8 *p;

        eir_data[0] = strlen(name) + 2;
        eir_data[1] = 9;
        p = &eir_data[2];
        strcpy((char *)p, name);
        p += strlen(name);
        *p = 0;
        data = p + 1;
    }
    code = *code_addr;
    data[0] = 11;
    data[1] = 0xff;
    data[2] = code;
    data[3] = code >> 8;
    data[4] = crc8((u8 *)code_addr, 2);
    data[5] = tws_local_channel;
    memcpy(data + 6, gbredr_local_dev.internal_addr_local, 6);
    eir->len = (data + 12) - eir->data;
    eir->type = eir->len < 18 ? 3 : eir->len < 27 ? 4 : 10;
}

/* Wait for the sibling to pair: publish the pair code (CRC8, channel and
 * our address) in manufacturer data of our EIR, enable inquiry scan and
 * page scan (BR/EDR) or BLE advertising and scanning. */
int tws_api_wait_pair_by_code_internal(int code, const char *name) TWS_CODE;
int tws_api_wait_pair_by_code_internal(int code, const char *name)
{
    int code_addr = code;
    u8 addr[12];
    u8 eir_data[240];
    struct bredr_local_eir eir;
    int count = 0, linked, err, le;
    struct lmp_connection *conn;

    for (;;) {
        conn = lmp_conn_for_address(NULL);
        if (conn == NULL) {
            linked = 0;
            break;
        }
        if ((conn->core.state & 8) == 0)
            return -14;
        os_time_dly(4);
        count++;
        if (count >= 10) {
            linked = 1;
            break;
        }
    }
    LOG_I("[Info]: [TWS_LMP]tws_api_wait_pair: %s\n\r\n", name);
    tws_build_pair_eir(&code_addr, name, eir_data, &eir);
    __disable_all_inquiry_and_scan();
    if (tws_link.link == NULL) {
        if (tws_conn.monitor_start != 0)
            return -22;
    } else if (!(tws_conn.monitor_start == 0 && !(linked | (tws_conn.role == 1)))) {
        return -22;
    }
    err = __inquiry_scan_enable(0, hci_param.addr, &eir);
    if (err != 0)
        return err;
    le = config_btctler_le_tws;
    if (tws_link.link != NULL || le != 0) {
        err = __page_scan_enable(0, hci_param.addr, 1, 0);
        if (err == 0 && le != 0 && tws_link.link == NULL)
            tws_wait_pair_by_ble();
    } else {
        memcpy(addr, hci_param.addr, 6);
        memcpy(&addr[6], gbredr_local_dev.internal_addr_local, 6);
        err = __page_scan_enable(0, addr, 2, 0);
    }
    if (err != 0) {
        link_inquiry_scan_disable();
        return err;
    }
    tws_conn_state = 1;
    return 0;
}

int tws_api_cancle_wait_pair_internal(void) TWS_CODE;
int tws_api_cancle_wait_pair_internal(void)
{
    if (tws_conn_state == 1) {
        tws_conn_state = 0;
        link_inquiry_scan_disable();
        link_page_scan_disable();
    }
    __disable_all_inquiry_and_scan();
    if (config_btctler_le_tws != 0) {
        link_le_adv_try_start_disable(0);
        link_le_scan_try_start_disable(0);
    }
    return 0;
}

int tws_api_remove_pairs_internal(void) TWS_CODE;
int tws_api_remove_pairs_internal(void)
{
    if (tws_link.link == NULL)
        return -22;
    if (tws_conn.monitor_start == 0)
        tws_conn_send_cmd(8, "V", 48);
    return 0;
}

/* ---- Phone link detach and core data sync ----------------------------- */

static void do_phone_link_detach(void *priv) TWS_CODE;

/* Core data from the peer's phone link: either the 4-byte detach instant,
 * or the 44-byte core (whose esco_state we keep when it was set). */
int tws_conn_sync_rx_core_data(struct lmp_connection *conn, u8 *rxbuffer, int len, u8 *txbuffer) TWS_CODE;
int tws_conn_sync_rx_core_data(struct lmp_connection *conn, u8 *rxbuffer, int len, u8 *txbuffer)
{
    u8 esco_state = conn->core.esco_state;
    u32 instant;

    if (len == 4) {
        if (tws_conn.phone_link_detach_instant == 0) {
            u8 timer;
            int offset;

            memcpy(&instant, rxbuffer, 4);
            tws_conn.phone_link_detach_instant = instant;
            {
                u32 slot_clk = tws_link_read_slot_clk();
                memcpy(&instant, rxbuffer, 4);
                offset = bredr_clkn2offset(slot_clk, instant);
            }
            timer = slot_timer_get();
            slot_timer_set(timer, do_phone_link_detach, (void *)(u32)timer, offset > 2 ? offset : 2);
        }
        return 0;
    }
    memcpy(&conn->core.stage, rxbuffer, 44);
    if (esco_state != 0)
        conn->core.esco_state = esco_state;
    return 0;
}

/* The phone link is gone on the peer: release the waiter, or detach our
 * side with the reason, and drop everything still queued from the peer. */
static void do_phone_link_detach(void *priv)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct list_head *node, *next;

    slot_timer_put((u8)(u32)priv);
    slot_timer_disable(tws_conn.slot_timer);
    if ((tws_conn.phone_link_detach & 1) != 0) {
        os_sem_post(tws_conn.sem_phone_detach);
    } else if (conn != NULL && conn->link != NULL) {
        bredr_link_event(conn->link, 2, 14, tws_conn.detach_code);
        lmp_private_clear_a2dp_packet(&conn->rejust_esco_req, 0);
    }
    for (node = tws_conn.rx_pending.next; node != &tws_conn.rx_pending; node = next) {
        next = node->next;
        __list_del_entry(node);
        bredr_rx_bulk_free((struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry)));
    }
    tws_conn.monitor_start = 0;
    tws_conn.phone_link_detach = 0;
    tws_conn.phone_link_detach_instant = 0;
}

/* What we send the peer as core data: the detach instant (4 bytes) while
 * the phone link is detaching, the 44-byte core when it changed, or nothing. */
static int tws_conn_sync_tx_core_data(struct lmp_connection *conn, u8 *txbuffer, int prev_req_err) TWS_CODE;
static int tws_conn_sync_tx_core_data(struct lmp_connection *conn, u8 *txbuffer, int prev_req_err)
{
    u32 instant;

    if ((tws_conn.phone_link_detach & 1) != 0) {
        instant = tws_conn.phone_link_detach_instant;
        if (instant == 0) {
            u8 run_slot = tws_conn.run_slot;
            int add = run_slot * 3 + (run_slot >> 1);
            u8 timer;

            tws_conn.phone_link_detach_instant = add + tws_link_read_slot_clk();
            timer = slot_timer_get();
            slot_timer_set(timer, do_phone_link_detach, (void *)(u32)timer, add);
            instant = tws_conn.phone_link_detach_instant;
        }
        put32(txbuffer, instant);
        return 4;
    }
    if (tws_conn.lmp_core_data_sync == 0)
        return 0;
    memcpy(txbuffer, &conn->core.stage, 44);
    return 44;
}

void tws_esco_remove_clear_state(void) TWS_CODE;
void tws_esco_remove_clear_state(void)
{
    memset(&tws_conn.esco_status, 0, 4);
    tws_link.post_step = LMP_TX_NULL;
    tws_link.post_instant = 0;
    tws_link.esco_rx_lmp_slot_clk = 0;
    tws_link.esco_rx_lmp_cmd = 0;
}

/* ---- eSCO setup, removal and role switch over the TWS link ------------- */

/* Step message from the master (opcode 13): event 1 opens, 2 removes the
 * eSCO link, 3 switches roles; the first byte is the event, the second the
 * step (retry count) and bytes 2..5 the instant. */
u8 tws_slave_rx_notice_step_fun(u8 *rx) TWS_CODE;
u8 tws_slave_rx_notice_step_fun(u8 *rx)
{
    u8 event = rx[0], step = rx[1];
    u32 instant = rx[2] | rx[3] << 8 | rx[4] << 16 | (u32)rx[5] << 24;
    struct lmp_connection *conn = tws_conn.lmp_conn;
    int slots;

    if (tws_link.link == NULL || conn == NULL)
        return 0;
    if (tws_conn.monitor_start != 2) {
        tws_conn.esco_wait_monitor_start = 0;
        return 0;
    }
    switch (event) {
    case 1:
        if (step < 2) {
            tws_link.post_step = LMP_TX_VERSION_REQ;
            tws_fast_setup_esco_link(conn, (struct lmp_esco_pdu *)(rx + 6), 0);
            slots = tws_step_fun_start(tws_post_open_esco_link, instant);
            tws_link.post_instant = instant;
            tws_link.remove_esco_timeout = 0;
            link_task_run_slots(&tws_link.link->task, slots + 20);
            tws_monitor_enable(tws_conn.lmp_conn->link, 1, 0);
            LOG_I("[Info]: [TWS_LMP]slave_tws_post_open_esco_link_post:  %d,%d, %d,%d\n\r\n",
                  tws_link.slot_instant_timer, slots, instant, tws_link.post_step);
        }
        break;
    case 2:
        if (step < 2) {
            tws_esco_set_acl_lt_addr(1);
            tws_slave_remove_esco_deal(instant, 0);
        }
        break;
    case 3:
        if (step == 0 && tws_conn.role_switch == 3) {
            slots = tws_step_fun_start(tws_esco_wait_post_role_switch, instant);
            LOG_I("[Info]: [TWS_LMP]tws_post_esco_role_switch_link_post:  %d,%d, %d,%d\n\r\n",
                  tws_link.slot_instant_timer, slots, instant, tws_link.post_step);
        }
        break;
    }
    return 1;
}

/* Run the function the peer asked for by id (the SDK's tws_func_stub table). */
static void tws_function_call_by_id(int id, u8 *data, u16 len, u8 rx)
{
    const struct tws_func_stub *p;

    for (p = tws_func_stub_begin; p < tws_func_stub_end; p++) {
        if (p->func_id == id) {
            p->func(data, len, rx);
            break;
        }
    }
}

/* 1 while an eSCO link is in use on the TWS link or on the phone link with
 * a period under 12 slots. */
int is_tws_esco_working(void) TWS_CODE;
int is_tws_esco_working(void)
{
    struct link_connection *link;
    struct lmp_connection *conn;

    local_irq_disable();
    link = tws_link.link;
    conn = tws_conn.lmp_conn;
    if (link != NULL && conn != NULL) {
        if (link->esco != NULL) {
            local_irq_enable();
            return tws_conn.esco_status;
        }
        if (conn->link != NULL && conn->link->esco != NULL && conn->link->esco->pdu.T_sco < 12) {
            local_irq_enable();
            return 1;
        }
    }
    local_irq_enable();
    return 0;
}

/* Master side of the eSCO steps: event 1 opens the link (step 0 waits until
 * the monitor link is quiet, steps 1 and 2 send the step message), 2 removes
 * it, 3 switches roles.  Returns 0 only when it had nothing it could act on. */
u8 tws_master_notice_slave_step_fun(u8 event, u8 reason, u8 step) TWS_CODE;
u8 tws_master_notice_slave_step_fun(u8 event, u8 reason, u8 step)
{
    struct link_connection *link = tws_link.link;
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct lmp_esco *esco;
    int slots, offset;

    if (link == NULL || conn == NULL)
        return 0;
    switch (event) {
    case 1:
        if (tws_conn.role != 0)
            break;
        switch (step) {
        case LMP_TX_NULL:
            if (conn->esco_conn == NULL)
                return 0;
            if (tws_conn.monitor_start != 2 || tws_sniff.enable || !list_empty(&tws_conn.rx_pending) ||
                !list_empty(&link->tx_acl_u)) {
                if (tws_link.slot_instant_timer == 0)
                    tws_link.slot_instant_timer = slot_timer_get();
                LOG_I("[Info]: [TWS_LMP]tws_esco_wait_monitor\n\r\n");
                local_irq_disable();
                slot_timer_set(tws_link.slot_instant_timer, tws_esco_wait_monitor, tws_conn.lmp_conn, 10);
                local_irq_enable();
                break;
            }
            if (&link->task != get_curr_task()) {
                if (tws_link.slot_instant_timer == 0)
                    tws_link.slot_instant_timer = slot_timer_get();
                LOG_I("[Info]: [TWS_LMP]tws_esco_wait_tws_rusume\n\r\n");
                local_irq_disable();
                slot_timer_set(tws_link.slot_instant_timer, tws_esco_wait_tws_rusume, tws_conn.lmp_conn, 2);
                local_irq_enable();
                break;
            }
            esco = tws_conn.lmp_conn->esco_conn;
            esco->pdu.tws_lt_addr = esco_creart_lt_addr();
            if (log_tag_const_i_TWS_LMP && (esco->pdu.tws_lt_addr & 8))
                log_print(2, NULL, "[Info]: [TWS_LMP]<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<pdu->tws_lt_addr err>>>>>>>>>>>>>>>>>>>\n\r\n");
            slots = tws_send_esco_step_fun(1, tws_post_open_esco_link, 0, 49, 300);
            LOG_I("[Info]: [TWS_LMP]TWS_OPEN_ESCO_LINK_POST=%d,%d,%d,%d,pdu->tws_lt_addr=%d\n\r\n",
                  tws_link.post_instant, 300, reason, slots, esco->pdu.tws_lt_addr);
            tws_link.post_step = LMP_TX_VERSION_REQ;
            break;
        case LMP_TX_FEATURES_REQ:
            if (tws_conn.esco_setup_flag != 0) {
                LOG_I("[Info]: [TWS_LMP]esco_setup_flag ==1\n\r\n");
                break;
            }
            {
                u32 clk = tws_link_read_slot_clk();
                u32 post_instant = tws_link.post_instant;
                offset = bredr_clkn2offset(clk, post_instant);
            }
            if (offset < 160) {
                tws_link.post_step = LMP_TX_FEATURES_REQ_EXT;
                slots = tws_step_fun_start(tws_post_open_esco_link, tws_link.post_instant);
                link_task_run_slots(&tws_link.link->task, slots + 20);
                LOG_I("[Info]: [TWS_LMP]NOTICE_SLAVE_STEP1_over =%x,%x,%x,%x\n\r\n", tws_link.post_instant, 160,
                      reason, slots);
            } else {
                slots = tws_send_esco_step_fun(1, tws_post_open_esco_link, 1, 50, 160);
                tws_link.post_step = LMP_WAIT_VERSION_RES;
                link_task_run_slots(&tws_link.link->task, slots);
                LOG_I("[Info]: [TWS_LMP]NOTICE_SLAVE_STEP1 =%x,%x,%x,%x\n\r\n", tws_link.post_instant, 160, reason,
                      slots);
            }
            tws_monitor_enable(tws_conn.lmp_conn->link, 1, 1);
            break;
        case LMP_WAIT_FEATURES_RES:
            slots = tws_step_fun_start(tws_post_open_esco_link, tws_link.post_instant);
            link_task_run_slots(&tws_link.link->task, slots + 20);
            tws_link.post_step = LMP_TX_FEATURES_REQ_EXT;
            LOG_I("[Info]: [TWS_LMP]NOTICE_SLAVE_STEP2 =%x,%x,%x,%x\n\r\n", tws_link.post_instant, 160, reason,
                  slots);
            break;
        }
        break;
    case 2: {
        u8 setup_flag, role;

        LOG_I("[Info]: [TWS_LMP]TWS_REMOVE_ESCO_LINK_STEP_FUN\n\r\n");
        conn = tws_conn.lmp_conn;
        if (conn->rx_esco_req_after_remove_esco != 0) {
            printf("rx_esco_req_after_remove_esco=%d\n", conn->rx_esco_req_after_remove_esco);
            return 0;
        }
        if (tws_conn.monitor_start != 2) {
            tws_conn.esco_wait_monitor_start = 0;
            if ((tws_conn.role | tws_conn.esco_setup_flag) != 0 || conn->esco_conn == NULL)
                return 0;
            LOG_I("[Info]: [TWS_LMP]<<<<<<<<<<<<<<<<<<<<<<wait_monitor_rx_remove_esco_req_before_esco\n\r\n");
            tws_post_remove_esco_link(tws_conn.lmp_conn);
            break;
        }
        setup_flag = tws_conn.esco_setup_flag;
        role = tws_conn.role;
        if (setup_flag != 0 && role == 0) {
            switch (step) {
            case LMP_TX_NULL:
                if (tws_link.link->esco != NULL)
                    tws_link.link->esco->rx_remove_esco_req_flag = 60;
                esco = conn->esco_conn;
                /* The SDK stores the state without the NULL check it made for the reason. */
                if (esco != NULL)
                    esco->reason = reason;
                esco->state = 5;
                slots = tws_send_esco_step_fun(2, tws_post_remove_esco_link, 0, 51, 400);
                LOG_I("[Info]: [TWS_LMP]NOTICE_SLAVE_STEP_START=%x,%x,%x,%x\n\r\n", tws_link.post_instant, 400,
                      reason, slots);
                tws_link.post_step = LMP_WAIT_FEATURES_RES_EXT;
                tws_esco_set_acl_lt_addr(1);
                break;
            case LMP_TX_FEATURES_REQ:
                {
                    u32 clk = tws_link_read_slot_clk();
                    u32 post_instant = tws_link.post_instant;
                    offset = bredr_clkn2offset(clk, post_instant);
                }
                if (offset < 80) {
                    slots = tws_step_fun_start(tws_post_remove_esco_link, tws_link.post_instant);
                    link_task_run_slots(&tws_link.link->task, slots + 20);
                    tws_link.post_step = LMP_WAIT_NAME_RES;
                    LOG_I("[Info]: [TWS_LMP]NOTICE_SLAVE_STEP2 over =%x,%x,%x,%x\n\r\n", tws_link.post_instant, 80,
                          reason, slots);
                } else {
                    bredr_esco_close_slot_time(tws_conn.lmp_conn->link);
                    slots = tws_send_esco_step_fun(2, tws_post_remove_esco_link, 1, 52, 80);
                    tws_link.post_step = LMP_TX_NAME_REQ;
                    LOG_I("[Info]: [TWS_LMP]NOTICE_SLAVE_STEP1 =%x,%x,%x,%x\n\r\n", tws_link.post_instant, 80,
                          reason, slots);
                }
                break;
            case LMP_WAIT_FEATURES_RES:
                tws_link.post_step = LMP_WAIT_NAME_RES;
                slots = tws_step_fun_start(tws_post_remove_esco_link, tws_link.post_instant);
                link_task_run_slots(&tws_link.link->task, slots + 20);
                LOG_I("[Info]: [TWS_LMP]NOTICE_SLAVE_STEP2 =%x,%x,%x,%x\n\r\n", tws_link.post_instant, 160, reason,
                      slots);
                break;
            }
        } else if ((role | setup_flag) == 0) {
            if ((u8)(tws_link.post_step - 3) < 3) {
                LOG_I("[Info]: [TWS_LMP]rx_remove_esco_req_before_esco_setup 1\n\r\n");
                tws_conn.lmp_conn->rx_remove_esco_req_before_esco_setup = 1;
            } else if (conn->esco_conn != NULL) {
                LOG_I("[Info]: [TWS_LMP]rx_remove_esco_req_before_esco\n\r\n");
                tws_post_remove_esco_link(tws_conn.lmp_conn);
            }
        } else if (setup_flag != 0 && role == 1) {
            LOG_I("[Info]: [TWS_LMP]slave_bredr_esco_close_slot_time\n\r\n");
            if (tws_link.link != NULL)
                tws_link.remove_esco_timeout = 400;
        }
        break;
    }
    case 3:
        if (tws_conn.esco_setup_flag != 0 && tws_conn.role == 1) {
            switch (step) {
            case LMP_TX_NULL:
                tws_link.esco_tx_sync_req_cnt = 20;
                slots = tws_send_esco_step_fun(3, tws_esco_wait_role_switch_timeout, 0, 53, 50);
                LOG_I("[Info]: [TWS_LMP]NOTICE_ROLE_SWITCH_STEP_START=%x,%x,%x,%x\n\r\n", tws_link.post_instant, 50,
                      reason, slots);
                break;
            case LMP_TX_FEATURES_REQ:
                slots = tws_step_fun_start(tws_esco_wait_post_role_switch, tws_link.post_instant);
                LOG_I("[Info]: [TWS_LMP]_ROLE_SWITCH_STEP_post=%x,%x,%x,%x\n\r\n", tws_link.post_instant, 160, reason,
                      slots);
                break;
            }
        }
        break;
    }
    return 1;
}

/* Give the phone link's ACL a free logical-transport address while the TWS
 * link holds the eSCO (suspend_lt_addr != 0), or restore it. */
void tws_esco_set_acl_lt_addr(u8 suspend_lt_addr) TWS_CODE;
void tws_esco_set_acl_lt_addr(u8 suspend_lt_addr)
{
    struct link_connection *link = tws_link.link;
    struct lmp_connection *conn = tws_conn.lmp_conn;
    u8 saved;
    int i;

    if (link == NULL || conn == NULL)
        return;
    saved = tws_link.phone_link_acl_lt_addr;
    if (suspend_lt_addr != 0 && saved == 0) {
        struct lmp_esco *esco = conn->esco_conn;
        struct ctrl_frame *frame = conn->link->frame;
        u8 acl_lt = frame->miscntl & 7;
        u8 pdu_byte;

        tws_link.phone_link_acl_lt_addr = acl_lt;
        memcpy(&pdu_byte, (u8 *)&esco->pdu + 1, 1);
        for (i = 7; i > 0; i--) {
            if (!(i == (link->frame->miscntl & 7) || i == acl_lt || i == (pdu_byte & 15) || i == (pdu_byte >> 4)))
                break;
        }
        tws_link.phone_link_acl_lt_addr = acl_lt | 0x80;
        __write_reg_lt_addr(frame, i);
        return;
    }
    if (saved != 0)
        __write_reg_lt_addr(conn->link->frame, saved & 7);
    tws_link.phone_link_acl_lt_addr = 0;
}

/* Take the eSCO link down: restore the monitor and ACL state and send the
 * response the phone's remove request is waiting for (or release the waiter
 * when the super timeout did it). */
void tws_post_remove_esco_link(void *p) TWS_CODE;
void tws_post_remove_esco_link(void *p)
{
    struct lmp_connection *conn = p;
    u8 post_step;
    u8 tran_id;

    if (conn == NULL || conn->esco_conn == NULL || tws_link.link == NULL)
        return;
    post_step = tws_link.post_step;
    if (post_step == 0 && log_tag_const_d_TWS_LMP != 0) {
        log_print(1, NULL, "[Debug]: [TWS_LMP]remove_esco_link resson 0\n\r\n");
        post_step = tws_link.post_step;
    }
    if (post_step != 0) {
        if (tws_conn.role == 0) {
            if (tws_conn.pure_monitor == 0)
                tws_monitor_enable(conn->link, 0, 1);
        } else {
            tws_monitor_enable(conn->link, 0, 0);
        }
    }
    tws_esco_set_acl_lt_addr(0);
    tran_id = conn->esco_conn->rx_esco_tran_id;
    if (tran_id != conn->tran_id)
        conn->tran_id = tran_id;
    remove_esco_link(conn, 19);
    memset(&tws_conn.esco_status, 0, 4);
    tws_link.post_step = LMP_TX_NULL;
    tws_link.post_instant = 0;
    if (tws_link.slot_instant_timer != 0) {
        slot_timer_put(tws_link.slot_instant_timer);
        tws_link.slot_instant_timer = 0;
    }
    tws_conn.lmp_conn->rx_remove_esco_req_before_esco_setup = 0;
    if (tws_conn.esco_super_timeout != 0) {
        os_sem_post(tws_conn.sem_phone_detach);
        tws_conn.esco_super_timeout = 0;
    } else {
        lmp_response(conn, "b04", 254, 1, 127, 13);
    }
}

void tws_esco_wait_tws_rusume(void *p) TWS_CODE;
void tws_esco_wait_tws_rusume(void *p)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;

    if (conn == NULL || conn->esco_conn == NULL) {
        LOG_D("[Debug]: [TWS_LMP]return wait_esco_monitor\n\r\n");
        return;
    }
    if (&tws_link.link->task == get_curr_task()) {
        tws_master_notice_slave_step_fun(1, 0, 0);
    } else {
        putchar(87);
        slot_timer_reset(tws_link.slot_instant_timer, 4);
    }
}

/* Poll (every 4 slots) until the monitor link is up and idle, then continue
 * opening the eSCO link; while the monitor is not up, count down the wait
 * and clear the receive window when it nearly runs out. */
void tws_esco_wait_monitor(void *p) TWS_CODE;
void tws_esco_wait_monitor(void *p)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;

    if (conn == NULL || conn->esco_conn == NULL) {
        LOG_I("[Info]: [TWS_LMP]return wait_esco_monitor\n\r\n");
        return;
    }
    putchar(119);
    if (tws_conn.monitor_start == 2) {
        if (!tws_sniff.enable && list_empty(&tws_conn.rx_pending) && list_empty(&tws_link.link->tx_acl_u)) {
            LOG_I("[Info]: [TWS_LMP]esco_wait_monitor_start=%d \r\n", tws_conn.esco_wait_monitor_start);
            tws_master_notice_slave_step_fun(1, 0, 0);
            return;
        }
    } else if (tws_conn.esco_wait_monitor_start != 0) {
        int left = tws_conn.esco_wait_monitor_start - 1;

        tws_conn.esco_wait_monitor_start = left;
        if (left < 10) {
            struct ctrl_frame *frame;

            LOG_I("[Info]: [TWS_LMP]esco_wait_monitor_start timeout\n\r\n");
            frame = tws_link.link->frame;
            frame->rx_clknl = 0;
            frame->wincntl = 0;
        }
    }
    slot_timer_reset(tws_link.slot_instant_timer, 4);
}

void tws_esco_wait_post_role_switch(void *p) TWS_CODE;
void tws_esco_wait_post_role_switch(void *p)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;

    if (conn == NULL || conn->esco_conn == NULL || tws_link.link == NULL) {
        LOG_I("[Info]: [TWS_LMP]tws_esco_wait_post_role_switch fail 1\n\r\n");
        return;
    }
    tws_link.post_instant = 0;
    tws_conn.role_switch = 7;
    if (tws_link.slot_instant_timer != 0) {
        slot_timer_put(tws_link.slot_instant_timer);
        tws_link.slot_instant_timer = 0;
    }
    if (!list_empty(&conn->link->rx_head)) {
        LOG_I("[Info]: [TWS_LMP]rx_head not list_empty2\n\r\n");
        bredr_link_event(conn->link, 1, 5);
    }
    tws_link.esco_wait_role_swtich = 0;
    tws_conn.role_switch = 7;
}

void tws_esco_wait_role_switch_timeout(void *p) TWS_CODE;
void tws_esco_wait_role_switch_timeout(void *p)
{
    LOG_I("[Info]: [TWS_LMP]tws_esco_wait_role_switch_timeout\n\r\n");
    if (tws_link.slot_instant_timer != 0) {
        slot_timer_put(tws_link.slot_instant_timer);
        tws_link.slot_instant_timer = 0;
    }
    tws_conn.role_switch = 0;
}

/* Arm the instant timer to call fun when post_instant arrives; returns the
 * slots to wait (4 when the instant is already past). */
int tws_step_fun_start(void (*tws_post_caback_fun)(void *), int post_instant) TWS_CODE;
int tws_step_fun_start(void (*tws_post_caback_fun)(void *), int post_instant)
{
    int slots;

    if (tws_conn.lmp_conn == NULL)
        return 4;
    slots = bredr_clkn2offset(tws_link_read_slot_clk(), post_instant);
    if ((u32)(slots - 4) > 69119996)
        slots = 4;
    local_irq_disable();
    if (tws_link.slot_instant_timer == 0)
        tws_link.slot_instant_timer = slot_timer_get();
    slot_timer_set(tws_link.slot_instant_timer, tws_post_caback_fun, tws_conn.lmp_conn, slots);
    local_irq_enable();
    LOG_I("[Info]: [TWS_LMP]tws_link.slot_instant_timer=%d\n\r\n", tws_link.slot_instant_timer);
    return slots;
}

/* Send the peer a step message (opcode 13) for the event, with the
 * instant (now + instant slots), and start the local timer for it. */
int tws_send_esco_step_fun(u8 event, void (*tws_post_caback_fun)(void *), u8 step, u8 post_step, int instant) TWS_CODE;
int tws_send_esco_step_fun(u8 event, void (*tws_post_caback_fun)(void *), u8 step, u8 post_step, int instant)
{
    u32 post_instant;

    if (tws_link.link == NULL || tws_conn.lmp_conn == NULL)
        return 0;
    post_instant = bredr_offset2clkn(tws_link_read_slot_clk(), instant);
    tws_link.post_instant = post_instant;
    switch (event) {
    case 1:
        LOG_I("[Info]: [TWS_LMP]tws_send_esco_step_fun end esco setup\n\r\n");
        tws_conn_send_cmd(13, "V114c14", post_step, event, step, tws_link.post_instant,
                          &tws_conn.lmp_conn->esco_conn->pdu);
        break;
    case 2:
    case 3:
        tws_conn_send_cmd(13, "V114", post_step, event, step, post_instant);
        break;
    }
    return tws_step_fun_start(tws_post_caback_fun, tws_link.post_instant);
}

/* Slave: the master asked to remove the eSCO link at instant (timeout != 0
 * removes it now). */
u8 tws_slave_remove_esco_deal(int instant, u8 timeout) TWS_CODE;
u8 tws_slave_remove_esco_deal(int instant, u8 timeout)
{
    struct link_connection *link = tws_link.link;
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct lmp_connection *target;
    struct lmp_esco *esco;
    int offset;

    if (link == NULL || conn == NULL)
        return 0;
    esco = conn->esco_conn;
    if (esco == NULL)
        return 0;
    if (link->esco == NULL) {
        target = conn;
        esco->reason = 19;
    } else {
        link->esco->rx_remove_esco_req_flag = 60;
        bredr_esco_close_slot_time(conn->link);
        target = tws_conn.lmp_conn;
        if (target->esco_conn != NULL)
            target->esco_conn->reason = 19;
    }
    tws_link.remove_esco_timeout = 0;
    tws_link.post_step = LMP_WAIT_FEATURES_RES_EXT;
    if (timeout != 0) {
        tws_post_remove_esco_link(target);
        offset = 16;
    } else {
        offset = tws_step_fun_start(tws_post_remove_esco_link, instant);
    }
    link_task_run_slots(&tws_link.link->task, offset);
    LOG_I("[Info]: [TWS_LMP]slave_tws_post_remove_esco_link_post:%d, %d,%d, %d,%d\n\r\n", timeout,
          tws_link.slot_instant_timer, offset, instant, tws_link.post_step);
    return 1;
}

/* ---- Sync channels: AFH and LMP slot --------------------------------- */

/* The peer's AFH update: apply the channel map from the instant on. */
int tws_afh_sync_rx(struct lmp_connection *conn, u8 *rx, int len, u8 *tx) TWS_CODE;
int tws_afh_sync_rx(struct lmp_connection *conn, u8 *rx, int len, u8 *tx)
{
    u32 instant;

    if (CONFIG_TWS_AFH_ENABLE == 0)
        return 0;
    memcpy(&instant, rx, 4);
    if (instant == tws_link.link->afh_slot_end)
        return 0;
    bredr_link_set_afh(tws_link.link, instant, 1, rx + 4);
    return 0;
}

void tws_afh_sync_master_rx(struct lmp_connection *conn, u8 *rx, int len) TWS_CODE;
void tws_afh_sync_master_rx(struct lmp_connection *conn, u8 *rx, int len)
{
}

/* Master: send the peer the phone link's channel map (instant + 10 bytes)
 * once it differs from the TWS link's; channels 10, 22, 34, 46, 58 and 70
 * are cleared from the copy first (the SDK's choice of wanted channels). */
static int tws_afh_master_tx(struct lmp_connection *conn, u8 *tx, int prev_req_err) TWS_CODE;
static int tws_afh_master_tx(struct lmp_connection *conn, u8 *tx, int prev_req_err)
{
    u8 channel_map[10];
    struct lmp_connection *lmp;
    struct link_connection *link;
    int i;
    u32 clk;
    u8 run_slot;

    if (CONFIG_TWS_AFH_ENABLE == 0)
        return 0;
    lmp = tws_conn.lmp_conn;
    if (lmp->esco_conn != NULL)
        return 0;
    if (tws_conn.update_afh == 0) {
        memcpy(channel_map, lmp->link->channel_map, 10);
        for (i = 10; i < 79; i += 12)
            channel_map[i / 8] &= (1 << (i & 6)) ^ 255;
        if (memcmp(tws_link.link->channel_map, channel_map, 10) == 0)
            return 0;
        tws_conn.update_afh = 1;
        clk = tws_link_read_slot_clk();
        run_slot = tws_conn.run_slot;
        link = tws_link.link;
        bredr_link_set_afh(link, run_slot * 3 + clk + (run_slot >> 1), 1, channel_map);
    } else {
        clk = tws_link_read_slot_clk();
        link = tws_link.link;
        if (bredr_clkn_after(clk, link->afh_slot_end) != 0) {
            tws_conn.update_afh = 0;
            return 0;
        }
    }
    if (tws_conn.update_afh == 0)
        return 0;
    link = tws_link.link;
    put32(tx, link->afh_slot_end);
    memcpy(tx + 4, link->channel_map, 10);
    return 14;
}

static void tws_afh_sync_tx_suss(void) TWS_CODE;
static void tws_conn_sync_tx_suss(void) TWS_CODE;

/* The sync channels the SDK collects by section (channel number in the name). */
const struct tws_sync_channel tws_conn_sync __attribute__((section(".tws_sync_channel.2"))) = {
    .channel = 2,
    .attribute = 2,
    .tx_sync_req = (int (*)(struct lmp_connection *, u8 *, int))tws_conn_sync_tx_core_data,
    .tx_sync_req_suss = tws_conn_sync_tx_suss,
    .rx_sync_req_and_res = tws_conn_sync_rx_core_data,
};

const struct tws_sync_channel tws_afh_sync __attribute__((section(".tws_sync_channel.3"))) = {
    .channel = 3,
    .attribute = 2,
    .tx_sync_req = tws_afh_master_tx,
    .tx_sync_req_suss = tws_afh_sync_tx_suss,
    .rx_sync_req_and_res = tws_afh_sync_rx,
    .rx_sync_res = tws_afh_sync_master_rx,
};

/* The phone link received an LMP PDU while eSCO is up: remember it (and
 * when) so the TWS link can pass it to the peer. */
int tws_conn_phone_link_rx_lmp_add(struct link_connection *lc, u16 lmp_value, u8 *data) TWS_CODE;
int tws_conn_phone_link_rx_lmp_add(struct link_connection *lc, u16 lmp_value, u8 *data)
{
    u32 slot_clk = 0;
    u16 value = 0;

    if (is_tws_esco_working() != 0) {
        slot_clk = tws_link_read_slot_clk();
        value = lmp_value;
    }
    tws_link.esco_rx_lmp_slot_clk = slot_clk;
    tws_link.esco_rx_lmp_cmd = value;
    return 0;
}

/* Slave side: hand the recorded LMP PDU (slot clock + command, 6 bytes)
 * to the master, once. */
int tws_lmp_slot_sync_rx(struct lmp_connection *conn, u8 *rx, int len, u8 *tx) TWS_CODE;
int tws_lmp_slot_sync_rx(struct lmp_connection *conn, u8 *rx, int len, u8 *tx)
{
    int working = is_tws_esco_working();
    u32 slot_clk = tws_link.esco_rx_lmp_slot_clk;
    u16 cmd;

    if (working == 0 || slot_clk == 0)
        return 0;
    cmd = tws_link.esco_rx_lmp_cmd;
    put32(tx, slot_clk);
    put16(tx + 4, cmd);
    tws_link.esco_rx_lmp_slot_clk = 0;
    return 24;
}

/* Master side: the slave's recorded LMP PDU arrived; if it was seen at about
 * the instant we recorded (12 slots either way), it is ours and nothing is
 * done, otherwise act on the commands 92, 13, 62 and 64. */
void tws_lmp_slot_sync_master_rx(struct lmp_connection *conn, u8 *rx, int len) TWS_CODE;
void tws_lmp_slot_sync_master_rx(struct lmp_connection *conn, u8 *rx, int len)
{
    u32 instant, slot_clk;
    u16 cmd;

    memcpy(&instant, rx, 4);
    if (instant == 0)
        return;
    slot_clk = tws_link.esco_rx_lmp_slot_clk;
    tws_link.esco_rx_lmp_slot_clk = 0;
    tws_link.esco_rx_lmp_cmd = 0;
    if (instant + 12 > slot_clk && instant - 12 < slot_clk)
        return;
    memcpy(&cmd, rx + 4, 2);
    switch (cmd) {
    case 92:
        lmp_response(conn, "111", 8, 46, 30);
        break;
    case 13:
        tws_master_notice_slave_step_fun(2, 19, 0);
        break;
    case 62:
        lmp_rx_incr_power(conn);
        break;
    case 64:
        lmp_rx_decr_power(conn);
        break;
    }
}

static int tws_lmp_slot_master_tx(struct lmp_connection *conn, u8 *tx, int prev_req_err) TWS_CODE;
static int tws_lmp_slot_master_tx(struct lmp_connection *conn, u8 *tx, int prev_req_err)
{
    return 0;
}

const struct tws_sync_channel tws_lmp_slot_sync __attribute__((section(".tws_sync_channel.13"))) = {
    .channel = 13,
    .attribute = 3,
    .tx_sync_req = tws_lmp_slot_master_tx,
    .rx_sync_req_and_res = tws_lmp_slot_sync_rx,
    .rx_sync_res = tws_lmp_slot_sync_master_rx,
};

/* ---- Events from the TWS task ----------------------------------------- */

static void do_tws_conn_detach(int reason) TWS_CODE;
void tws_role_switch_lock_timeout(void *p) TWS_CODE;

/* The TWS task's event dispatcher: connection start/detach, the pair
 * removal, core data arrival and so on, then every sync channel's own
 * event handler. */
void tws_conn_event_handler(int *msg) TWS_CODE;
void tws_conn_event_handler(int *msg)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    const struct tws_sync_channel *channel;
    OS_SEM *sem;

    switch (msg[0]) {
    case 22:
        tws_sys_timeout_del();
        if (msg[1] == 1) {
            if (conn != NULL) {
                int state = conn->esco_conn == NULL ? 8 : 24;

                if (conn->sbc_open != 0)
                    state |= 32;
                os_time_dly(2);
                tws_conn_send_cmd(1, "11A", 1, (u8)(tws_conn.tws_box_mode == 0 ? state : (state | -128)),
                                  hci_param.addr);
                break;
            }
            os_time_dly(2);
            tws_conn_send_cmd(1, "11A", 0, tws_conn.tws_box_mode == 0 ? 0 : 128, hci_param.addr);
        }
        sem = malloc(80);
        if (!sem)
            btctrler_assert_failed("sem", 0);
        os_sem_create(sem, 0);
        tws_conn.sem_role_req = sem;
        os_sem_pend(sem, 30);
        free(sem);
        tws_conn.sem_role_req = NULL;
        break;
    case 14: {
        struct bredr_rx_bulk *rx = (struct bredr_rx_bulk *)msg[1];

        tws_host_set_core_data(&rx->data[msg[2]], msg[3] - msg[2]);
        bredr_rx_bulk_free(rx);
        tws_conn_send_cmd(2, "1", 6);
        break;
    }
    case 56:
        tws_sys_timeout_del();
        break;
    case 59:
        tws_host_timeout_del();
        break;
    case 15:
        if (tws_conn.role == 0)
            tws_conn_role_switch_lock_ms(5000);
        break;
    case 45:
        do_tws_conn_detach(8);
        break;
    case 46:
        do_tws_conn_detach(tws_conn.detach_code);
        break;
    case 5:
        memset(gbredr_local_dev.internal_addr_remote, -1, 6);
        tws_conn_send_event(5, "111", 0, 0, 0);
        do_tws_conn_detach(12);
        break;
    case 54:
        link_conn_rx_bulk_avaliable((struct link_connection *)msg[1], NULL);
        break;
    }
    for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
        if (channel->event_handler != NULL)
            channel->event_handler(conn, msg[0], msg + 1);
    }
}

/* Keep the role switch from starting for timeout ms (bit 1 of the lock),
 * while a connection with the monitor up exists and no switch is going on. */
void tws_conn_role_switch_lock_ms(int timeout) TWS_CODE;
void tws_conn_role_switch_lock_ms(int timeout)
{
    local_irq_disable();
    if (tws_link.link != NULL && tws_conn.monitor_start == 2 && tws_conn.role_switch == 0) {
        tws_conn.role_switch_lock |= 2;
        local_irq_enable();
        if (rs_timer != 0)
            sys_timer_modify(rs_timer, timeout);
        else
            rs_timer = sys_timeout_add(NULL, tws_role_switch_lock_timeout, timeout);
    } else {
        local_irq_enable();
    }
}

/* Tear the TWS link down: hand the queued frames back to the phone link,
 * close the TWS link, report the phone link's fate to the TWS task (event 4)
 * and release whoever waits for the detach. */
static void tws_conn_detach_timeout(void *p) TWS_CODE;
static void do_tws_conn_detach(int reason)
{
    struct lmp_connection *conn = lmp_conn_for_address(NULL);
    struct link_connection *tws_lc;
    struct lmp_connection *lmp;
    struct list_head *node, *next;
    struct lmp_connection *reported = NULL;
    int slot_timer;

    tws_sniff_detach();
    tws_sys_timeout_del();
    local_irq_disable();
    if (tws_conn.monitor_start != 0) {
        tws_conn.monitor_start = 0;
        for (node = tws_conn.rx_pending.next; node != &tws_conn.rx_pending; node = next) {
            next = node->next;
            __list_del_entry(node);
            if (conn != NULL && !conn->link->monitor)
                list_add_tail(node, &conn->link->rx_head);
            else
                bredr_rx_bulk_free((struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry)));
        }
        for (node = tws_conn.tx_pending.next; node != &tws_conn.tx_pending; node = next) {
            next = node->next;
            __list_del_entry(node);
            if (conn != NULL)
                list_add_tail(node, &conn->link->tx_acl_u);
        }
        if (conn != NULL)
            link_conn_tx_bulk_avaiable(conn->link, NULL);
    }
    tws_lc = tws_link.link;
    lmp = tws_conn.lmp_conn;
    if (lmp != NULL && lmp->link != NULL) {
        if (!lmp->link->monitor)
            tws_monitor_enable(lmp->link, 0, 1);
        else
            tws_monitor_enable(lmp->link, 0, 0);
        lmp = tws_conn.lmp_conn;
        if (!lmp->link->monitor) {
            struct lmp_esco *esco = lmp->esco_conn;

            if (esco != NULL && esco->state == 5)
                remove_esco_link(lmp, 19);
        }
    }
    link_conn_close(tws_lc);
    tws_link.link = NULL;
    slot_timer = tws_link.slot_instant_timer;
    if (slot_timer != 0) {
        if (log_tag_const_i_TWS_LMP) {
            log_print(2, NULL, "[Info]: [TWS_LMP]put_tws_link.slot_instant_timer\n\r\n");
            slot_timer = tws_link.slot_instant_timer;
        }
        slot_timer_put(slot_timer);
        tws_link.slot_instant_timer = 0;
        if (reason == 8 && conn != NULL && conn->esco_conn != NULL)
            bredr_link_event(conn->link, 1, 9);
    }
    slot_timer_put(tws_conn.slot_timer);
    local_irq_enable();
    if (conn != NULL && (conn->core.state & 8) == 0) {
        u32 state = conn->core.state;
        struct link_connection *link = conn->link;

        if (!link->monitor && (tws_conn.detach == 0 || tws_conn.detach_code == 13)) {
            bredr_link_event(link, 1, 5);
            link_conn_follow_ctrl_disable(conn->link);
            reported = conn;
        } else if (reason == 8) {
            conn->err_code = 8;
            conn->core.state = state | 8;
            bredr_link_event(link, 2, 14, 8);
            goto report;
        } else {
            conn->err_code = 22;
            if (link->monitor) {
                conn->core.state = state | 8;
                bredr_link_event(link, 2, 14, 22);
            }
        }
    }
    if (reason != 11) {
report:
        tws_host_timeout_del_event();
        tws_conn_send_event(4, "111", tws_conn.role, reported != NULL, reason);
    }
    if ((tws_conn.phone_link_detach & 1) != 0)
        os_sem_post(tws_conn.sem_phone_detach);
    if (tws_conn.sem_tws_detach != NULL)
        os_sem_post(tws_conn.sem_tws_detach);
}

/* Start a role switch at the next opportunity (master, idle, monitoring). */
void tws_conn_switch_role(void) TWS_CODE;
void tws_conn_switch_role(void)
{
    local_irq_disable();
    if ((tws_conn.role_switch | tws_conn.role) == 0 && tws_conn.monitor_start == 2)
        tws_conn.role_switch = 1;
    local_irq_enable();
}

void tws_role_switch_lock_timeout(void *p)
{
    rs_timer = 0;
    tws_conn.role_switch_lock &= -3;
    LOG_D("[Debug]: [TWS_LMP]rs_unlock\n\r\n");
}

/* Detach from the sibling: send the detach command at once when the monitor
 * is not up, else wait for the role switch to let us go (and time out after
 * two seconds). */
void tws_conn_detach_internal(int reason) TWS_CODE;
void tws_conn_detach_internal(int reason)
{
    if (tws_link.link == NULL) {
        if (tws_conn.sem_tws_detach != NULL)
            os_sem_post(tws_conn.sem_tws_detach);
        return;
    }
    tws_sys_timeout_del();
    tws_sys_timer = sys_timeout_add(NULL, tws_conn_detach_timeout, 2000);
    local_irq_disable();
    tws_conn.detach = 1;
    tws_conn.detach_code = reason;
    if (tws_conn.monitor_start == 0) {
        local_irq_enable();
        tws_conn_send_cmd(14, "V1", 47, tws_conn.detach_code);
        return;
    }
    if (tws_conn.role == 0)
        tws_conn_switch_role();
    else
        tws_conn.role_switch_lock = 1;
    local_irq_enable();
}

static void tws_conn_detach_timeout(void *p)
{
    tws_sys_timer = 0;
    do_tws_conn_detach(8);
}

void tws_conn_channel_open(u8 channel) TWS_CODE;
void tws_conn_channel_open(u8 channel)
{
    tws_conn.channels |= 1 << channel;
}

static void tws_master_tx_role_switch_req(void) TWS_CODE;

/* A data channel closed; when the last one has, finish the role switch or
 * start the monitor depending on where the switch stands. */
void tws_conn_channel_close(u8 channel) TWS_CODE;
void tws_conn_channel_close(u8 channel)
{
    struct lmp_connection *conn;
    u8 channels, role_switch;

    if (CONFIG_BTCTLER_TWS_ENABLE == 0)
        return;
    conn = tws_conn.lmp_conn;
    channels = tws_conn.channels;
    if ((channels & (1 << channel)) == 0)
        return;
    channels &= ((1 << channel) ^ 255);
    tws_conn.channels = channels;
    if (channels != 0)
        return;
    role_switch = tws_conn.role_switch;
    if (tws_conn.role == 0) {
        if (role_switch != 2) {
            tws_master_start_monitor(NULL);
            return;
        }
        if (tws_link.link->state == 3) {
            LOG_D("[Debug]: [TWS_LMP]role_switch: supend\n\r\n");
            return;
        }
        if (tws_conn.phone_link_detach != 0) {
            tws_master_tx_role_switch_req();
            return;
        }
        if (list_empty(&tws_conn.rx_pending)) {
            if (lmp_conn_idle(conn) != 0 && list_empty(&tws_conn.tx_pending) &&
                link_conn_rx_empty(conn->link) != 0 && link_conn_tx_empty(conn->link) != 0 &&
                link_conn_tx_empty(tws_link.link) != 0)
                tws_master_tx_role_switch_req();
        } else if (log_tag_const_i_TWS_LMP) {
            struct link_connection *link = conn->link;
            struct bredr_tx_bulk *tx0 = link->tx_bulk[0], *tx1 = link->tx_bulk[1];
            int c = list_empty(&link->tx_acl_c);
            int u = list_empty(&link->tx_acl_u);

            log_print(2, NULL, "[Info]: [TWS_LMP]tx_busy: %x, %x, %d, %d\n\r\n", tx0, tx1, c, u);
        }
        os_taskq_post_type("tws", 54, 1, (int *)&conn->link);
    } else if (role_switch == 0) {
        tws_conn_slave_start_monitor(NULL);
    }
}

static void __start_monitor_by_instant(void) TWS_CODE;

/* Timer: step the master towards the monitor (state 1: retry in 128
 * slots; 3: switch the data sync on; 2: wait for the instant). */
static void tws_master_start_monitor(void *p)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;

    slot_timer_disable(tws_conn.slot_timer);
    switch (tws_conn.monitor_start) {
    case 1:
        slot_timer_set(tws_conn.slot_timer, tws_master_start_monitor, NULL, 128);
        break;
    case 3:
        tws_conn.link_reset = 1;
        tws_conn.monitor_start = 2;
        link_task_run_slots(&conn->link->task, 64);
        tws_data_sync_channel_open();
        os_taskq_post_type("tws", 15, 0, 0);
        break;
    case 2:
        __start_monitor_by_instant();
        break;
    }
}

static void tws_conn_slave_start_monitor(void *p)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;

    slot_timer_disable(tws_conn.slot_timer);
    switch (tws_conn.monitor_start) {
    case 1:
        tws_conn.link_reset = 1;
        tws_conn.monitor_start = 2;
        conn->link->task.sta_attribute = 1;
        link_task_idle_disable();
        link_task_add(&conn->link->task);
        tws_link.link->task.run_slot = 64;
        conn->link->task.run_slot = 64;
        link_task_run_slots(&conn->link->task, 64);
        os_taskq_post_type("tws", 15, 0, 0);
        break;
    case 2:
        __start_monitor_by_instant();
        break;
    }
}

/* djb2 hash (5381, times 33 plus the byte). */
u32 JBHash(u8 *data, int len) TWS_CODE;
u32 JBHash(u8 *data, int len)
{
    u32 hash = 5381;

    while (len != 0) {
        len--;
        hash = hash * 33 + *data++;
    }
    return hash;
}

/* ---- Receive and transmit paths on the TWS link ----------------------- */


/* The SDK's tws_slave_fliter_out_rx_data() (static inline in tws_pair.c):
 * nonzero when a frame received by the slave must not reach the phone side:
 * LMP PDUs other than 90, 91, 14, 15, and ACL data the host hook rejects. */
static int tws_slave_filter_out_rx_data(struct lmp_connection *conn, struct bredr_rx_bulk *rx)
{
    if (rx->llid == 3)
        return (rx->data[0] & -2) != 90 && (rx->data[0] & -2) != 14;
    return tws_host_fliter_out_acl_data(conn->core_s.handle, rx->llid, rx->data) != 0;
}

/* Interrupt context: a frame arrived on the TWS link (lc) or, with the
 * monitor up, on the phone link (lc is the monitor link). */
void tws_link_conn_rx_handler_in_irq(struct link_connection *lc, struct bredr_rx_bulk *rx, int ind) TWS_CODE;
void tws_link_conn_rx_handler_in_irq(struct link_connection *lc, struct bredr_rx_bulk *rx, int ind)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct lmp_connection *lmp;
    u8 monitor_start;
    int hash, role_is_slave;
    u32 length;

    if (lc->tws_link) {
        switch (rx->data[2]) {
        case 0:
            tws_conn_rx_handler_in_irq(rx);
            if (!rx->hold) {
                bredr_rx_bulk_free(rx);
                os_taskq_post_type("tws", 54, 1, (int *)&lc);
            }
            break;
        case 12:
            tws_local_media_sync_rx_handler_in_irq(rx);
            os_taskq_post_type("tws", 54, 1, (int *)&lc);
            break;
        }
        return;
    }
    monitor_start = tws_conn.monitor_start;
    if (monitor_start == 0)
        goto to_phone_link;
    if (tws_conn.prev_s_go != 0) {
        switch (rx->llid) {
        case 1:
            goto to_phone_link;
        case 2:
            tws_conn.prev_s_go = 0;
            break;
        }
    }
    if (monitor_start == 2) {
        if (tws_conn.esco_setup_flag != 0) {
            u8 pdu;

            if (rx->llid != 3)
                goto to_phone_link;
            /* LMP PDUs 92, 62 and 64 are recorded for the peer. */
            pdu = rx->data[0] & -2;
            switch (pdu) {
            case 92:
                tws_conn_phone_link_rx_lmp_add(NULL, 92, NULL);
                break;
            case 62:
            case 64:
                tws_conn_phone_link_rx_lmp_add(NULL, pdu, NULL);
                break;
            }
            goto to_phone_link;
        }
    } else if (tws_conn.role == 0) {
        if (rx->llid == 2 && conn->sbc_channel == (rx->data[3] << 8 | rx->data[2])) {
            tws_conn.prev_s_go = 1;
            goto to_phone_link;
        }
    }
    if (rx->llid == 2 && conn->sbc_channel == (rx->data[3] << 8 | rx->data[2]) &&
        (rx->data[6] << 8 | rx->data[7]) < conn->clear_a2dp_packet_num) {
        tws_conn.prev_s_go = 1;
        goto to_phone_link;
    }

    /* Queue the frame for the peer; the slave also numbers it, drops what the
     * host filter rejects and de-duplicates by hash. */
    bredr_rx_bulk_push(&tws_conn.rx_pending, rx);
    lmp = tws_conn.lmp_conn;
    tws_conn.rx_clkn++;
    rx->clkn = tws_conn.rx_clkn;
    if (tws_slave_filter_out_rx_data(lmp, rx)) {
        if (tws_conn.role == 1) {
            __list_del_entry(&rx->entry);
            bredr_rx_bulk_free(rx);
            os_taskq_post_type("tws", 54, 1, (int *)&lmp->link);
        } else if (tws_sniff.enable && (tws_sniff.state & 1)) {
            struct link_connection *link;

            __list_del_entry(&rx->entry);
            link = lmp->link;
            if (list_empty(&link->rx_head)) {
                bredr_link_event(link, 1, 5);
                link = lmp->link;
            }
            bredr_rx_bulk_push(&link->rx_head, rx);
        } else {
            rx->ready = 1;
        }
        return;
    }
    length = rx->len;
    if (rx->llid == 2 && length >= 32)
        length = 32;
    hash = JBHash(rx->data, length);
    rx->hash = hash;
    role_is_slave = tws_conn.role == 1;
    if (hash == 0) {
        if (role_is_slave) {
            __list_del_entry(&rx->entry);
            bredr_rx_bulk_free(rx);
            os_taskq_post_type("tws", 54, 1, (int *)&lmp->link);
        } else {
            rx->hash = 305419896;
        }
        return;
    }
    if (role_is_slave) {
        if (tws_conn.pure_monitor == 0) {
            int i;

            for (i = 0; i < 16; i++) {
                if (hash == tws_conn.history_hash[i]) {
                    __list_del_entry(&rx->entry);
                    bredr_rx_bulk_free(rx);
                    os_taskq_post_type("tws", 54, 1, (int *)&lmp->link);
                    break;
                }
            }
        }
        tws_conn_history_hash_add(hash);
    }
    return;

to_phone_link:
    bredr_rx_bulk_push(&lc->rx_head, rx);
    bredr_link_event(lc, 1, 5);
}

/* A tx event from the link layer: connect_post (23), setup done (28), the
 * ACL tx/rx boundaries (43, 44 notify the sync channels), eSCO steps
 * (49..53), peer function calls (63) ... then every channel's own handler. */
int tws_conn_tx_event_handler(int event, struct bredr_tx_bulk *tx) TWS_CODE;
int tws_conn_tx_event_handler(int event, struct bredr_tx_bulk *tx)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    const struct tws_sync_channel *channel;

    switch (event) {
    case 23:
        LOG_I("[Info]: [TWS_LMP]========connect_post\n\r\n");
        break;
    case 28:
        tws_conn.monitor_start = 3;
        break;
    case 47:
        os_taskq_post_type("tws", 46, 0, 0);
        break;
    case 48:
        os_taskq_post_type("tws", 5, 0, 0);
        break;
    case 43:
    case 44: {
        struct link_connection *link = tws_link.link;

        tws_conn.prev_req_err = 0;
        if (!tws_sniff.enable && link != NULL && !link->role)
            link->core.max_slot = 5;
        for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
            if (channel->tx_sync_req_suss != NULL)
                channel->tx_sync_req_suss();
        }
        break;
    }
    case 41:
        tws_conn.role_switch = 7;
        tws_conn_slave_start_monitor(NULL);
        break;
    case 49:
        tws_master_notice_slave_step_fun(1, 0, 1);
        break;
    case 50:
        tws_master_notice_slave_step_fun(1, 0, 2);
        break;
    case 51:
        tws_master_notice_slave_step_fun(2, 0, 1);
        break;
    case 52:
        tws_master_notice_slave_step_fun(2, 0, 2);
        break;
    case 53:
        tws_master_notice_slave_step_fun(3, 0, 1);
        break;
    case 63: {
        u32 func_id;
        memcpy(&func_id, &tx->data[4], sizeof(func_id));
        tws_function_call_by_id(func_id, &tx->data[8], tx->len - 8, 0);
        break;
    }
    }
    for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
        if (channel->event_handler != NULL)
            channel->event_handler(conn, event, (int *)&tx);
    }
    return 0;
}

/* ---- The API the TWS task calls ---------------------------------------- */

int tws_api_open_tws_wait_pair(int code, const char *name) TWS_CODE;
int tws_api_open_tws_wait_pair(int code, const char *name)
{
    LOG_I("[Info]: [TWS_LMP] %s %s \n\r\n", __func__, name);
    if (config_btctler_le_tws != 0 && tws_link.link == NULL)
        tws_wait_pair_by_ble();
    tws_conn_state = 12;
    return 0;
}

/* Pairing with a phone: inquiry scan with our EIR, and page scan. */
int tws_api_open_phone_wait_pair(int code, const char *name) TWS_CODE;
int tws_api_open_phone_wait_pair(int code, const char *name)
{
    int code_addr = code;
    u8 addr[12];
    u8 eir_data[240];
    struct bredr_local_eir eir;
    int err;

    LOG_I("[Info]: [TWS_LMP] %s %s\n\r\n", __func__, name);
    tws_build_pair_eir(&code_addr, name, eir_data, &eir);
    __disable_all_inquiry_and_scan();
    if (tws_conn.monitor_start != 0)
        return -22;
    err = __inquiry_scan_enable(0, hci_param.addr, &eir);
    if (err != 0)
        return err;
    if (tws_link.link != NULL || config_btctler_le_tws != 0) {
        err = __page_scan_enable(0, hci_param.addr, 1, 0);
    } else {
        memcpy(addr, hci_param.addr, 6);
        memcpy(&addr[6], gbredr_local_dev.internal_addr_local, 6);
        err = __page_scan_enable(0, addr, 2, 0);
    }
    if (err != 0) {
        link_inquiry_scan_disable();
        return err;
    }
    tws_conn_state = 1;
    return 0;
}

void tws_api_close_tws_pair(void) TWS_CODE;
void tws_api_close_tws_pair(void)
{
    LOG_I("[Info]: [TWS_LMP] %s \n\r\n", __func__);
    if (config_btctler_le_tws != 0) {
        link_le_adv_try_start_disable(0);
        link_le_scan_try_start_disable(0);
    }
}

void tws_api_close_phone_wait_pair(void) TWS_CODE;
void tws_api_close_phone_wait_pair(void)
{
    LOG_I("[Info]: [TWS_LMP] %s \n\r\n", __func__);
    link_inquiry_scan_disable();
    link_page_scan_disable();
}

int tws_api_remove_tws_pairs(void) TWS_CODE;
int tws_api_remove_tws_pairs(void)
{
    LOG_I("[Info]: [TWS_LMP] %s \n\r\n", __func__);
    if (tws_link.link == NULL)
        return -22;
    tws_conn_send_cmd(8, "V", 48);
    return 0;
}

int tws_api_disconnect(void) TWS_CODE;
int tws_api_disconnect(void)
{
    LOG_I("[Info]: [TWS_LMP] %s \n\r\n", __func__);
    if (tws_link.link == NULL)
        return -22;
    tws_conn_send_cmd(14, "V1", 47, 19);
    return 0;
}

int tws_api_open_tws_wait_conn(int timeout) TWS_CODE;
int tws_api_open_tws_wait_conn(int timeout)
{
    lmp_conn_for_address(NULL);
    LOG_I("[Info]: [TWS_LMP] %s \n\r\n", __func__);
    if (tws_link.link != NULL)
        return -22;
    if (get_esco_coder_busy_flag() != 0)
        return -22;
    put_buf(gbredr_local_dev.internal_addr_local, 6);
    tws_link.tws_ble_timeout = timeout;
    __disable_all_inquiry_and_scan();
    if (config_btctler_le_tws != 0)
        return tws_wait_connection_internal_ble(timeout);
    /* The SDK passes an undefined timeout here; the callee ignores it. */
    return tws_wait_connection_internal_bredr(timeout);
}

int tws_api_open_tws_conn(int timeout) TWS_CODE;
int tws_api_open_tws_conn(int timeout)
{
    LOG_I("[Info]: [TWS_LMP] %s \n\r\n", __func__);
    if (tws_link.link != NULL)
        return -22;
    tws_link.tws_ble_timeout = timeout;
    __disable_all_inquiry_and_scan();
    if (config_btctler_le_tws != 0) {
        tws_sys_timeout_del();
        return tws_create_connection_internal_ble(timeout);
    }
    return tws_create_connection_internal_bredr(timeout);
}

int tws_api_fast_conn_with_pair(int role_type) TWS_CODE;
int tws_api_fast_conn_with_pair(int role_type)
{
    __disable_all_inquiry_and_scan();
    if (config_btctler_le_tws == 0)
        return 0;
    tws_sys_timeout_del();
    return tws_create_connection_internal_ble_fast_conn(role_type);
}

int btstack_send_msg_to_stack(int argc, int *argv);

int le_tws_connect_set_timer(int type, int timeout) TWS_CODE;
int le_tws_connect_set_timer(int type, int timeout)
{
    int msg[2];

    msg[0] = type;
    msg[1] = timeout;
    return btstack_send_msg_to_stack(2, msg);
}

/* ---- Role switch and the master's tx request --------------------------- */

/* Advance the role switch (states: 2 and 3 back to 1, 6 means we are master,
 * 7 swaps the link roles once no eSCO is running). */
static void role_switch_state_machine(struct lmp_connection *conn)
{
    switch (tws_conn.role_switch) {
    case 2:
    case 3:
        tws_conn.role_switch = 1;
        break;
    case 6:
        tws_conn.role = 0;
        break;
    case 7:
        if (is_tws_esco_working() == 0) {
            if (tws_conn.role == 0) {
                tws_link_master_2_slave(conn->link);
                tws_conn.role = 1;
                link_conn_follow_ctrl_disable(conn->link);
            } else {
                tws_link_slave_2_master(conn->link);
                tws_conn.role = 0;
            }
            tws_conn.role_switch = 0;
        }
        break;
    }
}

/* Build the master's per-slot request to the peer (opcode 0x80): the slots
 * to run, the role-switch/detach request and every sync channel's data, and
 * queue it on the TWS link. */
static void tws_conn_master_tx_start_req(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct bredr_tx_bulk *tx;
    u8 *data;
    const struct tws_sync_channel *channel;
    int slots, offset, run_slot;
    u16 length;

    if (is_tws_esco_working() != 0 && link_conn_tx_empty(tws_link.link) == 0)
        return;
    tx = tws_tx_bulk_alloc(367);
    if (tx == NULL && tws_link.acl_communication != 0) {
        LOG_I("[Info]: [TWS_LMP]master_tx_NULL\n\r\n");
        return;
    }
    if (tx == NULL && tws_conn.monitor_start != 2) {
        LOG_I("[Info]: [TWS_LMP]monitor_start!=2 master_tx_NULL\n\r\n");
        return;
    }
    if (!tx)
        btctrler_assert_failed("tx != NULL master_tx_start_req", 0);
    tx->event_post = 43;
    tx->data[2] = 0;
    tx->data[3] = 0x80;
    tx->data[8] = 0;
    if (conn != NULL && conn->link != NULL) {
        struct link_connection *link = conn->link;
        u8 base_slot = tws_conn.run_slot;

        run_slot = base_slot;
        switch (tws_conn.role_switch) {
        case 0:
            if ((tws_conn.phone_link_detach & 1) != 0) {
                tx->data[8] = 1;
                tx->data[9] = tws_conn.detach_code;
                slots = run_slot - 20;
            } else if (tws_conn.pure_monitor == 0) {
                /* The slots to run on the TWS link: from the queued frames
                 * that are not LMP, the media queued and the box mode. */
                struct list_head *node;
                int packet_cnt = 0, slot;

                for (node = tws_conn.rx_pending.next; node != &tws_conn.rx_pending; node = node->next) {
                    struct bredr_rx_bulk *rx = (struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry));

                    packet_cnt += rx->llid != 3;
                }
                if (tws_conn.tws_box_mode != 0) {
                    slot = local_tws_run_slots_adjust(packet_cnt);
                    base_slot = tws_conn.run_slot;
                    link = conn->link;
                } else {
                    int cond = conn->sbc_open == 0 ? 64 : packet_cnt * 12 + 32;
                    int limit = run_slot - 32;

                    slot = cond > limit ? limit : cond;
                }
                slots = (u16)slot;
            } else {
                slots = 24;
            }
            break;
        case 1:
            if ((tws_conn.phone_link_detach & 1) == 0) {
                tws_conn.role_switch = 2;
                tx->data[8] = 4;
            } else {
                tws_conn.role_switch = 0;
            }
            slots = run_slot - 20;
            break;
        default:
            slots = run_slot - 20;
            break;
        }
        tws_link.link->task.run_slot = slots;
        link->task.run_slot = base_slot - slots;
        if (conn->core.esco_state == 1) {
            conn->core.esco_state = 2;
            offset = run_slot + 160;
        } else {
            offset = run_slot;
        }
        tws_conn.instant = bredr_offset2clkn(tws_link_read_slot_clk(), offset);
        put32(&tx->data[4], tws_conn.instant);
        tx->data[11] = tws_link.link->task.run_slot;
        tx->data[10] = conn->link->task.run_slot;
    }
    data = &tx->data[12];
    for (channel = tws_sync_channel_begin; channel < tws_sync_channel_end; channel++) {
        int len;

        if (tws_conn.monitor_start != 2 && (channel->attribute & 2) != 0)
            continue;
        if (is_tws_esco_working() != 0) {
            switch (channel->channel) {
            case 8:
            case 1:
            case 9:
            case 4:
            case 13:
            case 7:
                break;
            default:
                continue;
            }
        }
        if (tws_link.link->core.max_slot != 5) {
            switch (channel->channel) {
            case 6:
            case 3:
            case 10:
                continue;
            }
        }
        if (channel->tx_sync_req == NULL)
            continue;
        len = channel->tx_sync_req(conn, data + 4, tws_conn.prev_req_err);
        if (len == 0)
            continue;
        *data = channel->channel;
        btctler_little_endian_store_16(data, 1, len);
        data += ((len + 7) / 4) << 2;
    }
    tws_conn.prev_req_err = 1;
    length = data - tx->data;
    tx->len = length;
    tws_link.link->tws_start_req = 1;
    tx->tws_start_req = 1;
    put16(tx->data, length - 4);
    if (length >= 368)
        btctrler_assert_failed("tx->len <= EDR2DH3_SIZE", length);
    link_conn_tx_bulk_avaiable(tws_link.link, tx);
}

static void tws_sys_timeout_del(void)
{
    if (tws_sys_timer != 0) {
        sys_timeout_del(tws_sys_timer);
        tws_sys_timer = 0;
    }
    sys_timer_disable = 0;
}

static void __disable_all_inquiry_and_scan(void)
{
    link_inquiry_disable();
    link_inquiry_scan_disable();
    link_page_disable(254);
    link_page_scan_disable();
    link_task_schedule_resume();
}

/* BLE search result: the peer's search record (address, channel, CRC8 of
 * the pair code, pair code, access address).  Returns 1 when it matched. */
static int __tws_scan_search_rx_handler(struct ble_rx *rx, u8 role)
{
    struct tws_search_info search_info;

    memcpy(&search_info, rx->data, sizeof(search_info));
    if (search_info.pair_code == tws_conn.code && crc8((u8 *)&tws_conn.code, 2) == search_info.pc_crc) {
        if (tws_host_channel_match(search_info.channel) == 0)
            return 0;
        gbredr_local_dev.tws_aa_wait_connect = search_info.access_addr;
        memcpy(gbredr_local_dev.internal_addr_remote, search_info.addr, 6);
        tws_ble_search_timeout_clr();
        return 1;
    }
    link_task_schedule_resume();
    return 0;
}

/* Search window over: unless the timer was cancelled meanwhile, stop the
 * inquiry and report event 1. */
static void tws_ble_search_timeout(void *p)
{
    tws_sys_timer = 0;
    if (sys_timer_disable) {
        sys_timer_disable = 0;
        return;
    }
    link_inquiry_disable();
    tws_conn_send_event(1, NULL);
    link_task_schedule_resume();
}

static void __page_sibling_timeout(u8 *addr, int err)
{
    if (err == -35) {
        link_page_disable(254);
        tws_conn_send_event(3, NULL);
    }
}

static void tws_conn_sync_tx_suss(void)
{
    tws_conn.lmp_core_data_sync = 0;
}

static void tws_afh_sync_tx_suss(void)
{
}

/* Master: ask the peer to switch roles (opcode 0x82 with our link state). */
static void tws_master_tx_role_switch_req(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct bredr_tx_bulk *tx = tws_tx_bulk_alloc(54);
    int length;

    if (tx == NULL)
        return;
    tx->len = 12;
    tx->data[2] = 0;
    tx->data[3] = 0x82;
    length = tx->len + tws_link_get_rxtx_state(conn->link, (struct tws_link_rxtx_state *)&tx->data[12]);
    tx->len = length;
    put16(tx->data, length - 4);
    link_conn_tx_bulk_avaiable(tws_link.link, tx);
    tws_conn.role_switch = 3;
    link_conn_follow_ctrl_enable(conn->link);
    tws_conn_history_hash_clear();
}

/* The monitor instant arrived (or was reached): schedule opening the eSCO
 * link, or just run the phone link's slots, as the link states allow. */
static void __start_monitor_by_instant(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    u32 instant = tws_conn.instant;
    int slots;

    if (instant == 0)
        return;
    if (conn->esco_conn != NULL && conn->core.esco_state == 1)
        return;
    tws_conn.instant = 0;
    slots = bredr_clkn2offset(tws_link_read_slot_clk(), instant);
    if ((u32)(slots - 2) > 69119997)
        slots = 2;
    if (conn->core.esco_state == 2) {
        u8 timer = tws_link.slot_instant_timer;

        if (timer == 0) {
            timer = slot_timer_get();
            tws_link.slot_instant_timer = timer;
        }
        conn->core.esco_state = 3;
        slot_timer_set(timer, start_monitor_creat_esco, tws_conn.lmp_conn, slots);
    }
    if (conn->sniff != NULL && (conn->sniff->state & 1) != 0) {
        link_task_run(NULL);
        return;
    }
    int esco = is_tws_esco_working();
    u32 post_instant = tws_link.post_instant;
    if ((post_instant | esco) != 0)
        return;
    if (tws_link.link->state == 3)
        conn->link->task.run_slot = slots;
    else
        link_task_run_slots(&conn->link->task, slots);
}

/* SDK weak hooks intentionally do nothing until the application overrides
 * them. */
void tws_host_event_packet_handler(u8 *packet, int size) TWS_WEAK;
void tws_host_event_packet_handler(u8 *packet, int size) {}

int tws_host_get_core_data_tick(u8 *packet, int size) TWS_WEAK;
int tws_host_get_core_data_tick(u8 *packet, int size) { return 0; }

int tws_host_set_core_data_tick(u8 *packet, int size) TWS_WEAK;
int tws_host_set_core_data_tick(u8 *packet, int size) { return 0; }

void tws_user_data_handler(u8 *data, u16 len) TWS_WEAK;
void tws_user_data_handler(u8 *data, u16 len) {}

void user_set_tws_box_mode(u8 mode) TWS_CODE;
void user_set_tws_box_mode(u8 mode) { tws_conn.tws_box_mode = mode; }

void tws_conn_phone_link_rx_bulk_go(void) TWS_CODE;
void tws_conn_phone_link_rx_bulk_go(void)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    struct list_head *node, *next;

    tws_conn_send_event_check();
    for (node = tws_conn.rx_pending.next; node != &tws_conn.rx_pending; node = next) {
        next = node->next;
        __list_del_entry(node);
        bredr_rx_bulk_push(&conn->link->rx_head,
                           (struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry)));
        if (tws_conn.send_event == 1)
            tws_conn.send_event = 2;
    }
    tws_conn_try_send_event_to_lmp();
}

void tws_conn_set_pure_monitor_enable(bool enable) TWS_CODE;
void tws_conn_set_pure_monitor_enable(bool enable)
{
    tws_conn.pure_monitor = enable;
    tws_conn.run_slot = enable ? 64 : 100;
}

u32 tws_esco_get_time_offset(struct link_connection *lc, u8 T_sco) TWS_CODE;
u32 tws_esco_get_time_offset(struct link_connection *lc, u8 T_sco)
{
    int period = T_sco * 5;
    int sub = 8 - (bredr_get_master_slot_clk(lc) % period);

    if (sub < 0)
        sub += period;
    return (sub < 3 ? period - 2 : -2) + sub;
}

void test_esco_role_switch(u8 flag) TWS_CODE;
void test_esco_role_switch(u8 flag)
{
    struct link_connection *link = tws_link.link;

    if (tws_conn.lmp_conn != NULL && link != NULL && link->esco != NULL && tws_conn.role == 0)
        test_esco_role_switch_flag = flag;
}

void tws_conn_timeout_deal(void) TWS_CODE;
void tws_conn_timeout_deal(void) {}

void tws_phone_link_connection(struct lmp_connection *conn) TWS_CODE;
void tws_phone_link_connection(struct lmp_connection *conn)
{
    local_irq_disable();
    if (tws_conn.detach != 0 || tws_link.link == NULL) {
        local_irq_enable();
        return;
    }
    tws_conn.lmp_conn = conn;
    if (tws_conn.monitor_start == 0) {
        tws_host_timeout_set();
        slot_timer_set(tws_conn.slot_timer, check_for_setup_monitor_link,
                       &conn->rejust_esco_req, 4);
    }
    local_irq_enable();
}

void tws_phone_link_detach(struct lmp_connection *conn, u8 code) TWS_CODE;
void tws_phone_link_detach(struct lmp_connection *conn, u8 code)
{
    OS_SEM *sem;

    if (tws_link.link == NULL || tws_conn.monitor_start == 0)
        return;
    sem = malloc(80);
    if (sem == NULL) {
        if (config_asser != 0) {
            printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/tws/tws_protocol/tws_pair.c", 1859);
            printf("ASSERT-FAILD: sem ");
            cpu_assert_debug();
        } else {
            cpu_reset();
        }
    }
    os_sem_create(sem, 0);
    local_irq_disable();
    tws_conn.sem_phone_detach = sem;
    tws_conn.phone_link_detach |= 1;
    tws_conn.detach_code = code;
    local_irq_enable();
    tws_sniff_detach();
    if (tws_link.link != NULL && tws_conn.monitor_start == 2) {
        os_sem_pend(sem, 0);
        tws_conn.sem_phone_detach = NULL;
    }
    free(sem);
}

void tws_phone_link_del(u8 err_code) TWS_CODE;
void tws_phone_link_del(u8 err_code)
{
    tws_conn.lmp_conn = NULL;
    if (tws_link.link != NULL)
        tws_conn_send_event(6, "111", tws_conn.role, 0, err_code);
}

int tws_phone_link_sbc_data_len(int channel) TWS_CODE;
int tws_phone_link_sbc_data_len(int channel)
{
    struct list_head *node;
    int data_len = 0;

    local_irq_disable();
    if (tws_conn.monitor_start == 2) {
        for (node = tws_conn.rx_pending.next; node != &tws_conn.rx_pending; node = node->next) {
            struct bredr_rx_bulk *bulk = (struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry));
            if (bulk->llid == 2 &&
                ((u16)bulk->data[2] | ((u16)bulk->data[3] << 8)) == channel)
                data_len += (int)bulk->data[0] | ((int)bulk->data[1] << 8);
        }
    }
    local_irq_enable();
    return data_len;
}

int tws_phone_link_abandon_sbc_data(struct lmp_connection *conn, u16 seqn_number) TWS_CODE;
int tws_phone_link_abandon_sbc_data(struct lmp_connection *conn, u16 seqn_number)
{
    int result = 0;

    local_irq_disable();
    if (tws_conn.monitor_start == 2)
        result = lmp_private_abandon_sbc_data(conn, &tws_conn.rx_pending, seqn_number) & 0xffff;
    local_irq_enable();
    return result;
}

int tws_phone_link_rx_pend_empty(void) TWS_CODE;
int tws_phone_link_rx_pend_empty(void)
{
    return tws_conn.monitor_start == 0 || list_empty(&tws_conn.rx_pending);
}

void reset_local_channle(void) TWS_CODE;
void reset_local_channle(void)
{
    u8 channel = 0;

    if (syscfg_read(98, &channel, 1) < 0)
        tws_api_set_local_channel(85);
    else
        tws_api_set_local_channel(channel);
}

void tws_esco_role_switch_post(void *p) TWS_CODE;
void tws_esco_role_switch_post(void *p)
{
    struct lmp_connection *conn = tws_conn.lmp_conn;
    int esco_working = is_tws_esco_working();

    if (conn == NULL || !esco_working || tws_conn.monitor_start != 2 || tws_conn.role_switch != 7)
        return;
    if (tws_conn.role == 0) {
        tws_link_master_2_slave(conn->link);
        tws_conn.role = 1;
        tws_link.esco_wait_role_swtich = 10;
        link_conn_follow_ctrl_disable(conn->link);
    } else {
        tws_link_slave_2_master(conn->link);
        tws_conn.role = 0;
    }
    tws_conn.role_switch = 0;
}

u8 tws_lmp_esco_link_setup(u8 en) TWS_CODE;
u8 tws_lmp_esco_link_setup(u8 en)
{
    struct lmp_connection *conn;

    if (tws_conn.esco_setup_flag == 0)
        return 0;
    conn = tws_conn.lmp_conn;
    if (conn->rx_remove_esco_req_before_esco_setup == 0)
        return 0;
    conn->rx_remove_esco_req_before_esco_setup = 0;
    tws_master_notice_slave_step_fun(2, 0, 0);
    return 1;
}

int tws_post_remove_esco_link_super_timeout(void *p) TWS_CODE;
int tws_post_remove_esco_link_super_timeout(void *p)
{
    OS_SEM *sem;
    int assert_enabled;
    struct lmp_connection *conn = p;

    if (conn == NULL || conn->esco_conn == NULL || tws_link.link == NULL)
        return 0;
    assert_enabled = config_asser;
    sem = malloc(80);
    if (sem == NULL) {
        if (assert_enabled != 0) {
            printf("file:%s, line:%d", "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/tws/tws_protocol/tws_pair.c", 3547);
            printf("ASSERT-FAILD: sem ");
            cpu_assert_debug();
        } else {
            cpu_reset();
        }
    }
    os_sem_create(sem, 0);
    local_irq_disable();
    tws_conn.sem_phone_detach = sem;
    local_irq_enable();
    tws_conn.esco_super_timeout = 1;
    if (CONFIG_BTCTLER_TWS_ENABLE != 0)
        tws_master_notice_slave_step_fun(2, 19, 0);
    if (tws_link.link != NULL) {
        os_sem_pend(sem, 50);
        tws_conn.sem_phone_detach = NULL;
        tws_conn.esco_super_timeout = 0;
    }
    free(sem);
    return 1;
}

int tws_check_can_setup_esco(void) TWS_CODE;
int tws_check_can_setup_esco(void)
{
    if (tws_link.link == NULL || tws_conn.lmp_conn == NULL)
        return 1;
    if (tws_conn.monitor_start != 2) {
        tws_conn.esco_wait_monitor_start = 2000;
        return 0;
    }
    switch (tws_conn.role) {
    case 0:
        if (tws_conn.esco_setup_flag != 0)
            return 1;
        tws_conn.esco_wait_monitor_start = 0;
        return 0;
    case 1:
        return tws_conn.esco_setup_flag != 0;
    default:
        return 1;
    }
}

u8 tws_conn_switch_is_master(void) TWS_CODE;
u8 tws_conn_switch_is_master(void)
{
    if (tws_link.link == NULL)
        return 1;
    return (tws_conn.role_switch | tws_conn.role) == 0;
}

u8 tws_conn_switch_is_slave(void) TWS_CODE;
u8 tws_conn_switch_is_slave(void)
{
    return tws_conn.role == 1 && tws_link.link != NULL;
}

int tws_conn_switch_role_lock(void) TWS_CODE;
int tws_conn_switch_role_lock(void)
{
    int result = -22;

    local_irq_disable();
    if (tws_conn.role_switch == 0) {
        tws_conn.role_switch_lock |= 1;
        result = 0;
    }
    local_irq_enable();
    return result;
}

void tws_conn_switch_role_unlock(void) TWS_CODE;
void tws_conn_switch_role_unlock(void) { tws_conn.role_switch_lock &= -2; }

void tws_fast_search_cb_register(void (*handle)(u8 *, u8 *)) TWS_CODE;
void tws_fast_search_cb_register(void (*handle)(u8 *, u8 *)) { tws_fast_search_cb = handle; }

static int __fast_scan_search_rx_handler(struct ble_rx *rx, u8 role) TWS_CODE;
static int __fast_scan_search_rx_handler(struct ble_rx *rx, u8 role)
{
    struct tws_search_info info;

    memcpy(&info, rx->data, 28);
    if (info.pair_code == tws_conn.code && btctler_crc8_check((u8 *)&tws_conn.code, 2, info.pc_crc)) {
        gbredr_local_dev.tws_aa_wait_connect = info.access_addr;
        memcpy(gbredr_local_dev.internal_addr_remote, info.addr, 6);
        tws_ble_search_timeout_clr();
        if (tws_fast_search_cb != NULL)
            tws_fast_search_cb(info.addr, info.data);
        return 1;
    }
    link_task_schedule_resume();
    return 0;
}

void tws_ble_fast_search_scan(u8 *data, int pair_code, u8 suspend) TWS_CODE;
void tws_ble_fast_search_scan(u8 *data, int pair_code, u8 suspend)
{
    tws_conn.code = (u16)pair_code;
    fast_search_data = data;
    link_le_adv_try_start_disable(0);
    link_le_scan_try_start_disable(0);
    link_le_scan_enable(tws_le_get_search_aa(), 7, 0, 0, 0, 8, __fast_scan_search_rx_handler);
    if (suspend)
        link_le_scan_suspend_en();
}

void tws_ble_fast_search_scan_resume(u16 slot) TWS_CODE;
void tws_ble_fast_search_scan_resume(u16 slot) { link_le_scan_resume(slot); }

static int __fast_scan_connect_rx_handler(struct ble_rx *rx, u8 role) TWS_CODE;
static int __fast_scan_connect_rx_handler(struct ble_rx *rx, u8 role)
{
    struct tws_conn_info info;

    memcpy(&info.role_type, rx->data, 31);
    if (tws_fast_search_cb != NULL)
        tws_fast_search_cb(info.addr, info.data);
    return 0;
}

void tws_ble_fast_conn_scan(u8 *data, u8 suspend) TWS_CODE;
void tws_ble_fast_conn_scan(u8 *data, u8 suspend)
{
    fast_search_data = data;
    link_le_adv_try_start_disable(0);
    link_le_scan_try_start_disable(0);
    link_le_adv_enable(tws_le_get_search_aa(), 7, 100, 0, 0, 1, 2, 0);
    link_le_scan_enable(tws_le_get_search_aa(), 7, 0, 1, 1, -128, __fast_scan_connect_rx_handler);
    if (suspend) {
        link_le_scan_suspend_en();
        link_le_adv_suspend_en();
    }
}

void tws_ble_fast_conn_scan_resume(u16 slot) TWS_CODE;
void tws_ble_fast_conn_scan_resume(u16 slot) { link_le_scan_resume(slot); }
void tws_ble_fast_conn_adv_resume(u16 slot) TWS_CODE;
void tws_ble_fast_conn_adv_resume(u16 slot) { link_le_adv_resume(slot); }

void tws_ble_fast_search_conn_close(void) TWS_CODE;
void tws_ble_fast_search_conn_close(void)
{
    fast_search_data = NULL;
    link_le_adv_try_start_disable(0);
    link_le_scan_try_start_disable(0);
}

/* Commands carried by the peer's TWS ACL.  The byte offsets and packet
 * constants below follow the SDK's optimized dispatcher and sync-channel
 * framing. */
void tws_conn_rx_handler_in_irq(struct bredr_rx_bulk *rx) TWS_CODE;
void tws_conn_rx_handler_in_irq(struct bredr_rx_bulk *rx)
{
    u8 *data = rx->data;
    struct lmp_connection *conn = tws_conn.lmp_conn;

    switch (data[3]) {
    case 1: {
        struct lmp_connection *phone = lmp_conn_for_address(0);
        char channel = tws_host_get_local_channel();
        char peer_channel;
        int box = tws_conn.tws_box_mode != 0 ? 128 : 0;
        u8 cause = data[5];

        tws_local_channel = channel;
        peer_channel = channel == 76 ? 82 : 76;
        if (phone != NULL && data[4] != 0 && tws_pair_all_way != 0) {
            tws_conn_send_cmd(14, "1", 1);
            return;
        }
        if (data[4] != 0) {
            tws_conn_send_cmd(2, "111", 1, peer_channel, box);
            tws_conn.role = 1;
            memcpy(hci_param.addr, &data[6], 6);
            tws_conn_send_event(2, "111", 1, 1, cause);
        } else {
            if (tws_common_addr_en != 0) {
                for (unsigned i = 0; i < 6; i++)
                    hci_param.addr[i] = gbredr_local_dev.internal_addr_remote[i] +
                                        gbredr_local_dev.internal_addr_local[i];
            }
            tws_host_get_common_addr(gbredr_local_dev.internal_addr_local,
                                     gbredr_local_dev.internal_addr_remote,
                                     hci_param.addr, channel);
            if (log_tag_const_i_TWS_LMP)
                printf_buf(hci_param.addr, 6);
            int role = tws_host_get_local_role();
            if (role == 0)
                tws_conn_send_cmd(3, "V11AA1", 23, 1, peer_channel, hci_param.addr,
                                  gbredr_local_dev.internal_addr_local, box);
            else
                tws_conn_send_cmd(2, "V11AA1", 23, 1, peer_channel, hci_param.addr,
                                  gbredr_local_dev.internal_addr_local, box);
            tws_conn.role = role != 0;
            tws_conn_send_event(2, "111", tws_conn.role, phone != NULL,
                                (s8)cause < 0 ? 128 : 0);
        }
        if (tws_conn.sem_role_req != NULL)
            os_sem_post(tws_conn.sem_role_req);
        return;
    }
    case 2:
        switch (data[4]) {
        case 1: {
            u8 state;
            tws_local_channel = data[5];
            tws_conn.role = 0;
            if (tws_conn.sem_role_req != NULL)
                os_sem_post(tws_conn.sem_role_req);
            if (conn != NULL) {
                state = conn->esco_conn == NULL ? 8 : 24;
                if (conn->sbc_open != 0)
                    state |= 32;
                if ((s8)data[6] < 0)
                    state |= 128;
            } else {
                state = data[18] & 128;
            }
            tws_conn_send_event(2, "111", tws_conn.role, conn != NULL, state);
            if (conn != NULL) {
                if (tws_conn.monitor_start == 0) {
                    tws_host_timeout_set();
                    slot_timer_set(tws_conn.slot_timer, check_for_setup_monitor_link,
                                   &conn->rejust_esco_req, 4);
                }
            } else {
                memcpy(hci_param.addr, &data[6], 6);
                memcpy(gbredr_local_dev.internal_addr_remote, &data[12], 6);
                tws_conn_send_cmd(5, NULL);
            }
            return;
        }
        case 6: {
            int instant;
            u32 slot;
            do {
                slot = tws_link_read_slot_clk();
                instant = bredr_clkn2offset(slot, tws_conn.instant);
                if ((u32)(instant - 8) >= 69119992)
                    tws_conn.instant += 128;
            } while ((u32)(instant - 8) >= 69119992);
            tws_conn_send_cmd(7, "V4", 28, tws_conn.instant);
            slot_timer_set(tws_conn.slot_timer, tws_master_start_monitor, NULL, instant);
            return;
        }
        case 17:
            tws_rx_unsniff_accept(&data[5]);
            return;
        case 15:
            tws_rx_sniff_accept(&data[5]);
            return;
        default:
            return;
        }
    case 3:
        if (data[4] != 1)
            return;
        tws_local_channel = data[5];
        tws_conn.role = 1;
        if (tws_conn.sem_role_req != NULL)
            os_sem_post(tws_conn.sem_role_req);
        if (conn == NULL) {
            memcpy(hci_param.addr, &data[6], 6);
            memcpy(gbredr_local_dev.internal_addr_remote, &data[12], 6);
            tws_conn_send_cmd(5, NULL);
            tws_conn_send_event(2, "111", tws_conn.role, 0,
                                (s8)data[18] < 0 ? 128 : 0);
        } else {
            tws_conn_send_cmd(14, "1", 1);
        }
        return;
    case 5:
        if (conn != NULL && tws_conn.monitor_start == 0) {
            tws_host_timeout_set();
            slot_timer_set(tws_conn.slot_timer, check_for_setup_monitor_link,
                           &conn->rejust_esco_req, 4);
        }
        return;
    case 6: {
        struct link_connection *link;
        struct ctrl_frame *frame;
        struct lmp_connection *new_conn;
        if (conn != NULL)
            return;
        tws_conn.role = 1;
        tws_conn.monitor_start = 1;
        __disable_all_inquiry_and_scan();
        link = get_link_connection();
        if (link == NULL) {
            btctrler_assert_failed("link != NULL", 0);
        }
        link_connection_init(link);
        link->monitor |= 1;
        link->role = 1;
        frame = tws_link_monitor_frame_init();
        link->frame = frame;
        link->last_signed_bitoff = 0;
        for (unsigned i = 0; i < 2; i++)
            link_conn_set_rx_bulk_in_irq(link, i);
        new_conn = lmp_create_conn();
        if (new_conn == NULL) {
            btctrler_assert_failed("conn != NULL", 0);
        }
        new_conn->link = link;
        tws_conn.lmp_conn = new_conn;
        tws_link_monitor_frame_setup(frame, (struct bredr_frame_core_data *)&data[104]);
        link->last_signed_bitoff = frame->bitoff;
        memcpy(&link->core.pwr_set, &data[88], 16);
        memcpy(&new_conn->core.stage, &data[4], 44);
        memcpy(&new_conn->core_s.remote_sr, &data[48], 40);
        lmp_role_machine_handler_register(new_conn, &lmp_slave_machine);
        if (new_conn->core.esco_state == 1)
            tws_fast_setup_esco_link(new_conn, (struct lmp_esco_pdu *)&data[192], 0);
        if (new_conn->core.operation_mode == 2) {
            tws_lmp_rx_sniff_sync(new_conn, (struct lmp_sniff *)&data[152]);
            tws_sniff_with_phone();
        }
        tws_host_set_core_data(&data[208], rx->len - 208);
        tws_conn_send_cmd(2, "1", 6);
        if (tws_common_addr_en != 0)
            lmp_send_event_link_key_notification(new_conn, 0);
        return;
    }
    case 7: {
        u32 instant;
        int offset;
        if (conn == NULL || tws_conn.monitor_start != 1)
            return;
        instant = (u32)data[4] | ((u32)data[5] << 8) |
                  ((u32)data[6] << 16) | ((u32)data[7] << 24);
        do {
            offset = bredr_clkn2offset(tws_link_read_slot_clk(), instant);
            instant += 128;
        } while ((u32)offset >= 69120000);
        slot_timer_set(tws_conn.slot_timer, tws_conn_slave_start_monitor, NULL,
                       offset > 2 ? offset : 2);
        conn->link->super_timeout_begin = bredr_read_slot_clk();
        conn->link->super_timeout = (u16)((gbredr_local_dev.super_timeout_value * 1000) / 625);
        return;
    }
    case 0x80: {
        struct link_connection *link;
        struct bredr_tx_bulk *tx = NULL;
        u8 *tx_data = NULL;
        int tx_len = 0;
        int esco;
        u8 state;
        if (conn != NULL && conn->link != NULL && (conn->link->monitor & 1) == 0)
            return;
        link = tws_link.link;
        if (link == NULL)
            return;
        if ((tws_sniff.enable & 1) == 0 && link->role != 0) {
            if (link->core.max_slot != 5 && log_tag_const_i_TWS_LMP) {
                LOG_I("[Info]: [TWS_LMP]-resume_max_slot: %d\n\r\n", link->core.max_slot);
                link = tws_link.link;
            }
            link->core.max_slot = 5;
        }
        esco = is_tws_esco_working();
        link = tws_link.link;
        if (esco) {
            if (!link_conn_tx_empty(link)) {
                putchar('N');
                return;
            }
        } else if (link->state != 2) {
            goto no_tick_tx;
        }
        tx = tws_tx_bulk_alloc(679);
        if (tx != NULL) {
            tx->event_post = 44;
            tx_data = tx->data + 8;
            tx->data[2] = 0;
            tx->data[3] = 0x81;
            tx->data[4] = 0;
        }
        no_tick_tx:
        if (tws_conn.monitor_start == 2) {
            struct link_connection *phone_link = conn->link;
            u8 phone_run_slot = data[10];
            u8 instant = data[4];
            if (tx != NULL) {
                struct tws_conn_start_res *res = (struct tws_conn_start_res *)tx->data;
                res->seqn_err_num = phone_link->seqn_err_num;
                res->rx_packet_num = phone_link->rx_packet_num;
            }
            tws_conn.role = 1;
            phone_link->task.run_slot = phone_run_slot;
            tws_link.link->task.run_slot = data[11];
            tws_conn.instant = instant;
            if (conn->core.esco_state == 1)
                conn->core.esco_state = 2;
            int instant_delta = (int)instant - (int)phone_run_slot;
            if (instant_delta < 0)
                instant_delta += 134217728;
            int slots = bredr_clkn2offset(tws_link_read_slot_clk(), instant_delta);
            if ((u32)(slots - 2) > 69119997) {
                tws_conn_slave_start_monitor(NULL);
            } else {
                int esco_now = is_tws_esco_working();
                u32 post_instant = tws_link.post_instant;
                if ((post_instant | esco_now) == 0 && conn->core.esco_state != 1 &&
                    (tws_sniff.enable & 1) == 0)
                    link_task_run_slots(&tws_link.link->task, slots);
            }
        }
        state = data[8];
        if ((state & 2) != 0 && log_tag_const_i_TWS_LMP) {
            LOG_I("[Info]: [TWS_LMP]link_reset = 0\n\r\n");
            state = data[8];
        }
        if ((state & 4) != 0)
            tws_conn.role_switch = 5;
        else if (!is_tws_esco_working())
            tws_conn.role_switch = 0;
        if ((data[8] & 1) != 0)
            tws_conn.detach_code = data[9];
        if (tws_conn.role_switch == 0 && tx != NULL && tws_conn.phone_link_detach == 1) {
            struct tws_conn_start_res *res = (struct tws_conn_start_res *)tx->data;
            res->code = tws_conn.detach_code;
            res->request = 1;
        }
        u8 *rx_data = data + 12;
        struct tws_sync_channel *ch;
        for (ch = tws_sync_channel_begin; ch < tws_sync_channel_end; ch++) {
                int n = 0;
                if (is_tws_esco_working() &&
                    !(ch->channel == 8 || ch->channel == 1 || ch->channel == 9 ||
                      ch->channel == 13 || ch->channel == 4 || ch->channel == 7))
                    goto sync_next;
                if (tws_link.link->core.max_slot != 5 &&
                    (ch->channel == 6 || ch->channel == 3 || ch->channel == 10))
                    goto sync_next;
                if (rx_data - data < rx->len && *rx_data == ch->channel) {
                    int rx_len = btctler_little_endian_read_16(rx_data, 1);
                    if (ch->rx_sync_req_and_res == NULL) {
                        btctrler_assert_failed("sync_ch->rx_sync_req_and_res channel", ch->channel);
                    }
                    n = ch->rx_sync_req_and_res(conn, rx_data + 4, rx_len,
                                                tx_data != NULL ? tx_data + 4 : NULL);
                    rx_data += ((rx_len + 7) & 131068);
                } else if ((ch->attribute & 1) != 0 &&
                           ((ch->attribute & 2) == 0 || tws_conn.monitor_start == 2)) {
                    n = ch->rx_sync_req_and_res(conn, NULL, 0,
                                                tx_data != NULL ? tx_data + 4 : NULL);
                }
                if (n != 0) {
                    *tx_data = ch->channel;
                    btctler_little_endian_store_16(tx_data, 1, (u16)n);
                    tx_data += ((n + 7) / 4) << 2;
                }
sync_next:
            ;
        }
        if (tx != NULL && tx_data != NULL) {
            tx_len = tx_data - tx->data;
            tx->len = tx_len;
            if ((tx_len & 65528) >= 680)
                btctrler_assert_failed("tx->len <= EDR2DH5_SIZE", tx_len);
            (void)is_tws_esco_working();
            tx_len = tx->len;
            ((struct tws_conn_start_res *)tx->data)->head.len = tx_len - 4;
            bredr_tx_bulk_realloc(tx, tx_len);
            link_conn_tx_bulk_avaiable(tws_link.link, tx);
        }
        return;
    }
    case 0x81: {
        struct link_connection *link;
        int esco;
        tws_conn.prev_req_err = 0;
        esco = is_tws_esco_working();
        if (!esco && tws_conn.role_switch == 6) {
            tws_link_slave_2_master(conn->link);
            tws_conn.role_switch = 0;
        }
        esco = is_tws_esco_working();
        link = tws_link.link;
        if (!esco && link->state != 2)
            return;
        if (link != NULL && (tws_sniff.enable & 1) == 0 && link->role != 0)
            link->core.max_slot = 5;
        if ((data[4] & 1) != 0) {
            u8 old_detach = tws_conn.phone_link_detach;
            tws_conn.detach_code = data[5];
            tws_conn.phone_link_detach = old_detach | 2;
            if ((old_detach & 1) != 0)
                bredr_link_event(conn->link, 2, 14, data[5]);
        }
        if (is_tws_esco_working()) {
            struct lmp_connection *current_conn = tws_conn.lmp_conn;
            if (current_conn != NULL && is_tws_esco_working() &&
                tws_conn.role == 0 && tws_conn.role_switch == 2) {
                struct link_connection *phone_link = current_conn->link;
                if (!list_empty(&phone_link->rx_head)) {
                    if (log_tag_const_i_TWS_LMP)
                        phone_link = current_conn->link;
                    bredr_link_event(phone_link, 1, 5);
                    phone_link = current_conn->link;
                }
                if (list_empty(&phone_link->rx_head) && lmp_conn_idle(current_conn) &&
                    list_empty(&tws_conn.tx_pending) && link_conn_rx_empty(current_conn->link) &&
                    link_conn_tx_empty(current_conn->link) && link_conn_tx_empty(tws_link.link))
                    tws_master_tx_role_switch_req();
            }
        } else {
            struct lmp_connection *current_conn = tws_conn.lmp_conn;
            if (tws_conn.monitor_start == 2 &&
                ((tws_conn.role_switch | tws_conn.role_switch_lock |
                  tws_conn.phone_link_detach | tws_conn.pure_monitor) == 0) &&
                current_conn->sbc_open != 0) {
                int remain = lmp_private_get_rx_buffer_remain_size(&current_conn->rejust_esco_req);
                if (remain < 1025) {
                    tws_conn.try_switch = 0;
                } else {
                int packets = current_conn->link->rx_packet_num;
                int delta = data[6] + data[7];
                int zero_delta = delta == 0;
                if (!zero_delta)
                    tws_conn.adjust_slave_clkoffset_cnt = 0;
                if (packets < 2) {
                    if (tws_auto_role_switch != 0) {
                        if (delta > 7) {
                            if (++tws_conn.try_switch > 2) {
                                tws_conn.try_switch = 0;
                                tws_conn.try_switch_2 = 0;
                                tws_conn.role_switch_by_power = 0;
                                tws_conn.role_switch = 1;
                            }
                        } else if (delta < 5) {
                            tws_conn.try_switch = 0;
                        } else if (tws_conn.try_switch != 0) {
                            tws_conn.try_switch--;
                        }
                    }
                } else if (packets != 2) {
                    tws_conn.try_switch = 0;
                    if (zero_delta && ++tws_conn.adjust_slave_clkoffset_cnt > 4) {
                        tws_conn.adjust_slave_clkoffset_cnt = 0;
                        tws_link.sync_clkn_offset = 1;
                    }
                } else if (tws_conn.try_switch != 0) {
                    tws_conn.try_switch--;
                }
                if (tws_auto_role_switch != 0 && CONFIG_TWS_POWER_BALANCE_ENABLE != 0 &&
                    tws_conn.role_switch_by_power != 0) {
                    if (delta > 1) {
                        if (++tws_conn.try_switch_2 > 4) {
                            tws_conn.try_switch = 0;
                            tws_conn.try_switch_2 = 0;
                            tws_conn.role_switch_by_power = 0;
                            tws_conn.role_switch = 1;
                        }
                    } else {
                        tws_conn.try_switch_2 = 0;
                    }
                }
                }
            }
        }
        u8 *rx_data = data + 8;
        for (struct tws_sync_channel *ch = tws_sync_channel_begin;
             ch < tws_sync_channel_end; ch++) {
            if (rx_data - data >= rx->len)
                break;
            if (*rx_data != ch->channel || ch->rx_sync_res == NULL)
                continue;
            int n = btctler_little_endian_read_16(rx_data, 1);
            ch->rx_sync_res(conn, rx_data + 4, n);
            rx_data += ((n + 7) & 131068);
        }
        return;
    }
    case 0x82:
        if (tws_conn.role_switch_lock != 0)
            return;
        if (!is_tws_esco_working() && tws_link.link->state != 2) {
            if (log_tag_const_i_TWS_LMP)
                LOG_I("[Info]: [TWS_LMP]role LINK_STA_OPEN err\n\r\n");
            return;
        }
        while (!list_empty(&tws_conn.rx_pending)) {
            struct list_head *entry = tws_conn.rx_pending.next;
            __list_del_entry(entry);
            bredr_rx_bulk_free((struct bredr_rx_bulk *)((u8 *)entry - 16));
        }
        if (!link_conn_rx_empty(conn->link)) {
            if (log_tag_const_i_TWS_LMP)
                LOG_I("[Info]: [TWS_LMP]<<<<<<<<<<<<<<<<<role_switch rx_not_empty>>>>>>>>>>>>>>>>\\n\\r\\n");
            return;
        }
        tws_link_set_rxtx_state(conn->link, (struct tws_link_rxtx_state *)&data[12]);
        if (is_tws_esco_working()) {
            tws_master_notice_slave_step_fun(3, 0, 0);
            return;
        }
        tws_conn.role_switch = 6;
        tws_conn_send_cmd(-125, "V", 41);
        return;
    case 0x83:
        if (tws_conn.role_switch == 3) {
            tws_conn.role_switch = 7;
            if (tws_link.link->state != 3)
                slot_timer_set(tws_conn.slot_timer, tws_master_start_monitor, NULL, 8);
        }
        return;
    case 14:
        if (tws_conn.detach == 0) {
            if (data[4] == 13 || data[4] == 14)
                tws_conn.detach_code = data[4];
            else
                tws_conn.detach_code = 10;
        }
        tws_conn.role_switch = 0;
        os_taskq_post_type("tws", 46, 0, NULL);
        return;
    case 8:
        os_taskq_post_type("tws", 5, 0, NULL);
        return;
    case 13:
        tws_slave_rx_notice_step_fun(&data[4]);
        return;
    case 15:
        tws_rx_sniff_req(&data[4]);
        return;
    case 17:
        tws_rx_unsniff_req(&data[4]);
        return;
    case 18:
        tws_function_call_by_id(data[4], &data[8], rx->len - 8, 1);
        return;
    default:
        return;
    }
}

#ifdef UNITTEST
#include "test.h"

struct tws_link_connection tws_link;
struct bredr_local_device gbredr_local_dev;
struct lmp_tws_sniff tws_sniff;
u8 log_tag_const_i_TWS_LMP, log_tag_const_d_TWS_LMP;
struct tws_sync_channel tws_sync_channel_begin[1];
extern struct tws_sync_channel tws_sync_channel_end[1]
    __attribute__((alias("tws_sync_channel_begin")));
static u32 test_func_stub_id;
static u16 test_func_stub_len;
static int test_func_stub_rx;
static void *test_func_stub_data;
static void test_tws_func_stub(void *data, u16 len, bool rx)
{
    test_func_stub_id = tws_func_stub_begin[0].func_id;
    test_func_stub_data = data;
    test_func_stub_len = len;
    test_func_stub_rx = rx;
}
const struct tws_func_stub tws_func_stub_begin[1] = {
    { 0x78561234, test_tws_func_stub },
};
__asm__(".global tws_func_stub_end\n.set tws_func_stub_end, tws_func_stub_begin + 8");
int config_asser = 1;
int CONFIG_BTCTLER_TWS_ENABLE;
int CONFIG_TWS_AFH_ENABLE, CONFIG_TWS_POWER_BALANCE_ENABLE;
static u32 test_master_slot_clk;
static u32 test_rx_slot_clk, test_rx_wall_slot;
static u32 test_rx_clkn_begin, test_rx_clkn_end;
static int test_rx_clkn_offset;
static u8 test_rx_timer_id;
static int test_rx_timer_slot, test_rx_taskq_type, test_rx_taskq_calls;
static int test_rx_slot_clk_calls, test_rx_tx_empty_calls, test_rx_sem_pends;
static int test_rx_tx_empty_result = 1;
static int test_irq_disable_calls, test_host_timeout_calls;
static int test_rx_alloc_tx, test_rx_tx_reallocs, test_rx_bulk_avail_calls;
static u8 *test_rx_mutate_after_clock;
static struct link_connection *test_rx_clock_link;
static int test_rx_clock_change_link, test_rx_clock_change_run_slot;
static u8 test_rx_clock_run_slot;
static int test_rx_irq_change_link;
static struct link_connection *test_rx_irq_link;
static int test_rx_irq_set_post;
static u32 test_rx_irq_post_value;
static int test_rx_link_run_slots_calls;
static int test_afh_after_result, test_afh_after_calls;
static struct link_connection *test_afh_set_link;
static u32 test_afh_set_instant, test_afh_after_begin, test_afh_after_end;
static void (*test_rx_timer_handler)(void *);
static void *test_rx_timer_priv;
static struct link_connection *test_rx_test_link;
static struct link_connection *test_rx_follow_ctrl_link;
static struct lmp_connection *test_rx_test_conn;
static struct ctrl_frame test_rx_test_frame;
struct test_bredr_tx_bulk {
    struct bredr_tx_bulk bulk;
    u8 payload[680];
};
static struct test_bredr_tx_bulk test_rx_test_tx_storage;
#define test_rx_test_tx test_rx_test_tx_storage.bulk
static struct lmp_esco test_rx_aligned_esco __attribute__((aligned(256)));
u32 bredr_get_master_slot_clk(struct link_connection *lc)
{
    return test_master_slot_clk;
}

u32 tws_link_read_slot_clk(void)
{
    test_rx_slot_clk_calls++;
    if (test_rx_mutate_after_clock != NULL) {
        test_rx_mutate_after_clock[0] = 0x40;
        test_rx_mutate_after_clock[1] = 0x30;
        test_rx_mutate_after_clock[2] = 0x20;
        test_rx_mutate_after_clock[3] = 0x10;
        test_rx_mutate_after_clock = NULL;
    }
    if (test_rx_clock_change_run_slot) {
        tws_conn.run_slot = test_rx_clock_run_slot;
        test_rx_clock_change_run_slot = 0;
    }
    if (test_rx_clock_change_link) {
        tws_link.link = test_rx_clock_link;
        test_rx_clock_change_link = 0;
    }
    return test_rx_slot_clk;
}
u32 bredr_read_slot_clk(void) { return test_rx_wall_slot; }
int bredr_clkn2offset(int begin, int end)
{
    test_rx_clkn_begin = begin;
    test_rx_clkn_end = end;
    return test_rx_clkn_offset;
}
int bredr_clkn_after(int begin, int end)
{
    test_afh_after_calls++;
    test_afh_after_begin = begin;
    test_afh_after_end = end;
    return test_afh_after_result;
}
void bredr_link_set_afh(struct link_connection *link, u32 instant, u8 enable, u8 *map)
{
    test_afh_set_link = link;
    test_afh_set_instant = instant;
    link->afh_slot_end = instant;
    memcpy(link->channel_map, map, 10);
}
void slot_timer_set(u8 id, void (*handler)(void *), void *priv, int slot)
{
    test_rx_timer_id = id;
    test_rx_timer_handler = handler;
    test_rx_timer_priv = priv;
    test_rx_timer_slot = slot;
}
u8 slot_timer_get(void) { return 7; }
int os_taskq_post_type(const char *name, int type, int argc, int *argv)
{
    test_rx_taskq_calls++;
    test_rx_taskq_type = type;
    return 0;
}
int link_conn_tx_empty(struct link_connection *link)
{
    test_rx_tx_empty_calls++;
    return test_rx_tx_empty_result;
}
struct link_connection *get_link_connection(void) { return test_rx_test_link; }
void link_connection_init(struct link_connection *link) { memset(link, 0, sizeof(*link)); }
void link_le_adv_try_start_disable(int reason) { }
void link_le_scan_try_start_disable(int reason) { }
void link_inquiry_disable(void) { }
void link_inquiry_scan_disable(void) { }
void link_page_disable(int reason) { }
void link_page_scan_disable(void) { }
void link_task_schedule_resume(void) { }
void link_task_run_slots(struct link_task *task, int slots) { test_rx_link_run_slots_calls++; }
int link_conn_set_rx_bulk_in_irq(struct link_connection *link, int index) { return 0; }
struct ctrl_frame *tws_link_monitor_frame_init(void) { return &test_rx_test_frame; }
void tws_link_monitor_frame_setup(struct ctrl_frame *frame, struct bredr_frame_core_data *data) { }
struct lmp_connection *lmp_create_conn(void) { return test_rx_test_conn; }
void lmp_role_machine_handler_register(struct lmp_connection *conn, void *handler) { }
void *lbuf_alloc(struct lbuff_head *head, u32 len)
{
    return test_rx_alloc_tx ? &test_rx_test_tx : NULL;
}
void bredr_tx_bulk_realloc(struct bredr_tx_bulk *bulk, int size) { test_rx_tx_reallocs++; }
void link_conn_tx_bulk_avaiable(struct link_connection *link, struct bredr_tx_bulk *tx)
{
    test_rx_bulk_avail_calls++;
}
int os_sem_create(OS_SEM *sem, int count) { return 0; }
int os_sem_pend(OS_SEM *sem, int timeout) { test_rx_sem_pends++; return 0; }
void local_irq_disable(void)
{
    test_irq_disable_calls++;
    if (test_rx_irq_change_link) {
        tws_link.link = test_rx_irq_link;
        test_rx_irq_change_link = 0;
    }
    if (test_rx_irq_set_post) {
        tws_link.post_instant = test_rx_irq_post_value;
        test_rx_irq_set_post = 0;
    }
}
void local_irq_enable(void) { }
void tws_host_timeout_set(void) { test_host_timeout_calls++; }
void tws_link_master_2_slave(struct link_connection *link)
{
    tws_conn.lmp_conn->link = test_rx_follow_ctrl_link;
}
void link_conn_follow_ctrl_disable(struct link_connection *link)
{
    test_rx_test_link = link;
}
void tws_link_slave_2_master(struct link_connection *link) { }

int main(void)
{
    unsigned i;

    memset(&tws_conn, 0, sizeof(tws_conn));
    for (i = 0; i < 16; i++)
        tws_conn_history_hash_add((int)(0x100 + i));
    CHECK(tws_conn.history_hash_index == 0);
    for (i = 0; i < 16; i++)
        CHECK(tws_conn.history_hash[i] == (int)(0x100 + i));
    tws_conn_history_hash_add(-1);
    CHECK(tws_conn.history_hash_index == 1 && tws_conn.history_hash[0] == -1);
    tws_conn_history_hash_clear();
    CHECK(tws_conn.history_hash_index == 0);
    for (i = 0; i < 16; i++)
        CHECK(tws_conn.history_hash[i] == 0);

    tws_conn_set_pure_monitor_enable(true);
    CHECK(tws_conn.pure_monitor == 1 && tws_conn.run_slot == 64);
    tws_conn_set_pure_monitor_enable(false);
    CHECK(tws_conn.pure_monitor == 0 && tws_conn.run_slot == 100);

    /* Directly from the IR: period = T_sco * 5; normalize 8 - clock%period,
     * then add period-2 only when the normalized value is below 3. */
    test_master_slot_clk = 0;
    CHECK(tws_esco_get_time_offset(NULL, 2) == 6);
    test_master_slot_clk = 7;
    CHECK(tws_esco_get_time_offset(NULL, 2) == 9);
    test_master_slot_clk = 8;
    CHECK(tws_esco_get_time_offset(NULL, 2) == 8);
    test_master_slot_clk = 9;
    CHECK(tws_esco_get_time_offset(NULL, 2) == 7);

    /* The IR calls is_tws_esco_working before reloading post_instant. */
    {
        struct lmp_connection phone;
        struct link_connection phone_link, peer_link;
        memset(&phone, 0, sizeof(phone));
        memset(&phone_link, 0, sizeof(phone_link));
        memset(&peer_link, 0, sizeof(peer_link));
        memset(&tws_conn, 0, sizeof(tws_conn));
        memset(&tws_link, 0, sizeof(tws_link));
        phone.link = &phone_link;
        tws_conn.lmp_conn = &phone;
        tws_conn.instant = 100;
        tws_link.link = &peer_link;
        peer_link.state = 3;
        test_rx_clkn_offset = 10;
        test_rx_link_run_slots_calls = 0;
        test_rx_irq_post_value = 0x100;
        test_rx_irq_set_post = 1;
        __start_monitor_by_instant();
        CHECK(tws_conn.instant == 0);
        CHECK(tws_link.post_instant == 0x100);
        CHECK(test_rx_link_run_slots_calls == 0 && phone_link.task.run_slot == 0);
    }

    /* Raw AFH IR calls the slot clock before loading run_slot and before
     * reloading tws_link.link; the else path similarly loads the link and
     * its slot end after the clock helper. These mutations expose each SSA
     * ordering dependency. */
    {
        struct lmp_connection phone;
        struct link_connection phone_link, old_peer, current_peer;
        u8 tx[16] = { 0 };
        u32 slot_end;
        memset(&phone, 0, sizeof(phone));
        memset(&phone_link, 0, sizeof(phone_link));
        memset(&old_peer, 0, sizeof(old_peer));
        memset(&current_peer, 0, sizeof(current_peer));
        memset(&tws_conn, 0, sizeof(tws_conn));
        memset(&tws_link, 0, sizeof(tws_link));
        memset(phone_link.channel_map, 0xff, sizeof(phone_link.channel_map));
        phone.link = &phone_link;
        tws_conn.lmp_conn = &phone;
        tws_conn.run_slot = 1;
        tws_link.link = &old_peer;
        CONFIG_TWS_AFH_ENABLE = 1;
        test_rx_slot_clk = 100;
        test_rx_clock_run_slot = 8;
        test_rx_clock_change_run_slot = 1;
        test_rx_clock_link = &current_peer;
        test_rx_clock_change_link = 1;
        test_afh_set_link = NULL;
        CHECK(tws_afh_sync.tx_sync_req(NULL, tx, 0) == 14);
        CHECK(test_afh_set_link == &current_peer && test_afh_set_instant == 128);
        memcpy(&slot_end, tx, sizeof(slot_end));
        CHECK(slot_end == current_peer.afh_slot_end);

        tws_conn.update_afh = 1;
        old_peer.afh_slot_end = 90;
        current_peer.afh_slot_end = 120;
        tws_link.link = &old_peer;
        test_rx_clock_change_link = 1;
        test_rx_clock_link = &current_peer;
        test_afh_after_result = 0;
        test_afh_after_calls = 0;
        CHECK(tws_afh_sync.tx_sync_req(NULL, tx, 0) == 14);
        CHECK(test_afh_after_calls == 1);
        CHECK(test_afh_after_begin == 100 && test_afh_after_end == 120);
        memcpy(&slot_end, tx, sizeof(slot_end));
        CHECK(slot_end == 120);
        CONFIG_TWS_AFH_ENABLE = 0;
    }

    /* The IR dispatcher ignores an unrecognized command byte without
     * changing connection state. */
    {
        struct { struct bredr_rx_bulk bulk; u8 storage[64]; } packet;
        memset(&packet, 0, sizeof(packet));
        tws_conn.role_switch = 7;
        packet.bulk.data[3] = 0xff;
        tws_conn_rx_handler_in_irq(&packet.bulk);
        CHECK(tws_conn.role_switch == 7 && test_rx_taskq_calls == 0);
    }

    /* Command 7 reads its instant as a four-byte little-endian value, then
     * passes the normalized slot offset to the slot timer.  The clock helper
     * is stubbed to expose the exact arguments sent by the IR path. */
    {
        struct lmp_connection phone;
        struct link_connection phone_link;
        struct { struct bredr_rx_bulk bulk; u8 storage[64]; } packet;
        memset(&phone, 0, sizeof(phone));
        memset(&phone_link, 0, sizeof(phone_link));
        memset(&packet, 0, sizeof(packet));
        memset(&tws_conn, 0, sizeof(tws_conn));
        memset(&gbredr_local_dev, 0, sizeof(gbredr_local_dev));
        tws_conn.lmp_conn = &phone;
        tws_conn.monitor_start = 1;
        tws_conn.slot_timer = 9;
        phone.link = &phone_link;
        gbredr_local_dev.super_timeout_value = 625;
        test_rx_slot_clk = 0x12345000;
        test_rx_wall_slot = 0x87654321;
        test_rx_clkn_offset = 20;
        packet.bulk.data[3] = 7;
        packet.bulk.data[4] = 0x78;
        packet.bulk.data[5] = 0x56;
        packet.bulk.data[6] = 0x34;
        packet.bulk.data[7] = 0x12;
        tws_conn_rx_handler_in_irq(&packet.bulk);
        CHECK(test_rx_clkn_begin == (int)test_rx_slot_clk);
        CHECK(test_rx_clkn_end == 0x12345678);
        CHECK(test_rx_timer_id == 9 && test_rx_timer_slot == 20);
        CHECK(test_rx_timer_priv == NULL && test_rx_timer_handler == tws_conn_slave_start_monitor);
        CHECK(phone_link.super_timeout_begin == test_rx_wall_slot && phone_link.super_timeout == 1000);
    }

    /* Opcode 2/sub-opcode 6 has no null-connection guard in the SDK IR: it
     * reads the clock, sends the instant command, and arms the monitor timer. */
    {
        struct { struct bredr_rx_bulk bulk; u8 storage[64]; } packet;
        memset(&packet, 0, sizeof(packet));
        memset(&tws_conn, 0, sizeof(tws_conn));
        tws_conn.lmp_conn = NULL;
        tws_conn.instant = 64;
        tws_conn.slot_timer = 11;
        test_rx_slot_clk_calls = 0;
        test_rx_timer_handler = NULL;
        test_rx_clkn_offset = 10;
        packet.bulk.data[3] = 2;
        packet.bulk.data[4] = 6;
        tws_conn_rx_handler_in_irq(&packet.bulk);
        CHECK(test_rx_slot_clk_calls == 1);
        CHECK(test_rx_timer_id == 11 && test_rx_timer_handler == tws_master_start_monitor);
        CHECK(test_rx_timer_slot == 10);
    }

    /* Opcode 6 ORs bit 3 of the combined link-role byte.  sdk.h lays that
     * bit out as link->role (bit 2 remains link->tws_link). */
    {
        struct link_connection link;
        struct lmp_connection phone;
        struct { struct bredr_rx_bulk bulk; u8 storage[256]; } packet;
        memset(&link, 0, sizeof(link));
        memset(&phone, 0, sizeof(phone));
        memset(&packet, 0, sizeof(packet));
        memset(&tws_conn, 0, sizeof(tws_conn));
        test_rx_test_link = &link;
        test_rx_test_conn = &phone;
        tws_common_addr_en = 0;
        packet.bulk.data[3] = 6;
        packet.bulk.len = 208;
        tws_conn_rx_handler_in_irq(&packet.bulk);
        CHECK(link.monitor == 1 && link.role == 1 && link.tws_link == 0);
        CHECK(tws_conn.lmp_conn == &phone && phone.link == &link);
    }

    /* Opcode 0x80 returns when the existing phone link is not in monitor
     * mode. A stale/global TWS link must not let the packet through. */
    {
        struct link_connection phone_link, peer_link;
        struct lmp_connection phone;
        struct { struct bredr_rx_bulk bulk; u8 storage[64]; } packet;
        memset(&phone_link, 0, sizeof(phone_link));
        memset(&peer_link, 0, sizeof(peer_link));
        memset(&phone, 0, sizeof(phone));
        memset(&packet, 0, sizeof(packet));
        memset(&tws_conn, 0, sizeof(tws_conn));
        memset(&tws_link, 0, sizeof(tws_link));
        phone.link = &phone_link;
        phone_link.monitor = 0;
        peer_link.state = 2;
        tws_conn.lmp_conn = &phone;
        tws_link.link = &peer_link;
        test_rx_tx_empty_calls = 0;
        packet.bulk.data[3] = 0x80;
        tws_conn_rx_handler_in_irq(&packet.bulk);
        CHECK(test_rx_tx_empty_calls == 0);
        CHECK(tws_conn.role == 0 && tws_conn.instant == 0);
    }

    /* Opcode 0x80 takes the one-byte instant from data[4], subtracts the
     * phone run-slot byte with 27-bit wrap, and stores both run-slot bytes. */
    {
        struct link_connection phone_link, peer_link;
        struct lmp_connection phone;
        struct { struct bredr_rx_bulk bulk; u8 storage[96]; } packet;
        memset(&phone_link, 0, sizeof(phone_link));
        memset(&peer_link, 0, sizeof(peer_link));
        memset(&phone, 0, sizeof(phone));
        memset(&packet, 0, sizeof(packet));
        memset(&tws_conn, 0, sizeof(tws_conn));
        memset(&tws_link, 0, sizeof(tws_link));
        phone.link = &phone_link;
        phone_link.monitor = 1;
        peer_link.state = 2;
        tws_conn.lmp_conn = &phone;
        tws_conn.monitor_start = 2;
        tws_link.link = &peer_link;
        tws_link.post_instant = 1;
        test_rx_slot_clk = 123;
        test_rx_clkn_offset = 20;
        packet.bulk.len = 64;
        packet.bulk.data[3] = 0x80;
        packet.bulk.data[4] = 5;
        packet.bulk.data[5] = 0xaa;
        packet.bulk.data[6] = 0xbb;
        packet.bulk.data[7] = 0xcc;
        packet.bulk.data[10] = 10;
        packet.bulk.data[11] = 22;
        packet.bulk.data[12] = 0xff;
        tws_conn_rx_handler_in_irq(&packet.bulk);
        CHECK(tws_conn.instant == 5);
        CHECK(test_rx_clkn_begin == 123 && test_rx_clkn_end == 134217723);
        CHECK(phone_link.task.run_slot == 10 && peer_link.task.run_slot == 22);
    }

    /* Opcode 0x80 bit 4 requests role-switch state 5; bit 2 is only logged
     * by the SDK and must not clear the unrelated link_reset state. The
     * response length goes in the packet head while bulk->len stays tx_len. */
    {
        struct link_connection phone_link, peer_link;
        struct lmp_connection phone;
        struct { struct bredr_rx_bulk bulk; u8 storage[96]; } packet;
        memset(&phone_link, 0, sizeof(phone_link));
        memset(&peer_link, 0, sizeof(peer_link));
        memset(&phone, 0, sizeof(phone));
        memset(&packet, 0, sizeof(packet));
        memset(&tws_conn, 0, sizeof(tws_conn));
        memset(&tws_link, 0, sizeof(tws_link));
        phone.link = &phone_link;
        phone_link.monitor = 1;
        peer_link.state = 2;
        tws_conn.lmp_conn = &phone;
        tws_conn.monitor_start = 2;
        tws_conn.link_reset = 1;
        tws_conn.role_switch = 2;
        tws_link.link = &peer_link;
        tws_link.post_instant = 1;
        test_rx_alloc_tx = 1;
        test_rx_tx_reallocs = 0;
        test_rx_bulk_avail_calls = 0;
        packet.bulk.len = 64;
        packet.bulk.data[3] = 0x80;
        packet.bulk.data[4] = 9;
        packet.bulk.data[10] = 9;
        packet.bulk.data[11] = 10;
        packet.bulk.data[8] = 6;
        packet.bulk.data[12] = 0xff;
        tws_conn_rx_handler_in_irq(&packet.bulk);
        test_rx_alloc_tx = 0;
        CHECK(tws_conn.role_switch == 5 && tws_conn.link_reset == 1);
        CHECK(test_rx_test_tx.len == 8 && test_rx_test_tx.data[0] == 4);
        CHECK(test_rx_test_tx.event_post == 44);
        CHECK(test_rx_tx_reallocs == 1 && test_rx_bulk_avail_calls == 1);
    }

    /* The opcode 0x80 flags and channel dispatch are outside the monitor
     * state-update branch. The SDK also reloads the peer link after the
     * eSCO-state helper before deciding whether to allocate a tick response. */
    {
        struct link_connection phone_link, old_peer, current_peer;
        struct lmp_connection phone;
        struct { struct bredr_rx_bulk bulk; u8 storage[96]; } packet;
        memset(&phone_link, 0, sizeof(phone_link));
        memset(&old_peer, 0, sizeof(old_peer));
        memset(&current_peer, 0, sizeof(current_peer));
        memset(&phone, 0, sizeof(phone));
        memset(&packet, 0, sizeof(packet));
        memset(&tws_conn, 0, sizeof(tws_conn));
        memset(&tws_link, 0, sizeof(tws_link));
        phone.link = &phone_link;
        phone_link.monitor = 1;
        old_peer.state = 1;
        current_peer.state = 2;
        tws_conn.lmp_conn = &phone;
        tws_link.link = &old_peer;
        test_rx_alloc_tx = 1;
        test_rx_tx_empty_calls = 0;
        test_rx_tx_reallocs = 0;
        test_rx_bulk_avail_calls = 0;
        test_rx_irq_link = &current_peer;
        test_rx_irq_change_link = 1;
        packet.bulk.data[3] = 0x80;
        packet.bulk.data[8] = 4;
        tws_conn_rx_handler_in_irq(&packet.bulk);
        test_rx_alloc_tx = 0;
        CHECK(tws_conn.role_switch == 5);
        CHECK(test_rx_tx_reallocs == 1 && test_rx_bulk_avail_calls == 1);
        CHECK(test_rx_tx_empty_calls == 0);

        /* With eSCO active, allocation depends on the freshly loaded peer
         * link's TX-empty result: empty allocates, nonempty returns early. */
        memset(&tws_conn, 0, sizeof(tws_conn));
        memset(&tws_link, 0, sizeof(tws_link));
        memset(&phone, 0, sizeof(phone));
        memset(&phone_link, 0, sizeof(phone_link));
        memset(&old_peer, 0, sizeof(old_peer));
        memset(&current_peer, 0, sizeof(current_peer));
        memset(&packet, 0, sizeof(packet));
        phone.link = &phone_link;
        phone_link.monitor = 1;
        phone.esco_conn = &test_rx_aligned_esco;
        old_peer.state = 1;
        current_peer.state = 2;
        current_peer.esco = (struct bredr_esco_link *)&test_rx_aligned_esco;
        tws_conn.lmp_conn = &phone;
        tws_conn.esco_status = 1;
        tws_link.link = &old_peer;
        test_rx_irq_link = &current_peer;
        test_rx_irq_change_link = 1;
        test_rx_alloc_tx = 1;
        test_rx_tx_empty_result = 1;
        test_rx_tx_empty_calls = 0;
        test_rx_tx_reallocs = 0;
        test_rx_bulk_avail_calls = 0;
        packet.bulk.data[3] = 0x80;
        tws_conn_rx_handler_in_irq(&packet.bulk);
        CHECK(test_rx_tx_empty_calls == 1);
        CHECK(test_rx_tx_reallocs == 1 && test_rx_bulk_avail_calls == 1);
        test_rx_tx_empty_result = 0;
        test_rx_tx_empty_calls = 0;
        test_rx_tx_reallocs = 0;
        test_rx_bulk_avail_calls = 0;
        tws_conn_rx_handler_in_irq(&packet.bulk);
        test_rx_alloc_tx = 0;
        CHECK(test_rx_tx_empty_calls == 1);
        CHECK(test_rx_tx_reallocs == 0 && test_rx_bulk_avail_calls == 0);
        test_rx_tx_empty_result = 1;
    }

    /* Opcode 0x81 only returns for a non-eSCO connection whose link is not
     * in state 2. State 2 proceeds through the max-slot update. */
    {
        struct link_connection peer_link;
        struct { struct bredr_rx_bulk bulk; u8 storage[64]; } packet;
        memset(&peer_link, 0, sizeof(peer_link));
        memset(&packet, 0, sizeof(packet));
        memset(&tws_conn, 0, sizeof(tws_conn));
        memset(&tws_link, 0, sizeof(tws_link));
        memset(&tws_sniff, 0, sizeof(tws_sniff));
        peer_link.state = 3;
        peer_link.tws_link = 1;
        peer_link.role = 1;
        tws_link.link = &peer_link;
        packet.bulk.data[3] = 0x81;
        tws_conn_rx_handler_in_irq(&packet.bulk);
        CHECK(peer_link.core.max_slot == 0);
        peer_link.state = 2;
        tws_conn_rx_handler_in_irq(&packet.bulk);
        CHECK(peer_link.core.max_slot == 5);
    }

    /* The SDK loads the event-63 function ID as a 32-bit little-endian word. */
    {
        struct test_bredr_tx_bulk tx_storage;
        struct bredr_tx_bulk *tx = &tx_storage.bulk;
        memset(&tx_storage, 0, sizeof(tx_storage));
        tx->len = 12;
        tx->data[4] = 0x34;
        tx->data[5] = 0x12;
        tx->data[6] = 0x56;
        tx->data[7] = 0x78;
        test_func_stub_id = 0;
        test_func_stub_data = NULL;
        test_func_stub_len = 0;
        test_func_stub_rx = 1;
        tws_conn_tx_event_handler(63, tx);
        CHECK(test_func_stub_id == 0x78561234);
        CHECK(test_func_stub_data == &tx->data[8] && test_func_stub_len == 4);
        CHECK(test_func_stub_rx == 0);
    }

    /* The IR reads the 4-byte detach instant once to cache it, calls the
     * clock helper, then rereads the input buffer for the offset calculation. */
    {
        struct lmp_connection phone;
        u8 instant_data[4] = { 1, 0, 0, 0 };
        memset(&phone, 0, sizeof(phone));
        memset(&tws_conn, 0, sizeof(tws_conn));
        tws_conn.phone_link_detach_instant = 0;
        test_rx_mutate_after_clock = instant_data;
        test_rx_slot_clk = 456;
        test_rx_clkn_offset = 19;
        tws_conn_sync_rx_core_data(&phone, instant_data, 4, NULL);
        CHECK(tws_conn.phone_link_detach_instant == 1);
        CHECK(test_rx_clkn_begin == 456 && test_rx_clkn_end == 0x10203040);
        CHECK(test_rx_timer_id == 7 && test_rx_timer_slot == 19);
    }

    /* A non-null eSCO pointer with a zero low byte is still non-null. */
    {
        struct link_connection link;
        struct lmp_connection phone __attribute__((aligned(256)));
        memset(&link, 0, sizeof(link));
        memset(&phone, 0, sizeof(phone));
        memset(&tws_conn, 0, sizeof(tws_conn));
        phone.esco_conn = &test_rx_aligned_esco;
        CHECK(((uintptr_t)phone.esco_conn & 0xff) == 0);
        tws_link.link = &link;
        test_rx_sem_pends = 0;
        CHECK(tws_post_remove_esco_link_super_timeout(&phone) == 1);
        CHECK(test_rx_sem_pends == 1 && tws_conn.sem_phone_detach == NULL);
        CHECK(tws_conn.esco_super_timeout == 0);
        tws_link.link = NULL;
    }

    /* Command 14 carries the detach reason in byte 4; reason 13 is retained,
     * the role-switch state is cleared, and task message 46 is posted. */
    {
        struct { struct bredr_rx_bulk bulk; u8 storage[64]; } packet;
        memset(&packet, 0, sizeof(packet));
        tws_conn.detach = 0;
        tws_conn.detach_code = 0;
        tws_conn.role_switch = 5;
        test_rx_taskq_calls = 0;
        packet.bulk.data[3] = 14;
        packet.bulk.data[4] = 13;
        tws_conn_rx_handler_in_irq(&packet.bulk);
        CHECK(tws_conn.detach_code == 13 && tws_conn.role_switch == 0);
        CHECK(test_rx_taskq_calls == 1 && test_rx_taskq_type == 46);
    }

    /* send_event becomes pending exactly when the phone-link RX queue is
     * empty; the inverted condition is the SDK's observed behavior. */
    {
        struct link_connection phone_link;
        struct lmp_connection phone;
        struct list_head queued;
        memset(&phone_link, 0, sizeof(phone_link));
        memset(&phone, 0, sizeof(phone));
        phone.link = &phone_link;
        tws_conn.lmp_conn = &phone;
        INIT_LIST_HEAD(&phone_link.rx_head);
        tws_conn.send_event = 0;
        tws_conn_send_event_check();
        CHECK(tws_conn.send_event == 1);
        INIT_LIST_HEAD(&queued);
        phone_link.rx_head.next = &queued;
        phone_link.rx_head.prev = &queued;
        queued.next = &phone_link.rx_head;
        queued.prev = &phone_link.rx_head;
        tws_conn.send_event = 0;
        tws_conn_send_event_check();
        CHECK(tws_conn.send_event == 0);
        tws_conn.send_event = 2;
        INIT_LIST_HEAD(&phone_link.rx_head);
        tws_conn_send_event_check();
        CHECK(tws_conn.send_event == 2);
    }

    /* Phone-link connection proceeds only for the SDK's detach==0 and
     * existing-peer-link condition. */
    {
        struct link_connection peer_link;
        struct lmp_connection phone;
        memset(&peer_link, 0, sizeof(peer_link));
        memset(&phone, 0, sizeof(phone));
        memset(&tws_conn, 0, sizeof(tws_conn));
        memset(&tws_link, 0, sizeof(tws_link));
        tws_link.link = &peer_link;
        tws_conn.slot_timer = 22;
        test_host_timeout_calls = 0;
        tws_phone_link_connection(&phone);
        CHECK(tws_conn.lmp_conn == &phone && test_host_timeout_calls == 1);
        CHECK(test_rx_timer_id == 22 && test_rx_timer_handler == check_for_setup_monitor_link);
        tws_conn.lmp_conn = NULL;
        tws_conn.detach = 1;
        tws_phone_link_connection(&phone);
        CHECK(tws_conn.lmp_conn == NULL);
        tws_conn.detach = 0;
        tws_link.link = NULL;
        tws_phone_link_connection(&phone);
        CHECK(tws_conn.lmp_conn == NULL);
    }

    /* The switch-post callback can replace conn->link; the SDK reloads the
     * pointer before disabling follow-control. */
    {
        struct link_connection old_link, new_link;
        struct lmp_connection phone;
        memset(&old_link, 0, sizeof(old_link));
        memset(&new_link, 0, sizeof(new_link));
        memset(&phone, 0, sizeof(phone));
        memset(&tws_conn, 0, sizeof(tws_conn));
        memset(&tws_link, 0, sizeof(tws_link));
        phone.link = &old_link;
        old_link.esco = (struct bredr_esco_link *)&test_rx_aligned_esco;
        tws_link.link = &old_link;
        tws_conn.lmp_conn = &phone;
        tws_conn.esco_status = 1;
        tws_conn.monitor_start = 2;
        tws_conn.role_switch = 7;
        tws_conn.role = 0;
        test_rx_follow_ctrl_link = &new_link;
        test_rx_test_link = NULL;
        tws_esco_role_switch_post(NULL);
        CHECK(phone.link == &new_link && test_rx_test_link == &new_link);
        CHECK(tws_conn.role == 1 && tws_conn.role_switch == 0);
    }

    /* The SDK calls is_tws_esco_working before checking a null connection. */
    {
        int before;
        memset(&tws_conn, 0, sizeof(tws_conn));
        memset(&tws_link, 0, sizeof(tws_link));
        tws_conn.role_switch = 7;
        before = test_irq_disable_calls;
        tws_esco_role_switch_post(NULL);
        CHECK(test_irq_disable_calls == before + 1);
    }

    return TEST_DONE("btctrler_tws_pair");
}
#endif
