/* The SDK's TWS LE link (tws_le_link.c): the advertising and scanning tasks
 * that carry earbud pairing, with their try-start timers, as far as retained
 * TWS code (tws_pair, tws_ll_scan) and owned bredr_link.c / tws_ll_adv.c
 * call them.  What nothing calls (the data getter, the parameter init, the
 * search timer, the connect timer handler, the eSCO connect steps that
 * bredr_link.c no longer names) goes with the member.  This pad never uses
 * TWS; the code is kept faithful regardless.  SDK quirks kept: the
 * advertiser (80 bytes) and scanner (40 bytes) state come from bt_malloc
 * uninitialised and only the fields the enable sets are written (the
 * scanner's type is never set, though the probe handler tests it); le_adv /
 * le_scan are assigned before the allocation is checked, so a failed enable
 * leaves them NULL; the eSCO try-start stores role and task type into the
 * plain le_adv_parm, not le_adv_esco_parm; the enables ignore their timeout
 * argument; the connect info advertised is the 31 bytes of a local struct. */
#include <stddef.h>
#include <string.h>
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG int config_btctler_le_tws;
extern BTCTRLER_CONFIG u8 log_tag_const_c_TWS_LE, log_tag_const_i_TWS_LE;
extern void log_print(int level, const char *tag, const char *format, ...);
int putchar(int c);

#ifdef UNITTEST
extern volatile u32 test_random;
#define RANDOM_REG test_random
#else
#define RANDOM_REG (*(volatile u32 *)1981184)
#endif

static struct link_le_data tws_le_data __attribute__((aligned(2)));
struct le_scan_parm_t le_scan_parm;
struct le_adv_parm_t le_adv_esco_parm;
struct le_adv_parm_t le_adv_parm;
static struct link_le_adv *le_adv;
static struct link_le_scan *le_scan;
static u8 tws_ble_timer = 0xff;
static u8 debug_ble_link_state;
u8 le_channel_map_tab[3] = { 1, 2, 4 };

static int adv_idle_resume(struct link_task *task);
static int adv_idle_suspend(struct link_task *task);
static int adv_resume(struct link_task *task);
static int adv_suspend(struct link_task *task);
static int scan_resume(struct link_task *task);
static int scan_suspend(struct link_task *task);
static const struct link_task_ops adv_idle_task_ops = { adv_idle_resume, adv_idle_suspend };
static const struct link_task_ops adv_task_ops = { adv_resume, adv_suspend };
static const struct link_task_ops scan_task_ops = { scan_resume, scan_suspend };

u8 tws_le_get_esco_indicate(void)
{
    return tws_le_data.esco_indicate;
}

void tws_le_clr_esco_indicate(void)
{
    tws_le_data.esco_indicate = 0;
}

void tws_le_set_esco_indicate(void)
{
    tws_le_data.esco_indicate = 1;
}

void tws_le_data_reset(void)
{
    memset(&tws_le_data, 0, sizeof(tws_le_data));
}

void __attribute__((weak)) tws_remote_search_working(void)
{
}

/* Probe handlers: a received connect request (3) or scan request (4) wakes
 * the link scheduler; the owner's completed() callback sees every PDU. */
void tws_rx_probe_adv_pdu_handler(struct le_adv_link *link, struct ble_rx *rx)
{
    struct link_le_adv *adv;
    u8 type;

    (void)link;
    if (rx->type != 3)
        return;
    adv = le_adv;
    type = adv->type;
    switch (type) {
    case 4:
    case 1:
    case 3:
        local_irq_disable();
        tws_le_data.data_indicate = 1;
        link_task_schedule_suspend();
        local_irq_enable();
        adv = le_adv;
        break;
    default:
        if (type == 0) {
            tws_remote_search_working();
            adv = le_adv;
            type = adv->type;
        }
        if (type == 2)
            tws_le_data.data_indicate = 1;
        break;
    }
    if (adv->completed)
        adv->completed(rx, adv->role_type);
}

void tws_rx_probe_scan_pdu_handler(struct le_scan_link *link, struct ble_rx *rx)
{
    struct link_le_scan *scan;
    int (*completed)(struct ble_rx *, u8);

    (void)link;
    switch (rx->type) {
    case 0:
        tws_remote_state_setting();
        break;
    case 4:
        local_irq_disable();
        link_task_schedule_suspend();
        local_irq_enable();
        scan = le_scan;
        completed = scan->completed;
        if (completed) {
            int result = completed(rx, scan->role_type);

            if ((result != 0) & (le_scan->type == 0)) {
                le_scan_close();
                le_tws_connect_set_timer(2, 10);
            }
        }
        break;
    }
}

void le_scan_close(void)
{
    if (!le_scan)
        return;
    local_irq_disable();
    if (le_scan->state == 0) {
        local_irq_enable();
        return;
    }
    le_scan->state = 0;
    local_irq_enable();
    tws_ll_scan_close();
}

static void tws_le_adv_kickstart(void *p);
static void tws_le_result_check(void *p);

void tws_ble_adv_result_timerset(void)
{
    if (tws_ble_timer != 0xff)
        slot_timer_set(tws_ble_timer, tws_le_result_check, NULL, 12);
}

static void tws_le_result_check(void *p)
{
    struct link_le_adv *adv = le_adv;

    (void)p;
    if (!adv)
        return;
    if (tws_le_data.data_indicate) {
        tws_le_data.data_indicate = 0;
        tws_adv_write_connect_info(adv->role_type, adv->type);
    } else if (adv->type == 3) {
        if (adv->task_run_again & 1)
            adv_task_run_again();
    }
}

void tws_ble_adv_preset_reset(void)
{
    if (tws_ble_timer != 0xff)
        slot_timer_set(tws_ble_timer, tws_le_adv_kickstart, NULL, 2);
}

static void tws_le_adv_kickstart(void *p)
{
    (void)p;
    if (!le_adv)
        return;
    tws_ble_adv_result_timerset();
    tws_le_data.data_indicate = 0;
    le_adv_conn_data_reset();
    tws_ll_adv_set_params((u8 *)&le_adv->adv_param);
    tws_ll_adv_open();
    le_adv->state = 2;
}

void tws_ble_adv_preset_start(void)
{
    if (tws_ble_timer == 0xff)
        tws_ble_timer = slot_timer_get();
}

void tws_ble_adv_preset_close(void)
{
    if (tws_ble_timer != 0xff) {
        slot_timer_put(tws_ble_timer);
        tws_ble_timer = 0xff;
    }
}

void adv_task_run_again(void)
{
    if (!le_adv)
        return;
    le_adv->task.ops->suspend(&le_adv->task);
    link_task_set_slots(&le_adv->task, 16);
    le_adv->task.ops->resume(&le_adv->task);
}

void le_adv_close(void)
{
    if (!le_adv)
        return;
    local_irq_disable();
    if (le_adv->state == 0) {
        local_irq_enable();
        return;
    }
    le_adv->state = 0;
    local_irq_enable();
    tws_ll_adv_close();
}

void adv_disable(int inirq)
{
    (void)inirq;
    if (!le_adv)
        return;
    local_irq_disable();
    tws_ble_search_timeout_clr();
    if (le_adv->type != 2) {
        if (le_adv->search_en)
            link_task_del(&le_adv->idle_task);
        link_task_del(&le_adv->task);
        link_idle_task_enable_detect();
    }
    local_irq_enable();
    le_adv_close();
    local_irq_disable();
    debug_ble_link_state &= 0xfa;
    bt_free(le_adv);
    le_adv = NULL;
    local_irq_enable();
}

void adv_disable_for_connect_timeout(void)
{
    if (!le_adv)
        return;
    le_adv_close();
}

static void le_adv_scan_response_set_data(u8 length, const u8 *data)
{
    struct scan_response scan_resp_data;

    scan_resp_data.length = length;
    memcpy(scan_resp_data.data, data, length);
    tws_ll_adv_set_scan_rsp_data(&scan_resp_data.length);
}

/* The connect info is advertised as the 31 bytes of the struct itself. */
void le_adv_conn_data_reset(void)
{
    struct tws_conn_info tws_bredr_conn_info;

    tws_ble_get_connect_info(&tws_bredr_conn_info);
    tws_bredr_conn_info.working_mode = le_adv->type == 2 ? 21930 : 0;
    tws_bredr_conn_info.role_type = le_adv->role_type;
    tws_bredr_conn_info.channel = tws_api_get_local_channel();
    le_adv_scan_response_set_data(31, (const u8 *)&tws_bredr_conn_info);
}

/* The search info is advertised as its first 28 bytes (all of it). */
void le_adv_search_data(void)
{
    struct tws_search_info tws_bredr_search_info;

    tws_ble_get_search_info(&tws_bredr_search_info);
    le_adv_scan_response_set_data(28, tws_bredr_search_info.addr);
}

void link_le_adv_suspend_en(void)
{
    if (!le_adv)
        return;
    link_task_run_set(&le_adv->task, 0);
}

void link_le_adv_resume(u16 slot)
{
    if (!le_adv)
        return;
    link_task_run_slots(&le_adv->task, slot);
}

static int __le_adv_enable(u32 aa, u8 channel_map, u8 type, u8 search_en, u8 role_type, u8 task_type, int (*completed)(struct ble_rx *, u8))
{
    struct link_le_adv *adv;

    if (le_adv)
        return 0;
    local_irq_disable();
    adv = bt_malloc(80);
    le_adv = adv;
    if (!adv) {
        local_irq_enable();
        return -12;
    }
    adv->adv_param.AA = aa;
    adv->adv_param.Advertising_Channel_Map = channel_map;
    tws_ll_adv_set_params((u8 *)&adv->adv_param);
    if (type) {
        tws_le_data_reset();
        tws_ble_adv_preset_start();
    }
    le_adv->search_en = search_en;
    if (search_en && get_bredr_link_state() == 0) {
        adv = le_adv;
        adv->idle_task.type = 512;
        adv->idle_task.run_slot = 32;
        adv->idle_task.run_num = 1;
        adv->idle_task.priority = 1;
        adv->idle_task.ops = &adv_idle_task_ops;
        adv->idle_task.sta_attribute = 1;
        link_task_add(&adv->idle_task);
    }
    tws_ble_sniff_slot_reset(1);
    adv = le_adv;
    adv->completed = completed;
    adv->type = type;
    adv->esco_delay_cnt = 0;
    adv->esco_conn_step = LMP_TX_NULL;
    adv->task_run_again = 0;
    adv->role_type = role_type;
    adv->state = 2;
    debug_ble_link_state |= 1;
    adv->task.type = task_type;
    adv->task.run_slot = 32;
    adv->task.run_num = 1;
    adv->task.priority = 1;
    adv->task.ops = &adv_task_ops;
    adv->task.sta_attribute = 1;
    link_task_add(&adv->task);
    if (task_type == 16 || task_type == 8)
        link_task_idle_disable();
    else
        link_idle_task_enable_detect();
    local_irq_enable();
    return 0;
}

int link_le_adv_enable(u32 aa, u8 channel_map, u16 timeout, u8 type, u8 search_en, u8 role_type, u8 task_type, int (*completed)(struct ble_rx *, u8))
{
    (void)timeout;
    return __le_adv_enable(aa, channel_map, type, search_en, role_type, task_type, completed);
}

void link_le_adv_disable(int type)
{
    if (type)
        tws_ble_clr_connect_info();
    tws_le_clr_esco_indicate();
    tws_ble_sniff_slot_reset(0);
    tws_ble_adv_preset_close();
    adv_disable(0);
}

static int __le_adv_esco_enable(u32 aa, u8 channel_map, u8 type, u8 role_type, int (*completed)(struct ble_rx *, u8))
{
    struct link_le_adv *adv;

    if (le_adv)
        return 0;
    local_irq_disable();
    if (log_tag_const_i_TWS_LE)
        log_print(2, NULL, "[Info]: [TWS_LE]link_le_adv esco enable : 0x%x\r\n", aa);
    adv = bt_malloc(80);
    le_adv = adv;
    if (!adv) {
        local_irq_enable();
        return -12;
    }
    adv->adv_param.AA = aa;
    adv->adv_param.Advertising_Channel_Map = channel_map;
    tws_ll_adv_set_params((u8 *)&adv->adv_param);
    adv = le_adv;
    adv->completed = completed;
    adv->type = type;
    adv->esco_delay_cnt = 0;
    adv->esco_conn_step = LMP_TX_NULL;
    adv->task_run_again = 0;
    adv->role_type = role_type;
    adv->state = 2;
    debug_ble_link_state |= 1;
    tws_le_data.data_indicate = 0;
    tws_le_clr_esco_indicate();
    link_task_idle_disable();
    local_irq_enable();
    return 0;
}

int link_le_adv_esco_enable(u32 aa, u8 channel_map, u16 timeout, u8 type, u8 search_en, u8 role_type, u8 task_type, int (*completed)(struct ble_rx *, u8))
{
    (void)timeout;
    (void)search_en;
    (void)task_type;
    return __le_adv_esco_enable(aa, channel_map, type, role_type, completed);
}

/* The "try" starts record the parameters and let a timer run the enable once
 * the Classic side has finished detaching. */
void link_le_adv_esco_try_timeout_enable(void *p)
{
    (void)p;
    if (check_lmp_detch_over()) {
        le_adv_esco_parm.sys_time_id = 0;
        link_le_adv_esco_enable(le_adv_esco_parm.AA, le_adv_esco_parm.channel_map, 0, le_adv_esco_parm.type,
                                0, le_adv_esco_parm.role_type, 0, le_adv_esco_parm.completed);
    } else {
        le_adv_esco_parm.sys_time_id = sys_timeout_add(&le_adv_esco_parm, link_le_adv_esco_try_timeout_enable, 50);
    }
}

/* Kept: role and task type go to le_adv_parm, not le_adv_esco_parm. */
int link_le_adv_esco_try_start_enable(u32 aa, u8 channel_map, u16 timeout, u8 type, u8 search_en, u8 role_type, u8 task_type, int (*completed)(struct ble_rx *, u8))
{
    local_irq_disable();
    le_adv_esco_parm.type = type;
    le_adv_esco_parm.channel_map = channel_map;
    le_adv_esco_parm.timeout = timeout;
    le_adv_esco_parm.AA = aa;
    le_adv_esco_parm.completed = completed;
    le_adv_esco_parm.search_en = search_en;
    le_adv_parm.role_type = role_type;
    le_adv_parm.task_type = task_type;
    local_irq_enable();
    le_adv_esco_parm.sys_time_id = sys_timeout_add(&le_adv_esco_parm, link_le_adv_esco_try_timeout_enable, 10);
    return 0;
}

int link_le_adv_try_start_enable(u32 aa, u8 channel_map, u16 timeout, u8 type, u8 search_en, u8 role_type, u8 task_type, int (*completed)(struct ble_rx *, u8))
{
    local_irq_disable();
    le_adv_parm.type = type;
    le_adv_parm.channel_map = channel_map;
    le_adv_parm.timeout = timeout;
    le_adv_parm.AA = aa;
    le_adv_parm.completed = completed;
    le_adv_parm.search_en = search_en;
    le_adv_parm.role_type = role_type;
    le_adv_parm.task_type = task_type;
    local_irq_enable();
    le_tws_connect_set_timer(0, 10);
    return 0;
}

void link_le_adv_try_start_disable(int type)
{
    if (le_adv_esco_parm.sys_time_id) {
        sys_timer_del(le_adv_esco_parm.sys_time_id);
        le_adv_esco_parm.sys_time_id = 0;
    }
    if (le_adv_parm.sys_time_id) {
        sys_timer_del(le_adv_parm.sys_time_id);
        le_adv_parm.sys_time_id = 0;
    }
    link_le_adv_disable(type);
}

void scan_disable_for_connect_timeout(void)
{
    if (!le_scan)
        return;
    le_scan_close();
}

void link_le_scan_suspend_en(void)
{
    if (!le_scan)
        return;
    link_task_run_set(&le_scan->task, 0);
}

void link_le_scan_resume(u16 slot)
{
    if (!le_scan)
        return;
    link_task_run_slots(&le_scan->task, slot);
}

static int __le_scan_enable(u32 aa, u8 role_type, u8 task_type, int (*completed)(struct ble_rx *, u8))
{
    struct link_le_scan *scan;

    if (le_scan)
        return 0;
    local_irq_disable();
    scan = bt_malloc(40);
    le_scan = scan;
    if (!scan) {
        local_irq_enable();
        return -12;
    }
    scan->scan_param.AA = aa;
    scan->completed = completed;
    scan->role_type = role_type;
    scan->state = 2;
    debug_ble_link_state |= 2;
    scan->task.type = task_type;
    scan->task.run_slot = 32;
    scan->task.run_num = 1;
    scan->task.priority = 1;
    scan->task.ops = &scan_task_ops;
    scan->task.sta_attribute = 1;
    link_task_add(&scan->task);
    if (task_type == 16 || task_type == 8)
        link_task_idle_disable();
    else
        link_idle_task_enable_detect();
    if (get_bredr_link_state() == 0)
        link_tws_ble_inquiry_scan_enable();
    local_irq_enable();
    return 0;
}

int link_le_scan_enable(u32 aa, u8 channel_map, u16 timeout, u8 type, u8 role_type, u8 task_type, int (*completed)(struct ble_rx *, u8))
{
    (void)channel_map;
    (void)timeout;
    (void)type;
    return __le_scan_enable(aa, role_type, task_type, completed);
}

static void scan_disable(void)
{
    if (!le_scan)
        return;
    local_irq_disable();
    link_task_del(&le_scan->task);
    link_idle_task_enable_detect();
    debug_ble_link_state &= 0xf5;
    local_irq_enable();
    le_scan_close();
    local_irq_disable();
    bt_free(le_scan);
    le_scan = NULL;
    local_irq_enable();
}

void link_le_scan_disable(int type)
{
    if (type)
        tws_ble_clr_connect_info();
    scan_disable();
}

void link_le_scan_try_timeout_enable(void *p)
{
    (void)p;
    if (check_lmp_detch_over()) {
        le_scan_parm.sys_time_id = 0;
        link_le_scan_enable(le_scan_parm.AA, 0, 0, 0, le_scan_parm.role_type, le_scan_parm.task_type, le_scan_parm.completed);
    } else {
        le_scan_parm.sys_time_id = sys_timeout_add(&le_scan_parm, link_le_scan_try_timeout_enable, 50);
    }
}

int link_le_scan_try_start_enable(u32 aa, u8 channel_map, u16 timeout, u8 type, u8 role_type, u8 task_type, int (*completed)(struct ble_rx *, u8))
{
    local_irq_disable();
    le_scan_parm.type = type;
    le_scan_parm.channel_map = channel_map;
    le_scan_parm.timeout = timeout;
    le_scan_parm.AA = aa;
    le_scan_parm.completed = completed;
    le_scan_parm.role_type = role_type;
    le_scan_parm.task_type = task_type;
    local_irq_enable();
    le_scan_parm.sys_time_id = sys_timeout_add(&le_scan_parm, link_le_scan_try_timeout_enable, 10);
    return 0;
}

void link_le_scan_try_start_disable(int type)
{
    if (le_scan_parm.sys_time_id) {
        sys_timer_del(le_scan_parm.sys_time_id);
        le_scan_parm.sys_time_id = 0;
    }
    link_tws_inquiry_scan_disable();
    link_le_scan_disable(type);
}

static int adv_idle_resume(struct link_task *task)
{
    task->run_slot = 6;
    return 0;
}

static int adv_idle_suspend(struct link_task *task)
{
    (void)task;
    return 0;
}

static int adv_resume(struct link_task *task)
{
    struct link_le_adv *adv = le_adv;

    if (!adv)
        return 0;
    if (log_tag_const_c_TWS_LE) {
        putchar('A');
        adv = le_adv;
    }
    switch (adv->type) {
    case 3:
        task->run_slot = 18;
        break;
    case 1:
    case 4:
        task->run_slot = (RANDOM_REG % 2) * 16 + 16;
        break;
    default:
        task->run_slot = (RANDOM_REG % 3) * 15 + 15;
        le_adv_search_data();
        tws_ll_adv_set_params((u8 *)&le_adv->adv_param);
        tws_ll_adv_open();
        le_adv->state = 2;
        return 0;
    }
    adv->task_run_again++;
    tws_ble_adv_preset_reset();
    return 0;
}

static int adv_suspend(struct link_task *task)
{
    (void)task;
    if (!le_adv)
        return 0;
    if (tws_ble_timer != 0xff)
        slot_timer_disable(tws_ble_timer);
    le_adv_close();
    return 0;
}

static int scan_resume(struct link_task *task)
{
    struct link_le_scan *scan = le_scan;

    if (!scan)
        return 0;
    if (log_tag_const_c_TWS_LE) {
        putchar('S');
        scan = le_scan;
    }
    task->run_slot = (RANDOM_REG % 5) * 18 + 18;
    tws_ll_scan_open((u8 *)&scan->scan_param);
    le_scan->state = 2;
    return 0;
}

static int scan_suspend(struct link_task *task)
{
    (void)task;
    if (!le_scan)
        return 0;
    le_scan_close();
    return 0;
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#include <stdlib.h>

volatile u32 test_random;
int config_btctler_le_tws;
u8 log_tag_const_c_TWS_LE, log_tag_const_i_TWS_LE;

/* Every outside call appends a code to the trace; arguments of interest go to
 * the variables below. */
static char trace[256];
static void tr(const char *s) { strcat(trace, s); }
static int malloc_fail, malloc_size[4], mallocs, frees;
static void *freed[4];
static int logs;
static char log_text[96];
static u32 log_arg;
static int bredr_state, detch_over, putchars;
static char putchar_c;
static u8 timer_get_result = 7;
static u8 timer_put_arg, timer_disable_arg, timer_set_id;
static void (*timer_set_fn)(void *);
static int timer_set_slot;
static void *timer_set_priv;
static const u8 *set_params_arg;
static u8 set_params_copy[5];
static int set_params_n, open_n, close_n, scan_open_n, scan_close_n;
static const u8 *scan_open_arg;
static u8 rsp_len, rsp_data[31];
static int rsp_n;
static u8 conn_role_arg, conn_type_arg;
static int connect_info_writes, clr_connect_info, search_timeout_clr, sniff_reset_arg = -1;
static struct link_task *added[4], *deleted[4], *run_set_task, *run_slots_task, *set_slots_task;
static int added_n, deleted_n, run_set_val = -1, run_slots_val = -1, set_slots_val = -1, idle_disable_n, idle_detect_n;
static int sched_suspend_n, remote_state_n, remote_search_n;
static int connect_set_timer_type = -1, connect_set_timer_ms = -1;
static int sys_timeout_n, sys_timer_del_id = -1;
static void *sys_timeout_priv;
static void (*sys_timeout_fn)(void *);
static u32 sys_timeout_ms;
static u16 sys_timeout_result = 77;
static int completed_n, completed_result;
static struct ble_rx *completed_rx;
static u8 completed_role;
static int inquiry_enable_n, inquiry_disable_n;
static char local_channel = 3;

void local_irq_disable(void) { tr("d"); }
void local_irq_enable(void) { tr("e"); }
void *bt_malloc(int n)
{
    void *p;
    malloc_size[mallocs & 3] = n; mallocs++;
    if (malloc_fail) return NULL;
    p = malloc(128); memset(p, 0xA5, 128); return p;
}
void bt_free(void *p) { freed[frees & 3] = p; frees++; tr("f"); }
void log_print(int level, const char *tag, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); (void)tag;
    logs++; snprintf(log_text, sizeof(log_text), "%d:%s", level, fmt); log_arg = va_arg(ap, u32); va_end(ap); tr("L");
}
int putchar(int c) { putchars++; putchar_c = c; tr("P"); return c; }
u8 get_bredr_link_state() { tr("b"); return bredr_state; }
bool check_lmp_detch_over() { tr("o"); return detch_over; }
u8 slot_timer_get() { tr("g"); return timer_get_result; }
void slot_timer_put(u8 i) { timer_put_arg = i; tr("p"); }
void slot_timer_disable(u8 i) { timer_disable_arg = i; tr("x"); }
void slot_timer_set(u8 i, void (*fn)(void *), void *priv, int slot) { timer_set_id = i; timer_set_fn = fn; timer_set_priv = priv; timer_set_slot = slot; tr("t"); }
int tws_ll_adv_set_params(const u8 *d) { set_params_arg = d; memcpy(set_params_copy, d, 5); set_params_n++; tr("a"); return 0; }
int tws_ll_adv_open(void) { open_n++; tr("O"); return 0; }
int tws_ll_adv_close(void) { close_n++; tr("C"); return 0; }
int tws_ll_adv_set_scan_rsp_data(const u8 *d) { rsp_len = d[0]; memcpy(rsp_data, d + 1, 31); rsp_n++; tr("R"); return 0; }
int tws_ll_scan_open(const u8 *d) { scan_open_arg = d; scan_open_n++; tr("S"); return 0; }
int tws_ll_scan_close() { scan_close_n++; tr("s"); return 0; }
void tws_adv_write_connect_info(u8 role, u8 type) { conn_role_arg = role; conn_type_arg = type; connect_info_writes++; tr("W"); }
void tws_ble_clr_connect_info(void) { clr_connect_info++; tr("c"); }
void tws_ble_search_timeout_clr(void) { search_timeout_clr++; tr("z"); }
void tws_ble_sniff_slot_reset(u8 v) { sniff_reset_arg = v; tr("n"); }
void tws_ble_get_connect_info(struct tws_conn_info *c) { memset(c, 0x11, sizeof(*c)); c->role_type = 0x90; c->channel = 0x91; c->working_mode = 0x9293; tr("i"); }
void tws_ble_get_search_info(struct tws_search_info *s) { for (int k = 0; k < 28; k++) ((u8 *)s)[k] = 0x40 + k; tr("I"); }
char tws_api_get_local_channel() { tr("l"); return local_channel; }
void link_task_add(struct link_task *t) { added[added_n & 3] = t; added_n++; tr("A"); }
void link_task_del(struct link_task *t) { deleted[deleted_n & 3] = t; deleted_n++; tr("D"); }
void link_task_run_set(struct link_task *t, u8 v) { run_set_task = t; run_set_val = v; tr("r"); }
void link_task_run_slots(struct link_task *t, int v) { run_slots_task = t; run_slots_val = v; tr("u"); }
void link_task_set_slots(struct link_task *t, int v) { set_slots_task = t; set_slots_val = v; tr("w"); }
void link_task_schedule_suspend() { sched_suspend_n++; tr("h"); }
u32 link_task_idle_disable(void) { idle_disable_n++; tr("k"); return 0; }
bool link_idle_task_enable_detect(void) { idle_detect_n++; tr("j"); return 0; }
void tws_remote_state_setting(void) { remote_state_n++; tr("m"); }
int le_tws_connect_set_timer(int type, int ms) { connect_set_timer_type = type; connect_set_timer_ms = ms; tr("T"); return 0; }
u16 sys_timeout_add(void *priv, void (*fn)(void *), u32 ms) { sys_timeout_priv = priv; sys_timeout_fn = fn; sys_timeout_ms = ms; sys_timeout_n++; tr("Y"); return sys_timeout_result; }
void sys_timer_del(u16 id) { sys_timer_del_id = id; tr("X"); }
int link_tws_ble_inquiry_scan_enable(void) { inquiry_enable_n++; tr("E"); return 0; }
void link_tws_inquiry_scan_disable(void) { inquiry_disable_n++; tr("F"); }

static int completed(struct ble_rx *rx, u8 role) { completed_n++; completed_rx = rx; completed_role = role; tr("Z"); return completed_result; }

static void reset(void)
{
    le_adv = NULL; le_scan = NULL;
    memset(trace, 0, sizeof(trace)); memset(malloc_size, 0, sizeof(malloc_size)); mallocs = frees = malloc_fail = 0;
    logs = 0; putchars = 0; bredr_state = 0; detch_over = 0; timer_set_fn = NULL; timer_set_slot = -1;
    set_params_n = open_n = close_n = scan_open_n = scan_close_n = 0; rsp_n = 0; connect_info_writes = 0;
    clr_connect_info = search_timeout_clr = 0; sniff_reset_arg = -1; added_n = deleted_n = 0;
    run_set_task = run_slots_task = set_slots_task = NULL; run_set_val = run_slots_val = set_slots_val = -1;
    idle_disable_n = idle_detect_n = sched_suspend_n = remote_state_n = remote_search_n = 0;
    connect_set_timer_type = connect_set_timer_ms = -1; sys_timeout_n = 0; sys_timer_del_id = -1; sys_timeout_fn = NULL;
    completed_n = 0; completed_result = 0; completed_rx = NULL; inquiry_enable_n = inquiry_disable_n = 0;
    log_tag_const_c_TWS_LE = log_tag_const_i_TWS_LE = 0;
    tws_ble_timer = 0xff; debug_ble_link_state = 0; test_random = 0;
    memset(&tws_le_data, 0, sizeof(tws_le_data));
    memset(&le_adv_parm, 0, sizeof(le_adv_parm)); memset(&le_adv_esco_parm, 0, sizeof(le_adv_esco_parm)); memset(&le_scan_parm, 0, sizeof(le_scan_parm));
}

static struct link_le_adv *make_adv(u8 type)
{
    struct link_le_adv *a = calloc(1, sizeof(*a));
    a->type = type; a->state = 2; a->role_type = 5;
    le_adv = a; return a;
}
static struct link_le_scan *make_scan(void)
{
    struct link_le_scan *s = calloc(1, sizeof(*s));
    s->state = 2; s->role_type = 6;
    le_scan = s; return s;
}

int main(void)
{
    struct ble_rx rx;
    struct link_le_adv *a;
    struct link_le_scan *sc;
    struct link_task task;

    memset(&task, 0, sizeof(task));
    /* tables and initial values */
    CHECK(le_channel_map_tab[0] == 1 && le_channel_map_tab[1] == 2 && le_channel_map_tab[2] == 4);
    CHECK(adv_task_ops.resume == adv_resume && adv_task_ops.suspend == adv_suspend);
    CHECK(adv_idle_task_ops.resume == adv_idle_resume && adv_idle_task_ops.suspend == adv_idle_suspend);
    CHECK(scan_task_ops.resume == scan_resume && scan_task_ops.suspend == scan_suspend);
    CHECK(tws_ble_timer == 0xff);

    /* eSCO indicate */
    reset();
    tws_le_set_esco_indicate(); CHECK(tws_le_get_esco_indicate() == 1 && tws_le_data.data_indicate == 0);
    tws_le_data.data_indicate = 1; tws_le_clr_esco_indicate(); CHECK(tws_le_get_esco_indicate() == 0 && tws_le_data.data_indicate == 1);
    tws_le_set_esco_indicate(); tws_le_data_reset(); CHECK(tws_le_data.data_indicate == 0 && tws_le_data.esco_indicate == 0);

    /* enable: out of memory leaves le_adv NULL, -12, interrupts re-enabled */
    reset(); malloc_fail = 1;
    CHECK(link_le_adv_enable(0x11223344, 7, 99, 1, 1, 2, 16, completed) == -12 && le_adv == NULL && !strcmp(trace, "de") && malloc_size[0] == 80);
    /* enable: already running returns 0 and does nothing */
    reset(); a = make_adv(1);
    CHECK(link_le_adv_enable(1, 2, 3, 4, 5, 6, 7, completed) == 0 && trace[0] == 0 && mallocs == 0 && le_adv == a);
    le_adv = NULL; free(a);
    /* enable: type != 0, search enabled, no Classic link, normal task type */
    reset(); tws_le_data.data_indicate = 1; tws_le_data.esco_indicate = 1;
    CHECK(link_le_adv_enable(0x11223344, 7, 99, 1, 1, 2, 4, completed) == 0 && le_adv != NULL);
    a = le_adv;
    CHECK(!strcmp(trace, "dagbAnAje"));
    CHECK(a->adv_param.AA == 0x11223344 && a->adv_param.Advertising_Channel_Map == 7 && set_params_arg == (u8 *)&a->adv_param);
    CHECK(tws_le_data.data_indicate == 0 && tws_le_data.esco_indicate == 0);
    CHECK(a->search_en == 1 && a->type == 1 && a->role_type == 2 && a->state == 2 && a->completed == completed);
    CHECK(a->esco_delay_cnt == 0 && a->esco_conn_step == 0 && a->task_run_again == 0);
    CHECK(a->idle_task.type == 512 && a->idle_task.run_slot == 32 && a->idle_task.run_num == 1 && a->idle_task.priority == 1 && a->idle_task.ops == &adv_idle_task_ops && a->idle_task.sta_attribute == 1);
    CHECK(a->task.type == 4 && a->task.run_slot == 32 && a->task.run_num == 1 && a->task.priority == 1 && a->task.ops == &adv_task_ops && a->task.sta_attribute == 1);
    CHECK(added_n == 2 && added[0] == &a->idle_task && added[1] == &a->task && sniff_reset_arg == 1);
    CHECK(debug_ble_link_state == 1 && idle_detect_n == 1 && idle_disable_n == 0);
    /* fields the enable does not set stay as the allocator left them */
    CHECK(a->task_type == 0xA5 && a->channel_map_cnt == 0xA5 && a->task.state == 0xA5);
    /* enable: type 0 skips the data reset and preset start; no search; task type 16 and 8 disable idle */
    reset(); tws_le_data.data_indicate = 1;
    CHECK(link_le_adv_enable(1, 2, 3, 0, 0, 2, 16, completed) == 0);
    CHECK(!strcmp(trace, "danAke"));
    CHECK(tws_le_data.data_indicate == 1 && tws_ble_timer == 0xff && idle_disable_n == 1 && idle_detect_n == 0 && added_n == 1 && le_adv->search_en == 0);
    reset(); CHECK(link_le_adv_enable(1, 2, 3, 1, 0, 2, 8, completed) == 0 && idle_disable_n == 1 && tws_ble_timer == 7);
    /* search with a Classic link up: no idle task, bredr state asked once */
    reset(); bredr_state = 1;
    CHECK(link_le_adv_enable(1, 2, 3, 1, 1, 2, 4, completed) == 0 && added_n == 1 && added[0] == &le_adv->task && le_adv->search_en == 1);

    /* eSCO enable */
    reset(); malloc_fail = 1; log_tag_const_i_TWS_LE = 1;
    CHECK(link_le_adv_esco_enable(0xCAFE, 7, 99, 1, 1, 2, 16, completed) == -12 && le_adv == NULL && !strcmp(trace, "dLe") && log_arg == 0xCAFE);
    CHECK(!strcmp(log_text, "2:[Info]: [TWS_LE]link_le_adv esco enable : 0x%x\r\n"));
    reset(); a = make_adv(1); CHECK(link_le_adv_esco_enable(1, 2, 3, 4, 5, 6, 7, completed) == 0 && trace[0] == 0); le_adv = NULL; free(a);
    reset(); tws_le_data.data_indicate = 1; tws_le_data.esco_indicate = 1;
    CHECK(link_le_adv_esco_enable(0x55667788, 3, 9, 2, 1, 4, 8, completed) == 0);
    a = le_adv;
    CHECK(!strcmp(trace, "dake") && logs == 0 && malloc_size[0] == 80);
    CHECK(a->adv_param.AA == 0x55667788 && a->adv_param.Advertising_Channel_Map == 3 && a->completed == completed && a->type == 2 && a->role_type == 4 && a->state == 2);
    CHECK(a->esco_delay_cnt == 0 && a->esco_conn_step == 0 && a->task_run_again == 0 && debug_ble_link_state == 1);
    CHECK(tws_le_data.data_indicate == 0 && tws_le_data.esco_indicate == 0 && added_n == 0 && a->search_en == 0xA5);

    /* disable */
    reset(); a = make_adv(1); a->state = 2; tws_le_data.esco_indicate = 1; debug_ble_link_state = 0xFF; tws_ble_timer = 9;
    link_le_adv_disable(0);
    CHECK(clr_connect_info == 0 && tws_le_data.esco_indicate == 0 && sniff_reset_arg == 0 && timer_put_arg == 9 && tws_ble_timer == 0xff);
    CHECK(le_adv == NULL && frees == 1 && freed[0] == a && debug_ble_link_state == 0xFA);
    CHECK(!strcmp(trace, "npdzDjedeCdfe"));
    free(a);
    reset(); a = make_adv(1); link_le_adv_disable(1); CHECK(clr_connect_info == 1 && trace[0] == 'c'); free(a);
    /* adv_disable: type 2 skips the task removal; search removes the idle task first */
    reset(); a = make_adv(2); a->search_en = 1; adv_disable(0);
    CHECK(deleted_n == 0 && idle_detect_n == 0 && search_timeout_clr == 1 && close_n == 1 && le_adv == NULL);
    CHECK(!strcmp(trace, "dzedeCdfe"));
    free(a);
    reset(); a = make_adv(1); a->search_en = 1; adv_disable(0);
    CHECK(deleted_n == 2 && deleted[0] == &a->idle_task && deleted[1] == &a->task && idle_detect_n == 1);
    CHECK(!strcmp(trace, "dzDDjedeCdfe"));
    free(a);
    reset(); a = make_adv(1); adv_disable(0); CHECK(deleted_n == 1 && deleted[0] == &a->task); free(a);
    reset(); adv_disable(0); CHECK(trace[0] == 0);
    /* le_adv_close: state 0 does nothing but the irq bracket */
    reset(); a = make_adv(1); a->state = 0; le_adv_close(); CHECK(!strcmp(trace, "de") && close_n == 0);
    a->state = 2; memset(trace, 0, sizeof(trace)); le_adv_close(); CHECK(!strcmp(trace, "deC") && a->state == 0 && close_n == 1);
    le_adv = NULL; memset(trace, 0, sizeof(trace)); le_adv_close(); CHECK(trace[0] == 0); free(a);
    reset(); a = make_adv(1); adv_disable_for_connect_timeout(); CHECK(!strcmp(trace, "deC") && a->state == 0 && le_adv == a);
    le_adv = NULL; memset(trace, 0, sizeof(trace)); adv_disable_for_connect_timeout(); CHECK(trace[0] == 0); free(a);

    /* try-start disable cancels both timers, then disables */
    reset(); a = make_adv(1); le_adv_parm.sys_time_id = 21; le_adv_esco_parm.sys_time_id = 22;
    link_le_adv_try_start_disable(0);
    CHECK(sys_timer_del_id == 21 && le_adv_parm.sys_time_id == 0 && le_adv_esco_parm.sys_time_id == 0 && le_adv == NULL);
    CHECK(trace[0] == 'X' && trace[1] == 'X');
    free(a);
    reset(); link_le_adv_try_start_disable(1); CHECK(!strcmp(trace, "cn"));
    CHECK(clr_connect_info == 1 && sys_timer_del_id == -1);

    /* try-start enable stores the parameters and asks for the connect timer */
    reset();
    CHECK(link_le_adv_try_start_enable(0x01020304, 5, 0x1234, 6, 1, 2, 3, completed) == 0);
    CHECK(!strcmp(trace, "deT") && connect_set_timer_type == 0 && connect_set_timer_ms == 10);
    CHECK(le_adv_parm.type == 6 && le_adv_parm.channel_map == 5 && le_adv_parm.timeout == 0x1234 && le_adv_parm.AA == 0x01020304 && le_adv_parm.completed == completed);
    CHECK(le_adv_parm.search_en == 1 && le_adv_parm.role_type == 2 && le_adv_parm.task_type == 3);
    /* eSCO try-start: role and task type land in le_adv_parm (SDK quirk) */
    reset(); sys_timeout_result = 77;
    CHECK(link_le_adv_esco_try_start_enable(0x0a0b0c0d, 5, 0x4321, 6, 1, 2, 3, completed) == 0);
    CHECK(!strcmp(trace, "deY") && sys_timeout_priv == &le_adv_esco_parm && sys_timeout_fn == link_le_adv_esco_try_timeout_enable && sys_timeout_ms == 10);
    CHECK(le_adv_esco_parm.sys_time_id == 77 && le_adv_esco_parm.type == 6 && le_adv_esco_parm.channel_map == 5 && le_adv_esco_parm.timeout == 0x4321);
    CHECK(le_adv_esco_parm.AA == 0x0a0b0c0d && le_adv_esco_parm.completed == completed && le_adv_esco_parm.search_en == 1);
    CHECK(le_adv_esco_parm.role_type == 0 && le_adv_esco_parm.task_type == 0 && le_adv_parm.role_type == 2 && le_adv_parm.task_type == 3);
    /* eSCO timer: Classic busy -> retry in 50, else enable with the stored parameters */
    reset(); detch_over = 0; sys_timeout_result = 78;
    link_le_adv_esco_try_timeout_enable(NULL);
    CHECK(!strcmp(trace, "oY") && sys_timeout_ms == 50 && sys_timeout_fn == link_le_adv_esco_try_timeout_enable && sys_timeout_priv == &le_adv_esco_parm && le_adv_esco_parm.sys_time_id == 78);
    reset(); detch_over = 1; malloc_fail = 1; le_adv_esco_parm.sys_time_id = 5; le_adv_esco_parm.AA = 0xABCD; le_adv_esco_parm.type = 3; log_tag_const_i_TWS_LE = 1;
    link_le_adv_esco_try_timeout_enable(NULL);
    CHECK(le_adv_esco_parm.sys_time_id == 0 && log_arg == 0xABCD && sys_timeout_n == 0 && mallocs == 1);

    /* adv task: resume */
    reset(); a = make_adv(3); tws_ble_timer = 4; task.run_slot = 0; log_tag_const_c_TWS_LE = 1;
    CHECK(adv_resume(&task) == 0 && task.run_slot == 18 && a->task_run_again == 1 && putchars == 1 && putchar_c == 'A');
    CHECK(timer_set_fn == tws_le_adv_kickstart && timer_set_id == 4 && timer_set_slot == 2 && timer_set_priv == NULL && test_random == 0);
    reset(); a = make_adv(1); tws_ble_timer = 0xff; a->task_run_again = 255; test_random = 0xFFFFFFFE;
    adv_resume(&task); CHECK(task.run_slot == 16 && a->task_run_again == 0 && timer_set_fn == NULL);
    test_random = 3; adv_resume(&task); CHECK(task.run_slot == 32 && a->task_run_again == 1);
    a->type = 4; test_random = 0x10; adv_resume(&task); CHECK(task.run_slot == 16);
    a->type = 4; test_random = 0x11; adv_resume(&task); CHECK(task.run_slot == 32);
    CHECK(putchars == 0);
    /* default types: random % 3 * 15 + 15, search data advertised, advertising opened */
    a->type = 0; a->state = 0;
    test_random = 3; memset(trace, 0, sizeof(trace)); adv_resume(&task);
    CHECK(task.run_slot == 15 && !strcmp(trace, "IRaO") && rsp_len == 28 && rsp_data[0] == 0x40 && rsp_data[27] == 0x5B && a->state == 2 && set_params_arg == (u8 *)&a->adv_param);
    test_random = 4; adv_resume(&task); CHECK(task.run_slot == 30);
    test_random = 5; a->type = 2; adv_resume(&task); CHECK(task.run_slot == 45);
    reset(); a = make_adv(2); log_tag_const_c_TWS_LE = 1; test_random = 0; adv_resume(&task); CHECK(task.run_slot == 15 && putchars == 1 && trace[0] == 'P');
    le_adv = NULL; memset(trace, 0, sizeof(trace)); CHECK(adv_resume(&task) == 0 && trace[0] == 0); free(a);
    /* suspend: timer disabled only when there is one, then the link closed */
    reset(); a = make_adv(1); tws_ble_timer = 0xff;
    CHECK(adv_suspend(&task) == 0 && !strcmp(trace, "deC"));
    tws_ble_timer = 8; memset(trace, 0, sizeof(trace)); a->state = 2; adv_suspend(&task); CHECK(!strcmp(trace, "xdeC") && timer_disable_arg == 8);
    le_adv = NULL; memset(trace, 0, sizeof(trace)); CHECK(adv_suspend(&task) == 0 && trace[0] == 0); free(a);
    task.run_slot = 99; CHECK(adv_idle_resume(&task) == 0 && task.run_slot == 6 && adv_idle_suspend(&task) == 0);
    /* run again: suspend, 16 slots, resume (through the task's own ops) */
    reset(); a = make_adv(3); tws_ble_timer = 0xff; a->state = 0;
    a->task.ops = &adv_task_ops; a->state = 2; test_random = 0; a->task.run_slot = 0; adv_task_run_again();
    CHECK(!strcmp(trace, "deCw") && set_slots_task == &a->task && set_slots_val == 16 && a->state == 0);
    CHECK(a->task.run_slot == 18 && a->task_run_again == 1);
    le_adv = NULL; memset(trace, 0, sizeof(trace)); adv_task_run_again(); CHECK(trace[0] == 0); free(a);
    /* suspend_en / resume forward to the task layer */
    reset(); a = make_adv(1); link_le_adv_suspend_en(); link_le_adv_resume(0x1234);
    CHECK(run_set_task == &a->task && run_set_val == 0 && run_slots_task == &a->task && run_slots_val == 0x1234);
    le_adv = NULL; memset(trace, 0, sizeof(trace)); link_le_adv_suspend_en(); link_le_adv_resume(1); CHECK(trace[0] == 0); free(a);

    /* timers: preset start/close/reset/result */
    reset(); tws_ble_adv_preset_start(); CHECK(tws_ble_timer == 7 && !strcmp(trace, "g"));
    timer_get_result = 9; tws_ble_adv_preset_start(); CHECK(tws_ble_timer == 7 && !strcmp(trace, "g"));
    tws_ble_adv_result_timerset(); CHECK(timer_set_fn == tws_le_result_check && timer_set_id == 7 && timer_set_slot == 12);
    tws_ble_adv_preset_reset(); CHECK(timer_set_fn == tws_le_adv_kickstart && timer_set_slot == 2);
    tws_ble_adv_preset_close(); CHECK(timer_put_arg == 7 && tws_ble_timer == 0xff);
    memset(trace, 0, sizeof(trace)); tws_ble_adv_preset_close(); tws_ble_adv_result_timerset(); tws_ble_adv_preset_reset(); CHECK(trace[0] == 0);
    timer_get_result = 7;
    /* result check */
    reset(); tws_le_result_check(NULL); CHECK(trace[0] == 0);
    a = make_adv(1); a->role_type = 9; tws_le_data.data_indicate = 1; tws_le_result_check(NULL);
    CHECK(tws_le_data.data_indicate == 0 && conn_role_arg == 9 && conn_type_arg == 1 && !strcmp(trace, "W"));
    memset(trace, 0, sizeof(trace)); tws_le_result_check(NULL); CHECK(trace[0] == 0);
    a->type = 3; a->task_run_again = 2; tws_le_result_check(NULL); CHECK(trace[0] == 0);
    a->task_run_again = 3; a->task.ops = &adv_task_ops; a->state = 0; tws_le_result_check(NULL); CHECK(a->task_run_again == 4 && set_slots_val == 16);
    free(a);
    /* kickstart */
    reset(); tws_le_adv_kickstart(NULL); CHECK(trace[0] == 0);
    a = make_adv(1); a->state = 0; tws_ble_timer = 2; tws_le_data.data_indicate = 1;
    tws_le_adv_kickstart(NULL);
    CHECK(!strcmp(trace, "tilRaO") && tws_le_data.data_indicate == 0 && a->state == 2 && timer_set_fn == tws_le_result_check && set_params_arg == (u8 *)&a->adv_param);
    free(a);

    /* connect info: 31 bytes of the struct with type-dependent working mode */
    reset(); a = make_adv(2); a->role_type = 0x33;
    le_adv_conn_data_reset();
    CHECK(!strcmp(trace, "ilR") && rsp_len == 31 && rsp_data[0] == 0x33 && rsp_data[1] == 0x11 && rsp_data[2] == 3);
    CHECK(rsp_data[9] == (21930 & 0xff) && rsp_data[10] == (21930 >> 8) && rsp_data[8] == 0x11 && rsp_data[11] == 0x11 && rsp_data[30] == 0x11);
    a->type = 1; le_adv_conn_data_reset(); CHECK(rsp_data[9] == 0 && rsp_data[10] == 0);
    free(a);

    /* probe handlers: advertiser */
    memset(&rx, 0, sizeof(rx));
    reset(); a = make_adv(1); rx.type = 4;
    tws_rx_probe_adv_pdu_handler(NULL, &rx); CHECK(trace[0] == 0);
    rx.type = 3; a->completed = completed; a->role_type = 8;
    for (int t = 0; t < 5; t++) {
        reset(); a = make_adv(t); a->completed = completed; a->role_type = 8; tws_le_data.data_indicate = 0;
        tws_rx_probe_adv_pdu_handler(NULL, &rx);
        if (t == 1 || t == 3 || t == 4) CHECK(!strcmp(trace, "dheZ") && tws_le_data.data_indicate == 1 && sched_suspend_n == 1);
        if (t == 0) CHECK(!strcmp(trace, "Z") && tws_le_data.data_indicate == 0);
        if (t == 2) CHECK(!strcmp(trace, "Z") && tws_le_data.data_indicate == 1);
        CHECK(completed_n == 1 && completed_rx == &rx && completed_role == 8);
        free(a);
    }
    reset(); a = make_adv(7); a->completed = completed; tws_rx_probe_adv_pdu_handler(NULL, &rx); CHECK(!strcmp(trace, "Z") && tws_le_data.data_indicate == 0); free(a);
    reset(); a = make_adv(1); a->completed = NULL; tws_rx_probe_adv_pdu_handler(NULL, &rx); CHECK(!strcmp(trace, "dhe") && tws_le_data.data_indicate == 1); free(a);
    reset(); a = make_adv(1); a->completed = completed; rx.type = 5; tws_rx_probe_adv_pdu_handler(NULL, &rx); CHECK(trace[0] == 0); free(a);
    /* probe handlers: scanner */
    reset(); sc = make_scan(); sc->completed = completed; rx.type = 0; tws_rx_probe_scan_pdu_handler(NULL, &rx);
    CHECK(!strcmp(trace, "m")); rx.type = 3; memset(trace, 0, sizeof(trace)); tws_rx_probe_scan_pdu_handler(NULL, &rx); CHECK(trace[0] == 0);
    rx.type = 4; sc->type = 1; completed_result = 1; tws_rx_probe_scan_pdu_handler(NULL, &rx);
    CHECK(!strcmp(trace, "dheZ") && completed_role == 6 && completed_rx == &rx && scan_close_n == 0);
    sc->type = 0; completed_result = 0; memset(trace, 0, sizeof(trace)); tws_rx_probe_scan_pdu_handler(NULL, &rx); CHECK(!strcmp(trace, "dheZ"));
    completed_result = 5; memset(trace, 0, sizeof(trace)); tws_rx_probe_scan_pdu_handler(NULL, &rx);
    CHECK(!strcmp(trace, "dheZdesT") && sc->state == 0 && connect_set_timer_type == 2 && connect_set_timer_ms == 10);
    sc->completed = NULL; sc->state = 2; memset(trace, 0, sizeof(trace)); tws_rx_probe_scan_pdu_handler(NULL, &rx); CHECK(!strcmp(trace, "dhe") && sc->state == 2);
    free(sc);

    /* scanner enable */
    reset(); malloc_fail = 1;
    CHECK(link_le_scan_enable(0x77, 1, 2, 3, 4, 16, completed) == -12 && le_scan == NULL && !strcmp(trace, "de") && malloc_size[0] == 40);
    reset(); sc = make_scan(); CHECK(link_le_scan_enable(1, 2, 3, 4, 5, 6, completed) == 0 && trace[0] == 0); le_scan = NULL; free(sc);
    reset();
    CHECK(link_le_scan_enable(0x77889900, 1, 2, 3, 4, 16, completed) == 0);
    sc = le_scan;
    CHECK(!strcmp(trace, "dAkbEe") && sc->scan_param.AA == 0x77889900 && sc->completed == completed && sc->role_type == 4 && sc->state == 2);
    CHECK(debug_ble_link_state == 2 && sc->task.type == 16 && sc->task.run_slot == 32 && sc->task.run_num == 1 && sc->task.priority == 1 && sc->task.ops == &scan_task_ops && sc->task.sta_attribute == 1);
    CHECK(added_n == 1 && added[0] == &sc->task && sc->type == 0xA5 && sc->task_type == 0xA5);
    reset(); bredr_state = 1;
    CHECK(link_le_scan_enable(1, 1, 2, 3, 4, 5, completed) == 0 && !strcmp(trace, "dAjbe") && inquiry_enable_n == 0 && debug_ble_link_state == 2);
    reset(); link_le_scan_enable(1, 1, 2, 3, 4, 8, completed); CHECK(idle_disable_n == 1 && inquiry_enable_n == 1);
    /* disable */
    reset(); sc = make_scan(); debug_ble_link_state = 0xFF; link_le_scan_disable(0);
    CHECK(!strcmp(trace, "dDjedesdfe"));
    CHECK(deleted_n == 1 && deleted[0] == &sc->task && scan_close_n == 1 && sc->state == 0 && le_scan == NULL && debug_ble_link_state == 0xF5 && frees == 1);
    free(sc);
    reset(); sc = make_scan(); sc->state = 0; link_le_scan_disable(1); CHECK(clr_connect_info == 1 && scan_close_n == 0 && le_scan == NULL); free(sc);
    reset(); link_le_scan_disable(0); CHECK(trace[0] == 0);
    /* close, suspend, resume, connect timeout */
    reset(); sc = make_scan(); sc->state = 0; le_scan_close(); CHECK(!strcmp(trace, "de")); sc->state = 3; memset(trace, 0, sizeof(trace)); le_scan_close();
    CHECK(!strcmp(trace, "des") && sc->state == 0);
    sc->state = 1; memset(trace, 0, sizeof(trace)); scan_disable_for_connect_timeout(); CHECK(!strcmp(trace, "des") && le_scan == sc);
    memset(trace, 0, sizeof(trace)); sc->state = 3; CHECK(scan_suspend(&task) == 0 && !strcmp(trace, "des"));
    link_le_scan_suspend_en(); link_le_scan_resume(0x4321);
    CHECK(run_set_task == &sc->task && run_set_val == 0 && run_slots_task == &sc->task && run_slots_val == 0x4321);
    le_scan = NULL; memset(trace, 0, sizeof(trace)); le_scan_close(); scan_disable_for_connect_timeout(); link_le_scan_suspend_en(); link_le_scan_resume(1);
    CHECK(scan_suspend(&task) == 0 && scan_resume(&task) == 0 && trace[0] == 0); free(sc);
    /* scan resume: slot from the random register, scan parameters opened, state 2 */
    reset(); sc = make_scan(); sc->state = 0; test_random = 7; log_tag_const_c_TWS_LE = 1;
    CHECK(scan_resume(&task) == 0 && task.run_slot == 2 * 18 + 18 && putchars == 1 && putchar_c == 'S' && scan_open_arg == (u8 *)&sc->scan_param && sc->state == 2);
    CHECK(!strcmp(trace, "PS"));
    log_tag_const_c_TWS_LE = 0; test_random = 4; scan_resume(&task); CHECK(task.run_slot == 4 * 18 + 18 && putchars == 1);
    free(sc);
    /* scan try-start */
    reset(); sys_timeout_result = 55;
    CHECK(link_le_scan_try_start_enable(0x01020304, 5, 0x1234, 6, 2, 3, completed) == 0);
    CHECK(!strcmp(trace, "deY") && sys_timeout_priv == &le_scan_parm && sys_timeout_fn == link_le_scan_try_timeout_enable && sys_timeout_ms == 10 && le_scan_parm.sys_time_id == 55);
    CHECK(le_scan_parm.type == 6 && le_scan_parm.channel_map == 5 && le_scan_parm.timeout == 0x1234 && le_scan_parm.AA == 0x01020304 && le_scan_parm.completed == completed && le_scan_parm.role_type == 2 && le_scan_parm.task_type == 3);
    reset(); detch_over = 0; sys_timeout_result = 56; link_le_scan_try_timeout_enable(NULL);
    CHECK(!strcmp(trace, "oY") && sys_timeout_ms == 50 && le_scan_parm.sys_time_id == 56 && sys_timeout_fn == link_le_scan_try_timeout_enable);
    reset(); detch_over = 1; le_scan_parm.sys_time_id = 5; le_scan_parm.AA = 0x55; le_scan_parm.role_type = 2; le_scan_parm.task_type = 8; le_scan_parm.completed = completed;
    link_le_scan_try_timeout_enable(NULL);
    CHECK(le_scan_parm.sys_time_id == 0 && le_scan != NULL && le_scan->scan_param.AA == 0x55 && le_scan->role_type == 2 && le_scan->task.type == 8 && le_scan->completed == completed);
    reset(); sc = make_scan(); le_scan_parm.sys_time_id = 31;
    link_le_scan_try_start_disable(1);
    CHECK(sys_timer_del_id == 31 && le_scan_parm.sys_time_id == 0 && inquiry_disable_n == 1 && clr_connect_info == 1 && le_scan == NULL);
    CHECK(trace[0] == 'X' && trace[1] == 'F');
    free(sc);
    reset(); link_le_scan_try_start_disable(0); CHECK(sys_timer_del_id == -1 && inquiry_disable_n == 1 && clr_connect_info == 0);

    reset();
    return TEST_DONE("btctrler_tws_le_link");
}
#endif
