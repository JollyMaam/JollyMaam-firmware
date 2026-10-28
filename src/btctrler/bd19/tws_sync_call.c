/* BD19 SDK ownership for tws_sync_call.c.o, occurrence 1.
 * The BD19 implementation is a timer-backed linked queue and has a separate
 * by-link protocol path; it is not the BR23 ring-buffer implementation.
 */
#include "btctrler/glue.h"
#include <stdlib.h>
#include <string.h>

#define TWS_CODE __attribute__((section(".classic_tws_code")))
#define TWS_IRQ_CODE __attribute__((section(".tws_irq_code")))

#ifdef UNITTEST
static void *sync_test_malloc(u32 size);
static void sync_test_free(void *ptr);
#define malloc sync_test_malloc
#define free sync_test_free
#endif

static struct list_head sync_call_head = {
    .next = &sync_call_head,
    .prev = &sync_call_head,
};

struct tws_sync_call_data sc_slot = {
    .entry = { 0 },
    .tx = 0,
    .rx = 0,
    .timer = 255,
    .uuid = 0,
    .priv = 0,
    .instant = 0,
};

extern const struct tws_sync_call tws_sync_call_begin[];
extern const struct tws_sync_call tws_sync_call_end[];

extern u32 tws_link_read_slot_clk(void);
extern int tws_conn_send_cmd(u8 type, const char *format, ...);
extern u8 slot_timer_get(void);
extern void slot_timer_set(u8 timer, void (*callback)(void *), void *priv,
                           int delay);
extern void slot_timer_put(u8 timer);
extern u16 usr_timeout_add(void *priv, void (*callback)(void *), u32 msec,
                           u8 reload);
extern void usr_timer_del(u16 timer);
extern int os_taskq_post_type(const char *task, int type, int argc, int *argv);
extern void tws_core_tws_wait_tx(void);
extern int bredr_offset2clkn(int clock, int offset);
extern int bredr_clkn2offset(int clock, int instant);
extern void sys_key_event_filter_enable(void);
extern void sys_key_event_filter_disable(void);

static int tws_sync_call_tx_events(struct lmp_connection *conn, u8 *buffer,
                                   int err) TWS_IRQ_CODE;
static int tws_sync_call_rx_events_and_tx_events(
    struct lmp_connection *conn, u8 *rx, int len, u8 *tx) TWS_IRQ_CODE;
static void tws_sync_call_rx_events(struct lmp_connection *conn, u8 *data,
                                    int len) TWS_CODE;
static int key_sync_call_event_hander(struct lmp_connection *conn, int event,
                                      int *arg) TWS_CODE;
static void tws_sync_call_tx_events_suss(void) TWS_CODE;
static void tws_cmd_sync_call(struct tws_sync_call_data *sc) TWS_CODE;
static void __call_func_timeout(void *priv) TWS_CODE;
static void list_add_tail(struct list_head *entry);
static void __list_del_entry(struct list_head *entry);

const struct tws_sync_channel tws_sync_call
    __attribute__((section(".tws_sync_channel.9"), aligned(4))) = {
        .channel = 9,
        .attribute = 15,
        .tx_sync_req = tws_sync_call_tx_events,
        .tx_sync_req_suss = (void (*)())tws_sync_call_tx_events_suss,
        .rx_sync_req_and_res = tws_sync_call_rx_events_and_tx_events,
        .tx_sync_res_suss = (void (*)())tws_sync_call_tx_events_suss,
        .rx_sync_res = tws_sync_call_rx_events,
        .event_handler = key_sync_call_event_hander,
    };

static void list_add_tail(struct list_head *entry)
{
    struct list_head *prev = sync_call_head.prev;
    sync_call_head.prev = entry;
    entry->next = &sync_call_head;
    entry->prev = prev;
    prev->next = entry;
}

static void __list_del_entry(struct list_head *entry)
{
    struct list_head *next = entry->next;
    struct list_head *prev = entry->prev;
    next->prev = prev;
    prev->next = next;
}

static void tws_cmd_sync_call(struct tws_sync_call_data *sc)
{
    int clock = (int)tws_link_read_slot_clk();
    int offset = bredr_clkn2offset(clock, sc->instant);
    struct link_connection *link;

    local_irq_disable();
    link = tws_link.link;
    if (link != NULL) {
        u32 delta = (u32)offset - 4u;
        int mul = delta > 69119996u ? 2500 : (int)((u32)offset * 625u);
        int delay = mul / 1000;
        sc->timer = usr_timeout_add(sc, __call_func_timeout, (u32)delay, 1);
        list_add_tail(&sc->entry);
        local_irq_enable();
        return;
    }
    local_irq_enable();

    if (sc->tx != 0) {
        const struct tws_sync_call *call;
        for (call = tws_sync_call_begin; call < tws_sync_call_end; call++) {
            if (call->uuid == sc->uuid) {
                if (call->func != NULL)
                    call->func(sc->priv, -1);
                break;
            }
        }
    }
    free(sc);
}

int tws_api_sync_call_by_uuid(int uuid, int priv, int delay_ms) TWS_CODE;
int tws_api_sync_call_by_uuid(int uuid, int priv, int delay_ms)
{
    struct lmp_connection *conn;
    struct tws_sync_call_data *sc;

    local_irq_disable();
    if (tws_in_sniff_state()) {
        if (tws_link.plink != NULL)
            lmp_tx_unsniff_req(tws_conn.lmp_conn);
        else
            tws_tx_unsniff_req();
        local_irq_enable();
        if (!cpu_in_irq() && !cpu_irq_disabled()) {
            int i = 0;
            sys_key_event_filter_enable();
            do {
                os_time_dly(10);
                int still_sniffing = tws_in_sniff_state();
                i++;
                if (!still_sniffing || i >= 15)
                    break;
            } while (1);
            sys_key_event_filter_disable();
        }
    } else {
        conn = lmp_conn_for_address(NULL);
        if (conn == NULL || !lmp_in_sniff_mode(conn) ||
            (tws_link.link != NULL && tws_link.link->role)) {
            local_irq_enable();
        } else {
            local_irq_enable();
            (void)lmp_hci_cmd_to_conn(&conn->rejust_esco_req, 1, 2052);
            if (!cpu_in_irq() && !cpu_irq_disabled()) {
                int i = 0;
                sys_key_event_filter_enable();
                do {
                    os_time_dly(10);
                    int still_sniffing = lmp_in_sniff_mode(conn);
                    i++;
                    if (!still_sniffing || i >= 15)
                        break;
                } while (1);
                sys_key_event_filter_disable();
            }
        }
    }

    sc = malloc(sizeof(*sc));
    if (sc == NULL)
        return -12;
    sc->tx = 0;
    sc->rx = 0;
    sc->uuid = uuid;
    sc->priv = priv;
    sc->instant = bredr_offset2clkn((int)tws_link_read_slot_clk(),
                                    (delay_ms * 1000) / 625);
    tws_cmd_sync_call(sc);
    tws_core_tws_wait_tx();
    return 0;
}

static void __call_func_timeout(void *priv)
{
    struct tws_sync_call_data *sc = priv;
    const struct tws_sync_call *call;
    int uuid = sc->uuid;

    for (call = tws_sync_call_begin; call < tws_sync_call_end; call++) {
        if (call->uuid == uuid) {
            int event = sc->tx != 0 ? 1 : 2;
            if (call->task_name == NULL) {
                /* The SDK calls through a matching entry without a NULL check. */
                call->func(sc->priv, event);
            } else {
                int args[4] = {
                    (int)(uintptr_t)call->func, 2, sc->priv, event,
                };
                (void)os_taskq_post_type(call->task_name, 0x300000, 4, args);
            }
            break;
        }
    }

    local_irq_disable();
    __list_del_entry(&sc->entry);
    local_irq_enable();
    free(sc);
}

void tws_start_sync_callback(void *priv) TWS_CODE;
void tws_start_sync_callback(void *priv)
{
    const struct tws_sync_call *call;
    int uuid = sc_slot.uuid;
    (void)priv;

    for (call = tws_sync_call_begin; call < tws_sync_call_end; call++) {
        if (call->uuid == uuid) {
            if (call->func != NULL)
                call->func(sc_slot.priv, -1);
            break;
        }
    }

    /* Load after the user callback: the callback is allowed to mutate sc_slot. */
    if (sc_slot.timer != 255) {
        slot_timer_put((u8)sc_slot.timer);
        sc_slot.timer = 255;
    }
}

int tws_api_sync_call_by_link(int uuid, int priv, int delay_ms) TWS_CODE;
int tws_api_sync_call_by_link(int uuid, int priv, int delay_ms)
{
    struct link_connection *link = tws_link.link;
    int offset;

    if (link == NULL)
        return 0;
    local_irq_disable();
    sc_slot.tx = 0;
    sc_slot.rx = 0;
    sc_slot.uuid = uuid;
    sc_slot.priv = priv;
    offset = (delay_ms * 1000) / 625;
    sc_slot.instant = bredr_offset2clkn((int)tws_link_read_slot_clk(), offset);
    (void)tws_conn_send_cmd(10, "441", sc_slot.instant, sc_slot.uuid,
                            sc_slot.priv);
    if (sc_slot.timer == 255)
        sc_slot.timer = slot_timer_get();
    slot_timer_set((u8)sc_slot.timer, tws_start_sync_callback, link, offset);
    local_irq_enable();
    return 1;
}

void tws_sync_call_rx_events_by_link(u8 *rx) TWS_CODE;
void tws_sync_call_rx_events_by_link(u8 *rx)
{
    struct link_connection *link = tws_link.link;
    u32 instant;
    int offset;

    if (link == NULL)
        return;
    instant = (u32)rx[0] | ((u32)rx[1] << 8) | ((u32)rx[2] << 16) |
              ((u32)rx[3] << 24);
    sc_slot.tx = 0;
    sc_slot.rx = 0;
    sc_slot.uuid = (int)((u32)rx[4] | ((u32)rx[5] << 8) |
                         ((u32)rx[6] << 16) | ((u32)rx[7] << 24));
    sc_slot.priv = rx[8];
    offset = bredr_clkn2offset((int)tws_link_read_slot_clk(), (int)instant);
    if (sc_slot.timer == 255)
        sc_slot.timer = slot_timer_get();
    slot_timer_set((u8)sc_slot.timer, tws_start_sync_callback, link,
                   offset > 2 ? offset : 2);
    /* The SDK decodes instant for timer scheduling but leaves sc_slot.instant. */
}

static int tws_sync_call_tx_events(struct lmp_connection *conn, u8 *buffer,
                                   int err)
{
    struct tws_sync_call_data *sc;
    u32 *out = (u32 *)buffer;
    (void)conn;
    (void)err;

    local_irq_disable();
    for (sc = (struct tws_sync_call_data *)sync_call_head.next;
         &sc->entry != &sync_call_head;
         sc = (struct tws_sync_call_data *)sc->entry.next) {
        if (sc->tx == 0 && sc->rx == 0) {
            sc->tx = 1;
            *out++ = (u32)sc->uuid;
            *out++ = (u32)sc->priv;
            *out++ = (u32)sc->instant;
        }
    }
    local_irq_enable();
    return (int)((u8 *)out - buffer);
}

static int tws_sync_call_rx_events_and_tx_events(
    struct lmp_connection *conn, u8 *rx, int len, u8 *tx)
{
    if (len != 0)
        tws_sync_call_rx_events(conn, rx, len);
    return tx == NULL ? 0 : tws_sync_call_tx_events(conn, tx, 0);
}

static void tws_sync_call_rx_events(struct lmp_connection *conn, u8 *data,
                                    int len)
{
    int words = len / 4;
    (void)conn;

    /* This deliberately follows the SDK's padded triple-word protocol: for
     * lengths 4 or 8 it still reads the full 12-byte record. */
    for (int i = 0; len > 3 && i < words; i += 3) {
        u32 *input = (u32 *)data + i;
        struct tws_sync_call_data *sc = malloc(sizeof(*sc));
        if (sc == NULL)
            break;
        sc->tx = 0;
        sc->rx = 1;
        sc->uuid = (int)input[0];
        sc->priv = (int)input[1];
        sc->instant = (int)input[2];
        tws_cmd_sync_call(sc);
    }
}

static int key_sync_call_event_hander(struct lmp_connection *conn, int event,
                                      int *arg)
{
    (void)conn;
    (void)arg;
    if (event == 46) {
        local_irq_disable();
        struct tws_sync_call_data *sc =
            (struct tws_sync_call_data *)sync_call_head.next;
        while (&sc->entry != &sync_call_head) {
            struct tws_sync_call_data *next =
                (struct tws_sync_call_data *)sc->entry.next;
            const struct tws_sync_call *call;
            usr_timer_del(sc->timer);
            __list_del_entry(&sc->entry);
            int uuid = sc->uuid;
            for (call = tws_sync_call_begin; call < tws_sync_call_end; call++) {
                if (call->uuid == uuid) {
                    int event_arg = sc->tx != 0 ? -1 : -2;
                    if (call->func != NULL) {
                        if (call->task_name == NULL) {
                            call->func(sc->priv, event_arg);
                        } else {
                            int args[4] = {
                                (int)(uintptr_t)call->func, 2, sc->priv,
                                event_arg,
                            };
                            (void)os_taskq_post_type(call->task_name,
                                                     0x300000, 4, args);
                        }
                    }
                    break;
                }
            }
            free(sc);
            sc = next;
        }
        local_irq_enable();
    }
    return 0;
}

void tws_sync_call_init(void) TWS_CODE;
void tws_sync_call_init(void)
{
}

static void tws_sync_call_tx_events_suss(void)
{
}

#ifdef UNITTEST
#include "test.h"

static const char registered_task[] = "call-task";
static void callback(int priv, int event);
const struct tws_sync_call tws_sync_call_begin[] = {
    { 0x1234, callback, NULL },
    { 0x2345, callback, registered_task },
    { 0x3456, NULL, NULL },
};
__asm__(".globl tws_sync_call_end\n.set tws_sync_call_end, tws_sync_call_begin + 72");

struct tws_link_connection tws_link;
struct tws_connection tws_conn;
static struct link_connection mock_link;
static struct lmp_connection mock_lmp;
_Alignas(struct tws_sync_call_data)
static unsigned char heap[32][sizeof(struct tws_sync_call_data)];
static int heap_used[32];
static int malloc_calls, free_calls, fail_malloc;
static int irq_depth, irq_disable_calls, irq_enable_calls;
static u32 mock_clock = 100;
static u8 mock_slot_timer = 7;
static int slot_timer_set_calls, slot_timer_put_calls, timeout_calls;
static int timeout_delay, timeout_reload, timer_del_calls, wait_tx_calls;
static int send_cmd_calls, send_cmd_type;
static u32 send_cmd_instant;
static int send_cmd_uuid, send_cmd_priv;
static void (*slot_callback)(void *);
static void *slot_callback_priv;
static int slot_delay;
static int callback_count, callback_priv, callback_event, callback_irq_depth;
static int callback_head_empty, mutate_timer;
static u8 last_put_timer;
static int taskq_calls, taskq_type, taskq_argc;
static int taskq_args[4];
static int mock_sniff, mock_lmp_sniff, mock_cpu_irq, mock_irq_disabled;
static int key_enable_calls, key_disable_calls, time_dly_calls;

static void callback(int priv, int event)
{
    callback_count++;
    callback_priv = priv;
    callback_event = event;
    callback_irq_depth = irq_depth;
    callback_head_empty = sync_call_head.next == &sync_call_head;
    if (mutate_timer)
        sc_slot.timer = 255;
}

static void *sync_test_malloc(u32 size)
{
    malloc_calls++;
    if (fail_malloc || size != sizeof(struct tws_sync_call_data))
        return NULL;
    for (int i = 0; i < 32; i++) {
        if (!heap_used[i]) {
            heap_used[i] = 1;
            memset(heap[i], 0xa5, sizeof(heap[i]));
            return heap[i];
        }
    }
    return NULL;
}

static void sync_test_free(void *ptr)
{
    for (int i = 0; i < 32; i++) {
        if (ptr == heap[i]) {
            CHECK(heap_used[i]);
            heap_used[i] = 0;
            free_calls++;
            return;
        }
    }
    CHECK(0);
}

static void reset_fixture(void)
{
    memset(&tws_link, 0, sizeof(tws_link));
    memset(&tws_conn, 0, sizeof(tws_conn));
    memset(&mock_link, 0, sizeof(mock_link));
    memset(&mock_lmp, 0, sizeof(mock_lmp));
    sync_call_head.next = sync_call_head.prev = &sync_call_head;
    memset(&sc_slot, 0, sizeof(sc_slot));
    sc_slot.timer = 255;
    memset(heap_used, 0, sizeof(heap_used));
    malloc_calls = free_calls = fail_malloc = 0;
    irq_depth = irq_disable_calls = irq_enable_calls = 0;
    mock_clock = 100;
    mock_slot_timer = 7;
    slot_timer_set_calls = slot_timer_put_calls = timeout_calls = 0;
    mutate_timer = 0;
    last_put_timer = 0;
    timeout_delay = timeout_reload = timer_del_calls = wait_tx_calls = 0;
    send_cmd_calls = send_cmd_type = 0;
    send_cmd_instant = 0;
    send_cmd_uuid = send_cmd_priv = 0;
    slot_callback = NULL;
    slot_callback_priv = NULL;
    slot_delay = 0;
    callback_count = callback_priv = callback_event = 0;
    callback_irq_depth = callback_head_empty = 0;
    taskq_calls = taskq_type = taskq_argc = 0;
    memset(taskq_args, 0, sizeof(taskq_args));
    mock_sniff = mock_lmp_sniff = mock_cpu_irq = mock_irq_disabled = 0;
    key_enable_calls = key_disable_calls = time_dly_calls = 0;
}

u32 tws_link_read_slot_clk(void) { return mock_clock; }
int tws_conn_send_cmd(u8 type, const char *format, ...)
{
    va_list args;
    send_cmd_calls++;
    send_cmd_type = type;
    CHECK(strcmp(format, "441") == 0);
    va_start(args, format);
    send_cmd_instant = va_arg(args, u32);
    send_cmd_uuid = va_arg(args, int);
    send_cmd_priv = va_arg(args, int);
    va_end(args);
    return 0;
}
u8 slot_timer_get(void) { return mock_slot_timer; }
void slot_timer_set(u8 timer, void (*callback_fn)(void *), void *priv, int delay)
{
    slot_timer_set_calls++;
    CHECK(timer == mock_slot_timer || timer == 255);
    slot_callback = callback_fn;
    slot_callback_priv = priv;
    slot_delay = delay;
}
void slot_timer_put(u8 timer)
{ last_put_timer = timer; slot_timer_put_calls++; }
u16 usr_timeout_add(void *priv, void (*callback_fn)(void *), u32 msec, u8 reload)
{
    CHECK(priv != NULL && callback_fn == __call_func_timeout);
    timeout_calls++;
    timeout_delay = (int)msec;
    timeout_reload = reload;
    slot_callback = callback_fn;
    slot_callback_priv = priv;
    return 19;
}
void usr_timer_del(u16 timer) { CHECK(timer == 19); timer_del_calls++; }
int os_taskq_post_type(const char *task, int type, int argc, int *argv)
{
    CHECK(strcmp(task, (char *)registered_task) == 0);
    taskq_calls++;
    taskq_type = type;
    taskq_argc = argc;
    memcpy(taskq_args, argv, sizeof(taskq_args));
    return -1;
}
void tws_core_tws_wait_tx(void) { wait_tx_calls++; }
int bredr_offset2clkn(int clock, int offset) { return clock + offset; }
int bredr_clkn2offset(int clock, int instant) { return instant - clock; }
int tws_in_sniff_state(void) { return mock_sniff; }
void lmp_tx_unsniff_req(struct lmp_connection *conn) { (void)conn; }
void tws_tx_unsniff_req(void) { }
struct lmp_connection *lmp_conn_for_address(u8 *address)
{ CHECK(address == NULL); return &mock_lmp; }
int lmp_in_sniff_mode(struct lmp_connection *conn)
{ CHECK(conn == &mock_lmp); return mock_lmp_sniff; }
int lmp_hci_cmd_to_conn(void *req, int count, ...)
{ CHECK(req == &mock_lmp.rejust_esco_req && count == 1); return 0; }
void sys_key_event_filter_enable(void) { key_enable_calls++; }
void sys_key_event_filter_disable(void) { key_disable_calls++; }
void os_time_dly(int ticks) { CHECK(ticks == 10); time_dly_calls++; }
int cpu_in_irq(void) { return mock_cpu_irq; }
int cpu_irq_disabled(void) { return mock_irq_disabled; }
void local_irq_disable(void) { irq_depth++; irq_disable_calls++; }
void local_irq_enable(void) { irq_depth--; irq_enable_calls++; CHECK(irq_depth >= 0); }

static struct tws_sync_call_data *first_call(void)
{
    return (struct tws_sync_call_data *)sync_call_head.next;
}

int main(void)
{
    reset_fixture();
    CHECK(sc_slot.timer == 255 && sc_slot.uuid == 0 &&
          tws_sync_call.channel == 9 && tws_sync_call.attribute == 15);
    tws_sync_call_init();

    /* API schedules a linked-list timeout and emits the 3-word wire record. */
    tws_link.link = &mock_link;
    CHECK(tws_api_sync_call_by_uuid(0x1234, 42, 1000) == 0);
    CHECK(timeout_calls == 1 && timeout_delay == 1000 && timeout_reload == 1);
    CHECK(first_call()->uuid == 0x1234 && first_call()->priv == 42 &&
          first_call()->instant == 1700 && first_call()->timer == 19);
    u32 payload[3] = { 0 };
    CHECK(tws_sync_call_tx_events(NULL, (u8 *)payload, 0) == 12);
    CHECK(payload[0] == 0x1234 && payload[1] == 42 && payload[2] == 1700);
    CHECK(first_call()->tx == 1);

    /* Timeout uses a positive event; event 46 unlinks before negative event. */
    __call_func_timeout(first_call());
    CHECK(callback_count == 1 && callback_priv == 42 && callback_event == 1);
    CHECK(callback_irq_depth == 0 && !callback_head_empty);
    CHECK(free_calls == 1 && sync_call_head.next == &sync_call_head);
    CHECK(tws_api_sync_call_by_uuid(0x1234, 43, 1000) == 0);
    CHECK(tws_sync_call.event_handler(NULL, 46, NULL) == 0);
    CHECK(callback_count == 2 && callback_priv == 43 && callback_event == -2);
    CHECK(callback_irq_depth == 1 && callback_head_empty);
    CHECK(timer_del_calls == 1 && free_calls == 2);

    /* Task callbacks get [function, 2, priv, event]; post failure is ignored. */
    CHECK(tws_api_sync_call_by_uuid(0x2345, 44, 1000) == 0);
    __call_func_timeout(first_call());
    CHECK(taskq_calls == 1 && taskq_type == 0x300000 && taskq_argc == 4);
    CHECK(taskq_args[0] == (int)(uintptr_t)callback && taskq_args[1] == 2 &&
          taskq_args[2] == 44 && taskq_args[3] == 2);
    CHECK(tws_api_sync_call_by_uuid(0x2345, 45, 1000) == 0);
    CHECK(tws_sync_call.event_handler(NULL, 46, NULL) == 0);
    CHECK(taskq_calls == 2 && taskq_args[2] == 45 && taskq_args[3] == -2);
    CHECK(tws_api_sync_call_by_uuid(0x3456, 46, 1000) == 0);
    CHECK(tws_sync_call.event_handler(NULL, 46, NULL) == 0);
    CHECK(free_calls == 5 && sync_call_head.next == &sync_call_head);

    /* RX accepts >3 bytes, then walks padded triples using len/4. */
    reset_fixture();
    u32 padded[3] = { 0x1234, 50, 900 };
    tws_link.link = &mock_link;
    tws_sync_call_rx_events(NULL, (u8 *)padded, 4);
    CHECK(timeout_calls == 1 && first_call()->uuid == 0x1234 &&
          first_call()->priv == 50 && first_call()->instant == 900 &&
          first_call()->rx == 1);
    CHECK(tws_sync_call_tx_events(NULL, (u8 *)payload, 0) == 0);
    reset_fixture();
    tws_link.link = &mock_link;
    tws_sync_call_rx_events(NULL, (u8 *)padded, 8);
    CHECK(timeout_calls == 1 && first_call()->uuid == 0x1234 &&
          first_call()->instant == 900);

    /* By-link snapshots link and timer, and callback reloads timer afterwards. */
    reset_fixture();
    tws_link.link = &mock_link;
    CHECK(tws_api_sync_call_by_link(0x1234, 77, 1000) == 1);
    CHECK(irq_disable_calls == 1 && irq_enable_calls == 1 && irq_depth == 0);
    CHECK(send_cmd_calls == 1 && send_cmd_type == 10 &&
          send_cmd_instant == 1700 && send_cmd_uuid == 0x1234 &&
          send_cmd_priv == 77);
    CHECK(slot_timer_set_calls == 1 && slot_delay == 1600 &&
          slot_callback == tws_start_sync_callback &&
          slot_callback_priv == &mock_link);
    tws_start_sync_callback(&mock_link);
    CHECK(callback_count == 1 && callback_priv == 77 && callback_event == -1);
    CHECK(callback_irq_depth == 0);
    CHECK(slot_timer_put_calls == 1 && sc_slot.timer == 255);
    CHECK(last_put_timer == mock_slot_timer);
    CHECK(tws_api_sync_call_by_link(0x1234, 78, 1000) == 1);
    mutate_timer = 1;
    tws_start_sync_callback(&mock_link);
    CHECK(slot_timer_put_calls == 1 && sc_slot.timer == 255);
    CHECK(tws_api_sync_call_by_link(0x3456, 79, 1000) == 1);
    tws_start_sync_callback(&mock_link);
    CHECK(slot_timer_put_calls == 2 && sc_slot.timer == 255);

    reset_fixture();
    tws_link.link = &mock_link;
    mock_slot_timer = 255;
    CHECK(tws_api_sync_call_by_link(0x1234, 80, 1000) == 1);
    CHECK(slot_timer_set_calls == 1 && slot_callback == tws_start_sync_callback);
    tws_start_sync_callback(&mock_link);
    CHECK(slot_timer_put_calls == 0 && sc_slot.timer == 255);

    /* Link RX reads nine bytes and deliberately leaves sc_slot.instant intact. */
    reset_fixture();
    tws_link.link = &mock_link;
    sc_slot.instant = 0xdeadbeef;
    u8 link_data[9] = { 0x65, 0, 0, 0, 0x45, 0x23, 0, 0, 0x66 };
    tws_sync_call_rx_events_by_link(link_data);
    CHECK(sc_slot.uuid == 0x2345 && sc_slot.priv == 0x66 &&
          sc_slot.tx == 0 && sc_slot.rx == 0 &&
          (u32)sc_slot.instant == 0xdeadbeef);
    CHECK(slot_delay == 2 && slot_timer_set_calls == 1);

    /* The slot-delay multiply wraps as i32 before signed division in the IR. */
    reset_fixture();
    tws_link.link = &mock_link;
    u32 wide_record[3] = { 0x1234, 1, 100u + 3500000u };
    tws_sync_call_rx_events(NULL, (u8 *)wide_record, 12);
    CHECK(timeout_calls == 1 && timeout_delay ==
          (int)((int)((u32)3500000u * 625u) / 1000));
    reset_fixture();
    tws_link.link = &mock_link;
    u32 wrapped_deadline[3] = { 0x1234, 1, 100u + 69120001u };
    tws_sync_call_rx_events(NULL, (u8 *)wrapped_deadline, 12);
    CHECK(timeout_calls == 1 && timeout_delay == 2);

    /* Unsniff paths wait at least once and cap the retry loop at 15 ticks. */
    reset_fixture();
    tws_link.link = &mock_link;
    tws_link.plink = &mock_link;
    mock_sniff = 1;
    CHECK(tws_api_sync_call_by_uuid(0x1234, 2, 1) == 0);
    CHECK(time_dly_calls == 15 && key_enable_calls == 1 &&
          key_disable_calls == 1);
    reset_fixture();
    tws_link.link = &mock_link;
    mock_lmp_sniff = 1;
    CHECK(tws_api_sync_call_by_uuid(0x1234, 2, 1) == 0);
    CHECK(time_dly_calls == 15 && key_enable_calls == 1 &&
          key_disable_calls == 1);

    /* Reinitialize remains a no-op, and allocation failure returns -12. */
    reset_fixture();
    fail_malloc = 1;
    CHECK(tws_api_sync_call_by_uuid(0x1234, 1, 1) == -12);
    CHECK(wait_tx_calls == 0 && malloc_calls == 1);
    return TEST_DONE("btctrler_bd19_tws_sync_call");
}
#endif
