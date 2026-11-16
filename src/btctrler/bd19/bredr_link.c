/* BD19 bredr_link.c.o occurrence 1: the Classic link layer (classic_new).
 * A later SDK revision than BR23's bredr_link.c: frame register helpers moved
 * to bredr_frame.c, TWS BLE/fix-txrx/disturb-scan paths are gone, and this
 * member adds the RX packet/eSCO handlers, QoS polling, eSCO hardware slots,
 * 1-to-2 eSCO scheduling and role-switch page procedures. Preserve the SDK's
 * ordering, reloads, volatile register pairs and quirks. */
#include "btctrler/glue.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RF_CODE  __attribute__((section(".classic_rf_code")))
#define IRQ_CODE __attribute__((section(".classic_irq_code")))
#define RF_DATA  __attribute__((section(".classic_rf_data")))
#define RF_BSS   __attribute__((section(".classic_rf_bss")))
#define RF_CONST __attribute__((section(".classic_rf_const")))
#define IRQ_CONST __attribute__((section(".classic_irq_const")))
#define BL_FILE "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/bd19/baseband/classic_new/bredr_link.c"
#define REG(a) (*(volatile u32 *)(uintptr_t)(a))
#define memcpy __builtin_memcpy
#define memset __builtin_memset

void log_print(int level, const char *tag, const char *format, ...);
int printf(const char *format, ...);
int putchar(int c);
void p33_soft_reset(void);
int os_taskq_post_type(const char *name, int type, int argc, int *argv);
int os_sem_pend(OS_SEM *sem, int timeout);
extern const struct link_task_ops tws_link_task_ops;
extern BTCTRLER_CONFIG u8 log_tag_const_i_BL, log_tag_const_d_BL, log_tag_const_e_BL;
extern BTCTRLER_CONFIG int config_btctler_eir_version_info_len, config_btctler_hci_standard, config_btctler_mode;
extern BTCTRLER_CONFIG int CONFIG_BTCTLER_TWS_ENABLE, CONFIG_BTCTLER_QOS_ENABLE, CONFIG_BREDR_INQUIRY;
extern BTCTRLER_CONFIG int config_btctler_modules, CONFIG_TEST_DUT_CODE, CONFIG_TEST_DUT_ONLY_BOX_CODE;
extern BTCTRLER_CONFIG int config_force_bt_pwr_tab_using_normal_level;
extern BTCTRLER_CONFIG int CONFIG_INQUIRY_SCAN_POWER, CONFIG_PAGE_SCAN_POWER, CONFIG_INQUIRY_POWER, CONFIG_PAGE_POWER;

#define INFO(...) do { if (log_tag_const_i_BL) log_print(2, NULL, __VA_ARGS__); } while (0)
#define DEBUG(...) do { if (log_tag_const_d_BL) log_print(1, NULL, __VA_ARGS__); } while (0)
#define ERROR(...) do { if (log_tag_const_e_BL) log_print(4, NULL, __VA_ARGS__); } while (0)

/* The SDK evaluates the condition once, then branches on config_asser. */
#define SDK_ASSERT(p, line, text) do { _Bool ok_ = (p); if (config_asser) { if (!ok_) { \
    printf("file:%s, line:%d", BL_FILE, line); printf("ASSERT-FAILD: " text " "); cpu_assert_debug(); \
    } } else if (!ok_) cpu_reset(); } while (0)

#define FOR_EACH_LINK(lc) \
    for ((lc) = (struct link_connection *)bredr_link_v.link_conn_head.next; \
         &(lc)->entry != &bredr_link_v.link_conn_head; (lc) = (struct link_connection *)(lc)->entry.next)

/* ---- state ---------------------------------------------------------------- */

static u32 link_rx_bulk[2][2][200] __attribute__((used, section(".bredr_rxtx_bulk"), aligned(4)));
struct bredr_link_var bredr_link_v RF_BSS;
static struct link_page *g_page RF_DATA;
static struct link_page_scan *g_page_scan RF_DATA;
static struct link_inquiry *inquiry RF_DATA;
static struct link_inquiry_scan *inquiry_scan RF_DATA;
static struct link_page *g_quick_page RF_DATA;
static struct link_page_scan *g_quick_page_scan RF_DATA;

/* GIAC (0x9e8b33) and its access-code parity. */
static const u8 local_lap[6] RF_CONST = { 0x33, 0x8b, 0x9e, 0, 0, 0 };
static const u8 local_bch[5] RF_CONST = { 0xe2, 0x3a, 0x1a, 0x33, 0x02 };
static const u16 ab_train_table[3] RF_CONST = { 1, 128, 256 };
/* { 2M/1M payload, max payload, packet type, slots }. */
static const u16 packet_2M_table[3][4] RF_CONST = {
    { 108, 54, 4, 1 }, { 421, 367, 10, 3 }, { 679, 679, 14, 5 },
};
static const u16 packet_1M_table_tws[3][4] RF_CONST = {
    { 27, 27, 4, 1 }, { 183, 183, 11, 3 }, { 339, 339, 15, 5 },
};
static const u16 packet_1M_table[6][4] RF_CONST = {
    { 17, 17, 3, 1 }, { 27, 27, 4, 1 }, { 121, 121, 10, 3 },
    { 183, 183, 11, 3 }, { 224, 224, 14, 5 }, { 339, 339, 15, 5 },
};
const u16 pwr_tb[4] RF_CONST = { 10, 100, 500, 1000 };
const u8 dec_fast_tb[8] RF_CONST = { 0xfa, 0xc8, 0x96, 0x64, 0x50, 0x3c, 0x32, 0x02 };
u16 esco_interrupt_a2dp_shedult_cn RF_DATA;
u32 last_local_offset RF_DATA;
u32 last_bitoff RF_DATA;
u32 last_offset RF_DATA;
u32 last_soft_bitoff RF_DATA;
StaticQueue_t esco_sem RF_BSS;
static const u16 seq_num[4] RF_CONST = { 0, 3, 12, 15 };
static const u8 msbc_mute_data[58] RF_CONST = {
    0xad, 0x00, 0x00, 0xc5, 0x00, 0x00, 0x00, 0x00, 0x77, 0x6d, 0xb6, 0xdd, 0xdb, 0x6d, 0xb7, 0x76,
    0xdb, 0x6d, 0xdd, 0xb6, 0xdb, 0x77, 0x6d, 0xb6, 0xdd, 0xdb, 0x6d, 0xb7, 0x76, 0xdb, 0x6d, 0xdd,
    0xb6, 0xdb, 0x77, 0x6d, 0xb6, 0xdd, 0xdb, 0x6d, 0xb7, 0x76, 0xdb, 0x6d, 0xdd, 0xb6, 0xdb, 0x77,
    0x6d, 0xb6, 0xdd, 0xdb, 0x6d, 0xb7, 0x76, 0xdb, 0x6c, 0x00,
};

static int inquiry_scan_resume(struct link_task *task) RF_CODE;
static int inquiry_scan_suspend(struct link_task *task) RF_CODE;
static int page_scan_resume(struct link_task *task) RF_CODE;
static int page_scan_suspend(struct link_task *task) RF_CODE;
static int inquiry_resume(struct link_task *task) RF_CODE;
static int inquiry_suspend(struct link_task *task) RF_CODE;
static int page_resume(struct link_task *task) RF_CODE;
static int page_suspend(struct link_task *task) RF_CODE;

static const struct link_task_ops inquiry_scan_task_ops RF_CONST = { inquiry_scan_resume, inquiry_scan_suspend };
static const struct link_task_ops page_scan_task_ops RF_CONST = { page_scan_resume, page_scan_suspend };
static const struct link_task_ops inquiry_task_ops RF_CONST = { inquiry_resume, inquiry_suspend };
static const struct link_task_ops page_task_ops RF_CONST = { page_resume, page_suspend };
static const struct link_task_ops conn_task_ops IRQ_CONST = { link_conn_task_resume, link_conn_task_suspend };

static inline void cpu_reset(void) { p33_soft_reset(); }

static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

static inline void __list_del_entry(struct list_head *entry)
{
    struct list_head *prev = entry->prev, *next = entry->next;

    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry)
{
    __list_del_entry(entry);
    entry->next = entry;
    entry->prev = entry;
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
    struct list_head *prev = head->prev;

    head->prev = new;
    new->next = head;
    new->prev = prev;
    prev->next = new;
}

/* ---- basics --------------------------------------------------------------- */

void RF_CODE schdule_debug(u8 x) { }
void RF_CODE DEBUG_SINGAL_TWS_CONNECTION(u8 x) { }

u8 *RF_CODE link_conn_get_rx_buffer(struct link_connection *lc, int index)
{
    return (u8 *)link_rx_bulk[lc->index][index];
}

__attribute__((weak)) u8 RF_CODE get_esco_packet_dump(void)
{
    return 120;
}

u8 RF_CODE get_bredr_link_state(void)
{
    return bredr_link_v.bredr_link_state;
}

u8 RF_CODE get_link_bredr_id(struct link_connection *lc)
{
    return lc->index;
}

void RF_CODE bredr_link_close(void)
{
}

void RF_CODE bredr_link_init(void)
{
    bredr_link_v.glt_addr = 0;
    bredr_link_v.remote_device_state = 0;
    g_page = NULL;
    g_page_scan = NULL;
    inquiry = NULL;
    inquiry_scan = NULL;
    g_quick_page = NULL;
    g_quick_page_scan = NULL;
    INIT_LIST_HEAD(&bredr_link_v.link_conn_head);
}

void RF_CODE bredr_pll_comp_ext_reset(void)
{
    struct link_connection *lc;

    if (!bd_base.bredr_running)
        return;
    FOR_EACH_LINK(lc) {
        if (lc->frame)
            bredr_pll_comp_reset(lc->frame);
    }
}

u8 RF_CODE bredr_link_check_used(void)
{
    return inquiry_scan || inquiry || g_page_scan || g_page;
}

void RF_CODE bredr_link_reset_power(void)
{
    struct link_connection *lc;

    if (!bd_base.bredr_running)
        return;
    if (inquiry_scan && inquiry_scan->frame)
        bredr_normal_pwr_set(inquiry_scan->frame, 15);
    if (g_page_scan && g_page_scan->frame)
        bredr_normal_pwr_set(g_page_scan->frame, 15);
    if (g_page && g_page->frame)
        bredr_normal_pwr_set(g_page->frame, 15);
    if (inquiry && inquiry->frame)
        bredr_normal_pwr_set(inquiry->frame, 15);
    FOR_EACH_LINK(lc) {
        if (lc->frame)
            bredr_pwr_set(lc, 0, 15);
    }
}

/* Transmit power: a forced table level, else the fixed level, else the
 * requested step; capped at the maximum. */
void RF_CODE bredr_normal_pwr_set(struct ctrl_frame *frame, u8 pwr_step)
{
    u8 fix = rf.bredr_fix_pwr;
    u8 level;

    if (config_force_bt_pwr_tab_using_normal_level)
        level = config_force_bt_pwr_tab_using_normal_level;
    else
        level = fix == 0xff ? pwr_step : fix;
    if (level > bt_get_pwr_max_level())
        level = bt_get_pwr_max_level();
    frame->ana_index &= ~0xff;
    frame->ana_index |= level;
    bta_set_amp_sepa(level);
}

/* A link's power step: set (sel 0, or the forced table level) or moved by
 * inc, clamped to 0..max and below bredr_pwr_max; a fixed level wins and a
 * TWS link always uses the maximum.  Returns 1 at the floor, 2 at a ceiling
 * or when fixed, else 0. */
int RF_CODE bredr_pwr_set(struct link_connection *lc, char sel, char inc)
{
    s8 *pwr = &lc->core.pwr_set;
    struct ctrl_frame *frame;
    int ret = 0;
    s8 cap;

    if (sel) {
        *pwr = *pwr + inc;
    } else {
        *pwr = inc;
        if (config_force_bt_pwr_tab_using_normal_level)
            *pwr = config_force_bt_pwr_tab_using_normal_level;
    }
    if (*pwr < 0) {
        *pwr = 0;
        ret = 1;
    } else if (*pwr > bt_get_pwr_max_level()) {
        *pwr = bt_get_pwr_max_level();
        ret = 2;
    }
    cap = rf.bredr_pwr_max & 15;
    if (*pwr >= cap) {
        *pwr = cap;
        ret = 2;
    }
    if (rf.bredr_fix_pwr != 0xff) {
        *pwr = rf.bredr_fix_pwr;
        ret = 2;
    }
    if (lc->tws_link)
        *pwr = bt_get_pwr_max_level();
    frame = lc->frame;
    frame->ana_index &= ~0xff;
    frame->ana_index |= *pwr;
    frame->tws_ana_index &= ~0xff;
    lc->frame->tws_ana_index |= bt_get_pwr_max_level();
    bta_set_amp_sepa(*pwr);
    return ret;
}

void RF_CODE bredr_link_check_frame_used(void)
{
    struct link_connection *lc;

    FOR_EACH_LINK(lc) {
    }
}

void RF_CODE __read_fhs_packet(struct bredr_remote_device *remote, u8 *fhs)
{
    remote->access_addr[0] = fhs[0];
    remote->access_addr[1] = fhs[1];
    remote->access_addr[2] = fhs[2];
    remote->access_addr[3] = fhs[3];
    remote->access_addr[4] = fhs[4] & 3;
    remote->addr[0] = fhs[4] >> 2;
    remote->addr[0] |= fhs[5] << 6;
    remote->addr[1] = fhs[5] >> 2;
    remote->addr[1] |= fhs[6] << 6;
    remote->addr[2] = fhs[6] >> 2;
    remote->addr[2] |= fhs[7] << 6;
    remote->eir = (fhs[7] >> 2) & 1;
    remote->sr = (fhs[7] >> 4) & 3;
    remote->addr[3] = fhs[8];
    remote->addr[4] = fhs[9];
    remote->addr[5] = fhs[10];
    remote->class_dev = fhs[11];
    remote->class_dev |= fhs[12] << 8;
    remote->class_dev |= fhs[13] << 16;
    remote->lt_addr = fhs[14] & 7;
    remote->clkn = fhs[14] >> 3;
    remote->clkn |= fhs[15] << 5;
    remote->clkn |= fhs[16] << 13;
    remote->clkn |= (fhs[17] & 31) << 21;
    remote->clkn <<= 2;
}

int puts(const char *s);
void put_buf(const u8 *buf, int len);
u16 CRC16(const void *ptr, u32 len);
extern u32 __VERSION_BEGIN;
/* Both idle detectors return i8 in the BD19 objects; the generated header
 * declares bool. Bind the SDK symbols with their byte ABI. */
u8 link_task_idle_task_enable_detect_ir(void) __asm__("link_task_idle_task_enable_detect");
u8 link_idle_task_enable_detect_ir(void) __asm__("link_idle_task_enable_detect");
#define link_task_idle_task_enable_detect link_task_idle_task_enable_detect_ir
#define link_idle_task_enable_detect link_idle_task_enable_detect_ir
static void inquiry_scan_disable(void) RF_CODE;
static void __page_scan_disable(struct link_page_scan *page_scan) RF_CODE;
static inline u8 bredr_hw_read_lt_addr(struct ctrl_frame *frame);

static void __write_fhs_packet(u8 *mac_addr, int class_dev, const struct bredr_local_eir *eir, u8 lt_addr, u8 *fhs) RF_CODE;

int RF_CODE __inquiry_scan_enable(int clk_offset, u8 *addr, struct bredr_local_eir *eir)
{
    struct ctrl_frame *frame;
    struct link_inquiry_scan *scan;

    local_irq_disable();
    if (inquiry_scan) {
        local_irq_enable();
        return 0;
    }
    puts("link_inquiry_scan_enable\n");
    put_buf(addr, 6);
    frame = bredr_bd_get_frame();
    if (frame == NULL) {
        local_irq_enable();
        return -12;
    }
    inquiry_scan = bt_malloc(304);
    if (inquiry_scan == NULL) {
        bredr_bd_put_frame(frame);
        local_irq_enable();
        return -12;
    }
    __write_reg_bdaddr(frame, local_lap);
    __write_reg_bch(frame, local_bch);
    __write_fhs_packet(addr, gbredr_local_dev.dev_class, eir, 0, inquiry_scan->fhs);
    scan = inquiry_scan;
    if (eir->enable)
        memcpy(scan->eir_data, eir->data, eir->len);
    scan->substate = 0;
    scan->clk_offset = clk_offset;
    scan->frame = frame;
    scan->state = 1;
    bredr_frame_inquiry_scan_init(scan, eir);
    scan = inquiry_scan;
    scan->task.type = 2;
    scan->task.run_slot = 26;
    scan->task.run_num = 1;
    scan->task.priority = 1;
    scan->task.ops = &inquiry_scan_task_ops;
    scan->task.sta_attribute = 1;
    bredr_link_v.bredr_link_state |= 8;
    link_task_add(&scan->task);
    local_irq_enable();
    link_idle_task_enable_detect();
    return 0;
}

static void __write_fhs_packet(u8 *mac_addr, int class_dev, const struct bredr_local_eir *eir, u8 lt_addr, u8 *fhs)
{
    u8 acc[5];

    __get_access_addr(acc, mac_addr);
    fhs[0] = acc[0];
    fhs[1] = acc[1];
    fhs[2] = acc[2];
    fhs[3] = acc[3];
    fhs[4] = mac_addr[0] << 2 | (acc[4] & 3);
    fhs[5] = mac_addr[1] << 2 | mac_addr[0] >> 6;
    fhs[6] = mac_addr[2] << 2 | mac_addr[1] >> 6;
    fhs[7] = mac_addr[2] >> 6 | eir->enable << 2 | eir->sr << 4 | 0x80;
    fhs[8] = mac_addr[3];
    fhs[9] = mac_addr[4];
    fhs[10] = mac_addr[5];
    fhs[11] = class_dev;
    fhs[12] = class_dev >> 8;
    fhs[13] = class_dev >> 16;
    fhs[14] = lt_addr & 7;
    fhs[15] = 0;
    fhs[16] = 0;
    fhs[17] = 0;
}

/* The idle task may run when every connected link is in a sniff transition
 * (or there is none) and the task list agrees. */
u8 RF_CODE link_idle_task_enable_detect(void)
{
    struct link_connection *lc;
    u8 enable = 1;

    local_irq_disable();
    FOR_EACH_LINK(lc) {
        if (lc->frame && !lc->sniff_sta)
            enable = 0;
    }
    if (enable == 1)
        enable = link_task_idle_task_enable_detect();
    local_irq_enable();
    if (enable)
        link_task_idle_enable();
    return enable;
}

__attribute__((weak)) u16 RF_CODE eir_fill_version_info(u8 *data, u16 allow_len)
{
    int len = config_btctler_eir_version_info_len;
    int room, n;
    u16 crc;

    if (len == 0)
        return 0;
    room = allow_len - 4;
    n = room > len ? len : room;
    memcpy(data + 4, (u8 *)&__VERSION_BEGIN + 1, n & 0xffff);
    data[0] = n + 3;
    data[1] = 0xfe;
    crc = CRC16(data + 4, n & 0xffff);
    data[2] = crc;
    data[3] = crc >> 8;
    printf("allow_len:%x ver_actual_len:%x\n", allow_len, len + 4);
    printf("version_str:%x %s \n", &__VERSION_BEGIN, (u8 *)&__VERSION_BEGIN + 1);
    return n + 4;
}

__attribute__((weak)) u8 RF_CODE add_data_2_eir(u8 *data)
{
    return 0;
}

__attribute__((weak)) u8 RF_CODE update_eir_data_hook(u8 *data, u8 len)
{
    return len;
}

/* Build the EIR: local name, a 16-byte private version block, the version
 * info, then the application's data; the hook may change the length. */
void RF_CODE update_eir_data(u8 *eir_data)
{
    u32 name_len = strlen((const char *)gbredr_local_dev.local_name);
    u16 len = name_len + 1;
    u8 *p, *data;

    SDK_ASSERT(len <= 32, 595, "name_len <= sizeof(gbredr_local_dev.local_name)");
    if (!gbredr_local_dev.eir.enable)
        return;
    memset(&gbredr_local_dev.eir, 0, 8);
    gbredr_local_dev.eir.enable = 1;
    gbredr_local_dev.eir.sr = 1;
    gbredr_local_dev.eir.data = eir_data;
    eir_data[0] = name_len + 2;
    eir_data[1] = 9;
    p = eir_data + 2;
    memcpy(p, gbredr_local_dev.local_name, len);
    p += len;
    *p++ = 17;
    *p++ = 0xff;
    memcpy(p, gbredr_local_dev.priv_version, 16);
    p += 16;
    p += eir_fill_version_info(p, 240 - (p - gbredr_local_dev.eir.data));
    p += add_data_2_eir(p);
    data = gbredr_local_dev.eir.data;
    gbredr_local_dev.eir.len = p - data;
    gbredr_local_dev.eir.len = update_eir_data_hook(data, gbredr_local_dev.eir.len);
    SDK_ASSERT(gbredr_local_dev.eir.len < 240, 627, "eir->len < MAX_EIR_LEN");
    len = gbredr_local_dev.eir.len;
    gbredr_local_dev.eir.type = len < 17 ? 3 : (len < 121 ? 10 : 14);
}

/* Deviation kept from the BR23 baseline: the SDK frees an uninitialised
 * pointer when it built no EIR (HCI standard mode with the Classic module);
 * only an EIR this function allocated is freed. */
int RF_CODE link_inquiry_scan_enable(int clk_offset)
{
    u8 *eir_data = NULL;
    int ret;

    if (!config_btctler_hci_standard || !(config_btctler_mode & 1)) {
        eir_data = malloc(240);
        update_eir_data(eir_data);
    }
    ret = __inquiry_scan_enable(clk_offset, hci_param.addr, &gbredr_local_dev.eir);
    if (eir_data)
        free(eir_data);
    return ret & 255;
}

void RF_CODE link_inquiry_scan_disable(void)
{
    inquiry_scan_disable();
}

static void inquiry_scan_disable(void)
{
    struct link_inquiry_scan *scan;

    local_irq_disable();
    if (inquiry_scan == NULL) {
        local_irq_enable();
        return;
    }
    link_task_del(&inquiry_scan->task);
    bredr_link_v.bredr_link_state &= ~8;
    bredr_bd_put_frame(inquiry_scan->frame);
    scan = inquiry_scan;
    inquiry_scan = NULL;
    local_irq_enable();
    bt_free(scan);
    link_idle_task_enable_detect();
}

void RF_CODE link_page_scan_timeout(void *p)
{
    page_scan_resume(&((struct link_page_scan *)p)->task);
}

static int page_scan_resume(struct link_task *task)
{
    struct link_page_scan *scan = (struct link_page_scan *)((u8 *)task - offsetof(struct link_page_scan, task));

    if (scan->frame) {
        putchar('P');
        if (scan->timer == 0xff)
            scan->timer = slot_timer_get();
        memset(&scan->remote, 0, sizeof(scan->remote));
        bredr_normal_pwr_set(scan->frame, CONFIG_PAGE_SCAN_POWER);
        bredr_frame_page_scan_resume(scan);
        scan->substate = 0;
        scan->state = 2;
    }
    return 0;
}

void RF_CODE __quick_page_scan_disable(void)
{
    struct link_page_scan *scan;

    local_irq_disable();
    scan = g_quick_page_scan;
    g_quick_page_scan = NULL;
    __page_scan_disable(scan);
    local_irq_enable();
    if (scan)
        bt_free(scan);
}

static void __page_scan_disable(struct link_page_scan *scan)
{
    if (scan == NULL)
        return;
    page_scan_suspend(&scan->task);
    link_task_del(&scan->task);
    if (scan->frame) {
        bredr_bd_put_frame(scan->frame);
        scan->frame = NULL;
    }
    link_idle_task_enable_detect();
    bredr_link_v.bredr_link_state &= ~36;
}

__attribute__((weak)) u8 RF_CODE testbox_get_connect_status(void)
{
    return 0;
}

__attribute__((weak)) u8 RF_CODE chargestore_get_connect_status(void)
{
    return 0;
}

u8 crc8(const u8 *data, u16 len);
char tws_api_get_local_channel();

/* The SDK's static __calc_tws_info_crc (line 740). */
static inline __attribute__((always_inline)) void __calc_tws_info_crc(struct tws_pair_info *info)
{
    info->crc = crc8(&info->lt_addr, 15);
}

int RF_CODE __page_scan_enable(int clk_offset, u8 *local_addr, u8 rand_enable, u8 quick_mode, u8 *tws_addr)
{
    struct ctrl_frame *frame;
    struct link_page_scan *scan;

    printf("page_scan_enable: %d\n", quick_mode);
    put_buf(local_addr, 6);
    local_irq_disable();
    if (quick_mode ? g_quick_page_scan : g_page_scan) {
        local_irq_enable();
        return 0;
    }
    frame = bredr_bd_get_frame();
    if (frame == NULL) {
        local_irq_enable();
        return -12;
    }
    scan = bt_malloc(164);
    if (scan == NULL) {
        puts("page_scan bt_malloc err\n");
        bredr_bd_put_frame(frame);
        local_irq_enable();
        return -12;
    }
    if (quick_mode)
        g_quick_page_scan = scan;
    else
        g_page_scan = scan;
    scan->fhs = bd_base.rx_fhs;
    memcpy(scan->local_addr, local_addr, 6);
    __get_access_addr(scan->local_acc, local_addr);
    if (quick_mode) {
        memcpy(scan->tws_addr, tws_addr, 6);
        put_buf(scan->tws_addr, 6);
        __get_access_addr(scan->tws_acc, scan->tws_addr);
        scan->tws_tx_info.channel = tws_api_get_local_channel();
        scan->tws_tx_info.version = 0;
        memcpy(scan->tws_tx_info.addr, local_addr, 6);
        memcpy(scan->tws_tx_info.mac_addr, hci_param.addr, 6);
        __calc_tws_info_crc(&scan->tws_tx_info);
        scan->quick_mode = 1;
    }
    scan->substate = 0;
    scan->frame = frame;
    scan->rand_enable = rand_enable;
    scan->timer = 0xff;
    scan->state = 1;
    bredr_frame_page_scan_init(scan);
    if (CONFIG_BTCTLER_TWS_ENABLE && (quick_mode || memcmp(local_addr, gbredr_local_dev.internal_addr_local, 6) == 0))
        scan->task.type = 128;
    else
        scan->task.type = 4;
    scan->task.run_slot = 32;
    scan->task.run_num = 1;
    scan->task.priority = 1;
    scan->task.ops = &page_scan_task_ops;
    scan->task.sta_attribute = 1;
    bredr_link_v.bredr_link_state |= 4;
    link_task_add(&scan->task);
    local_irq_enable();
    if (testbox_get_connect_status() || chargestore_get_connect_status())
        link_task_idle_disable();
    else
        link_idle_task_enable_detect();
    return 0;
}

int RF_CODE link_page_scan_enable(int clk_offset)
{
    return __page_scan_enable(clk_offset, hci_param.addr, 0, 0, NULL);
}

void RF_CODE link_page_scan_disable(void)
{
    struct link_page_scan *scan, *quick;

    local_irq_disable();
    scan = g_page_scan;
    g_page_scan = NULL;
    __page_scan_disable(scan);
    quick = g_quick_page_scan;
    g_quick_page_scan = NULL;
    __page_scan_disable(quick);
    local_irq_enable();
    if (scan)
        bt_free(scan);
    if (quick)
        bt_free(quick);
}

__attribute__((weak)) int RF_CODE weak_link_inquiry_result_irq_handler(u8 type, u8 *addr, int class_dev, s16 clk_offset,
                                                                      u8 rssi, u8 *eir, u8 eir_len)
{
    memset(eir + eir_len, 0, 240 - eir_len);
    return 0;
}

/* Keep only the EIR's local-name field (type 8 or 9), at most 32 bytes. */
void RF_CODE inquiry_filter_eir_name(u8 *eir, u16 len, struct link_inquiry_result *result)
{
    u8 *p = eir;
    u16 n;

    while (len) {
        if (p[0] == 0)
            return;
        if ((p[1] & ~1) == 8) {
            n = p[0] + 1;
            n = n > len ? len : n;
            n = n < 32 ? n : 32;
            result->eir_len = n;
            memcpy(result->eir_data, p, n);
            return;
        }
        n = p[0] + 1;
        len -= n;
        p += n;
    }
}

static void inquiry_timeout() RF_CODE;
static int __inquiry_result_handler(struct link_inquiry_result *result) RF_CODE;

void RF_CODE link_inquiry_rx_packet_handler(int len)
{
    struct link_inquiry *inq = inquiry;
    struct link_inquiry_result *result = &inq->result;
    u16 crc;
    u8 i;
    u8 num;

    if (len) {
        crc = CRC16(inq->result.remote.addr, 6);
        for (i = 0; i < 30; i++) {
            if (crc == inquiry->result_crc[i])
                return;
        }
        num = inquiry->result_num;
        if (num < 30) {
            inquiry->result_num = ++num;
            inquiry->result_crc[num] = crc;    /* SDK quirk: slot 0 unused, 30 overflows */
        }
        if (inquiry->substate) {
            inq->result.eir_data = bt_malloc(240);
            if (inq->result.eir_data)
                inquiry_filter_eir_name(inquiry->eir, len, result);
            else
                inq->result.eir_len = 0;
        }
        inquiry->substate = 2;
        slot_timer_set(inquiry->timer, inquiry_timeout, NULL, 512);
        inquiry->completed(result);
        if (inquiry->search_num) {
            if (--inquiry->search_num == 0) {
                inquiry->completed(NULL);
                return;
            }
            goto timeout;
        }
        inq = inquiry;
    }
    if (inq == NULL)
        return;
timeout:
    if (bredr_clkn_after(bredr_read_slot_clk(), inquiry->timeout))
        inquiry->completed(NULL);
}

void RF_CODE inquiry_disable(int reason)
{
    if (inquiry == NULL)
        return;
    link_task_del(&inquiry->task);
    link_idle_task_enable_detect();
    if (inquiry->result.eir_data) {
        bt_free(inquiry->result.eir_data);
        inquiry->result.eir_data = NULL;
    }
    bredr_bd_put_frame(inquiry->frame);
    bt_free(inquiry);
    inquiry = NULL;
}

int RF_CODE link_inquiry_enable(u8 *lap, int time, u8 num, int (*completed)(struct link_inquiry_result *))
{
    struct ctrl_frame *frame;
    u8 addr[6];

    if (inquiry)
        return 0;
    frame = bredr_bd_get_frame();
    if (frame == NULL)
        return -12;
    inquiry = bt_malloc(196);
    if (inquiry == NULL) {
        bredr_bd_put_frame(frame);
        return -12;
    }
    inquiry->fhs = bd_base.rx_fhs;
    inquiry->eir = bd_base.rx_fhs;
    bredr_normal_pwr_set(frame, rf.bredr_inquiry_pwr_max);
    addr[0] = lap[0];
    addr[1] = lap[1];
    addr[2] = lap[2];
    addr[3] = 0;
    addr[4] = 0;
    addr[5] = 0;
    __write_reg_bdaddr(frame, addr);
    __write_reg_bch(frame, local_bch);
    INIT_LIST_HEAD(&inquiry->head);
    inquiry->substate = 0;
    inquiry->timeout = bredr_read_slot_clk() + time * 1000 / 625;
    inquiry->completed = completed ? completed : __inquiry_result_handler;
    inquiry->search_num = num;
    inquiry->clk_offset = gbredr_local_dev.clk_base;
    inquiry->frame = frame;
    inquiry->timer = 0xff;
    inquiry->state = 1;
    bredr_frame_inquiry_init(inquiry);
    inquiry->task.type = 8;
    inquiry->task.run_slot = 32;
    inquiry->task.run_num = 1;
    inquiry->task.priority = 1;
    inquiry->task.ops = &inquiry_task_ops;
    inquiry->task.sta_attribute = 1;
    link_task_idle_disable();
    link_task_add(&inquiry->task);
    return 0;
}

static int __inquiry_result_handler(struct link_inquiry_result *result)
{
    if (result == NULL) {
        inquiry_disable(0);
        hci_send_event(1, "1", 0);
        return 0;
    }
    if (result->eir_data == NULL) {
        hci_send_event(34, "1A11321", 1, result->remote.addr, 0, 0, 0, result->remote.class_dev,
                       result->remote.clk_offset, result->remote.rssi);
    } else {
        hci_send_event(47, "1A11321C032", 1, result->remote.addr, 0, 0, result->remote.class_dev,
                       result->remote.clk_offset, result->remote.rssi, result->eir_data);
        bt_free(result->eir_data);
        result->eir_data = NULL;
    }
    return 0;
}

void RF_CODE link_inquiry_disable(void)
{
    inquiry_disable(0);
}

int RF_CODE __get_lt_addr(void)
{
    u8 used = bredr_link_v.glt_addr;
    int i;

    for (i = 1; i < 8; i++) {
        if (!(used & 1 << i)) {
            bredr_link_v.glt_addr = used | 1 << i;
            return i;
        }
    }
    if (config_asser) {
        printf("file:%s, line:%d", BL_FILE, 1267);
        printf("ASSERT-FAILD: 0 no_lt_addr: %x\n", bredr_link_v.glt_addr);
        cpu_assert_debug();
    } else {
        cpu_reset();
    }
    return 1;
}

void RF_CODE __put_lt_addr(int lt_addr)
{
    bredr_link_v.glt_addr &= ~(1 << lt_addr);
}

/* A free eSCO LT address: the lowest 1..7 not used by a link's ACL or eSCO
 * LT address (two bytes per link, two links at most); 5 when all are taken. */
u8 RF_CODE esco_creart_lt_addr(void)
{
    struct link_connection *lc;
    u8 tmp[4] = { 0 };
    int i = 0, j;
    u8 lt;

    FOR_EACH_LINK(lc) {
        if (lc->frame) {
            tmp[i] = bredr_hw_read_lt_addr(lc->frame);
            tmp[i + 1] = lc->esco_lt_addr;
            i += 2;
        }
    }
    put_buf(tmp, 4);
    for (lt = 1; lt < 8; lt++) {
        for (j = 0; j < 4; j++) {
            if (tmp[j] == lt && tmp[j] != 0)
                break;
        }
        if (j == 4)
            return lt;
    }
    return 5;
}

/* Static inline in the SDK's frame header. */
static inline u8 bredr_hw_read_lt_addr(struct ctrl_frame *frame)
{
    return (frame->miscntl >> 4) & 7;
}

static inline void bredr_frame_set_ab_train(struct ctrl_frame *frame, int ab_train)
{
    frame->miscntl = (frame->miscntl & ~0x80) | ab_train << 7;
}

void RF_CODE link_page_timeout(void *p)
{
    struct link_page *page = p;
    struct ctrl_frame *frame = page->frame;
    void (*completed)(u8 *, int);
    u32 clk, diff;
    u8 lt_addr;

    if (page->quick_mode == 0 && page->substate < 2) {
        clk = READ_SLOT_CLK();
        diff = clk - page->ab_clkn + (clk > page->ab_clkn ? 0 : 0x8000000);
        if (diff > 35) {
            page->ab_clkn = clk;
            page->ab_train = !page->ab_train;
            bredr_frame_set_ab_train(frame, page->ab_train);
        }
        lt_addr = __get_lt_addr();
        __put_lt_addr(page->lt_addr);
        page->lt_addr = lt_addr;
        __write_fhs_packet(page->local_addr, gbredr_local_dev.dev_class, &gbredr_local_dev.eir, lt_addr, page->fhs);
        page->substate = 0;
        slot_timer_reset(page->timer, 18);
    } else {
        page_resume(&page->task);
    }
    if (page->completed && page->timeout && bredr_clkn_after(READ_SLOT_CLK(), page->timeout)) {
        completed = page->completed;
        page->completed = NULL;
        completed(page->remote.addr, 4);
    }
}

static int page_resume(struct link_task *task)
{
    struct link_page *page = (struct link_page *)((u8 *)task - offsetof(struct link_page, task));

    if (page->frame) {
        putchar('C');
        page->substate = 0;
        page->clk_offset = gbredr_local_dev.clk_base;
        if (page->timer == 0xff)
            page->timer = slot_timer_get();
        bredr_normal_pwr_set(page->frame, CONFIG_PAGE_POWER);
        bredr_frame_page_resume(page);
        page->state = 2;
    }
    return 0;
}

int RF_CODE __page_enable(u8 *addr, u8 ab_train, int clk_offset, u8 *local_addr, u16 timeout, u8 rand_enable,
                          u8 quick_mode, u8 *tws_addr, void (*completed)(u8 *, int))
{
    struct link_connection *lc;
    struct ctrl_frame *frame;
    struct link_page *page;
    int links = 0, lt_addr;

    printf("link_page_enable %d\n", quick_mode);
    put_buf(local_addr, 6);
    put_buf(addr, 6);
    local_irq_disable();
    if (quick_mode ? g_quick_page : g_page) {
        local_irq_enable();
        return 0;
    }
    FOR_EACH_LINK(lc)
        links += lc->frame != NULL;
    if (links >= 2) {
        local_irq_enable();
        return -14;
    }
    frame = bredr_bd_get_frame();
    if (frame == NULL) {
        local_irq_enable();
        return -12;
    }
    page = bt_malloc(168);
    if (page == NULL) {
        bredr_bd_put_frame(frame);
        local_irq_enable();
        return -12;
    }
    if (quick_mode)
        g_quick_page = page;
    else
        g_page = page;
    bredr_normal_pwr_set(frame, rf.bredr_page_pwr_max);
    memcpy(page->remote.addr, addr, 6);
    memcpy(page->local_addr, local_addr, 6);
    __get_access_addr(page->remote.access_addr, addr);
    __get_access_addr(page->local_acc, local_addr);
    if (quick_mode) {
        memcpy(page->tws_addr, tws_addr, 6);
        __get_access_addr(page->tws_acc, page->tws_addr);
        put_buf(page->tws_addr, 6);
        lt_addr = __get_lt_addr();
        if ((lt_addr & 255) == 7) {
            lt_addr = __get_lt_addr();
            __put_lt_addr(7);
        }
        page->lt_addr = lt_addr;
        page->tws_tx_info.channel = tws_api_get_local_channel();
        page->tws_tx_info.version = 0;
        page->tws_tx_info.lt_addr = page->lt_addr;
        memcpy(page->tws_tx_info.addr, local_addr, 6);
        memcpy(page->tws_tx_info.mac_addr, hci_param.addr, 6);
        __calc_tws_info_crc(&page->tws_tx_info);
    } else {
        page->lt_addr = __get_lt_addr();
        __write_fhs_packet(page->local_addr, gbredr_local_dev.dev_class, &gbredr_local_dev.eir, page->lt_addr,
                           page->fhs);
    }
    page->substate = 0;
    page->ab_clkn = READ_SLOT_CLK();
    page->ab_train_max = ab_train_table[ab_train];
    page->clk_offset = gbredr_local_dev.clk_base;
    page->remote.sr = ab_train;
    page->remote.clk_offset = clk_offset;
    page->frame = frame;
    page->rand_enable = rand_enable;
    page->completed = completed;
    page->timer = slot_timer_get();
    page->quick_mode = quick_mode;
    page->state = 1;
    page->timeout = timeout ? bredr_offset2clkn(READ_SLOT_CLK(), timeout * 1000 / 625) : 0;
    bredr_frame_page_init(page);
    bredr_link_v.bredr_link_state |= 1;
    page->task.type = quick_mode ? 64 : 16;
    page->task.run_slot = 32;
    page->task.run_num = 1;
    page->task.priority = 1;
    page->task.ops = &page_task_ops;
    page->task.sta_attribute = 1;
    link_task_idle_disable();
    link_task_add(&page->task);
    local_irq_enable();
    return 0;
}

int RF_CODE link_page_enable(u8 *addr, u8 ab_train, int clk_offset, void (*completed)(u8 *, int))
{
    return __page_enable(addr, ab_train, clk_offset, hci_param.addr, gbredr_local_dev.page_timeout_value, 0, 0, NULL,
                         completed);
}

static void __page_disable(struct link_page *page) RF_CODE;

void RF_CODE link_page_disable(int reason)
{
    struct link_page *pages[2];
    struct link_page *page;
    int i;

    local_irq_disable();
    pages[0] = g_page;
    g_page = NULL;
    __page_disable(pages[0]);
    pages[1] = g_quick_page;
    g_quick_page = NULL;
    __page_disable(pages[1]);
    bredr_link_v.bredr_link_state &= ~33;
    local_irq_enable();
    link_idle_task_enable_detect();
    for (i = 0; i < 2; i++) {
        page = pages[i];
        if (page) {
            if (page->completed)
                page->completed(page->remote.addr, reason);
            bt_free(page);
        }
    }
}

static void __page_disable(struct link_page *page)
{
    if (page == NULL)
        return;
    page_suspend(&page->task);
    link_task_del(&page->task);
    if (page->frame) {
        bredr_bd_put_frame(page->frame);
        page->frame = NULL;
    }
    if (page->lt_addr)
        __put_lt_addr(page->lt_addr);
}

struct link_connection *RF_CODE get_link_connection(void)
{
    struct link_connection *lc = malloc(740), *p;
    u32 used = 0;
    int i;

    if (lc == NULL)
        return NULL;
    memset(lc, 0, 740);
    FOR_EACH_LINK(p)
        used |= 1 << p->index;
    for (i = 0; i < 2; i++) {
        if (!(used & 1 << i)) {
            lc->index = i;
            goto add;
        }
    }
    if (config_asser) {
        printf("file:%s, line:%d", BL_FILE, 1643);
        printf("ASSERT-FAILD: 0 link_conn_index: %x\n", used);
        cpu_assert_debug();
    } else {
        cpu_reset();
    }
add:
    list_add_tail(&lc->entry, &bredr_link_v.link_conn_head);
    return lc;
}

struct link_connection *RF_CODE link_connection_for_addr(u8 *addr)
{
    struct link_connection *lc;

    FOR_EACH_LINK(lc) {
        if (memcmp(lc->core.remote_addr, addr, 6) == 0)
            return lc;
    }
    return NULL;
}

int RF_CODE link_conn_set_rx_bulk_in_irq(struct link_connection *lc, int index)
{
    struct ctrl_frame *frame = lc->frame;
    u8 *buf = (u8 *)link_rx_bulk[lc->index][index];

    __write_reg_rxptr(frame, index, buf);
    __clean_reg_rxfull(frame, index);
    lc->rx_data[index] = buf;
    return 1;
}

static void lc_lrc_winsize_dec(struct link_connection *lc) RF_CODE __attribute__((noinline));
static void connection_get_agc_ctl_pwr(struct link_connection *lc) RF_CODE __attribute__((noinline));

int RF_CODE link_conn_rx_empty(struct link_connection *lc)
{
    struct bredr_payload_head head[2];

    local_irq_disable();
    if (g_dut_hdl == NULL)
        link_conn_rx_handler(lc, head, 0);
    local_irq_enable();
    return lc->rx_pend == 0;
}

void IRQ_CODE link_conn_rx_handler(struct link_connection *lc, struct bredr_payload_head *head, int have_head)
{
    struct ctrl_frame *frame = lc->frame;
    struct bredr_payload_head *h;
    u8 index, type, llid, bit;
    u16 len, check;
    int i, ok;

    if (lc->in_rx_handler)
        return;
    lc->in_rx_handler = 1;
    lc->sniff_Communicate = 1;
    if (!have_head)
        bredr_frame_get_rx_payload_head(frame, head);
    if (CONFIG_BTCTLER_TWS_ENABLE)
        link_conn_rx_esco_handler(lc, have_head);
    for (i = 0; i < 2; i++) {
        h = &head[i];
        index = h->index;
        if (index == 0xff)
            break;
        type = h->type;
        llid = h->llid;
        len = h->len;
        check = h->check;
        bit = 1 << index;
        if ((check & 2) && !(lc->rx_pend & bit)) {
            lc_lrc_winsize_dec(lc);
            if (type > 1) {
                lc->rx_packet_total++;
                if (check == 127 || check == 255)
                    lc->rx_packet_num++;
            }
            if (type < 10)
                lc->rx_packet_slots += 2;
            else
                lc->rx_packet_slots += type < 14 ? 4 : 6;
            if (lc->role)
                bredr_fre_offset_adjust(lc);
            if (!lc->test_con_flag)
                connection_get_agc_ctl_pwr(lc);
            link_conn_super_timeout_reset(lc);
            if (!lc->tws_link && lc->state == 2 && !lc->sniff_sta && lc->esco == NULL && link_conn_num() == 1)
                link_task_try_run_slots(&lc->task, 8);
        }
        if ((lc->rx_follow_ctrl & bit) && !(lc->rx_pend & bit)) {
            if (lc->rx_follow_ctrl_enable)
                break;
            __clean_reg_rxfull(lc->frame, index);
            lc->rx_follow_ctrl &= ~bit;
            h->index = 0xff;
            continue;
        }
        if (CONFIG_BTCTLER_TWS_ENABLE)
            tws_link_conn_rx_packet_handler(lc, h);
        if (check == 127) {
            switch (type) {
            case 0:
            case 1:
            case 2:
            case 9:
                __clean_reg_rxfull(frame, index);
                break;
            default:
                ok = len <= 679;
                if (config_asser) {
                    if (!ok) {
                        printf("file:%s, line:%d", BL_FILE, 3234);
                        printf("ASSERT-FAILD: len <= 679 %d\n", len);
                        cpu_assert_debug();
                    }
                } else if (!ok) {
                    cpu_reset();
                }
                if (ok && len == 0) {
                    __clean_reg_rxfull(frame, index);
                    break;
                }
                if (!lc->tws_link)
                    link_conn_rx_packet_handler(lc, llid, len, index);
                break;
            }
        } else {
            __clean_reg_rxfull(frame, index);
        }
        if (lc->rx_pend & bit)
            break;
    }
    connection_tx_handler(lc);
    lc->in_rx_handler = 0;
}

int RF_CODE link_conn_tx_empty(struct link_connection *lc)
{
    int empty;

    local_irq_disable();
    if (g_dut_hdl == NULL)
        connection_tx_handler(lc);
    empty = lc->tx_bulk[0] == NULL && lc->tx_bulk[1] == NULL && list_empty(&lc->tx_acl_c) && list_empty(&lc->tx_acl_u);
    local_irq_enable();
    return empty;
}

/* Retire what the baseband sent from each empty tx buffer (post actions:
 * PTT, power, unsniff ack, encryption, AFH, max slot, events) and refill it
 * from the LMP queue, then the ACL queue.  The second buffer is refilled only
 * when the first one got a packet.  A TWS monitor link only drains. */
void IRQ_CODE connection_tx_handler(void *_lc)
{
    struct link_connection *lc = _lc;
    struct ctrl_frame *frame = lc->frame;
    struct bredr_tx_bulk **slot, *tx, *next = NULL;
    u8 update_index[2] = { 0xff, 0xff };
    u8 index;
    u8 enc;
    int i;

    if (lc->in_tx_handler)
        return;
    lc->in_tx_handler = 1;
    bredr_frame_get_empty_txbuf_index(frame, update_index);
    if (CONFIG_BTCTLER_TWS_ENABLE && lc->monitor) {
        for (;;) {
            tx = bredr_tx_bulk_pop(&lc->tx_acl_c);
            if (tx == NULL)
                tx = bredr_tx_bulk_pop(&lc->tx_acl_u);
            if (tx == NULL)
                break;
            if (tx->event_prob)
                bredr_link_event(lc, 1, tx->event_prob);
            if (tx->event_post)
                bredr_link_event(lc, 1, tx->event_post);
            if (tx->max_slot_post)
                lc->core.max_slot = tx->max_slot_post;
            bredr_tx_bulk_free(tx);
        }
        lc->in_tx_handler = 0;
        return;
    }
    for (i = 0; i < 2; i++) {
        index = update_index[i];
        if (index == 0xff)
            continue;
        slot = &lc->tx_bulk[index];
        tx = *slot;
        if (tx && tx != lc->tx_bulk[index == 0] && tx->offset == tx->len) {
            *slot = NULL;
            /* LMP_accepted_ext(packet_type_table_req): switch PTT now. */
            if (tx->llid == 3 && tx->data[0] > 253 && tx->data[1] == 1 && tx->data[3] == 11)
                link_conn_set_ptt(lc, 1);
            if (tx->pwr_post)
                lc->power.normal.pwr_txing = 0;
            if (tx->unsniff_ack_check) {
                lc->unsniff_ack = 1;
                puts("unsniff_ack_1\n");
            }
            if (tx->pause_enc_debug)
                puts("pause_enc_debug\n");
            enc = tx->enc_post;
            if (enc & 4) {
                link_conn_set_encrypt(lc, (enc >> 1) & 1, enc & 1);
                puts("post_enc\n");
            }
            if (tx->set_afh)
                bredr_link_set_afh(lc, lc->afh.instant, lc->afh.en, lc->channel_map);
            if (tx->max_slot_post)
                lc->core.max_slot = tx->max_slot_post;
            if (tx->event_post) {
                if (!lc->tws_link)
                    bredr_link_event(lc, 1, tx->event_post);
                else if (CONFIG_BTCTLER_TWS_ENABLE)
                    tws_conn_tx_event_handler(tx->event_post, tx);
            }
            bredr_tx_bulk_free(tx);
        }
        if (next || i == 0) {
            next = bredr_tx_bulk_pop(&lc->tx_acl_c);
            if (CONFIG_BTCTLER_TWS_ENABLE) {
                if (next == NULL && lc->send_acl_enable && (lc->tws_link || !lc->sniff_sta))
                    next = bredr_tx_bulk_pop(&lc->tx_acl_u);
            } else if (next == NULL && lc->send_acl_enable) {
                next = bredr_tx_bulk_pop(&lc->tx_acl_u);
            }
            if (next) {
                lc->sniff_long_interval_filter = 2;
                __link_ctrl_tx(next, index, lc);
            }
        } else {
            next = NULL;
        }
        *slot = next;
    }
    lc->in_tx_handler = 0;
}

/* A link's supervision timeout (its own slot timer).  After 1000 slots
 * without a packet the power goes to step 9; a TWS link in sniff gets 6400
 * slots more. */
void RF_CODE link_conn_super_timeout(void *p)
{
    struct link_connection *lc = p;
    u32 now = bredr_read_slot_clk();
    int offset, t, ret;

    slot_timer_reset(lc->super_timer, 800);
    if (lc->frame == NULL || lc->super_timeout == 0)
        return;
    offset = bredr_clkn2offset(lc->super_timeout_begin, now);
    if (offset > 999)
        bredr_pwr_set(lc, 0, 9);
    t = offset;
    if (lc->sniff_sta)
        t = lc->tws_link ? offset - 6400 : offset;
    if (lc->super_timeout > t)
        return;
    bredr_power_on();
    if (CONFIG_BTCTLER_TWS_ENABLE) {
        if (lc->tws_link) {
            ret = os_taskq_post_type("tws", 45, 0, NULL);
            if (ret == 0) {
                lc->super_timeout = 0;
                slot_timer_disable(lc->super_timer);
            }
            printf("tws_link_super_timeout: %d\n", ret);
            return;
        }
        if (lc->monitor)
            return;
    }
    if (bredr_link_event(lc, 1, 3) == 0) {
        lc->super_timeout = 0;
        slot_timer_disable(lc->super_timer);
    }
}

__attribute__((weak)) void RF_CODE spp_printf(const char *format, ...)
{
}

void RF_CODE link_conn_qos_enable(struct link_connection *lc)
{
    local_irq_disable();
    if (lc && lc->qos_check_enable == 0)
        lc->qos_check_enable = 3;
    local_irq_enable();
}

void RF_CODE link_conn_qos_disable(struct link_connection *lc)
{
    local_irq_disable();
    if (lc == NULL || lc->qos_check_enable == 0) {
        local_irq_enable();
        return;
    }
    lc->qos_check_enable = 0;
    lc->rx_state = 66;
    if ((REG(0x1c0110) & 7) == lc->frame->id)
        REG(0x1c0114) &= ~1;
    local_irq_enable();
}

void RF_CODE link_conn_esco_qos_enable(struct link_connection *lc)
{
}

void RF_CODE link_conn_qos_esco_disable(struct link_connection *lc)
{
}

/* Per-run QoS polling decision: rx_state 71 polls the link (register
 * 0x1c0114 bit 0 for this frame), 66 stops it. */
void RF_CODE link_conn_qos_check(struct link_connection *lc, u32 run_slots)
{
    u8 enable, state, sum;
    u16 run;
    int ratio, few_slots;

    if (!CONFIG_BTCTLER_QOS_ENABLE)
        goto reset;
    if (lc->pure_monitor) {
        lc->rx_state = 66;
        goto reset;
    }
    if (run_slots == 0)
        goto reset;
    enable = lc->qos_check_enable;
    if (enable == 0 || lc->rx_full || (enable & 2))
        goto reset;
    if (!lc->monitor && lc->slave_rx_state == 66)
        goto reset;
    if (bredr_clkn2offset(lc->resume_clkn, READ_SLOT_CLK()) - (int)run_slots > 39) {
        lc->rx_state = 66;
        goto reset;
    }
    sum = lc->rx_packet_sum + lc->rx_packet_num;
    lc->rx_packet_sum = sum;
    lc->rx_packet_num = 0;
    run = lc->run_slots_sum += run_slots;
    state = lc->rx_state;
    if (state == 71) {
        if (run < 100)
            goto poll;
        if (lc->rx_packet_slots * 100 / run > 59)
            lc->rx_state = 66;
        goto reset;
    }
    if (run < 400)
        goto check;
    ratio = lc->rx_packet_total ? sum * 100 / lc->rx_packet_total : 0;
    few_slots = sum ? run / sum < 35 : 1;
    if (ratio > 70 && few_slots) {
        lc->rx_state = 71;
        lc->stop_incr_power = 0;
    }
reset:
    lc->run_slots_sum = 0;
    lc->rx_packet_num = 0;
    memset(&lc->rx_packet_sum, 0, 4);  /* rx_packet_sum, rx_packet_total, rx_packet_slots */
    lc->qos_check_enable &= ~2;
    if (lc->tws_link)
        return;
    state = lc->rx_state;
check:
    if (state == 66) {
        REG(0x1c0114) &= ~1;
        if (!lc->stop_incr_power && !lc->monitor && lc->esco == NULL && list_empty(&lc->tx_acl_c))
            bredr_link_event(lc, 2, 12, 2);
        return;
    }
poll:
    REG(0x1c0110) &= ~7;
    REG(0x1c0110) |= lc->frame->id;
    REG(0x1c0114) = lc->pure_monitor || lc->monitor ? 1291 : 1303;
}

int IRQ_CODE link_conn_task_resume(struct link_task *task)
{
    struct link_connection *lc = (struct link_connection *)((u8 *)task - offsetof(struct link_connection, task));
    u8 qos;

    if (lc->esco || lc->pure_monitor) {
        qos = 19;
        goto set;
    }
    if (lc->monitor) {
        qos = 31;
set:
        REG(0x1c0014) &= ~0xff;
        REG(0x1c0014) |= qos;
    }
    link_conn_qos_check(lc, lc->run_slots);
    lc->rx_full = 0;
    lc->state = 2;
    lc->resume_clkn = READ_SLOT_CLK();
    bredr_bd_frame_enable(lc->frame, 0, 0);
    if (!lc->sniff_sta) {
        wind_align_set(103);
        windset(lc->frame, 103, 0, 0);
        REG(0x1c0008) = (REG(0x1c0008) & ~0xfff) | 188;
    }
    return 0;
}

int IRQ_CODE link_conn_task_suspend(struct link_task *task)
{
    struct link_connection *lc = (struct link_connection *)((u8 *)task - offsetof(struct link_connection, task));

    if (g_dut_hdl)
        return 0;
    if (lc->monitor)
        REG(0x1c0014) = (REG(0x1c0014) & ~0xff) | 19;
    if (lc->state == 3)
        return 0;
    lc->state = 3;
    bredr_bd_frame_disable(lc->frame);
    lc->run_slots = bredr_clkn2offset(lc->resume_clkn, READ_SLOT_CLK());
    return 0;
}

void RF_CODE link_connection_init(struct link_connection *lc)
{
    lc->state = 2;
    lc->send_acl_enable = 1;
    lc->core.max_slot = 1;
    lc->super_timer = slot_timer_get();
    lc->afh_timer = 0xff;
    lc->adjust_clkoffset_enable = 1;
    lc->rx_state = 66;
    lc->slave_rx_state = 71;
    lc->task.type = 32;
    lc->task.run_slot = 200;
    lc->task.run_num = 1;
    lc->task.priority = 2;
    lc->task.ops = &conn_task_ops;
    lc->task.sta_attribute = 1;
    INIT_LIST_HEAD(&lc->task.entry);
    INIT_LIST_HEAD(&lc->tx_acl_c);
    INIT_LIST_HEAD(&lc->tx_acl_u);
    INIT_LIST_HEAD(&lc->rx_head);
    memset(lc->channel_map, 0xff, 10);
    bredr_rx_bulk_used_reset(lc->index, 100);
}

static inline void bredr_frame_set_packet_type_table(struct ctrl_frame *frame, u8 enable);
static inline u8 bredr_frame_get_ltaddr(struct ctrl_frame *frame);

struct link_connection *RF_CODE create_link_connection(struct ctrl_frame *frame, struct bredr_remote_device *remote,
                                                       u8 role, u8 tws)
{
    struct link_connection *lc = get_link_connection(), *p;
    int i;

    if (lc == NULL)
        return NULL;
    link_connection_init(lc);
    lc->role = role;
    lc->frame = frame;
    lc->core.remote_sr = remote->sr;
    lc->core.remote_class_dev = remote->class_dev;
    lc->test_con_flag = 0;
    memcpy(lc->core.remote_addr, remote->addr, 6);
    memcpy(lc->remote_mac_addr, remote->mac_addr, 6);
    bredr_pwr_set(lc, 0, 15);
    lc->offset_packet = 60;
    lc->offset_sun = 0;
    lc->super_timeout_begin = bredr_read_slot_clk();
    lc->super_timeout = gbredr_local_dev.super_timeout_value * 1000 / 625;
    for (i = 0; i < 2; i++)
        link_conn_set_rx_bulk_in_irq(lc, i);
    bredr_frame_set_irq_handler(frame, lc, bredr_frame_conn_rx_irq_handler);
    if (CONFIG_BREDR_INQUIRY)
        inquiry_disable(0);
    inquiry_scan_disable();
    link_task_schedule_suspend();
    link_page_disable(254);
    link_page_scan_disable();
    link_task_schedule_resume();
    bredr_link_v.bredr_link_state |= 16;
    link_task_idle_disable();
    if (CONFIG_BTCTLER_TWS_ENABLE) {
        if (tws) {
            memcpy(gbredr_local_dev.internal_addr_remote, lc->core.remote_addr, 6);
            lc->super_timeout = 1600;
            slot_timer_set(lc->super_timer, link_conn_super_timeout, lc, 800);
            lc->tws_link = 1;
            lc->task.ops = &tws_link_task_ops;
            lc->task.sta_attribute = 3;
            lc->task.type = 256;
            lc->core.pwr_set = 9;
            lc->core.max_slot = 5;
            bredr_pwr_set(lc, 0, 9);
            link_conn_set_ptt(lc, 1);
            bdhw_set_afh(frame, 1, lc->channel_map);
            tws_link_conn_start_in_irq(lc, lc->role);
            link_task_add(&lc->task);
            bredr_link_v.glt_addr |= 1 << bredr_frame_get_ltaddr(lc->frame);
            return lc;
        }
        tws_tx_unsniff_req();
        slot_timer_set(lc->super_timer, link_conn_super_timeout, lc, 800);
    } else {
        slot_timer_set(lc->super_timer, link_conn_super_timeout, lc, 800);
        FOR_EACH_LINK(p) {
            if (p != lc && p->frame && !p->tws_link)
                link_conn_set_max_rx_bulk_persent(p, 70);
        }
    }
    link_task_add(&lc->task);
    bredr_link_event(lc, 2, 1, role);
    return lc;
}

void RF_CODE link_conn_set_ptt(struct link_connection *lc, u8 ptt)
{
    bredr_frame_set_packet_type_table(lc->frame, ptt);
}

static inline u8 bredr_frame_get_ltaddr(struct ctrl_frame *frame)
{
    return (frame->miscntl >> 4) & 7;
}

void RF_CODE link_conn_set_max_rx_bulk_persent(struct link_connection *lc, int percent)
{
    bredr_rx_bulk_set_max_used_persent(lc->index, percent);
}

/* The packet table row for a payload: the first whose length fits, within
 * the allowed slots, else the last allowed one. */
const u16 *IRQ_CODE get_packet_type(int len, u8 ppt, u8 slot, u8 tws, u16 tx_offset)
{
    const u16 (*table)[4];
    int i, rows;

    if (config_asser) {
        if (slot == 0) {
            printf("file:%s, line:%d", BL_FILE, 2291);
            printf("ASSERT-FAILD: slot >= 1 slot = %d\n", slot);
            cpu_assert_debug();
        }
    } else if (slot == 0) {
        cpu_reset();
    }
    if (ppt) {
        table = packet_2M_table;
        rows = 3;
    } else {
        table = tws ? packet_1M_table_tws : packet_1M_table;
        rows = tws ? 3 : 6;
    }
    for (i = 0; i < rows; i++) {
        if (table[i][3] > slot)
            break;
        if (table[i][0] >= len)
            return table[i];
    }
    return table[i - 1];
}

/* Frequency offset tracking: average 60 packet offsets, then trim the
 * oscillator (the SDK only does this in the normal Classic mode). */
void RF_CODE bredr_fre_offset_adjust(struct link_connection *lc)
{
    struct ctrl_frame *frame = lc->frame;
    int sum;

    if (bredr_get_dut_enable())
        return;
    if (!(config_btctler_mode & 1))
        return;
    if (lc->test_con_flag)
        return;
    sum = lc->offset_sun + (s16)(0 - REG(0x1cfc18));
    lc->offset_sun = sum;
    if (lc->offset_packet < 61 && --lc->offset_packet == 0) {
        lc->offset_sun = sum / 60;
        bredr_bd_close_frame();
        frame->osc_set += ((0 - lc->offset_sun) << 20) / 24000;
        bredr_bd_open_frame();
    }
}

static inline u8 bredr_frame_get_packet_type_table(struct ctrl_frame *frame);

void IRQ_CODE __link_ctrl_tx(struct bredr_tx_bulk *tx, int index, struct link_connection *lc)
{
    struct ctrl_frame *frame = lc->frame;
    const u16 *row;
    int remain, n;
    u16 offset;
    u8 enc, event;

    enc = tx->enc_prob;
    if (enc & 4) {
        link_conn_set_encrypt(lc, (enc >> 1) & 1, enc & 1);
        puts("prob_enc\n");
    }
    event = tx->event_prob;
    if (event) {
        if (CONFIG_BTCTLER_TWS_ENABLE && lc->tws_link)
            tws_conn_tx_event_handler(event, tx);
        else
            bredr_link_event(lc, 1, event);
    }
    switch (tx->llid) {
    case 3:
        if (lc->tws_link) {
            if (config_asser) {
                printf("file:%s, line:%d", BL_FILE, 2383);
                printf("ASSERT-FAILD: 0 tx->llid == ACL_C: %d, %d\n", tx->data[0], tx->data[1]);
                cpu_assert_debug();
            } else {
                cpu_reset();
            }
        }
        __write_reg_txinfo(frame, index, tx->data, tx->len, tx->llid, 1, tx->packet);
        tx->offset = tx->len;
        list_del(&tx->entry);
        break;
    case 1:
    case 2:
        remain = tx->len - tx->offset;
        row = get_packet_type(remain, bredr_frame_get_packet_type_table(frame), lc->core.max_slot, lc->tws_link, 0);
        n = remain > row[1] ? row[1] : remain;
        offset = tx->offset;
        __write_reg_txinfo(frame, index, &tx->data[offset], n, tx->llid == 2 && offset == 0 ? 2 : 1, 1, row[2]);
        tx->offset += n;
        if (tx->len == tx->offset)
            list_del(&tx->entry);
        break;
    default:
        if (config_asser) {
            printf("file:%s, line:%d", BL_FILE, 2416);
            printf("ASSERT-FAILD: 0 unknow_llid: %d\n", tx->llid);
            cpu_assert_debug();
        } else {
            cpu_reset();
        }
        break;
    }
    local_irq_disable();
    __clean_reg_txempty(frame, index);
    local_irq_enable();
}

void RF_CODE link_conn_set_encrypt(struct link_connection *lc, u8 rx_enc, u8 tx_enc)
{
    __write_reg_encry(lc->frame, rx_enc, tx_enc);
}

static inline u8 bredr_frame_get_packet_type_table(struct ctrl_frame *frame)
{
    return (frame->miscntl >> 3) & 1;
}

void RF_CODE link_conn_tx_post_handler(struct link_connection *lc, struct bredr_tx_bulk *tx)
{
    if (tx->event_prob)
        bredr_link_event(lc, 1, tx->event_prob);
    if (tx->event_post)
        bredr_link_event(lc, 1, tx->event_post);
    if (tx->max_slot_post)
        lc->core.max_slot = tx->max_slot_post;
    bredr_tx_bulk_free(tx);
}

static inline void bredr_frame_set_packet_type_table(struct ctrl_frame *frame, u8 enable)
{
    frame->miscntl = enable ? frame->miscntl | 8 : frame->miscntl & ~8;
}

void RF_CODE bredr_link_set_afh(struct link_connection *lc, u32 instant, u8 enable, u8 *channel_map)
{
    u32 clk, end, target, slots;
    u8 timer;

    if (!enable) {
        bdhw_disable_afh(lc->frame);
        return;
    }
    if (lc->channel_map_update) {
        if (lc->afh_slot_end == instant)
            return;
        bredr_link_enable_afh(lc);
    }
    lc->channel_map_update = 1;
    memcpy(lc->channel_map, channel_map, 10);
    end = lmp_sniff_and_afh_offset_ali(lc, instant) + instant;
    if (CONFIG_BTCTLER_TWS_ENABLE)
        lc->afh_slot_end = end;
    clk = bredr_get_master_slot_clk(lc);
    target = (end + 0x7fffffe) & 0x7ffffff;
    if (target >= clk)
        slots = (target - clk) & 0xffffffe;
    else if (((clk - target) & 0xffffffe) > 69120000)
        slots = (((end + 0x7fffffe) | 0x8000000) - clk) & 0xffffffe;
    else
        slots = 2;
    slots = slots > 2 ? slots : 2;
    timer = lc->afh_timer;
    if (timer == 0xff)
        timer = lc->afh_timer = slot_timer_get();
    slot_timer_set(timer, bredr_link_enable_afh, lc, slots > 69120000 ? 2 : slots);
}

void RF_CODE link_conn_tx_bulk_avaiable(struct link_connection *lc, struct bredr_tx_bulk *tx)
{
    struct link_connection *p;

    if (!(config_btctler_modules & 1))
        return;
    local_irq_disable();
    FOR_EACH_LINK(p) {
        if (p != lc)
            continue;
        if (lc->frame == NULL)
            break;
        if (tx) {
            if (tx->llid == 3) {
                if (CONFIG_TEST_DUT_ONLY_BOX_CODE || CONFIG_TEST_DUT_CODE)
                    dut_clear_tx_buf(lc);
                list_add_tail(&tx->entry, &lc->tx_acl_c);
            } else {
                if (CONFIG_BTCTLER_TWS_ENABLE && !lc->tws_link)
                    tws_conn_role_switch_lock_ms(300);
                list_add_tail(&tx->entry, &lc->tx_acl_u);
            }
        }
        if (g_dut_hdl == NULL)
            connection_tx_handler(lc);
        local_irq_enable();
        return;
    }
    if (tx)
        bredr_tx_bulk_free(tx);
    local_irq_enable();
}

void RF_CODE frame_info_prf(struct ctrl_frame *frame)
{
    printf("clk:%x %x\n", frame->clk_offseth, frame->clk_offsetl);
    printf("addr:%x %x %x\n", frame->bdaddr0, frame->bdaddr1, frame->bdaddr2);
    printf("bch:%x %x %x\n", frame->bchh, frame->bchm, frame->bchl);
}

/* A new channel map from the link's statistics: of the 30 most received
 * channels (counts taken mod 256), those under an error percentage that
 * starts at 10 and grows by 5 up to 25, until more than 14 are found; under
 * 20 channels, fill up to 30 with the lowest error rates.  mode != 0 means
 * all 79 channels.  Returns 1 if the map changed. */
int RF_CODE bredr_link_update_afh(struct link_connection *lc, u8 *channel_map, u8 *tx_channel_classification, u8 mode)
{
    u8 err_persent[2][79];      /* [0] channel, [1] error % */
    u8 rx_cnt[2][79];           /* [0] channel, [1] rx count & 255 */
    u8 curr_channel_map[10];
    int i, j, limit, cnt;
    u16 rx;
    u8 a, b, c, ch;

    local_irq_disable();
    if (lc == NULL) {
        local_irq_enable();
        return -22;
    }
    memcpy(curr_channel_map, lc->channel_map, 10);
    if (mode) {
        memset(channel_map, 0xff, 9);
        channel_map[9] = 127;
        local_irq_enable();
        goto result;
    }
    for (i = 0; i < 79; i++) {
        rx = lc->afh.rx_num[i];
        rx_cnt[0][i] = i;
        rx_cnt[1][i] = rx;
        err_persent[0][i] = i;
        err_persent[1][i] = (rx & 255) > 19 ? lc->afh.err_num[i] * 100 / (rx & 255) : 0;
    }
    local_irq_enable();
    for (i = 0; i < 78; i++) {
        for (j = i + 1; j < 79; j++) {
            a = rx_cnt[1][i];
            b = rx_cnt[1][j];
            if (a < b) {
                rx_cnt[1][j] = a;
                rx_cnt[1][i] = b;
                c = rx_cnt[0][j];
                rx_cnt[0][j] = rx_cnt[0][i];
                rx_cnt[0][i] = c;
            }
        }
    }
    limit = 10;
    cnt = 0;
    for (;;) {
        memset(channel_map, 0, 10);
        for (i = 0; i < 30; i++) {
            ch = rx_cnt[0][i];
            if (rx_cnt[1][i] && err_persent[1][ch] < limit) {
                channel_map[ch >> 3] |= 1 << (ch & 7);
                cnt++;
            }
        }
        if (cnt > 14) {
            if (cnt >= 20)
                goto result;
            break;
        }
        limit += 5;
        if (limit > 29)
            break;
    }
    for (i = 0; i < 78; i++) {
        for (j = i + 1; j < 79; j++) {
            a = err_persent[1][i];
            b = err_persent[1][j];
            if (a > b) {
                err_persent[1][j] = a;
                err_persent[1][i] = b;
                c = err_persent[0][j];
                err_persent[0][j] = err_persent[0][i];
                err_persent[0][i] = c;
            }
        }
    }
    for (i = 0; cnt < 30 && i < 79; i++) {
        ch = err_persent[0][i];
        if (!(channel_map[ch >> 3] & 1 << (ch & 7))) {
            channel_map[ch >> 3] |= 1 << (ch & 7);
            cnt++;
        }
    }
result:
    return memcmp(channel_map, curr_channel_map, 10) != 0;
}

#define CLEAR_PENDING(lc, index) do { (lc)->rx_follow_ctrl &= ~(1 << (index)); (lc)->rx_pend &= ~(1 << (index)); } while (0)

int IRQ_CODE link_conn_rx_packet_handler(struct link_connection *lc, u8 llid, u16 len, u8 index)
{
    u8 *data = lc->rx_data[index];
    struct ctrl_frame *frame = lc->frame;
    struct lmp_connection *conn = lmp_conn_for_link(lc);
    struct bredr_rx_bulk *bulk;
    int total, push = 1;
    u8 bit;

    if (lmp_cmd_handler_in_irq(lc, data, llid)) {
        __clean_reg_rxfull(frame, index);
        return 0;
    }
    switch (llid) {
    case 3:
        bulk = bredr_rx_bulk_alloc(lc->index, len);
        break;
    case 2:
        if (g_lmp_update_start_flag) {
            bulk = bredr_rx_bulk_alloc(0, len);
            break;
        }
        if (lc->rx_bulk) {
            bredr_rx_bulk_free(lc->rx_bulk);
            lc->rx_bulk = NULL;
        }
        total = (data[1] << 8 | data[0]) + 4;
        if (total > 2047) {
            __clean_reg_rxfull(frame, index);
            CLEAR_PENDING(lc, index);
            return 0;
        }
        if (total < len) {
            put_buf(lc->rx_data[index == 0], 16);
            put_buf(lc->rx_data[index], len);
            __clean_reg_rxfull(frame, index);
            CLEAR_PENDING(lc, index);
            return 0;
        }
        (void)frame->rxcrc[index];      /* read and unused in the SDK */
        if (conn && tws_check_sbc_data(conn, data, len)) {
            __clean_reg_rxfull(frame, index);
            CLEAR_PENDING(lc, index);
            return 0;
        }
        if (CONFIG_BTCTLER_TWS_ENABLE) {
            bulk = bredr_rx_bulk_alloc_reserve(lc->index, total, lc->monitor ? 0 : 36);
        } else {
            bulk = bredr_rx_bulk_alloc(lc->index, total);
            if (len < total) {
                lc->rx_bulk = bulk;
                push = 0;
            }
        }
        break;
    default:
        if (CONFIG_BTCTLER_TWS_ENABLE) {
            bulk = bredr_rx_bulk_alloc_reserve(lc->index, len, lc->monitor ? 0 : 36);
            break;
        }
        bulk = lc->rx_bulk;
        if (bulk == NULL) {
            __clean_reg_rxfull(frame, index);
            return -22;
        }
        total = (bulk->data[1] << 8 | bulk->data[0]) + 4;
        if (bulk->len + len > total) {
            bredr_rx_bulk_free(bulk);
            lc->rx_bulk = NULL;
            __clean_reg_rxfull(frame, index);
            return -22;
        }
        memcpy(&bulk->data[bulk->len], data, len);
        __clean_reg_rxfull(frame, index);
        bulk->len += len;
        if (bulk->len < total)
            return 0;
        lc->rx_bulk = NULL;
        goto push;
    }
    bit = 1 << index;
    if (bulk == NULL) {
        lc->rx_pend |= bit;
        lc->rx_follow_ctrl |= bit;
        lc->rx_full = 1;
        lc->rx_state = 66;
        if ((REG(0x1c0110) & 7) == frame->id)
            REG(0x1c0114) &= ~1;
        putchar('F');
        return -12;
    }
    bulk->len = len;
    bulk->llid = llid;
    bulk->hash = frame->rxcrc[index];
    memcpy(bulk->data, data, len);
    lc->prev_rxcrc = bulk->hash;
    if (lc->rx_follow_ctrl_enable) {
        lc->rx_follow_ctrl |= bit;
    } else {
        __clean_reg_rxfull(frame, index);
        lc->rx_follow_ctrl &= ~bit;
    }
    lc->rx_pend &= ~bit;
    if (!push)
        return 0;
    if (CONFIG_BTCTLER_TWS_ENABLE && tws_core_plink_rx_irq_handler(lc, bulk) == -22)
        return 0;
push:
    bredr_rx_bulk_push(&lc->rx_head, bulk);
    bredr_link_event(lc, 1, 5);
    return 0;
}

void RF_CODE bredr_link_printf_state(void)
{
}

void IRQ_CODE bredr_link_head_err_irq_handler(struct ctrl_frame *frame)
{
    struct bredr_payload_head head;
    struct link_connection *lc;

    if (!CONFIG_BTCTLER_TWS_ENABLE)
        return;
    head.check = 2;
    FOR_EACH_LINK(lc) {
        if (lc->frame == frame)
            tws_link_conn_rx_packet_handler(lc, &head);
    }
}

void RF_CODE bredr_pwr_set_tws_ana(struct link_connection *lc, char level);

void IRQ_CODE link_conn_rx_esco_handler(struct link_connection *lc, int call_from_irq)
{
    struct bredr_esco_link *esco;
    u32 con;
    u16 stat;

    if (!call_from_irq || tws_link.link == NULL || lc->tws_link)
        return;
    esco = lc->esco;
    if (esco == NULL)
        return;
    con = esco->hw.reg->BREDR_ESCO_CON;
    if (!(con & 1))
        return;
    stat = (con & 2) ? lc->frame->escorxstat1 : lc->frame->escorxstat0;
    if ((s8)stat < 0 && !(stat & 512))
        lc->esco_tws_nack_flag = 1;
    if (!lc->monitor)
        return;
    if ((stat & 7) != esco->pdu.lt_addr)
        return;
    if (stat & 64) {
        lc->phone_rx_esco_nack = 0;
        return;
    }
    if (++lc->phone_rx_esco_nack > 40 && lc->phone_rx_esco_inc_anc == 0) {
        lc->phone_rx_esco_nack = 0;
        lc->phone_rx_esco_inc_anc = 200;
        bredr_pwr_set_tws_ana(lc, bt_get_pwr_max_level() > 9 ? 5 : 3);
        putchar('@');
    }
}

void RF_CODE bredr_pwr_set_tws_ana(struct link_connection *lc, char level)
{
    struct ctrl_frame *frame;

    if (lc->tws_link)
        return;
    if (level > bt_get_pwr_max_level())
        level = bt_get_pwr_max_level();
    frame = lc->frame;
    frame->tws_ana_index &= ~0xff;
    frame->tws_ana_index |= level;
}

static void lc_lrc_winsize_dec(struct link_connection *lc)
{
    if (lc->sniff_rx_flag)
        lc->sniff_rx_flag = 0;
}

/* Remote power control from the frame's AGC hint: 1 asks the peer to lower
 * its power, 2 to raise it, at a rate that slows with repeated requests. */
static void connection_get_agc_ctl_pwr(struct link_connection *lc)
{
    struct link_power *p = &lc->power.normal;
    int agc;
    u16 limit;
    u8 req;

    if (g_dut_hdl)
        return;
    if (CONFIG_BTCTLER_TWS_ENABLE && lc->tws_link)
        return;
    agc = bredr_frame_get_agc(lc->frame);
    req = p->pwr_tx_decr_req;
    if ((req && (agc & 255) == 2) || ((agc & 255) == 1 && p->pwr_tx_incr_req))
        limit = 10;
    else
        limit = pwr_tb[p->pwr_tx_cnt];
    if (++p->pwr_set_cnt <= limit)
        return;
    p->pwr_set_cnt = 0;
    switch ((u8)agc) {
    case 1:
        if (p->min_power || p->pwr_txing)
            return;
        p->pwr_txing = 1;
        if (req < 6) {
            p->pwr_tx_decr_req = req + 1;
            p->pwr_tx_cnt = 0;
        } else if (req < 12) {
            p->pwr_tx_decr_req = req + 1;
            p->pwr_tx_cnt = 1;
        } else if (req < 18) {
            p->pwr_tx_decr_req = req + 1;
            p->pwr_tx_cnt = 2;
        } else {
            p->pwr_tx_cnt = 3;
        }
        if (list_empty(&lc->tx_acl_c))
            bredr_link_event(lc, 2, 12, 1);
        p->pwr_tx_incr_req = 0;
        p->max_power = 0;
        break;
    case 2:
        if (p->pwr_txing)
            return;
        p->pwr_txing = 1;
        req = p->pwr_tx_incr_req;
        if (req < 6) {
            p->pwr_tx_incr_req = req + 1;
            p->pwr_tx_cnt = 0;
        } else if (req < 12) {
            p->pwr_tx_incr_req = req + 1;
            p->pwr_tx_cnt = 1;
        } else if (req < 18) {
            p->pwr_tx_incr_req = req + 1;
            p->pwr_tx_cnt = 2;
        } else {
            p->pwr_tx_cnt = 3;
        }
        if (list_empty(&lc->tx_acl_c))
            bredr_link_event(lc, 2, 12, 2);
        if (p->max_power)
            p->pwr_tx_cnt = 3;
        p->pwr_tx_decr_req = 0;
        p->min_power = 0;
        break;
    }
}

void RF_CODE link_conn_super_timeout_reset(struct link_connection *lc)
{
    if (lc == NULL)
        return;
    lc->super_timeout_begin = bredr_read_slot_clk();
    slot_timer_reset(lc->super_timer, 800);
}

int RF_CODE link_conn_rx_bulk_total_size(struct link_connection *lc)
{
    return bredr_rx_bulk_total_size(lc->index);
}

int RF_CODE link_conn_rx_bulk_remain_size(struct link_connection *lc, int unit_size)
{
    return bredr_rx_bulk_remain_size(lc->index, unit_size);
}

struct bredr_rx_bulk *RF_CODE link_conn_rx_bulk_alloc_for(struct link_connection *lc, int len)
{
    return bredr_rx_bulk_alloc_for(lc->index, len);
}

void RF_CODE link_conn_rx_bulk_avaliable(struct link_connection *lc, struct bredr_rx_bulk *bulk)
{
    struct bredr_payload_head head[2];
    struct link_connection *p;

    if (bulk)
        bredr_rx_bulk_free(bulk);
    local_irq_disable();
    FOR_EACH_LINK(p) {
        if (p->frame && !p->rx_follow_ctrl_enable && g_dut_hdl == NULL)
            link_conn_rx_handler(p, head, 0);
    }
    local_irq_enable();
}

void RF_CODE link_conn_follow_ctrl_enable(struct link_connection *lc)
{
    lc->rx_follow_ctrl_enable = 1;
}

void RF_CODE link_conn_follow_ctrl_disable(struct link_connection *lc)
{
    struct bredr_payload_head head[2];

    local_irq_disable();
    lc->rx_follow_ctrl_enable = 0;
    if (g_dut_hdl == NULL)
        link_conn_rx_handler(lc, head, 0);
    local_irq_enable();
}

/* The SDK's static helpers at lines 3337, 3352 and 1674, inlined into
 * link_connection_close. */
static inline __attribute__((always_inline)) void link_connection_clear_rx_bulk(struct link_connection *lc)
{
    struct list_head *node, *next;

    if (lc->rx_bulk) {
        bredr_rx_bulk_free(lc->rx_bulk);
        lc->rx_bulk = NULL;
    }
    for (node = lc->rx_head.next; node != &lc->rx_head; node = next) {
        next = node->next;
        __list_del_entry(node);
        bredr_rx_bulk_free((struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry)));
    }
}

static inline __attribute__((always_inline)) void link_connection_clear_tx_bulk(struct link_connection *lc)
{
    struct list_head *node, *next;

    if (lc->tx_bulk[0]) {
        bredr_tx_bulk_free(lc->tx_bulk[0]);
        if (lc->tx_bulk[1] == lc->tx_bulk[0])
            lc->tx_bulk[1] = NULL;
        lc->tx_bulk[0] = NULL;
    }
    if (lc->tx_bulk[1]) {
        bredr_tx_bulk_free(lc->tx_bulk[1]);
        lc->tx_bulk[1] = NULL;
    }
    for (node = lc->tx_acl_c.next; node != &lc->tx_acl_c; node = next) {
        next = node->next;
        bredr_tx_bulk_free((struct bredr_tx_bulk *)((u8 *)node - offsetof(struct bredr_tx_bulk, entry)));
    }
    for (node = lc->tx_acl_u.next; node != &lc->tx_acl_u; node = next) {
        next = node->next;
        bredr_tx_bulk_free((struct bredr_tx_bulk *)((u8 *)node - offsetof(struct bredr_tx_bulk, entry)));
    }
}

static inline __attribute__((always_inline)) void __put_link_connection(struct link_connection *lc)
{
    __list_del_entry(&lc->entry);
}

void RF_CODE link_connection_close(struct link_connection *lc)
{
    struct bredr_role_switch *rs;

    local_irq_disable();
    if (!lc->tws_link)
        link_conn_qos_disable(lc);
    if (!(lc->role && !lc->tws_link))
        __put_lt_addr(bredr_frame_get_ltaddr(lc->frame));
    slot_timer_put(lc->super_timer);
    lc->super_timeout = 0;
    if (lc->esco)
        bredr_esco_link_close(lc);
    if (lc->afh_timer != 0xff) {
        slot_timer_put(lc->afh_timer);
        lc->afh_timer = 0xff;
    }
    rs = lc->rs;
    if (rs) {
        if (rs->page)
            bt_free(rs->page);
        if (lc->rs->page_scan)
            bt_free(lc->rs->page_scan);
        bt_free(lc->rs);
        lc->rs = NULL;
    }
    bredr_link_v.bredr_link_state &= ~16;
    link_task_del(&lc->task);
    if (CONFIG_TEST_DUT_CODE)
        bredr_dut_detch(lc);
    link_idle_task_enable_detect();
    link_connection_clear_rx_bulk(lc);
    link_connection_clear_tx_bulk(lc);
    bredr_bd_put_frame(lc->frame);
    lc->frame = NULL;
    __put_link_connection(lc);
    local_irq_enable();
    bredr_rx_bulk_state();
    if (CONFIG_BTCTLER_TWS_ENABLE && !lc->tws_link)
        link_task_schedule_resume();
    free(lc);
}

int link_conn_num_phone(void);

void RF_CODE bredr_esco_link_close(struct link_connection *lc)
{
    struct list_head *node, *next;
    struct ctrl_frame *frame;
    int tws, i;

    printf("esco_close: %x\n", lc->esco);
    tws = CONFIG_BTCTLER_TWS_ENABLE;
    if (lc && tws && lc->tws_link) {
        bredr_esco_tws_link_close(lc);
        tws_esco_remove_clear_state();
        return;
    }
    if (lc->esco == NULL) {
        printf("no esco link return");
        return;
    }
    if (lc->esco->timer0 != 0xff) {
        slot_timer_put(lc->esco->timer0);
        lc->esco->timer0 = 0xff;
    }
    if (lc->esco->timer1 != 0xff) {
        slot_timer_put(lc->esco->timer1);
        lc->esco->timer1 = 0xff;
    }
    bredr_pwr_set_tws_ana(lc, bt_get_pwr_max_level());
    __pcm_out_disable(0);
    __pcm_out_disable(1);
    local_irq_disable();
    if (lc->role) {
        frame = lc->frame;
        __write_reg_clkoffset(frame, frame->clk_offset);
        frame = lc->frame;
        bredr_set_frame_bitoff(frame, frame->bitoff);
    }
    esco_interrupt_a2dp_shedult_cn = 0;
    bredr_put_esco_hw(&lc->esco->hw);
    frame = lc->frame;
    frame->format |= 96;
    bd_set_prio(frame, 20, 0);
    set_btagc_pwr_sel(1);
    bredr_rx_bulk_free(lc->esco->rx_bulk[0]);
    bredr_rx_bulk_free(lc->esco->rx_bulk[1]);
    for (i = 0; i < 3; i++) {
        if (lc->esco->tx_bulk[i])
            bredr_tx_bulk_free(lc->esco->tx_bulk[i]);
    }
    for (node = lc->esco->tx_esco.next; node != &lc->esco->tx_esco; node = next) {
        next = node->next;
        bredr_tx_bulk_free((struct bredr_tx_bulk *)((u8 *)node - offsetof(struct bredr_tx_bulk, entry)));
    }
    for (node = lc->esco->rx_esco.next; node != &lc->esco->rx_esco; node = next) {
        next = node->next;
        __list_del_entry(node);
        bredr_rx_bulk_free((struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry)));
    }
    bt_free(lc->esco);
    lc->esco = NULL;
    lc->adjust_clkoffset_enable = 1;
    if (tws) {
        reset_tws_supertimeout();
        link_task_run_slots(&lc->task, 64);
        if (bredr_esco_tws_link_close(lc))
            puts("bredr_esco_tws_link_close over\n");
        tws_esco_remove_clear_state();
    } else if (link_conn_num_phone() > 1) {
        link_task_try_run_slots(&lc->task, 64);
    }
    local_irq_enable();
    os_sem_post((OS_SEM *)&esco_sem);
    puts("esco_link_close: exit\n");
}

void RF_CODE bredr_link_esco_rx_irq_handler(int esco_id)
{
    struct link_connection *lc;
    struct bredr_esco_link *esco;

    FOR_EACH_LINK(lc) {
        esco = lc->esco;
        if (esco && esco->hw.esco_id == esco_id && esco->hw.frame) {
            if (!lc->tws_link)
                audio_sync_lat_hopclkn(lc->frame);
            if (lc->esco->get_data)
                lc->esco->get_data(lc);
            return;
        }
    }
}

int RF_CODE link_conn_get_ptt(struct link_connection *lc)
{
    return bredr_frame_get_packet_type_table(lc->frame);
}

int RF_CODE link_conn_get_ltaddr(struct link_connection *lc)
{
    return bredr_frame_get_ltaddr(lc->frame);
}

int RF_CODE lc_send_packet(struct link_connection *lc, struct bredr_tx_bulk *tx)
{
    return 0;
}

static inline void bredr_frame_set_encrypt_key(struct ctrl_frame *frame, const u8 *key);
static inline s16 bredr_frame_local_slot_diff(struct ctrl_frame *frame);

void RF_CODE link_conn_set_encrypt_key(struct link_connection *lc, u8 *key)
{
    bredr_frame_set_encrypt_key(lc->frame, key);
}

static inline void bredr_frame_set_encrypt_key(struct ctrl_frame *frame, const u8 *key)
{
    int i;

    for (i = 0; i < 8; i++)
        frame->cryptk[i] = key[i * 2 + 1] << 8 | key[i * 2];
}

s16 RF_CODE lc_local_slot_diff(struct link_connection *lc)
{
    return bredr_frame_local_slot_diff(lc->frame);
}

static inline s16 bredr_frame_local_slot_diff(struct ctrl_frame *frame)
{
    return frame->bitoff;
}

u32 RF_CODE lc_local_slot_bitoff(struct link_connection *lc)
{
    return (s16)lc->frame->bitoff;
}

u32 RF_CODE lc_local_slot_offset(struct link_connection *lc)
{
    s16 bitoff = lc->frame->bitoff;

    SDK_ASSERT(bitoff < 1250, 3631, "offset < 1250");
    return 1249 - bitoff;
}

void RF_CODE lp_winsize_init(struct lp_ws_t *lp_ws)
{
    bredr_link_v.lp_ws = lp_ws;
}

u8 low_power_get_default_osc_type(void);
void low_power_reset_osc_type(u8 type);
u8 low_power_get_osc_type(void);
extern BTCTRLER_CONFIG int sniff_support_reset_anchor_point, sniff_long_interval;

/* The SDK's static helpers at lines 3650, 3692 and 3727, inlined into
 * lc_sniff_ctrl. */
static inline __attribute__((always_inline)) void lc_sniff_init_lp_ws(struct link_connection *lc)
{
    u8 osc = low_power_get_default_osc_type();
    struct lp_ws_t *ws;

    lc->lp_cur_osc = osc;
    lc->lp_last_osc = osc;
    ws = bredr_link_v.lp_ws;
    lc->winsize = osc == 1 ? ws->bt_osc_ws_init : ws->lrc_ws_init;
}

static inline __attribute__((always_inline)) void lc_lrc_osc_change(void)
{
    struct link_connection *lc;
    u8 osc;

    switch (bredr_link_v.lp_ws->osc_change_mode) {
    case 0:
        return;
    case 1:
        osc = low_power_get_default_osc_type();
        FOR_EACH_LINK(lc) {
            if (lc->frame && lc->sniff_rx_flag)
                osc = 1;
        }
        break;
    default:
        osc = 1;
        break;
    }
    low_power_reset_osc_type(osc);
    FOR_EACH_LINK(lc) {
        if (lc->frame)
            lc->lp_cur_osc = osc;
    }
}

static inline __attribute__((always_inline)) void cal_sniff_interval_offset(struct link_connection *lc, u32 *param)
{
    struct ctrl_frame *frame;
    struct lp_ws_t *ws;
    u32 clk_offset = 0;
    u16 bitoff = 0, current;
    int inc;

    if (!lc->role) {
        frame = lc->frame;
        current = frame->bitoff;
    } else {
        ws = bredr_link_v.lp_ws;
        inc = low_power_get_osc_type() == 1 ? ws->bt_osc_ws_inc : ws->lrc_ws_inc;
        if (param && sniff_support_reset_anchor_point)
            inc = *param == sniff_long_interval ? inc : 20;
        frame = lc->frame;
        preserve_bitoff_cal(frame, frame->bitoff, &clk_offset, &bitoff, inc >> 1);
        __write_reg_clkoffset(lc->frame, clk_offset);
        frame = lc->frame;
        frame->miscntl |= 4;
        if (lc->sniff_rx_flag)
            frame->winsz0 += inc;
        else
            frame->winsz0 = inc + 103;
        current = frame->bitoff = bitoff;
    }
    preserve_bitoff_cal(frame, current, &clk_offset, &bitoff, 200);
    if (lc->tws_link && lc->state == 2)
        bredr_bd_frame_enable(lc->frame, 0, 1);
    if (param) {
        param[0] = clk_offset;
        param[1] = bitoff;
    }
}

s32 RF_CODE lc_sniff_ctrl(struct link_connection *lc, SNIFF_CTRL_CMD cmd, u32 *param)
{
    struct ctrl_frame *frame;

    if (lc == NULL)
        return 0;
    frame = lc->frame;
    switch ((int)cmd) {
    case 1:
        lc->wide_win_flag = 1;
        lc->adjust_clkoffset_enable = 1;
        lc->sniff_rx_flag = 1;
        break;
    case 2:
        lc->adjust_clkoffset_enable = 0;
        bredr_bd_frame_disable(frame);
        while (REG(0x1c0000) & 256)
            ;
        break;
    case 3:
        lc->adjust_clkoffset_enable = 0;
        lc_lrc_osc_change();
        cal_sniff_interval_offset(lc, param);
        if (CONFIG_BTCTLER_TWS_ENABLE && lc->tws_link && lc->role && tws_link.plink)
            link_conn_super_timeout_reset(tws_link.plink);
        break;
    case 5:
        set_btagc_pwr_sel(0);
        lc_sniff_init_lp_ws(lc);
        lc->sniff_sta = 1;
        if (lc->tws_link)
            break;
        if (tws_link.link) {
            frame = lc->frame;
            tws_link.twscon = frame->twscon;
            frame->miscntl &= ~512;
            frame->twscon = 0;
            if (!tws_link.link->role)
                link_conn_qos_disable(lc);
            else
                frame->miscntl |= 8192;
        } else {
            link_conn_qos_disable(lc);
        }
        windset(lc->frame, 113, 0, 0);
        break;
    case 6:
        if (!lc->tws_link) {
            if (tws_link.link) {
                frame->miscntl |= 512;
                if (frame->twscon == 0)
                    frame->twscon = tws_link.twscon;
                if (tws_link.link->role)
                    frame->miscntl &= ~8192;
            }
            if (lc->role) {
                frame->bitoff = lc->sniff_last_bitoff;
                __write_reg_clkoffset(frame, lc->sniff_last_soft_clknoff);
                frame = lc->frame;
            }
            windset(frame, 103, 0, 0);
        }
        lc->sniff_sta = 0;
        lc->adjust_clkoffset_enable = 1;
        set_btagc_pwr_sel(1);
        REG(0x1c0000) |= 4;
        lc->frame->miscntl &= ~260;
        break;
    }
    return 0;
}

/* The generated header declares bool; the SDK returns a byte. */
u8 link_sniff_sta_detect_all_ir(void) __asm__("link_sniff_sta_detect_all");

u8 RF_CODE link_sniff_sta_detect_all_ir(void)
{
    struct link_connection *lc;
    u8 links = 0, sniff = 0;

    if (g_page || g_quick_page || inquiry)
        return 0;
    FOR_EACH_LINK(lc) {
        if (lc->frame) {
            links++;
            sniff += lc->sniff_sta;
        }
    }
    return links && links == sniff;
}

void RF_CODE lc_qos_ctrl(struct link_connection *lc, int enable)
{
}

int RF_CODE link_conn_num(void)
{
    struct link_connection *lc;
    int n = 0;

    FOR_EACH_LINK(lc)
        n += lc->frame != NULL;
    return n;
}

int RF_CODE link_conn_num_phone(void)
{
    struct link_connection *lc;
    int n = 0;

    if (CONFIG_BTCTLER_TWS_ENABLE)
        return 0;
    FOR_EACH_LINK(lc) {
        if (lc->frame)
            n += !lc->tws_link;
    }
    return n;
}

int RF_CODE link_conn_num_more_than_one(void)
{
    return link_conn_num_phone() > 1;
}

static void role_switch_page_timeout(void *p) RF_CODE;
static void role_switch_page_rx_handler(void *p) RF_CODE;

/* The SDK's static role_switch_page (line 4024), inlined into its caller. */
static inline __attribute__((always_inline)) void role_switch_page(struct link_connection *lc)
{
    struct ctrl_frame *frame = lc->frame;
    struct link_page *page = lc->rs->page;
    u8 tog, lt_addr;

    puts("role_switch_page\n");
    tog = frame->txtog & 1;
    memcpy(page->remote.addr, lc->core.remote_addr, 6);
    memcpy(page->local_addr, hci_param.addr, 6);
    __get_access_addr(page->remote.access_addr, lc->core.remote_addr);
    __get_access_addr(page->local_acc, page->local_addr);
    page->substate = 0;
    page->ab_clkn = READ_SLOT_CLK();
    lt_addr = __get_lt_addr();
    page->lt_addr = lt_addr;
    page->clk_offset = gbredr_local_dev.clk_base;
    page->remote.sr = lc->core.remote_sr;
    page->remote.clk_offset = 0;
    page->frame = frame;
    page->state = 1;
    __write_fhs_packet(page->local_addr, gbredr_local_dev.dev_class, &gbredr_local_dev.eir, lt_addr, page->fhs);
    __write_reg_txinfo(frame, 0, page->fhs, 18, 3, 0, 2);
    __write_reg_txinfo(frame, 1, page->fhs, 18, 3, 0, 2);
    frame->format = 98;
    __write_reg_linkcntl(frame, tog, 16386);
    __write_reg_linkcntl(frame, tog ^ 1, 16401);
    __write_reg_txphdr(frame, tog, 2);
    __write_reg_txphdr(frame, tog ^ 1, 2);
    __clean_reg_txempty(frame, tog);
    __clean_reg_txempty(frame, tog ^ 1);
    __write_reg_rxptr(frame, 0, page->fhs);
    __write_reg_rxptr(frame, 1, page->fhs);
    __clean_reg_rxfull(frame, 0);
    __clean_reg_rxfull(frame, 1);
    lc->rs->rs_timeout_en = 1;
    page->timer = slot_timer_get();
    slot_timer_set(page->timer, role_switch_page_timeout, lc, 16);
    bredr_frame_set_irq_handler(frame, lc, role_switch_page_rx_handler);
    bredr_pwr_set(lc, 0, 15);
}

void RF_CODE bredr_switch_role_to_master(struct link_connection *lc, int slot_offset)
{
    struct link_page *page = bt_malloc(168);
    struct bredr_role_switch *rs;

    SDK_ASSERT(page != NULL, 4294, "rs_page");
    rs = bt_malloc(20);
    SDK_ASSERT(rs != NULL, 4297, "rs");
    rs->slot_offset = slot_offset;
    rs->page = page;
    lc->rs = rs;
    page->backup_lt_addr = bredr_hw_read_lt_addr(lc->frame);
    role_switch_page(lc);
}

void RF_CODE link_role_switch_page_scan_completed(struct link_page_scan *scan, int err)
{
    struct link_connection *lc = scan->private_data;
    struct ctrl_frame *frame = lc->frame;
    int i;

    slot_timer_put(scan->timer);
    if (err == 0) {
        lc->role = 1;
        __put_lt_addr(scan->backup_lt_addr);
    } else {
        __write_reg_bdaddr(frame, hci_param.addr);
        __write_reg_bch(frame, scan->local_acc);
        bredr_hw_write_lt_addr(frame, scan->backup_lt_addr);
        bredr_frame_set_ab_train(frame, scan->ab_train);
    }
    bt_free(scan);
    bredr_frame_set_irq_handler(frame, lc, bredr_frame_conn_rx_irq_handler);
    for (i = 0; i < 2; i++)
        link_conn_set_rx_bulk_in_irq(lc, i);
    lmp_role_switch_completed(lc, err);
    bredr_link_event(lc, 2, 4, err);
}

void RF_CODE bredr_switch_role_to_slave(struct link_connection *lc, int slot_offset)
{
    struct link_page_scan *scan = bt_malloc(164);
    struct ctrl_frame *frame;

    SDK_ASSERT(scan != NULL, 4354, "rs_page_scan");
    frame = lc->frame;
    scan->frame = frame;
    scan->slot_offset = slot_offset;
    scan->private_data = lc;
    scan->backup_lt_addr = bredr_hw_read_lt_addr(frame);
    scan->fhs = bd_base.rx_fhs;
    bredr_frame_rs_page_scan_enable(scan);
    bredr_pwr_set(lc, 0, 15);
}

u32 RF_CODE bredr_read_slot_clk(void)
{
    return READ_SLOT_CLK();
}

u32 RF_CODE bredr_finetimecnt(void)
{
    return GET_FINETIMECNT();
}

void RF_CODE link_conn_close(struct link_connection *lc)
{
    struct link_connection *p;

    if (lc)
        link_connection_close(lc);
    if (CONFIG_BTCTLER_TWS_ENABLE)
        return;
    local_irq_disable();
    FOR_EACH_LINK(p) {
        if (p->frame)
            link_conn_set_max_rx_bulk_persent(p, 100);
    }
    local_irq_enable();
}

void RF_CODE bredr_get_link_slot_clk(struct link_connection *lc, u32 *mclk, u32 *lclk)
{
    u32 now = READ_SLOT_CLK();
    int clk = ((int)__read_reg_clkoffset(lc->frame) >> 1) + now;

    if (clk < 0)
        clk += 0x8000000;
    else if (clk > 0x7ffffff)
        clk -= 0x8000000;
    if (mclk)
        *mclk = clk;
    if (lclk)
        *lclk = now;
}

u32 RF_CODE bredr_get_master_slot_clk(struct link_connection *lc)
{
    u32 clk;

    bredr_get_link_slot_clk(lc, &clk, NULL);
    return clk;
}

void RF_CODE bredr_link_enable_afh(void *_lc)
{
    struct link_connection *lc = _lc, *p;

    local_irq_disable();
    if (lc->afh_timer != 0xff) {
        slot_timer_put(lc->afh_timer);
        lc->afh_timer = 0xff;
    }
    local_irq_enable();
    lc->channel_map_update = 0;
    bdhw_set_afh(lc->frame, 1, lc->channel_map);
    if (!CONFIG_BTCTLER_TWS_ENABLE)
        return;
    FOR_EACH_LINK(p) {
        if (lc->tws_link != p->tws_link)
            bdhw_set_afh(p->frame, 2, lc->channel_map);
    }
}

/* Which eSCO buffer the hardware is on; returns 1 for the second bank. */
u8 RF_CODE get_esco_buf_index(void *_lc, u8 *pkt_flag)
{
    struct link_connection *lc = _lc;
    u32 con;

    SDK_ASSERT(lc != NULL, 4598, "_lc");
    con = lc->esco->hw.reg->BREDR_ESCO_CON;
    *pkt_flag = 0;
    if (con & 2) {
        if (con & 32)
            *pkt_flag = 1;
        if (!((s8)con < 0))
            *pkt_flag = con & 32 ? 3 : 2;
        return 0;
    }
    if (con & 64)
        *pkt_flag = 1;
    if (!(con & 256))
        *pkt_flag = con & 64 ? 3 : 2;
    return 1;
}

u8 RF_CODE bredr_get_esco_packet_type(u8 packet)
{
    switch (packet) {
    case 0:
    case 7:
    case 12:
    case 13:
        return packet;
    case 38:
        return 6;
    case 44:
        return 12;
    case 55:
        return 7;
    case 61:
        return 13;
    default:
        return 0;
    }
}

void RF_CODE bredr_esco_mcrst_con(void *_lc)
{
    struct link_connection *lc = _lc;
    struct bredr_esco_link *esco = lc->esco;

    if (esco)
        REG(0x1c013c) |= 1 << esco->hw.esco_id;
}

void RF_CODE bredr_esco_step_by_step(void *_lc)
{
    struct link_connection *lc = _lc;
    struct bredr_esco_link *esco = lc->esco;     /* the SDK loads before its NULL test */

    if (lc == NULL || esco == NULL)
        return;
    bredr_esco_mcrst_con(lc);
    switch (esco->esco_setp) {
    case 1:
        if (g_quick_page_scan)
            link_task_run(&g_quick_page_scan->task);
        break;
    case 2:
        if (tws_esco_transmit_deal(2, lc) == 0) {
            tws_esco_acl_communication(lc, NULL);
            tws_esco_transmit_deal(7, lc);
        }
        break;
    case 3:
        tws_esco_transmit_deal(7, lc);
        break;
    case 5:
        esco_1to2_deal(lc, 5);
        break;
    }
}

int RF_CODE esco_1to2_deal(struct link_connection *lc, u8 esco_setp)
{
    struct link_connection *p;

    if (esco_setp == 5)
        link_other_task_run_slots(&lc->task, 12);
    if (lc->esco->max_slot_cnt > 20) {
        lc->esco->max_slot_cnt = 0;
        FOR_EACH_LINK(p) {
            if (p->state && p->core.max_slot != 1) {
                if (p == lc)
                    puts("not_max_slot\n");
                else
                    puts("other_not_max_slot\n");
                bredr_link_event(p, 2, 6, 1);
            }
        }
    }
    return 0;
}

int RF_CODE get_esco_conn_bitoff(s16 *bitoff)
{
    struct link_connection *lc;

    FOR_EACH_LINK(lc) {
        if (lc->esco) {
            *bitoff = lc->frame->bitoff;
            return 1;
        }
    }
    return 0;
}

void RF_CODE set_1to2_esco_interrupt_a2dp_shedult_cnt(u8 en, u16 shedule_cnt_t)
{
    struct link_connection *lc;

    if (en) {
        esco_interrupt_a2dp_shedult_cn = shedule_cnt_t;
        return;
    }
    esco_interrupt_a2dp_shedult_cn = 0;
    FOR_EACH_LINK(lc) {
        if (lc->esco)
            lc->esco->rx_remove_esco_req_flag = 0;
    }
}

int RF_CODE bredr_link_clk_offset(struct link_connection *lc)
{
    return __read_reg_clkoffset(lc->frame) >> 1;
}

int RF_CODE link_rand_page_enable(u8 *addr)
{
    return 0;
}

/* The firmware-update hand-over keeps a struct bredr_table image at
 * UPDATA_BREDR_BASE_BEG (see bredr_frame.c). */
extern u32 UPDATA_BREDR_BASE_BEG[];
#define UPDATA_BT ((struct bredr_table *)UPDATA_BREDR_BASE_BEG)->bt_updata

void RF_CODE __bt_updata_save_link_info(struct link_connection *lc, struct bd_bt_updata *info)
{
    struct link_power *p = &lc->power.normal;

    printf("\n\n\n-------fun:%s parm = 0x%x\n", __func__, info);
    info->max_power = p->max_power;
    info->min_power = p->min_power;
    info->pwr_txing = p->pwr_txing;
    info->pwr_tx_cnt = p->pwr_tx_cnt;
    info->pwr_tx_decr_req = p->pwr_tx_decr_req;
    info->pwr_tx_incr_req = p->pwr_tx_incr_req;
    info->pwr_set_cnt = p->pwr_set_cnt;
    info->super_timeout_begin = lc->super_timeout_begin;
    printf("clkoff_drift:%x %x\n", lc->clkoff_drift, info->super_timeout_begin);
    __bt_updata_save_curr_used_frame(lc->frame, info);
}

struct link_connection *RF_CODE __bt_updata_resume_link_connection_info(struct ctrl_frame *frame, u8 role)
{
    struct link_connection *lc = get_link_connection();
    struct link_power *p;
    u8 bits;
    int i;

    if (lc == NULL)
        return NULL;
    link_connection_init(lc);
    printf("role:%\n", UPDATA_BT.bt_updata_role);
    lc->role = role;
    lc->frame = frame;
    lc->last_signed_bitoff = UPDATA_BT.last_signed_bitoff;
    lc->core.remote_sr = 0;
    lc->core.remote_class_dev = 0;
    lc->send_acl_enable |= 1;
    lc->clkoff_drift = UPDATA_BT.last_clkoff_drift;
    lc->adjust_clkoffset_enable = 1;
    p = &lc->power.normal;
    bits = *((u8 *)&UPDATA_BT.pwr_tx_decr_req - 1);
    *(u8 *)p = (*(u8 *)p & ~127) | (bits & 2) | (bits & 1) | (bits & 4) | (bits & 120);
    p->pwr_tx_decr_req = UPDATA_BT.pwr_tx_decr_req;
    p->pwr_tx_incr_req = UPDATA_BT.pwr_tx_incr_req;
    p->pwr_set_cnt = UPDATA_BT.pwr_set_cnt;
    bredr_pwr_set(lc, 0, 15);
    lc->super_timeout = 1280;
    lc->super_timeout_begin = UPDATA_BT.super_timeout_begin;
    for (i = 0; i < 2; i++)
        link_conn_set_rx_bulk_in_irq(lc, i);
    link_task_add(&lc->task);
    __bt_updata_lmp_connection_resume(lc, role);
    return lc;
}

int RF_CODE __bt_updata_connection_resume(u32 time_slice)
{
    struct ctrl_frame *frame = __bt_updata_bd_get_frame_rusume();

    if (frame == NULL)
        return -12;
    puts("__bt_updata_connection_resume\n");
    __bt_updata_resume_link_connection_info(frame, 1);
    __bt_updata_resume_bredr_rx_irq_handler();
    return 1;
}

static int inquiry_scan_resume(struct link_task *task)
{
    struct ctrl_frame *frame;

    if (inquiry_scan == NULL)
        return 0;
    putchar('I');
    frame = inquiry_scan->frame;
    frame->txtog = 0;
    bredr_normal_pwr_set(frame, CONFIG_INQUIRY_SCAN_POWER);
    bredr_bd_frame_enable(frame, 0, 1);
    inquiry_scan->substate = 0;
    inquiry_scan->state = 2;
    return 0;
}

static int inquiry_scan_suspend(struct link_task *task)
{
    struct link_inquiry_scan *scan = inquiry_scan;

    if (scan == NULL || scan->state == 3)
        return 0;
    bredr_bd_frame_disable(scan->frame);
    inquiry_scan->state = 3;
    return 0;
}

static int page_scan_suspend(struct link_task *task)
{
    struct link_page_scan *scan = (struct link_page_scan *)((u8 *)task - offsetof(struct link_page_scan, task));
    u32 clkn;

    if (scan->timer != 0xff) {
        slot_timer_put(scan->timer);
        scan->timer = 0xff;
    }
    scan->state = 3;
    if (scan->rand_enable)
        task->run_slot = ((REG(0x1e3b00) << 5) & 32) + 32;
    if (scan->quick_mode) {
        clkn = scan->remote_clkn + 4;
        scan->remote_clkn = clkn > 19088763 ? 19088752 : clkn;
    }
    if (scan->frame)
        bredr_bd_frame_disable(scan->frame);
    return 0;
}

static void inquiry_timeout()
{
    struct ctrl_frame *frame;
    u32 now, diff;

    if (inquiry == NULL)
        return;
    frame = inquiry->frame;
    now = READ_SLOT_CLK();
    diff = now - inquiry->ab_clkn + (now > inquiry->ab_clkn ? 0 : 0x8000000);
    if (diff > 4096) {
        inquiry->ab_clkn = now;
        inquiry->ab_train = !inquiry->ab_train;
        bredr_frame_set_ab_train(frame, inquiry->ab_train);
    }
    REG(0x1c0000) |= 4;
    inquiry->substate = 0;
    slot_timer_reset(inquiry->timer, 18);
    if (bredr_clkn_after(now, inquiry->timeout))
        inquiry->completed(NULL);
}

static int inquiry_resume(struct link_task *task)
{
    struct ctrl_frame *frame = inquiry->frame;
    u8 timer;

    bredr_normal_pwr_set(frame, CONFIG_INQUIRY_POWER);
    bredr_bd_frame_enable(frame, 0, 1);
    inquiry->state = 2;
    timer = inquiry->timer;
    if (timer == 0xff)
        timer = inquiry->timer = slot_timer_get();
    slot_timer_set(timer, inquiry_timeout, NULL, 18);
    return 0;
}

static int inquiry_suspend(struct link_task *task)
{
    bredr_bd_frame_disable(inquiry->frame);
    inquiry->state = 3;
    slot_timer_put(inquiry->timer);
    inquiry->timer = 0xff;
    return 0;
}

static int page_suspend(struct link_task *task)
{
    struct link_page *page = (struct link_page *)((u8 *)task - offsetof(struct link_page, task));

    page->state = 3;
    if (page->timer != 0xff) {
        slot_timer_put(page->timer);
        page->timer = 0xff;
    }
    if (page->rand_enable)
        task->run_slot = ((REG(0x1e3b00) << 5) & 32) + 32;
    if (page->frame)
        bredr_bd_frame_disable(page->frame);
    return 0;
}

static void role_switch_page_timeout(void *p)
{
    struct link_connection *lc = p;
    struct ctrl_frame *frame = lc->frame;
    struct link_page *page = lc->rs->page;
    u8 tog;
    int i;

    (void)frame->rxtog;                 /* read and unused in the SDK */
    tog = frame->txtog & 1;
    putchar('F');
    __write_reg_clkoffset(frame, 0);
    __put_lt_addr(page->lt_addr);
    page->remote.lt_addr = page->backup_lt_addr;
    bredr_hw_write_lt_addr(frame, page->remote.lt_addr);
    frame->format = 99;
    __write_reg_linkcntl(frame, tog, 16401);
    __write_reg_linkcntl(frame, tog ^ 1, 16385);
    __write_reg_txphdr(frame, tog, 2);
    __write_reg_txphdr(frame, tog ^ 1, 2);
    for (i = 0; i < 2; i++)
        link_conn_set_rx_bulk_in_irq(lc, i);
    frame->bitoff = 0;
    frame->rxtog = 0;
    frame->txtog = 0;
    frame->txbuf0cntl |= 1;
    frame->txbuf1cntl |= 1;
    slot_timer_put(page->timer);
    frame->bufcntl &= ~3840;
    frame->bufcntl |= 256;
    bd_frame_odd_even(frame, 0);
    bt_free(lc->rs->page);
    bt_free(lc->rs);
    lc->rs = NULL;
    lmp_role_switch_completed(lc, 1);
    bredr_link_event(lc, 2, 4, 1);
}

static void role_switch_page_rx_handler(void *p)
{
    struct link_connection *lc = p;
    struct ctrl_frame *frame = lc->frame;
    struct link_page *page = lc->rs->page;
    u16 header;
    u8 index, check;
    int i;

    index = (frame->rxtog & 1) ^ 1;
    (void)frame->txtog;                 /* read and unused in the SDK */
    putchar('x');
    (void)(&frame->rxphdr0)[index];
    header = (&frame->rxheader0)[index];
    check = (&frame->rxstat0)[index] >> 7;
    if (check != 127) {
        printf("check: %x\n", check);
        return;
    }
    if (header & 120) {
        putchar('E');
        return;
    }
    switch (page->substate) {
    case 0:
        REG(0x1c0000) |= 4;
        slot_timer_reset(page->timer, 16);
        page->remote.lt_addr = page->lt_addr;
        page->remote.clk_offset = 0;
        __write_reg_clkoffset(frame, 0);
        __write_reg_bdaddr(frame, page->local_addr);
        __write_reg_bch(frame, page->local_acc);
        frame->miscntl = page->lt_addr << 4;
        windset(frame, 103, 0, 0);
        frame->bitoff = 0;
        bredr_set_frame_bitoff(frame, 0);
        frame->rxtog = 0;
        frame->txtog = 1;
        frame->txbuf0cntl |= 1;
        frame->txbuf1cntl |= 1;
        frame->rxbuf0cntl &= ~1;
        frame->rxbuf1cntl &= ~1;
        frame->linkcntl0 = 1;
        frame->linkcntl1 = 17;
        frame->format = 98;
        frame->bufcntl = 8451;
        putchar('B');
        break;
    case 1:
        frame->rxtog = 0;
        putchar('C');
        break;
    }
    page->substate++;
    lc->rs->rs_timeout_en = 0;
    bredr_hw_write_lt_addr(frame, page->remote.lt_addr);
    if (page->substate <= 1)
        return;
    frame->bufcntl &= ~3840;
    frame->bufcntl |= 256;
    frame->bufcntl &= ~32;
    frame->bufcntl |= 3;
    frame->txbuf0cntl |= 1;
    frame->txbuf1cntl |= 1;
    slot_timer_put(page->timer);
    __put_lt_addr(page->backup_lt_addr);
    bt_free(lc->rs->page);
    bt_free(lc->rs);
    lc->rs = NULL;
    lc->role = 0;
    bredr_frame_set_irq_handler(frame, lc, bredr_frame_conn_rx_irq_handler);
    for (i = 0; i < 2; i++)
        link_conn_set_rx_bulk_in_irq(lc, i);
    lmp_role_switch_completed(lc, 0);
    bredr_link_event(lc, 2, 4, 0);
}

/* Slots to run before the next eSCO instant on a phone link that shares
 * the radio with another link (step 5); the packet dump hook can force the
 * remove-request countdown. */
int RF_CODE bredr_esco_1to2_deal(struct link_connection *lc)
{
    struct bredr_esco_link *esco;
    struct link_connection *p;
    int w_sco, slots, offset, ret;
    u16 flag, adjust;
    u8 cnt;
    u32 now;

    if (lc == NULL || (esco = lc->esco) == NULL)
        return 0;
    w_sco = esco->pdu.W_sco;
    slots = w_sco + 2;
    esco->esco_setp = 5;
    cnt = lc->delay_shedule_cnt;
    if (cnt == 0) {
        slots = w_sco + 4;
        flag = esco->rx_remove_esco_req_flag;
        if (flag) {
            esco->rx_remove_esco_req_flag = --flag;
            slots = flag % 10 == 0 ? w_sco + 2 : w_sco;
            putchar('T');
        }
    } else {
        lc->delay_shedule_cnt = --cnt;
        if (cnt == 52 || get_esco_packet_dump() == 0) {
            if (get_esco_packet_dump() == 0)
                lc->delay_shedule_cnt = 0;
            flag = esco_interrupt_a2dp_shedult_cn + 60;
            lc->esco->rx_remove_esco_req_flag = flag;
            printf("lc->esco->rx_remove_esco_req_flag=%d\n", flag);
            link_other_task_run_slots(&lc->task, lc->esco->pdu.T_sco << 2);
            bredr_link_event(lc, 2, 7, 1);
            return 0;
        }
        putchar('t');
    }
    esco = lc->esco;
    if (esco->pdu.T_sco == 6) {
        if (esco->acl_work_cnt % 5 == 0) {
            esco->esco_setp = 5;
            return 2;
        }
        return slots;
    }
    now = bredr_read_slot_clk();
    FOR_EACH_LINK(p) {
        if (p == lc)
            continue;
        offset = bredr_clkn2offset(p->super_timeout_begin, now);
        esco = lc->esco;
        if (offset > 1600 && esco->pdu.W_sco > 2) {
            if (!esco->Wsco_adjust)
                puts("Audo Decrease Wsco\n");
            adjust = 4;
        } else {
            if (esco->Wsco_adjust)
                puts("Audo Restore Wsco\n");
            adjust = 0;
        }
        lc->esco->Wsco_adjust = adjust;
    }
    esco = lc->esco;
    ret = slots - esco->Wsco_adjust;
    if (!(esco->acl_work_cnt & 1))
        ret -= 2;
    return ret > 2 ? ret : 2;
}

u8 RF_CODE bredr_tws_esco_deal(struct link_connection *lc)
{
    static u8 run_page_scan_cnt RF_DATA;
    struct link_connection *p;
    u8 status, slots;

    if (lc == NULL || lc->esco == NULL)
        return 0;
    slots = lc->esco->pdu.W_sco + 2;
    lc->esco->esco_setp = 4;
    if (tws_esco_transmit_deal(2, lc)) {
        status = 0;
        if (tws_esco_acl_communication(lc, &status))
            lc->esco->esco_setp = 3;
        lc->esco->max_slot_cnt++;
        link_task_run(&lc->task);
        if (lc->esco->max_slot_cnt > 20) {
            lc->esco->max_slot_cnt = 0;
            FOR_EACH_LINK(p) {
                if (CONFIG_BTCTLER_TWS_ENABLE && p->tws_link)
                    continue;
                if (p->state && p->core.max_slot != 1)
                    bredr_link_event(p, 2, 6, 1);
            }
        }
        return slots;
    }
    if (g_quick_page_scan) {
        if (g_quick_page_scan->substate == 0) {
            if (++run_page_scan_cnt > 4) {
                lc->esco->esco_setp = 1;
                run_page_scan_cnt = 0;
            }
            link_task_run(&lc->task);
        }
        return slots;
    }
    if (tws_esco_transmit_deal(1, lc)) {
        lc->esco->esco_setp = 2;
        link_task_run(&lc->task);
    }
    return slots;
}

static void esco_get_time_offset(struct link_connection *lc, s16 *bitoff) RF_CODE __attribute__((noinline));

/* The eSCO T-slot tick (a fine-count slot timer per link). */
void RF_CODE bredr_esco_T_slot_tick(void *_lc)
{
    struct link_connection *lc = _lc;
    struct bredr_esco_link *esco = lc->esco;
    volatile struct bredr_esco_reg *reg;
    struct ctrl_frame *frame;
    s16 bitoff = 0;
    u16 slots;
    u32 clk, next, bit;

    if (esco == NULL)
        return;
    if (lc->phone_rx_esco_nack < 2) {
        lc->phone_rx_esco_nack = 0;
        if (lc->phone_rx_esco_inc_anc)
            lc->phone_rx_esco_inc_anc--;
    }
    frame = lc->frame;
    reg = esco->hw.reg;
    reg->BREDR_ESCO_BITOFF = frame->bitoff;
    if (lc->role) {
        __write_reg_clkoffset(frame, frame->clk_offset);
        frame = lc->frame;
        bredr_set_frame_bitoff(frame, frame->bitoff);
        esco = lc->esco;
        reg = esco->hw.reg;
    }
    esco->esco_setp = 0;
    if (!(reg->BREDR_ESCO_CON & 1)) {
        reg->BREDR_ESCO_BITOFF = lc->frame->bitoff;
        clk = READ_SLOT_CLK() >> 1;
        esco = lc->esco;
        next = esco->esco_next_offset;
        if (clk > next || (next == clk && esco->esco_next_soft_bitoff < REG(0x1c00bc))) {
            slots = 0;
            goto offset;
        }
        bit = 256 << esco->hw.esco_id;
        REG(0x1c0020) |= bit;
        REG(0x1c001c) |= bit;
        esco->hw.reg->BREDR_ESCO_CON |= 1;
        if (CONFIG_BTCTLER_TWS_ENABLE) {
            tws_esco_monitor_init(lc);
            link_task_run(&lc->task);
        }
        slots = 0;
    } else if (CONFIG_BTCTLER_TWS_ENABLE) {
        slots = bredr_tws_esco_deal(lc);
    } else if (link_conn_num_phone() > 1) {
        slots = bredr_esco_1to2_deal(lc);
        if (slots == 0)
            goto offset;
        link_task_run(&lc->task);
    } else {
        slots = 0;
    }
    bredr_bd_frame_enable(lc->frame, 0, 0);
    if (lc->esco->esco_first_rx_data_flag)
        windset(lc->frame, 118, 0, 0);
offset:
    esco_get_time_offset(lc, &bitoff);
    esco = lc->esco;
    if (esco->esco_setp && slots && esco->timer1 != 0xff)
        slot_timer_set(esco->timer1, bredr_esco_step_by_step, lc, slots);
    lc->adjust_clkoffset_enable = 1;
}

/* Arm the T-slot tick for the next eSCO instant and remember where it is. */
static void esco_get_time_offset(struct link_connection *lc, s16 *bitoff)
{
    struct bredr_esco_link *esco = lc->esco;
    struct ctrl_frame *frame;
    u32 local_clk, clkn, mclkn;
    u16 local_bitoff;
    int t_sco, offset;
    s16 soft;
    u32 next;

    local_irq_disable();
    frame = lc->frame;
    preserve_bitoff_cal(frame, frame->bitoff, &clkn, &local_bitoff, 600);
    mclkn = get_mclkn(clkn, local_bitoff, &local_clk);
    t_sco = esco->pdu.T_sco;
    offset = esco->pdu.D_sco - ((esco->pdu.timing_flags & 2 ? mclkn ^ 0x4000000 : mclkn) % t_sco);
    offset += (offset < 1 ? t_sco : 0) + (lc->esco->hw.reg->BREDR_ESCO_CON & 1 ? 0 : t_sco);
    offset += offset < 2 ? t_sco : 0;
    if (lc->esco->timer0 != 0xff) {
        slot_timer_set_finecnt(lc->esco->timer0, bredr_esco_T_slot_tick, lc, local_clk + offset, local_bitoff);
        if (!(lc->esco->hw.reg->BREDR_ESCO_CON & 1)) {
            next = (local_clk + offset) >> 1;
            esco->esco_next_offset = next;
            soft = local_bitoff + 600;
            esco->esco_next_soft_bitoff = soft;
            if (soft > 1249) {
                esco->esco_next_soft_bitoff = local_bitoff - 650;
                esco->esco_next_offset = next + 1;
            }
        }
    }
    local_irq_enable();
    last_bitoff = lc->frame->bitoff;
    last_local_offset = local_clk;
    last_offset = offset;
    *bitoff = local_bitoff;
    last_soft_bitoff = local_bitoff;
}

void RF_CODE esco_debug_fun_check_buf_sta(void *_lc)
{
    struct link_connection *lc = _lc;
    struct bredr_esco_link *esco = lc->esco;
    struct list_head *node, *head;
    struct bredr_rx_bulk *rx;
    u16 num;
    int len;

    local_irq_disable();
    num = len = 0;
    for (node = esco->rx_esco.next; node != &esco->rx_esco; node = node->next) {
        rx = (struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry));
        if (rx->llid == 4) {
            num++;
            len += rx->len;
        }
    }
    printf("phone_esco_rx_len:%d-%d\n", len, num);
    if (CONFIG_BTCTLER_TWS_ENABLE) {
        head = &tws_link.link->esco->rx_esco;
        num = len = 0;
        for (node = head->next; node != head; node = node->next) {
            rx = (struct bredr_rx_bulk *)((u8 *)node - offsetof(struct bredr_rx_bulk, entry));
            if (rx->llid == 4) {
                num++;
                len += rx->len;
            }
        }
        printf("tws_esco_rx_len:%d-%d\n", len, num);
    }
    bredr_rx_bulk_state();
    local_irq_enable();
}

void *RF_CODE bredr_find_esco_packet(void *_lc)
{
    struct link_connection *lc = _lc;
    struct list_head *head, *node;

    if (CONFIG_BTCTLER_TWS_ENABLE && tws_esco_transmit_deal(6, lc))
        goto wait;
    if (lc == NULL || lc->esco == NULL)
        return NULL;
    local_irq_disable();
    head = &lc->esco->rx_esco;
    node = head->next;
    local_irq_enable();
    if (node != head)
        return (u8 *)node - offsetof(struct bredr_rx_bulk, entry);
wait:
    os_sem_pend((OS_SEM *)&esco_sem, 10);
    return NULL;
}

void *RF_CODE bredr_get_esco_packet(void *_lc, int *flag)
{
    struct link_connection *lc = _lc;
    struct bredr_esco_link *esco = lc->esco;
    struct bredr_rx_bulk *rx = NULL;

    if (!CONFIG_BTCTLER_TWS_ENABLE || tws_esco_transmit_deal(6, lc) == 0) {
        local_irq_disable();
        if (lc->esco == NULL) {
            local_irq_enable();
            *flag = 0;
            return NULL;
        }
        rx = bredr_rx_bulk_pop(&esco->rx_esco);
        local_irq_enable();
    }
    if (esco->suspend_rx_data_store == 1) {
        if (rx)
            bredr_rx_bulk_free(rx);
        *flag = -2;
        return NULL;
    }
    if (rx)
        return rx;
    if (esco->esco_dump_cnt)
        *flag = -1;
    return NULL;
}

int RF_CODE bredr_get_esco_hw(struct ctrl_frame *frame, struct bredr_esco_hw *hw)
{
    u8 used = bredr_link_v.esco_used;
    int i;

    for (i = 0; i < 3; i++) {
        if (!(used & 1 << i)) {
            bredr_link_v.esco_used = used | 1 << i;
            hw->reg = (volatile struct bredr_esco_reg *)0x1c00c8 + i;
            hw->esco_id = i;
            hw->frame = &frame->esco[i];
            frame->format &= ~96;
            frame->format |= (i << 5) & 8160;
            printf("esco_hw %d\n", i & 255);
            frame->escorxstat0 = 0;
            frame->escorxstat1 = 0;
            hw->reg->BREDR_ESCO_BITOFF = frame->bitoff;
            return 0;
        }
    }
    return -14;
}

void RF_CODE bredr_put_esco_hw(struct bredr_esco_hw *hw)
{
    u8 id = hw->esco_id;
    u32 bit;

    if (id == 0xff)
        return;
    bit = 256 << id;
    REG(0x1c0018) &= ~bit;
    REG(0x1c0020) |= bit;
    hw->frame->con = 0;
    hw->reg->BREDR_ESCO_CON = 0;
    bredr_link_v.esco_used &= ~(1 << id);
    hw->esco_id = 0xff;
}

void RF_CODE bredr_esco_close_slot_time(struct link_connection *lc)
{
    if (lc == NULL || lc->esco == NULL) {
        printf("no esco link return");
        return;
    }
    puts("bredr_esco_close_slot_time\n");
    lc->adjust_clkoffset_enable = 0;
    lc->esco->esco_first_rx_data_flag = 1;
    tws_link_esco_close_adjust_clkoffset();
}

int os_sem_create(OS_SEM *sem, int count);
static void bredr_esco_get_data(void *_lc) RF_CODE;

void RF_CODE bredr_esco_link_open(struct link_connection *lc, struct lmp_esco_pdu *pdu, int link_type)
{
    const u8 sco_packet_type[3] = { 5, 6, 7 };
    struct ctrl_frame *frame = lc->frame;
    struct bredr_esco_link *esco;
    volatile struct bredr_esco_frame *esco_frame;
    volatile struct bredr_esco_reg *reg;
    s16 bitoff = 0;
    u8 dump, edr, m2s, s2m;
    u32 addr;

    esco = bt_malloc(116);
    SDK_ASSERT(esco != NULL, 5446, "esco");
    lc->esco = esco;
    esco->hw.frame = NULL;
    esco->hw.esco_id = 0xff;
    dump = get_esco_packet_dump();
    lc->delay_shedule_cnt = dump;
    if (dump > 5)
        lc->delay_shedule_cnt = dump - 5;
    else if (dump == 0)
        lc->delay_shedule_cnt = 60;
    lc->phone_rx_esco_nack = 0;
    lc->phone_rx_esco_inc_anc = 0;
    INIT_LIST_HEAD(&esco->rx_esco);
    INIT_LIST_HEAD(&esco->tx_esco);
    bredr_link_v.esco_last_seqn = 85;
    os_sem_create((OS_SEM *)&esco_sem, 0);
    esco->slot = 1;
    esco->timer0 = slot_timer_get();
    esco->timer1 = slot_timer_get();
    esco->link_type = link_type;
    esco->esco_dump_cnt = get_esco_packet_dump();
    esco->phone_lc = NULL;
    memcpy(&esco->pdu, &pdu->handle, 14);
    edr = pdu->packet_type_s2m > 13;
    if (tws_link.link == NULL)
        link_conn_qos_disable(lc);
    printf("link_type: %d,w_sco:%d,T_sco=%d,dump_cnt%d\n", link_type, pdu->W_sco, pdu->T_sco, esco->esco_dump_cnt);
    if (link_type == 0) {
        pdu->lt_addr = bredr_hw_read_lt_addr(lc->frame);
        m2s = sco_packet_type[pdu->packet_type_m2s];
        s2m = sco_packet_type[pdu->packet_type_s2m];
    } else {
        m2s = bredr_get_esco_packet_type(pdu->packet_type_m2s);
        s2m = bredr_get_esco_packet_type(pdu->packet_type_s2m);
        printf("pcket_type: %d, %d, %d, %d\n", m2s, s2m, pdu->packet_len_m2s, pdu->packet_len_s2m);
    }
    esco->rx_bulk[0] = bredr_rx_bulk_alloc(lc->index, pdu->packet_len_m2s);
    esco->rx_bulk[1] = bredr_rx_bulk_alloc(lc->index, pdu->packet_len_m2s);
    esco->tx_bulk[0] = bredr_tx_bulk_alloc(pdu->packet_len_s2m);
    esco->tx_bulk[1] = bredr_tx_bulk_alloc(pdu->packet_len_s2m);
    memset(esco->tx_bulk[0]->data, 0xaa, pdu->packet_len_s2m);
    memset(esco->tx_bulk[1]->data, 0xaa, pdu->packet_len_s2m);
    lc->adjust_clkoffset_enable = 0;
    bredr_get_esco_hw(frame, &esco->hw);
    printf("lt_addr: %d, esco_id: %d\n", pdu->lt_addr, esco->hw.esco_id);
    esco->get_data = bredr_esco_get_data;
    esco_frame = esco->hw.frame;
    esco_frame->con = edr << 12 | (link_type == 0) << 3 | edr << 13 | s2m << 4 | m2s << 8 | pdu->lt_addr;
    addr = (u32)esco->rx_bulk[0]->data;
    esco_frame->rxptrl0 = addr;
    esco_frame->rxptrh0 = addr >> 16;
    addr = (u32)esco->rx_bulk[1]->data;
    esco_frame->rxptrl1 = addr;
    esco_frame->rxptrh1 = addr >> 16;
    esco_frame->rxlen = pdu->packet_len_m2s;
    addr = (u32)esco->tx_bulk[0]->data;
    esco_frame->txptrl0 = addr;
    esco_frame->txptrh0 = addr >> 16;
    addr = (u32)esco->tx_bulk[1]->data;
    esco_frame->txptrl1 = addr;
    esco_frame->txptrh1 = addr >> 16;
    esco_frame->txlen = pdu->packet_len_s2m;
    reg = esco->hw.reg;
    reg->BREDR_ESCO_INTERVAL = pdu->T_sco * 2 - 1;
    reg->BREDR_ESCO_CNT = (pdu->W_sco * 2 + 3) | 192;
    reg->BREDR_ESCO_CON = 32768;
    bd_set_prio(frame, 29, 0);
    set_btagc_pwr_sel(0);
    lc->esco->max_slot_cnt = 20;
    esco->hw.reg->BREDR_ESCO_BITOFF = lc->frame->bitoff;
    esco_get_time_offset(lc, &bitoff);
    lc->esco->esco_first_rx_data_flag = 0;
    if (CONFIG_BTCTLER_TWS_ENABLE && bredr_esco_tws_link_open(lc, 1))
        lc->esco->esco_first_rx_data_flag = 1;
}

/* The eSCO RX interrupt work: take the received buffer, re-arm both
 * hardware buffers (mSBC mute frames when nothing is queued) and queue the
 * packet for the codec. */
static void bredr_esco_get_data(void *_lc)
{
    static u8 seqN RF_DATA;
    static u8 last_ind RF_DATA = 0xff;
    struct link_connection *lc = _lc;
    struct bredr_esco_link *esco = lc->esco;
    struct bredr_rx_bulk *bulk;
    struct bredr_tx_bulk *tx;
    u32 addr, clk;
    u16 head;
    u8 flag = 0, f, ind;
    int tws;

    if (esco == NULL)
        return;
    lc->frame->txtog &= ~32;
    ind = get_esco_buf_index(lc, &flag);
    f = flag;
    if (!(f & 2) && esco->esco_first_rx_data_flag)
        esco->esco_first_rx_data_flag = 0;
    if (tws_link.link && !tws_link.link->role && lc->esco_tws_nack_flag) {
        f |= 2;
        flag = f;
        lc->esco_tws_nack_flag = 0;
    }
    bulk = esco->rx_bulk[ind];
    if ((u32)link_conn_rx_bulk_remain_size(lc, esco->pdu.packet_len_m2s) < (u32)(esco->pdu.packet_len_m2s << 2)) {
        puts("\nwaring_get_remain_alloc_bulk_full_persent_size\n");
        lmp_private_clear_sco_packet(4);
    }
    esco->acl_work_cnt++;
    esco->rx_bulk[ind] = bredr_rx_bulk_alloc(lc->index, esco->pdu.packet_len_m2s);
    if (esco->rx_bulk[ind] == NULL)
        esco_debug_fun_check_buf_sta(lc);
    SDK_ASSERT(esco->rx_bulk[ind] != NULL, 5172, "esco->rx_bulk[ind]");
    if (ind == 0) {
        addr = (u32)esco->rx_bulk[0]->data;
        esco->hw.frame->rxptrl0 = addr;
        esco->hw.frame->rxptrh0 = addr >> 16;
    } else {
        addr = (u32)esco->rx_bulk[1]->data;
        esco->hw.frame->rxptrl1 = addr;
        esco->hw.frame->rxptrh1 = addr >> 16;
    }
    tx = bredr_tx_bulk_pop(&esco->tx_esco);
    if (tx) {
        bredr_tx_bulk_free(esco->tx_bulk[ind]);
        esco->tx_bulk[ind] = tx;
        list_del(&tx->entry);
    } else if (esco->pdu.air_mode == 2) {
        memset(esco->tx_bulk[ind]->data, 0xaa, esco->pdu.packet_len_s2m);
    } else {
        head = seq_num[seqN] << 12 | 2049;
        seqN = seqN + 1 > 3 ? 0 : seqN + 1;
        switch (esco->pdu.packet_len_s2m) {
        case 60:
            *(u16 *)esco->tx_bulk[ind]->data = head;
            memcpy(&esco->tx_bulk[ind]->data[2], msbc_mute_data, esco->pdu.packet_len_s2m - 2);
            break;
        case 30:
            *(u16 *)esco->tx_bulk[0]->data = head;
            memcpy(&esco->tx_bulk[0]->data[2], msbc_mute_data, esco->pdu.packet_len_s2m - 2);
            memcpy(esco->tx_bulk[1]->data, msbc_mute_data + 28, esco->pdu.packet_len_s2m);
            break;
        }
    }
    if (ind == 0) {
        addr = (u32)esco->tx_bulk[0]->data;
        esco->hw.frame->txptrl0 = addr;
        esco->hw.frame->txptrh0 = addr >> 16;
    } else {
        addr = (u32)esco->tx_bulk[1]->data;
        esco->hw.frame->txptrl1 = addr;
        esco->hw.frame->txptrh1 = addr >> 16;
    }
    bulk->llid = 4;
    bulk->len = esco->pdu.packet_len_m2s;
    bulk->payload_head[0] = 0;
    bulk->payload_head[1] = 0;
    bulk->payload_head[2] = 0;
    bulk->payload_head[3] = bulk->len;
    clk = bredr_get_master_slot_clk(lc);
    bulk->hash = clk;
    SDK_ASSERT(bulk->len == esco->pdu.packet_len_s2m, 5219, "(bulk->len == esco->pdu.packet_len_s2m)");
    if (ind == last_ind) {
        puts("why esco_idx_err");
        f |= 1;
        flag = f;
    }
    last_ind = ind;
    if ((f & 2) && tws_link.link)
        putchar('+');
    tws = CONFIG_BTCTLER_TWS_ENABLE;
    if (tws && f) {
        if (esco->pdu.T_sco > 6 && tws_esco_transmit_deal(2, lc) && !tws_esco_transmit_deal(10, lc)) {
            esco->rsvd_state = 0xaa;
            bulk->payload_head[2] = f;
        } else {
            esco->rsvd_state = 0;
        }
    }
    if (f & 1) {
        if (tws_link.link)
            putchar('#');
        bulk->payload_head[2] |= 32;
    } else {
        esco->rx_esco_num++;
    }
    esco->max_slot_cnt++;
    if (esco->suspend_rx_data_store) {
        bredr_rx_bulk_free(bulk);
        esco->rsvd_state = 0;
    } else if (esco->esco_dump_cnt) {
        esco->esco_dump_cnt--;
        bredr_rx_bulk_free(bulk);
        esco->rsvd_state = 0;
        if (get_esco_packet_dump() == 0) {
            esco->esco_dump_cnt = 0;
            goto dump_done;
        }
        if (esco->esco_dump_cnt == 0) {
dump_done:
            /* SDK quirk: reads the freed bulk's hash. */
            printf("rx_begin=%d,%d\n", get_esco_packet_dump(), bulk->hash);
        }
    } else {
        esco->esco_dump_cnt = 0;
        bredr_rx_bulk_push(&esco->rx_esco, bulk);
    }
    if (tws) {
        if (esco->rsvd_state == 0xaa)
            tws_esco_transmit_deal(5, lc);
        if (!tws_esco_transmit_deal(2, lc) || tws_esco_transmit_deal(10, lc))
            tws_clear_timeout_tr_esco_packet(lc, 0);
    }
    os_sem_post((OS_SEM *)&esco_sem);
    REG(0x1c013c) &= ~(1 << lc->esco->hw.esco_id);
    if (lc->role) {
        __write_reg_clkoffset(lc->frame, lc->frame->clk_offset);
        bredr_set_frame_bitoff(lc->frame, lc->frame->bitoff);
    }
    lc->esco->hw.reg->BREDR_ESCO_BITOFF = lc->frame->bitoff;
    if (tws)
        tws_esco_role_switch_post(NULL);
}
