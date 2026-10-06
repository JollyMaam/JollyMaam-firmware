/* BLE adaptive frequency hopping for the master role: per-channel receive
 * statistics gathered while connected, a ranking of the 37 data channels
 * into a good and a bad list, and the channel map update derived from it.
 * The link layer thread consumes the result through ll_hdl.thread.  SDK
 * quirks kept: the handlers read afh_param without a null check once the
 * state is 7 (only ll_afh_open's malloc result is asserted, and the memset
 * runs on a failed allocation anyway); the channel index of a received
 * packet is not range checked; expected_rx_cnt is overwritten with the new
 * rx_cnt before it is incremented for a packet with MD set; the bad-channel
 * pick takes the list tail (and prints '=') when the first entry already has
 * a history weight; ll_update_ch_done_afh_callback sets the state to 4 and
 * then 1 back to back.  The SDK's asserts print "file:%s, line:%d" and a
 * text; they go through btctrler_assert_failed here. */
#include "btctrler/glue.h"
#include <string.h>

#ifdef UNITTEST
/* The host struct is larger than the SDK's 764 bytes. */
#define malloc afh_test_malloc
#define free afh_test_free
#endif
extern BTCTRLER_CONFIG int config_btctler_le_afh_en;
extern BTCTRLER_CONFIG u8 log_tag_const_i_LL_AFH, log_tag_const_d_LL_AFH;
extern BTCTRLER_CONFIG u8 log_tag_const_e_LL_AFH, log_tag_const_w_LL_AFH;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(const void *buffer, int size);
extern int putchar(int c);
extern void *malloc(unsigned int size);
extern void free(void *ptr);

#define LL_CODE __attribute__((section(".ble_ll_code")))

#ifdef UNITTEST
#define DO_CSYNC() ((void)0)
#else
#define DO_CSYNC() __asm__ volatile("csync;")
#endif

#ifndef UNITTEST
_Static_assert(sizeof(struct afh_param) == 764, "SDK afh_param size");
_Static_assert(sizeof(struct _channel_quality) == 20, "SDK channel quality size");
_Static_assert(offsetof(struct afh_param, channel_quality) == 20, "SDK channel quality offset");
_Static_assert(offsetof(struct afh_param, new_online_channels_nums) == 760, "SDK online channel count");
_Static_assert(offsetof(struct ble_rx, channel) == 11, "SDK rx channel");
#endif

/* The channels a new connection starts with: all 37. */
const u8 first_used_channels[37] __attribute__((section(".ble_ll_const"))) = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
};

/* An ASSERT() with the SDK's condition text; the value is what the SDK
 * printed (0 where it printed nothing). */
#define AFH_ASSERT(cond, what, value) \
    do { if (!(cond)) btctrler_assert_failed(what, (int)(value)); } while (0)

static inline void atomic_set(atomic_t *v, int i)
{
    local_irq_disable();
    DO_CSYNC();
    v->counter = i;
    local_irq_enable();
}

/* The SDK's atomic_read as it appears in the handlers: barrier, then a
 * volatile load. */
static inline int afh_pend_read(struct afh_param *afh)
{
    DO_CSYNC();
    return *(volatile int *)&afh->afh_pend.counter;
}

static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

static inline void list_add(struct list_head *new, struct list_head *head)
{
    struct list_head *next = head->next;
    next->prev = new;
    new->next = next;
    new->prev = head;
    head->next = new;
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
    struct list_head *prev = head->prev;
    head->prev = new;
    new->next = head;
    new->prev = prev;
    prev->next = new;
}

static inline void list_del(struct list_head *entry)
{
    struct list_head *prev = entry->prev;
    struct list_head *next = entry->next;
    next->prev = prev;
    prev->next = next;
    entry->next = entry;
    entry->prev = entry;
}

/* How many connection events one channel gets before the statistics round
 * is complete: 7000 ms over the interval (1.25 ms units) and channel count,
 * plus one, truncated to a byte. */
static u8 CUR_INTERVAL_EFFECT_TIMES(u16 interval, u8 ch_nums) LL_CODE;
static u8 CUR_INTERVAL_EFFECT_TIMES(u16 interval, u8 ch_nums)
{
    return (u8)(7000 / (((u32)interval * 5 >> 2) * ch_nums) + 1);
}

/* Insert member into a list ordered by history weight, lowest first: it goes
 * in front of the first entry whose weight is not below its own. */
static void channle_weight_sort(struct _channel_quality *member, struct list_head *head) LL_CODE;
static void channle_weight_sort(struct _channel_quality *member, struct list_head *head)
{
    struct _channel_quality *p;
    for (p = (struct _channel_quality *)head->next; &p->entry != head;
         p = (struct _channel_quality *)p->entry.next) {
        if (member->history_weight > p->history_weight) continue;
        list_add(&member->entry, p->entry.prev);
        return;
    }
    list_add_tail(&member->entry, head);
}

/* Insert member into the good list ordered by average weight per effect
 * time, lowest first (in front of the first entry that is not below it). */
static inline void channle_average_weight_sort(struct _channel_quality *member, struct list_head *head)
{
    struct _channel_quality *p;
    for (p = (struct _channel_quality *)head->next; &p->entry != head;
         p = (struct _channel_quality *)p->entry.next) {
        if (log_tag_const_e_LL_AFH) {
            AFH_ASSERT(member->effect_times != 0, "member->effect_times mem div 0", 0);
            AFH_ASSERT(p->effect_times != 0, "p->effect_times list div 0", 0);
        }
        if ((u32)member->average_weight / member->effect_times > (u32)p->average_weight / p->effect_times)
            continue;
        list_add(&member->entry, p->entry.prev);
        return;
    }
    list_add_tail(&member->entry, head);
}

/* The channel map was applied: allow the next statistics round. */
void ll_update_ch_done_afh_callback(struct le_link *link) LL_CODE;
void ll_update_ch_done_afh_callback(struct le_link *link)
{
    if (config_btctler_le_afh_en) {
        struct afh_param *afh = link->ll_data.afh_param;
        atomic_set(&afh->afh_pend, 4);
        atomic_set(&afh->afh_pend, 1);
        if (log_tag_const_i_LL_AFH)
            log_print(2, NULL, "[Info]: [LL_AFH]--- AFH OUTPUT DONE \r\n\r\n");
    }
}

/* End of a connection event on channel param: turn the channel's receive
 * counts into a weight, rank the channel in the good list and, when a
 * round is complete, update the history weights and hand over to the link
 * layer thread (state 1 to 2). */
void conn_evt_probe_evtchannel_handler(struct le_link *link, int param) LL_CODE;
void conn_evt_probe_evtchannel_handler(struct le_link *link, int param)
{
    if (!(config_btctler_le_roles & 1) || !config_btctler_le_afh_en) return;
    if (link->role == 1 || link->state != 7) return;

    struct afh_param *afh = link->ll_data.afh_param;
    if (!afh) {
        putchar(65);
        return;
    }
    if (afh_pend_read(afh) != 1) return;

    struct _channel_quality *ch = &afh->channel_quality[param];
    struct afh_param *afh2 = link->ll_data.afh_param;
    u8 d = log_tag_const_d_LL_AFH;

    if (d) log_print(1, NULL, "[Debug]: [LL_AFH]ch=%2d  \r\n", (int)(ch - afh2->channel_quality) & 255);

    u16 current_weight = 0;
    if (ch->rx_cnt != 0) {
        u32 hit = (u32)ch->rx_cnt * 1000 / (u32)ch->expected_rx_cnt;
        if (d) {
            log_print(1, NULL, "[Debug]: [LL_AFH]cnt=%d %d  \r\n", (int)ch->rx_cnt, (int)ch->expected_rx_cnt);
            log_print(1, NULL, "[Debug]: [LL_AFH]a_rssi=%d %d  \r\n", (int)ch->rx_rssi_average,
                      (int)ch->rx_rssi_average / (int)ch->rx_cnt);
        }
        current_weight = (u16)((int)ch->rx_rssi_average / (int)ch->rx_cnt + (int)hit);
        ch->rx_rssi_average = 0;
        ch->rx_cnt = 0;
        ch->expected_rx_cnt = 0;
    }
    if (d) log_print(1, NULL, "[Debug]: [LL_AFH]cur_w=%d  \r\n", (int)current_weight);

    u8 e = log_tag_const_e_LL_AFH;
    if (e)
        AFH_ASSERT((u32)ch->average_weight + current_weight < 16777216,
                   "(current_channel->average_weight + current_weight) <= ((2 << 23) - 1)", ch->average_weight);
    ch->average_weight = ch->average_weight + current_weight;
    if (e)
        AFH_ASSERT(ch->effect_times != 255, "(u8)(current_channel->effect_times + 1)", ch->effect_times);
    ch->effect_times = (u8)(ch->effect_times + 1);
    if (d && ch->effect_times == 255)
        log_print(1, NULL, "[Debug]: [LL_AFH]effect_times MAX LIMIT\r\n\r\n");

    /* Re-rank the channel: out of whatever list it is in, into the good
     * list by average weight per effect time (lowest first). */
    struct list_head *entry = &ch->entry;
    if (entry->next != entry && entry->next != NULL) list_del(entry);
    struct list_head *good = &afh2->good_ch_list_head;
    channle_average_weight_sort(ch, good);

    u16 interval = link->ll_data.interval;
    u8 online = afh2->new_online_channels_nums;
    u8 now_channel_effect_times = CUR_INTERVAL_EFFECT_TIMES(interval, online);
    if (e)
        AFH_ASSERT(now_channel_effect_times != 255, "now_channel_effect_times < ((2 << 7) - 1) overflow", 0);
    if (now_channel_effect_times == 0) {
        if (d && log_tag_const_w_LL_AFH)
            log_print(1, NULL, "[Debug]: [LL_AFH]update period may=%d ms\r\n\r\n", (int)(((u32)interval * 5 >> 2) * online));
        now_channel_effect_times = 1;
    }
    if (d)
        log_print(1, NULL, "[Debug]: [LL_AFH]effect=%d %d  \r\n", (int)now_channel_effect_times,
                  (int)ch->pre_interval_effect);

    if (now_channel_effect_times != ch->pre_interval_effect) {
        u32 effect_times = ch->effect_times;
        if (effect_times > now_channel_effect_times && effect_times <= ch->pre_interval_effect) {
            afh2->complete_sample_nums++;
            if (d)
                log_print(1, NULL, "[Debug]: [LL_AFH]ave_w0=%d\r\n\r\n", (int)((u32)ch->average_weight / ch->effect_times));
        }
        ch->pre_interval_effect = now_channel_effect_times;
    }

    u8 effect_times = ch->effect_times;
    if (effect_times < now_channel_effect_times) {
        if (d) log_print(1, NULL, "[Debug]: [LL_AFH]\r\n\r\n");
        return;
    }
    if (effect_times == now_channel_effect_times) {
        afh2->complete_sample_nums++;
        if (d)
            log_print(1, NULL, "[Debug]: [LL_AFH]ave_w1=%d\r\n\r\n", (int)((u32)ch->average_weight / ch->effect_times));
    } else if (d) {
        log_print(1, NULL, "[Debug]: [LL_AFH]\r\n\r\n");
    }

    if (afh2->complete_sample_nums == afh2->new_online_channels_nums || ch->effect_times == 255) {
        afh2->complete_sample_nums = 0;
        struct _channel_quality *p;
        for (p = (struct _channel_quality *)good->next; &p->entry != good;
             p = (struct _channel_quality *)p->entry.next) {
            u32 average = (u32)p->average_weight / p->effect_times;
            p->history_weight = (u16)((p->history_weight + average) >> 1);
            p->effect_times = 0;
            p->average_weight = 0;
        }
        atomic_set(&afh->afh_pend, 2);
        thread_resume(&ll_hdl.thread);
    }
}

/* One received packet on a data channel: count it (and, with MD set, one
 * more expected packet) and add its RSSI. */
void conn_rx_probe_channel_handler(struct le_link *link, struct ble_rx *rx) LL_CODE;
void conn_rx_probe_channel_handler(struct le_link *link, struct ble_rx *rx)
{
    if (!(config_btctler_le_roles & 1) || !config_btctler_le_afh_en) return;
    if (link->role == 1 || link->state != 7) return;

    u8 channel = rx->channel;
    s8 rssi = rx->rssi;
    u8 md = rx->md;
    struct afh_param *afh = link->ll_data.afh_param;
    if (afh_pend_read(afh) != 1) return;

    struct _channel_quality *ch = &afh->channel_quality[channel];
    u8 e = log_tag_const_e_LL_AFH;
    if (e)
        AFH_ASSERT(ch->rx_cnt + 1 <= 4095, "(current_channel->rx_cnt + 1) <= ((2 << 11) - 1)", ch->rx_cnt);
    ch->rx_cnt = ch->rx_cnt + 1;
    /* The SDK writes the new rx_cnt over expected_rx_cnt here. */
    ch->expected_rx_cnt = ch->rx_cnt;
    if (md) {
        if (e)
            AFH_ASSERT(ch->expected_rx_cnt + 1 <= 4095, "(current_channel->expected_rx_cnt + 1) <= ((2 << 11) - 1)",
                       ch->expected_rx_cnt);
        ch->expected_rx_cnt = ch->expected_rx_cnt + 1;
    }
    if (e) {
        AFH_ASSERT((int)ch->rx_rssi_average + (int)rssi >= -32768,
                   "(current_channel->rx_rssi_average + input->rssi) >= (-(2 << 14))", ch->rx_rssi_average);
        AFH_ASSERT((int)ch->rx_rssi_average + (int)rssi < 32768,
                   "(current_channel->rx_rssi_average + input->rssi) <= ((2 << 14) - 1)", ch->rx_rssi_average);
    }
    ch->rx_rssi_average = (s16)(ch->rx_rssi_average + rssi);
}

/* The link layer thread's step once a round is complete (state 2): when
 * fewer than 13 channels would stay, first put some channels from the bad
 * list back into the map; then take the first two entries of the good list
 * out of the map and into the bad list, empty the good list, ask the
 * controller to apply the map (state 3). */
void ll_update_channel_map_post_handler(struct le_link *link) LL_CODE;
void ll_update_channel_map_post_handler(struct le_link *link)
{
    if (!(config_btctler_le_roles & 1) || !config_btctler_le_afh_en) return;
    if (link->role == 1 || link->state != 7) return;

    struct afh_param *afh = link->ll_data.afh_param;
    if (afh_pend_read(afh) != 2) return;

    if (log_tag_const_i_LL_AFH) log_print(2, NULL, "[Info]: [LL_AFH]+++ AFH OUTPUT START \r\n\r\n");

    u8 online = afh->new_online_channels_nums;
    afh->new_online_channels_nums = (u8)(online - 2);
    if ((u8)(online - 2) < 13) {
        u8 retry_nums;
        pseudo_random_genrate(&retry_nums, 1);
        retry_nums = (u8)btctler_get_rand_from_assign_range(retry_nums, (u8)(15 - online), (u8)(20 - online));
        afh->new_online_channels_nums = (u8)(afh->new_online_channels_nums + retry_nums);
        /* Put channels from the bad list into the map. */
        while (retry_nums--) {
            struct _channel_quality *p = (struct _channel_quality *)afh->bad_ch_list_head.next;
            if (p->history_weight != 0) {
                putchar(61);
                p = (struct _channel_quality *)afh->bad_ch_list_head.prev;
            }
            u32 index = (u32)(p - afh->channel_quality);
            link->ll_data.channel[(index >> 3) & 31] |= (u8)(1 << (index & 7));
            list_del(&p->entry);
        }
    }
    /* The first two good-list entries leave the map and join the bad list. */
    for (u8 i = 0; i != 2; i++) {
        struct list_head *first = afh->good_ch_list_head.next;
        u32 index = (u32)((struct _channel_quality *)first - afh->channel_quality);
        link->ll_data.channel[(index >> 3) & 31] &= (u8)~(1 << (index & 7));
        list_del(first);
        channle_weight_sort((struct _channel_quality *)first, &afh->bad_ch_list_head);
    }
    struct list_head *good = &afh->good_ch_list_head;
    struct list_head *p = good->next;
    while (p != good) {
        struct list_head *next = p->next;
        list_del(p);
        p = next;
    }
    le_channel_map_update_from_controller(link);

    u8 w = log_tag_const_w_LL_AFH;
    u8 d = log_tag_const_d_LL_AFH;
    if (w && d) log_print(1, NULL, "[Debug]: [LL_AFH]ch: \r\n");
    for (int i = 0; i != 37; i++) {
        if (d && w && (link->ll_data.channel[i >> 3] & (1 << (i & 7))))
            log_print(1, NULL, "[Debug]: [LL_AFH] %d\r\n", i);
    }
    if (w && d) log_print(1, NULL, "[Debug]: [LL_AFH]\r\n\r\n");
    atomic_set(&afh->afh_pend, 3);
}

/* New master connection: allocate and initialise the statistics (every
 * channel in use, the bad list holding only channels the connection
 * parameters leave out) and mark it ready (state 1). */
void ll_afh_open(struct ble_conn_param *conn_param) LL_CODE;
void ll_afh_open(struct ble_conn_param *conn_param)
{
    if (!(config_btctler_le_roles & 1) || !config_btctler_le_afh_en) return;

    u8 i_tag = log_tag_const_i_LL_AFH;
    if (i_tag) log_print(2, NULL, "[Info]: [LL_AFH]--func=%s\r\n", "ll_afh_open");
    conn_param->afh_param = malloc(764);
    AFH_ASSERT(conn_param->afh_param != NULL, "conn_param->afh_param ", 0);
    memset(conn_param->afh_param, 0, 764);

    struct afh_param *afh = conn_param->afh_param;
    afh->new_online_channels_nums = 37;
    INIT_LIST_HEAD(&afh->bad_ch_list_head);
    INIT_LIST_HEAD(&afh->good_ch_list_head);
    AFH_ASSERT(conn_param->interval != 0, "conn_param->interval init interval=0?", 0);
    u8 first_effect_times = CUR_INTERVAL_EFFECT_TIMES(conn_param->interval, afh->new_online_channels_nums);
    if (log_tag_const_e_LL_AFH) {
        AFH_ASSERT(first_effect_times != 255, "first_effect_times < ((2 << 7) - 1) overflow", 0);
        if (first_effect_times != 255)
            AFH_ASSERT(first_effect_times != 0, "first_effect_times : Can not be 0", 0);
    }
    if (i_tag) log_print(2, NULL, "[Info]: [LL_AFH]init channel_effect_times=%d\r\n", (int)first_effect_times);
    memset(&afh->channel_quality, 0, 740);
    memset(conn_param->channel, 0, 5);
    for (int i = 0; i != 37; i++) {
        u8 channel = first_used_channels[i];
        conn_param->channel[channel >> 3] |= (u8)(1 << (channel & 7));
    }
    if (i_tag) {
        log_print(2, NULL, "[Info]: [LL_AFH]--first_used_channels_map\r\n");
        printf_buf(conn_param->channel, 5);
    }
    for (int i = 0; i != 37; i++) {
        if (!(conn_param->channel[i >> 3] & (1 << (i & 7))))
            channle_weight_sort(&afh->channel_quality[i], &afh->bad_ch_list_head);
        afh->channel_quality[i].pre_interval_effect = first_effect_times;
    }
    atomic_set(&afh->afh_pend, 1);
}

/* Connection closed: free the statistics. */
void ll_afh_close(struct le_link *link) LL_CODE;
void ll_afh_close(struct le_link *link)
{
    if (!(config_btctler_le_roles & 1) || !config_btctler_le_afh_en) return;
    if (log_tag_const_i_LL_AFH) log_print(2, NULL, "[Info]: [LL_AFH]--func=%s\r\n", "ll_afh_close");
    if (link && link->ll_data.afh_param) {
        free(link->ll_data.afh_param);
        link->ll_data.afh_param = NULL;
    }
}

#ifdef UNITTEST
#include "test.h"

int config_btctler_le_afh_en = 1, config_btctler_le_roles = 1, config_asser;
u8 log_tag_const_i_LL_AFH, log_tag_const_d_LL_AFH, log_tag_const_e_LL_AFH, log_tag_const_w_LL_AFH;
struct link_layer_handle ll_hdl;

void *calloc(unsigned long n, unsigned long size);
static struct afh_param *last_alloc;
static void *freed;
void *afh_test_malloc(unsigned int size)
{
    CHECK(size == 764);
    last_alloc = calloc(1, sizeof(struct afh_param));
    return last_alloc;
}
void afh_test_free(void *ptr) { freed = ptr; }

static int irq_off, irq_on, putchars[8], nputchar, resumes, map_updates, rand_args[3], asserts;
static const char *last_assert;
static int last_assert_value, map_at_update;
static struct le_link *map_link;
void local_irq_disable(void) { irq_off++; }
void local_irq_enable(void) { irq_on++; }
int putchar(int c) { putchars[nputchar++ & 7] = c; return c; }
void log_print(int level, const char *tag, const char *format, ...) { (void)level; (void)tag; (void)format; }
void printf_buf(const void *buffer, int size) { (void)buffer; (void)size; }
void thread_resume(struct thread *th) { CHECK(th == &ll_hdl.thread); resumes++; }
void le_channel_map_update_from_controller(struct le_link *link) { map_link = link; map_updates++; map_at_update = link->ll_data.channel[0]; }
void pseudo_random_genrate(u8 *dest, unsigned int size) { CHECK(size == 1); *dest = 0x42; }
uint32_t btctler_get_rand_from_assign_range(uint32_t rand, uint32_t min, uint32_t max)
{
    rand_args[0] = rand; rand_args[1] = min; rand_args[2] = max;
    return 3;
}
void btctrler_assert_failed(const char *what, int value) { asserts++; last_assert = what; last_assert_value = value; }

static struct le_link link;
static struct ble_rx rx;
static struct afh_param *afh;

static void reset(void)
{
    memset(&link, 0, sizeof(link));
    memset(&rx, 0, sizeof(rx));
    config_btctler_le_afh_en = config_btctler_le_roles = 1;
    log_tag_const_i_LL_AFH = log_tag_const_d_LL_AFH = log_tag_const_e_LL_AFH = log_tag_const_w_LL_AFH = 0;
    link.role = 0;
    link.state = 7;
    link.ll_data.interval = 24;
    asserts = nputchar = resumes = map_updates = irq_off = irq_on = 0;
    ll_afh_open(&link.ll_data);
    afh = link.ll_data.afh_param;
}

static struct _channel_quality *cq(int i) { return &afh->channel_quality[i]; }

int main(void)
{
    /* Interval units are 1.25 ms: (interval * 5 >> 2) * channels is the
     * round time in ms; 7000 / that + 1 as a byte. */
    CHECK(CUR_INTERVAL_EFFECT_TIMES(24, 37) == 7);      /* 30 ms * 37 = 1110; 7000/1110 = 6 */
    CHECK(CUR_INTERVAL_EFFECT_TIMES(1, 37) == 190);     /* 7000 / 37 = 189 */
    CHECK(CUR_INTERVAL_EFFECT_TIMES(3000, 37) == 1);    /* 7000 / 138750 = 0 */
    CHECK(CUR_INTERVAL_EFFECT_TIMES(1, 1) == 89);       /* 7001 mod 256 */
    CHECK(CUR_INTERVAL_EFFECT_TIMES(4, 1) == 121);      /* 7000 / 5 + 1 = 1401 mod 256 */

    /* Open: statistics zeroed except the 37 online channels and the first
     * effect-times, every channel in the map (bits 0..36), empty lists, state 1. */
    reset();
    CHECK(afh == last_alloc);
    CHECK(afh->new_online_channels_nums == 37 && afh->complete_sample_nums == 0);
    CHECK(afh->afh_pend.counter == 1);
    CHECK(afh->bad_ch_list_head.next == &afh->bad_ch_list_head && afh->bad_ch_list_head.prev == &afh->bad_ch_list_head);
    CHECK(afh->good_ch_list_head.next == &afh->good_ch_list_head && afh->good_ch_list_head.prev == &afh->good_ch_list_head);
    CHECK(!memcmp(link.ll_data.channel, "\xff\xff\xff\xff\x1f", 5));
    for (int i = 0; i < 37; i++)
        CHECK(cq(i)->pre_interval_effect == 7 && cq(i)->rx_cnt == 0 && cq(i)->effect_times == 0 && cq(i)->history_weight == 0);
    CHECK(irq_off == 1 && irq_on == 1);
    CHECK(asserts == 0);

    /* Open disabled by config: nothing allocated, nothing touched. */
    for (int mode = 0; mode < 2; mode++) {
        reset();
        struct afh_param *before = afh;
        link.ll_data.afh_param = NULL;
        last_alloc = NULL;
        if (mode) config_btctler_le_roles = 2; else config_btctler_le_afh_en = 0;
        ll_afh_open(&link.ll_data);
        CHECK(last_alloc == NULL && link.ll_data.afh_param == NULL);
        link.ll_data.afh_param = before;
        freed = NULL;
        ll_afh_close(&link);
        CHECK(freed == NULL && link.ll_data.afh_param == before);
    }

    /* Close: frees and clears; a null link or null statistics is fine. */
    reset();
    freed = NULL;
    ll_afh_close(&link);
    CHECK(freed == afh && link.ll_data.afh_param == NULL);
    freed = NULL;
    ll_afh_close(&link);
    ll_afh_close(NULL);
    CHECK(freed == NULL);

    /* Open's asserts (error tag set): a zero interval is reported; the
     * all-channels start is not. */
    reset();
    log_tag_const_e_LL_AFH = 1;
    link.ll_data.interval = 5;
    ll_afh_open(&link.ll_data);
    CHECK(asserts == 0 && last_alloc->channel_quality[0].pre_interval_effect == 32);   /* 7000 / (6 * 37) = 31 */
    link.ll_data.interval = 3000;
    ll_afh_open(&link.ll_data);
    CHECK(asserts == 0 && last_alloc->channel_quality[0].pre_interval_effect == 1);

    /* Received packets: the counts only change in state 1 for a master link
     * in state 7. */
    reset();
    rx.channel = 5;
    rx.rssi = -40;
    conn_rx_probe_channel_handler(&link, &rx);
    CHECK(cq(5)->rx_cnt == 1 && cq(5)->expected_rx_cnt == 1 && cq(5)->rx_rssi_average == -40);
    rx.md = 1;
    rx.rssi = 10;
    conn_rx_probe_channel_handler(&link, &rx);
    CHECK(cq(5)->rx_cnt == 2 && cq(5)->expected_rx_cnt == 3 && cq(5)->rx_rssi_average == -30);
    /* The SDK overwrites expected_rx_cnt with rx_cnt: a prior mismatch is lost. */
    cq(5)->expected_rx_cnt = 100;
    rx.md = 0;
    conn_rx_probe_channel_handler(&link, &rx);
    CHECK(cq(5)->rx_cnt == 3 && cq(5)->expected_rx_cnt == 3 && cq(5)->rx_rssi_average == -20);
    for (int blocked = 0; blocked < 4; blocked++) {
        int before = cq(5)->rx_cnt;
        if (blocked == 0) afh->afh_pend.counter = 2;
        if (blocked == 1) link.role = 1;
        if (blocked == 2) link.state = 6;
        if (blocked == 3) config_btctler_le_afh_en = 0;
        conn_rx_probe_channel_handler(&link, &rx);
        CHECK(cq(5)->rx_cnt == before);
        afh->afh_pend.counter = 1; link.role = 0; link.state = 7; config_btctler_le_afh_en = 1;
    }
    config_btctler_le_roles = 2;
    conn_rx_probe_channel_handler(&link, &rx);
    CHECK(cq(5)->rx_cnt == 3);
    config_btctler_le_roles = 1;
    /* Counter wrap and the RSSI sum limits are asserted only with the error tag. */
    cq(5)->rx_cnt = 4095;
    conn_rx_probe_channel_handler(&link, &rx);
    CHECK(asserts == 0 && cq(5)->rx_cnt == 0 && cq(5)->expected_rx_cnt == 0);
    log_tag_const_e_LL_AFH = 1;
    cq(5)->rx_cnt = 4095;
    conn_rx_probe_channel_handler(&link, &rx);
    CHECK(asserts == 1 && last_assert_value == 4095 && cq(5)->rx_cnt == 0);
    cq(5)->rx_cnt = 1; cq(5)->rx_rssi_average = -32768;
    rx.rssi = -1;
    asserts = 0;
    conn_rx_probe_channel_handler(&link, &rx);
    CHECK(asserts == 1 && last_assert_value == -32768 && cq(5)->rx_rssi_average == 32767);
    cq(5)->rx_rssi_average = 32767; rx.rssi = 1;
    asserts = 0;
    conn_rx_probe_channel_handler(&link, &rx);
    CHECK(asserts == 1 && cq(5)->rx_rssi_average == -32768);
    cq(5)->rx_rssi_average = 32766; rx.rssi = 1;
    asserts = 0;
    conn_rx_probe_channel_handler(&link, &rx);
    CHECK(asserts == 0 && cq(5)->rx_rssi_average == 32767);
    cq(5)->rx_rssi_average = -32767; rx.rssi = -1;
    conn_rx_probe_channel_handler(&link, &rx);
    CHECK(asserts == 0 && cq(5)->rx_rssi_average == -32768);

    /* Event handler: channel 3 saw 10 of 20 expected packets with RSSI sum
     * -500: weight -500/10 + 10*1000/20 = 450, the counts are cleared, the
     * channel's average_weight and effect_times start; the round is not
     * complete (effect_times 1 < 7), the channel goes to the good list. */
    reset();
    cq(3)->rx_cnt = 10; cq(3)->expected_rx_cnt = 20; cq(3)->rx_rssi_average = -500;
    conn_evt_probe_evtchannel_handler(&link, 3);
    CHECK(cq(3)->rx_cnt == 0 && cq(3)->expected_rx_cnt == 0 && cq(3)->rx_rssi_average == 0);
    CHECK(cq(3)->average_weight == 450 && cq(3)->effect_times == 1 && cq(3)->pre_interval_effect == 7);
    CHECK(afh->good_ch_list_head.next == &cq(3)->entry && afh->good_ch_list_head.prev == &cq(3)->entry);
    CHECK(afh->complete_sample_nums == 0 && afh->afh_pend.counter == 1 && resumes == 0);
    /* Channel 4: all 10 of 10 at RSSI 0: weight 1000.  The good list is
     * ordered by average weight / effect_times, lowest first. */
    cq(4)->rx_cnt = 10; cq(4)->expected_rx_cnt = 10;
    conn_evt_probe_evtchannel_handler(&link, 4);
    CHECK(cq(4)->average_weight == 1000 && afh->good_ch_list_head.next == &cq(3)->entry && cq(3)->entry.next == &cq(4)->entry);
    /* Channel 5: weight 700 goes between them. */
    cq(5)->rx_cnt = 7; cq(5)->expected_rx_cnt = 10;
    conn_evt_probe_evtchannel_handler(&link, 5);
    CHECK(cq(5)->average_weight == 700);
    CHECK(afh->good_ch_list_head.next == &cq(3)->entry && cq(3)->entry.next == &cq(5)->entry);
    CHECK(cq(5)->entry.next == &cq(4)->entry && cq(4)->entry.next == &afh->good_ch_list_head);
    CHECK(cq(4)->entry.prev == &cq(5)->entry && afh->good_ch_list_head.prev == &cq(4)->entry);
    /* Channel 3 again with no packets: weight 0; it leaves the list and is
     * ranked again by 450/2 = 225, still first. */
    conn_evt_probe_evtchannel_handler(&link, 3);
    CHECK(cq(3)->average_weight == 450 && cq(3)->effect_times == 2);
    CHECK(afh->good_ch_list_head.next == &cq(3)->entry && cq(3)->entry.next == &cq(5)->entry);
    /* Channel 5 (700/2 = 350 after its second pass) stays above channel 3. */
    conn_evt_probe_evtchannel_handler(&link, 5);
    CHECK(cq(5)->effect_times == 2 && afh->good_ch_list_head.next == &cq(3)->entry && cq(3)->entry.next == &cq(5)->entry);
    CHECK(cq(5)->entry.next == &cq(4)->entry);
    /* Statistics not ready (state 2), role, state, config and a missing
     * afh_param: nothing changes (a missing one prints 'A'). */
    afh->afh_pend.counter = 2;
    conn_evt_probe_evtchannel_handler(&link, 3);
    CHECK(cq(3)->effect_times == 2);
    afh->afh_pend.counter = 1;
    link.role = 1;
    conn_evt_probe_evtchannel_handler(&link, 3);
    link.role = 0; link.state = 8;
    conn_evt_probe_evtchannel_handler(&link, 3);
    link.state = 7; config_btctler_le_afh_en = 0;
    conn_evt_probe_evtchannel_handler(&link, 3);
    config_btctler_le_afh_en = 1; config_btctler_le_roles = 2;
    conn_evt_probe_evtchannel_handler(&link, 3);
    config_btctler_le_roles = 1;
    CHECK(cq(3)->effect_times == 2 && nputchar == 0);
    link.ll_data.afh_param = NULL;
    conn_evt_probe_evtchannel_handler(&link, 3);
    CHECK(nputchar == 1 && putchars[0] == 65);
    link.ll_data.afh_param = afh;

    /* A round completes when the channel reaches the round's effect-times
     * and the completed channels equal the online channels: history weights
     * become (history + average / effect) / 2, the per-round sums restart,
     * the thread is woken, state 2. */
    reset();
    cq(3)->rx_cnt = 10; cq(3)->expected_rx_cnt = 20; cq(3)->rx_rssi_average = -500;
    cq(3)->effect_times = 6; cq(3)->average_weight = 700; cq(3)->history_weight = 100;
    afh->complete_sample_nums = 36;
    conn_evt_probe_evtchannel_handler(&link, 3);
    /* 700 + 450 = 1150, effect 7 == round effect 7: 1150 / 7 = 164. */
    CHECK(cq(3)->history_weight == (100 + 164) / 2);
    CHECK(cq(3)->effect_times == 0 && cq(3)->average_weight == 0 && cq(3)->pre_interval_effect == 7);
    CHECK(afh->complete_sample_nums == 0 && afh->afh_pend.counter == 2 && resumes == 1);
    /* Or when a channel's effect_times saturates at 255. */
    reset();
    cq(7)->effect_times = 254; cq(7)->average_weight = 2550; cq(7)->history_weight = 11;
    conn_evt_probe_evtchannel_handler(&link, 7);
    /* average 2550 + 0, effect 255: 10; (11 + 10) / 2 = 10; not past the round's 7 */
    CHECK(cq(7)->history_weight == 10 && cq(7)->effect_times == 0 && afh->afh_pend.counter == 2 && resumes == 1);
    /* A channel past the round effect-times (effect 8 > 7 and <= pre 9) counts as complete once. */
    reset();
    cq(9)->effect_times = 7; cq(9)->pre_interval_effect = 9; cq(9)->average_weight = 80;
    conn_evt_probe_evtchannel_handler(&link, 9);
    CHECK(cq(9)->effect_times == 8 && cq(9)->pre_interval_effect == 7 && afh->complete_sample_nums == 1);
    /* Previous round's effect-times lower than the current: changed without counting. */
    reset();
    cq(9)->effect_times = 7; cq(9)->pre_interval_effect = 5;
    conn_evt_probe_evtchannel_handler(&link, 9);
    CHECK(cq(9)->pre_interval_effect == 7 && afh->complete_sample_nums == 0);
    /* Overflow asserts (error tag): effect_times 255 and the weight sum. */
    reset();
    log_tag_const_e_LL_AFH = 1;
    cq(2)->effect_times = 255;
    cq(2)->average_weight = 1;
    conn_evt_probe_evtchannel_handler(&link, 2);
    CHECK(asserts == 1 && last_assert_value == 255 && cq(2)->effect_times == 0);
    asserts = 0;
    cq(2)->effect_times = 1;
    cq(2)->average_weight = 16777215;
    cq(2)->rx_cnt = 1; cq(2)->expected_rx_cnt = 1;     /* weight 1000 */
    conn_evt_probe_evtchannel_handler(&link, 2);
    CHECK(asserts == 1 && last_assert_value == 16777215 && cq(2)->average_weight == (16777215 + 1000) % 16777216);

    /* Channel map post handler: first two good-list entries leave the map
     * and join the bad list (lowest history weight first); the good list is
     * emptied; state 3. */
    reset();
    afh->new_online_channels_nums = 37;
    cq(3)->history_weight = 50; cq(5)->history_weight = 20;
    list_add_tail(&cq(3)->entry, &afh->good_ch_list_head);
    list_add_tail(&cq(5)->entry, &afh->good_ch_list_head);
    list_add_tail(&cq(4)->entry, &afh->good_ch_list_head);
    afh->afh_pend.counter = 1;
    ll_update_channel_map_post_handler(&link);
    CHECK(map_updates == 0 && link.ll_data.channel[0] == 0xff);
    afh->afh_pend.counter = 2;
    ll_update_channel_map_post_handler(&link);
    CHECK(afh->new_online_channels_nums == 35 && map_updates == 1 && map_link == &link);
    CHECK(map_at_update == 0xd7);
    CHECK(link.ll_data.channel[0] == 0xd7 && link.ll_data.channel[1] == 0xff && link.ll_data.channel[4] == 0x1f);
    CHECK(afh->bad_ch_list_head.next == &cq(5)->entry && cq(5)->entry.next == &cq(3)->entry);
    CHECK(cq(3)->entry.next == &afh->bad_ch_list_head && afh->bad_ch_list_head.prev == &cq(3)->entry);
    CHECK(afh->good_ch_list_head.next == &afh->good_ch_list_head && afh->good_ch_list_head.prev == &afh->good_ch_list_head);
    CHECK(cq(4)->entry.next == &cq(4)->entry && cq(4)->entry.prev == &cq(4)->entry);
    CHECK(afh->afh_pend.counter == 3);
    /* Not in state 2 again: nothing. */
    ll_update_channel_map_post_handler(&link);
    CHECK(map_updates == 1);

    /* With fewer than 13 channels left the handler first re-enables bad
     * channels: online 14 - 2 = 12; the random count is in [15-14, 20-14]
     * (here 3); the bad list head is taken, or its tail when that has a
     * history weight (printing '='). */
    reset();
    afh->new_online_channels_nums = 14;
    memset(link.ll_data.channel, 0, 5);
    link.ll_data.channel[1] = 0x0c;              /* channels 10 and 11 */
    cq(0)->history_weight = 0; cq(1)->history_weight = 5; cq(2)->history_weight = 7;
    list_add_tail(&cq(0)->entry, &afh->bad_ch_list_head);
    list_add_tail(&cq(1)->entry, &afh->bad_ch_list_head);
    list_add_tail(&cq(2)->entry, &afh->bad_ch_list_head);
    list_add_tail(&cq(10)->entry, &afh->good_ch_list_head);
    list_add_tail(&cq(11)->entry, &afh->good_ch_list_head);
    afh->afh_pend.counter = 2;
    ll_update_channel_map_post_handler(&link);
    CHECK(rand_args[0] == 0x42 && rand_args[1] == 1 && rand_args[2] == 6);
    CHECK(afh->new_online_channels_nums == 15);
    /* Round 1 takes channel 0 (weight 0); round 2 the head is 1 (weight 5): the tail, 2; round 3 the head 1 again. */
    CHECK(nputchar == 2 && putchars[0] == 61 && putchars[1] == 61);
    CHECK(link.ll_data.channel[0] == 0x07 && link.ll_data.channel[1] == 0x00);
    /* Channels 10 and 11 then join the bad list ordered by weight 0: both equal, the second goes in front of the first. */
    CHECK(afh->bad_ch_list_head.next == &cq(11)->entry && cq(11)->entry.next == &cq(10)->entry);
    CHECK(afh->afh_pend.counter == 3);

    /* Done callback: states 4 then 1; disabled config does nothing. */
    reset();
    afh->afh_pend.counter = 3;
    irq_off = irq_on = 0;
    ll_update_ch_done_afh_callback(&link);
    CHECK(afh->afh_pend.counter == 1 && irq_off == 2 && irq_on == 2);
    afh->afh_pend.counter = 3;
    config_btctler_le_afh_en = 0;
    ll_update_ch_done_afh_callback(&link);
    CHECK(afh->afh_pend.counter == 3);

    /* The initial channel table is 0..36. */
    for (int i = 0; i < 37; i++) CHECK(first_used_channels[i] == i);
    return TEST_DONE("btctrler_ll_afh");
}
#endif
