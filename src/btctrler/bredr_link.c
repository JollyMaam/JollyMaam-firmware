/* The SDK's bredr_link.c: the Classic link layer between the link manager
 * (LMP, owned) and the baseband (bredr_frame.c).  It runs the page, page
 * scan, inquiry and inquiry scan procedures and each connection as link
 * tasks (link_task.c), moves ACL and LMP packets between the frame buffers
 * and the rx/tx bulk queues in the baseband interrupt, keeps the clock
 * offset and bit offset tracked, handles AFH, power control, QoS polling,
 * role switch and sniff anchors.
 *
 * Kept as the SDK has it unless a comment says otherwise.  Not owned (no
 * caller anywhere): eSCO data paths, TWS forwarding, the fixed rx/tx test
 * mode, the firmware-update hand-over and debug printers; see the leftover
 * list in recover.py. */
#include "btctrler/glue.h"

#include <stdint.h>
#include <string.h>

#ifdef UNITTEST
/* Host tests: the SDK's allocation sizes are for 32-bit pointers. */
#include <stdlib.h>
void *bredr_link_test_malloc(unsigned long size);
void bredr_link_test_free(void *ptr);
#define malloc bredr_link_test_malloc
#define free bredr_link_test_free
#else
/* The literal allocation and clear sizes below are the SDK's. */
_Static_assert(sizeof(struct link_connection) == 728, "SDK link_connection size");
_Static_assert(sizeof(struct link_inquiry_scan) == 304, "SDK link_inquiry_scan size");
_Static_assert(sizeof(struct link_page_scan) == 120, "SDK link_page_scan size");
_Static_assert(sizeof(struct link_inquiry) == 192, "SDK link_inquiry size");
_Static_assert(sizeof(struct link_page) == 168, "SDK link_page size");
_Static_assert(sizeof(struct link_tws_ble) == 68, "SDK link_tws_ble size");
_Static_assert(sizeof(struct link_fix_rxtx) == 132, "SDK link_fix_rxtx size");
_Static_assert(sizeof(struct bredr_role_switch) == 20, "SDK bredr_role_switch size");
_Static_assert(sizeof(struct inquiry_eir) == 16, "SDK inquiry_eir size");
_Static_assert(sizeof(struct bredr_remote_device) == 36, "SDK bredr_remote_device size");
_Static_assert(offsetof(struct bredr_tx_bulk, data) == 28, "SDK bredr_tx_bulk header size");
_Static_assert(offsetof(struct link_inquiry, result_num) == offsetof(struct link_inquiry, result_crc) + 60,
               "result_num follows result_crc[30]");
_Static_assert(sizeof(struct tws_conn_info) == 31, "SDK tws_conn_info size");
#endif

#define RF_CODE   __attribute__((section(".classic_rf_code")))
#define RAM_CODE  __attribute__((section(".volatile_ram_code")))
#define RF_DATA   __attribute__((section(".classic_rf_data")))
#define RF_BSS    __attribute__((section(".classic_rf_bss")))
#define RF_CONST  __attribute__((section(".classic_rf_const")))

#ifdef UNITTEST
static volatile u32 test_regs[0x400];
/* The slot timer's busy flag (0x1c001c) reads back clear on the host. */
static volatile u32 *test_reg(u32 a)
{
    if (a == 0x1c001c) test_regs[0x1c / 4] = 0;
    return &test_regs[(a & 0xfff) / 4];
}
#define REG32(a) (*test_reg(a))
#else
#define REG32(a) (*(volatile u32 *)(a))
#endif
#define PHY_TO_RADIO(p) ((u32)(uintptr_t)(p) - REG32(0x1cfc44))

void log_print(int level, const char *tag, const char *format, ...);
void usr_timer_del(u16 id);
void printf_buf(const u8 *buf, u32 len);
extern const u8 log_tag_const_i_BL, log_tag_const_d_BL, log_tag_const_e_BL;

/* ---- state ---------------------------------------------------------------- */

struct bredr_link_var bredr_link_v RF_BSS;
StaticQueue_t esco_sem RF_BSS;

static struct link_page *page RF_DATA;
static struct link_page_scan *page_scan RF_DATA;
static struct link_inquiry *inquiry RF_DATA;
static struct link_inquiry_scan *inquiry_scan RF_DATA;
static volatile u8 page_disable_active RF_DATA;
static volatile u8 inquiry_disable_active RF_DATA;
static volatile u8 ps_disable_active RF_DATA;
static volatile u8 inq_scan_disable_active RF_DATA;
static struct link_tws_ble *tws_ble RF_DATA;
static struct link_inquiry_scan *tws_inquiry_scan RF_DATA;
static u32 page_inquiry_clkn RF_DATA;
static u8 packet_table_support_en RF_DATA = 0xff;
static struct link_fix_rxtx *fix_txrx RF_DATA;

/* GIAC (0x9e8b33) and its access-code parity. */
static const u8 local_lap[6] RF_CONST = { 0x33, 0x8b, 0x9e, 0, 0, 0 };
static const u8 local_bch[5] RF_CONST = { 0xe2, 0x3a, 0x1a, 0x33, 0x02 };
static const u8 ble_local_lap[6] RF_CONST = { 0xaa, 0xaa, 0xaa, 0, 0, 0 };
static const u8 ble_local_bch[5] RF_CONST = { 0x55, 0x55, 0x55, 0x55, 0x55 };
const char offset_step_tb[9] RF_CONST = { 0x88, 0xb0, 0xd8, 0xec, 0x00, 0x14, 0x28, 0x50, 0x88 };
static const u16 ab_train_table[3] RF_CONST = { 1, 128, 256 };
/* { max payload, packet type, slots }. */
static const u16 packet_2M_table[3][3] RF_CONST = { { 54, 4, 1 }, { 367, 10, 3 }, { 679, 14, 5 } };
static const u16 packet_1M_table[6][3] RF_CONST = {
    { 17, 3, 1 }, { 27, 4, 1 }, { 121, 10, 3 }, { 183, 11, 3 }, { 224, 14, 5 }, { 339, 15, 5 },
};
const u16 pwr_tb[4] RF_CONST = { 10, 100, 1000, 5000 };
const u8 dec_fast_tb[8] RF_CONST = { 0xfa, 0xc8, 0x96, 0x64, 0x50, 0x3c, 0x32, 0x02 };
const u8 channel_delay[3] RF_CONST = { 4, 2, 0 };
static const u16 iut_aclsco_table[10][2] RF_CONST = {
    { 10, 8 }, { 10, 5 }, { 17, 3 }, { 20, 6 }, { 27, 4 }, { 30, 7 }, { 121, 10 }, { 183, 11 }, { 224, 14 }, { 339, 15 },
};
static const u16 iut_edracl_table[6][2] RF_CONST = {
    { 54, 4 }, { 83, 8 }, { 367, 10 }, { 552, 11 }, { 679, 14 }, { 1021, 15 },
};
static const u8 packet_table[9] RF_CONST = { 0x04, 0x0b, 0x0f, 0x24, 0x2a, 0x2e, 0x28, 0x2b, 0x2f };

static int inquiry_scan_resume(struct link_task *task);
static int inquiry_scan_suspend(struct link_task *task);
static int page_scan_resume(struct link_task *task);
static int page_scan_suspend(struct link_task *task);
static int inquiry_resume(struct link_task *task);
static int inquiry_suspend(struct link_task *task);
static int page_resume(struct link_task *task);
static int page_suspend(struct link_task *task);
int link_conn_task_resume(struct link_task *task);
int link_conn_task_suspend(struct link_task *task);
static int tws_inquiry_scan_resume(struct link_task *task);
static int tws_inquiry_scan_suspend(struct link_task *task);
static int fix_txrx_resume(struct link_task *task);
static int fix_txrx_suspend(struct link_task *task);
static void __timer_register(int slot);
static void __timer_remove(void);
static u16 frame_bitoff_adjust(struct ctrl_frame *frame);
static void page_scan_step_2(void);
static void tws_bredr_master_connect_enable(void);

static const struct link_task_ops inquiry_scan_task_ops RF_CONST = { inquiry_scan_resume, inquiry_scan_suspend };
static const struct link_task_ops page_scan_task_ops RF_CONST = { page_scan_resume, page_scan_suspend };
static const struct link_task_ops inquiry_task_ops RF_CONST = { inquiry_resume, inquiry_suspend };
static const struct link_task_ops page_task_ops RF_CONST = { page_resume, page_suspend };
static const struct link_task_ops conn_task_ops RF_CONST = { link_conn_task_resume, link_conn_task_suspend };
static const struct link_task_ops tws_inquiry_scan_task_ops RF_CONST = { tws_inquiry_scan_resume, tws_inquiry_scan_suspend };
static const struct link_task_ops fix_txrx_task_ops RF_CONST = { fix_txrx_resume, fix_txrx_suspend };

static RF_CODE void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

static RF_CODE void list_add_tail(struct list_head *new, struct list_head *head)
{
    struct list_head *prev = head->prev;
    head->prev = new;
    new->next = head;
    new->prev = prev;
    prev->next = new;
}

static RF_CODE void __list_del_entry(struct list_head *entry)
{
    struct list_head *prev = entry->prev, *next = entry->next;
    next->prev = prev;
    prev->next = next;
}

/* Unlink and point the node at itself. */
static RF_CODE void list_del(struct list_head *entry)
{
    struct list_head *prev = entry->prev, *next = entry->next;
    next->prev = prev;
    prev->next = next;
    entry->next = entry;
    entry->prev = entry;
}

static RF_CODE int list_empty(const struct list_head *head)
{
    return head->next == head;
}

#define FOR_EACH_LINK(lc) \
    for (struct list_head *n_ = bredr_link_v.link_conn_head.next; \
         n_ != &bredr_link_v.link_conn_head && ((lc) = (struct link_connection *)n_, 1); n_ = n_->next)

/* ---- basics --------------------------------------------------------------- */

RF_CODE void DEBUG_SINGAL_TWS_CONNECTION(u8 x)
{
    (void)x;
}

__attribute__((weak)) RF_CODE u8 get_esco_packet_dump(void)
{
    return 120;
}

RF_CODE u8 get_bredr_link_state(void)
{
    return bredr_link_v.bredr_link_state;
}

RF_CODE u8 get_link_bredr_id(struct link_connection *lc)
{
    return lc->index;
}

RF_CODE void bredr_link_close(void)
{
    local_irq_disable();
    if (bredr_link_v.super_timerout_timer) {
        usr_timer_del(bredr_link_v.super_timerout_timer);
        bredr_link_v.super_timerout_timer = 0;
    }
    local_irq_enable();
}

RF_CODE void bredr_link_init(void)
{
    bredr_link_v.glt_addr = 0;
    bredr_link_v.super_timerout_timer = 0;
    bredr_link_v.remote_device_state = 0;
    page = 0;
    page_scan = 0;
    inquiry = 0;
    inquiry_scan = 0;
    page_disable_active = 0;
    inquiry_disable_active = 0;
    ps_disable_active = 0;
    inq_scan_disable_active = 0;
    INIT_LIST_HEAD(&bredr_link_v.link_conn_head);
}

RF_CODE void bredr_pll_comp_ext_reset(void)
{
    struct link_connection *lc;
    if (!bd_base.bredr_running) return;
    FOR_EACH_LINK(lc)
        if (lc->frame) bredr_pll_comp_reset(lc->frame);
}

RF_CODE u8 bredr_link_check_used(void)
{
    return tws_ble || inquiry_scan || inquiry || page_scan || page;
}

RF_CODE void bredr_link_reset_power(void)
{
    struct link_connection *lc;
    if (!bd_base.bredr_running) return;
    if (tws_ble && tws_ble->frame) bredr_normal_pwr_set(tws_ble->frame, 15);
    if (inquiry_scan && inquiry_scan->frame) bredr_normal_pwr_set(inquiry_scan->frame, 15);
    if (page_scan && page_scan->frame) bredr_normal_pwr_set(page_scan->frame, 15);
    if (page && page->frame) bredr_normal_pwr_set(page->frame, 15);
    if (inquiry && inquiry->frame) bredr_normal_pwr_set(inquiry->frame, 15);
    if (tws_inquiry_scan && tws_inquiry_scan->frame) bredr_normal_pwr_set(tws_inquiry_scan->frame, 15);
    FOR_EACH_LINK(lc)
        if (lc->frame) bredr_pwr_set(lc, 0, 15);
}

/* Transmit power: the fixed level if set, capped at the maximum. */
RF_CODE void bredr_normal_pwr_set(struct ctrl_frame *frame, u8 pwr_step)
{
    u8 step = rf.bredr_fix_pwr == 0xff ? pwr_step : rf.bredr_fix_pwr;
    if (step > bt_get_pwr_max_level()) step = bt_get_pwr_max_level();
    frame->TXPWER = bt_get_txpwr_tb(step);
    frame->TXSET = bt_get_txset_tb(step);
}

/* A link's power step: set (sel 0) or moved by inc, clamped to 0..max and
 * below bredr_pwr_max; a fixed level wins.  Returns 1 at the floor, 2 at a
 * ceiling or when fixed, else 0. */
RF_CODE int bredr_pwr_set(struct link_connection *lc, char sel, char inc)
{
    s8 *pwr = &lc->core.pwr_set;
    int ret = 0;
    *pwr = sel ? (s8)(*pwr + (s8)inc) : (s8)inc;
    s8 v;
    if (*pwr < 0) {
        *pwr = 0;
        v = 0;
        ret = 1;
    } else if ((int)*pwr > (int)bt_get_pwr_max_level()) {
        v = (s8)bt_get_pwr_max_level();
        *pwr = v;
        ret = 2;
    } else {
        v = *pwr;
    }
    u8 cap = rf.bredr_pwr_max & 15;
    if (!((int)v < (int)cap)) {
        *pwr = (s8)cap;
        v = (s8)cap;
        ret = 2;
    }
    if (rf.bredr_fix_pwr != 0xff) {
        *pwr = (s8)rf.bredr_fix_pwr;
        v = (s8)rf.bredr_fix_pwr;
        ret = 2;
    }
    lc->frame->TXPWER = bt_get_txpwr_tb((u8)v);
    lc->frame->TXSET = bt_get_txset_tb((u8)*pwr);
    return ret;
}

RF_CODE void bredr_link_check_frame_used(void)
{
    struct link_connection *lc;
    u8 dbg = log_tag_const_d_BL;
    if (dbg) log_print(1, NULL, "[Debug]: [BL]check frame \n\r\n");
    if (tws_ble && tws_ble->frame && dbg) log_print(1, NULL, "[Debug]: [BL]tws_ble \n\r\n");
    if (tws_inquiry_scan && tws_inquiry_scan->frame && dbg) log_print(1, NULL, "[Debug]: [BL]tws_inquiry_scan \n\r\n");
    if (inquiry_scan && inquiry_scan->frame && dbg) log_print(1, NULL, "[Debug]: [BL]inquiry_scan\n\r\n");
    if (page_scan && page_scan->frame && dbg) log_print(1, NULL, "[Debug]: [BL]page_scan\n\r\n");
    if (page && page->frame && dbg) log_print(1, NULL, "[Debug]: [BL]page\n\r\n");
    if (inquiry && inquiry->frame && dbg) log_print(1, NULL, "[Debug]: [BL]inquiry\n\r\n");
    FOR_EACH_LINK(lc)
        if (lc->frame && dbg) log_print(1, NULL, "[Debug]: [BL]conn\n\r\n");
}

/* ---- FHS packets ---------------------------------------------------------- */

RF_CODE void __read_fhs_packet(struct bredr_remote_device *remote, u8 *fhs)
{
    remote->access_addr[0] = fhs[0];
    remote->access_addr[1] = fhs[1];
    remote->access_addr[2] = fhs[2];
    remote->access_addr[3] = fhs[3];
    remote->access_addr[4] = fhs[4] & 3;
    remote->addr[0] = (u8)(fhs[5] << 6 | fhs[4] >> 2);
    remote->addr[1] = (u8)(fhs[6] << 6 | fhs[5] >> 2);
    remote->addr[2] = (u8)(fhs[7] << 6 | fhs[6] >> 2);
    remote->eir = (fhs[7] >> 2) & 1;
    remote->sr = (fhs[7] >> 4) & 3;
    remote->addr[3] = fhs[8];
    remote->addr[4] = fhs[9];
    remote->addr[5] = fhs[10];
    remote->class_dev = (u32)fhs[13] << 16 | (u32)fhs[12] << 8 | fhs[11];
    remote->lt_addr = fhs[14] & 7;
    remote->clkn = (((u32)(fhs[17] & 31) << 21 | (u32)fhs[16] << 13 | (u32)fhs[15] << 5 | fhs[14] >> 3) << 1);
}

extern BTCTRLER_CONFIG int config_btctler_eir_version_info_len;
extern BTCTRLER_CONFIG int config_btctler_hci_standard;
extern BTCTRLER_CONFIG int config_btctler_mode;
extern BTCTRLER_CONFIG int CONFIG_BTCTLER_TWS_ENABLE;
extern u8 __VERSION_BEGIN[];
u16 CRC16(const void *ptr, u32 len);
void *malloc(unsigned long size);
void free(void *ptr);
int printf(const char *format, ...);

static RF_CODE void __write_fhs_packet(u8 *mac_addr, int class_dev, const struct bredr_local_eir *eir, u8 lt_addr, u8 *fhs)
{
    u8 acc[5];
    __get_access_addr(acc, mac_addr);
    fhs[0] = acc[0];
    fhs[1] = acc[1];
    fhs[2] = acc[2];
    fhs[3] = acc[3];
    fhs[4] = (u8)(mac_addr[0] << 2 | (acc[4] & 3));
    fhs[5] = (u8)(mac_addr[1] << 2 | mac_addr[0] >> 6);
    fhs[6] = (u8)(mac_addr[2] << 2 | mac_addr[1] >> 6);
    fhs[7] = (u8)((mac_addr[2] >> 6 | eir->enable << 2 | eir->sr << 4) | 0x80);
    fhs[8] = mac_addr[3];
    fhs[9] = mac_addr[4];
    fhs[10] = mac_addr[5];
    fhs[11] = (u8)class_dev;
    fhs[12] = (u8)(class_dev >> 8);
    fhs[13] = (u8)(class_dev >> 16);
    fhs[14] = lt_addr & 7;
    fhs[15] = 0;
    fhs[16] = 0;
    fhs[17] = 0;
}

/* Let the idle task run only when no connected link is in sniff transition
 * (sniff_sta); returns what link_task_idle_task_enable_detect said. */
RF_CODE bool link_idle_task_enable_detect(void)
{
    struct link_connection *lc;
    local_irq_disable();
    FOR_EACH_LINK(lc) {
        if (lc->frame && !lc->sniff_sta) {
            local_irq_enable();
            return 0;
        }
    }
    u8 enable = link_task_idle_task_enable_detect();
    local_irq_enable();
    if (enable) link_task_idle_enable();
    return enable;
}

/* ---- EIR ------------------------------------------------------------------ */

__attribute__((weak)) RF_CODE u16 eir_fill_version_info(u8 *data, u16 allow_len)
{
    int len = config_btctler_eir_version_info_len;
    if (!len) return 0;
    int room = (int)allow_len - 4;
    int n = room > len ? len : room;
    memcpy(data + 4, __VERSION_BEGIN + 1, (u32)n & 0xffff);
    data[0] = (u8)(n + 3);
    data[1] = 0xfe;
    u16 crc = CRC16(data + 4, (u32)n & 0xffff);
    data[2] = (u8)crc;
    data[3] = (u8)(crc >> 8);
    printf("allow_len:%x ver_actual_len:%x\n", allow_len, len + 4);
    printf("version_str:%x %s \n", __VERSION_BEGIN, __VERSION_BEGIN + 1);
    return (u16)(n + 4);
}

__attribute__((weak)) RF_CODE u8 update_eir_data_hook(u8 *eir_data, u8 len)
{
    (void)eir_data;
    return len;
}

__attribute__((weak)) RF_CODE u8 add_data_2_eir(u8 *eir)
{
    (void)eir;
    return 0;
}

/* Build the EIR: local name, a 16-byte private version block, the version
 * info, then the application's data; pick the smallest DM packet for it. */
RF_CODE void update_eir_data(u8 *eir_data)
{
    u32 name_len = strlen((const char *)gbredr_local_dev.local_name);
    u32 len = (name_len + 1) & 0xffff;
    if (!(len < 33)) btctrler_assert_failed("name_len <= sizeof(gbredr_local_dev.local_name)", 0);
    if (!gbredr_local_dev.eir.enable) return;
    memset(&gbredr_local_dev.eir, 0, 8);         /* one 8-byte store in the SDK */
    gbredr_local_dev.eir.enable = 1;
    gbredr_local_dev.eir.sr = 1;
    gbredr_local_dev.eir.data = eir_data;
    eir_data[0] = (u8)(name_len + 2);
    eir_data[1] = 9;
    u8 *p = eir_data + 2;
    memcpy(p, gbredr_local_dev.local_name, len);
    p += len;
    *p++ = 17;
    *p++ = 0xff;
    memcpy(p, gbredr_local_dev.priv_version, 16);
    p += 16;
    p += eir_fill_version_info(p, (u16)(240 - (p - gbredr_local_dev.eir.data)));
    p += add_data_2_eir(p);
    u8 *d = gbredr_local_dev.eir.data;
    gbredr_local_dev.eir.len = (u8)(p - d);
    update_eir_data_hook(d, gbredr_local_dev.eir.len);
    if (!(gbredr_local_dev.eir.len < 240)) btctrler_assert_failed("eir->len < MAX_EIR_LEN", 0);
    u8 l = gbredr_local_dev.eir.len;
    gbredr_local_dev.eir.type = l < 17 ? 3 : (l < 121 ? 10 : 14);
}

/* ---- inquiry scan --------------------------------------------------------- */

RF_CODE int __inquiry_scan_enable(int clk_base, u8 *addr, struct bredr_local_eir *eir)
{
    if (inquiry_scan) return -22;
    if (log_tag_const_i_BL) {
        log_print(2, NULL, "[Info]: [BL]link_inquiry_scan_enable\n\r\n");
        printf_buf(addr, 6);
    }
    struct ctrl_frame *frame = bredr_bd_get_frame();
    if (!frame) return -12;
    inquiry_scan = bt_malloc(304);
    if (!inquiry_scan) {
        bredr_bd_put_frame(frame);
        return -12;
    }
    page_inquiry_clkn = 0;
    frame->miscntl = 0;
    frame->wincntl = (u16)-32734;
    frame->bitoff = 0;
    __write_reg_clkoffset(frame, 0);
    __write_reg_bdaddr(frame, local_lap);
    __write_reg_bch(frame, local_bch);
    memcpy(inquiry_scan->eir_data, eir->data, eir->len);
    if (eir->enable) {
        __write_reg_txericntl(eir->len, eir->type, inquiry_scan->eir_data);
        __write_reg_txphdr(frame, 0, (u16)(eir->len << 3 | 3));
        __write_reg_txphdr(frame, 1, (u16)(eir->len << 3 | 3));
    }
    __write_fhs_packet(addr, gbredr_local_dev.dev_class, eir, 0, inquiry_scan->fhs);
    __write_reg_txptr(frame, 0, inquiry_scan->fhs);
    __write_reg_txptr(frame, 1, inquiry_scan->fhs);
    __write_reg_format(frame, inquiry_scan->ab_train, 11);
    __write_reg_linkcntl(frame, 0, eir->enable << 8 | 16386);
    __write_reg_linkcntl(frame, 1, eir->enable << 8 | 16402);
    struct link_inquiry_scan *s = inquiry_scan;
    s->substate = 0;
    s->clk_offset = (u32)clk_base;
    s->frame = frame;
    s->state = 1;
    s->task.type = 2;
    s->task.run_slot = 26;
    s->task.run_num = 1;
    s->task.priority = 1;
    s->task.ops = &inquiry_scan_task_ops;
    s->task.sta_attribute = 1;
    bredr_link_v.bredr_link_state |= 8;
    link_task_add(&s->task);
    link_idle_task_enable_detect();
    return 0;
}

/* Changed: the SDK frees an uninitialised pointer when it built no EIR (HCI
 * standard mode with the Classic module); our build's constants made LTO drop
 * that free, so only an EIR this function allocated is freed. */
RF_CODE int link_inquiry_scan_enable(int clk_base)
{
    u8 *eir_data = 0;
    if (!config_btctler_hci_standard || !(config_btctler_mode & 1)) {
        eir_data = malloc(240);
        update_eir_data(eir_data);
    }
    int ret = __inquiry_scan_enable(clk_base, hci_param.addr, &gbredr_local_dev.eir);
    if (eir_data) free(eir_data);
    return ret & 255;
}

static RF_CODE void inquiry_scan_disable(void)
{
    struct link_inquiry_scan *s = inquiry_scan;
    if (!s || inq_scan_disable_active) return;
    inq_scan_disable_active = 1;
    link_task_del(&s->task);
    link_idle_task_enable_detect();
    bredr_link_v.bredr_link_state &= ~8;
    bredr_bd_put_frame(inquiry_scan->frame);
    bt_free(inquiry_scan);
    inquiry_scan = 0;
    inq_scan_disable_active = 0;
}

RF_CODE void link_inquiry_scan_disable(void)
{
    inquiry_scan_disable();
}

__attribute__((weak)) RF_CODE u8 testbox_get_connect_status(void)
{
    return 0;
}

__attribute__((weak)) RF_CODE u8 chargestore_get_connect_status(void)
{
    return 0;
}

/* ---- page scan ------------------------------------------------------------ */

RF_CODE int __page_scan_enable(int clk_base, u8 *local_addr, int addr_num, u8 rand_enable)
{
    struct link_connection *lc;
    if (page_scan) return 0;
    if (log_tag_const_i_BL) {
        log_print(2, NULL, "[Info]: [BL]page_scan_enable\n\r\n");
        printf_buf(local_addr, 6);
    }
    u32 conn = 0;
    FOR_EACH_LINK(lc) conn += lc->frame != 0;
    if (conn > 1) return -12;
    struct ctrl_frame *frame = bredr_bd_get_frame();
    if (!frame) return -12;
    page_scan = bt_malloc(120);
    if (!page_scan) {
        bredr_bd_put_frame(frame);
        return -12;
    }
    page_inquiry_clkn = 0;
    page_scan->fhs = bd_base.rx_fhs;
    __write_reg_lmprxptr(frame, 0, bd_base.rx_fhs);
    __write_reg_lmprxptr(frame, 1, page_scan->fhs);
    __write_reg_rxptr(frame, 0, page_scan->fhs);
    __write_reg_rxptr(frame, 1, page_scan->fhs);
    memcpy(page_scan->local_addr[0], local_addr, (u32)addr_num * 6);
    for (int i = 0; i < addr_num; i++) __get_access_addr(page_scan->local_acc[i], local_addr + i * 6);
    struct link_page_scan *s = page_scan;
    s->substate = 0;
    s->clk_offset = (s16)clk_base;
    s->frame = frame;
    s->rand_enable = rand_enable;
    s->addr_num = (u8)addr_num;
    s->addr_index = 0;
    s->state = 1;
    if (CONFIG_BTCTLER_TWS_ENABLE)
        s->task.type = memcmp(local_addr, gbredr_local_dev.internal_addr_local, 6) ? 4 : 128;
    else
        s->task.type = 4;
    s->task.run_slot = 32;
    s->task.run_num = 1;
    s->task.priority = 1;
    s->task.ops = &page_scan_task_ops;
    s->task.sta_attribute = 1;
    bredr_link_v.bredr_link_state |= 4;
    link_task_add(&s->task);
    if (testbox_get_connect_status() || chargestore_get_connect_status()) link_task_idle_disable();
    else link_idle_task_enable_detect();
    return 0;
}

RF_CODE int link_page_scan_enable(int clk_base)
{
    return __page_scan_enable(clk_base, hci_param.addr, 1, 0);
}

static RF_CODE void page_scan_disable(void)
{
    struct link_page_scan *s = page_scan;
    if (!s || ps_disable_active) return;
    ps_disable_active = 1;
    if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]page_scan_disable\n\r\n");
    link_task_del(&page_scan->task);
    link_idle_task_enable_detect();
    bredr_link_v.bredr_link_state &= ~36;
    if (page_scan->frame) bredr_bd_put_frame(page_scan->frame);
    bt_free(page_scan);
    page_scan = 0;
    ps_disable_active = 0;
}

RF_CODE void link_page_scan_disable(void)
{
    page_scan_disable();
}

/* ---- inquiry -------------------------------------------------------------- */

/* Keep only the EIR's local-name field (type 8 or 9), cut to 34 bytes. */
RF_CODE void inquiry_filter_eir_name(u8 *eir, u16 len, struct link_inquiry_result *result)
{
    u8 *p = eir;
    while (len && p[0]) {
        if ((p[1] & 0xfe) == 8) {
            u16 n = (u16)(p[0] + 1) > len ? len : (u16)(p[0] + 1);
            result->eir_len = n;
            if (n > 34) {
                p[0] = 33;
                result->eir_len = 34;
                n = 34;
            }
            memcpy(result->eir_data, p, n);
            return;
        }
        u32 step = p[0] + 1u;
        len = (u16)(len - step);
        p += step;
    }
}

RF_CODE void inquiry_disable(int inirq)
{
    (void)inirq;
    struct link_inquiry *q = inquiry;
    if (!q || inquiry_disable_active) return;
    inquiry_disable_active = 1;
    __timer_remove();
    link_task_del(&q->task);
    link_idle_task_enable_detect();
    if (inquiry->result.eir_data) {
        bt_free(inquiry->result.eir_data);
        inquiry->result.eir_data = 0;
    }
    bredr_bd_put_frame(inquiry->frame);
    bt_free(inquiry);
    inquiry = 0;
    inquiry_disable_active = 0;
}

static int __inquiry_result_handler(struct link_inquiry_result *result);

RF_CODE int link_inquiry_enable(u8 *_lap, int timeout_ms, u8 num, int (*completed)(struct link_inquiry_result *))
{
    u8 lap[6];
    if (inquiry) return 0;
    struct ctrl_frame *frame = bredr_bd_get_frame();
    if (!frame) return -12;
    inquiry = bt_malloc(192);
    if (!inquiry) {
        bredr_bd_put_frame(frame);
        return -12;
    }
    INIT_LIST_HEAD(&inquiry->eir_list);
    inquiry->fhs = bd_base.rx_fhs;
    inquiry->eir = bd_base.rx_eir;
    bredr_normal_pwr_set(frame, rf.bredr_inquiry_pwr_max & 15);
    struct link_inquiry *q = inquiry;
    memset(q->result_crc, 0, 61);                /* the CRCs and result_num */
    lap[0] = _lap[0];
    lap[1] = _lap[1];
    lap[2] = _lap[2];
    lap[3] = 0;
    lap[4] = 0;
    lap[5] = 0;
    u8 tog = frame->txtog & 1;
    REG32(0x1c00c4) = PHY_TO_RADIO(q->eir) | 0x80000000u;
    frame->wincntl = 70;
    __write_reg_lmprxptr(frame, 0, q->fhs);
    __write_reg_lmprxptr(frame, 1, inquiry->fhs);
    __write_reg_rxptr(frame, 0, inquiry->fhs);
    __write_reg_rxptr(frame, 1, inquiry->fhs);
    __write_reg_clkoffset(frame, 0);
    __write_reg_bdaddr(frame, lap);
    __write_reg_bch(frame, local_bch);
    __write_reg_format(frame, inquiry->ab_train, 8);
    __write_reg_linkcntl(frame, tog, 16384);
    INIT_LIST_HEAD(&inquiry->head);
    inquiry->substate = 0;
    q = inquiry;
    q->timeout = READ_SLOT_CLK() + (u32)((timeout_ms * 1000) / 625);
    q->completed = completed ? completed : __inquiry_result_handler;
    q->search_num = num;
    q->clk_offset = (s16)gbredr_local_dev.clk_base;
    q->frame = frame;
    q->state = 1;
    q->task.type = 8;
    q->task.run_slot = 32;
    q->task.run_num = 1;
    q->task.priority = 1;
    q->task.ops = &inquiry_task_ops;
    q->task.sta_attribute = 1;
    link_task_idle_disable();
    link_task_add(&inquiry->task);
    return 0;
}

/* Default inquiry result: an HCI Inquiry Result (with RSSI) or Extended
 * Inquiry Result event; 0 ends the inquiry (Inquiry Complete). */
static RF_CODE int __inquiry_result_handler(struct link_inquiry_result *result)
{
    if (!result) {
        struct list_head *n = inquiry->eir_list.next, *next;
        for (; n != &inquiry->eir_list; n = next) {
            next = n->next;
            list_del(n);
            free(n);
        }
        link_inquiry_disable();
        hci_send_event(1, "1", 0);
        return 0;
    }
    u8 *eir = result->eir_data;
    if (!eir) {
        hci_send_event(34, "1A11321", 1, result->remote.addr, 0, 0, 0, result->remote.class_dev,
                       result->remote.clk_offset, (int)result->remote.rssi);
    } else {
        hci_send_event(47, "1A11321C034", 1, result->remote.addr, 0, 0, result->remote.class_dev,
                       result->remote.clk_offset, (int)result->remote.rssi, eir);
        bt_free(result->eir_data);
        result->eir_data = 0;
    }
    return 0;
}

RF_CODE void link_inquiry_disable(void)
{
    inquiry_disable(0);      /* the SDK passes an undefined value; unused */
}

/* ---- LT addresses --------------------------------------------------------- */

RF_CODE void link_conn_set_ptt(struct link_connection *lc, u8 en)
{
    u8 v = lc->frame->miscntl;
    lc->frame->miscntl = en ? (u8)(v | 8) : (u8)(v & ~8);
}

RF_CODE int link_conn_get_ptt(struct link_connection *lc)
{
    return (lc->frame->miscntl >> 3) & 1;
}

/* The lowest free LT address 1..7; when none is free the SDK logs, bumps the
 * bitmap by one and returns its old value. */
RF_CODE int __get_lt_addr(void)
{
    u8 used = bredr_link_v.glt_addr;
    for (int i = 1; i < 8; i++) {
        if (!(used & 1 << i)) {
            bredr_link_v.glt_addr = (u8)(used | 1 << i);
            if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]glt %x\n\r\n", (used | 1 << i) & 255);
            return i;
        }
    }
    if (log_tag_const_e_BL) log_print(4, NULL, "<Error>: [BL]glt %x\n\r\n", used);
    u8 old = bredr_link_v.glt_addr;
    bredr_link_v.glt_addr = (u8)(old + 1);
    return old;
}

RF_CODE void __put_lt_addr(int lt_addr)
{
    bredr_link_v.glt_addr = (u8)(bredr_link_v.glt_addr & ((1 << lt_addr) ^ 255));
}

/* A free eSCO LT address: the lowest 1..7 not used by any link's ACL or
 * eSCO LT address; 5 when all are taken.  Only the first four slots are
 * compared.  Changed: the SDK fills a 4-byte buffer without a bound (a
 * second link writes past it); here it has room for four links. */
RF_CODE u8 esco_creart_lt_addr(void)
{
    struct link_connection *lc;
    u8 tmp[8] = { 0 };
    int i = 0;
    FOR_EACH_LINK(lc) {
        if (lc->frame) {
            tmp[i] = lc->frame->miscntl & 7;
            tmp[i + 1] = lc->esco_lt_addr;
            i += 2;
        }
    }
    for (u32 lt = 1; lt < 8; lt++) {
        int j;
        for (j = 0; j < 4; j++)
            if (tmp[j] == lt && tmp[j] != 0) break;
        if (j == 4) return (u8)lt;
    }
    return 5;
}

/* ---- page ------------------------------------------------------------------ */

RF_CODE int __page_enable(u8 *remote_addr, u8 remote_sr, int clk_offset, u8 *local_addr, u16 timeout,
                          u8 rand_enable, void (*completed)(u8 *, int))
{
    struct link_connection *lc;
    if (page) return 0;
    int conn = 0;
    FOR_EACH_LINK(lc) conn += lc->frame != 0;
    if (!(conn < 2)) return -14;
    struct ctrl_frame *frame = bredr_bd_get_frame();
    if (!frame) return -12;
    page = bt_malloc(168);
    if (!page) {
        bredr_bd_put_frame(frame);
        return -12;
    }
    bredr_normal_pwr_set(frame, rf.bredr_page_pwr_max & 15);
    __write_reg_lmprxptr(frame, 0, page->fhs);
    __write_reg_lmprxptr(frame, 1, page->fhs);
    __write_reg_rxptr(frame, 0, page->fhs);
    __write_reg_rxptr(frame, 1, page->fhs);
    struct link_page *p = page;
    memcpy(p->remote.addr, remote_addr, 6);
    memcpy(p->local_addr, local_addr, 6);
    __get_access_addr(p->remote.access_addr, remote_addr);
    __get_access_addr(page->local_acc, local_addr);
    page->substate = 0;
    page->ab_clkn = READ_SLOT_CLK();
    int lt = __get_lt_addr();
    p = page;
    p->lt_addr = (u8)lt;
    p->ab_train_max = (u8)ab_train_table[remote_sr];   /* 256 (R2) truncates to 0 */
    p->clk_offset = (s16)gbredr_local_dev.clk_base;
    p->remote.sr = remote_sr;
    p->remote.clk_offset = clk_offset;
    p->frame = frame;
    p->rand_enable = rand_enable;
    p->completed = completed;
    u32 t = (u32)bredr_offset2clkn((int)READ_SLOT_CLK(), (int)((u32)timeout * 1000 / 625));
    p = page;
    p->timeout = t;
    p->state = 1;
    __write_fhs_packet(p->local_addr, gbredr_local_dev.dev_class, &gbredr_local_dev.eir, p->lt_addr, p->fhs);
    bredr_link_v.bredr_link_state |= 1;
    p = page;
    p->task.type = 16;
    p->task.run_slot = 32;
    p->task.run_num = 1;
    p->task.priority = 1;
    p->task.ops = &page_task_ops;
    p->task.sta_attribute = 1;
    link_task_idle_disable();
    link_task_add(&page->task);
    return 0;
}

/* The clock offset from HCI Create Connection counts if bit 15 says valid. */
RF_CODE int link_page_enable(u8 *remote_addr, u8 remote_sr, int clk_offset, void (*completed)(u8 *, int))
{
    int off = (s16)clk_offset < 0 ? ((clk_offset << 1) & 0xfffe) : 0;
    return __page_enable(remote_addr, remote_sr, off, hci_param.addr, gbredr_local_dev.page_timeout_value, 0, completed);
}

static RF_CODE void page_disable(u8 put_lt_addr, int err)
{
    struct link_page *p = page;
    if (!p || page_disable_active) return;
    page_disable_active = 1;
    if (put_lt_addr) __put_lt_addr(p->lt_addr);
    link_task_del(&page->task);
    link_idle_task_enable_detect();
    bredr_link_v.bredr_link_state &= ~33;
    if (page->frame) bredr_bd_put_frame(page->frame);
    p = page;
    if (p->completed) {
        p->completed(p->remote.addr, err);
        p = page;
    }
    bt_free(p);
    page = 0;
    page_disable_active = 0;
}

RF_CODE void link_page_disable(int err)
{
    page_disable(1, err);
}

/* ---- connections: allocation and rx buffers ----------------------------- */

/* A zeroed connection with the lowest free index (0 or 1), on the list. */
RF_CODE struct link_connection *get_link_connection(void)
{
    struct link_connection *lc = malloc(728), *p;
    if (!lc) return 0;
    memset(lc, 0, 728);
    u32 used = 0;
    FOR_EACH_LINK(p) used |= 1u << p->index;
    int i;
    for (i = 0; i < 2; i++) {
        if (!(used & 1u << i)) {
            lc->index = (u8)i;
            break;
        }
    }
    if (i == 2) btctrler_assert_failed("0 link_conn_index", (int)used);
    list_add_tail(&lc->entry, &bredr_link_v.link_conn_head);
    return lc;
}

RF_CODE int link_conn_set_rx_bulk_in_irq(struct link_connection *lc, int ind)
{
    struct ctrl_frame *frame = lc->frame;
    u8 *buf = (u8 *)bd_base.link_rx_bulk[lc->index][ind];
    __write_reg_lmprxptr(frame, ind, buf);
    __write_reg_rxptr(frame, ind, buf);
    __clean_reg_rxfull(lc->frame, ind);
    lc->rx_data[ind] = buf;
    return 1;
}

static void connection_rx_handler(struct link_connection *lc, int irq);
static void connection_tx_handler(void *_lc);

RF_CODE int link_conn_rx_empty(struct link_connection *lc)
{
    local_irq_disable();
    if (!g_dut_hdl) connection_rx_handler(lc, 0);
    local_irq_enable();
    return lc->rx_pend == 0;
}

/* ---- connection receive -------------------------------------------------- */

extern BTCTRLER_CONFIG u8 rx_fre_offset_adjust_enable;
extern BTCTRLER_CONFIG int config_bt_function;
extern const u8 log_tag_const_c_BL;
int os_taskq_post_type(const char *name, int type, int argc, int *argv);
int putchar(int c);

/* Receive power control: ask the peer to lower its power (AGC 1) or raise
 * it (AGC 2), stepping the request size up by how many in a row. */
static RF_CODE void lc_rx_power_ctrl(struct link_connection *lc)
{
    if (CONFIG_BTCTLER_TWS_ENABLE && lc->tws_link) return;
    struct link_power *pw = &lc->power.normal;
    int agc = bredr_frame_get_agc(lc->frame);
    u8 decr = pw->pwr_tx_decr_req;
    u32 a = (u32)agc & 255;
    u32 cnt;
    if (decr && a == 2) cnt = 10;
    else if (a == 1 && pw->pwr_tx_incr_req) cnt = 10;
    else cnt = pwr_tb[pw->pwr_tx_cnt & 15];
    u16 n = (u16)(pw->pwr_set_cnt + 1);
    pw->pwr_set_cnt = n;
    if (!(n > cnt)) return;
    pw->pwr_set_cnt = 0;
    if ((s8)agc == 1) {
        if (pw->min_power || pw->pwr_txing) return;
        pw->pwr_txing = 1;
        if (decr < 6) {
            pw->pwr_tx_decr_req = (u8)(decr + 1);
            pw->pwr_tx_cnt = 0;
        } else if (decr < 12) {
            pw->pwr_tx_decr_req = (u8)(decr + 1);
            pw->pwr_tx_cnt = 1;
        } else if (decr < 18) {
            pw->pwr_tx_decr_req = (u8)(decr + 1);
            pw->pwr_tx_cnt = 2;
        } else {
            pw->pwr_tx_cnt = 3;
        }
        bredr_link_event(lc, 2, 12, 1);
        pw->pwr_tx_incr_req = 0;
        pw->max_power = 0;
    } else if ((s8)agc == 2) {
        if (pw->max_power || pw->pwr_txing) return;
        pw->pwr_txing = 1;
        u8 incr = pw->pwr_tx_incr_req;
        if (incr < 6) {
            pw->pwr_tx_incr_req = (u8)(incr + 1);
            pw->pwr_tx_cnt = 0;
        } else if (incr < 12) {
            pw->pwr_tx_incr_req = (u8)(incr + 1);
            pw->pwr_tx_cnt = 1;
        } else if (incr < 18) {
            pw->pwr_tx_incr_req = (u8)(incr + 1);
            pw->pwr_tx_cnt = 2;
        } else {
            pw->pwr_tx_cnt = 3;
        }
        bredr_link_event(lc, 2, 12, 2);
        pw->pwr_tx_decr_req = 0;
        pw->min_power = 0;
    }
}

/* LMP_unsniff_req and LMP_accepted(unsniff_req) are answered here, in the
 * interrupt.  1 if the PDU was consumed. */
static RF_CODE int lc_rx_unsniff(struct link_connection *lc, int index, u8 *data)
{
    struct lmp_connection *conn = lmp_conn_for_link(lc);
    if (!conn) return 0;
    u8 op = data[0];
    if ((op & 0xfe) == 48) {
        conn->tran_id = op & 1;
        if (!CONFIG_BTCTLER_TWS_ENABLE || !tws_link.link || !tws_conn.role) lmp_rx_unsniff_req(conn);
    } else if ((op & 0xfe) == 6 && data[1] == 24) {
        conn->tran_id = op & 1;
        if (!CONFIG_BTCTLER_TWS_ENABLE || !tws_link.link || !tws_conn.role) lmp_rx_accepted_unsniff_req(conn);
    } else {
        return 0;
    }
    __clean_reg_rxfull(lc->frame, index);
    return 1;
}

/* One received ACL/LMP packet in buffer index: reassemble L2CAP, hand whole
 * packets and LMP PDUs to the rx queue (and the LMP thread). */
static RF_CODE void __conn_rx_handler(struct link_connection *lc, int index, u8 llid, u16 len)
{
    u8 *data = lc->rx_data[index];
    struct ctrl_frame *frame = lc->frame;
    struct lmp_connection *conn = lmp_conn_for_link(lc);
    struct bredr_rx_bulk *rx;
    int push;
    u8 bit = (u8)(1 << index);

    if (!(sniff_support_reset_anchor_point | config_btctler_hci_standard) && llid != 3 && conn)
        lmp_tx_unsniff_req(conn);
    if (conn) lmp_sniff_subrating_cnt(conn);

    if ((llid & 3) == 3) {
        rx = bredr_rx_bulk_alloc(lc->index, len);
        push = 1;
    } else if ((llid & 3) == 2) {
        if (g_lmp_update_start_flag) {
            rx = bredr_rx_bulk_alloc(0, len);
            push = 1;
        } else {
            if (lc->rx_bulk) {
                bredr_rx_bulk_free(lc->rx_bulk);
                lc->rx_bulk = 0;
            }
            u32 total = (u32)(data[1] << 8 | data[0]) + 4;
            if (total > 2047) {
                if (log_tag_const_e_BL) log_print(4, NULL, "<Error>: [BL]l2cap_len = %x\n\r\n", total);
                __clean_reg_rxfull(frame, index);
                lc->rx_follow_ctrl &= (u8)~bit;
                lc->rx_pend &= (u8)~bit;
                return;
            }
            if (CONFIG_BTCTLER_TWS_ENABLE) {
                if (lc->tws_link) {
                    int err = 0;
                    rx = tws_rx_bulk_alloc_in_irq(len, data, &err);
                    if (!rx && !err) {
                        __clean_reg_rxfull(frame, index);
                        lc->rx_follow_ctrl &= (u8)~bit;
                        lc->rx_pend &= (u8)~bit;
                        int arg = (int)(uintptr_t)lc;
                        os_taskq_post_type("tws", 54, 1, &arg);
                        return;
                    }
                    push = 1;
                    if (len < total) {
                        lc->rx_bulk = rx;
                        push = 0;
                    }
                } else {
                    rx = bredr_rx_bulk_alloc(lc->index, (int)total);
                    push = 1;
                }
            } else {
                rx = bredr_rx_bulk_alloc(lc->index, (int)total);
                push = 1;
                if (len < total) {
                    lc->rx_bulk = rx;
                    push = 0;
                }
            }
        }
    } else {
        if (CONFIG_BTCTLER_TWS_ENABLE && !lc->tws_link) {
            rx = bredr_rx_bulk_alloc(lc->index, len);
            push = 1;
        } else {
            rx = lc->rx_bulk;
            if (!rx) {
                if (log_tag_const_d_BL) log_print(1, NULL, "[Debug]: [BL]drop_packet1\n\r\n");
                __clean_reg_rxfull(frame, index);
                return;
            }
            u32 total = (u32)(rx->data[1] << 8 | rx->data[0]) + 4;
            if ((u32)rx->len + len > total) {
                if (log_tag_const_d_BL)
                    log_print(1, NULL, "[Debug]: [BL]drop_packet: %d, %d, %d\n\r\n", len, rx->len, total);
                bredr_rx_bulk_free(rx);
                lc->rx_bulk = 0;
                __clean_reg_rxfull(frame, index);
                return;
            }
            memcpy(&rx->data[rx->len], data, len);
            __clean_reg_rxfull(frame, index);
            rx->len = (u16)(rx->len + len);
            if (rx->len < total) return;
            lc->rx_bulk = 0;
            goto push_it;
        }
    }

    if (!rx) {
        lc->rx_pend |= bit;
        lc->rx_follow_ctrl |= bit;
        if (log_tag_const_c_BL) putchar(lc->tws_link ? 'f' : 'F');
        return;
    }
    rx->len = len;
    rx->llid = llid;
    memcpy(rx->data, data, len);
    if (lc->rx_follow_ctrl_enable) {
        lc->rx_follow_ctrl |= bit;
    } else {
        __clean_reg_rxfull(frame, index);
        lc->rx_follow_ctrl &= (u8)~bit;
    }
    lc->rx_pend &= (u8)~bit;
    if (!push) return;
push_it:
    if (CONFIG_BTCTLER_TWS_ENABLE) {
        tws_link_conn_rx_handler_in_irq(lc, rx, index);
    } else {
        bredr_rx_bulk_push(&lc->rx_head, rx);
        bredr_link_event(lc, 1, 5);
    }
}

/* Drain both receive buffers in the order the baseband filled them. */
static RF_CODE void connection_rx_handler(struct link_connection *lc, int irq)
{
    struct ctrl_frame *frame = lc->frame;
    if (lc->in_rx_handler) {
        if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]in_rx_handler\n\r\n");
        return;
    }
    lc->in_rx_handler = 1;
    lc->sniff_Communicate = 1;
    local_irq_disable();
    u8 tog = frame->rxtog & 1;
    u8 rb0 = frame->rxbuf0cntl;
    u8 rb1 = frame->rxbuf1cntl;
    if (tog != (frame->rxtog & 1)) {
        lc->in_rx_handler = 0;
        local_irq_enable();
        return;
    }
    local_irq_enable();

    int adjust = 0;
    int index = 255;
    for (;;) {
        int prev = index;
        if (tog == 0) index = (rb0 & 1) ? 0 : ((rb1 & 1) ? 1 : prev);
        else index = (rb1 & 1) ? 1 : ((rb0 & 1) ? 0 : prev);
        if (index == prev) break;

        __write_reg_rxint(frame, index, 0);
        u16 stat = (&frame->rxstat0)[index];
        u16 hdr = (&frame->rxphdr0)[index];
        u32 st = (u32)stat >> 8;
        if (irq) {
            if (lc->qos_frame_status == 1) lc->qos_rx = 1;
            connection_tx_handler(lc);
        }
        if (irq == 1 && !adjust && (st & 2)) {
            if (lc->state == 2) bt_rf_protect(lc->frame, 0);
            if (lc->adjust_clkoffset_enable && lc->role) {
                if (lc->sniff_rx_flag) lc->winsize = bredr_link_v.lp_ws->lrc_ws_init;
                lc->btosc_winsize_cnt = 0;
                __rx_adjust_clkoffset(lc);
            }
            if (lc->role && rx_fre_offset_adjust_enable) bredr_fre_offset_adjust(lc);
            if (!lc->test_con_flag && !g_dut_hdl) lc_rx_power_ctrl(lc);
            lc->sniff_rx_flag = 0;
            adjust = 1;
        }
        if (irq) {
            u32 freq = frame->rxfreq & 127;     /* the SDK reads rxfreq twice */
            u32 hop = 0;
            if ((s8)frame->rxfreq > -1) hop = frame->miscntl >> 6;
            u8 ch = bd_base.frq_table[hop][freq];
            lc->afh.rx_cnt[ch]++;
            if (st != 143) lc->afh.err_cnt[ch]++;
            u32 now = READ_SLOT_CLK();
            if (bredr_clkn2offset((int)lc->afh.clkn_begin, (int)now) > 7999) {
                memcpy(lc->afh.rx_num, lc->afh.rx_cnt, 158);
                memcpy(lc->afh.err_num, lc->afh.err_cnt, 79);
                if (++lc->afh.reset_cnt > 3) {
                    memset(lc->afh.rx_cnt, 0, 158);
                    memset(&lc->afh.reset_cnt, 0, 80);   /* reset_cnt and err_cnt */
                }
                lc->afh.clkn_begin = now;
                if (!lc->tws_link && !lc->role && (config_bt_function & 2)) bredr_link_event(lc, 1, 15);
            }
        }
        u8 bit = (u8)(1 << index);
        if ((lc->rx_follow_ctrl & bit) && !(lc->rx_pend & bit)) {
            if (lc->rx_follow_ctrl_enable) {
                lc->in_rx_handler = 0;
                return;
            }
            __clean_reg_rxfull(lc->frame, index);
            lc->rx_follow_ctrl &= (u8)~bit;
            continue;                          /* same flags: the next pass ends */
        }

        u8 llid = hdr & 3;
        u16 len = (hdr >> 3) & 1023;
        if ((u8)st == 0x8f) {
            switch (stat & 15) {
            case 0: case 1: case 2:
                __clean_reg_rxfull(frame, index);
                break;
            case 9:
                if (log_tag_const_e_BL) log_print(4, NULL, "<Error>: [BL]aux1_packet\n\r\n");
                __clean_reg_rxfull(frame, index);
                break;
            default:
                if (len == 0) {
                    __clean_reg_rxfull(frame, index);
                    break;
                }
                if (llid == 3) {
                    u8 *d = lc->rx_data[index];
                    if ((d[0] & 0xfe) == 120) {
                        bredr_link_set_afh(lc, (u32)d[3] << 16 | (u32)d[4] << 24 | (u32)d[2] << 8 | d[1], d[5], d + 6);
                        __clean_reg_rxfull(frame, index);
                        break;
                    }
                    if (!lc->tws_link && lc_rx_unsniff(lc, index, d)) break;
                }
                if (CONFIG_BTCTLER_TWS_ENABLE && !lc->tws_link && !(lc->rx_pend & bit)) lc->rx_packet_num++;
                __conn_rx_handler(lc, index, llid, len);
                break;
            }
            if (irq) lc->super_timeout_begin = READ_SLOT_CLK();
            lc->seqn_err = 0;
        } else {
            if ((u8)st == 0xaf && !lc->tws_link && CONFIG_BTCTLER_TWS_ENABLE) lc->seqn_err_num++;
            if ((st & 34) != 2 && lc->seqn_err != 0xff) {
                if (++lc->seqn_err > 6) {
                    lc->seqn_err = 0;
                    if (bredr_pwr_set(lc, 1, 1) == 2) lc->seqn_err = 0xff;
                }
            }
            __clean_reg_rxfull(frame, index);
        }
        if (lc->rx_pend & bit) break;
        if (index == 0) {
            if (rb1 == 0) break;
            rb0 = 0;
        } else {
            if (rb0 == 0) break;
            rb1 = 0;
        }
    }
    lc->in_rx_handler = 0;
}

/* ---- connection transmit -------------------------------------------------- */

extern BTCTRLER_CONFIG int CONFIG_TEST_DUT_CODE;
extern BTCTRLER_CONFIG int config_btctler_le_tws;
extern int CONFIG_TWS_SUPER_TIMEOUT;

RF_CODE int link_conn_tx_empty(struct link_connection *lc)
{
    int ret = 0;
    local_irq_disable();
    if (!g_dut_hdl) connection_tx_handler(lc);
    if (!lc->tx_bulk[0] && !lc->tx_bulk[1] && list_empty(&lc->tx_acl_c)) ret = list_empty(&lc->tx_acl_u) != 0;
    local_irq_enable();
    return ret;
}

/* Retire what the baseband sent from each tx buffer (post actions: PTT,
 * power, unsniff ack, encryption, AFH, max slot, events) and refill the
 * buffer from the LMP queue, then the ACL queue.  A TWS monitor link only
 * drains its queues. */
static RF_CODE void connection_tx_handler(void *_lc)
{
    struct link_connection *lc = _lc;
    struct ctrl_frame *frame = lc->frame;
    s8 update_index[2] = { -1, -1 };
    if (lc->in_tx_handler) return;
    lc->in_tx_handler = 1;
    local_irq_disable();
    u8 tog = frame->txtog & 1;
    u8 tb0 = frame->txbuf0cntl;
    u8 tb1 = frame->txbuf1cntl;
    (void)frame->linkcntl0;                    /* read and unused in the SDK */
    (void)frame->linkcntl1;
    if (tog != (frame->txtog & 1)) {
        lc->in_tx_handler = 0;
        local_irq_enable();
        return;
    }
    local_irq_enable();

    if (CONFIG_BTCTLER_TWS_ENABLE && lc->monitor) {
        for (;;) {
            struct bredr_tx_bulk *tx = bredr_tx_bulk_pop(&lc->tx_acl_c);
            if (!tx) tx = bredr_tx_bulk_pop(&lc->tx_acl_u);
            if (!tx) {
                lc->in_tx_handler = 0;
                return;
            }
            if (tx->event_prob) bredr_link_event(lc, 1, tx->event_prob);
            if (tx->event_post) bredr_link_event(lc, 1, tx->event_post);
            if (tx->max_slot_post) lc->core.max_slot = tx->max_slot_post;
            bredr_tx_bulk_free(tx);
        }
    }

    /* The buffer the toggle points at first, then the other; only empty ones. */
    int index = tog;
    for (int i = 0; i < 2; i++) {
        if (index) {
            if (tb1 & 1) update_index[i] = 1;
            index = 0;
        } else {
            if (tb0 & 1) update_index[i] = 0;
            index = 1;
        }
    }

    for (int i = 0; i < 2; i++) {
        s8 idx = update_index[i];
        if (idx == -1) continue;
        struct bredr_tx_bulk **slot = &lc->tx_bulk[idx];
        struct bredr_tx_bulk *tx = *slot, *prev = 0;
        if (tx && tx != lc->tx_bulk[idx == 0] && tx->offset == tx->len) {
            bredr_link_qos_open(lc);
            /* LMP_accepted_ext(packet_type_table_req): switch PTT now. */
            if (tx->llid == 3 && tx->data[0] > 253 && tx->data[1] == 1 && tx->data[3] == 11) lc_write_ptt(lc, 1);
            if (tx->tws_start_req) lc->tws_start_req = 0;
            if (tx->pwr_post) lc->power.normal.pwr_txing = 0;
            if (tx->unsniff_ack_check) {
                lc->unsniff_ack = 1;
                if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]unsniff_ack_1\n\r\n");
            }
            if (tx->pause_enc_debug && log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]pause_enc_debug\n\r\n");
            u8 enc = tx->enc_post;
            if (enc & 4) {
                lc_write_encry(lc, (enc >> 1) & 1, enc & 1);
                if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]post_enc\n\r\n");
            }
            if (tx->set_afh) bredr_link_set_afh(lc, lc->afh.instant, lc->afh.en, lc->channel_map);
            if (tx->max_slot_post) lc->core.max_slot = tx->max_slot_post;
            if (tx->event_post && lc->tws_link) {
                prev = tx;                       /* TWS: its event goes below */
            } else {
                if (tx->event_post) bredr_link_event(lc, 1, tx->event_post);
                bredr_tx_bulk_free(tx);
            }
        }
        struct bredr_tx_bulk *next = bredr_tx_bulk_pop(&lc->tx_acl_c);
        if (!next && lc->send_acl_enable) next = bredr_tx_bulk_pop(&lc->tx_acl_u);
        if (next) {
            lc->sniff_long_interval_filter = 2;
            __link_ctrl_tx(next, idx, lc);
            bredr_link_qos_close(lc);
        }
        *slot = next;
        if (prev && CONFIG_BTCTLER_TWS_ENABLE) {
            tws_conn_tx_event_handler(prev->event_post, prev);
            bredr_tx_bulk_free(prev);
        }
    }
    lc->in_tx_handler = 0;
}

/* Supervision timeout over all links (a slot timer callback).  After 1000
 * slots without a packet the power goes to step 9; in sniff the timeout gets
 * 6400 slots more. */
RF_CODE void link_conn_super_timeout(void *p)
{
    (void)p;
    struct link_connection *lc;
    u32 now = READ_SLOT_CLK();
    FOR_EACH_LINK(lc) {
        if (!lc->frame || !lc->super_timeout) continue;
        int off = bredr_clkn2offset((int)lc->super_timeout_begin, (int)now);
        if (off > 999) bredr_pwr_set(lc, 0, 9);
        int t = lc->sniff_sta ? off - 6400 : off;
        if ((int)lc->super_timeout > t) continue;
        bredr_power_on();
        if (log_tag_const_i_BL)
            log_print(2, NULL, "[Info]: [BL]-- link super timeot :0x%x, %d, %d\n\r\n", lc, lc->super_timeout, t);
        if (CONFIG_BTCTLER_TWS_ENABLE && lc->tws_link) {
            int r = os_taskq_post_type("tws", 45, 0, 0);
            if (!r) lc->super_timeout = 0;
            if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]tws_link_super_timeout: %d\n\r\n", r);
            continue;
        }
        if (!bredr_link_event(lc, 1, 3)) lc->super_timeout = 0;
    }
}

RF_CODE void link_connection_timeout(struct link_connection *lc)
{
    bt_rf_protect(lc->frame, 0);
}

/* The slot timer at clkn + slot (0x1c00e4), its interrupt enabled. */
static RF_CODE void __timer_register(int slot)
{
    REG32(0x1c0018) |= 512;
    REG32(0x1c001c) = 1;
    while (REG32(0x1c001c)) ;
    REG32(0x1c00e4) = REG32(0x1c0020) + (u32)slot;
    REG32(0x1c000c) |= 512;
}

static RF_CODE void __timer_remove(void)
{
    REG32(0x1c0018) |= 512;
    REG32(0x1c000c) &= ~512u;
}

#define LC_OF_TASK(t) ((struct link_connection *)((u8 *)(t) - offsetof(struct link_connection, task)))

RF_CODE int link_conn_task_resume(struct link_task *task)
{
    struct link_connection *lc = LC_OF_TASK(task), *t;
    if (CONFIG_BTCTLER_TWS_ENABLE) {
        if (!lc->tws_link) {
            FOR_EACH_LINK(t) {
                if (t->frame && t->tws_link) {
                    tws_monitor_link_txpwr_cpy(lc->frame, t->frame);
                    break;
                }
            }
        }
        lc->seqn_err_num = 0;
        lc->rx_packet_num = 0;
    }
    lc->state = 2;
    bt_rf_protect(lc->frame, 1);
    __timer_register(8);
    if (__read_reg_clkoffset(lc->frame) & 1) bredr_bd_frame_enable(lc->frame, 1, 1);
    else bredr_bd_frame_enable(lc->frame, 0, 1);
    return 0;
}

RF_CODE int link_conn_task_suspend(struct link_task *task)
{
    struct link_connection *lc = LC_OF_TASK(task);
    if (g_dut_hdl || lc->state == 3) return 0;
    lc->state = 3;
    bredr_bd_frame_disable(lc->frame);
    bt_rf_protect(lc->frame, 0);
    return 0;
}

RF_CODE void bredr_frame_suspend_control(u8 en)
{
    struct link_connection *lc;
    FOR_EACH_LINK(lc) {
        if (!lc->frame) continue;
        if (en) bredr_bd_frame_disable(lc->frame);
        else if (__read_reg_clkoffset(lc->frame) & 1) bredr_bd_frame_enable(lc->frame, 1, 1);
        else bredr_bd_frame_enable(lc->frame, 0, 1);
    }
}

/* ---- connections: create and close ---------------------------------------- */

RF_CODE void link_connection_init(struct link_connection *lc)
{
    lc->state = 2;
    lc->send_acl_enable = 1;
    lc->core.max_slot = 5;
    lc->afh_timer = 0xff;
    lc->qos_timer = 0xff;
    lc->adjust_clkoffset_enable = 1;
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
}


/* A new connection on frame (page or page scan done): end the procedures
 * that are still running and start the link task.  A TWS link (local address
 * is the internal one) gets the TWS task ops, its own supervision timeout and
 * an AFH map without channels i-1..i+1 for i = 10, 22, ..., 70. */
RF_CODE struct link_connection *create_link_connection(struct ctrl_frame *frame, struct bredr_remote_device *remote,
                                                       u8 *local_addr, u8 role, s16 last_signed_bitoff,
                                                       u8 ble_connect, u8 tws_role)
{
    struct link_connection *lc = get_link_connection(), *p;
    if (!lc) return 0;
    link_connection_init(lc);
    lc->role = role & 1;
    lc->frame = frame;
    lc->last_signed_bitoff = last_signed_bitoff;
    lc->core.remote_sr = remote->sr;
    lc->core.remote_class_dev = remote->class_dev;
    lc->test_con_flag = 0;
    memcpy(lc->core.remote_addr, remote->addr, 6);
    bredr_pwr_set(lc, 0, 15);
    lc->offset_packet = 60;
    lc->offset_sun = 0;
    lc->super_timeout_begin = READ_SLOT_CLK();
    lc->super_timeout = (u16)((u32)gbredr_local_dev.super_timeout_value * 1000 / 625);
    for (int i = 0; i < 2; i++) link_conn_set_rx_bulk_in_irq(lc, i);
    if (CONFIG_BREDR_INQUIRY && !get_page_remote_name()) inquiry_disable(0);   /* SDK: undefined arg, unused */
    inquiry_scan_disable();
    if (role == 1) {
        page_disable(1, 254);
        page_scan_disable();
        if (ble_connect && config_btctler_le_tws) {
            link_le_adv_try_start_disable(0);
            link_le_scan_try_start_disable(0);
            tws_ble_connect_disable(0);
        }
    } else {
        page_scan_disable();
        page_disable(0, 254);
        if (ble_connect && config_btctler_le_tws) {
            link_le_scan_try_start_disable(0);
            link_le_adv_try_start_disable(0);
            tws_ble_connect_disable(0);
        }
    }
    bredr_link_v.bredr_link_state |= 16;
    link_task_idle_disable();
    if (CONFIG_BTCTLER_TWS_ENABLE) {
        if (memcmp(local_addr, gbredr_local_dev.internal_addr_local, 6)) goto not_tws;
        if (memcmp(lc->core.remote_addr, gbredr_local_dev.internal_addr_remote, 6)) {
            if (role == 0) goto not_tws;
            memcpy(gbredr_local_dev.internal_addr_remote, lc->core.remote_addr, 6);
        }
        lc->super_timeout = (u16)(CONFIG_TWS_SUPER_TIMEOUT * 1000 / 625);
        lc->tws_link = 1;
        lc->task.ops = &tws_link_task_ops;
        lc->task.sta_attribute = 3;
        lc->task.type = 256;
        lc->core.pwr_set = 9;
        bredr_pwr_set(lc, 0, 9);
        frame->miscntl |= 8;
        for (int i = 10; i < 71; i += 12)
            for (int j = i - 1; j <= (i | 1); j++)
                lc->channel_map[j / 8] &= (u8)((1 << (j & 7)) ^ 255);
        bdhw_set_afh(frame, lc->channel_map);
        link_task_add(&lc->task);
        tws_link_conn_start_in_irq(lc, ble_connect ? tws_role : role);
        bredr_link_v.glt_addr |= (u8)(1 << (lc->frame->miscntl & 7));
        return lc;
not_tws:
        tws_tx_unsniff_req();
    }
    FOR_EACH_LINK(p)
        if (p->frame && p != lc && !p->tws_link) link_conn_set_max_rx_bulk_persent(p, 70);
    link_task_add(&lc->task);
    bredr_link_event(lc, 2, 1, role);
    return lc;
}

RF_CODE void link_conn_set_max_rx_bulk_persent(struct link_connection *lc, int persent)
{
    bredr_rx_bulk_set_max_used_persent(lc->index, persent);
}

/* The packet type for len bytes within slot slots: the first table row
 * (1M or, with ppt, 2M) that holds len, else the last that fits the slots. */
RF_CODE const u16 *get_packet_type(int len, u8 ppt, u8 slot)
{
    if (!slot) btctrler_assert_failed("slot >= 1", slot);
    const u16 (*t)[3] = packet_1M_table;
    u32 n = 6;
    if (ppt) {
        n = (packet_table_support_en & 1) | 2;
        t = packet_2M_table;
    }
    u32 i;
    for (i = 0; i < n; i++) {
        if (t[i][2] > slot) break;
        if (!((int)t[i][0] < len)) return t[i];
    }
    return t[i - 1];
}

/* Average the frequency offset (0x1cfc18) over 60 packets once, then move
 * PLL_COMP by it. */
RF_CODE void bredr_fre_offset_adjust(struct link_connection *lc)
{
    struct ctrl_frame *frame = lc->frame;
    if (bredr_get_dut_enable()) return;
    if (!(config_btctler_mode & 1)) return;
    if (lc->test_con_flag) return;
    s32 sum = (s16)(0 - REG32(0x1cfc18)) + lc->offset_sun;
    lc->offset_sun = sum;
    u8 n = lc->offset_packet;
    if (!(n < 61)) return;
    lc->offset_packet = --n;
    if (n) return;
    lc->offset_sun = sum / 60;
    radio_set_eninv(0);
    REG32(0x1c0000) |= 0x80000000u;
    s32 comp = (s32)((u32)(0 - lc->offset_sun) << 20) / 24000;
    frame->PLL_COMP = (u32)comp + frame->PLL_COMP;
    radio_set_eninv(1);
}

/* Track the master: the bit offset of the received packet within the
 * window, and the clock offset nudged by one slot when the bit offset
 * crosses half a slot (a wide window takes the received slot directly). */
RF_CODE void __rx_adjust_clkoffset(struct link_connection *link)
{
    struct ctrl_frame *frame = link->frame;
    int clk = __read_reg_clkoffset(frame);
    frame->wincntl = (u16)(REG32(0x1c0028) + 20);
    u16 rxbit = frame->rxbit;
    u16 wincntl = frame->wincntl;
    s16 sub6 = (s16)((s16)((rxbit & 1023) - 68) - ((wincntl >> 1) & 2047));
    if (!link->tws_link) {
        int odd = (link->frame->rx_clknl & 1) != 0;
        s16 ali;
        if (sub6 > 0) ali = odd ? (s16)(sub6 + 625) : sub6;
        else ali = odd ? (s16)(sub6 + 625) : (s16)(sub6 + 1250);
        link->signed_bitoff_ali = (u16)ali;
    }
    u8 wide = link->wide_win_flag;
    link->wide_win_flag = 0;
    link->btosc_winsize_cnt = 0;
    s16 last = link->last_signed_bitoff;
    u16 lastb = (u16)((last < 0 ? last + 625 : last) & 1023);
    frame->bitoff = (u16)((sub6 < 0 ? sub6 + 625 : sub6) & 1023);
    u16 hi = frame->rxbit >> 13;
    int remote = clk;
    if (wide) {
        u16 lobit = ((u16)sub6 >> 10) & 1;
        remote = clk - hi + lobit;
        link->clkoff_drift = (s16)(hi - lobit);
        __write_reg_clkoffset(frame, remote);
    } else if (lastb < 300) {
        if ((u32)frame->bitoff > (u32)lastb + 324) {
            remote = clk + 1;
            link->clkoff_drift--;
            __write_reg_clkoffset(frame, remote);
        }
    } else if (lastb > 324) {
        if ((int)frame->bitoff < (int)lastb - 324) {
            remote = clk - 1;
            link->clkoff_drift++;
            __write_reg_clkoffset(frame, remote);
        }
    }
    link->last_signed_bitoff = sub6;
    if (link->state == 3) return;
    if (link->sync_train_state || link->csbi_train_state) return;
    bd_frame_odd_even(link->frame, remote & 1 ? 1 : 0);
}

/* Load tx into buffer index: LMP whole (asserting on a TWS link), ACL in
 * pieces of the largest packet type that fits, then mark the buffer full. */
RF_CODE void __link_ctrl_tx(struct bredr_tx_bulk *tx, int index, struct link_connection *lc)
{
    struct ctrl_frame *frame = lc->frame;
    u8 enc = tx->enc_prob;
    if (enc & 4) lc_write_encry(lc, (enc >> 1) & 1, enc & 1);
    u8 ev = tx->event_prob;
    if (ev) {
        if (CONFIG_BTCTLER_TWS_ENABLE && lc->tws_link) tws_conn_tx_event_handler(ev, tx);
        else bredr_link_event(lc, 1, ev);
    }
    switch (tx->llid) {
    case 3:
        if (lc->tws_link) btctrler_assert_failed("0 tx->llid == ACL_C", tx->data[0]);
        __write_reg_txinfo(frame, index, tx->data, tx->len, tx->llid, 1, tx->packet);
        tx->offset = tx->len;
        list_del(&tx->entry);
        break;
    case 1:
    case 2: {
        int left = (int)tx->len - (int)tx->offset;
        const u16 *type = get_packet_type(left, (frame->miscntl >> 3) & 1, lc->core.max_slot);
        int n = left > (int)type[0] ? (int)type[0] : left;
        u16 off = tx->offset;
        __write_reg_txinfo(frame, index, &tx->data[off], n, (tx->llid == 2 && off == 0) ? 2 : 1, 1, type[1]);
        tx->offset = (u16)(tx->offset + n);
        if (tx->len == tx->offset) list_del(&tx->entry);
        break;
    }
    default:
        btctrler_assert_failed("0 unknow_llid", tx->llid);
        break;
    }
    local_irq_disable();
    __clean_reg_txempty(frame, index);
    local_irq_enable();
}

RF_CODE void lc_write_encry(struct link_connection *lc, u8 rx_enc, u8 tx_enc)
{
    __write_reg_encry(lc->frame, rx_enc, tx_enc);
}

/* ---- QoS: blank the radio between polls ------------------------------------ */

/* Alternates 6-slot (eninv on) and 36-slot (eninv off) phases; a phase with
 * eninv off ends only after a packet was received (qos_rx). */
RF_CODE void bredr_link_qos_switch(void *_lc)
{
    struct link_connection *lc = _lc;
    if (lc->qos_frame_status == 0) {
        radio_set_eninv(1);
        lc->qos_rx = 0;
        lc->qos_frame_status = 1;
        slot_timer_set(lc->qos_timer, bredr_link_qos_switch, lc, 6);
    } else if (lc->qos_rx == 0) {
        slot_timer_set(lc->qos_timer, bredr_link_qos_switch, lc, 6);
    } else {
        radio_set_eninv(0);
        lc->qos_frame_status = 0;
        slot_timer_set(lc->qos_timer, bredr_link_qos_switch, lc, 36);
    }
}

RF_CODE void bredr_link_qos_open(void *_lc)
{
    struct link_connection *lc = _lc;
    if (!(config_bt_function & 4) || lc->tws_link || lc->role) return;
    if (lc->qos_timer == 0xff) lc->qos_timer = slot_timer_get();
    lc->qos_rx = 0;
    radio_set_eninv(0);
    lc->qos_frame_status = 0;
    slot_timer_set(lc->qos_timer, bredr_link_qos_switch, lc, 24);
}

RF_CODE void bredr_link_qos_close(void *_lc)
{
    struct link_connection *lc = _lc;
    if (!(config_bt_function & 4) || lc->tws_link || lc->role) return;
    if (lc->qos_timer != 0xff) {
        slot_timer_put(lc->qos_timer);
        lc->qos_timer = 0xff;
    }
    radio_set_eninv(1);
}

/* Queue tx (LMP or ACL) and try to load it now. */
RF_CODE void link_conn_tx_bulk_avaiable(struct link_connection *lc, struct bredr_tx_bulk *tx)
{
    local_irq_disable();
    if (!lc || !lc->frame) {
        bredr_tx_bulk_free(tx);
        goto out;
    }
    if (tx) {
        if (tx->llid == 3 && CONFIG_TEST_DUT_CODE) dut_clear_tx_buf(lc);
        if (tx->llid == 3) list_add_tail(&tx->entry, &lc->tx_acl_c);
        else list_add_tail(&tx->entry, &lc->tx_acl_u);
        bredr_link_qos_close(lc);
    }
    if (!g_dut_hdl) connection_tx_handler(lc);
out:
    local_irq_enable();
}

/* ---- AFH -------------------------------------------------------------------- */

/* A new channel map from the link's statistics: of the 30 most received
 * channels (counts taken mod 256), those under an error percentage that
 * starts at 10 and grows by 5 up to 25, until more than 14 are found; under
 * 20 channels, fill up to 30 with the lowest error rates.  mode != 0 means
 * all 79 channels.  Returns 1 if the map changed. */
RF_CODE int bredr_link_update_afh(struct link_connection *lc, u8 *channel_map, u8 *tx_channel_classification, u8 mode)
{
    u8 err_persent[2][79];      /* [0] channel, [1] error % */
    u8 rx_cnt[2][79];           /* [0] channel, [1] rx count & 255 */
    u8 curr_channel_map[10];
    local_irq_disable();
    if (!lc) {
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
    for (int i = 0; i < 79; i++) {
        u16 rx = lc->afh.rx_num[i];
        rx_cnt[0][i] = (u8)i;
        rx_cnt[1][i] = (u8)rx;
        err_persent[0][i] = (u8)i;
        err_persent[1][i] = (rx & 255) > 19 ? (u8)((u32)lc->afh.err_num[i] * 100 / (rx & 255)) : 0;
    }
    local_irq_enable();
    for (int i = 0; i < 78; i++) {
        for (int j = i + 1; j < 79; j++) {
            u8 a = rx_cnt[1][i], b = rx_cnt[1][j];
            if (a < b) {
                rx_cnt[1][j] = a;
                rx_cnt[1][i] = b;
                u8 c = rx_cnt[0][j];
                rx_cnt[0][j] = rx_cnt[0][i];
                rx_cnt[0][i] = c;
            }
        }
    }
    int limit = 10, cnt = 0;
    for (;;) {
        memset(channel_map, 0, 10);
        for (int i = 0; i < 30; i++) {
            u8 ch = rx_cnt[0][i];
            if (!rx_cnt[1][i] || !((int)err_persent[1][ch] < limit)) continue;
            /* Both branches set the bit in the SDK. */
            if ((hci_param.features[4] & 16) && (tx_channel_classification[ch >> 3] & 1 << (ch & 7))) {
                channel_map[ch >> 3] |= (u8)(1 << (ch & 7));
                cnt++;
            } else {
                channel_map[ch >> 3] |= (u8)(1 << (ch & 7));
                cnt++;
            }
        }
        if (cnt > 14) {
            if (!(cnt < 20)) goto result;
            break;
        }
        limit += 5;
        if (limit > 29) break;
    }
    for (int i = 0; i < 78; i++) {
        for (int j = i + 1; j < 79; j++) {
            u8 a = err_persent[1][i], b = err_persent[1][j];
            if (a > b) {
                err_persent[1][j] = a;
                err_persent[1][i] = b;
                u8 c = err_persent[0][j];
                err_persent[0][j] = err_persent[0][i];
                err_persent[0][i] = c;
            }
        }
    }
    for (int i = 0; cnt < 30 && i < 79; i++) {
        u8 ch = err_persent[0][i];
        if (!(channel_map[ch >> 3] & 1 << (ch & 7))) {
            channel_map[ch >> 3] |= (u8)(1 << (ch & 7));
            cnt++;
        }
    }
result:
    return memcmp(channel_map, curr_channel_map, 10) != 0;
}

/* ---- rx bulk wrappers ------------------------------------------------------- */

RF_CODE int link_conn_rx_bulk_total_size(struct link_connection *lc)
{
    return bredr_rx_bulk_total_size(lc->index);
}

RF_CODE int link_conn_rx_bulk_remain_size(struct link_connection *lc, int unit_size)
{
    return bredr_rx_bulk_remain_size(lc->index, unit_size);
}

RF_CODE struct bredr_rx_bulk *link_conn_rx_bulk_alloc_for(struct link_connection *lc, int len)
{
    return bredr_rx_bulk_alloc_for(lc->index, len);
}

/* Free bulk; room may have come free, so drain links waiting on it. */
RF_CODE void link_conn_rx_bulk_avaliable(struct link_connection *lc, struct bredr_rx_bulk *bulk)
{
    (void)lc;
    struct link_connection *p;
    if (bulk) bredr_rx_bulk_free(bulk);
    local_irq_disable();
    FOR_EACH_LINK(p)
        if (p->frame && !p->rx_follow_ctrl_enable && !g_dut_hdl) connection_rx_handler(p, 0);
    local_irq_enable();
}

RF_CODE void link_conn_follow_ctrl_enable(struct link_connection *lc)
{
    lc->rx_follow_ctrl_enable = 1;
}

RF_CODE void link_conn_follow_ctrl_disable(struct link_connection *lc)
{
    local_irq_disable();
    lc->rx_follow_ctrl_enable = 0;
    if (!g_dut_hdl) connection_rx_handler(lc, 0);
    local_irq_enable();
}

RF_CODE void link_conn_super_timeout_reset(struct link_connection *lc)
{
    lc->super_timeout_begin = READ_SLOT_CLK();
}

/* Tear a connection down: task, frame, LT address (slave keeps none), eSCO,
 * QoS and AFH timers, role-switch state, queued packets; then free it. */
RF_CODE void link_connection_close(struct link_connection *lc)
{
    struct link_connection *t;
    if (log_tag_const_i_BL) {
        log_print(2, NULL, "[Info]: [BL]link_connection_close :0x%x\n\r\n", lc);
        if (lc->tws_link) log_print(2, NULL, "[Info]: [BL]TWS dis\n\r\n");
    }
    int conn = 0;
    FOR_EACH_LINK(t) conn += t->frame != 0;
    if (conn == 1) bredr_link_v.bredr_link_state &= ~16;
    link_task_del(&lc->task);
    bredr_bd_put_frame(lc->frame);
    local_irq_disable();
    if (!lc->role) __put_lt_addr(lc->frame->miscntl & 7);
    lc->super_timeout = 0;
    if (lc->esco) bredr_esco_link_close(lc);
    if ((config_bt_function & 4) && !lc->tws_link && !lc->role && lc->qos_timer != 0xff) {
        slot_timer_put(lc->qos_timer);
        lc->qos_timer = 0xff;
    }
    if (lc->afh_timer != 0xff) {
        slot_timer_put(lc->afh_timer);
        lc->afh_timer = 0xff;
    }
    if (lc->rs) {
        if (lc->rs->page) bt_free(lc->rs->page);
        if (lc->rs->page_scan) bt_free(lc->rs->page_scan);
        bt_free(lc->rs);
        lc->rs = 0;
    }
    if (CONFIG_TEST_DUT_CODE) bredr_dut_detch(lc);
    lc->frame = 0;
    link_idle_task_enable_detect();

    /* link_connection_clear_rx_bulk */
    if (lc->rx_bulk) {
        bredr_rx_bulk_free(lc->rx_bulk);
        lc->rx_bulk = 0;
    }
    for (struct list_head *n = lc->rx_head.next, *next; n != &lc->rx_head; n = next) {
        next = n->next;
        __list_del_entry(n);
        bredr_rx_bulk_free((struct bredr_rx_bulk *)((u8 *)n - offsetof(struct bredr_rx_bulk, entry)));
    }
    /* link_connection_clear_tx_bulk: the queues are freed, not unlinked */
    if (lc->tx_bulk[0]) {
        bredr_tx_bulk_free(lc->tx_bulk[0]);
        if (lc->tx_bulk[1] == lc->tx_bulk[0]) lc->tx_bulk[1] = 0;
        lc->tx_bulk[0] = 0;
    }
    if (lc->tx_bulk[1]) bredr_tx_bulk_free(lc->tx_bulk[1]);
    lc->tx_bulk[1] = 0;
    for (struct list_head *n = lc->tx_acl_c.next, *next; n != &lc->tx_acl_c; n = next) {
        next = n->next;
        bredr_tx_bulk_free((struct bredr_tx_bulk *)((u8 *)n - offsetof(struct bredr_tx_bulk, entry)));
    }
    for (struct list_head *n = lc->tx_acl_u.next, *next; n != &lc->tx_acl_u; n = next) {
        next = n->next;
        bredr_tx_bulk_free((struct bredr_tx_bulk *)((u8 *)n - offsetof(struct bredr_tx_bulk, entry)));
    }
    __list_del_entry(&lc->entry);
    local_irq_enable();
    free(lc);
    bredr_rx_bulk_state();
}

/* ---- eSCO ------------------------------------------------------------------- */

RF_CODE void bredr_esco_link_close(struct link_connection *lc)
{
    if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]esco_close: %x\n\r\n", lc->esco);
    if (lc && CONFIG_BTCTLER_TWS_ENABLE && lc->tws_link) {
        bredr_esco_tws_link_close(lc);
        tws_esco_remove_clear_state();
        return;
    }
    if (!lc->esco) return;
    __pcm_out_disable(0);
    __pcm_out_disable(1);
    if (lc->esco->timer0) {
        slot_timer_put(lc->esco->timer0);
        lc->esco->timer0 = 0;
    }
    if (lc->esco->timer1) {
        slot_timer_put(lc->esco->timer1);
        lc->esco->timer1 = 0;
    }
    _write_reg_esco_clkalign(lc->frame, 0);
    radio_set_channel(3);
    radio_set_esco_next(0);
    radio_set_esco_rsvd(0);
    local_irq_disable();
    __set_default_sco_rx_buf();
    bredr_rx_bulk_free(lc->esco->rx_bulk[0]);
    bredr_rx_bulk_free(lc->esco->rx_bulk[1]);
    for (int i = 0; i < 3; i++)
        if (lc->esco->tx_bulk[i]) bredr_tx_bulk_free(lc->esco->tx_bulk[i]);
    for (struct list_head *n = lc->esco->tx_esco.next, *next; n != &lc->esco->tx_esco; n = next) {
        next = n->next;
        bredr_tx_bulk_free((struct bredr_tx_bulk *)((u8 *)n - offsetof(struct bredr_tx_bulk, entry)));
    }
    for (struct list_head *n = lc->esco->rx_esco.next, *next; n != &lc->esco->rx_esco; n = next) {
        next = n->next;
        __list_del_entry(n);
        bredr_rx_bulk_free((struct bredr_rx_bulk *)((u8 *)n - offsetof(struct bredr_rx_bulk, entry)));
    }
    if (lc->esco->tws_esco_tx_info) bt_free(lc->esco->tws_esco_tx_info);
    bt_free(lc->esco);
    lc->esco = 0;
    if (CONFIG_BTCTLER_TWS_ENABLE) {
        bredr_esco_tws_link_close(lc);
        tws_esco_remove_clear_state();
    }
    local_irq_enable();
    os_sem_post((OS_SEM *)&esco_sem);
    if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]esco_link_close: exit\n\r\n");
}

/* ---- receive interrupt -------------------------------------------------------
 * The SDK inlines one handler per procedure into bredr_link_rx_irq_handler;
 * they are separate static functions here, named as in the SDK's debug info. */

#define RX_OK(stat) (((stat) & 0xff00) == 0x8f00)     /* header and payload ok */


static RF_CODE void tws_ble_connect_rx_handler(struct ctrl_frame *frame)
{
    struct link_tws_ble *b = tws_ble;
    u8 local_addr[6];
    u8 role_type = b->role_type;
    u8 le_type = b->le_type;
    __timer_remove();
    if (le_type == 1) {
        if (!tws_ble_slave_step_handler(&b->remote, frame)) return;
    } else {
        if (!tws_ble_master_step_handler(frame)) return;    /* SDK: undefined arg, unused */
    }
    tws_ble->frame = 0;
    memcpy(local_addr, tws_ble->local_addr, 6);
    if (le_type == 0) create_link_connection(frame, &b->remote, local_addr, 0, 0, 1, role_type);
    else create_link_connection(frame, &b->remote, local_addr, 1, tws_ble->last_signed_bitoff, 1, role_type);
    link_task_schedule_resume();
}

static RF_CODE void inquiry_scan_rx_handler(struct ctrl_frame *frame)
{
    struct link_inquiry_scan *s = inquiry_scan;
    u16 stat = (&frame->rxstat0)[(frame->rxtog & 1) ^ 1];
    if (!RX_OK(stat) || inq_scan_disable_active) return;
    if ((stat & 15) == 0) {
        s->substate = 1;
        inquiry_scan->rx_clkn = READ_SLOT_CLK();
    }
    bredr_frame_agc_set(frame, 1, 0);
}

/* Page scan: ID (0) -> FHS (1) -> ID response sent (2) -> first POLL (3);
 * after the third step the connection exists. */
static RF_CODE void page_scan_rx_handler(struct ctrl_frame *frame)
{
    struct link_page_scan *ps = page_scan;
    struct bredr_remote_device *remote = &ps->remote;
    u8 ridx = (frame->rxtog & 1) ^ 1;
    u8 ttog = frame->txtog & 1;
    u16 stat = (&frame->rxstat0)[ridx];
    u8 format;
    if (!RX_OK(stat) || ps_disable_active) return;
    bredr_frame_agc_set(frame, 1, 0);
    switch (page_scan->substate) {
    case 0:
        bd_frame_odd_even(frame, 2);
        frame->wincntl = 440;
        frame_bitoff_adjust(frame);
        __timer_register(16);
        format = 7;
        break;
    case 1: {
        if ((stat & 15) != 2) return;
        __read_fhs_packet(remote, page_scan->fhs);
        ps->remote.clk_offset = (s32)(ps->remote.clkn - __read_reg_clkn(frame));
        frame->wincntl = (u16)REG32(0x1c0028);
        u16 rxbit = frame_bitoff_adjust(frame);
        page_scan->last_rxbit = rxbit;
        if (rxbit < 88) ps->remote.clk_offset++;
        __write_reg_clkoffset(frame, ps->remote.clk_offset);
        __write_reg_bdaddr(frame, ps->remote.addr);
        __write_reg_bch(frame, ps->remote.access_addr);
        __timer_register(6);
        format = 3;
        break;
    }
    case 2:
        page_scan_step_2();
        format = 3;
        break;
    case 3:
        frame->rxtog = 0;
        format = 3;
        break;
    default:
        return;
    }
    if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]%d \r\n", page_scan->substate);
    struct link_page_scan *p = page_scan;
    p->substate++;
    frame->miscntl = ps->remote.lt_addr;
    __write_reg_format(frame, p->ab_train, format);
    __write_reg_linkcntl(frame, ttog, 16400);
    __write_reg_linkcntl(frame, ttog ^ 1, 16384);
    __write_reg_txphdr(frame, ttog, 2);
    p = page_scan;
    if (p->substate > 2) {
        __timer_remove();
        p->state = 4;
        frame->bufcntl &= ~0xf00;
        frame->bufcntl = frame->bufcntl;
        frame->bufcntl &= ~32;
        frame->bufcntl |= 3;
        frame->txbuf0cntl |= 1;
        frame->txbuf1cntl |= 1;
        p->frame = 0;
        create_link_connection(frame, remote, p->local_addr[p->addr_index], 1, p->last_signed_bitoff, 0, 0);
    }
}

/* Page: ID response (0) -> FHS acknowledged (1) -> POLL; then connected, or
 * the page timeout reports 4. */
static RF_CODE void page_rx_handler(struct ctrl_frame *frame)
{
    struct link_page *pg = page;
    u8 ridx = (frame->rxtog & 1) ^ 1;
    u8 ttog = frame->txtog & 1;
    u16 stat = (&frame->rxstat0)[ridx];
    (void)(&frame->rxphdr0)[ridx];             /* read and unused in the SDK */
    u8 type, format;
    if (RX_OK(stat)) bredr_frame_agc_set(frame, 1, 0);
    if (stat & 15) return;
    if (page_disable_active) return;
    switch (page->substate) {
    case 0:
        __timer_register(16);
        type = 2;
        format = 6;
        break;
    case 1:
        pg->remote.lt_addr = page->lt_addr;
        __write_reg_clkoffset(frame, 0);
        __write_reg_bdaddr(frame, page->local_addr);
        __write_reg_bch(frame, page->local_acc);
        frame->rxtog = 0;
        frame->rxstat0 |= 144;
        frame->rxstat1 |= 144;
        __timer_register(32);
        type = 1;
        format = 2;
        break;
    default:
        type = 1;
        format = 2;
        break;
    }
    page->substate++;
    __write_reg_lt_addr(frame, pg->remote.lt_addr);
    __write_reg_format(frame, page->ab_train, format);
    __write_reg_linkcntl(frame, ttog, type | 16400);
    __write_reg_linkcntl(frame, ttog ^ 1, type | 16384);
    if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]%d \r\n", page->substate);
    __clean_reg_txempty(frame, ttog);
    struct link_page *p = page;
    if (p->substate > 2) {
        __timer_remove();
        pg->remote.clk_offset = 0;
        p->clk_offset = (s16)gbredr_local_dev.clk_base;
        frame->bufcntl &= ~0xf00;
        frame->bufcntl |= 256;
        frame->bufcntl &= ~32;
        frame->bufcntl |= 3;
        frame->txbuf0cntl |= 1;
        frame->txbuf1cntl |= 1;
        p->frame = 0;
        void (*completed)(u8 *, int) = p->completed;
        if (completed) {
            p->completed = 0;
            completed(p->remote.addr, 0);
            p = page;
        }
        create_link_connection(frame, &pg->remote, p->local_addr, 0, 0, 0, 0);
    } else if (p->completed && bredr_clkn_after(READ_SLOT_CLK(), page->timeout)) {
        p = page;
        void (*completed)(u8 *, int) = p->completed;
        p->completed = 0;
        completed(p->remote.addr, 4);
    }
}

/* Fixed rx test: count bits, bit errors and bad packets.  With neither
 * buffer full the SDK indexes the registers with 255. */
static RF_CODE void fix_txrx_handler(struct ctrl_frame *frame)
{
    u8 tog = frame->rxtog & 1;
    u8 rb0 = frame->rxbuf0cntl;
    u8 rb1 = frame->rxbuf1cntl;
    int index;
    if (tog) index = (rb1 & 1) ? 1 : ((rb0 & 1) ? 0 : 255);
    else index = (rb0 & 1) ? 0 : ((rb1 & 1) ? 1 : 255);
    u16 stat = (&frame->rxstat0)[index];
    u16 hdr = (&frame->rxphdr0)[index];
    u32 st = (u32)stat >> 8;
    u8 llid = hdr & 3;
    u16 len = (hdr >> 3) & 1023;
    __write_reg_rxint(frame, index, 0);
    if (st == 143) {
        switch (stat & 15) {
        case 0: case 1: case 2: case 9:
            break;
        default:
            if ((u8)(llid - 1) < 2) fix_txrx->rx_sum_b += (u32)len << 3;
            break;
        }
    } else {
        struct link_fix_rxtx *f = fix_txrx;
        u8 *data = f->rx_data[index];
        if (st == 139) {
            if ((u8)(llid - 1) < 2) {
                u16 err = rd_data(data, len);
                if (err > (u32)len << 1) {
                    f->rx_invail_p += 1;
                } else {
                    f->rx_err_b += err;
                    f->rx_sum_b += (u32)len << 3;
                }
            }
            f->rx_perr_p++;
        } else {
            f->rx_herr_p++;
        }
    }
    __clean_reg_rxfull(frame, index);
}

/* Inquiry: FHS (0), then its EIR (1) if the FHS says one follows.  Each
 * address is reported once (CRC16 of it remembered).  Kept SDK bug: the
 * 30th CRC is stored at result_crc[30], which is result_num and search_num. */
static RF_CODE void inquiry_rx_handler(struct ctrl_frame *frame)
{
    struct link_inquiry *q = inquiry, *iq;
    int idx = (frame->rxtog & 1) ^ 1;
    struct link_inquiry_result *result = &q->result;
    u16 stat = (&frame->rxstat0)[idx];
    u16 len = ((&frame->rxphdr0)[idx] >> 3) & 1023;
    int sta = 0;
    if (inquiry_disable_active) return;
    if (!RX_OK(stat)) {
        if (q->substate != 1) return;
        q->substate = 2;
        sta = 3;
    }
    bredr_frame_agc_set(frame, 1, 0);
    switch (inquiry->substate) {
    case 0:
        if ((stat & 15) != 2) goto restart;
        __read_fhs_packet(&q->result.remote, inquiry->fhs);
        q->result.remote.clk_offset = (s32)(q->result.remote.clkn - __read_reg_clkn(frame));
        q->result.remote.rssi = bredr_analog_get_realtime_rssi();
        if (q->result.remote.eir) {
            inquiry->substate++;
        } else {
            REG32(0x1c00c4) &= 0x7fffffff;
            sta = 1;
        }
        __timer_register(8);
        break;
    case 1:
        switch (stat & 15) {
        case 3: case 4: case 10: case 11: case 14: case 15:
            inquiry->substate = 2;
            REG32(0x1c00c4) &= 0x7fffffff;
            goto report;
        default:
            REG32(0x1c00c4) &= 0x7fffffff;
            goto restart;
        }
    default:
        break;
    }
    if (sta == 1) goto report;
    if (sta == 2) goto restart;                /* never set; as in the SDK */
    goto check_timeout;

report: {
        u16 crc = CRC16(q->result.remote.addr, 6);
        iq = inquiry;
        for (u32 i = 0; i < 30; i++)
            if (crc == iq->result_crc[i]) return;
        if (iq->result_num < 30) {
            u16 *crcs = iq->result_crc;
            iq->result_num++;
            crcs[iq->result_num] = crc;
        }
        if (iq->substate) {
            q->result.eir_data = bt_malloc(240);
            if (q->result.eir_data) inquiry_filter_eir_name(inquiry->eir, len, result);
            else q->result.eir_len = 0;
        }
        inquiry->substate = 2;
        __timer_register(512);
        inquiry->completed(result);
        iq = inquiry;
        if (iq->search_num) {
            if (--iq->search_num == 0) {
                iq->completed(0);
                return;
            }
            goto timeout;
        }
        if (!iq) return;
        goto timeout;
    }
restart:
    inquiry->substate = 2;
    __timer_register(8);
check_timeout:
    if (!inquiry) return;
timeout:
    if (bredr_clkn_after(READ_SLOT_CLK(), inquiry->timeout)) inquiry->completed(0);
}

/* Role switch, new slave side (rs->page): our page response train. */
static RF_CODE void role_switch_page_rx_handler(struct link_connection *lc, struct ctrl_frame *frame)
{
    struct link_page *p = lc->rs->page;
    u8 ridx = (frame->rxtog & 1) ^ 1;
    u8 ttog = frame->txtog & 1;
    u16 stat = (&frame->rxstat0)[ridx];
    (void)(&frame->rxphdr0)[ridx];
    if (!RX_OK(stat)) return;
    if (stat & 15) {
        if (log_tag_const_c_BL) putchar('E');
        return;
    }
    switch (p->substate) {
    case 0:
        __timer_remove();
        bredr_bd_frame_disable(frame);
        REG32(0x1c0000) |= 0x80000000u;
        __timer_register(16);
        p->remote.lt_addr = p->lt_addr;
        __write_reg_clkoffset(frame, 0);
        __write_reg_bdaddr(frame, p->local_addr);
        __write_reg_bch(frame, p->local_acc);
        frame->bitoff = 0;
        frame->rxstat0 |= 144;
        frame->rxstat1 |= 144;
        frame->wincntl = 80;
        frame->miscntl &= 63;
        frame->format &= ~64;
        p->remote.clk_offset = 0;
        bd_frame_odd_even(frame, 0);
        break;
    case 1:
        frame->rxtog = 0;
        bd_frame_odd_even(frame, 0);
        break;
    default:
        break;
    }
    p->substate++;
    lc->rs->rs_timeout_en = 0;
    __write_reg_lt_addr(frame, p->remote.lt_addr);
    __write_reg_format(frame, p->ab_train, 2);
    __write_reg_linkcntl(frame, ttog, 16401);
    __write_reg_linkcntl(frame, ttog ^ 1, 16385);
    __write_reg_txphdr(frame, ttog, 2);
    __write_reg_txphdr(frame, ttog ^ 1, 2);
    __clean_reg_txempty(frame, ttog);
    __clean_reg_txempty(frame, ttog ^ 1);
    __clean_reg_rxfull(frame, ridx);
    if (p->substate > 1) {
        __timer_remove();
        frame->bufcntl &= ~0xf00;
        frame->bufcntl |= 256;
        frame->bufcntl &= ~32;
        frame->bufcntl |= 3;
        frame->txbuf0cntl |= 1;
        frame->txbuf1cntl |= 1;
        bt_free(lc->rs->page);
        bt_free(lc->rs);
        lc->rs = 0;
        lc->role = 0;
        for (int i = 0; i < 2; i++) link_conn_set_rx_bulk_in_irq(lc, i);
        lmp_role_switch_completed(lc, 0);
        bredr_link_event(lc, 2, 4, 0);
    }
}

/* Role switch, new master side (rs->page_scan): take the FHS, align the
 * clock to it, then the first packet in the new timing. */
static RF_CODE void role_switch_page_scan_rx_handler(struct link_connection *lc, struct ctrl_frame *frame)
{
    struct link_page_scan *ps = lc->rs->page_scan;
    u8 rb0 = frame->rxbuf0cntl;
    u8 rb1 = frame->rxbuf1cntl;
    u8 ttog = frame->txtog & 1;
    int idx = 255;
    u16 stat;
    for (;;) {
        if (frame->rxtog & 1) idx = (rb1 & 1) ? 1 : ((rb0 & 1) ? 0 : idx);
        else idx = (rb0 & 1) ? 0 : ((rb1 & 1) ? 1 : idx);
        stat = (&frame->rxstat0)[idx];
        if (RX_OK(stat)) {
            if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]%d\r\n", ps->substate);
            if (ps->substate != 0 || (stat & 15) == 2) break;
        }
        __clean_reg_rxfull(frame, idx);
        if (idx == 0) {
            if (!rb1) return;
            rb0 = 0;
        } else {
            if (!rb0) return;
            rb1 = 0;
        }
    }
    switch (ps->substate) {
    case 0: {
        __read_fhs_packet(&ps->remote, ps->fhs);
        s32 off = (s32)(ps->remote.clkn - __read_reg_clkn(frame));
        ps->remote.clk_offset = off;
        REG32(0x1c0028) = 46;
        frame->wincntl = (u16)(REG32(0x1c0028) + 20);
        u16 so = lc->rs->slot_offset;
        s16 sub = (s16)(so - ((frame->wincntl >> 1) & 2047));
        if (so) ps->remote.clk_offset = off = off + 2;
        if (sub > 624) {
            frame->bitoff = (u16)(sub + 399) & 1023;
            ps->remote.clk_offset = off = off - 1;
        } else if (sub < 0) {
            frame->bitoff = (u16)(sub + 625) & 1023;
            ps->remote.clk_offset = off = off + 1;
        } else {
            frame->bitoff = (u16)sub & 1023;
        }
        lc->last_signed_bitoff = sub;
        bd_frame_odd_even(frame, off & 1 ? 1 : 0);
        __write_reg_clkoffset(frame, ps->remote.clk_offset);
        __write_reg_bdaddr(frame, ps->remote.addr);
        __write_reg_bch(frame, ps->remote.access_addr);
        frame->rxstat0 |= 144;
        frame->rxstat1 |= 144;
        frame->miscntl &= 63;
        frame->format &= ~64;
        lc->rs->rs_timeout_en = 2;
        ps->last_packet_clkn = __read_reg_clkn(frame);
        __clean_reg_rxfull(frame, idx);
        __timer_register(12);
        break;
    }
    case 1: {
        frame->rxbuf0cntl |= 1;
        frame->rxbuf1cntl |= 1;
        frame->rxtog = 0;
        u16 rxbit = frame->rxbit;
        u16 wincntl = frame->wincntl;
        s16 sub = (s16)((s16)((rxbit & 1023) - 68) - ((wincntl >> 1) & 2047));
        frame->bitoff = (u16)((sub < 0 ? sub + 625 : sub) & 1023);
        u16 hi = frame->rxbit >> 13;
        s32 off = (s32)(((u16)sub >> 10) & 1) + (ps->remote.clk_offset - hi);
        ps->remote.clk_offset = off;
        __write_reg_clkoffset(frame, off);
        lc->last_signed_bitoff = sub;
        bd_frame_odd_even(frame, ps->remote.clk_offset & 1 ? 1 : 0);
        lc->rs->rs_timeout_en = 0;
        __timer_remove();
        break;
    }
    default:
        break;
    }
    putchar(ps->substate + '0');
    ps->substate++;
    __write_reg_lt_addr(frame, ps->remote.lt_addr);
    __write_reg_format(frame, ps->ab_train, 3);
    __write_reg_linkcntl(frame, ttog, 16400);
    __write_reg_linkcntl(frame, ttog ^ 1, 16384);
    __write_reg_txphdr(frame, ttog, 2);
    __write_reg_txphdr(frame, ttog ^ 1, 2);
    if (ps->substate > 1) {
        __timer_remove();
        frame->bufcntl &= ~0xf00;
        frame->bufcntl = frame->bufcntl;
        frame->bufcntl &= ~32;
        frame->bufcntl |= 3;
        frame->txbuf0cntl |= 1;
        frame->txbuf1cntl |= 1;
        __put_lt_addr(ps->backup_lt_addr);
        bt_free(lc->rs->page_scan);
        bt_free(lc->rs);
        lc->rs = 0;
        lc->role = 1;
        for (int i = 0; i < 2; i++) link_conn_set_rx_bulk_in_irq(lc, i);
        lmp_role_switch_completed(lc, 0);
        bredr_link_event(lc, 2, 4, 0);
    }
}

/* The baseband's receive interrupt for frame: dispatch to the procedure or
 * connection that owns it. */
RF_CODE void bredr_link_rx_irq_handler(struct ctrl_frame *frame)
{
    struct link_connection *lc;
    int tws = CONFIG_BTCTLER_TWS_ENABLE;
    if (tws && tws_ble && tws_ble->frame == frame) {
        if (config_btctler_le_tws) tws_ble_connect_rx_handler(frame);
        return;
    }
    if (inquiry_scan && inquiry_scan->frame == frame) {
        inquiry_scan_rx_handler(frame);
        return;
    }
    if (page_scan && page_scan->frame == frame) {
        page_scan_rx_handler(frame);
        return;
    }
    if (page && page->frame == frame) {
        page_rx_handler(frame);
        return;
    }
    if (fix_txrx && fix_txrx->frame == frame) {
        fix_txrx_handler(frame);
        return;
    }
    if (inquiry && inquiry->frame == frame) {
        if (CONFIG_BREDR_INQUIRY) inquiry_rx_handler(frame);
        return;
    }
    FOR_EACH_LINK(lc) {
        if (lc->frame != frame) continue;
        if (lc->rs) {
            if (lc->rs->page) {
                if (!tws) role_switch_page_rx_handler(lc, frame);
            } else if (lc->rs->page_scan) {
                role_switch_page_scan_rx_handler(lc, frame);
            }
        } else if (g_dut_hdl) {
            if (CONFIG_TEST_DUT_CODE) dut_rx_handler(lc);
        } else {
            connection_rx_handler(lc, 1);
        }
    }
}

/* ---- timeout interrupt -------------------------------------------------------
 * The slot timer (__timer_register) fires this when a procedure step got no
 * answer in time.  The SDK inlines one handler per procedure. */

extern BTCTRLER_CONFIG int CONFIG_INQUIRY_PAGE_OFFSET_ADJUST;

static RF_CODE void tws_ble_connect_timeout(void)
{
    if (tws_ble->conn_type == 2) return;
    adv_disable_for_connect_timeout();
    scan_disable_for_connect_timeout();
    tws_ble_connect_disable(1);
    link_task_schedule_resume();
}

/* TWS over LE: the LE side gave the peer's clock; start the Classic frame. */
static RF_CODE void tws_ble_connect_start(void)
{
    struct link_tws_ble *b = tws_ble;
    if (b->le_type == 0) {
        tws_bredr_master_connect_enable();
        return;
    }
    struct ctrl_frame *frame = b->frame;
    u32 now = READ_SLOT_CLK();
    bredr_bd_frame_enable(frame, now & 1 ? 1 : 0, 1);
    frame->wincntl = 0x800c;
    __write_reg_clkoffset(frame, (int)(-2 - now + tws_ble->remote.clkn));
    __write_reg_bdaddr(frame, tws_ble->remote.addr);
    __write_reg_bch(frame, tws_ble->remote.access_addr);
    frame->miscntl = tws_ble->remote.lt_addr;
    __write_reg_format(frame, 0, 3);
    __write_reg_linkcntl(frame, 0, 16400);
    __write_reg_linkcntl(frame, 1, 16384);
    __write_reg_txphdr(frame, 0, 2);
    __write_reg_txphdr(frame, 1, 2);
    frame->bufcntl &= ~0xf00;
    frame->bufcntl = frame->bufcntl;
    frame->bufcntl &= ~32;
    frame->bufcntl |= 3;
    frame->txbuf0cntl |= 1;
    frame->txbuf1cntl |= 1;
    tws_ble->state = 2;
    if (tws_ble->conn_type == 2) return;
    if (tws_le_get_esco_indicate()) {
        __timer_register(86);
        tws_le_clr_esco_indicate();
    } else {
        __timer_register(32);
    }
}

static RF_CODE void page_scan_timeout(void)
{
    struct link_page_scan *ps = page_scan;
    if (ps->substate == 1) {
        page_inquiry_clkn += 2048;
        __write_reg_clkoffset(ps->frame, (int)page_inquiry_clkn);
    }
    switch (page_scan->substate) {
    case 2:
        page_scan_step_2();
        break;
    case 0:
        __timer_register(20);
        break;
    default:
        page_scan_resume(0);
        break;
    }
}

static RF_CODE void inquiry_scan_timeout(void)
{
    struct link_inquiry_scan *s = inquiry_scan;
    if (inq_scan_disable_active) return;
    s->state = 3;
    bredr_bd_frame_disable(s->frame);
    REG32(0x1c0000) |= 0x80000000u;
}

/* Inquiry train step: swap the A/B train every 4096 slots; a device whose
 * EIR did not come is counted, and reported without it the fifth time. */
static RF_CODE void inquiry_timeout(void)
{
    struct link_inquiry *q0 = inquiry;
    if (inquiry_disable_active) return;
    struct ctrl_frame *frame = q0->frame;
    u32 now = READ_SLOT_CLK();
    struct link_inquiry *q = inquiry;
    u32 ab = q->ab_clkn;
    if (now - ab + (now > ab ? 0 : 0x8000000u) > 4096) {
        q->ab_clkn = now;
        u8 t = q->ab_train == 0;
        q->ab_train = t;
        frame->format = (u8)(t << 7 | (frame->format & 127));
    }
    if (q->substate == 1) {
        u8 *addr = q0->result.remote.addr;
        u8 find = 0;
        struct inquiry_eir *r = (struct inquiry_eir *)q->eir_list.next;
        for (;;) {
            struct inquiry_eir *n = (struct inquiry_eir *)r->entry.next;
            if (&r->entry == &q->eir_list) break;
            int same = !memcmp(r->addr, addr, 6);
            u8 cnt = r->cnt;
            if (same) {
                if (cnt == 0xff) {
                    find = 1;
                    r = n;
                    continue;
                }
                r->cnt = ++cnt;
                find = 1;
            }
            if (cnt == 5) {
                r->cnt = 0xff;
                q->completed(&q->result);
                q = inquiry;
            }
            r = n;
        }
        if (!find) {
            struct inquiry_eir *e = malloc(16);
            e->cnt = 1;
            memcpy(e->addr, addr, 6);
            list_add_tail(&e->entry, &q->eir_list);
        }
    }
    q->substate = 0;
    if (CONFIG_INQUIRY_PAGE_OFFSET_ADJUST) {
        radio_set_eninv(0);
        REG32(0x1c00c4) |= 0x80000000u;
        s32 d = (s8)offset_step_tb[inquiry->offset_step] - (s32)rf.osc_offset;
        frame->PLL_COMP = (u32)((s32)((u32)d << 20) / 24000);
        radio_set_eninv(1);
        u8 step = (u8)(inquiry->offset_step + 1);
        inquiry->offset_step = step > 9 ? 0 : step;
    } else {
        REG32(0x1c00c4) |= 0x80000000u;
    }
    __timer_register(18);
    if (bredr_clkn_after((int)now, (int)inquiry->timeout)) inquiry->completed(0);
}

/* The lowest free LT address from start (wrapping past 7 to 1); else the
 * plain search. */
static RF_CODE int __get_lt_addr_behind(int start)
{
    u8 used = bredr_link_v.glt_addr;
    for (int i = start > 7 ? 1 : start; i < 8; i++) {
        if (!(used & 1 << i)) {
            bredr_link_v.glt_addr = (u8)(1 << i | used);
            return i;
        }
    }
    return __get_lt_addr();
}

/* Page train step: swap the A/B train every 2048 slots, move to the next LT
 * address, rewrite the FHS; the page timeout reports 4. */
static RF_CODE void page_timeout(void)
{
    struct link_page *p0 = page;
    struct ctrl_frame *frame = p0->frame;
    u8 ttog = frame->txtog & 1;
    if (page_disable_active) return;
    u32 now = READ_SLOT_CLK();
    struct link_page *p = page;
    u32 ab = p->ab_clkn;
    if (now - ab + (now > ab ? 0 : 0x8000000u) > 2048) {
        p->ab_clkn = now;
        u8 t = p->ab_train == 0;
        p->ab_train = t;
        frame->format = (u8)(t << 7 | (frame->format & 127));
    }
    __put_lt_addr(p->lt_addr);
    p = page;
    u8 lt = (u8)__get_lt_addr_behind(p->lt_addr + 1);
    p = page;
    p->lt_addr = lt;
    __write_fhs_packet(p->local_addr, gbredr_local_dev.dev_class, &gbredr_local_dev.eir, lt, p->fhs);
    __write_reg_format(frame, page->ab_train, 4);
    __write_reg_linkcntl(frame, ttog, 16386);
    __clean_reg_txempty(frame, ttog);
    __write_reg_txphdr(frame, ttog, 2);
    if (CONFIG_INQUIRY_PAGE_OFFSET_ADJUST) {
        radio_set_eninv(0);
        REG32(0x1c00c4) |= 0x80000000u;
        s32 d = (s8)offset_step_tb[page->offset_step] - (s32)rf.osc_offset;
        frame->PLL_COMP = (u32)((s32)((u32)d << 20) / 24000);
        radio_set_eninv(1);
        u8 step = (u8)(page->offset_step + 1);
        page->offset_step = step > 9 ? 0 : step;
    }
    page->substate = 0;
    __timer_register(18);
    if (page->completed && bredr_clkn_after((int)READ_SLOT_CLK(), (int)page->timeout)) {
        p = page;
        void (*completed)(u8 *, int) = p->completed;
        p->completed = 0;
        completed(p->remote.addr, 4);
    }
}

/* Role switch (rs->page): the first timeout restarts the page response from
 * the backup LT address; the second gives up (role switch result 1). */
static RF_CODE void role_switch_page_timeout(struct link_connection *lc)
{
    struct bredr_role_switch *rs = lc->rs;
    struct link_page *pg = rs->page;
    struct ctrl_frame *frame = lc->frame;
    (void)frame->rxtog;
    u8 ttog = frame->txtog & 1;
    if (rs->rs_timeout_en) {
        rs->rs_timeout_en = 0;
        __timer_register(16);
        struct ctrl_frame *f = lc->frame;
        struct link_page *p = lc->rs->page;
        u8 t = f->txtog & 1;
        __write_reg_clkoffset(f, 0);
        __write_reg_bdaddr(f, p->local_addr);
        __write_reg_bch(f, p->local_acc);
        f->bitoff = 0;
        f->rxtog = 0;
        f->rxstat0 |= 144;
        f->rxstat1 |= 144;
        f->wincntl = 80;
        f->miscntl &= 63;
        f->format &= ~64;
        p->remote.clk_offset = 0;
        p->substate++;
        p->remote.lt_addr = p->backup_lt_addr;
        __write_reg_lt_addr(f, p->backup_lt_addr);
        __write_reg_format(f, p->ab_train, 2);
        __write_reg_linkcntl(f, t, 16401);
        __write_reg_linkcntl(f, t ^ 1, 16385);
        __write_reg_txphdr(f, t, 2);
        __write_reg_txphdr(f, t ^ 1, 2);
        bd_frame_odd_even(f, 0);
        return;
    }
    if (log_tag_const_c_BL) putchar('F');
    __write_reg_clkoffset(frame, 0);
    __write_reg_lt_addr(frame, pg->remote.lt_addr);
    __write_reg_format(frame, pg->ab_train, 2);
    __write_reg_linkcntl(frame, ttog, 16401);
    __write_reg_linkcntl(frame, ttog ^ 1, 16385);
    __write_reg_txphdr(frame, ttog, 2);
    __write_reg_txphdr(frame, ttog ^ 1, 2);
    for (int i = 0; i < 2; i++) link_conn_set_rx_bulk_in_irq(lc, i);
    frame->bitoff = 0;
    frame->txtog = 0;
    frame->txbuf0cntl |= 1;
    frame->txbuf1cntl |= 1;
    __timer_remove();
    frame->bufcntl &= ~0xf00;
    frame->bufcntl |= 256;
    bd_frame_odd_even(frame, 0);
    bt_free(lc->rs->page);
    bt_free(lc->rs);
    lc->rs = 0;
    lmp_role_switch_completed(lc, 1);
    bredr_link_event(lc, 2, 4, 1);
}

/* Role switch (rs->page_scan): 1 waits once more, 2 means the FHS came
 * (role_switch_page_scan), else give up and go back to the old timing. */
static RF_CODE void role_switch_page_scan_timeout(struct link_connection *lc)
{
    struct bredr_role_switch *rs = lc->rs;
    struct link_page_scan *ps = rs->page_scan;
    struct ctrl_frame *frame = lc->frame;
    (void)frame->rxtog;
    u8 ttog = frame->txtog & 1;
    switch (rs->rs_timeout_en) {
    case 1:
        rs->rs_timeout_en = 0;
        __timer_register(8);
        return;
    case 2:
        role_switch_page_scan(lc);
        return;
    }
    if (log_tag_const_c_BL) putchar('F');
    __write_reg_clkoffset(frame, 0);
    ps->remote.lt_addr = ps->backup_lt_addr;
    __write_reg_lt_addr(frame, ps->backup_lt_addr);
    __write_reg_format(frame, ps->ab_train, 2);
    __write_reg_linkcntl(frame, 0, 16401);
    __write_reg_linkcntl(frame, 1, 16385);
    __write_reg_bdaddr(frame, hci_param.addr);
    __write_reg_bch(frame, ps->local_acc[0]);
    __write_reg_txphdr(frame, ttog, 2);
    __write_reg_txphdr(frame, ttog ^ 1, 2);
    for (int i = 0; i < 2; i++) link_conn_set_rx_bulk_in_irq(lc, i);
    frame->bitoff = 0;
    frame->wincntl = 80;
    frame->txtog = 0;
    frame->txbuf0cntl |= 1;
    frame->txbuf1cntl |= 1;
    __timer_remove();
    frame->bufcntl &= ~0xf00;
    frame->bufcntl |= 256;
    frame->bufcntl |= 3;
    bd_frame_odd_even(frame, 0);
    bt_free(lc->rs->page_scan);
    bt_free(lc->rs);
    lc->rs = 0;
    lmp_role_switch_completed(lc, 1);
    bredr_link_event(lc, 2, 4, 1);
}

RF_CODE void bredr_link_timeout_irq_handler(void)
{
    struct link_connection *lc;
    struct link_tws_ble *b = tws_ble;
    if (b) {
        if (b->state == 2) {
            if (config_btctler_le_tws) tws_ble_connect_timeout();
        } else if (b->state == 1 && config_btctler_le_tws) {
            tws_ble_connect_start();
        }
    }
    if (page_scan && page_scan->state == 2) page_scan_timeout();
    if (inquiry_scan && inquiry_scan->state == 2) inquiry_scan_timeout();
    if (inquiry && inquiry->state == 2 && CONFIG_BREDR_INQUIRY) inquiry_timeout();
    if (page && page->state == 2) page_timeout();
    FOR_EACH_LINK(lc) {
        struct bredr_role_switch *rs = lc->rs;
        if (rs) {
            if (rs->page) {
                if (!CONFIG_BTCTLER_TWS_ENABLE) role_switch_page_timeout(lc);
            } else if (rs->page_scan) {
                role_switch_page_scan_timeout(lc);
            }
        } else if (lc->state == 2) {
            link_connection_timeout(lc);
        }
    }
}

/* ---- link helpers ------------------------------------------------------------ */

u8 low_power_get_default_osc_type(void);
u8 low_power_get_osc_type(void);
void low_power_reset_osc_type(u8 type);

RF_CODE void lc_write_ptt(struct link_connection *lc, u8 en)
{
    u8 v = lc->frame->miscntl;
    lc->frame->miscntl = en ? (u8)(v | 8) : (u8)(v & ~8);
}

RF_CODE void link_conn_set_encrypt(struct link_connection *lc, u8 rx_enc, u8 tx_enc)
{
    __write_reg_encry(lc->frame, rx_enc, tx_enc);
}

RF_CODE void link_conn_set_encrypt_key(struct link_connection *lc, u8 *key)
{
    struct ctrl_frame *frame = lc->frame;
    for (int i = 0; i < 8; i++) frame->cryptk[i] = (u16)(key[i * 2 + 1] << 8 | key[i * 2]);
}

/* The bit offset within the slot; as master corrected by the window. */
__attribute__((used)) RAM_CODE u32 lc_local_slot_bitoff(struct link_connection *lc)
{
    struct ctrl_frame *frame = lc->frame;
    int role = lc->role;
    u16 off = frame->bitoff;
    if (role) off = (u16)(off - 16 + ((frame->wincntl >> 1) & 2047));
    return (u32)(s32)(s16)off;
}

/* Microseconds to the next local slot boundary (odd clock offsets shift by a
 * slot). */
RF_CODE u32 lc_local_slot_offset(struct link_connection *lc)
{
    int clk = __read_reg_clkoffset(lc->frame);
    s16 b = (s16)lc_local_slot_bitoff(lc);
    if (clk & 1) return (u32)(b < 626 ? 625 - b : 1250 - b % 625);
    return (u32)(b < 626 ? 1250 - b : 625 - b % 625);
}

/* Low-power oscillator for the next sniff anchor (lc_lrc_osc_change in the
 * SDK): mode 1 uses the BT oscillator while any link waits for a sniff
 * packet, mode 2 always. */
static RF_CODE void lc_lrc_osc_change(void)
{
    struct link_connection *t;
    u8 src = 2;
    switch (bredr_link_v.lp_ws->osc_change_mode) {
    case 0:
        return;
    case 1:
        src = low_power_get_default_osc_type();
        FOR_EACH_LINK(t)
            if (t->frame && t->sniff_rx_flag) src = 2;
        break;
    default:
        break;
    }
    low_power_reset_osc_type(src);
    FOR_EACH_LINK(t)
        if (t->frame) t->lp_cur_osc = src;
}

/* Sniff anchor control from the link manager: 1 start a sniff receive
 * window, 2 stop the frame, 3 widen the window for the next anchor (slave;
 * returns the slots moved + 2, or 4 as master), 5 enter sniff, 6 leave. */
RF_CODE s32 lc_sniff_ctrl(struct link_connection *lc, SNIFF_CTRL_CMD cmd, u32 *parm)
{
    struct ctrl_frame *frame = lc->frame;
    __read_reg_clkoffset(frame);               /* unused in the SDK */
    switch ((int)cmd) {
    case 1:
        REG32(0x1c0000) |= 1024;
        lc->wide_win_flag = 1;
        lc->adjust_clkoffset_enable = 1;
        lc->sniff_rx_flag = 1;
        return 0;
    case 2:
        bredr_bd_frame_disable(frame);
        return 0;
    case 3: {
        lc->adjust_clkoffset_enable = 0;
        struct ctrl_frame *f = lc->frame;
        u8 def = low_power_get_default_osc_type();
        lc_lrc_osc_change();
        if (!lc->role) return 4;
        lc->lp_last_osc = lc->lp_cur_osc;
        u32 drift, total;
        if (!lc->sniff_rx_flag) {
            drift = total = lc->winsize;
        } else {
            u16 *inc;
            if (def == 2) {
                if (lc->btosc_winsize_cnt == 0) lc->winsize = 0;
                inc = &bredr_link_v.lp_ws->bt_osc_ws_inc;
            } else {
                inc = &bredr_link_v.lp_ws->lrc_ws_inc;
            }
            drift = *inc;
            if (parm && sniff_support_reset_anchor_point) drift = *parm == (u32)sniff_long_interval ? drift : 20;
            u32 ws = lc->winsize;
            total = ws + drift;
            if (!(total < 4075 - REG32(0x1c0028))) {
                total = 4095 - REG32(0x1c0028) - 20;
                drift = total > ws ? total - ws : 0;
            }
            lc->winsize = (u16)total;
        }
        if (low_power_get_osc_type() == 2) lc->btosc_winsize_cnt++;
        u32 reg = REG32(0x1c0028);
        u32 clkn_drift = drift / 1250;
        s32 bit = (s32)(f->bitoff & 1023) - (s32)((drift >> 1) % 625);
        f->wincntl = (u16)(total + 20 + reg);
        if (bit < 0) {
            clkn_drift++;
            bit += 625;
        }
        f->bitoff = (u16)bit & 1023;
        lc->wide_win_flag = 1;
        f->bitoff |= 16384;
        __write_reg_clkoffset(f, bredr_link_clk_offset(lc) + (int)clkn_drift);
        return (int)clkn_drift + 2;
    }
    case 5: {
        u8 osc = low_power_get_default_osc_type();
        lc->lp_cur_osc = osc;
        lc->lp_last_osc = osc;
        struct lp_ws_t *ws = bredr_link_v.lp_ws;
        lc->winsize = osc == 2 ? ws->bt_osc_ws_init : ws->lrc_ws_init;
        lc->sniff_sta = 1;
        return 0;
    }
    case 6:
        lc->adjust_clkoffset_enable = 1;
        lc->sniff_sta = 0;
        return 0;
    default:
        if (log_tag_const_e_BL) log_print(4, NULL, "<Error>: [BL]unknow_cmd !!!\n\r\n");
        return 0;
    }
}

RF_CODE void lc_qos_ctrl(struct link_connection *lc, int enable)
{
    (void)lc;
    (void)enable;
}

/* More than one non-TWS connection (always 0 with TWS). */
RF_CODE int link_conn_num_more_than_one(void)
{
    struct link_connection *lc;
    if (CONFIG_BTCTLER_TWS_ENABLE) return 0;
    int n = 0;
    FOR_EACH_LINK(lc)
        if (lc->frame) n += !lc->tws_link;
    return n > 1;
}

/* ---- role switch ---------------------------------------------------------- */

/* Become master: page the old master in our own timing (role_switch_page in
 * the SDK, inlined), aligning the new slot to the old one's bit offset. */
RF_CODE void bredr_switch_role_to_master(struct link_connection *link, int offset)
{
    struct link_page *pg = bt_malloc(168);
    if (!pg) btctrler_assert_failed("rs_page", 0);
    struct bredr_role_switch *rs = bt_malloc(20);
    if (!rs) btctrler_assert_failed("rs", 0);
    rs->slot_offset = (u16)offset;
    rs->page = pg;
    link->rs = rs;
    struct ctrl_frame *frame = link->frame;
    pg->backup_lt_addr = frame->miscntl & 7;

    struct link_page *p = rs->page;
    if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]role_switch_page\n\r\n");
    u8 ttog = frame->txtog & 1;
    memcpy(p->remote.addr, link->core.remote_addr, 6);
    memcpy(p->local_addr, hci_param.addr, 6);
    __get_access_addr(p->remote.access_addr, link->core.remote_addr);
    __get_access_addr(p->local_acc, p->local_addr);
    p->substate = 0;
    p->ab_clkn = READ_SLOT_CLK();
    u8 lt = (u8)__get_lt_addr();
    p->lt_addr = lt;
    p->clk_offset = (s16)gbredr_local_dev.clk_base;
    p->remote.sr = link->core.remote_sr;
    p->remote.clk_offset = 0;
    p->frame = frame;
    p->state = 1;
    __write_fhs_packet(p->local_addr, gbredr_local_dev.dev_class, &gbredr_local_dev.eir, lt, p->fhs);
    u16 bitoff = frame->bitoff;
    u16 wincntl = frame->wincntl;
    u32 sum = (u32)bitoff + 65520 + ((wincntl & 4094) >> 1);
    s16 sb = (s16)sum;
    if ((sum & 0xffff) == 625) {
        p->remote.clk_offset++;
        frame->bitoff = 0;
        sb = 0;
        goto check_neg;
    }
    frame->bitoff = (u16)((sb < 0 ? sb + 625 : sb) & 1023);
    if (sb < 0) {
        if ((u16)link->last_signed_bitoff < 312) {
            p->remote.clk_offset++;
            __write_reg_clkoffset(frame, p->remote.clk_offset);
        }
    } else if ((u16)sb < 312) {
check_neg:
        if (link->last_signed_bitoff < 0) {
            p->remote.clk_offset--;
            __write_reg_clkoffset(frame, p->remote.clk_offset);
        }
    }
    link->last_signed_bitoff = sb;
    frame->wincntl = 80;
    __write_reg_txptr(frame, ttog, p->fhs);
    __write_reg_format(frame, p->ab_train, 2);
    __write_reg_linkcntl(frame, ttog, 16386);
    __write_reg_linkcntl(frame, ttog ^ 1, 16401);
    __write_reg_txphdr(frame, ttog, 2);
    __write_reg_txphdr(frame, ttog ^ 1, 2);
    __clean_reg_txempty(frame, ttog);
    __clean_reg_txempty(frame, ttog ^ 1);
    __write_reg_lmprxptr(frame, 0, p->fhs);
    __write_reg_lmprxptr(frame, 1, p->fhs);
    __write_reg_rxptr(frame, 0, p->fhs);
    __write_reg_rxptr(frame, 1, p->fhs);
    __clean_reg_rxfull(frame, 0);
    __clean_reg_rxfull(frame, 1);
    link->rs->rs_timeout_en = 1;
    __timer_register(16);
    bredr_pwr_set(link, 0, 15);
}

/* Become slave: page scan for the new master's FHS on our address. */
RF_CODE void role_switch_page_scan(struct link_connection *link)
{
    struct ctrl_frame *frame = link->frame;
    struct link_page_scan *ps = link->rs->page_scan;
    u8 rtog = frame->rxtog & 1;
    u8 ttog = frame->txtog & 1;
    frame->wincntl = 80;
    s16 sub = (s16)(5 - ((frame->wincntl >> 1) & 2047));
    link->last_signed_bitoff = 0;
    ps->remote.clk_offset = 0;
    ps->substate = 0;
    if (sub < 0) {
        frame->bitoff = (u16)(sub + 625) & 1023;
        ps->remote.clk_offset = 1;
        __write_reg_clkoffset(frame, 1);
        bd_frame_odd_even(frame, 1);
    } else {
        frame->bitoff = (u16)sub & 1023;
        bd_frame_odd_even(frame, 0);
    }
    link->last_signed_bitoff = sub;
    __write_reg_lmprxptr(frame, 0, ps->fhs);
    __write_reg_lmprxptr(frame, 1, ps->fhs);
    __write_reg_rxptr(frame, 0, ps->fhs);
    __write_reg_rxptr(frame, 1, ps->fhs);
    memcpy(ps->local_addr[0], hci_param.addr, 6);
    __get_access_addr(ps->local_acc[0], hci_param.addr);
    __write_reg_lt_addr(frame, ps->backup_lt_addr);
    __write_reg_format(frame, ps->ab_train, 3);
    __write_reg_linkcntl(frame, ttog, 16384);
    __write_reg_linkcntl(frame, ttog ^ 1, 16400);
    __write_reg_bdaddr(frame, hci_param.addr);
    __write_reg_bch(frame, ps->local_acc[0]);
    __write_reg_clkoffset(frame, ps->remote.clk_offset);
    __write_reg_txphdr(frame, ttog, 2);
    __write_reg_txphdr(frame, ttog ^ 1, 2);
    frame->bufcntl &= ~0xf00;
    frame->bufcntl = frame->bufcntl;
    frame->txbuf0cntl |= 1;
    frame->txbuf1cntl |= 1;
    link->rs->rs_timeout_en = 1;
    __timer_register(32);
    if (!rtog) {
        __clean_reg_rxfull(frame, 1);
        frame->rxbuf0cntl |= 1;
    } else {
        __clean_reg_rxfull(frame, 0);
        frame->rxbuf1cntl |= 1;
    }
    bredr_pwr_set(link, 0, 15);
}

RF_CODE void bredr_switch_role_to_slave(struct link_connection *link, int offset)
{
    struct link_page_scan *ps = bt_malloc(120);
    if (!ps) btctrler_assert_failed("rs_page_scan", 0);
    struct bredr_role_switch *rs = bt_malloc(20);
    if (!rs) btctrler_assert_failed("rs", 0);
    rs->slot_offset = (u16)offset;
    rs->page_scan = ps;
    ps->fhs = bd_base.rx_fhs;
    link->rs = rs;
    ps->backup_lt_addr = link->frame->miscntl & 7;
    role_switch_page_scan(link);
}

/* ---- clocks ------------------------------------------------------------------- */

RF_CODE u32 bredr_read_slot_clk(void)
{
    return READ_SLOT_CLK();
}

RF_CODE u32 bredr_finetimecnt(void)
{
    return GET_FINETIMECNT();
}

/* Close lc (if any); the remaining links may use all of the rx buffer. */
RF_CODE void link_conn_close(struct link_connection *lc)
{
    struct link_connection *p;
    if (lc) link_connection_close(lc);
    FOR_EACH_LINK(p) {
        if (!p->frame) continue;
        if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]lc:0x%x close,only one bd:0x%x\n\r\n", lc, p);
        if (!CONFIG_BTCTLER_TWS_ENABLE) link_conn_set_max_rx_bulk_persent(p, 100);
    }
}

/* The master's slot clock (local + offset, mod 2^27) and the local one. */
RF_CODE void bredr_get_link_slot_clk(struct link_connection *lc, u32 *mclk, u32 *lclk)
{
    int now = (int)READ_SLOT_CLK();
    int m = __read_reg_clkoffset(lc->frame) + now;
    if (m < 0) m += 0x8000000;
    else if (m > 0x7ffffff) m -= 0x8000000;
    if (mclk) *mclk = (u32)m;
    if (lclk) *lclk = (u32)now;
}

RF_CODE u32 bredr_get_master_slot_clk(struct link_connection *lc)
{
    u32 m;
    bredr_get_link_slot_clk(lc, &m, 0);
    return m;
}

/* ---- AFH switch ---------------------------------------------------------------- */

RF_CODE void bredr_link_enable_afh(void *_lc)
{
    struct link_connection *lc = _lc;
    local_irq_disable();
    if (lc->afh_timer != 0xff) {
        slot_timer_put(lc->afh_timer);
        lc->afh_timer = 0xff;
    }
    local_irq_enable();
    lc->channel_map_update = 0;
    bdhw_set_afh(lc->frame, lc->channel_map);
}

/* Switch to map at the AFH instant (slot_end): now if the instant is past
 * or more than 69120000 slots off, else from a slot timer two slots before
 * it.  The SDK records afh_slot_end only with TWS. */
RF_CODE void bredr_link_set_afh(struct link_connection *lc, u32 slot_end, u8 enable, u8 *map)
{
    struct ctrl_frame *frame = lc->frame;
    lc->afh.en = enable;
    if (!enable) {
        bdhw_disable_afh(frame);
        return;
    }
    if (lc->channel_map_update) {
        if (lc->afh_slot_end == slot_end) return;
        bredr_link_enable_afh(lc);
    }
    memcpy(lc->channel_map, map, 10);
    u32 end = (u32)lmp_sniff_and_afh_offset_ali(lc, slot_end) + slot_end;
    if (CONFIG_BTCTLER_TWS_ENABLE) lc->afh_slot_end = end;
    u32 m = bredr_get_master_slot_clk(lc);
    u32 sub = end + 134217726;
    u32 at = sub & 0x7ffffff;
    u32 off = 2;
    if (m > at) {
        if (!(((m - at) & 0xffffffe) > 69120000)) goto timer;
        at = sub | 0x8000000;
    }
    off = (at - m) & 0xffffffe;
    if (!(off > 2)) off = 2;
    if (off > 69120000) {
        bdhw_set_afh(frame, lc->channel_map);
        return;
    }
timer:
    if (lc->afh_timer == 0xff) lc->afh_timer = slot_timer_get();
    lc->channel_map_update = 1;
    slot_timer_set(lc->afh_timer, bredr_link_enable_afh, lc, (int)off);
}

/* ---- eSCO helpers -------------------------------------------------------------- */

RF_CODE u8 bredr_get_esco_packet_type(u8 packet)
{
    switch (packet) {
    case 0: case 7: case 12: case 13:
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

RF_CODE void bredr_esco_link_set_channel(struct link_connection *lc, u8 channel, u8 rsvd, u8 next)
{
    (void)lc;
    radio_set_channel(channel);
    radio_set_esco_next(next);
    radio_set_esco_rsvd(rsvd);
}

__attribute__((weak)) RF_CODE void esco_rx_notice_to_decode(void)
{
}

RF_CODE void esco_rx_notice_to_do(void)
{
    os_sem_post((OS_SEM *)&esco_sem);
    esco_rx_notice_to_decode();
}

RF_CODE void bredr_esco_close_slot_time(struct link_connection *lc)
{
    if (!lc->esco) return;
    if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]bredr_esco_close_slot_time\n\r\n");
    local_irq_disable();
    __pcm_out_disable(0);
    __pcm_out_disable(1);
    if (lc->esco->timer0) {
        slot_timer_put(lc->esco->timer0);
        lc->esco->timer0 = 0;
    }
    if (lc->esco->timer1) {
        slot_timer_put(lc->esco->timer1);
        lc->esco->timer1 = 0;
    }
    _write_reg_esco_clkalign(lc->frame, 0);
    radio_set_channel(3);
    radio_set_esco_next(0);
    radio_set_esco_rsvd(0);
    local_irq_enable();
}

__attribute__((used)) RAM_CODE int bredr_link_clk_offset(struct link_connection *lc)
{
    return __read_reg_clkoffset(lc->frame);
}

/* ---- TWS -------------------------------------------------------------------------- */

RF_CODE void tws_remote_state_setting(void)
{
    bredr_link_v.remote_device_state = 1;
}

/* TWS over LE, slave: the first packet sets the clock offset and bit
 * offset.  Returns the step done (0 first). */
RF_CODE u8 tws_ble_slave_step_handler(struct bredr_remote_device *remote, struct ctrl_frame *frame)
{
    u8 step = tws_ble->substate;
    if (step == 0) {
        frame->rxtog = 0;
        frame->rxstat0 |= 128;
        frame->rxstat1 |= 128;
        REG32(0x1c0028) = 46;
        frame->wincntl = (u16)(REG32(0x1c0028) + 20);
        u16 rxbit = frame->rxbit;
        u16 wincntl = frame->wincntl;
        s16 sub = (s16)((s16)((rxbit & 1023) - 68) - ((wincntl >> 1) & 2047));
        frame->bitoff = (u16)((sub < 0 ? sub + 625 : sub) & 1023);
        u32 clkn = remote->clkn;
        u16 lo = frame->rx_clknl;
        u16 hi = frame->rx_clknh;
        s32 off = (s32)(clkn - ((u32)hi << 16 | lo)) + (((u16)sub >> 15) & 1);
        remote->clk_offset = off;
        __write_reg_clkoffset(frame, off);
        tws_ble->last_signed_bitoff = sub;
        bredr_bd_frame_enable(frame, remote->clk_offset & 1 ? 1 : 0, 1);
        __timer_register(30);
        step = tws_ble->substate;
    }
    tws_ble->substate = (u8)(step + 1);
    return step;
}

RF_CODE u8 tws_ble_master_step_handler(struct ctrl_frame *frame)
{
    (void)frame;
    u8 step = tws_ble->substate;
    if (step == 0) {
        __timer_register(36);
        step = tws_ble->substate;
    }
    tws_ble->substate = (u8)(step + 1);
    return step;
}

/* What the LE advertising tells the TWS peer: LT address, our clock 16
 * slots ahead (even), offset 9, the internal address, search data. */
RF_CODE void tws_ble_get_connect_info(struct tws_conn_info *tc_info)
{
    local_irq_disable();
    u8 lt = gbredr_local_dev.tws_ble_lt_addr;
    if (!lt) {
        lt = (u8)__get_lt_addr();
        gbredr_local_dev.tws_ble_lt_addr = lt;
    }
    tc_info->lt_addr = lt;
    gbredr_local_dev.tws_mclkn_adjust = 0;
    u32 clkn = READ_SLOT_CLK() + 16;
    if (clkn & 1) {
        clkn++;
        gbredr_local_dev.tws_mclkn_adjust = 1;
    }
    tc_info->clkn = clkn;
    gbredr_local_dev.tws_ble_mclkn = clkn;
    tc_info->clk_offset = 9;
    memcpy(tc_info->addr, gbredr_local_dev.internal_addr_local, 6);
    if (fast_search_data) memcpy(tc_info->data, fast_search_data, 14);
    local_irq_enable();
}

RF_CODE void tws_ble_clr_connect_info(void)
{
    local_irq_disable();
    u8 lt = gbredr_local_dev.tws_ble_lt_addr;
    if (lt) {
        __put_lt_addr(lt);
        gbredr_local_dev.tws_ble_lt_addr = 0;
    }
    local_irq_enable();
    tws_ble_connect_disable(1);
    link_task_schedule_resume();
}

RF_CODE void tws_ble_connect_disable(int type)
{
    local_irq_disable();
    if (!tws_ble) {
        local_irq_enable();
        return;
    }
    if (type) {
        u8 lt = gbredr_local_dev.tws_ble_lt_addr;
        if (lt) {
            __put_lt_addr(lt);
            gbredr_local_dev.tws_ble_lt_addr = 0;
        }
        if (tws_ble->frame) bredr_bd_put_frame(tws_ble->frame);
    }
    bt_free(tws_ble);
    tws_ble = 0;
    local_irq_enable();
    if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]tws ble close\n\r\n");
}

/* TWS over LE, master side (after advertising): a frame on the internal
 * addresses waiting for the slave's first packet. */
RF_CODE void tws_adv_write_connect_info(u8 role, u8 conn_type)
{
    if (fast_search_data) return;
    local_irq_disable();
    if (tws_ble) {
        local_irq_enable();
        return;
    }
    struct ctrl_frame *frame = bredr_bd_get_frame();
    if (!frame) {
        local_irq_enable();
        return;
    }
    struct link_tws_ble *b = bt_malloc(68);
    tws_ble = b;
    if (!b) {
        bredr_bd_put_frame(frame);
        local_irq_enable();
        return;
    }
    b->frame = frame;
    u8 lt = gbredr_local_dev.tws_ble_lt_addr;
    b->remote.lt_addr = lt;
    b->lt_addr = lt;
    b->remote.clk_offset = 0;
    b->remote.sr = gbredr_local_dev.eir.sr;
    b->remote.eir = gbredr_local_dev.eir.enable;
    b->remote.class_dev = gbredr_local_dev.dev_class;
    memcpy(b->remote.addr, gbredr_local_dev.internal_addr_remote, 6);
    memcpy(b->local_addr, gbredr_local_dev.internal_addr_local, 6);
    __get_access_addr(b->remote.access_addr, gbredr_local_dev.internal_addr_remote);
    __get_access_addr(tws_ble->local_acc, gbredr_local_dev.internal_addr_local);
    b = tws_ble;
    b->state = 1;
    b->role_type = role;
    b->le_type = 0;
    b->conn_type = conn_type;
    frame = b->frame;
    __write_reg_clkoffset(frame, gbredr_local_dev.tws_mclkn_adjust ? 1 : 0);
    __write_reg_bdaddr(frame, tws_ble->local_addr);
    __write_reg_bch(frame, tws_ble->local_acc);
    frame->rxtog = 0;
    frame->rxstat0 |= 144;
    frame->rxstat1 |= 144;
    __write_reg_lt_addr(frame, tws_ble->lt_addr);
    __write_reg_format(frame, 0, 2);
    __write_reg_linkcntl(frame, 0, 16401);
    __write_reg_linkcntl(frame, 1, 16385);
    frame->bufcntl &= ~0xf00;
    frame->bufcntl |= 256;
    frame->bufcntl &= ~32;
    frame->bufcntl |= 3;
    frame->txbuf0cntl |= 1;
    frame->txbuf1cntl |= 1;
    REG32(0x1c0028) = 46;
    frame->wincntl = (u16)(REG32(0x1c0028) + 20);
    tws_remote_state_setting();
    if (tws_ble->conn_type == 2) tws_bredr_master_connect_enable();
    else __timer_register(2);
    local_irq_enable();
    if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]master\n\r\n");
}

/* TWS over LE, slave side (after scanning): the master's connect info from
 * its advertising on channel (37..39). */
RF_CODE void tws_scan_write_connect_info(u8 channel, u8 *info, u8 role)
{
    (void)role;
    struct tws_conn_info tc;
    local_irq_disable();
    memcpy(&tc, info, 31);
    tws_remote_state_setting();
    if (!tws_host_channel_match((char)tc.channel)) {
        link_task_schedule_resume();
        local_irq_enable();
        if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]channel is same\n\r\n");
        return;
    }
    if (tc.working_mode == 0x55aa) tws_le_set_esco_indicate();
    if (tws_ble) {
        if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]ble slave is err\n\r\n");
        local_irq_enable();
        return;
    }
    struct ctrl_frame *frame = bredr_bd_get_frame();
    if (!frame) btctrler_assert_failed("0 frame is err", 0);
    tws_ble = bt_malloc(68);
    if (!tws_ble) {
        bredr_bd_put_frame(frame);
        btctrler_assert_failed("0 tws_ble no ram", 0);
    }
    u16 off = (u16)tc.clk_offset;
    __timer_register((int)((off + 248 + channel_delay[channel - 37]) & 255));
    struct link_tws_ble *b = tws_ble;
    b->remote.clk_offset = (s16)off;
    b->remote.clkn = tc.clkn;
    b->frame = frame;
    b->remote.sr = gbredr_local_dev.eir.sr;
    b->remote.eir = gbredr_local_dev.eir.enable;
    b->remote.lt_addr = tc.lt_addr & 7;
    b->remote.class_dev = gbredr_local_dev.dev_class;
    memcpy(b->remote.addr, tc.addr, 6);
    __get_access_addr(b->remote.access_addr, tc.addr);
    b = tws_ble;
    memcpy(b->local_addr, gbredr_local_dev.internal_addr_local, 6);
    __get_access_addr(b->local_acc, gbredr_local_dev.internal_addr_local);
    b = tws_ble;
    b->state = 1;
    b->role_type = tc.role_type == 0;
    b->le_type = 1;
    local_irq_enable();
    if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]slave\n\r\n");
}

RF_CODE void tws_link_wait_connection_resume(struct link_task *task)
{
    struct link_connection *lc = LC_OF_TASK(task);
    if (lc->tws_link || !tws_ble) return;
    lc->state = 2;
    if (__read_reg_clkoffset(lc->frame) & 1) bredr_bd_frame_enable(lc->frame, 1, 1);
    else bredr_bd_frame_enable(lc->frame, 0, 1);
}

RF_CODE void tws_link_wait_connection_suspend(struct link_task *task)
{
    struct link_connection *lc = LC_OF_TASK(task);
    if (lc->tws_link || !tws_ble) return;
    lc->state = 3;
    bredr_bd_frame_disable(lc->frame);
}

/* The SDK loads tws_ble->frame before the null check; here it is loaded
 * after it, so the compiler cannot drop the check. */
RF_CODE void tws_bredr_master_connect_resume(void)
{
    struct link_tws_ble *b = tws_ble;
    if (!b) return;
    bredr_bd_frame_enable(b->frame, gbredr_local_dev.tws_mclkn_adjust ? 1 : 0, 1);
}

RF_CODE void tws_bredr_master_connect_suspend(void)
{
    if (!tws_ble) return;
    bredr_bd_frame_disable(tws_ble->frame);
}

static RF_CODE void tws_bredr_master_connect_enable(void)
{
    struct ctrl_frame *frame = tws_ble->frame;
    bredr_bd_frame_enable(frame, gbredr_local_dev.tws_mclkn_adjust ? 1 : 0, 1);
    tws_ble->state = 2;
    if (tws_ble->conn_type != 2) __timer_register(32);
}

RF_CODE int link_tws_ble_inquiry_scan_enable(void)
{
    if (tws_inquiry_scan) return -22;
    local_irq_disable();
    struct ctrl_frame *frame = bredr_bd_get_frame();
    if (!frame) {
        local_irq_enable();
        return -12;
    }
    tws_inquiry_scan = bt_malloc(304);
    if (!tws_inquiry_scan) {
        bredr_bd_put_frame(frame);
        local_irq_enable();
        return -12;
    }
    frame->miscntl = 0;
    frame->wincntl = 0;
    frame->bitoff = 0;
    __write_reg_clkoffset(frame, 0);
    __write_reg_bdaddr(frame, ble_local_lap);
    __write_reg_bch(frame, ble_local_bch);
    __write_reg_format(frame, 0, 11);
    struct link_inquiry_scan *s = tws_inquiry_scan;
    s->substate = 0;
    s->frame = frame;
    s->state = 1;
    s->task.type = 2;
    s->task.run_slot = 26;
    s->task.run_num = 1;
    s->task.priority = 1;
    s->task.ops = &tws_inquiry_scan_task_ops;
    s->task.sta_attribute = 1;
    link_task_add(&s->task);
    local_irq_enable();
    return 0;
}

RF_CODE void link_tws_inquiry_scan_disable(void)
{
    if (!tws_inquiry_scan) return;
    local_irq_disable();
    link_task_del(&tws_inquiry_scan->task);
    bredr_bd_put_frame(tws_inquiry_scan->frame);
    bt_free(tws_inquiry_scan);
    tws_inquiry_scan = 0;
    local_irq_enable();
}

RF_CODE u8 check_tws_ble_connecting(void)
{
    return tws_ble != 0;
}

RF_CODE int link_rand_page_enable(u8 *remote_addr)
{
    (void)remote_addr;
    return 0;
}

/* ---- fixed tx/rx test mode -------------------------------------------------- */

/* The { max payload, packet type } row for packet in group 0 (basic rate)
 * or 2 (EDR), else the last row.  For any other group the SDK's table
 * pointer is undefined; packet_table only yields 0 and 2. */
RF_CODE const u16 *txrx_get_packet_max_len(int packet, u8 group)
{
    const u16 (*t)[2] = iut_aclsco_table;
    u32 n = 3;
    if (group == 0) n = 10;
    else if (group == 2) {
        t = iut_edracl_table;
        n = 6;
    }
    u32 i;
    for (i = 0; i < n; i++)
        if (t[i][1] == packet) return t[i];
    return t[i - 1];
}

RF_CODE void fix_txrx_tx_fill_payload(struct ctrl_frame *frame)
{
    if (fix_txrx->role == 3) return;
    u16 len = fix_txrx->packet_len;
    struct bredr_tx_bulk *tx = bredr_tx_bulk_alloc(len);
    if (!tx) return;
    memset(tx, 0, 28);
    struct link_fix_rxtx *f = fix_txrx;
    tx->packet = f->packet_num;
    tx->offset = 0;
    tx->len = len;
    tx->event_post = 0;
    tx->llid = 2;
    memset(tx->data, f->payload, len);
    const u16 *type = get_packet_type(len, (frame->miscntl >> 3) & 1, 5);
    u16 max = type[0];
    fix_txrx->tx_bulk[0] = tx;
    int n = len > max ? max : len;
    __write_reg_txinfo(frame, 0, tx->data, n, tx->llid, 1, type[1]);
    __write_reg_txinfo(frame, 1, tx->data, n, tx->llid, 1, type[1]);
}

/* Bit errors in len bytes against the PRBS9 sequence. */
RF_CODE u16 rd_data(u8 *data_ptr, u16 len)
{
    const u8 *ref = prbs9_table1;
    u16 errors = 0;
    while (len--) {
        for (u8 b = *data_ptr ^ *ref; b; b >>= 1) errors = (u16)(errors + (b & 1));
        data_ptr++;
        ref++;
    }
    return errors;
}

/* Channel fre: even ones 0..39, odd ones 40..78. */
static RF_CODE u8 cal_hop_fre(u8 fre)
{
    return fre & 1 ? (u8)((fre >> 1) + 40) : (u8)(fre >> 1);
}

/* Fixed-frequency rx/tx test (role 3 receives).  payload's high byte picks
 * a single carrier (non-zero) or PN9 data. */
RF_CODE int __fix_txrx_enable(u8 role, u8 *remote_addr, u8 fre, u8 packet_type, u16 payload)
{
    if (fix_txrx) return -12;
    struct ctrl_frame *frame = bredr_bd_get_frame();
    if (!frame) return -12;
    fix_txrx = bt_malloc(132);
    if (!fix_txrx) {
        bredr_bd_put_frame(frame);
        return -12;
    }
    INIT_LIST_HEAD(&fix_txrx->tx_acl_u);
    frame->miscntl = 7;
    frame->bitoff = 0;
    fix_txrx->role = role;
    memset(&fix_txrx->rx_err_b, 0, 20);
    memcpy(fix_txrx->remote.mac_addr, remote_addr, 6);
    __get_access_addr(fix_txrx->remote.access_addr, remote_addr);
    u8 pt = packet_table[packet_type];
    struct link_fix_rxtx *f = fix_txrx;
    f->packet_group = pt >> 4;
    f->packet_num = pt & 15;
    u8 m = frame->miscntl;
    frame->miscntl = (u32)(packet_type - 3) < 6 ? (u8)(m | 8) : (u8)(m & ~8);
    f->packet_len = *txrx_get_packet_max_len(pt & 15, pt >> 4);
    f->payload = (u8)payload;
    f->data_type = (u8)(payload >> 8);
    if (role == 3) {
        frame->wincntl = 0x800e;
        __write_reg_clkoffset(frame, 0);
        __write_reg_bdaddr(frame, fix_txrx->remote.mac_addr);
        __write_reg_bch(frame, fix_txrx->remote.access_addr);
        __write_reg_format(frame, 0, 3);
        __write_reg_linkcntl(frame, 0, 16400);
        __write_reg_linkcntl(frame, 1, 16384);
        frame->bufcntl &= ~0xf00;
        frame->bufcntl = frame->bufcntl;
        __write_reg_txphdr(frame, 0, 2);
        __write_reg_txphdr(frame, 1, 2);
    } else {
        REG32(0x1c0028) = 46;
        frame->wincntl = (u16)(REG32(0x1c0028) + 20);
        __write_reg_clkoffset(frame, 0);
        __write_reg_bdaddr(frame, fix_txrx->remote.mac_addr);
        __write_reg_bch(frame, fix_txrx->remote.access_addr);
        __write_reg_format(frame, 0, 2);
        __write_reg_linkcntl(frame, 0, 16401);
        __write_reg_linkcntl(frame, 1, 16385);
        frame->bufcntl &= ~0xf00;
        frame->bufcntl |= 256;
    }
    if (fix_txrx->data_type) {
        if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]single \n\r\n");
        REG32(0x1c00d0) &= ~1u;
        fix_txrx_tx_fill_payload(frame);
        bt_set_single_carrier();
        bredr_freq_table_init(0);
    } else {
        if (log_tag_const_i_BL) log_print(2, NULL, "[Info]: [BL]pn9 \n\r\n");
        bt_reload_trim();
        REG32(0x1c00d0) |= 1;
        REG32(0x1c00d0) &= ~2u;
        bredr_freq_table_init(1);
    }
    __clean_reg_txempty(frame, 0);
    __clean_reg_txempty(frame, 1);
    frame->bufcntl &= ~32;
    frame->bufcntl &= ~2;
    frame->bufcntl |= 1;
    frame->linkcntl0 |= 32;
    frame->linkcntl1 |= 32;
    for (int i = 0; i < 2; i++) {
        u8 *buf = (u8 *)bd_base.link_rx_bulk[0][i];
        __write_reg_lmprxptr(frame, i, buf);
        __write_reg_rxptr(frame, i, buf);
        __clean_reg_rxfull(frame, i);
        fix_txrx->rx_data[i] = buf;
    }
    f = fix_txrx;
    f->fre = fre;
    f->frame = frame;
    f->state = 1;
    f->task.type = 4096;
    f->task.run_slot = 32;
    f->task.run_num = 1;
    f->task.priority = 1;
    f->task.ops = &fix_txrx_task_ops;
    f->task.sta_attribute = 1;
    REG32(0x1c0000) |= 32768;
    REG32(0x1c0000) |= 524288;
    REG32(0x1c0000) |= 65536;
    u8 ch = cal_hop_fre(fre);
    frame->rxfreq = ch;
    frame->txfreq = ch;
    link_task_idle_disable();
    link_task_add(&fix_txrx->task);
    return 0;
}

/* ---- procedure tasks ------------------------------------------------------------- */

/* Next AGC setting of six (two tries each), for scans and pages. */
static RF_CODE void link_agc_reset(struct ctrl_frame *frame, u8 *agc)
{
    static const u8 agc_set_table[6] = { 8, 8, 21, 21, 27, 27 };
    u8 n = (u8)(*agc + 1);
    n = n > 5 ? 0 : n;
    *agc = n;
    bredr_frame_agc_set(frame, 0, (char)agc_set_table[n]);
}

static RF_CODE int inquiry_scan_resume(struct link_task *task)
{
    (void)task;
    if (inq_scan_disable_active) return 0;
    struct ctrl_frame *frame = inquiry_scan->frame;
    if (log_tag_const_c_BL) putchar('I');
    link_agc_reset(frame, &inquiry_scan->agc);
    page_inquiry_clkn += 32768;
    __write_reg_clkoffset(frame, (int)page_inquiry_clkn);
    bredr_bd_frame_enable(frame, 0, 1);
    struct link_inquiry_scan *s = inquiry_scan;
    s->substate = 0;
    s->state = 2;
    __timer_register(s->task.run_slot - 8);
    return 0;
}

/* Slots (up to 8) still needed to answer an ID received just now. */
static RF_CODE int inquiry_scan_suspend(struct link_task *task)
{
    (void)task;
    if (inq_scan_disable_active || inquiry_scan->state == 3) return 0;
    __timer_remove();
    u32 now = READ_SLOT_CLK();
    bredr_bd_frame_disable(inquiry_scan->frame);
    REG32(0x1c0000) |= 0x80000000u;
    struct link_inquiry_scan *s = inquiry_scan;
    s->state = 3;
    int ret = 0;
    if (s->substate) {
        u32 rx = s->rx_clkn;
        u32 end = rx + 32;
        if (!(end < now)) {
            if (end > 0x7ffffff) {
                if (now > 0x4000000) {
                    ret = (int)(end - now);
                } else {
                    u32 e = rx - 134217696;
                    ret = e > now ? (int)(e - now) : 0;
                }
            } else {
                u32 d = end - now;
                ret = d < 32 ? (int)d : 0;
            }
        }
    }
    return ret < 8 ? ret : 8;
}

static RF_CODE int page_scan_resume(struct link_task *task)
{
    (void)task;
    if (ps_disable_active) return 0;
    if (!page_scan->frame) return 0;
    if (log_tag_const_c_BL) putchar('P');
    struct link_page_scan *ps = page_scan;
    struct ctrl_frame *frame = ps->frame;
    u8 ttog = frame->txtog & 1;
    REG32(0x1c0000) &= ~256u;
    REG32(0x1c0000) |= 256;
    memset(&ps->remote, 0, 36);
    frame->miscntl = 0;
    frame->wincntl = 0x8022;
    frame->bitoff = 0;
    u32 clkn = page_inquiry_clkn;
    if (inquiry_scan) {
        __write_reg_clkoffset(frame, (int)clkn);
    } else {
        page_inquiry_clkn = clkn + 32768;
        __write_reg_clkoffset(frame, (int)page_inquiry_clkn);
    }
    ps = page_scan;
    u8 idx = (u8)(ps->addr_index + 1);
    idx = idx < ps->addr_num ? idx : 0;
    ps->addr_index = idx;
    __write_reg_bdaddr(frame, ps->local_addr[idx]);
    ps = page_scan;
    __write_reg_bch(frame, ps->local_acc[ps->addr_index]);
    __write_reg_format(frame, page_scan->ab_train, 5);
    __write_reg_linkcntl(frame, ttog, 16384);
    __write_reg_txphdr(frame, ttog, 2);
    __write_reg_txphdr(frame, ttog ^ 1, 2);
    link_agc_reset(frame, &page_scan->agc);
    bredr_bd_frame_enable(frame, 0, 1);
    ps = page_scan;
    ps->substate = 0;
    ps->state = 2;
    __timer_register(18);
    return 0;
}

/* 32 more slots while a page response is under way; a random page scan
 * picks a run of 32 or 64 slots. */
static RF_CODE int page_scan_suspend(struct link_task *task)
{
    if (ps_disable_active) return 0;
    struct link_page_scan *ps = page_scan;
    struct ctrl_frame *frame = ps->frame;
    if (!frame) return 0;
    if (ps->substate) return 32;
    bredr_bd_frame_disable(frame);
    REG32(0x1c0000) |= 0x80000000u;
    __timer_remove();
    if (page_scan->rand_enable) task->run_slot = (u16)(((REG32(0x1e3b00) << 5) & 32) + 32);
    REG32(0x1c0000) |= 1024;
    return 0;
}

static RF_CODE int inquiry_resume(struct link_task *task)
{
    (void)task;
    if (inquiry_disable_active) return 0;
    struct link_inquiry *q = inquiry;
    struct ctrl_frame *frame = q->frame;
    link_agc_reset(frame, &q->agc);
    bredr_bd_frame_enable(frame, 0, 1);
    inquiry->state = 2;
    __timer_register(18);
    return 0;
}

static RF_CODE int inquiry_suspend(struct link_task *task)
{
    (void)task;
    if (inquiry_disable_active) return 0;
    bredr_bd_frame_disable(inquiry->frame);
    inquiry->state = 3;
    __timer_remove();
    return 0;
}

static RF_CODE int page_resume(struct link_task *task)
{
    (void)task;
    if (!page->frame || page_disable_active) return 0;
    if (log_tag_const_c_BL) putchar('Q');
    struct link_page *p = page;
    struct ctrl_frame *frame = p->frame;
    u8 ttog = frame->txtog & 1;
    p->substate = 0;
    p->clk_offset = (s16)gbredr_local_dev.clk_base;
    __write_reg_clkoffset(frame, p->remote.clk_offset);
    __write_reg_bdaddr(frame, page->remote.addr);
    __write_reg_bch(frame, page->remote.access_addr);
    frame->miscntl = 0;
    frame->wincntl = 80;
    __write_reg_txptr(frame, ttog, page->fhs);
    __write_reg_format(frame, page->ab_train, 4);
    __write_reg_linkcntl(frame, ttog, 16386);
    __write_reg_linkcntl(frame, ttog ^ 1, 16385);
    __clean_reg_txempty(frame, ttog);
    __clean_reg_txempty(frame, ttog ^ 1);
    __write_reg_txphdr(frame, ttog, 2);
    __write_reg_txphdr(frame, ttog ^ 1, 2);
    link_agc_reset(frame, &page->agc);
    bredr_bd_frame_enable(frame, 0, 1);
    page->state = 2;
    __timer_register(18);
    return 0;
}

static RF_CODE int page_suspend(struct link_task *task)
{
    struct link_page *p = page;
    struct ctrl_frame *frame = p->frame;
    if (!frame || page_disable_active) return 0;
    p->state = 3;
    bredr_bd_frame_disable(frame);
    REG32(0x1c0000) |= 0x80000000u;
    __timer_remove();
    if (page->rand_enable) task->run_slot = (u16)(((REG32(0x1e3b00) << 5) & 32) + 32);
    return 0;
}

/* Bit offset for the received ID: rxbit moved back 88 us (wrapping at a
 * slot); returns rxbit. */
static RF_CODE u16 frame_bitoff_adjust(struct ctrl_frame *frame)
{
    u16 rxbit = frame->rxbit;
    u16 b = rxbit & 1023;
    u16 v;
    if (b < 88) v = (u16)(b + 224);
    else if (b > 399) v = (u16)(rxbit + 624);
    else v = (u16)(b - 88);
    frame->bitoff = v & 1023;
    return b;
}

/* Page scan, after our FHS response: the master's timing from its packet. */
static RF_CODE void page_scan_step_2(void)
{
    struct link_page_scan *ps = page_scan;
    struct ctrl_frame *frame = ps->frame;
    frame->rxtog = 0;
    frame->rxstat0 |= 128;
    frame->rxstat1 |= 128;
    REG32(0x1c0028) = 46;
    frame->wincntl = (u16)(REG32(0x1c0028) + 20);
    u16 rxbit = frame->rxbit;
    u16 wincntl = frame->wincntl;
    s16 sub = (s16)((s16)((rxbit & 1023) - 68) - ((wincntl >> 1) & 2047));
    frame->bitoff = (u16)((sub < 0 ? sub + 625 : sub) & 1023);
    if (sub < 0 && ps->last_rxbit > 87) {
        ps->remote.clk_offset++;
        __write_reg_clkoffset(frame, ps->remote.clk_offset);
        ps = page_scan;
    }
    ps->last_signed_bitoff = sub;
    u8 ttog = frame->txtog & 1;
    ps->substate++;
    __write_reg_format(frame, ps->ab_train, 3);
    __write_reg_linkcntl(frame, ttog, 16400);
    __write_reg_txphdr(frame, ttog, 2);
    __timer_register(50);
}

/* As tws_bredr_master_connect_resume: frame loaded after the null check. */
static RF_CODE int tws_inquiry_scan_resume(struct link_task *task)
{
    (void)task;
    struct link_inquiry_scan *s = tws_inquiry_scan;
    if (!s) return 0;
    struct ctrl_frame *frame = s->frame;
    link_agc_reset(frame, &s->agc);
    bredr_bd_frame_enable(frame, 0, 1);
    s = tws_inquiry_scan;
    s->substate = 0;
    s->state = 2;
    s->task.run_slot = 4;
    return 0;
}

static RF_CODE int tws_inquiry_scan_suspend(struct link_task *task)
{
    (void)task;
    struct link_inquiry_scan *s = tws_inquiry_scan;
    if (!s || s->state == 3) return 0;
    bredr_bd_frame_disable(s->frame);
    REG32(0x1c0000) |= 0x80000000u;
    tws_inquiry_scan->state = 3;
    return 0;
}

static RF_CODE int fix_txrx_resume(struct link_task *task)
{
    (void)task;
    if (!fix_txrx->frame) return 0;
    if (log_tag_const_c_BL) putchar('k');
    struct ctrl_frame *frame = fix_txrx->frame;
    REG32(0x1c0000) &= ~256u;
    REG32(0x1c0000) |= 256;
    bredr_bd_frame_enable(frame, 0, 1);
    fix_txrx->state = 2;
    __timer_register(18);
    return 0;
}

static RF_CODE int fix_txrx_suspend(struct link_task *task)
{
    (void)task;
    struct ctrl_frame *frame = fix_txrx->frame;
    if (!frame) return 0;
    bredr_bd_frame_disable(frame);
    REG32(0x1c0000) |= 0x80000000u;
    __timer_remove();
    REG32(0x1c0000) |= 1024;
    return 0;
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#undef malloc
#undef free

/* ---- host doubles ---- */
int CONFIG_BTCTLER_TWS_ENABLE, config_btctler_hci_standard, config_btctler_mode = 1, config_asser;
int config_btctler_eir_version_info_len, CONFIG_BREDR_INQUIRY = 1, config_bt_function, CONFIG_TEST_DUT_CODE;
int config_btctler_le_tws, CONFIG_TWS_SUPER_TIMEOUT, CONFIG_INQUIRY_PAGE_OFFSET_ADJUST;
u8 rx_fre_offset_adjust_enable;
const int sniff_support_reset_anchor_point = 1, sniff_long_interval = 800;
const u8 log_tag_const_i_BL, log_tag_const_d_BL, log_tag_const_e_BL, log_tag_const_c_BL;
struct bredr_dut *g_dut_hdl;
u8 g_lmp_update_start_flag;
struct common_rf_param rf;
struct hci_parameter hci_param;
struct bredr_local_device gbredr_local_dev;
struct bredr_table bd_base;
u8 __VERSION_BEGIN[8];
const u8 prbs9_table1[511] = { 0xff, 0x00, 0x0f };

void *bredr_link_test_malloc(unsigned long size)
{
    /* get_link_connection's 728 and the EIR list's 16; others as asked. */
    return calloc(1, size == 728 ? sizeof(struct link_connection) : size == 16 ? sizeof(struct inquiry_eir) : size);
}
void bredr_link_test_free(void *ptr) { free(ptr); }
void *bt_malloc(int size)
{
    size_t n = size == 304 ? sizeof(struct link_inquiry_scan) : size == 120 ? sizeof(struct link_page_scan)
             : size == 192 ? sizeof(struct link_inquiry) : size == 168 ? sizeof(struct link_page)
             : size == 68 ? sizeof(struct link_tws_ble) : size == 132 ? sizeof(struct link_fix_rxtx)
             : size == 20 ? sizeof(struct bredr_role_switch) : (size_t)size;
    return calloc(1, n);
}
void bt_free(void *ptr) { free(ptr); }

static int irq_depth, tasks_added, tasks_deleted, frames_put, rxfull_cleared, txempty_cleared, timer_now = 1000;
static int events, last_event[4], pushes, asserts;
static u8 agc_value;
static struct ctrl_frame test_frame;
static struct { int index, len, llid, packet; u8 *data; } txinfo[4];
static int txinfos;
static struct bredr_rx_bulk *pushed;

void local_irq_disable(void) { irq_depth++; }
void local_irq_enable(void) { irq_depth--; }
void log_print(int level, const char *tag, const char *format, ...) { (void)level; (void)tag; (void)format; }
void printf_buf(const u8 *buf, u32 len) { (void)buf; (void)len; }
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; asserts++; }
u32 READ_SLOT_CLK() { return (u32)timer_now; }
int bredr_offset2clkn(int clkn, int offset) { return clkn + offset; }
int bredr_clkn2offset(int a, int b) { return b - a; }
int bredr_clkn_after(int a, int b) { return a > b; }
struct ctrl_frame *bredr_bd_get_frame() { return &test_frame; }
void bredr_bd_put_frame(struct ctrl_frame *frame) { (void)frame; frames_put++; }
void link_task_add(struct link_task *task) { (void)task; tasks_added++; }
void link_task_del(struct link_task *task) { (void)task; tasks_deleted++; }
u32 link_task_idle_disable(void) { return 0; }
u32 link_task_idle_enable(void) { return 0; }
bool link_task_idle_task_enable_detect(void) { return 0; }
void __get_access_addr(u8 *acc, const u8 *addr) { for (int i = 0; i < 5; i++) acc[i] = (u8)(i == 4 ? 0xa7 : 0xa0 + i); (void)addr; }
void __write_reg_lmprxptr(struct ctrl_frame *f, int i, u8 *p) { (void)f; (void)i; (void)p; }
void __write_reg_rxptr(struct ctrl_frame *f, int i, u8 *p) { (void)f; (void)i; (void)p; }
void __write_reg_txptr(struct ctrl_frame *f, int i, u8 *p) { (void)f; (void)i; (void)p; }
void __write_reg_clkoffset(struct ctrl_frame *f, int v) { (void)f; (void)v; }
void __write_reg_bdaddr(struct ctrl_frame *f, const u8 *a) { (void)f; (void)a; }
void __write_reg_bch(struct ctrl_frame *f, const u8 *a) { (void)f; (void)a; }
void __write_reg_format(struct ctrl_frame *f, u8 a, u8 b) { (void)f; (void)a; (void)b; }
void __write_reg_linkcntl(struct ctrl_frame *f, int i, int v) { (void)f; (void)i; (void)v; }
void __write_reg_txphdr(struct ctrl_frame *f, int i, u16 v) { (void)f; (void)i; (void)v; }
void __write_reg_txericntl(int a, int b, void *c) { (void)a; (void)b; (void)c; }
void __clean_reg_rxfull(struct ctrl_frame *f, int i) { (void)f; (void)i; rxfull_cleared++; }
void __clean_reg_txempty(struct ctrl_frame *f, int i) { (void)f; (void)i; txempty_cleared++; }
void __write_reg_txinfo(struct ctrl_frame *f, int index, void *data, int len, int llid, int flow, int packet)
{
    (void)f; (void)flow;
    txinfo[txinfos].index = index; txinfo[txinfos].data = data; txinfo[txinfos].len = len;
    txinfo[txinfos].llid = llid; txinfo[txinfos].packet = packet; txinfos++;
}
u8 bt_get_pwr_max_level(void) { return 9; }
u32 bt_get_txpwr_tb(u8 step) { return 100u + step; }
u32 bt_get_txset_tb(u8 step) { return 200u + step; }
int bredr_frame_get_agc(struct ctrl_frame *f) { (void)f; return agc_value; }
int bredr_link_event(void *link, int argc, ...)
{
    va_list ap;
    va_start(ap, argc);
    last_event[0] = argc;
    for (int i = 0; i < argc && i < 3; i++) last_event[i + 1] = va_arg(ap, int);
    va_end(ap);
    (void)link;
    events++;
    return 0;
}
struct lmp_connection *lmp_conn_for_link(void *lc) { (void)lc; return 0; }
struct bredr_rx_bulk *bredr_rx_bulk_alloc(int id, int len)
{
    (void)id;
    struct bredr_rx_bulk *rx = calloc(1, sizeof(*rx) + (size_t)len);
    return rx;
}
void bredr_rx_bulk_free(struct bredr_rx_bulk *rx) { free(rx); }
void bredr_rx_bulk_push(struct list_head *head, struct bredr_rx_bulk *rx) { (void)head; pushed = rx; pushes++; }
u16 CRC16(const void *ptr, u32 len) { const u8 *p = ptr; return (u16)(p[0] << 8 | p[len - 1]); }
u32 GET_FINETIMECNT() { return 0; }
int bredr_frame_agc_set(struct ctrl_frame *f, char sel, char step) { (void)f; (void)sel; (void)step; return 0; }
char bredr_analog_get_realtime_rssi(void) { return -40; }
u32 __read_reg_clkn(struct ctrl_frame *f) { (void)f; return 100; }
int __read_reg_clkoffset(struct ctrl_frame *f) { return (int)(f->clk_offseth << 16 | f->clk_offsetl); }

static struct link_connection *test_link(void)
{
    struct link_connection *lc = calloc(1, sizeof(*lc));
    lc->frame = &test_frame;
    INIT_LIST_HEAD(&lc->tx_acl_c);
    INIT_LIST_HEAD(&lc->tx_acl_u);
    INIT_LIST_HEAD(&lc->rx_head);
    return lc;
}

static int completed_err = -1;
static void page_done(u8 *addr, int err) { (void)addr; completed_err = err; }
static int inquiry_results;
static int inquiry_done(struct link_inquiry_result *r) { (void)r; inquiry_results++; return 0; }

int main(void)
{
    bredr_link_init();

    /* LT addresses: the lowest free 1..7; put frees; "behind" searches
     * upward from its start, wrapping 8 to 1, else falls back. */
    CHECK(__get_lt_addr() == 1 && __get_lt_addr() == 2 && bredr_link_v.glt_addr == 0x06);
    __put_lt_addr(1);
    CHECK(bredr_link_v.glt_addr == 0x04 && __get_lt_addr() == 1);
    CHECK(__get_lt_addr_behind(2) == 3 && bredr_link_v.glt_addr == 0x0e);
    CHECK(__get_lt_addr_behind(8) == 4);       /* start 8 wraps to 1; 1..3 taken */
    bredr_link_v.glt_addr = 0xfe;
    CHECK(__get_lt_addr() == 0xfe && bredr_link_v.glt_addr == 0xff);   /* full: old value, bumped */
    bredr_link_v.glt_addr = 0;

    /* FHS: pack, then read back a packet with the clock filled in. */
    {
        u8 mac[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 }, fhs[18];
        struct bredr_local_eir eir = { 0 };
        eir.enable = 1;
        eir.sr = 1;
        __write_fhs_packet(mac, 0x240404, &eir, 3, fhs);
        const u8 want[18] = { 0xa0, 0xa1, 0xa2, 0xa3, 0x47, 0x88, 0xcc, 0x94, 0x44, 0x55, 0x66, 0x04, 0x04, 0x24, 3, 0, 0, 0 };
        CHECK(!memcmp(fhs, want, 18));
        fhs[14] = 0x2b; fhs[15] = 0x01; fhs[16] = 0x02; fhs[17] = 0x23;
        struct bredr_remote_device r;
        memset(&r, 0, sizeof(r));
        __read_fhs_packet(&r, fhs);
        CHECK(!memcmp(r.addr, mac, 6) && r.eir == 1 && r.sr == 1 && r.lt_addr == 3);
        CHECK(r.access_addr[0] == 0xa0 && r.access_addr[4] == 3 && r.class_dev == 0x240404);
        CHECK(r.clkn == 12615754);               /* (3<<21 | 2<<13 | 1<<5 | 5) << 1 */
    }

    /* Packet types: first row that holds len within slot slots. */
    CHECK(get_packet_type(100, 0, 5)[0] == 121 && get_packet_type(100, 0, 5)[1] == 10);
    CHECK(get_packet_type(400, 0, 3)[0] == 183);
    CHECK(get_packet_type(30, 0, 1)[0] == 27 && get_packet_type(30, 0, 1)[1] == 4);
    CHECK(get_packet_type(1000, 1, 5)[0] == 679 && get_packet_type(1000, 1, 5)[1] == 14);
    CHECK(get_packet_type(10, 1, 1)[0] == 54);
    CHECK(txrx_get_packet_max_len(11, 0)[0] == 183 && txrx_get_packet_max_len(15, 2)[0] == 1021);
    CHECK(txrx_get_packet_max_len(99, 2)[0] == 1021);   /* no match: last row */
    CHECK(bredr_get_esco_packet_type(38) == 6 && bredr_get_esco_packet_type(12) == 12 &&
          bredr_get_esco_packet_type(61) == 13 && bredr_get_esco_packet_type(5) == 0);
    CHECK(cal_hop_fre(0) == 0 && cal_hop_fre(1) == 40 && cal_hop_fre(78) == 39 && cal_hop_fre(77) == 78);

    /* ACL out in two pieces at max_slot 1 on 1M: DH1 27 (start), DM1 3. */
    {
        struct link_connection *lc = test_link();
        struct bredr_tx_bulk *tx = calloc(1, sizeof(*tx) + 30);
        tx->llid = 2;
        tx->len = 30;
        lc->core.max_slot = 1;
        test_frame.miscntl = 0;
        list_add_tail(&tx->entry, &lc->tx_acl_u);
        txinfos = 0;
        __link_ctrl_tx(tx, 1, lc);
        __link_ctrl_tx(tx, 0, lc);
        CHECK(txinfos == 2 && txinfo[0].len == 27 && txinfo[0].llid == 2 && txinfo[0].packet == 4 && txinfo[0].index == 1);
        CHECK(txinfo[1].len == 3 && txinfo[1].llid == 1 && txinfo[1].packet == 3 && txinfo[1].data == tx->data + 27);
        CHECK(tx->offset == 30 && list_empty(&lc->tx_acl_u) && txempty_cleared == 2 && irq_depth == 0);
        free(tx);
        free(lc);
    }

    /* L2CAP reassembly: a 10-byte start of a 12-byte payload (16 with the
     * header), then 6 more; one push with the whole packet. */
    {
        struct link_connection *lc = test_link();
        u8 start[10] = { 12, 0, 0x40, 0, 1, 2, 3, 4, 5, 6 }, cont[6] = { 7, 8, 9, 10, 11, 12 };
        lc->rx_data[0] = start;
        lc->rx_data[1] = cont;
        pushes = 0;
        __conn_rx_handler(lc, 0, 2, 10);
        CHECK(pushes == 0 && lc->rx_bulk && lc->rx_bulk->len == 10);
        __conn_rx_handler(lc, 1, 1, 6);
        CHECK(pushes == 1 && !lc->rx_bulk && pushed->len == 16 && pushed->data[4] == 1 && pushed->data[15] == 12);
        CHECK(last_event[0] == 1 && last_event[1] == 5);
        free(pushed);
        /* A continuation past the L2CAP length drops the packet. */
        __conn_rx_handler(lc, 0, 2, 10);
        __conn_rx_handler(lc, 1, 1, 6);
        free(pushed);
        u8 big[7] = { 0 };
        lc->rx_data[1] = big;
        __conn_rx_handler(lc, 0, 2, 10);
        pushes = 0;
        __conn_rx_handler(lc, 1, 1, 7);
        CHECK(pushes == 0 && !lc->rx_bulk);
        free(lc);
    }

    /* Transmit power: set or step, clamped to 0..9 (max level) and below
     * bredr_pwr_max; a fixed level wins. */
    {
        struct link_connection *lc = test_link();
        rf.bredr_pwr_max = 15;
        rf.bredr_fix_pwr = 0xff;
        CHECK(bredr_pwr_set(lc, 0, 5) == 0 && lc->core.pwr_set == 5 && test_frame.TXPWER == 105);
        CHECK(bredr_pwr_set(lc, 1, -7) == 1 && lc->core.pwr_set == 0);
        CHECK(bredr_pwr_set(lc, 0, 12) == 2 && lc->core.pwr_set == 9);
        rf.bredr_pwr_max = 6;
        CHECK(bredr_pwr_set(lc, 0, 6) == 2 && lc->core.pwr_set == 6);   /* not below the cap */
        rf.bredr_fix_pwr = 3;
        CHECK(bredr_pwr_set(lc, 0, 1) == 2 && lc->core.pwr_set == 3 && test_frame.TXSET == 203);
        rf.bredr_fix_pwr = 0xff;

        /* Receive power control: after more than pwr_tb[cnt] packets at
         * AGC 1 ask the peer to lower its power; one request at a time. */
        agc_value = 1;
        events = 0;
        for (int i = 0; i < 10; i++) lc_rx_power_ctrl(lc);
        CHECK(events == 0);
        lc_rx_power_ctrl(lc);
        CHECK(events == 1 && last_event[1] == 12 && last_event[2] == 1 && lc->power.normal.pwr_tx_decr_req == 1 &&
              lc->power.normal.pwr_txing && lc->power.normal.pwr_tx_cnt == 0);
        for (int i = 0; i < 11; i++) lc_rx_power_ctrl(lc);
        CHECK(events == 1);                      /* still in flight */
        for (int n = 2; n <= 7; n++) {
            lc->power.normal.pwr_txing = 0;
            for (int i = 0; i < 11; i++) lc_rx_power_ctrl(lc);
        }
        /* The 7th request (decr was 6) moves to pwr_tb[1] = 100 packets. */
        CHECK(events == 7 && lc->power.normal.pwr_tx_decr_req == 7 && lc->power.normal.pwr_tx_cnt == 1);
        agc_value = 2;
        lc->power.normal.pwr_txing = 0;
        for (int i = 0; i < 101; i++) lc_rx_power_ctrl(lc);
        CHECK(events == 8 && last_event[2] == 2 && lc->power.normal.pwr_tx_decr_req == 0 &&
              lc->power.normal.pwr_tx_incr_req == 1 && lc->power.normal.pwr_tx_cnt == 0);
        free(lc);
    }

    /* Page: LT address 1, clock offset valid only with bit 15, the timeout
     * in slots; disable reports the error and frees everything. */
    {
        u8 addr[6] = { 1, 2, 3, 4, 5, 6 };
        gbredr_local_dev.page_timeout_value = 8000;
        tasks_added = tasks_deleted = frames_put = 0;
        CHECK(link_page_enable(addr, 1, 0x8123, page_done) == 0);
        CHECK(page && page->lt_addr == 1 && page->remote.clk_offset == 0x0246 && page->ab_train_max == 128);
        CHECK(page->timeout == 1000 + 12800 && page->task.type == 16 && page->task.run_slot == 32);
        CHECK((bredr_link_v.bredr_link_state & 1) && tasks_added == 1);
        CHECK(link_page_enable(addr, 1, 0x0123, page_done) == 0);   /* already paging */
        link_page_disable(7);
        CHECK(!page && completed_err == 7 && bredr_link_v.glt_addr == 0 && !(bredr_link_v.bredr_link_state & 1));
        CHECK(tasks_deleted == 1 && frames_put == 1);
        CHECK(link_page_enable(addr, 2, 0x0123, 0) == 0 && page->remote.clk_offset == 0 && page->ab_train_max == 0);
        link_page_disable(0);
    }

    /* Page scan and inquiry. */
    {
        CHECK(link_page_scan_enable(5) == 0 && page_scan && page_scan->task.type == 4 && page_scan->clk_offset == 5);
        CHECK((bredr_link_v.bredr_link_state & 4) && page_scan->addr_num == 1);
        link_page_scan_disable();
        CHECK(!page_scan && !(bredr_link_v.bredr_link_state & 4));

        u8 lap[3] = { 0x33, 0x8b, 0x9e };
        REG32(0x1c000c) = 0x200;
        CHECK(link_inquiry_enable(lap, 10240, 5, 0) == 0);
        CHECK(inquiry->timeout == 1000 + 16384 && inquiry->search_num == 5 && inquiry->completed == __inquiry_result_handler);
        CHECK(inquiry->task.type == 8 && (REG32(0x1c00c4) & 0x80000000u));
        link_inquiry_disable();
        CHECK(!inquiry && REG32(0x1c000c) == 0);
    }

    /* Inquiry results: an address is reported once.  SDK quirk: the 30th
     * new address's CRC lands on result_num and search_num. */
    {
        u8 lap[3] = { 0x33, 0x8b, 0x9e };
        u8 fhs[18] = { 0 };
        link_inquiry_enable(lap, 10240, 0, inquiry_done);
        inquiry->fhs = fhs;
        inquiry->state = 2;
        fhs[5] = 0x12 << 2;                      /* addr[0] = 0x12 >> ... */
        fhs[10] = 0x34;                          /* addr[5] */
        test_frame.rxtog = 1;                    /* index 0 */
        test_frame.rxstat0 = 0x8f02;
        inquiry_results = 0;
        inquiry_rx_handler(&test_frame);
        CHECK(inquiry_results == 1 && inquiry->result_num == 1 && inquiry->substate == 2);
        inquiry->substate = 0;
        inquiry_rx_handler(&test_frame);
        CHECK(inquiry_results == 1);             /* same CRC: not again */
        inquiry->substate = 0;
        inquiry->result_num = 29;
        inquiry->search_num = 0;
        fhs[10] = 0x56;
        inquiry_rx_handler(&test_frame);
        u16 crc = (u16)(inquiry->result.remote.addr[0] << 8 | 0x56);
        CHECK(inquiry_results == 2 && inquiry->result_num == (u8)crc && inquiry->search_num == (u8)(crc >> 8));
        link_inquiry_disable();
    }

    /* AFH map from link statistics. */
    {
        struct link_connection *lc = test_link();
        u8 map[10], want[10] = { 0xff, 0xff, 0xff, 0x3f };
        memset(lc->channel_map, 0xff, 10);
        for (int i = 0; i < 79; i++) lc->afh.rx_num[i] = 100;
        CHECK(bredr_link_update_afh(lc, map, 0, 0) == 1 && !memcmp(map, want, 10));   /* the 30 best */
        memset(lc->afh.rx_num, 0, sizeof(lc->afh.rx_num));
        CHECK(bredr_link_update_afh(lc, map, 0, 0) == 1 && !memcmp(map, want, 10));   /* none: fill 30 */
        CHECK(bredr_link_update_afh(lc, map, 0, 1) == 1 && map[0] == 0xff && map[8] == 0xff && map[9] == 127);
        memcpy(lc->channel_map, map, 10);
        CHECK(bredr_link_update_afh(lc, map, 0, 1) == 0);
        /* Kept SDK quirk: the count is not reset between error limits, so
         * 10 good channels counted twice end the search with a 10-channel map. */
        for (int i = 0; i < 79; i++) {
            lc->afh.rx_num[i] = i < 10 ? 200 : 50;
            lc->afh.err_num[i] = i < 10 ? 0 : 25;
        }
        const u8 ten[10] = { 0xff, 0x03 };
        CHECK(bredr_link_update_afh(lc, map, 0, 0) == 1 && !memcmp(map, ten, 10));
        CHECK(bredr_link_update_afh(0, map, 0, 0) == -22 && irq_depth == 0);
        free(lc);
    }

    /* Clocks and slot offsets. */
    {
        struct link_connection *lc = test_link();
        u32 m, l;
        test_frame.clk_offseth = 0x0800;          /* offset 0x8000000 - 1000 + 5 */
        test_frame.clk_offsetl = 0;
        timer_now = 1000;
        bredr_get_link_slot_clk(lc, &m, &l);
        CHECK(m == 1000 && l == 1000);            /* 0x8000000 + 1000 wraps */
        test_frame.clk_offseth = 0xffff;
        test_frame.clk_offsetl = 0xfc00;          /* -1024 */
        CHECK(bredr_get_master_slot_clk(lc) == 0x8000000u - 24);
        test_frame.clk_offseth = 0;
        test_frame.clk_offsetl = 1;
        lc->role = 0;
        test_frame.bitoff = 100;
        CHECK(lc_local_slot_bitoff(lc) == 100 && lc_local_slot_offset(lc) == 525);
        test_frame.clk_offsetl = 2;
        CHECK(lc_local_slot_offset(lc) == 1150);
        test_frame.bitoff = 700;
        CHECK(lc_local_slot_offset(lc) == 625 - 75);
        lc->role = 1;
        test_frame.bitoff = 10;
        test_frame.wincntl = 80;                  /* 10 - 16 + 40 */
        CHECK(lc_local_slot_bitoff(lc) == 34);
        free(lc);
    }

    /* Test-mode helpers. */
    {
        u8 data[3] = { 0xff, 0x01, 0x0e };       /* vs ff 00 0f: 0 + 1 + 1 bit errors */
        CHECK(rd_data(data, 3) == 2);
        test_frame.rxbit = 50;
        CHECK(frame_bitoff_adjust(&test_frame) == 50 && test_frame.bitoff == 274);
        test_frame.rxbit = 0x2000 | 500;         /* high bits ignored */
        CHECK(frame_bitoff_adjust(&test_frame) == 500 && test_frame.bitoff == 100);
        test_frame.rxbit = 200;
        CHECK(frame_bitoff_adjust(&test_frame) == 200 && test_frame.bitoff == 112);
    }

    CHECK(asserts == 0 && irq_depth == 0);
    return TEST_DONE("btctrler_bredr_link");
}
#endif
