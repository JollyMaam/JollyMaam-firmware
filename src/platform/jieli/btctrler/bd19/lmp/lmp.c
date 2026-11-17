/* BD19 lmp.c.o occurrence 1: the Classic link manager.
 * The SDK builds this member as one translation unit: Classic/lmp.c includes
 * lmp_name_req.c, lmp_master.c, lmp_slave.c, lmp_inquiry_page.c,
 * lmp_role_switch.c, lmp_encryption.c, lmp_sniff.c, lmp_esco.c, lmp_a2dp.c
 * and lmp_escape3.c, and the static helpers of list.h, cpu.h and
 * bredr_link.h. This file mirrors that: only lmp.c is compiled, the part
 * files beside it are included at the end. Preserve the SDK's ordering,
 * reloads, log calls and quirks. */
/* glue.h exports these for the BR23 split sources; in this member they are
 * file-static, so keep its declarations away from our definitions. */
#define lmp_detach_check glue_lmp_detach_check
#define lmp_rx_handler glue_lmp_rx_handler
#define lmp_send_acl_u_packet_to_host glue_lmp_send_acl_u_packet_to_host
#define lmp_dhkey_check_accept glue_lmp_dhkey_check_accept
#define page_completed glue_page_completed
#define lmp_cmd_is_match_size glue_lmp_cmd_is_match_size
#define link_conn_rx_remote_max_power_res glue_link_conn_rx_remote_max_power_res
#include "btctrler/glue.h"
#undef lmp_detach_check
#undef lmp_rx_handler
#undef lmp_send_acl_u_packet_to_host
#undef lmp_dhkey_check_accept
#undef page_completed
#undef lmp_cmd_is_match_size
#undef link_conn_rx_remote_max_power_res
#include "btctrler/lmp_pdu.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LMP_CODE   __attribute__((section(".classic_lmp_code")))
#define LMP_IRQ    __attribute__((section(".lmp_irq_code")))
#define LMP_DATA   __attribute__((section(".classic_lmp_data")))
#define LMP_BSS    __attribute__((section(".classic_lmp_bss")))
#define LMP_CONST  __attribute__((section(".classic_lmp_const")))
/* Static helpers the SDK kept as separate (inlinehint) bodies. */
#define KEPT       static inline __attribute__((noinline))
/* Static helpers the SDK inlined everywhere (no body left). */
#define INLINE     static inline __attribute__((always_inline))
#define REG32(a) (*(volatile u32 *)(uintptr_t)(a))
#define memcpy __builtin_memcpy
#define memset __builtin_memset
#define memcmp __builtin_memcmp

#define SDK_DIR "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/Classic/"

void log_print(int level, const char *tag, const char *format, ...);
int printf(const char *format, ...);
int puts(const char *s);
int putchar(int c);
void put_buf(const void *buf, int len);
void p33_soft_reset(void);
void delay(u32 cnt);

extern BTCTRLER_CONFIG u8 log_tag_const_v_LMP, log_tag_const_d_LMP, log_tag_const_i_LMP, log_tag_const_e_LMP;
extern BTCTRLER_CONFIG int CONFIG_A2DP_DATA_CACHE_HI_AAC, CONFIG_A2DP_DATA_CACHE_HI_SBC;
extern BTCTRLER_CONFIG int CONFIG_A2DP_DATA_CACHE_LOW_AAC, CONFIG_A2DP_DATA_CACHE_LOW_SBC;
extern BTCTRLER_CONFIG int CONFIG_A2DP_DELAY_TIME, config_bredr_afh_user, CONFIG_BREDR_INQUIRY;
extern BTCTRLER_CONFIG int config_btctler_bredr_master, CONFIG_BTCTLER_FAST_CONNECT_ENABLE;
extern BTCTRLER_CONFIG int config_btctler_hci_standard, config_btctler_le_master_multilink;
extern BTCTRLER_CONFIG int CONFIG_BTCTLER_TWS_ENABLE, config_bt_function, config_bt_security_vulnerability;
extern BTCTRLER_CONFIG int CONFIG_ESCO_MUX_RX_BULK_ENABLE, CONFIG_LMP_MASTER_ESCO_ENABLE;
extern BTCTRLER_CONFIG int CONFIG_LMP_NAME_REQ_ENABLE, CONFIG_LMP_PASSKEY_ENABLE;
extern BTCTRLER_CONFIG int CONFIG_TEST_DUT_ONLY_BOX_CODE, config_update_mode, ESCO_FORWARD_ENABLE;
extern BTCTRLER_CONFIG int sniff_long_interval, sniff_support_reset_anchor_point;
extern void (*lmp_ch_update_resume_hdl)(u8 *);
int get_cur_connect_phone_mac_addr();
int filter_out_sbc_data_en();

/* The generated header declares bool where these SDK bodies return a byte;
 * bind the SDK symbols with their byte ABI. */
u8 lmp_connection_esco_open_ir(void) __asm__("lmp_connection_esco_open");
#define lmp_connection_esco_open lmp_connection_esco_open_ir
u8 check_lmp_detch_over_ir(void) __asm__("check_lmp_detch_over");
#define check_lmp_detch_over check_lmp_detch_over_ir
u8 lmp_rx_role_switch_standby_ir(struct lmp_connection *conn) __asm__("lmp_rx_role_switch_standby");
#define lmp_rx_role_switch_standby lmp_rx_role_switch_standby_ir
u8 lmp_sniff_is_the_main_sniff_ir(struct lmp_connection *conn) __asm__("lmp_sniff_is_the_main_sniff");
#define lmp_sniff_is_the_main_sniff lmp_sniff_is_the_main_sniff_ir
u8 lmp_rx_sniff_standby_ir(struct lmp_connection *conn) __asm__("lmp_rx_sniff_standby");
#define lmp_rx_sniff_standby lmp_rx_sniff_standby_ir
u8 link_idle_task_enable_detect_ir(void) __asm__("link_idle_task_enable_detect");
#define link_idle_task_enable_detect link_idle_task_enable_detect_ir

/* The SDK's log_x macros prefix the level and tag and append "\r\n". */
#define LOG_V(fmt, ...) do { if (log_tag_const_v_LMP) log_print(0, NULL, "[Verb]: [LMP]" fmt "\r\n", ##__VA_ARGS__); } while (0)
#define LOG_D(fmt, ...) do { if (log_tag_const_d_LMP) log_print(1, NULL, "[Debug]: [LMP]" fmt "\r\n", ##__VA_ARGS__); } while (0)
#define LOG_I(fmt, ...) do { if (log_tag_const_i_LMP) log_print(2, NULL, "[Info]: [LMP]" fmt "\r\n", ##__VA_ARGS__); } while (0)
#define LOG_E(fmt, ...) do { if (log_tag_const_e_LMP) log_print(4, NULL, "<Error>: [LMP]" fmt "\r\n", ##__VA_ARGS__); } while (0)

/* The SDK evaluates the condition once, then branches on config_asser;
 * each part file defines LMP_FILE. */
#define SDK_ASSERT(p, line, text, ...) do { _Bool ok_ = (p); if (config_asser) { if (!ok_) { \
    printf("file:%s, line:%d", SDK_DIR LMP_FILE, line); printf("ASSERT-FAILD: " text, ##__VA_ARGS__); \
    cpu_assert_debug(); } } else if (!ok_) cpu_reset(); } while (0)

#define FOR_EACH_LINK(lc) \
    for ((lc) = (struct link_connection *)bredr_link_v.link_conn_head.next; \
         &(lc)->entry != &bredr_link_v.link_conn_head; (lc) = (struct link_connection *)(lc)->entry.next)

/* ---- include_lib/driver/cpu/bd19/asm/cpu.h, system/generic/list.h ------- */

KEPT void cpu_reset(void) { p33_soft_reset(); }

KEPT int list_empty(const struct list_head *head)
{
    return head->next == head;
}

KEPT void list_add_tail(struct list_head *new, struct list_head *head)
{
    struct list_head *prev = head->prev;

    head->prev = new;
    new->next = head;
    new->prev = prev;
    prev->next = new;
}

KEPT void __list_del_entry(struct list_head *entry)
{
    struct list_head *prev = entry->prev, *next = entry->next;

    next->prev = prev;
    prev->next = next;
}

KEPT void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

/* ---- lib/btctrler/include/classic/bredr_link.h ---------------------------- */

static void lmp_sniff_anchor_timeout(void *priv);

KEPT void link_conn_set_max_slot(struct link_connection *link, u8 slot)
{
    link->core.max_slot = slot;
}

KEPT u8 link_conn_get_max_slot(struct link_connection *link)
{
    return link->core.max_slot;
}

KEPT s8 link_conn_get_rssi(struct link_connection *link)
{
    return bredr_analog_get_rssi(link->frame);
}

KEPT void link_conn_set_acl_u_tx_enable(struct link_connection *link, u8 en)
{
    /* bit 0 in this member's view of the field */
    link->send_acl_enable = en ? link->send_acl_enable | 1 : link->send_acl_enable & ~1;
}

KEPT void link_conn_rx_remote_max_power_res(struct link_connection *link, u8 stop)
{
    link->power.normal.max_power = 1;
    if (stop)
        link->stop_incr_power = 1;
}

KEPT void link_conn_rx_remote_min_power_res(struct link_connection *link)
{
    link->power.normal.min_power = 1;
}

KEPT void link_conn_set_esco_lt_addr(struct link_connection *link, u8 lt_addr)
{
    link->esco_lt_addr = lt_addr;
}

KEPT void link_conn_a2dp_channel_open(struct link_connection *link)
{
    link_task_reset_slot(&link->task, 2);
}

KEPT void link_conn_a2dp_channel_close(struct link_connection *link)
{
    link_task_reset_slot(&link->task, 1);
}

KEPT void link_conn_run(struct link_connection *link)
{
    if (link)
        link_task_run(&link->task);
    else
        link_task_run(NULL);
}

KEPT void link_conn_run_slots(struct link_connection *link, int slots)
{
    link_task_run_slots(&link->task, slots);
}

KEPT void link_conn_run_enable(struct link_connection *link, u8 en)
{
    link_task_run_set(&link->task, en);
}

KEPT void link_conn_disable_afh(struct link_connection *link)
{
    if (link->afh_timer != 0xff) {
        slot_timer_put(link->afh_timer);
        link->afh_timer = 0xff;
    }
    bdhw_disable_afh(link->frame);
}

KEPT void link_conn_enter_role_switch_probe(struct link_connection *link)
{
    bredr_bd_frame_disable(link->frame);
}

KEPT void link_conn_pll_comp_reset(struct link_connection *link)
{
    bredr_pll_comp_reset(link->frame);
}

KEPT void link_conn_set_first_anchor_point_timer(struct link_connection *link, u8 timer, void *priv, u32 slots)
{
    slot_timer_set_finecnt(timer, lmp_sniff_anchor_timeout, priv, READ_SLOT_CLK() + slots, link->frame->bitoff);
}

KEPT void link_conn_set_afh_enable(struct link_connection *link, u8 *map, u32 instant)
{
    link->afh.en = 1;
    link->afh.instant = instant;
    memcpy(link->channel_map, map, 10);
}

/* ---- state ----------------------------------------------------------------- */

struct bredr_dut *g_dut_hdl LMP_DATA;
u8 g_lmp_update_start_flag LMP_DATA;
u8 sniff_establish_by_remote LMP_DATA;
struct lmp_connection *main_conn LMP_DATA;
struct lmp_connection *other_conn LMP_DATA;
static u16 sniff_t_slot LMP_DATA = 800;
static u16 sniff_attemp LMP_DATA = 4;
static u8 sniff_num LMP_DATA;
u8 last_sniff_Communicate LMP_DATA;
static u8 sniff_anchor_point_start_tx_empty LMP_DATA;
static struct lmp_connection lmp_acl_link[1] LMP_BSS;
u8 key_special_bind LMP_DATA;
struct lmp_connection *update_conn LMP_DATA;
static volatile int bt_res_updata_flag LMP_DATA;
static u8 key_action_test_flag LMP_DATA;
static int (*uboot_revic_handle)(u8 *, int) LMP_DATA;
const u8 ex_info_type_match_len_tab[5][2] LMP_CONST = { { 0, 5 }, { 1, 18 }, { 2, 6 }, { 4, 4 }, { 3, 34 } };
static u16 (*get_vbat_level_handle)(void) LMP_DATA;
static u8 (*get_vbat_percent_handle)(void) LMP_DATA;
static u8 *(*sdfile_get_burn_code_handle)(u8 *) LMP_DATA;
static u8 *bt_get_flash_id(void);
static u16 bt_get_chip_key_crc(void);
static u8 *(*get_flash_id_handle)(void) LMP_DATA = bt_get_flash_id;
static u16 (*get_chip_key_crc_handle)(void) LMP_DATA = bt_get_chip_key_crc;
static u8 *(*get_sdk_ver_info_handle)(u8 *) LMP_DATA;
static u8 fre_offset_trim_flag LMP_DATA;
struct page_parm_t page_parm LMP_DATA;
static u16 set_qos_poll_interval LMP_BSS;
u8 local_private_key[24] LMP_BSS;
struct inquiry_scan_parm_t inquiry_scan_parm LMP_BSS;
struct page_scan_parm_t page_scan_parm LMP_BSS;
static void (*lmp_update_rx_handler)(int, struct bredr_rx_bulk *) LMP_DATA;
static u8 lmp_dut_en LMP_DATA;
static u8 lmp_dut_phone_en LMP_DATA;
static u16 prev_seqn_number LMP_DATA;
static const char test_name[10] LMP_CONST = "autotest";

#define LMP_FILE "lmp.c"

/* ---- Classic/lmp.c ----------------------------------------------------------- */

static const lmp_match_t lmp_match_cmd_table[38] LMP_CONST = {
    { 6, 2 }, { 22, 17 }, { 70, 1 }, { 10, 1 }, { 12, 3 }, { 64, 2 }, { 14, 2 }, { 130, 17 },
    { 122, 4 }, { 124, 17 }, { 116, 1 }, { 118, 3 }, { 32, 2 }, { 30, 2 }, { 12, 16 }, { 78, 9 },
    { 80, 9 }, { 102, 1 }, { 66, 1 }, { 90, 2 }, { 92, 2 }, { 2, 2 }, { 4, 17 }, { 8, 3 },
    { 88, 3 }, { 86, 7 }, { 98, 1 }, { 104, 9 }, { 46, 10 }, { 34, 17 }, { 36, 1 }, { 24, 5 },
    { 110, 3 }, { 94, 1 }, { 96, 3 }, { 48, 1 }, { 74, 6 }, { 76, 6 },
};
static const lmp_match_t lmp_match_cmd_table_e3[2] LMP_CONST = { { 1, 4 }, { 2, 5 } };
static const lmp_match_t lmp_match_cmd_table_e4[7] LMP_CONST = {
    { 1, 4 }, { 3, 12 }, { 4, 12 }, { 2, 5 }, { 11, 3 }, { 12, 16 }, { 13, 4 },
};

static u8 LMP_CODE lmp_cmd_is_match_size(u8 opcode, u8 *data, u8 len)
{
    const lmp_match_t *table = lmp_match_cmd_table;
    u8 num = 38, op = opcode, i;

    if (!config_bt_security_vulnerability)
        return 1;
    switch (opcode) {
    case 0xfc:
        table = lmp_match_cmd_table_e3;
        num = 2;
        op = data[1];
        break;
    case 0xfe:
        table = lmp_match_cmd_table_e4;
        num = 7;
        op = data[1];
        break;
    }
    for (i = 0; i < num; i++) {
        if (table[i].cmd == op) {
            if (table[i].cmd_len == len)
                return 1;
            LOG_E("no_match op= %d,%d-%d-%d!!!", op, i, len, table[i].cmd_len);
            return 0;
        }
    }
    return 1;
}

void LMP_CODE lmp_send_event_encryption_change(struct lmp_connection *conn)
{
    hci_send_event(8, "1H1", 0, conn->core_s.handle, 1);
}

void LMP_CODE lmp_send_event_role_change(struct lmp_connection *conn, u8 result)
{
    hci_send_event(18, "1A1", result, conn->core_s.remote_addr, conn->core.role);
}

void LMP_CODE lmp_send_event_link_key_notification(struct lmp_connection *conn, int ret)
{
    hci_send_event(24, "Ac161", conn->core_s.remote_addr, conn->core.link_key, ret);
}

void LMP_CODE lmp_send_event_connection_request(struct lmp_connection *conn, u8 link_type)
{
    hci_send_event(4, "A31", conn->core_s.remote_addr, conn->core_s.remote_class_dev, link_type);
}

void LMP_CODE lmp_send_event_link_request(struct lmp_connection *conn)
{
    hci_send_event(23, "A", conn->core_s.remote_addr);
}

void LMP_CODE lmp_send_event_auth_complete(struct lmp_connection *conn, u8 result)
{
    hci_send_event(6, "1H", result, conn->core_s.handle);
}

void LMP_CODE lmp_send_event_connection_complete(struct lmp_connection *conn, u8 err_code, u16 handle, u8 link_type)
{
    hci_send_event(3, "1HA11", err_code, handle, conn->core_s.remote_addr, link_type, 0);
    os_time_dly(1);
}

void LMP_CODE lmp_send_event_mode_change(struct lmp_connection *conn, u16 T_sniff)
{
    hci_send_event(20, "1212", 0, conn->core_s.handle, conn->core.operation_mode, T_sniff);
}

void LMP_CODE lmp_f3_function(struct lmp_connection *conn, u8 *out)
{
    u8 io_cap[3];
    u8 r[16];
    struct lmp_simple_pair *simple;

    memset(r, 0, 16);
    simple = conn->simple;
    io_cap[0] = simple->local_IO_Capability;
    io_cap[1] = simple->local_OOB_Data_Present;
    io_cap[2] = simple->local_Authentication_Req;
    if (simple->simple_protocols == 2)
        memcpy(r, &simple->passkey, 4);
    LOG_I("passkey = %d\n", conn->simple->passkey);
    f3_function_api(conn->simple->local_ecdh_key, conn->simple->local_random_num, conn->simple->remote_random_num,
                    r, io_cap, hci_param.addr, conn->core_s.remote_addr, out);
}

void LMP_CODE make_xor(u8 *a, u8 *b, u8 *out)
{
    int i;

    for (i = 0; i < 16; i++)
        out[i] = a[i] ^ b[i];
}

__attribute__((weak)) void LMP_CODE get_random_number(u8 *buf, u8 len)
{
    u32 rand;
    u8 n;

    while (len) {
        rand = REG32(0x1e3b00);
        n = len < 4 ? len : 4;
        memcpy(buf, &rand, n);
        buf += n;
        len -= n;
        delay(10);
    }
}

void LMP_CODE make_rand_num(u8 *buf)
{
    get_random_number(buf, 16);
}

void LMP_CODE lmp_init(void)
{
    struct lmp_connection *conn;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++)
        memset(conn, 0, sizeof(*conn));
}

struct lmp_connection *LMP_CODE lmp_create_conn(void)
{
    struct lmp_connection *conn;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->core_s.handle == 0) {
            memset(conn, 0, sizeof(*conn));
            conn->core_s.handle = conn - lmp_acl_link + 0x30;
            INIT_LIST_HEAD(&conn->rx_acl_u);
            conn->change_rx_bulk_timer = 0xff;
            return conn;
        }
    }
    return NULL;
}

void LMP_CODE __1to2_test_putchar_for_addr(u8 *addr, int a, int b)
{
}

struct lmp_connection *LMP_CODE lmp_conn_for_active_state(void)
{
    struct lmp_connection *conn;

    local_irq_disable();
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->core_s.handle == 0 || conn->link == NULL)
            continue;
        if (CONFIG_BTCTLER_TWS_ENABLE) {
            local_irq_enable();
            return conn;
        }
        if (conn->esco_conn || conn->sbc_open) {
            local_irq_enable();
            return conn;
        }
    }
    local_irq_enable();
    return NULL;
}

struct lmp_connection *LMP_IRQ lmp_conn_for_address(u8 *addr)
{
    struct lmp_connection *conn;

    local_irq_disable();
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->core_s.handle == 0 || conn->link == NULL)
            continue;
        if (addr == NULL) {
            local_irq_enable();
            return conn;
        }
        if (memcmp(conn->core_s.remote_addr, addr, 6) == 0) {
            local_irq_enable();
            return conn;
        }
    }
    local_irq_enable();
    return NULL;
}

s8 LMP_CODE bredr_get_rssi_for_address(u8 *addr)
{
    struct lmp_connection *conn;
    s8 rssi = -127;

    if (addr == NULL)
        return -127;
    local_irq_disable();
    conn = lmp_conn_for_address(addr);
    if (conn)
        rssi = link_conn_get_rssi(conn->link);
    local_irq_enable();
    return rssi;
}

s8 LMP_CODE get_rssi_api(s8 *phone_rssi, s8 *tws_rssi)
{
    struct ctrl_frame *frame;

    if (tws_rssi)
        *tws_rssi = -127;
    if (phone_rssi)
        *phone_rssi = -127;
    if (tws_link.link) {
        frame = tws_link.link->frame;
        if (tws_rssi && frame)
            *tws_rssi = bredr_analog_get_rssi(frame);
        if (tws_link.plink) {
            frame = tws_link.plink->frame;
            if (phone_rssi && frame)
                *phone_rssi = bredr_analog_get_rssi(frame);
        }
    } else if (phone_rssi) {
        *phone_rssi = bredr_get_rssi_for_address((u8 *)get_cur_connect_phone_mac_addr());
    }
    return 0;
}

struct lmp_connection *LMP_CODE lmp_conn_for_handle(int handle)
{
    struct lmp_connection *conn;

    if (!(config_btctler_modules & 1))
        return NULL;
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->core_s.handle && conn->core_s.handle == handle)
            return conn;
    }
    return NULL;
}

struct lmp_connection *LMP_CODE lmp_get_other_conn(struct lmp_connection *conn)
{
    struct lmp_connection *p;

    for (p = lmp_acl_link; p < lmp_acl_link + 1; p++) {
        if (p != conn && p->core_s.handle)
            return p;
    }
    return NULL;
}

struct lmp_connection *LMP_CODE lmp_esco_conn_for_handle(int handle)
{
    struct lmp_connection *conn;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->esco_conn && conn->esco_conn->hci_handle == handle)
            return conn;
    }
    return NULL;
}

void LMP_CODE lmp_esco_rejust_establish(u8 req)
{
    struct lmp_connection *conn;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->core_s.handle)
            conn->rejust_esco_req = req;
    }
}

struct lmp_connection *LMP_IRQ lmp_conn_for_link(void *link)
{
    struct lmp_connection *conn;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->link == link)
            return conn;
    }
    return NULL;
}

u8 *LMP_CODE lmp_private_remote_addr_for_handler(int handle)
{
    struct lmp_connection *conn = lmp_conn_for_handle(handle);

    if (conn)
        return conn->core_s.remote_addr;
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->core_s.handle && conn->esco_conn && conn->esco_conn->hci_handle == handle)
            return conn->core_s.remote_addr;
    }
    return NULL;
}

u16 LMP_CODE lmp_private_handler_for_remote_addr(u8 *addr, int link_type)
{
    struct lmp_connection *conn = lmp_conn_for_address(addr);
    struct lmp_esco *esco;

    if (conn == NULL)
        return 0;
    if (link_type == 1) {
        if ((conn->core.state & 9) == 8)
            return 0;
        return conn->core_s.handle;
    }
    esco = conn->esco_conn;
    if (esco && memcmp(conn->core_s.remote_addr, addr, 6) == 0)
        return esco->hci_handle;
    return 0;
}

u8 LMP_CODE lmp_get_tws_phone_addr(u8 *buf, int type)
{
    struct lmp_connection *conn;

    if (type != 1)
        return 0;
    conn = lmp_conn_for_address(NULL);
    if (conn == NULL)
        return 0;
    memcpy(buf, &conn->core_s.handle, 2);
    memcpy(buf + 2, conn->core_s.remote_addr, 6);
    return 1;
}

u8 LMP_CODE lmp_connection_esco_open(void)
{
    struct lmp_connection *conn;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->core_s.handle && conn->esco_conn)
            return 1;
    }
    return 0;
}

u8 *LMP_CODE lmp_conn_remote_address(struct lmp_connection *conn)
{
    return conn->core_s.remote_addr;
}

void LMP_CODE lmp_setup_complete(struct lmp_connection *conn)
{
    u8 map[10];

    memset(map, 0, 10);
    if (conn->core.state & 4)
        return;
    conn->core.state |= 4;
    lmp_request(conn, "1", PDU(SETUP_COMPLETE));
    memset(map, 0xff, 10);
    lmp_tx_set_afh_req(conn, map);
    lmp_tx_name_req(conn);
}

void LMP_CODE lmp_rx_features_req_ext(struct lmp_connection *conn, u8 *data)
{
    u8 *dst, page, max = 0;
    int i;

    if (data[0] == 0)
        dst = conn->core_s.remote_features;
    else
        dst = &conn->core_s.remote_ext_features[(data[0] << 3) - 8];
    memcpy(dst, data + 2, 8);
    for (i = 0; i < 8; i++)
        max |= hci_param.ext_features[i + 8];
    page = data[0];
    lmp_response(conn, "b04C008", 0xfe, FEATURES_RES_EXT, page, max ? 2 : 1,
                 &hci_param.ext_features[(page << 3) - 8]);
}

void LMP_CODE lmp_rx_features_res_ext(struct lmp_connection *conn, u8 *data)
{
    if (data[0] == 0) {
        LOG_E("ext_page = 0 ????");
        return;
    }
    memcpy(&conn->core_s.remote_ext_features[(data[0] << 3) - 8], data + 2, 8);
    if (!(conn->core.state & 1))
        hci_send_event(61, "AC008", conn->core_s.remote_addr, conn->core_s.remote_ext_features);
}

void LMP_CODE lmp_rx_feature_req(struct lmp_connection *conn, u8 *data)
{
    memcpy(conn->core_s.remote_features, data, 8);
    lmp_response(conn, "1C008", PDU(FEATURES_RES), hci_param.features);
}

void LMP_CODE lmp_rx_features_res(struct lmp_connection *conn, u8 *data)
{
    memcpy(conn->core_s.remote_features, data, 8);
}

void LMP_CODE lmp_rx_version_req(struct lmp_connection *conn)
{
    lmp_response(conn, "1122", PDU(VERSION_RES), hci_param.lmp_pal_version, hci_param.manufacturer_name,
                 hci_param.lmp_pal_subversion);
}

void LMP_CODE lmp_rx_version_res(struct lmp_connection *conn, u8 *data)
{
    hci_send_event(12, "1H122", 0, conn->core_s.handle, data[0], btctler_little_endian_read_16(data, 1),
                   btctler_little_endian_read_16(data, 3));
}

void LMP_CODE lmp_rx_name_req(struct lmp_connection *conn, u8 *data)
{
    u8 offset;

    conn->core.state |= 0x400000;
    offset = data[0];
    lmp_response(conn, "111C014", PDU(NAME_RES), offset, strlen(gbredr_local_dev.local_name),
                 &gbredr_local_dev.local_name[offset]);
}

void LMP_CODE lmp_rx_name_res(struct lmp_connection *conn, u8 *data)
{
    u8 len, offset;

    if (!CONFIG_LMP_NAME_REQ_ENABLE)
        return;
    if (conn->remote_name == NULL) {
        conn->remote_name = lmp_malloc(248);
        SDK_ASSERT(conn->remote_name, 866, "conn->remote_name ");
        memset(conn->remote_name, 0, 248);
    }
    len = data[1];
    offset = data[0];
    if (len > offset + 14) {
        memcpy(conn->remote_name + offset, data + 2, 14);
        lmp_response(conn, "11", PDU(NAME_REQ), data[0] + 14);
        return;
    }
    memcpy(conn->remote_name + offset, data + 2, (u8)(len - offset));
    LOG_I("REMOTE_NAME : %s", conn->remote_name);
    hci_send_event(7, "1AC032", 0, conn->core_s.remote_addr, conn->remote_name);
    lmp_free(conn->remote_name);
    conn->remote_name = NULL;
}

void LMP_CODE lmp_tx_max_slot(struct lmp_connection *conn)
{
    int f = conn->core_s.remote_features[0];
    int slot;

    if (f & 2)
        slot = 5;
    else
        slot = ((f << 1) & 2) | 1;
    link_conn_set_max_slot(conn->link, slot);
    lmp_request(conn, "11", PDU(MAX_SLOT), slot);
}

void LMP_CODE lmp_rx_max_slot(struct lmp_connection *conn, u8 *data)
{
    struct link_connection *link = conn->link;
    u8 slot = link_conn_get_max_slot(link);

    if (slot == data[0])
        return;
    if (slot < data[0] && lmp_connection_esco_open()) {
        lmp_request(conn, "11", PDU(MAX_SLOT), slot);
        return;
    }
    link_conn_set_max_slot(link, data[0]);
}

void LMP_CODE lmp_rx_max_slot_req(struct lmp_connection *conn, u8 *data)
{
    struct link_connection *link = conn->link;
    u8 slot = link_conn_get_max_slot(link);

    if (slot < data[0] && lmp_connection_esco_open()) {
        LOG_I("not accept slot");
        lmp_response(conn, "111", PDU(NOT_ACCEPTED), MAX_SLOT_REQ, 0x1e);
        return;
    }
    link_conn_set_max_slot(link, data[0]);
    lmp_response(conn, "11", PDU(ACCEPTED), MAX_SLOT_REQ);
}

void LMP_CODE lmp_rx_accepted_max_slot_req(struct lmp_connection *conn)
{
    u8 slot = link_conn_get_max_slot(conn->link);

    hci_send_event(27, "21", conn->core_s.handle, slot);
}

void LMP_CODE lmp_rx_not_accepted_max_slot_req(struct lmp_connection *conn)
{
}

void LMP_CODE lmp_rx_temp_key(struct lmp_connection *conn)
{
    hci_send_event(10, "121", 0, conn->core_s.handle, 1);
}

void LMP_CODE lmp_rx_timing_accuracy_req(struct lmp_connection *conn)
{
    lmp_response(conn, "111", PDU(TIMING_ACCURACY_RES), gbredr_local_dev.drift, gbredr_local_dev.jitter);
}

void LMP_CODE lmp_rx_timing_accuracy_res(struct lmp_connection *conn, u8 *data)
{
}

void LMP_CODE lmp_tx_timing_accuracy_req(struct lmp_connection *conn)
{
    if (conn->core.state & 0x100)
        return;
    conn->core.state |= 0x100;
    lmp_request(conn, "111", PDU(TIMING_ACCURACY_REQ), gbredr_local_dev.drift, gbredr_local_dev.jitter);
}

void LMP_CODE lmp_vendor_qos_interval_config(u16 interval)
{
    if (interval == 0 || (interval >= 6 && interval <= 4096))
        set_qos_poll_interval = interval;
}

void LMP_CODE lmp_tx_qos_req(struct lmp_connection *conn)
{
    if ((conn->core.state & 0x200) || set_qos_poll_interval == 0)
        return;
    LOG_I("send LMP_QUALITY_OF_SERVICE_REQ");
    conn->core.state |= 0x200;
    lmp_request(conn, "121", PDU(QUALITY_OF_SERVICE_REQ), set_qos_poll_interval, 2);
}

void LMP_CODE lmp_tx_clkoffset_req(struct lmp_connection *conn)
{
    lmp_request(conn, "1", PDU(CLKOFFSET_REQ));
}

void LMP_CODE lmp_rx_clkoffset_req(struct lmp_connection *conn)
{
    lmp_response(conn, "12", PDU(CLKOFFSET_RES), (bredr_link_clk_offset(conn->link) >> 1) & 0x7fff);
}

void LMP_CODE lmp_rx_clkoffset_res(struct lmp_connection *conn, u8 *data)
{
    hci_send_event(28, "122", 0, conn->core_s.handle, data[1] << 8 | data[0]);
}

void LMP_CODE lmp_rx_detch(struct lmp_connection *conn, u8 *data)
{
    switch (data[0]) {
    case 0x08:
        data[0] = 0x13;
        break;
    case 0x22:
        data[0] = 0x08;
        break;
    }
    conn->err_code = data[0];
    conn->core.state |= 8;
    os_time_dly(20);
}

void LMP_CODE lmp_tx_detch(struct lmp_connection *conn, u8 reason)
{
    if (conn->link == NULL || (conn->core.state & 8))
        return;
    conn->core.step = LMP_TX_FINISH;
    conn->core.stage = LMP_STAGE_DISCONN;
    conn->core.state |= 8;
    conn->err_code = reason;
    lmp_request(conn, "11", PDU(DETACH), reason == 0x16 ? 0x13 : reason);
    bredr_link_event(conn->link, 1, 5);
    os_time_dly(20);
}

void LMP_CODE lmp_tx_packet_type_table_req(struct lmp_connection *conn)
{
    LOG_I("LMP_TX_PACKET_TYPE_TABLE_REQ");
    if ((config_btctler_mode & 1) && g_dut_hdl == NULL)
        lmp_request(conn, "111", 0xfe, PACKET_TYPE_TABLE_REQ, 1);
}

void LMP_CODE lmp_rx_packet_type_table_req(struct lmp_connection *conn, u8 *data)
{
    if ((config_btctler_mode & 1) && g_dut_hdl == NULL && data[0] == 0)
        lmp_response(conn, "b05", 0xfe, NOT_ACCEPTED_EXT, ESCAPE_4, PACKET_TYPE_TABLE_REQ, 0x1e);
    else
        lmp_response(conn, "1111", 0xfe, ACCEPTED_EXT, ESCAPE_4, PACKET_TYPE_TABLE_REQ);
}

void LMP_CODE lmp_connection_ctl_slot(struct lmp_connection *conn, u8 slot)
{
    struct lmp_connection *p;

    for (p = lmp_acl_link; p < lmp_acl_link + 1; p++) {
        if (p == conn || p->core_s.handle == 0)
            continue;
        link_conn_set_max_slot(p->link, slot);
        lmp_request(p, "11", PDU(MAX_SLOT), slot);
    }
}

void LMP_CODE lmp_send_max_slot(struct lmp_connection *conn, u8 slot)
{
    lmp_request(conn, "M11", slot, PDU(MAX_SLOT), slot);
}

void LMP_CODE lmp_tx_channel_classification_timeout(void *priv)
{
    static u8 last_map[10] LMP_DATA;
    struct lmp_connection *conn = priv;

    if (conn == NULL || conn->link == NULL || conn->afh_report.timer == 0)
        return;
    conn->afh_report.timer = 0;
    if (memcmp(conn->afh_report.tx_channel_classification, last_map, 10) == 0)
        return;
    conn->tran_id &= ~1;
    lmp_response(conn, "11C010", 0xfe, CHANNEL_CLASSIFICATION, conn->afh_report.tx_channel_classification);
    memcpy(last_map, conn->afh_report.tx_channel_classification, 10);
}

void LMP_CODE lmp_tx_channel_classification(u8 *map)
{
    struct lmp_connection *conn;
    u16 interval;

    if (!(hci_param.features[4] & 0x10))
        return;
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->core_s.handle == 0)
            continue;
        if (conn->afh_report.mode == 0 || conn->rs_process || conn->core.role != 1 ||
            !(conn->core_s.remote_features[4] & 0x10))
            continue;
        if (conn->afh_report.timer)
            continue;
        memcpy(conn->afh_report.tx_channel_classification, map, 10);
        interval = conn->afh_report.min_interval;
        if (interval <= 1792) {
            interval = conn->afh_report.max_interval;
            interval = interval < 1792 ? interval : 1792;
        }
        conn->afh_report.timer = sys_timeout_add(conn, lmp_tx_channel_classification_timeout, (u16)(interval * 0.625));
    }
}

void LMP_CODE lmp_master_tx_set_afh_user(u8 *map)
{
    struct lmp_connection *conn;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->core_s.handle && conn->core.role == 0)
            lmp_tx_set_afh_req(conn, map);
    }
}

void LMP_CODE lmp_rx_channel_classification(struct lmp_connection *conn, u8 *data)
{
    memcpy(conn->afh_report.rx_channel_classification, data, 10);
}

void LMP_CODE lmp_tx_channel_classification_req(struct lmp_connection *conn, u8 mode)
{
    conn->afh_report.mode = mode;
    lmp_request(conn, "11122", 0xfe, CHANNEL_CLASSIFICATION_REQ, mode, 1600, 48000);
}

void LMP_CODE lmp_rx_channel_classification_req(struct lmp_connection *conn, u8 *data)
{
    u8 mode = data[0];

    conn->afh_report.mode = mode;
    conn->afh_report.min_interval = data[2] << 8 | data[3];
    conn->afh_report.max_interval = data[4] << 8 | data[5];
    if (mode == 0 && conn->afh_report.timer) {
        sys_timeout_del(conn->afh_report.timer);
        conn->afh_report.timer = 0;
    }
}

void LMP_CODE lmp_channel_classification_close(struct lmp_connection *conn)
{
    conn->afh_report.mode = 0;
    if (conn->afh_report.timer) {
        sys_timeout_del(conn->afh_report.timer);
        conn->afh_report.timer = 0;
    }
}

void LMP_CODE lmp_rx_not_accepted_afh_req(struct lmp_connection *conn)
{
}

void LMP_CODE __lmp_enable_afh(struct lmp_connection *conn)
{
}

void LMP_CODE lmp_set_fix_power(u8 *addr, u8 level)
{
    struct lmp_connection *conn = lmp_conn_for_address(addr);

    if (conn)
        bredr_pwr_set(conn->link, 0, level);
}

void LMP_CODE lmp_rx_incr_power(struct lmp_connection *conn)
{
    u8 ret = bredr_pwr_set(conn->link, 1, 1);

    if ((config_btctler_mode & 1) && g_dut_hdl == NULL) {
        if (ret == 2 && link_conn_tx_empty(conn->link)) {
            LOG_I("LMP_TX_MAX_POWER\n");
            lmp_response(conn, "1", PDU(MAX_POWER));
        }
    } else if (ret == 2) {
        LOG_I("LMP_TX_MAX_POWER\n");
        lmp_response(conn, "1", PDU(MAX_POWER));
    }
}

void LMP_CODE lmp_rx_decr_power(struct lmp_connection *conn)
{
    if ((u8)bredr_pwr_set(conn->link, 1, -1) == 1)
        lmp_response(conn, "1", PDU(MIN_POWER));
}

void LMP_CODE lmp_rx_max_power(struct lmp_connection *conn)
{
    link_conn_rx_remote_max_power_res(conn->link, 1);
}

void LMP_CODE lmp_rx_min_power(struct lmp_connection *conn)
{
    link_conn_rx_remote_min_power_res(conn->link);
}

void LMP_CODE lmp_tx_power_req(struct lmp_connection *conn, u8 type)
{
    if (conn == NULL || (conn->core.state & 0x200000))
        return;
    switch (type) {
    case 1:
        if ((conn->core_s.remote_features[7] & 4) && (hci_param.features[7] & 4))
            lmp_request(conn, "P111", 0xfe, POWER_CONTROL_REQ, 0);
        else
            lmp_request(conn, "P11", PDU(DECR_POWER_REQ), 0);   /* type 1 lowers, type 2 raises */
        break;
    case 2:
        if ((conn->core_s.remote_features[7] & 4) && (hci_param.features[7] & 4))
            lmp_request(conn, "P111", 0xfe, POWER_CONTROL_REQ, 1);
        else
            lmp_request(conn, "P11", PDU(INCR_POWER_REQ), 0);
        break;
    }
}

void LMP_CODE lmp_set_supervision_timeout(struct lmp_connection *conn, int timeout)
{
}

void LMP_CODE lmp_rx_supervision_timeout(struct lmp_connection *conn, u8 *data)
{
    hci_send_event(56, "22", conn->core_s.handle, data[1] << 8 | data[0]);
}

void LMP_CODE lmp_tx_disconnect(struct lmp_connection *conn, int *hci_cmd)
{
    lmp_tx_detch(conn, hci_cmd[2]);
}

void LMP_CODE lmp_rx_quality_of_service_req(struct lmp_connection *conn, u8 *data)
{
    lmp_response(conn, "11", PDU(ACCEPTED), QUALITY_OF_SERVICE_REQ);
}

void LMP_CODE lmp_rx_host_connection_req(struct lmp_connection *conn)
{
    u32 state = conn->core.state;

    conn->core.state = state | 1;
    if (!(state & 0x400000))
        hci_send_event(0xf9, "A1", conn->core_s.remote_addr, 0);
    SDK_ASSERT(conn->core.role == 1, 1503, "conn->core.role == ROLE_SLAVE ");
    if (CONFIG_BTCTLER_FAST_CONNECT_ENABLE && !conn->link->fast_con_flag && !conn->link->test_con_flag)
        lmp_tx_detch(conn, 0x13);
}

void LMP_CODE lmp_rx_setup_complete(struct lmp_connection *conn)
{
    lmp_setup_complete(conn);
    lmp_send_event_connection_complete(conn, 0, conn->core_s.handle, 1);
    if (conn->core.role != 1 && !(conn->core.state & 0x2000))
        return;
    conn->core.state = (conn->core.state & ~0x43) | 0x42;
    hci_send_event(32, "A1", conn->core_s.remote_addr, conn->core_s.remote_sr);
}

void LMP_CODE lmp_rx_accepted_host_connection_req(struct lmp_connection *conn)
{
    u32 state = conn->core.state;

    if (state & 8) {
        conn->err_code = 0x16;
        lmp_request(conn, "11", PDU(DETACH), 0x16);
        return;
    }
    if (!(state & 0x400000))
        hci_send_event(0xf9, "A1", conn->core_s.remote_addr, 0);
    lmp_tx_setup_complete(conn);
    conn->core.state = (conn->core.state & ~3) | 2;
}

void LMP_CODE lmp_rx_not_accepted_host_connection_req(struct lmp_connection *conn, u8 *data)
{
    lmp_tx_detch(conn, data[1]);
}

void LMP_CODE lmp_tx_resume_encryption_req(struct lmp_connection *conn)
{
    if (conn->core.enc_state & 0x1000) {
        conn->core.enc_state &= ~0x1000;
        lmp_response(conn, "11", 0xfe, RESUME_ENCRYPTION_REQ);
    }
}

void LMP_CODE lmp_tx_supervision_timeout(struct lmp_connection *conn)
{
    lmp_request(conn, "12", PDU(SUPERVISION_TIMEOUT), gbredr_local_dev.super_timeout_value * 1000 / 625);
}

void LMP_CODE lmp_tx_features_req(struct lmp_connection *conn)
{
    lmp_request(conn, "1C008", PDU(FEATURES_REQ), hci_param.features);
}

void LMP_CODE lmp_tx_get_remote_addr_req(struct lmp_connection *conn)
{
    lmp_request(conn, "111", 0xfc, 0x84, 1);
}

void LMP_CODE lmp_tx_version_req(struct lmp_connection *conn)
{
    lmp_request(conn, "1122", PDU(VERSION_REQ), hci_param.lmp_pal_version, hci_param.manufacturer_name,
                hci_param.lmp_pal_subversion);
}

int LMP_CODE lmp_tx_features_req_ext(struct lmp_connection *conn, int page_num)
{
    if (page_num == 0)
        lmp_request(conn, "b04C008", 0xfe, FEATURES_REQ_EXT, 0, 1, hci_param.features);
    else
        lmp_request(conn, "b04C008", 0xfe, FEATURES_REQ_EXT, page_num, 1,
                    &hci_param.ext_features[(page_num << 3) - 8]);
    return 1;
}

int LMP_CODE lmp_tx_name_req(struct lmp_connection *conn)
{
    lmp_request(conn, "11", PDU(NAME_REQ), 0);
    return 1;
}

void LMP_CODE lmp_tx_connection_req(struct lmp_connection *conn)
{
    conn->core.state |= 1;
    lmp_request(conn, "1", PDU(HOST_CONNECTION_REQ));
}

void LMP_CODE lmp_tx_setup_complete(struct lmp_connection *conn)
{
    lmp_setup_complete(conn);
}

void LMP_CODE lmp_tx_max_slot_req(struct lmp_connection *conn, u8 slot)
{
    lmp_request(conn, "11", PDU(MAX_SLOT_REQ), slot);
}

void LMP_CODE lmp_tx_auto_rate(struct lmp_connection *conn)
{
    lmp_request(conn, "1", PDU(AUTO_RATE));
}

void LMP_CODE lmp_ecdh_publickey(struct lmp_connection *conn)
{
    struct lmp_simple_pair *simple = conn->simple;
    u8 half;

    simple->encapsulated_major_type = 1;
    simple->encapsulated_minor_type = 1;
    simple->encapsulated_playload_length = 48;
    link_conn_follow_ctrl_enable(conn->link);
    LOG_I("Start Public Key");
    get_random_number(local_private_key, 24);
    uECC_compute_public_key_api(local_private_key, conn->simple->local_public_key, uECC_secp192r1());
    endian_change(conn->simple->local_public_key, conn->simple->encapsulated_playload_length >> 1);
    half = conn->simple->encapsulated_playload_length >> 1;
    endian_change(&conn->simple->local_public_key[half], half);
    LOG_I("End Public Key");
    conn->simple->local_public_key_len = 0;
    link_conn_follow_ctrl_disable(conn->link);
}

void LMP_CODE lmp_authenticated_by_using_one_of_three_algorithms(struct lmp_connection *conn)
{
    struct lmp_simple_pair *simple = conn->simple;

    SDK_ASSERT(simple, 1698, "simple ");
    if (!(simple->local_Authentication_Req & 1) && !(simple->remote_Authentication_Req & 1)) {
        simple->simple_protocols = 0;
        simple->passkey_cnt = 0;
    } else {
        simple->simple_protocols = 0xff;
        if ((simple->local_IO_Capability < 2 && simple->remote_IO_Capability < 2) ||
            simple->local_IO_Capability == 3 || simple->remote_IO_Capability == 3) {
            simple->simple_protocols = 0;
            simple->passkey_cnt = 0;
        } else if (simple->local_IO_Capability == 2 || simple->remote_IO_Capability == 2) {
            simple->simple_protocols = 2;
        } else if (simple->remote_OOB_Data_Present && simple->local_OOB_Data_Present) {
            simple->simple_protocols = 1;
        }
    }
    LOG_I("simple_protocols = %d", simple->simple_protocols);
    SDK_ASSERT(simple->simple_protocols != 0xff, 1742, "simple->simple_protocols != SP_RESERVED ");
}

void LMP_CODE lmp_io_capability_init(struct lmp_connection *conn, u8 *data, u8 local)
{
    struct lmp_simple_pair *simple = conn->simple;

    simple->remote_IO_Capability = data[0];
    simple->remote_OOB_Data_Present = data[1];
    simple->remote_Authentication_Req = data[2];
    simple->remote_public_key_len = 0;
    simple->simple_protocols = 0;
    simple->passkey_cnt = 0;
    if (local)
        lmp_authenticated_by_using_one_of_three_algorithms(conn);
    hci_send_event(50, "A111", conn->core_s.remote_addr, conn->simple->remote_IO_Capability,
                   conn->simple->remote_OOB_Data_Present, conn->simple->remote_Authentication_Req);
}

void LMP_CODE lmp_rx_io_capability_req(struct lmp_connection *conn, u8 *data)
{
    conn->simple = lmp_malloc(208);
    SDK_ASSERT(conn->simple, 1779, "conn->simple ");
    conn->simple->state |= 4;
    lmp_io_capability_init(conn, data, 0);
    conn->core.stage = LMP_STAGE_AUTH_WITH_SIMPLE_PAIR_BY_REMOTE;
    conn->core.step = LMP_WAIT_IO_CAPABILITY_REPLY;
    hci_send_event(49, "A", conn->core_s.remote_addr);
}

void LMP_CODE lmp_rx_encapsulated_payload(struct lmp_connection *conn, u8 *data)
{
    u8 ret;

    LOG_I("rx_encap_payload\n");
    if (conn->simple == NULL)
        return;
    memcpy(&conn->simple->remote_public_key[conn->simple->remote_public_key_len], data, 16);
    conn->simple->remote_public_key_len += 16;
    lmp_response(conn, "11", PDU(ACCEPTED), ENCAPSULATED_PAYLOAD);
    if (conn->simple->remote_public_key_len != conn->simple->encapsulated_playload_length)
        return;
    LOG_I("Start DH Key");
    {
        u8 key[conn->simple->encapsulated_playload_length];
        u8 half;

        btctler_reverse_bytes(conn->simple->remote_public_key, key, conn->simple->encapsulated_playload_length >> 1);
        half = conn->simple->encapsulated_playload_length >> 1;
        btctler_reverse_bytes(&conn->simple->remote_public_key[half], key + half, half);
        uECC_shared_secret_api(key, local_private_key, conn->simple->local_ecdh_key, uECC_secp192r1(), &ret);
        if (ret == 0)
            SDK_ASSERT(0, 1841, "0 shared_secret() failed (2)");
        endian_change(conn->simple->local_ecdh_key, 24);
        LOG_I("End DH Key");
    }
}

void LMP_CODE lmp_rx_pairing_confirm(struct lmp_connection *conn, u8 *data)
{
    memcpy(conn->simple->remote_commitment_num, data, 16);
    conn->simple->state |= 0x10;
}

void LMP_CODE master_first_dhkey_check(struct lmp_connection *conn)
{
    u8 check[16];

    lmp_f3_function(conn, check);
    lmp_request(conn, "1C016", PDU(DHKEY_CHECK), check);
}

int LMP_CODE lmp_master_check_c(struct lmp_connection *conn)
{
    u8 z[1];
    u8 c[16];
    struct lmp_simple_pair *simple = conn->simple;
    u32 numeric;

    if (simple->simple_protocols == 2) {
        LOG_I("conn->simple->passkey_cnt = %d", simple->passkey_cnt);
        simple = conn->simple;
        z[0] = (simple->passkey & (1 << simple->passkey_cnt)) ? 0x81 : 0x80;
        simple->passkey_cnt++;
    } else {
        z[0] = 0;
    }
    f1_function_api(simple->remote_public_key, simple->local_public_key, simple->remote_random_num, z, c);
    if (memcmp(c, conn->simple->remote_commitment_num, 16) != 0) {
        lmp_response(conn, "111", PDU(NOT_ACCEPTED), SIMPLE_PAIRING_NUMBER, 5);
        return 0;
    }
    lmp_response(conn, "11", PDU(ACCEPTED), SIMPLE_PAIRING_NUMBER);
    simple = conn->simple;
    if (simple->simple_protocols == 2) {
        if (simple->passkey_cnt > 19) {
            simple->passkey_cnt = 0;
            LOG_I("master_first_dhkey_check");
            master_first_dhkey_check(conn);
            return 1;
        }
        z[0] = (simple->passkey & (1 << simple->passkey_cnt)) ? 0x81 : 0x80;
        make_rand_num(simple->local_random_num);
        f1_function_api(conn->simple->local_public_key, conn->simple->remote_public_key, conn->simple->local_random_num,
                        z, conn->simple->local_commitment_num);
        lmp_response(conn, "1C016", PDU(SIMPLE_PAIRING_CONFIRM), conn->simple->local_commitment_num);
        return 2;
    }
    numeric = g_function_api(simple->local_public_key, simple->remote_public_key, simple->local_random_num,
                             simple->remote_random_num);
    hci_send_event(51, "A4", conn->core_s.remote_addr, numeric);
    return 3;
}

void LMP_CODE lmp_rx_pairing_number(struct lmp_connection *conn, u8 *data)
{
    memcpy(conn->simple->remote_random_num, data, 16);
}

static void LMP_CODE lmp_dhkey_check_accept(struct lmp_connection *conn)
{
    u8 check[16];

    lmp_f3_function(conn, check);
    lmp_response(conn, "11", PDU(ACCEPTED), DHKEY_CHECK);
    lmp_response(conn, "1C016", PDU(DHKEY_CHECK), check);
}

static int LMP_CODE lmp_dhkey_check(struct lmp_connection *conn, u8 *data)
{
    u8 remote[16];
    u8 r[16];
    u8 io_cap[3];
    u8 check[16];
    struct lmp_simple_pair *simple;

    memset(r, 0, 16);
    memcpy(remote, data, 16);
    simple = conn->simple;
    io_cap[0] = simple->remote_IO_Capability;
    io_cap[1] = simple->remote_OOB_Data_Present;
    io_cap[2] = simple->remote_Authentication_Req;
    if (simple->simple_protocols == 2)
        memcpy(r, &simple->passkey, 4);
    f3_function_api(simple->local_ecdh_key, simple->remote_random_num, simple->local_random_num, r, io_cap,
                    conn->core_s.remote_addr, hci_param.addr, check);
    conn->core.enc_state |= 0x2000;
    return memcmp(remote, check, 16) == 0;
}

/* Static helpers the SDK inlined into the receive path (lmp.c ~1059-1097,
 * 1374-1390, 2022-2065) and lmp_encryption.c (~306-363). */
INLINE void lmp_acl_u_tx_enable(struct lmp_connection *conn, u8 en)
{
    if (conn->link)
        link_conn_set_acl_u_tx_enable(conn->link, en);
}

INLINE void lmp_rx_accepted_packet_type_table_req(struct lmp_connection *conn)
{
    link_conn_set_ptt(conn->link, 1);
    lmp_acl_u_tx_enable(conn, 1);
}

INLINE void lmp_rx_power_control_req(struct lmp_connection *conn, u8 *data)
{
    switch (data[0]) {
    case 0:
        if ((u8)bredr_pwr_set(conn->link, 1, -1) == 1)
            lmp_response(conn, "111", 0xfe, POWER_CONTROL_RES, 0x3f);
        else
            lmp_response(conn, "111", 0xfe, POWER_CONTROL_RES, 0x15);
        break;
    case 1:
        if ((u8)bredr_pwr_set(conn->link, 1, 1) == 2)
            lmp_response(conn, "111", 0xfe, POWER_CONTROL_RES, 0x2a);
        else
            lmp_response(conn, "111", 0xfe, POWER_CONTROL_RES, 0x15);
        break;
    case 2:
        bredr_pwr_set(conn->link, 0, bt_get_pwr_max_level());
        lmp_response(conn, "111", 0xfe, POWER_CONTROL_RES, 0x2a);
        break;
    }
}

INLINE void lmp_rx_test_activate(struct lmp_connection *conn)
{
    if (!lmp_dut_en && (config_btctler_mode & 1)) {
        lmp_response(conn, "111", PDU(NOT_ACCEPTED), TEST_ACTIVATE, 0x1e);
        lmp_tx_detch(conn, 0x13);
        return;
    }
    local_irq_disable();
    if (g_dut_hdl == NULL)
        g_dut_hdl = lmp_malloc(36);
    SDK_ASSERT(g_dut_hdl, 2034, "g_dut_hdl ");
    memset(g_dut_hdl, 0, 36);
    bredr_pwr_set(conn->link, 0, bt_get_pwr_max_level());
    lmp_response(conn, "11", PDU(ACCEPTED), TEST_ACTIVATE);
    local_irq_enable();
    hci_send_event(0xfa, "1", 1);
}

INLINE void lmp_rx_test_control(struct lmp_connection *conn, struct bredr_rx_bulk *rx)
{
    struct bredr_dut *dut;

    local_irq_disable();
    if (g_dut_hdl == NULL)
        g_dut_hdl = lmp_malloc(36);
    SDK_ASSERT(g_dut_hdl, 2052, "g_dut_hdl ");
    dut = g_dut_hdl;
    memset(dut, 0, 36);
    dut->scenario = rx->data[1] ^ 0x55;
    dut->hopping_mode = rx->data[2] ^ 0x55;
    dut->tx_fre = rx->data[3] ^ 0x55;
    dut->rx_fre = rx->data[4] ^ 0x55;
    dut->power_mode = rx->data[5] ^ 0x55;
    dut->poll_period = rx->data[6] ^ 0x55;
    dut->packet_type = rx->data[7] ^ 0x55;
    dut->data_len = (rx->data[9] << 8 | rx->data[8]) ^ 0x5555;
    dut->start = 0;
    lmp_response(conn, "D11", PDU(ACCEPTED), TEST_CONTROL);
    local_irq_enable();
}

static void lmp_rx_encryption_mode_req(struct lmp_connection *conn);
static void lmp_rx_encryption_key_size_req(struct lmp_connection *conn, u8 *data);
static void lmp_rx_encryption_key_size_mask_req(struct lmp_connection *conn);
static void lmp_rx_start_encryption_req(struct lmp_connection *conn, u8 *data);
void lmp_rx_not_accepted_in_rand(struct lmp_connection *conn);

INLINE void lmp_rx_accepted_ext(struct lmp_connection *conn, u8 op)
{
    switch (op) {
    case REMOVE_ESCO_LINK_REQ:
        LOG_I("ACC LMP_REMOVE_ESCO_LINK_REQ");
        break;
    case ESCO_LINK_REQ:
        LOG_I("ACC LMP_ESCO_LINK_REQ");
        break;
    case PACKET_TYPE_TABLE_REQ:
        LOG_I("LMP_PACKET_TYPE_TABLE_REQ");
        lmp_rx_accepted_packet_type_table_req(conn);
        break;
    }
}

INLINE void lmp_rx_not_accepted_ext(struct lmp_connection *conn, u8 *data)
{
    switch (data[1]) {
    case REMOVE_ESCO_LINK_REQ:
        LOG_I("LMP_REMOVE_ESCO_LINK_REQ");
        break;
    case ESCO_LINK_REQ:
        LOG_E("LMP_ESCO_LINK_REQ NOT_ACCEPT %x", data[2]);
        lmp_esco_link_removed(conn, data[2]);
        break;
    case PACKET_TYPE_TABLE_REQ:
        LOG_I("LMP_PACKET_TYPE_TABLE_REQ");
        lmp_acl_u_tx_enable(conn, 1);
        break;
    case IO_CAPABILITY_REQ:
        if (data[2])
            lmp_send_event_auth_complete(conn, data[2]);
        break;
    }
}

INLINE void lmp_escape4_command_process(struct lmp_connection *conn, u8 op, u8 *opcode2, u8 *data)
{
    switch (op) {
    case ACCEPTED_EXT:
        LOG_I("E4_LMP_ACCEPTED_EXT");
        *opcode2 = data[1];
        lmp_rx_accepted_ext(conn, data[1]);
        break;
    case NOT_ACCEPTED_EXT:
        *opcode2 = data[1];
        if (data[1] != CHANNEL_CLASSIFICATION)
            LOG_E("E4_LMP_NOT_ACCEPTED_EXT=%d", data[1]);
        lmp_rx_not_accepted_ext(conn, data);
        break;
    case FEATURES_REQ_EXT:
        LOG_I("LMP_FEATURES_REQ_EXT");
        lmp_rx_features_req_ext(conn, data);
        break;
    case FEATURES_RES_EXT:
        LOG_I("LMP_FEATURES_RES_EXT");
        lmp_rx_features_res_ext(conn, data);
        break;
    case PACKET_TYPE_TABLE_REQ:
        LOG_I("LMP_PACKET_TYPE_TABLE_REQ");
        lmp_rx_packet_type_table_req(conn, data);
        break;
    case CHANNEL_CLASSIFICATION_REQ:
        LOG_D("LMP_CHANNEL_CLASSIFICATION_REQ");
        lmp_rx_channel_classification_req(conn, data);
        break;
    case CHANNEL_CLASSIFICATION:
        LOG_D("LMP_CHANNEL_CLASSIFICATION");
        lmp_rx_channel_classification(conn, data);
        break;
    case KEYPRESS_NOTIFICATION:
        LOG_I("LMP_KEYPRESS_NOTIFICATION type = %d", data[0]);
        break;
    case ESCO_LINK_REQ:
        LOG_I("LMP_ESCO_LINK_REQ");
        lmp_rx_esco_link_req(conn, data);
        break;
    case REMOVE_ESCO_LINK_REQ:
        LOG_I("LMP_REMOVE_ESCO_LINK_REQ");
        break;
    case SNIFF_SUBRATING_REQ:
        LOG_I("LMP_SNIFF_SUBRATING_REQ");
        break;
    case POWER_CONTROL_REQ:
        LOG_I("LMP_POWER_CONTROL_REQ");
        lmp_rx_power_control_req(conn, data);
        break;
    case POWER_CONTROL_RES:
        LOG_I("LMP_POWER_CONTROL_REs %d", data[0]);
        if (data[0] == 0x2a)
            link_conn_rx_remote_max_power_res(conn->link, 0);
        break;
    case IO_CAPABILITY_REQ:
        LOG_I("LMP_IO_CAPABILITY_REQ");
        lmp_rx_io_capability_req(conn, data);
        break;
    case IO_CAPABILITY_RES:
        LOG_I("LMP_IO_CAPABILITY_RES");
        break;
    case PAUSE_ENCRYPTION_REQ:
        LOG_I("LMP_PAUSE_ENCRYPTION_REQ");
        lmp_rx_pause_encryption_req(conn);
        break;
    case RESUME_ENCRYPTION_REQ:
        LOG_I("LMP_RESUME_ENCRYPTIONG_REQ");
        lmp_rx_resume_encryption_req(conn);
        break;
    default:
        lmp_response(conn, "b05", 0xfe, NOT_ACCEPTED_EXT, ESCAPE_4, op, 0x1a);
        LOG_I("unknow_escape4_opcode: %d", op);
        break;
    }
}

INLINE void lmp_rx_accepted(struct lmp_connection *conn, u8 *data)
{
    switch (data[0] << 1) {
    case PDU(SNIFF_REQ):
        LOG_I("LMP_SNIFF_REQ");
        lmp_rx_accepted_sniff_req(conn);
        break;
    case PDU(UNSNIFF_REQ):
        LOG_I("LMP_UNSNIFF_REQ");
        lmp_rx_accepted_unsniff_req(conn);
        break;
    case PDU(SCO_LINK_REQ):
        LOG_I("LMP_SCO_LINK_REQ");
        break;
    case PDU(REMOVE_SCO_LINK_REQ):
        LOG_I("LMP_REMOVE_SCO_LINK_REQ");
        break;
    case PDU(MAX_SLOT_REQ):
        LOG_I("LMP_MAX_SLOT_REQ");
        lmp_rx_accepted_max_slot_req(conn);
        break;
    case PDU(HOST_CONNECTION_REQ):
        LOG_I("LMP_HOST_CONNECTION_REQ");
        lmp_rx_accepted_host_connection_req(conn);
        break;
    case PDU(ENCAPSULATED_HEADER):
        LOG_I("LMP_ENCAPSULATED_HEADER");
        break;
    case PDU(ENCAPSULATED_PAYLOAD):
        LOG_I("LMP_ENCAPSULATED_PAYLOAD");
        break;
    case PDU(DHKEY_CHECK):
        LOG_I("LMP_DHKEY_CHECK");
        break;
    case PDU(SIMPLE_PAIRING_NUMBER):
        LOG_I("LMP_SIMPLE_PAIRING_NUMBER");
        break;
    case PDU(IN_RAND):
        break;
    case PDU(ENCRYPTION_MODE_REQ):
        LOG_I("LMP_ENCRYPTION_MODE_REQ");
        break;
    case PDU(ENCRYPTION_KEY_SIZE_REQ):
        LOG_I("LMP_ENCRYPTION_KEY_SIZE_REQ");
        break;
    case PDU(START_ENCRYPTION_REQ):
        LOG_I("LMP_START_ENCRYPTION_REQ");
        lmp_rx_accepted_start_encryption_req(conn);
        break;
    case PDU(STOP_ENCRYPTION_REQ):
        LOG_I("LMP_STOP_ENCRYPTION_REQ");
        lmp_rx_accepted_stop_encryption_req(conn);
        break;
    case PDU(QUALITY_OF_SERVICE_REQ):
        LOG_I("LMP_QUALITY_OF_SERVICE_REQ");
        break;
    case PDU(SWITCH_REQ):
        LOG_I("LMP_SWITCH_REQ");
        lmp_rx_accepted_switch_req(conn);
        break;
    default:
        LOG_E("unknow_opcpde: %d, %x", data[0], data[1]);
        break;
    }
}

INLINE void lmp_rx_not_accepted(struct lmp_connection *conn, u8 *data)
{
    switch (data[0] << 1) {
    case PDU(SNIFF_REQ):
        LOG_I("not accept LMP_SNIFF_REQ \n");
        lmp_rx_not_accepted_sniff_req(conn);
        break;
    case PDU(UNSNIFF_REQ):
        LOG_I("LMP_UNSNIFF_REQ");
        lmp_rx_not_accepted_unsniff_req(conn);
        break;
    case PDU(MAX_SLOT_REQ):
        LOG_I("LMP_MAX_SLOT_REQ");
        break;
    case PDU(SCO_LINK_REQ):
        LOG_I("LMP_SCO_LINK_REQ");
        break;
    case PDU(SET_AFH):
        LOG_I("LMP_SET_AFH");
        break;
    case PDU(HOST_CONNECTION_REQ):
        LOG_I("LMP_HOST_CONNECTION_REQ reason:0x%x", data[1]);
        lmp_rx_not_accepted_host_connection_req(conn, data);
        break;
    case PDU(AU_RAND):
        LOG_I("LMP_AU_RAND reason:0x%x", data[1]);
        lmp_rx_not_accept_au_rand(conn, data);
        break;
    case PDU(IN_RAND):
        LOG_I("LMP_IN_RAND");
        lmp_rx_not_accepted_in_rand(conn);
        break;
    case PDU(ENCAPSULATED_HEADER):
        LOG_I("LMP_ENCAPSULATED_HEADER");
        break;
    case PDU(ENCAPSULATED_PAYLOAD):
        LOG_I("LMP_ENCAPSULATED_PAYLOAD");
        break;
    case PDU(DHKEY_CHECK):
        LOG_I("LMP_DHKEY_CHECK");
        lmp_tx_detch(conn, 0x10);
        break;
    case PDU(ENCRYPTION_MODE_REQ):
        LOG_I("LMP_ENCRYPTION_MODE_REQ");
        break;
    case PDU(ENCRYPTION_KEY_SIZE_REQ):
        LOG_I("LMP_ENCRYPTION_KEY_SIZE_REQ");
        break;
    case PDU(START_ENCRYPTION_REQ):
        LOG_I("LMP_START_ENCRYPTION_REQ");
        break;
    case PDU(SWITCH_REQ):
        LOG_I("LMP_SWITCH_REQ switch_req: %x", data[1]);
        lmp_rx_not_accepted_switch_req(conn);
        break;
    default:
        LOG_E("unknow_opcpde: %d, %x", data[0], data[1]);
        break;
    }
}

INLINE void lmp_command_process(struct lmp_connection *conn, u8 opcode, u8 *opcode1, struct bredr_rx_bulk *rx)
{
    u8 *data = &rx->data[1];

    switch (opcode) {
    case PDU(ACCEPTED):
        LOG_I("LMP_ACCEPTED");
        *opcode1 = data[0] << 1;
        lmp_rx_accepted(conn, data);
        break;
    case PDU(NOT_ACCEPTED):
        LOG_I("LMP_NOT_ACCEPTED");
        *opcode1 = data[0] << 1;
        lmp_rx_not_accepted(conn, data);
        break;
    case PDU(FEATURES_REQ):
        LOG_I("LMP_FEATURES_REQ");
        lmp_rx_feature_req(conn, data);
        break;
    case PDU(FEATURES_RES):
        LOG_I("LMP_FEATURES_RES");
        lmp_rx_features_res(conn, data);
        break;
    case PDU(VERSION_REQ):
        LOG_I("LMP_VERSION_REQ");
        lmp_rx_version_req(conn);
        break;
    case PDU(VERSION_RES):
        LOG_I("LMP_VERSION_RES");
        lmp_rx_version_res(conn, data);
        break;
    case PDU(NAME_REQ):
        LOG_I("LMP_NAME_REQ");
        lmp_rx_name_req(conn, data);
        break;
    case PDU(NAME_RES):
        LOG_I("LMP_NAME_RES");
        lmp_rx_name_res(conn, data);
        break;
    case PDU(MAX_SLOT):
        LOG_I("LMP_MAX_SLOT");
        lmp_rx_max_slot(conn, data);
        break;
    case PDU(MAX_SLOT_REQ):
        LOG_I("LMP_MAX_SLOT_REQ");
        lmp_rx_max_slot_req(conn, data);
        if (set_qos_poll_interval)
            lmp_tx_qos_req(conn);
        break;
    case PDU(AUTO_RATE):
        LOG_I("LMP_AUTO_RATE");
        break;
    case PDU(INCR_POWER_REQ):
        LOG_I("LMP_INCR_POWER_REQ");
        lmp_rx_incr_power(conn);
        break;
    case PDU(DECR_POWER_REQ):
        LOG_I("LMP_DECR_POWER_REQ");
        lmp_rx_decr_power(conn);
        break;
    case PDU(MAX_POWER):
        LOG_I("LMP_MAX_POWER");
        lmp_rx_max_power(conn);
        break;
    case PDU(MIN_POWER):
        LOG_I("LMP_MIN_POWER");
        lmp_rx_min_power(conn);
        break;
    case PDU(SUPERVISION_TIMEOUT):
        LOG_I("LMP_SUPERVISION_TIMEOUT");
        lmp_rx_supervision_timeout(conn, data);
        break;
    case PDU(SET_AFH):
        LOG_I("LMP_SET_AFH");
        break;
    case PDU(CLKOFFSET_REQ):
        LOG_I("LMP_CLKOFFSET_REQ");
        lmp_rx_clkoffset_req(conn);
        break;
    case PDU(CLKOFFSET_RES):
        LOG_I("LMP_CLKOFFSET_RES");
        lmp_rx_clkoffset_res(conn, data);
        break;
    case PDU(DETACH):
        LOG_I("LMP_DETACH: %d\n", data[0]);
        lmp_rx_detch(conn, data);
        break;
    case PDU(QUALITY_OF_SERVICE_REQ):
        LOG_I("LMP_QUALITY_OF_SERVICE_REQ");
        lmp_rx_quality_of_service_req(conn, data);
        break;
    case PDU(PREFERRED_RATE):
        LOG_E("WARNING:LMP_PREFERRED_RATE!!! %X ", data[0]);
        break;
    case PDU(TEMP_KEY):
        LOG_I("LMP_TEMP_KEY");
        lmp_rx_temp_key(conn);
        break;
    case PDU(TIMING_ACCURACY_REQ):
        LOG_I("LMP_TIMING_ACCURACY_REQ");
        lmp_rx_timing_accuracy_req(conn);
        break;
    case PDU(TIMING_ACCURACY_RES):
        LOG_I("LMP_TIMING_ACCURACY_RES");
        break;
    case PDU(SNIFF_REQ):
        LOG_I("LMP_SNIFF_REQ");
        if (!lmp_rx_sniff_standby(conn)) {
            LOG_I("LMP_NOT_ACCEPTED1");
            lmp_response(conn, "111", PDU(NOT_ACCEPTED), SNIFF_REQ, 0x24);
        } else if (!lmp_esco_sniff_check(conn)) {
            LOG_I("LMP_NOT_ACCEPTED2");
            lmp_response(conn, "111", PDU(NOT_ACCEPTED), SNIFF_REQ, 0x24);
        } else {
            lmp_rx_sniff_req(conn, data);
        }
        break;
    case PDU(UNSNIFF_REQ):
        LOG_I("LMP_UNSNIFF_REQ");
        lmp_rx_unsniff_req(conn);
        break;
    case PDU(SCO_LINK_REQ):
        LOG_I("LMP_SCO_LINK_REQ");
        lmp_rx_sco_link_req(conn, data);
        break;
    case PDU(REMOVE_SCO_LINK_REQ):
        LOG_I("LMP_REMOVE_SCO_LINK_REQ");
        break;
    case PDU(TEST_ACTIVATE):
        LOG_I("LMP_TEST_ACTIVATE");
        if (!CONFIG_TEST_DUT_ONLY_BOX_CODE)
            lmp_rx_test_activate(conn);
        break;
    case PDU(TEST_CONTROL):
        LOG_I("LMP_TEST_CONTROL");
        lmp_rx_test_control(conn, rx);
        break;
    case PDU(HOST_CONNECTION_REQ):
        LOG_I("LMP_HOST_CONNECTION_REQ");
        lmp_rx_host_connection_req(conn);
        break;
    case PDU(SETUP_COMPLETE):
        LOG_I("LMP_SETUP_COMPLET");
        lmp_rx_setup_complete(conn);
        break;
    case PDU(IN_RAND):
        LOG_I("LMP_IN_RAND");
        lmp_rx_in_rand(conn, data);
        break;
    case PDU(COMB_KEY):
        LOG_I("LMP_COMB_KEY");
        lmp_rx_comb_key(conn, data);
        break;
    case PDU(AU_RAND):
        LOG_I("LMP_AU_RAND");
        lmp_rx_au_rand(conn, data);
        break;
    case PDU(SRES):
        LOG_I("LMP_SRES");
        lmp_rx_sres(conn, data);
        break;
    case PDU(ENCRYPTION_MODE_REQ):
        LOG_I("LMP_ENCRYPTION_MODE_REQ");
        lmp_rx_encryption_mode_req(conn);
        break;
    case PDU(ENCRYPTION_KEY_SIZE_REQ):
        LOG_I("LMP_ENCRYPTION_KEY_SIZE_REQ 0x%x", data[0]);
        lmp_rx_encryption_key_size_req(conn, data);
        break;
    case PDU(ENCRYPTION_KEY_SIZE_MASK_REQ):
        LOG_I("LMP_ENCRYPTION_KEY_SIZE_MASK_REQ");
        lmp_rx_encryption_key_size_mask_req(conn);
        break;
    case PDU(START_ENCRYPTION_REQ):
        LOG_I("LMP_START_ENCRYPTION_REQ");
        lmp_rx_start_encryption_req(conn, data);
        break;
    case PDU(STOP_ENCRYPTION_REQ):
        LOG_I("LMP_STOP_ENCRYPTION_REQ");
        lmp_rx_stop_encryption_req(conn);
        break;
    case PDU(SLOT_OFFSET):
        LOG_I("LMP_SLOT_OFFSET");
        lmp_rx_slot_offset(conn, data);
        break;
    case PDU(SWITCH_REQ):
        LOG_I("LMP_SWITCH_REQ");
        if (!lmp_rx_role_switch_standby(conn))
            lmp_response(conn, "111", PDU(NOT_ACCEPTED), SWITCH_REQ, 0x24);
        else
            lmp_rx_switch_req(conn, data);
        break;
    case PDU(ENCAPSULATED_HEADER):
        LOG_I("LMP_ENCAPSULATED_HEADER");
        break;
    case PDU(ENCAPSULATED_PAYLOAD):
        LOG_I("LMP_ENCAPSULATED_PAYLOAD");
        lmp_rx_encapsulated_payload(conn, data);
        break;
    case PDU(SIMPLE_PAIRING_CONFIRM):
        LOG_I("LMP_SIMPLE_PAIRING_CONFIRM");
        lmp_rx_pairing_confirm(conn, data);
        break;
    case PDU(SIMPLE_PAIRING_NUMBER):
        LOG_I("LMP_SIMPLE_PAIRING_NUMBER");
        lmp_rx_pairing_number(conn, data);
        break;
    case PDU(DHKEY_CHECK):
        LOG_I("LMP_DHKEY_CHECK");
        break;
    default:
        lmp_response(conn, "111", PDU(NOT_ACCEPTED), opcode >> 1, 0x1a);
        LOG_E("unknow_opcode: %d, %x", data[0], data[1]);
        break;
    }
}

void LMP_CODE lmp_user0_command_process(struct lmp_connection *conn, u8 opcode, u8 *opcode1, u8 *data)
{
    if (opcode != 0) {
        LOG_E("user0 unknow_opcode: %d, %x", data[0], data[1]);
        return;
    }
    if (data[0] == 4 && data[1] == 0 && data[2] == hci_param.addr_sum)
        lmp_request(conn, "1111", 1, 4, 0, 0);
    if (data[0] == 0 && data[1] == 50 && data[2] == 21)
        lmp_response(conn, "1111111111", 0, 1, 50, 29, 0, 0, 0, 0, 0, 0);
}

void LMP_CODE lmp_acl_c_handler(struct lmp_connection *conn, struct bredr_rx_bulk *rx, u8 *opcode0, u8 *opcode1,
                                u8 *opcode2)
{
    u8 *data = rx->data;

    *opcode0 = data[0] & ~1;
    conn->tran_id = data[0] & 1;
    switch (*opcode0) {
    case 0xf8:
    case 0xfa:
        break;
    case 0xfc:
        *opcode1 = rx->data[1];
        if (lmp_cmd_is_match_size(*opcode0, data, rx->len))
            lmp_escape3_command_process(conn, *opcode1, opcode2, &rx->data[2]);
        break;
    case 0xfe:
        *opcode1 = rx->data[1];
        if (lmp_cmd_is_match_size(*opcode0, data, rx->len))
            lmp_escape4_command_process(conn, *opcode1, opcode2, &rx->data[2]);
        break;
    default:
        if (lmp_cmd_is_match_size(*opcode0, data, rx->len))
            lmp_command_process(conn, *opcode0, opcode1, rx);
        break;
    }
}

void LMP_CODE lmp_timeout_irq_handler(void)
{
}

__attribute__((weak)) int LMP_CODE wifibox_bt_freq_finish_post(void)
{
    return 0;
}

void LMP_CODE lmp_authentication_requested(struct lmp_connection *conn)
{
}

void bredr_esco_standard_tws_link_close(struct link_connection *link);
static void lmp_rx_handler(struct lmp_connection *conn);
static void page_completed(u8 *addr, int err);

/* lmp.c ~517: forget a detached connection. */
INLINE void lmp_conn_release(struct lmp_connection *conn)
{
    local_irq_disable();
    conn->core.state = 0;
    conn->core_s.handle = 0;
    conn->link = NULL;
    conn->lmp_detch_start = 0;
    local_irq_enable();
}

static void LMP_CODE lmp_detach_check(void)
{
    struct lmp_connection *conn;
    struct link_connection *link;
    u32 state;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->core_s.handle == 0 || !(conn->core.state & 8))
            continue;
        lmp_rx_handler(conn);
        LOG_I("do_detach : %d\n", conn->err_code);
        local_irq_disable();
        if (conn->sbc_open)
            os_sem_post(&conn->sbc_sem);
        local_irq_enable();
        if (conn->simple) {
            if (conn->simple->ecdh)
                lmp_free(conn->simple->ecdh);
            lmp_free(conn->simple);
            conn->simple = NULL;
            LOG_I("free_simple\n");
        }
        lmp_free_encrypt(conn);
        lmp_channel_classification_close(conn);
        if (conn->enc_timer) {
            sys_timer_del(conn->enc_timer);
            conn->enc_timer = 0;
        }
        if (conn->timer) {
            sys_timer_del(conn->timer);
            conn->timer = 0;
        }
        if (conn->wf_det_time) {
            sys_timer_del(conn->wf_det_time);
            conn->wf_det_time = 0;
        }
        if (conn->remote_name) {
            lmp_free(conn->remote_name);
            conn->remote_name = NULL;
            LOG_I("free_remote_name\n");
        }
        if (conn->link && conn->link->esco) {
            if (CONFIG_BTCTLER_TWS_ENABLE && conn->err_code == 8)
                tws_post_remove_esco_link_super_timeout(conn);
            if (ESCO_FORWARD_ENABLE && conn->esco_conn->tws_esco_forward)
                bredr_esco_standard_tws_link_close(conn->link);
            else
                bredr_esco_link_close(conn->link);
        }
        lmp_esco_link_removed(conn, 0x13);
        lmp_sniff_detach(conn);
        if (conn->change_rx_bulk_timer != 0xff) {
            slot_timer_put(conn->change_rx_bulk_timer);
            conn->change_rx_bulk_timer = 0xff;
        }
        if (CONFIG_BTCTLER_TWS_ENABLE)
            tws_phone_link_detach(conn, conn->err_code);
        local_irq_disable();
        lmp_private_abandon_sbc_data(conn, &conn->rx_acl_u, 0);
        local_irq_enable();
        if (!list_empty(&conn->rx_acl_u)) {
            os_time_dly(2);
            if (conn->err_code != 8)
                continue;
        }
        state = conn->core.state;
        if (state & 0x200000) {
            conn->lmp_detch_start = 1;
            conn->core.state = 0;
        } else if (state & 2) {
            conn->core.state = state & ~2;
            conn->lmp_detch_start = 1;
            if (!config_btctler_hci_standard)
                hci_send_event(5, "1H11A", 0, conn->core_s.handle, conn->err_code, 1, conn->core_s.remote_addr);
            else
                hci_send_event(5, "1H1", 0, conn->core_s.handle, conn->err_code);
            os_time_dly(2);
        } else if (state & 1) {
            conn->lmp_detch_start = 1;
            conn->core.state = state & ~1;
            lmp_send_event_connection_complete(conn, conn->err_code, 0, 1);
        }
        link = conn->link;
        if (link == NULL || !list_empty(&link->rx_head))
            continue;
        if (!list_empty(&link->tx_acl_c) || !list_empty(&link->tx_acl_u)) {
            os_time_dly(40);
            link = conn->link;
        }
        conn->link = NULL;
        link_conn_close(link);
        lmp_role_switch_detach(conn);
        LOG_I("lmp_del_suss\n");
        lmp_conn_release(conn);
        wifibox_bt_freq_finish_post();
        if (config_btctler_mode & 2)
            lmp_scan_enable(3);
        if (config_btctler_hci_standard)
            lmp_scan_enable(hci_param.scan_enable);
        if (CONFIG_BTCTLER_TWS_ENABLE)
            tws_phone_link_del(conn->err_code);
        hci_send_event(0xfe, "11", 0, 0);
    }
}

static void LMP_CODE lmp_connection_timeout(void *priv)
{
    struct lmp_connection *conn = priv;

    sys_timer_del(conn->timer);
    conn->timer = 0;
    if ((conn->core.state & 2) && ((conn->core.enc_state & 4) || !hci_param.need_enc)) {
        LOG_I("lmp_rm_conn_timeout\n");
        return;
    }
    if (conn->link == NULL)
        return;
    if (g_dut_hdl) {
        LOG_I("dut rm_conn_timeout\n");
        return;
    }
    lmp_tx_detch(conn, 8);
    LOG_I("lmp_super_timeout\n");
    lmp_detach_check();
}

int LMP_CODE lmp_connection_start(struct link_connection *link, u8 role)
{
    struct lmp_connection *conn = lmp_create_conn();
    struct lmp_connection *p;

    SDK_ASSERT(conn != NULL, 2928, "conn != NULL %s", "lmp_connection_start");
    if (conn == NULL)
        return -12;
    if (!CONFIG_BTCTLER_TWS_ENABLE) {
        for (p = lmp_acl_link; p < lmp_acl_link + 1; p++) {
            if (p == conn || p->link == NULL)
                continue;
            LOG_I("----active bd\n");
            link_conn_set_max_rx_bulk_persent(p->link, 60);
            os_time_dly(2);
        }
    }
    conn->link = link;
    conn->core.role = role;
    conn->core.link_policy = hci_param.default_link_policy;
    conn->core.state = 0;
    conn->core_s.remote_sr = link->core.remote_sr;
    conn->core_s.remote_class_dev = link->core.remote_class_dev;
    memcpy(conn->core_s.remote_addr, link->core.remote_addr, 6);
    if (role == 0) {
        if (CONFIG_LMP_NAME_REQ_ENABLE && page_parm.remote_name_req) {
            page_parm.remote_name_req = 0;
            conn->core.state = 0x200000;
            lmp_role_machine_handler_register(conn, lmp_name_req_machine);
            lmp_name_req_init(conn);
        } else {
            lmp_role_machine_handler_register(conn, lmp_master_machine);
            lmp_master_init(conn);
        }
    } else {
        lmp_role_machine_handler_register(conn, lmp_slave_machine);
        lmp_slave_init(conn);
    }
    conn->timer = sys_timer_add(conn, lmp_connection_timeout, 5000);
    conn->core.state |= 1;
    if (config_btctler_hci_standard)
        lmp_scan_enable(hci_param.scan_enable);
    return 0;
}

int LMP_CODE __bt_updata_lmp_connection_resume(struct link_connection *link, u8 role)
{
    struct lmp_connection *conn = lmp_create_conn();

    SDK_ASSERT(conn != NULL, 3021, "conn != NULL %s", "__bt_updata_lmp_connection_resume");
    if (conn == NULL)
        return -12;
    LOG_I("uboot_lmp_connection_start\n");
    conn->link = link;
    conn->core.role = role;
    if (role) {
        lmp_role_machine_handler_register(conn, lmp_slave_machine);
        lmp_slave_init(conn);
    }
    conn->core.state = 1;
    return 0;
}

int LMP_CODE lmp_conn_idle(struct lmp_connection *conn)
{
    struct link_connection *link;
    struct list_head *node;
    struct bredr_rx_bulk *rx;

    if (conn == NULL || (link = conn->link) == NULL)
        return 1;
    if (conn->busy)
        return 0;
    for (node = link->rx_head.next; node != &link->rx_head; node = node->next) {
        rx = (struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry));
        switch (rx->llid) {
        case 3:
            return 0;
        case 2:
            if ((rx->data[3] << 8 | rx->data[2]) != conn->sbc_channel)
                return 0;
            break;
        }
    }
    for (node = conn->rx_acl_u.next; node != &conn->rx_acl_u; node = node->next) {
        rx = (struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry));
        if (!rx->delete && (rx->data[3] << 8 | rx->data[2]) != conn->sbc_channel)
            return 0;
    }
    return 1;
}

#define RX_OF(node) ((struct bredr_rx_bulk *)((u8 *)(node) - offsetof(struct bredr_rx_bulk, entry)))
#define TX_OF(node) ((struct bredr_tx_bulk *)((u8 *)(node) - offsetof(struct bredr_tx_bulk, entry)))

void LMP_CODE lmp_tx_suspend(void)
{
    struct lmp_connection *conn;
    struct list_head *node, *next;

    local_irq_disable();
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->link == NULL)
            continue;
        for (node = conn->link->tx_acl_u.next; node != &conn->link->tx_acl_u; node = next) {
            next = node->next;
            bredr_tx_bulk_free(TX_OF(node));
        }
        for (node = conn->link->tx_acl_c.next; node != &conn->link->tx_acl_c; node = next) {
            next = node->next;
            bredr_tx_bulk_free(TX_OF(node));
        }
    }
    local_irq_enable();
}

int LMP_CODE lmp_conn_suspend(u8 suspend)
{
    struct lmp_connection *conn;
    struct link_connection *link;
    struct list_head *node, *next;
    int ret = 0;
    u8 bits;

    if (!(config_btctler_modules & 1))
        return 0;
    link_conn_rx_bulk_avaliable(NULL, NULL);
    local_irq_disable();
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->link == NULL)
            continue;
        conn->core.suspend = suspend;
        tws_phone_link_state_change(conn, NULL, NULL);
        if (conn->sbc_channel) {
            if (CONFIG_BTCTLER_TWS_ENABLE)
                tws_phone_link_abandon_sbc_data(conn, 0);
            lmp_private_abandon_sbc_data(conn, &conn->link->rx_head, 0);
            for (node = conn->rx_acl_u.next; node != &conn->rx_acl_u; node = next) {
                next = node->next;
                bits = *((u8 *)RX_OF(node) + 2);
                if ((bits & 8) || (bits & 0xc0) == 0x40) {
                    __list_del_entry(node);
                    bredr_rx_bulk_free(RX_OF(node));
                }
            }
        }
        link = conn->link;
        if (list_empty(&link->rx_head) | list_empty(&conn->rx_acl_u))
            ret = 1;
        if (conn->simple || conn->encrypt || conn->rs_process) {
            if (!ret)
                continue;
        } else {
            ret = 1;
        }
        bredr_link_event(link, 1, 5);
    }
    local_irq_enable();
    return 0;
}

void LMP_CODE lmp_conn_resume(void)
{
    struct lmp_connection *conn;

    local_irq_disable();
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->link == NULL)
            continue;
        conn->core.suspend = 0;
        tws_phone_link_state_change(conn, NULL, NULL);
        link_conn_rx_bulk_avaliable(conn->link, NULL);
    }
    local_irq_enable();
}

__attribute__((weak)) int LMP_CODE acl_u_packet_analyse(u8 *packet, u16 len)
{
    return 0;
}

__attribute__((weak)) void LMP_CODE a2dp_rx_notice_to_decode(void)
{
}

static void LMP_CODE lmp_send_acl_u_packet_to_host(struct lmp_connection *conn)
{
    struct list_head *node, *next;
    struct bredr_rx_bulk *rx;
    u8 *bits, b;

    local_irq_disable();
    for (node = conn->rx_acl_u.next; node != &conn->rx_acl_u; node = next) {
        rx = RX_OF(node);
        next = node->next;
        bits = (u8 *)rx + 2;
        b = *bits;
        if (b & 8) {
            __list_del_entry(node);
            link_conn_rx_bulk_avaliable(conn->link, rx);
            continue;
        }
        if (b & 0x20)
            continue;
        *bits = b | 0x20;
        local_irq_enable();
        if (hci_send_acl_data(rx->payload_head, rx->len + 4)) {
            local_irq_disable();
            *bits &= ~0x20;
            local_irq_enable();
            return;
        }
        if (config_btctler_hci_standard) {
            local_irq_disable();
            *bits |= 8;
            local_irq_enable();
        }
        return;
    }
    local_irq_enable();
}

int LMP_CODE tws_check_sbc_data(struct lmp_connection *conn, u8 *data, u16 len)
{
    if ((data[3] << 8 | data[2]) == conn->sbc_channel && filter_out_sbc_data_en())
        return 1;
    return 0;
}

static void LMP_CODE lmp_rx_handler(struct lmp_connection *conn)
{
    int hci_cmd = 0;
    struct bredr_rx_bulk *rx, *pkt, *s;
    u16 len, cid;
    int need;
    u8 *bits;

    if (conn == NULL || conn->link == NULL)
        return;
    conn->busy = 1;
    while ((rx = bredr_rx_bulk_pop(&conn->link->rx_head)) != NULL) {
        if (conn->timer)
            sys_timer_modify(conn->timer, 10000);
        if (rx->llid == 3) {
            lmp_role_machine(conn, rx, &hci_cmd);
            if (CONFIG_BTCTLER_TWS_ENABLE)
                tws_phone_link_state_change(conn, rx, &hci_cmd);
            link_conn_rx_bulk_avaliable(conn->link, rx);
            continue;
        }
        if (lmp_dut_en && !lmp_dut_phone_en)
            break;                      /* sic: the popped bulk is not freed */
        if (g_lmp_update_start_flag) {
            if (lmp_update_rx_handler)
                lmp_update_rx_handler(conn->core_s.handle, rx);
            link_conn_rx_bulk_avaliable(conn->link, rx);
            continue;
        }
        s = conn->acl_s_bulk;
        if (rx->llid == 2) {
            if (s) {
                link_conn_rx_bulk_avaliable(conn->link, s);
                LOG_D("bad_packet_0: %d\n", conn->acl_s_bulk->len);
                conn->acl_s_bulk = NULL;
            }
            len = rx->len;
            need = (rx->data[1] << 8 | rx->data[0]) + 4;
            if (len < need) {
                LOG_V("acl_u_s: %d, %d\n", need, len);
                conn->acl_s_bulk = rx;
                continue;
            }
            pkt = rx;
        } else {
            if (s == NULL) {
                link_conn_rx_bulk_avaliable(conn->link, rx);
                continue;
            }
            need = (s->data[1] << 8 | s->data[0]) + 4;
            if (s->len + rx->len > need) {
                link_conn_rx_bulk_avaliable(conn->link, s);
                conn->acl_s_bulk = NULL;
                LOG_D("bad_packet_1\n");
                link_conn_rx_bulk_avaliable(conn->link, rx);
                continue;
            }
            memcpy(&s->data[s->len], rx->data, rx->len);
            s->len += rx->len;
            link_conn_rx_bulk_avaliable(conn->link, rx);
            len = s->len;
            if (len < need)
                continue;
            conn->acl_s_bulk = NULL;
            pkt = s;
        }
        pkt->payload_head[0] = conn->core_s.handle;
        bits = (u8 *)pkt + 2;
        pkt->payload_head[1] = (conn->core_s.handle >> 8) | ((*bits & 7) == 1 ? 0x10 : 0x20);
        pkt->payload_head[2] = len;
        pkt->payload_head[3] = len >> 8;
        cid = pkt->data[3] << 8 | pkt->data[2];
        if (acl_u_packet_analyse(pkt->data, len) < 0) {
            link_conn_rx_bulk_avaliable(conn->link, pkt);
            continue;
        }
        local_irq_disable();
        if (cid == conn->sbc_channel) {
            if (conn->clear_a2dp_packet_num) {
                if ((pkt->data[6] << 8 | pkt->data[7]) - conn->clear_a2dp_packet_num < 0) {
                    local_irq_enable();
                    link_conn_rx_bulk_avaliable(conn->link, pkt);
                    continue;
                }
                conn->clear_a2dp_packet_num = 0;
            }
            if (conn->sbc_open) {
                *bits = (*bits & ~0xf0) | 0x70;     /* hold, send2host, sbc_flag 1 */
                a2dp_rx_notice_to_decode();
                os_sem_post(&conn->sbc_sem);
            }
        }
        list_add_tail(&pkt->entry, &conn->rx_acl_u);
        local_irq_enable();
        if (CONFIG_BTCTLER_TWS_ENABLE) {
            if (cid == conn->sbc_channel) {
                if (conn->sbc_open)
                    sbc_sync_packet_len(pkt->len);
            } else {
                tws_conn_role_switch_lock_ms(300);
            }
        }
        lmp_send_acl_u_packet_to_host(conn);
    }
    conn->busy = 0;
}

int LMP_IRQ lmp_cmd_handler_in_irq(void *lc, u8 *data, u8 llid)
{
    struct lmp_connection *conn = lmp_conn_for_link(lc);
    u8 op;

    if (conn == NULL)
        return 0;
    if (llid != 3 && !(sniff_support_reset_anchor_point | config_btctler_hci_standard))
        lmp_tx_unsniff_req(conn);
    if (llid != 3)
        return 0;
    op = data[0];
    switch (op & ~1) {
    case PDU(SET_AFH):
        bredr_link_set_afh(lc, data[3] << 16 | data[4] << 24 | data[2] << 8 | data[1], data[5], data + 6);
        return 1;
    case PDU(UNSNIFF_REQ):
        conn->tran_id = op & 1;
        lmp_rx_unsniff_req(conn);
        return 1;
    case PDU(ACCEPTED):
        if (data[1] == UNSNIFF_REQ) {
            conn->tran_id = op & 1;
            lmp_rx_accepted_unsniff_req(conn);
            return 1;
        }
        break;
    }
    return 0;
}

void LMP_CODE check_1to2_conn_exit_sniff(void *__conn)
{
    struct lmp_connection *conn = __conn;
    u8 timer;

    if (conn == NULL || conn->esco_conn == NULL)
        return;
    putchar('w');
    timer = conn->esco_conn->esco_conn_timer;
    if (timer == 0xff) {
        timer = slot_timer_get();
        conn->esco_conn->esco_conn_timer = timer;
    }
    if (get_1to2_conn_exit_sniff()) {
        slot_timer_set(timer, check_1to2_conn_exit_sniff, conn, 10);
        putchar('@');
        return;
    }
    bredr_link_event(conn->link, 1, 9);
}

void LMP_CODE check_exit_esco_rx_bulk_change(void *__conn)
{
    struct lmp_connection *conn = __conn;

    local_irq_disable();
    if (conn == NULL || conn->link == NULL || !conn->change_rx_bulk_mode) {
        local_irq_enable();
        return;
    }
    putchar('u');
    if (bredr_bulk_change_rx_bulk(0)) {
        conn->change_rx_bulk_mode = 0;
        if (conn->change_rx_bulk_timer != 0xff) {
            slot_timer_put(conn->change_rx_bulk_timer);
            conn->change_rx_bulk_timer = 0xff;
        }
        local_irq_enable();
        return;
    }
    if (conn->change_rx_bulk_timer == 0xff) {
        conn->change_rx_bulk_timer = slot_timer_get();
        if (conn->change_rx_bulk_timer == 0xff) {
            local_irq_enable();
            return;
        }
    }
    slot_timer_set(conn->change_rx_bulk_timer, check_exit_esco_rx_bulk_change, conn, 10);
    local_irq_enable();
}

void LMP_CODE check_enter_esco_rx_bulk_change(void *__conn)
{
    struct lmp_connection *conn = __conn;

    local_irq_disable();
    if (conn == NULL || conn->esco_conn == NULL) {
        local_irq_enable();
        return;
    }
    putchar('k');
    if (bredr_bulk_change_rx_bulk(1)) {
        conn->change_rx_bulk_mode = 1;
        if (conn->change_rx_bulk_timer != 0xff) {
            slot_timer_put(conn->change_rx_bulk_timer);
            conn->change_rx_bulk_timer = 0xff;
        }
        bredr_link_event(conn->link, 1, 9);
        local_irq_enable();
        return;
    }
    if (conn->change_rx_bulk_timer == 0xff) {
        conn->change_rx_bulk_timer = slot_timer_get();
        if (conn->change_rx_bulk_timer == 0xff) {
            local_irq_enable();
            return;
        }
    }
    slot_timer_set(conn->change_rx_bulk_timer, check_enter_esco_rx_bulk_change, conn, 4);
    local_irq_enable();
}

u8 LMP_CODE wait_enter_esco_rx_bulk_change(struct lmp_connection *conn)
{
    if (conn == NULL || conn->esco_conn == NULL || conn->link == NULL)
        return 0;
    if (conn->change_rx_bulk_mode)
        return 1;
    check_enter_esco_rx_bulk_change(conn);
    return 0;
}

void LMP_CODE lmp_event_handler(int *event)
{
    struct lmp_connection *conn;
    int type = event[1], arg;

    switch (type) {
    case 1:
        lmp_connection_start((struct link_connection *)event[0], event[2]);
        return;
    case 2:
        return;
    }
    conn = lmp_conn_for_link((void *)event[0]);
    if (conn == NULL)
        return;
    switch (type) {
    case 14:
        LOG_I("lmp_event_detach: %d\n", event[2]);
        arg = event[2];
        conn->err_code = arg;
        conn->core.state |= 8;
        lmp_request(conn, "11", PDU(DETACH), (u8)arg);
        LOG_I("req_exit\n");
        break;
    case 3:
        conn->err_code = 8;
        conn->core.state |= 8;
        LOG_I("lmp_super_timeout\n");
        break;
    case 4:
        lmp_role_switch_machine(conn, 0);
        break;
    case 6:
        lmp_send_max_slot(conn, event[2]);
        break;
    case 7:
        lmp_connection_ctl_slot(conn, event[2]);
        break;
    case 9:
        if (conn->esco_conn == NULL)
            break;
        if (CONFIG_ESCO_MUX_RX_BULK_ENABLE && !wait_enter_esco_rx_bulk_change(conn))
            break;
        if (conn->link && conn->esco_conn)
            link_conn_set_esco_lt_addr(conn->link, conn->esco_conn->pdu.lt_addr);
        if (CONFIG_BTCTLER_TWS_ENABLE) {
            if (!tws_check_can_setup_esco()) {
                lmp_hci_exit_sniff_mode(conn);
                tws_master_notice_slave_step_fun(1, 0, 0);
                break;
            }
        } else if (link_conn_num_more_than_one()) {
            if (get_1to2_conn_exit_sniff()) {
                lmp_hci_exit_sniff_mode(conn);
                check_1to2_conn_exit_sniff(conn);
                break;
            }
            if (conn->esco_conn->esco_conn_timer != 0xff) {
                puts("slot_timer_put esco_conn_timer\n");
                slot_timer_put(conn->esco_conn->esco_conn_timer);
                conn->esco_conn->esco_conn_timer = 0xff;
            }
        }
        lmp_event_esco_link_setup(conn);
        break;
    case 10:
        if (CONFIG_BTCTLER_TWS_ENABLE) {
            bredr_esco_tws_link_open(conn->link, 0);
            tws_lmp_esco_link_setup(1);
        }
        break;
    case 11:
        hci_send_event(19, "1H2", 1, conn->core_s.handle, 1);
        break;
    case 5:
        lmp_rx_handler(conn);
        break;
    case 12:
        lmp_tx_power_req(conn, event[2]);
        break;
    case 15:
        lmp_tx_set_afh(conn, 0);
        break;
    }
    lmp_detach_check();
}

void LMP_CODE bredr_close_all_scan(void)
{
    if (CONFIG_BREDR_INQUIRY)
        link_inquiry_disable();
    link_inquiry_scan_disable();
    link_page_disable(0xfe);
    link_page_scan_disable();
}

u8 LMP_CODE check_lmp_detch_over(void)
{
    struct lmp_connection *conn;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->core_s.handle && conn->lmp_detch_start)
            return 0;
    }
    return 1;
}

void LMP_CODE lmp_disconnect_all_standard_connect(void)
{
    struct lmp_connection *conn;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++)
        lmp_tx_detch(conn, 0x13);
}

u8 LMP_CODE lmp_standard_connect_check(void)
{
    struct lmp_connection *conn;
    u8 ret = 0;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->link)
            ret = 0x80;
    }
    if (ret == 0)
        ret = get_bredr_link_state() & 1;
    return ret;
}

void LMP_CODE lmp_hci_cmd_handler(int *cmd)
{
    u8 addr[6];
    struct lmp_connection *conn;

    switch (cmd[0]) {
    case 0x0c03:
        lmp_disconnect_all_standard_connect();
        break;
    case 0x0c1a:
        LOG_I("HCI_WRITE_SCAN_ENABLE : %d", cmd[1]);
        lmp_scan_enable(cmd[1]);
        break;
    case 0x0402:
        if (CONFIG_BREDR_INQUIRY) {
            link_inquiry_disable();
            hci_send_event(1, "1", 0);
        }
        break;
    case 0x0401:
        if (CONFIG_BREDR_INQUIRY)
            link_inquiry_enable((u8 *)&cmd[1], cmd[2] * 1280, cmd[3], 0);
        break;
    case 0x0cfe:
        LOG_I(" HCI_WRITE_CONNECTION_CANCEL \n");
        memcpy(addr, &cmd[1], 6);
        conn = lmp_conn_for_address(addr);
        if (conn)
            lmp_tx_detch(conn, 0x13);
        break;
    case 0x0cfd:
        LOG_I("HCI_CNACLE_CREATE_CONNECTION\n");
        if (CONFIG_BTCTLER_TWS_ENABLE)
            link_page_disable(0xfd);
        else
            link_page_try_start_disable(0xfd);
        break;
    case 0x0405:
        LOG_I("HCI_CREATE_CONNECTION\n");
        if (CONFIG_BTCTLER_TWS_ENABLE) {
            link_page_enable((u8 *)&cmd[1], cmd[5], cmd[4], page_completed);
        } else if (CONFIG_BTCTLER_FAST_CONNECT_ENABLE) {
            if (cmd[6] == 0xaa) {
                hci_param.emitter_or_receiver = 1;
                lmp_set_features_req_step((u8 *)&cmd[1]);
            }
        } else {
            link_page_try_start_enable(0, (u8 *)&cmd[1], cmd[5], cmd[4]);
            if (cmd[6] == 0xaa)
                hci_param.emitter_or_receiver = 1;
        }
        break;
    case 0x0419:
        if (CONFIG_LMP_NAME_REQ_ENABLE) {
            LOG_I("HCI_REMOTE_NAME_REQUEST");
            memcpy(addr, &cmd[1], 6);
            lmp_hci_remote_name_request(addr, cmd[3], cmd[4]);
        }
        break;
    case 0x0c3f:
        LOG_I("HCI_SET_AFH_HOST_CHANNEL_CLASSIFICATION ");
        if (config_bredr_afh_user)
            lmp_master_tx_set_afh_user((u8 *)&cmd[1]);
        else
            lmp_tx_channel_classification((u8 *)&cmd[1]);
        break;
    }
}

void LMP_CODE lmp_hci_cmd_addr_to_conn(u8 *addr, int *cmd)
{
    struct lmp_connection *conn = lmp_conn_for_address(addr);

    SDK_ASSERT(conn, 3950, "conn ");
    lmp_role_machine(conn, NULL, cmd);
}

void LMP_CODE lmp_hci_cmd_handler_to_conn(u8 *data, int *cmd)
{
    struct lmp_connection *conn = lmp_conn_for_handle(btctler_little_endian_read_16(data, 0) & 0xfff);

    SDK_ASSERT(conn, 3958, "conn ");
    lmp_role_machine(conn, NULL, cmd);
}

void LMP_CODE lmp_hci_cmd_to_conn_handler(int *cmd)
{
    struct lmp_connection *conn = (struct lmp_connection *)cmd[0];
    int *hci_cmd;
    u8 esco;

    if (conn == NULL)
        return;
    hci_cmd = &cmd[1];
    switch (*hci_cmd) {
    case 0x080b:
        if (conn->core.role != cmd[2])
            conn->core.role_switch = 1;
        break;
    case 0x0c35:
        if (!list_empty(&conn->rx_acl_u))
            lmp_send_acl_u_packet_to_host(conn);
        break;
    case 0x0406:
        lmp_tx_disconnect(conn, cmd);
        break;
    case 0x0cfc:
        lmp_send_test_key(conn, cmd[2]);
        break;
    case 0x0cfb:
        lmp_send_user_info(conn, cmd[2]);
        break;
    case 0x0803:
        LOG_I("HCI_SNIFF_MODE=%d,%d,%d,%d\n", cmd[2], cmd[3], cmd[4], cmd[5]);
        lmp_hci_sniff_mode(conn, cmd[2], cmd[3], cmd[4], cmd[5]);
        break;
    case 0x0804:
        LOG_I("HCI_EXIT_SNIFF_MODE\n");
        lmp_hci_exit_sniff_mode(conn);
        break;
    case 0x0428:
        if (!CONFIG_LMP_MASTER_ESCO_ENABLE)
            break;
        esco = 0;
        if (hci_param.features[3] & 0x80)
            esco = conn->core_s.remote_features[3] >> 7;
        conn->core.step = LMP_TX_SCO_LINK_REQ | esco;
        conn->core.stage = LMP_STAGE_SCO_CONN_BY_REMOTE + esco;
        break;
    }
    lmp_role_machine(conn, NULL, hci_cmd);
    if (CONFIG_BTCTLER_TWS_ENABLE)
        tws_phone_link_state_change(conn, NULL, hci_cmd);
    lmp_detach_check();
}

int LMP_CODE lmp_private_get_rx_buffer_total_size(void *_conn)
{
    struct lmp_connection *conn = _conn;
    int size = 0;

    if (!(config_btctler_modules & 1))
        return 0;
    local_irq_disable();
    if (conn && conn->link)
        size = link_conn_rx_bulk_total_size(conn->link);
    local_irq_enable();
    return size;
}

int LMP_CODE lmp_private_get_rx_buffer_remain_size(void *_conn)
{
    struct lmp_connection *conn = _conn;
    int size = 0;

    if (!(config_btctler_modules & 1))
        return 0;
    local_irq_disable();
    if (conn && conn->link) {
        size = link_conn_rx_bulk_remain_size(conn->link, conn->sbc_packet_size);
        if (!link_conn_rx_empty(conn->link)) {
            if (++conn->rxbuf_full > 9) {
                conn->rxbuf_full = 0;
                local_irq_enable();
                return 0;
            }
        } else if (size > 2048) {
            conn->rxbuf_full = 0;
        }
    }
    local_irq_enable();
    return size;
}

void LMP_CODE lmp_private_set_max_rx_buf_persent(u8 *addr, int percent)
{
    struct lmp_connection *conn;

    local_irq_disable();
    if (addr) {
        conn = lmp_conn_for_address(addr);
        if (conn && conn->link)
            link_conn_set_max_rx_bulk_persent(conn->link, percent);
    } else {
        for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
            if (conn->link)
                link_conn_set_max_rx_bulk_persent(conn->link, percent);
        }
    }
    local_irq_enable();
}

struct lmp_connection *LMP_CODE get_updata_conn(void)
{
    struct lmp_connection *conn;

    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->core_s.handle)
            return conn;
    }
    return NULL;
}

void LMP_CODE lmp_update_rx_handler_register(void (*handler)(int, struct bredr_rx_bulk *))
{
    lmp_update_rx_handler = handler;
}

void LMP_CODE lmp_role_machine_handler_register(struct lmp_connection *conn, void *handle)
{
    conn->role_machine_handler = handle;
}

void LMP_CODE lmp_role_machine(struct lmp_connection *conn, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    int ret;

    if (conn == NULL || conn->link == NULL || (conn->core.state & 8) || conn->role_machine_handler == NULL)
        return;
    ret = conn->role_machine_handler(conn, rx, hci_cmd);
    while (ret) {
        if (ret == 2)
            rx = NULL;
        ret = conn->role_machine_handler(conn, rx, hci_cmd);
    }
}

u8 LMP_CODE lmp_get_conn_num(void)
{
    struct lmp_connection *conn;
    u8 num = 0;

    local_irq_disable();
    for (conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (conn->core_s.handle)
            num += conn->link != NULL;
    }
    local_irq_enable();
    return num;
}

void LMP_CODE lmp_close_all(void)
{
    link_page_try_start_disable(-14);
    link_inquiry_scan_try_start_disable();
    link_page_scan_try_start_disable();
}

void LMP_CODE bredr_set_dut_enble(u8 en, u8 phone)
{
    lmp_dut_en = en;
    lmp_dut_phone_en = phone;
    bt_dut_reset_mdm();
}

u8 LMP_CODE bredr_get_dut_enable(void)
{
    return lmp_dut_en;
}

void LMP_CODE lmp_reset(void)
{
    SDK_ASSERT(lmp_get_conn_num() == 0, 4186, "lmp_get_conn_num() == 0 ");
    lmp_close_all();
    link_task_close_all();
    bredr_link_close();
    link_bulk_init();
    bredr_link_init();
    lmp_init();
}

/* ---- the SDK's included parts ---------------------------------------------- */

#include "lmp_name_req.c"
#include "lmp_master.c"
#include "lmp_slave.c"
#include "lmp_inquiry_page.c"
#include "lmp_encryption.c"
#include "lmp_role_switch.c"
#include "lmp_sniff.c"
#include "lmp_esco.c"
#include "lmp_escape3.c"
#include "lmp_a2dp.c"
