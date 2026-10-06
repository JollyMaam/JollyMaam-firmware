/* LE vendor Wi-Fi detection (channel avoidance negotiated with the peer): the
 * slave sends its Wi-Fi channel bitmap (vendor opcode 81), the master keeps
 * the peer's bitmap in a small afh_param block, and the link layer thread
 * turns a changed negotiation result into a channel map update.  Everything
 * is gated by CONFIG_WIFI_DETECT_ENABLE == 2.  SDK quirks kept: the probe
 * reads the link's afh_param pointer before the configuration check and
 * divides by the connection interval unchecked, in 625 us units where the
 * interval is in 1.25 ms units; init stores the malloc result before testing
 * it; the master's negotiation counter and the slave's last-sent map are
 * shared by all links; ll_wifi_detect_thread has no caller in this archive. */
#include "btctrler/glue.h"
#include <string.h>

#ifdef UNITTEST
/* The host libc keeps its own malloc, free and puts. */
#define malloc wifi_test_malloc
#define free wifi_test_free
#define puts wifi_test_puts
#endif

extern BTCTRLER_CONFIG int CONFIG_WIFI_DETECT_ENABLE;
extern BTCTRLER_CONFIG u8 log_tag_const_i_LL_AFH;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(const void *buffer, int size);
extern void *malloc(unsigned int size);
extern void free(void *ptr);
extern int puts(const char *s);
extern void wfdet_ble_process(u8 *info);
extern void wfdet_ble_negotiation(u8 *local, u8 *peer, u8 *result, u8 len);
extern void channel_packet_clear(void);

#define LL_CODE __attribute__((section(".ble_ll_code")))

#ifdef UNITTEST
#define DO_CSYNC() ((void)0)
#else
#define DO_CSYNC() __asm__ volatile("csync;")
#endif

#ifndef UNITTEST
_Static_assert(sizeof(WDF_AFH_PARAM) == 8, "SDK WDF_AFH_PARAM size");
_Static_assert(offsetof(WDF_AFH_PARAM, next_target_evt) == 0, "SDK next_target_evt");
_Static_assert(offsetof(WDF_AFH_PARAM, peer_wfdet_info) == 2, "SDK peer_wfdet_info");
_Static_assert(offsetof(struct le_link, wifi_detect_pend) == 324, "SDK wifi_detect_pend");
#endif

/* This pad's Wi-Fi channel bitmap; the master updates it in wfdet_ble_process. */
static u8 local_wfdet_info[5] __attribute__((section(".ble_ll_data"))) = {0xff, 0xff, 0xff, 0xff, 0xff};
/* The map the slave sent last. */
static u8 slave_laster_map[5] __attribute__((section(".ble_ll_data")));
/* Master: negotiations seen since the last local detection run. */
static u8 master_times __attribute__((section(".ble_ll_data"))) = 1;
/* Control procedure steps of the slave's report: opcode 81, then the two
 * words the SDK listed. */
static const ll_step_extend slave_vendor_wifi_detect_steps[3]
    __attribute__((section(".ble_ll_const"), aligned(2))) = {81, 0x8000, 4351};

static inline void atomic_set(atomic_t *v, int i)
{
    local_irq_disable();
    DO_CSYNC();
    v->counter = i;
    local_irq_enable();
}

/* The channel map was applied: the next negotiation may start. */
void ll_update_ch_done_wfdet_callback(struct le_link *link) LL_CODE;
void ll_update_ch_done_wfdet_callback(struct le_link *link)
{
    if (CONFIG_WIFI_DETECT_ENABLE == 2)
        atomic_set(&link->wifi_detect_pend, 4);
}

/* The peer acknowledged the report. */
void __slave_vendor_wifi_detect_done(struct le_link *link) LL_CODE;
void __slave_vendor_wifi_detect_done(struct le_link *link)
{
    if (CONFIG_WIFI_DETECT_ENABLE == 2) {
        __ble_ops->ioctrl(link->hw, 18, 0);
        atomic_set(&link->wifi_detect_pend, 4);
    }
}

static void __slave_ll_vendor_wifi_detect_req_callback(void *priv) LL_CODE;
static void __slave_ll_vendor_wifi_detect_req_callback(void *priv)
{
    __ble_ops->ioctrl(*(void **)priv, 18, 1);
}

void __slave_ll_send_vendor_wifi_detect_req(struct le_link *link) LL_CODE;
void __slave_ll_send_vendor_wifi_detect_req(struct le_link *link)
{
    if (CONFIG_WIFI_DETECT_ENABLE == 2)
        ll_send_control_data_callback(link, __slave_ll_vendor_wifi_detect_req_callback, 81, "LB", 5,
                                      local_wfdet_info);
}

/* Slave: send the local map unless it is what was sent last. */
static void le_slave_wifi_detect_report_thread(struct le_link *link) LL_CODE;
static void le_slave_wifi_detect_report_thread(struct le_link *link)
{
    if (memcmp(slave_laster_map, local_wfdet_info, 5) == 0) {
        atomic_set(&link->wifi_detect_pend, 4);
        return;
    }
    memcpy(slave_laster_map, local_wfdet_info, 5);
    if (log_tag_const_i_LL_AFH) {
        log_print(2, NULL, "[Info]: [LL_AFH]>>>>>>>>>>>> Slave_reort\r\n");
        printf_buf(local_wfdet_info, 5);
    }
    ll_control_data_step_start(link, slave_vendor_wifi_detect_steps);
    atomic_set(&link->wifi_detect_pend, 3);
}

/* Master: run the local detection every conn_link_num-th time, then
 * negotiate with the peer's map and apply a changed channel map. */
static void le_master_wifi_detect_thread(struct le_link *link) LL_CODE;
static void le_master_wifi_detect_thread(struct le_link *link)
{
    u8 negotiation_ch[5];
    WDF_AFH_PARAM *wfdet = link->ll_data.afh_param;

    if ((int)master_times < ll_get_conn_link_num()) {
        master_times = (u8)(master_times + 1);
    } else {
        master_times = 1;
        wfdet_ble_process(local_wfdet_info);
    }
    wfdet_ble_negotiation(local_wfdet_info, wfdet->peer_wfdet_info, negotiation_ch, 5);
    if (memcmp(negotiation_ch, link->ll_data.channel, 5) != 0) {
        if (log_tag_const_i_LL_AFH) {
            log_print(2, NULL, "[Info]: [LL_AFH]>>>>>>>>>>>>>> channelm_s_n\r\n");
            printf_buf(local_wfdet_info, 5);
            printf_buf(wfdet->peer_wfdet_info, 5);
            printf_buf(negotiation_ch, 5);
        }
        memcpy(link->ll_data.channel, negotiation_ch, 5);
        le_channel_map_update_from_controller(link);
        atomic_set(&link->wifi_detect_pend, 3);
    } else {
        atomic_set(&link->wifi_detect_pend, 4);
    }
}

/* Link layer thread step for a connected link with a run requested (2) by
 * the probe. */
void ll_wifi_detect_thread(struct le_link *link) LL_CODE;
void ll_wifi_detect_thread(struct le_link *link)
{
    if (CONFIG_WIFI_DETECT_ENABLE == 2 && link->state == 7) {
        DO_CSYNC();
        if (*(volatile int *)&link->wifi_detect_pend.counter == 2) {
            if (link->role == 1) {
                wfdet_ble_process(local_wfdet_info);
                le_slave_wifi_detect_report_thread(link);
            } else {
                le_master_wifi_detect_thread(link);
            }
        }
    }
}

/* Connection event probe: when idle (4) and the target event has passed,
 * request a run (2), set the next target event and wake the link thread. */
void ll_wifi_detect_probe_process(struct le_link *link) LL_CODE;
void ll_wifi_detect_probe_process(struct le_link *link)
{
    WDF_AFH_PARAM *wfdet = link->ll_data.afh_param;

    if (CONFIG_WIFI_DETECT_ENABLE == 2) {
        DO_CSYNC();
        if (*(volatile int *)&link->wifi_detect_pend.counter == 4) {
            u32 period = 1000000 / ((u32)link->ll_data.interval * 625) + 1;
            u16 target = wfdet->next_target_evt;

            if (target == 0) {
                target = (u16)period;
                wfdet->next_target_evt = target;
            }
            if ((s16)(u16)(link->event_cnt - target) >= 0) {
                wfdet->next_target_evt = (u16)((u32)link->event_cnt + period);
                atomic_set(&link->wifi_detect_pend, 2);
                thread_resume(&ll_hdl.thread);
            }
        }
    }
}

/* Master: the slave's report (opcode 81) arrived; keep its map. */
void ll_master_rx_wifi_detect_report(struct le_link *link, u8 *buf, u16 len) LL_CODE;
void ll_master_rx_wifi_detect_report(struct le_link *link, u8 *buf, u16 len)
{
    (void)len;
    if (CONFIG_WIFI_DETECT_ENABLE == 2) {
        WDF_AFH_PARAM *wfdet = link->ll_data.afh_param;

        memcpy(wfdet->peer_wfdet_info, buf, 5);
        if (log_tag_const_i_LL_AFH) {
            log_print(2, NULL, "[Info]: [LL_AFH]-- master wfdet_info\r\n");
            printf_buf(local_wfdet_info, 5);
            printf_buf(buf, 5);
        }
    }
}

/* A connection is set up: allocate the 8 byte state block. */
void ll_wifi_detect_init(struct le_link *link) LL_CODE;
void ll_wifi_detect_init(struct le_link *link)
{
    if (CONFIG_WIFI_DETECT_ENABLE == 2) {
        WDF_AFH_PARAM *wfdet = malloc(8);

        link->ll_data.afh_param = wfdet;
        if (!wfdet) {
            puts("no afh_param memory!!!");
            return;
        }
        memset(wfdet, 0, 8);
        memset(wfdet->peer_wfdet_info, 0xff, 5);
        atomic_set(&link->wifi_detect_pend, 4);
    }
}

/* A connection is torn down: release the block; with no connection left,
 * forget the detection history. */
void ll_wifi_detect_ext(struct le_link *link) LL_CODE;
void ll_wifi_detect_ext(struct le_link *link)
{
    if (CONFIG_WIFI_DETECT_ENABLE == 2) {
        if (ll_get_conn_link_num() == 0)
            channel_packet_clear();
        if (link && link->ll_data.afh_param) {
            free(link->ll_data.afh_param);
            link->ll_data.afh_param = NULL;
        }
    }
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>

int CONFIG_WIFI_DETECT_ENABLE = 2;
u8 log_tag_const_i_LL_AFH;
const struct ble_operation *__ble_ops;
struct link_layer_handle ll_hdl;

static int irq_off, irq_on, resumes, map_updates, mallocs, frees, puts_calls, steps_started, process_calls, neg_calls;
static int conn_links, packet_clears, ioctl_calls, ioctl_arg[3], ctl_calls, logs, pbufs;
static void *alloc_result, *freed_ptr, *ioctl_hw;
static void (*ctl_cb)(void *);
static int ctl_opcode, ctl_n;
static const char *ctl_fmt;
static const u8 *ctl_buf;
static const ll_step_extend *started_steps;
static u8 neg_out[5];
static u8 neg_local[5], neg_peer[5];
static u8 process_set[5];

void local_irq_disable(void) { irq_off++; }
void local_irq_enable(void) { irq_on++; }
void log_print(int level, const char *tag, const char *format, ...) { (void)level; (void)tag; (void)format; logs++; }
void printf_buf(const void *buffer, int size) { (void)buffer; CHECK(size == 5); pbufs++; }
void *malloc(unsigned int size) { mallocs++; CHECK(size == 8); return alloc_result; }
void free(void *ptr) { frees++; freed_ptr = ptr; }
int puts(const char *s) { puts_calls++; CHECK(!strcmp(s, "no afh_param memory!!!")); return 0; }
void thread_resume(struct thread *th) { CHECK(th == &ll_hdl.thread); resumes++; }
void le_channel_map_update_from_controller(struct le_link *link) { (void)link; map_updates++; }
int ll_get_conn_link_num(void) { return conn_links; }
void channel_packet_clear(void) { packet_clears++; }
void wfdet_ble_process(u8 *info) { process_calls++; memcpy(info, process_set, 5); }
void wfdet_ble_negotiation(u8 *local, u8 *peer, u8 *result, u8 len)
{
    CHECK(len == 5);
    neg_calls++;
    memcpy(neg_local, local, 5);
    memcpy(neg_peer, peer, 5);
    memcpy(result, neg_out, 5);
}
void ll_control_data_step_start(struct le_link *link, const ll_step_extend *steps) { (void)link; steps_started++; started_steps = steps; }
void ll_send_control_data_callback(struct le_link *link, void (*callback)(void *), int opcode, const char *format, ...)
{
    va_list ap;
    (void)link;
    va_start(ap, format);
    ctl_calls++;
    ctl_cb = callback;
    ctl_opcode = opcode;
    ctl_fmt = format;
    ctl_n = va_arg(ap, int);
    ctl_buf = va_arg(ap, const u8 *);
    va_end(ap);
}
static void fake_ioctrl(void *hw, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    ioctl_hw = hw;
    ioctl_arg[0] = cmd;
    ioctl_arg[1] = va_arg(ap, int);
    ioctl_calls++;
    va_end(ap);
}
static const struct ble_operation ops = { .ioctrl = fake_ioctrl };

static struct le_link link;
static u8 hw_token;

static void reset(void)
{
    memset(&link, 0, sizeof(link));
    link.hw = &hw_token;
    link.state = 7;
    link.ll_data.interval = 16;
    CONFIG_WIFI_DETECT_ENABLE = 2;
    __ble_ops = &ops;
    irq_off = irq_on = resumes = map_updates = mallocs = frees = puts_calls = steps_started = 0;
    process_calls = neg_calls = conn_links = packet_clears = ioctl_calls = ctl_calls = logs = pbufs = 0;
    alloc_result = freed_ptr = ioctl_hw = NULL;
    log_tag_const_i_LL_AFH = 0;
    memset(process_set, 0xff, 5);
    memset(neg_out, 0, 5);
}

int main(void)
{
    static WDF_AFH_PARAM blk;
    /* The slave's step table: opcode 81 then 0x8000 and 0x10ff. */
    CHECK(slave_vendor_wifi_detect_steps[0] == 81 && slave_vendor_wifi_detect_steps[1] == 0x8000 &&
          slave_vendor_wifi_detect_steps[2] == 0x10ff);
    CHECK(master_times == 1);
    CHECK(!memcmp(local_wfdet_info, "\xff\xff\xff\xff\xff", 5));

    /* Done / channel map callbacks: only when configured to 2. */
    reset();
    ll_update_ch_done_wfdet_callback(&link);
    CHECK(link.wifi_detect_pend.counter == 4 && irq_off == 1 && irq_on == 1);
    link.wifi_detect_pend.counter = 9;
    CONFIG_WIFI_DETECT_ENABLE = 1;
    ll_update_ch_done_wfdet_callback(&link);
    __slave_vendor_wifi_detect_done(&link);
    __slave_ll_send_vendor_wifi_detect_req(&link);
    ll_wifi_detect_probe_process(&link);
    ll_wifi_detect_init(&link);
    ll_wifi_detect_ext(&link);
    ll_wifi_detect_thread(&link);
    CHECK(link.wifi_detect_pend.counter == 9 && ioctl_calls == 0 && ctl_calls == 0 && mallocs == 0 &&
          frees == 0 && packet_clears == 0 && resumes == 0 && process_calls == 0 && irq_off == 1);
    reset();
    __slave_vendor_wifi_detect_done(&link);
    CHECK(ioctl_calls == 1 && ioctl_hw == &hw_token && ioctl_arg[0] == 18 && ioctl_arg[1] == 0 &&
          link.wifi_detect_pend.counter == 4);

    /* The control-data request carries the 5 byte map and the callback
     * that ioctrl(18, 1) on the link's handle. */
    reset();
    __slave_ll_send_vendor_wifi_detect_req(&link);
    CHECK(ctl_calls == 1 && ctl_opcode == 81 && !strcmp(ctl_fmt, "LB") && ctl_n == 5 && ctl_buf == local_wfdet_info);
    ctl_cb(&link.hw);
    CHECK(ioctl_calls == 1 && ioctl_hw == &hw_token && ioctl_arg[0] == 18 && ioctl_arg[1] == 1);

    /* Init: allocation failure prints and leaves the state alone; the
     * pointer is stored even when null. */
    reset();
    link.ll_data.afh_param = &blk;
    ll_wifi_detect_init(&link);
    CHECK(mallocs == 1 && puts_calls == 1 && link.ll_data.afh_param == NULL && link.wifi_detect_pend.counter == 0);
    reset();
    static u8 mem[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    alloc_result = mem;
    ll_wifi_detect_init(&link);
    CHECK(link.ll_data.afh_param == mem && !memcmp(mem, "\0\0\xff\xff\xff\xff\xff\0", 8) &&
          link.wifi_detect_pend.counter == 4);

    /* Ext: clear packets only with no connections; free a held block. */
    reset();
    link.ll_data.afh_param = &blk;
    conn_links = 2;
    ll_wifi_detect_ext(&link);
    CHECK(packet_clears == 0 && frees == 1 && freed_ptr == &blk && link.ll_data.afh_param == NULL);
    ll_wifi_detect_ext(&link);
    CHECK(frees == 1 && packet_clears == 0);
    conn_links = 0;
    ll_wifi_detect_ext(NULL);
    CHECK(packet_clears == 1 && frees == 1);

    /* Report: the 5 bytes land at offset 2; logs only when enabled. */
    reset();
    memset(&blk, 0, sizeof(blk));
    link.ll_data.afh_param = &blk;
    ll_master_rx_wifi_detect_report(&link, (u8 *)"\1\2\3\4\5", 6);
    CHECK(!memcmp(blk.peer_wfdet_info, "\1\2\3\4\5", 5) && blk.next_target_evt == 0 && logs == 0);
    log_tag_const_i_LL_AFH = 1;
    ll_master_rx_wifi_detect_report(&link, (u8 *)"\6\7\x08\x09\x0a", 6);
    CHECK(!memcmp(blk.peer_wfdet_info, "\6\7\x08\x09\x0a", 5) && logs == 1 && pbufs == 2);

    /* Probe: pend 4 only.  interval 16: 1000000 / 10000 = 100, +1. */
    reset();
    memset(&blk, 0, sizeof(blk));
    link.ll_data.afh_param = &blk;
    link.wifi_detect_pend.counter = 3;
    ll_wifi_detect_probe_process(&link);
    CHECK(blk.next_target_evt == 0 && resumes == 0);
    link.wifi_detect_pend.counter = 4;
    link.event_cnt = 5;
    ll_wifi_detect_probe_process(&link);     /* target set to 101; 5 - 101 < 0 */
    CHECK(blk.next_target_evt == 101 && resumes == 0 && link.wifi_detect_pend.counter == 4);
    link.event_cnt = 101;
    ll_wifi_detect_probe_process(&link);     /* 0 >= 0: next = 101 + 101 */
    CHECK(blk.next_target_evt == 202 && resumes == 1 && link.wifi_detect_pend.counter == 2);
    /* Event counter wrap: 3 - 65530 as a signed 16 bit difference is 9. */
    link.wifi_detect_pend.counter = 4;
    blk.next_target_evt = 65530;
    link.event_cnt = 3;
    ll_wifi_detect_probe_process(&link);
    CHECK(blk.next_target_evt == 104 && resumes == 2);
    /* Just before the target: -1 does not fire. */
    link.wifi_detect_pend.counter = 4;
    blk.next_target_evt = 1000;
    link.event_cnt = 999;
    ll_wifi_detect_probe_process(&link);
    CHECK(blk.next_target_evt == 1000 && resumes == 2);
    /* Interval 1 (1.25 ms): 1000000 / 625 = 1600, +1 = 1601. */
    link.ll_data.interval = 1;
    blk.next_target_evt = 0;
    link.event_cnt = 0;
    ll_wifi_detect_probe_process(&link);
    CHECK(blk.next_target_evt == 1601 + 0 && resumes == 2);

    /* Thread: needs state 7 and pend 2. */
    reset();
    link.ll_data.afh_param = &blk;
    link.wifi_detect_pend.counter = 2;
    link.state = 6;
    ll_wifi_detect_thread(&link);
    link.state = 7;
    link.wifi_detect_pend.counter = 3;
    ll_wifi_detect_thread(&link);
    CHECK(process_calls == 0 && neg_calls == 0 && link.wifi_detect_pend.counter == 3);

    /* Slave: unchanged map (zero at start vs 0xff... differ first). */
    reset();
    link.role = 1;
    link.wifi_detect_pend.counter = 2;
    memset(process_set, 0x11, 5);
    ll_wifi_detect_thread(&link);
    CHECK(process_calls == 1 && !memcmp(local_wfdet_info, "\x11\x11\x11\x11\x11", 5) &&
          steps_started == 1 && started_steps == slave_vendor_wifi_detect_steps && link.wifi_detect_pend.counter == 3 &&
          !memcmp(slave_laster_map, local_wfdet_info, 5));
    link.wifi_detect_pend.counter = 2;
    log_tag_const_i_LL_AFH = 1;
    ll_wifi_detect_thread(&link);
    CHECK(process_calls == 2 && steps_started == 1 && link.wifi_detect_pend.counter == 4 && logs == 0);
    memset(process_set, 0x22, 5);
    link.wifi_detect_pend.counter = 2;
    ll_wifi_detect_thread(&link);
    CHECK(steps_started == 2 && logs == 1 && pbufs == 1 && link.wifi_detect_pend.counter == 3);

    /* Master (role 0): with one link the detection runs every time; with
     * three links on the 1st and 2nd call only the negotiation runs, the 3rd
     * (times == 3, not below 3) resets the counter and detects. */
    reset();
    master_times = 1;
    link.role = 0;
    memset(&blk, 0, sizeof(blk));
    memset(blk.peer_wfdet_info, 0x33, 5);
    link.ll_data.afh_param = &blk;
    memcpy(link.ll_data.channel, "\1\2\3\4\5", 5);
    memcpy(neg_out, "\1\2\3\4\5", 5);                    /* same as the link's map */
    memset(process_set, 0x44, 5);
    conn_links = 3;
    for (int i = 0; i < 3; i++) {
        link.wifi_detect_pend.counter = 2;
        ll_wifi_detect_thread(&link);
        CHECK(link.wifi_detect_pend.counter == 4 && map_updates == 0);
    }
    CHECK(process_calls == 1 && master_times == 1 && neg_calls == 3);
    CHECK(!memcmp(neg_peer, blk.peer_wfdet_info, 5) && !memcmp(neg_local, "\x44\x44\x44\x44\x44", 5));
    /* A different result replaces the channel map and requests the update. */
    memcpy(neg_out, "\x09\x08\x07\x06\x05", 5);
    link.wifi_detect_pend.counter = 2;
    log_tag_const_i_LL_AFH = 1;
    ll_wifi_detect_thread(&link);
    CHECK(!memcmp(link.ll_data.channel, "\x09\x08\x07\x06\x05", 5) && map_updates == 1 && link.wifi_detect_pend.counter == 3 &&
          logs == 1 && pbufs == 3 && master_times == 2);
    /* One link: never below, always detects. */
    conn_links = 1;
    master_times = 5;
    process_calls = 0;
    link.wifi_detect_pend.counter = 2;
    ll_wifi_detect_thread(&link);
    CHECK(process_calls == 1 && master_times == 1);
    /* Counter reaches 255 + 1 wraps as a byte when links > 255 is impossible;
     * a count of zero links resets as well. */
    conn_links = 0;
    master_times = 1;
    link.wifi_detect_pend.counter = 2;
    ll_wifi_detect_thread(&link);
    CHECK(process_calls == 2 && master_times == 1);
    return TEST_DONE("ll_wifi_detect");
}
#endif
