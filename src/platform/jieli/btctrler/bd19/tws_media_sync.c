/* BD19 SDK tws_media_sync.c.o occurrence 1. Preserve the two-slot queues,
 * callback ordering, hardware widths and no-op time-conversion entry points. */
#include "btctrler/glue.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define MEDIA_CODE __attribute__((section(".tws_media_sync_code")))
#define MSG_FLAGS(m) (*(u32 *)(void *)(m))
#define MEDIA_HANDLE(e) ((struct audio_tws_sync *)((u8 *)(e) - offsetof(struct audio_tws_sync, entry)))
#ifdef UNITTEST
static volatile u32 media_mmio;
#define MEDIA_MMIO media_mmio
#else
#define MEDIA_MMIO (*(volatile u32 *)0x1c0000)
#endif

extern BTCTRLER_CONFIG int CONFIG_BTCTLER_TWS_ENABLE;
struct list_head aud_sync_head = { &aud_sync_head, &aud_sync_head };
static bool audio_sync_nettime_base __attribute__((aligned(4)));
static volatile u8 tws_other_side_on_play;

static void tws_audio_sync_select_setup(u8 enable) MEDIA_CODE;
static int tws_media_sync_tx_msg(struct lmp_connection *, u8 *, int) MEDIA_CODE;
static int tws_media_sync_rx_msg_and_tx_msg(struct lmp_connection *, u8 *, int, u8 *) MEDIA_CODE;
static void tws_media_sync_rx_msg(struct lmp_connection *, u8 *, int) MEDIA_CODE;
static int tws_media_sync_event_handler(struct lmp_connection *, int, int *) MEDIA_CODE;
static void tws_media_sync_tx_msg_suss(void) MEDIA_CODE;
static void tws_media_other_offline(void) MEDIA_CODE;

const struct tws_sync_channel tws_media_sync
    __attribute__((section(".tws_sync_channel.7"))) = {
        .channel = 7, .attribute = 7,
        .tx_sync_req = tws_media_sync_tx_msg,
        .tx_sync_req_suss = (void (*)())tws_media_sync_tx_msg_suss,
        .rx_sync_req_and_res = tws_media_sync_rx_msg_and_tx_msg,
        .tx_sync_res_suss = (void (*)())tws_media_sync_tx_msg_suss,
        .rx_sync_res = tws_media_sync_rx_msg,
        .event_handler = tws_media_sync_event_handler,
};

static void list_add(struct list_head *entry)
{
    struct list_head *next = aud_sync_head.next;
    next->prev = entry;
    entry->next = next;
    entry->prev = &aud_sync_head;
    aud_sync_head.next = entry;
}

static void list_del(struct list_head *entry)
{
    struct list_head *prev = entry->prev, *next = entry->next;
    next->prev = prev;
    prev->next = next;
    entry->next = entry;
    entry->prev = entry;
}

static int list_empty(void) { return aud_sync_head.next == &aud_sync_head; }

void bt_audio_sync_nettime_select(u8 base) MEDIA_CODE;
void bt_audio_sync_nettime_select(u8 base)
{
    local_irq_disable();
    struct lmp_connection *active = lmp_conn_for_active_state();
    if (link_conn_num_more_than_one()) {
        struct lmp_connection *other = lmp_get_other_conn(active);
        if (other) {
            struct ctrl_frame *frame = other->link->frame;
            frame->miscntl_of_audsync = 0;
            frame->miscntl &= (u16)~2048u;
        }
    }
    if (base == 3) {
        if (!active || !active->link || (active->core.state & 8))
            base = 1;
        else
            base = 0;
    }
    if (base == 0) {
        if (active && active->link && !(active->core.state & 8)) {
            struct link_connection *phone = active->link;
            MEDIA_MMIO &= ~4096u;
            struct link_connection *peer = tws_link.link;
            if (peer) {
                struct ctrl_frame *frame = peer->frame;
                frame->miscntl_of_audsync = 0;
                frame->miscntl &= (u16)~6144u;
            }
            struct ctrl_frame *frame = phone->frame;
            frame->miscntl_of_audsync = 2048;
            frame->miscntl |= 2048;
        }
        audio_sync_nettime_base = false;
    } else if (base == 1 && CONFIG_BTCTLER_TWS_ENABLE) {
        audio_sync_nettime_base = true;
        tws_audio_sync_select_setup(1);
    }
    local_irq_enable();
}

static void tws_audio_sync_select_setup(u8 enable)
{
    if (!audio_sync_nettime_base) return;
    local_irq_disable();
    if (enable && tws_link.link) {
        struct lmp_connection *phone = lmp_conn_for_address(NULL);
        if (phone) {
            struct ctrl_frame *frame = phone->link->frame;
            frame->miscntl_of_audsync = 0;
            frame->miscntl &= (u16)~2048u;
        }
        struct link_connection *peer = tws_link.link;
        if (!peer) { local_irq_enable(); return; }
        u16 mask;
        if (peer->role == 0) {
            MEDIA_MMIO |= 4096;
            mask = 4096;
        } else mask = 6144;
        struct ctrl_frame *frame = peer->frame;
        frame->miscntl_of_audsync = mask;
        frame->miscntl |= mask;
    }
    local_irq_enable();
}

u32 bt_audio_sync_lat_time(void) MEDIA_CODE;
u32 bt_audio_sync_lat_time(void)
{
    local_irq_disable();
    struct lmp_connection *active = lmp_conn_for_active_state();
    bool base = audio_sync_nettime_base;
    u32 result = 0;
    if (active && !base) result = bredr_get_master_slot_clk(active->link);
    else if (base && tws_link.link) result = bredr_get_master_slot_clk(tws_link.link);
    local_irq_enable();
    return result;
}

u32 bt_audio_tws_lat_time(void) MEDIA_CODE;
u32 bt_audio_tws_lat_time(void) { return 0; }

__attribute__((weak)) u32 bt_tws_master_slot_clk(void) MEDIA_CODE;
u32 bt_tws_master_slot_clk(void)
{
    local_irq_disable();
    struct link_connection *peer = tws_link.link;
    u32 result = peer ? bredr_get_master_slot_clk(peer) : 0;
    local_irq_enable();
    return result;
}

void get_remote_clk_bitoff(s32 *clock, u32 *bitoff) MEDIA_CODE;
void get_remote_clk_bitoff(s32 *clock, u32 *bitoff)
{
    if (CONFIG_BTCTLER_TWS_ENABLE && tws_link.link) {
        *clock = bredr_link_clk_offset(tws_link.link);
        *bitoff = (u32)lc_local_slot_bitoff(tws_link.link) % 625u;
        if (*bitoff < 4u) *clock = bredr_link_clk_offset(tws_link.link);
    }
}

u32 get_bt_slot_time(u8 type, u32 time, int *out, int (*local_time)(void)) MEDIA_CODE;
u32 get_bt_slot_time(u8 type, u32 time, int *out, int (*local_time)(void))
{ (void)type; (void)time; (void)out; (void)local_time; return 0; }

int bt_send_audio_sync_data(void *handle, void *data, u32 len) MEDIA_CODE;
int bt_send_audio_sync_data(void *handle, void *data, u32 len)
{
    if (!CONFIG_BTCTLER_TWS_ENABLE || !handle || !tws_link.link) return 0;
    struct audio_tws_sync *sync = handle;
    local_irq_disable();
    u8 index = sync->load_index;
    struct audio_sync_msg *msg = &sync->msg_pool[index];
    u32 flags = MSG_FLAGS(msg);
    if (flags & 1) { local_irq_enable(); return 0; }
    u32 next = (flags & ~32640u) | (((u32)len << 7) & 32640u);
    MSG_FLAGS(msg) = next;
    __builtin_memcpy(msg->msg, data, len);
    MSG_FLAGS(msg) = (next & ~121u) | 9u;
    u8 increment = index + 1;
    sync->load_index = increment > 1 ? 0 : increment;
    local_irq_enable();
    return len;
}

void bt_media_sync_set_handler(void *handle, void *priv, void (*handler)(void *, int *, int)) MEDIA_CODE;
void bt_media_sync_set_handler(void *handle, void *priv, void (*handler)(void *, int *, int))
{
    struct audio_tws_sync *sync = handle;
    sync->priv = priv;
    sync->event_handler = handler;
}

void *bt_media_sync_open(void) MEDIA_CODE;
void *bt_media_sync_open(void)
{
    struct audio_tws_sync *sync = zalloc(sizeof(*sync));
    if (sync && CONFIG_BTCTLER_TWS_ENABLE) {
        local_irq_disable();
        list_add(&sync->entry);
        *(u8 *)sync->msg_pool[0].msg = 1;
        MSG_FLAGS(&sync->msg_pool[0]) = (MSG_FLAGS(&sync->msg_pool[0]) & ~32761u) | 129u;
        sync->load_index++;
        local_irq_enable();
    }
    return sync;
}

int bt_media_sync_master(u8 type) MEDIA_CODE;
int bt_media_sync_master(u8 type)
{
    if (!CONFIG_BTCTLER_TWS_ENABLE || !tws_link.link) return 1;
    if (type == 1) return tws_link.link->role ^ 1;
    return tws_api_get_role() == 0;
}

u8 bt_media_device_online(u8 type) MEDIA_CODE;
u8 bt_media_device_online(u8 type)
{
    if (!CONFIG_BTCTLER_TWS_ENABLE) return 0;
    if (type == 0) return tws_link.link != NULL;
    if (type == 1) return tws_other_side_on_play;
    return 0;
}

void bt_media_sync_close(void *handle) MEDIA_CODE;
void bt_media_sync_close(void *handle)
{
    struct audio_tws_sync *sync = handle;
    if (CONFIG_BTCTLER_TWS_ENABLE) {
        local_irq_disable();
        if (sync) {
            sync->priv = NULL;
            sync->event_handler = NULL;
            list_del(&sync->entry);
        }
        local_irq_enable();
    }
    if (sync) free(sync);
}

void sbc_sync_packet_len(int len) MEDIA_CODE;
void sbc_sync_packet_len(int len) { (void)len; }

void tws_media_clear_msg_pool(void) MEDIA_CODE;
void tws_media_clear_msg_pool(void)
{
    local_irq_disable();
    for (struct list_head *e = aud_sync_head.next; e != &aud_sync_head; e = e->next) {
        struct audio_tws_sync *sync = MEDIA_HANDLE(e);
        if (sync->event_handler) {
            sync->tx_index = 0;
            sync->load_index = 0;
            __builtin_memset(sync->msg_pool, 0, 104);
        }
    }
    local_irq_enable();
}

static int tws_media_sync_tx_msg(struct lmp_connection *conn, u8 *out, int err)
{
    (void)conn; (void)err;
    int result = 0;
    for (struct list_head *e = aud_sync_head.next; e != &aud_sync_head; e = e->next) {
        struct audio_tws_sync *sync = MEDIA_HANDLE(e);
        struct audio_sync_msg *msg = &sync->msg_pool[sync->tx_index];
        u32 flags = MSG_FLAGS(msg);
        if ((flags & 3) == 1) {
            out[0] = (flags >> 3) & 15;
            out[1] = sync->msg_pool[sync->tx_index].role;
            out[2] = 0; out[3] = 0;
            u8 index = sync->tx_index;
            __builtin_memcpy(out + 4, sync->msg_pool[index].msg, sync->msg_pool[index].byte_len);
            msg = &sync->msg_pool[sync->tx_index];
            flags = MSG_FLAGS(msg);
            MSG_FLAGS(msg) = flags | 2;
            result = ((flags >> 7) & 255) + 4;
        }
    }
    if (list_empty()) {
        out[0] = 2; out[1] = 0; out[2] = 0; out[3] = 0;
        return 4;
    }
    return result;
}

static int tws_media_sync_rx_msg_and_tx_msg(struct lmp_connection *conn, u8 *rx, int len, u8 *tx)
{
    if (len) tws_media_sync_rx_msg(conn, rx, len);
    return tx ? tws_media_sync_tx_msg(conn, tx, 0) : 0;
}

static void tws_media_sync_rx_msg(struct lmp_connection *conn, u8 *data, int len)
{
    (void)conn;
    if (data[0] == 0) tws_other_side_on_play = data[4];
    else if (data[0] == 1) {
        int args[3];
        for (struct list_head *e = aud_sync_head.next; e != &aud_sync_head; e = e->next) {
            struct audio_tws_sync *sync = MEDIA_HANDLE(e);
            void (*handler)(void *, int *, int) = sync->event_handler;
            if (handler) {
                args[0] = 3;
                args[1] = (int)(uintptr_t)(data + 4);
                args[2] = len - 4;
                handler(sync->priv, args, 12);
            }
        }
    } else if (data[0] == 2) tws_media_other_offline();
}

static int tws_media_sync_event_handler(struct lmp_connection *conn, int event, int *arg)
{
    (void)conn; (void)arg;
    if (event == 22) tws_audio_sync_select_setup(1);
    else if (event == 15) {
        int args[2];
        for (struct list_head *e = aud_sync_head.next; e != &aud_sync_head; e = e->next) {
            struct audio_tws_sync *sync = MEDIA_HANDLE(e);
            if (sync->event_handler) {
                local_irq_disable();
                u8 index = sync->load_index;
                struct audio_sync_msg *msg = &sync->msg_pool[index];
                u32 flags = MSG_FLAGS(msg);
                if (!(flags & 1)) {
                    *(u8 *)msg->msg = 1;
                    MSG_FLAGS(msg) = (flags & ~32761u) | 129u;
                    u8 increment = index + 1;
                    sync->load_index = increment > 1 ? 0 : increment;
                }
                local_irq_enable();
                args[0] = 6;
                sync->event_handler(sync->priv, args, 8);
            }
        }
    } else if (event == 45 || event == 46) {
        tws_audio_sync_select_setup(0);
        tws_media_other_offline();
        tws_media_clear_msg_pool();
    }
    return 0;
}

static void tws_media_sync_tx_msg_suss(void)
{
    for (struct list_head *e = aud_sync_head.next; e != &aud_sync_head; e = e->next) {
        struct audio_tws_sync *sync = MEDIA_HANDLE(e);
        u8 index = sync->tx_index;
        struct audio_sync_msg *msg = &sync->msg_pool[index];
        u32 flags = MSG_FLAGS(msg);
        if (flags & 2) {
            MSG_FLAGS(msg) = flags & ~3u;
            u8 increment = index + 1;
            sync->tx_index = increment > 1 ? 0 : increment;
        }
    }
}

static void tws_media_other_offline(void)
{
    int args[2];
    if (tws_other_side_on_play) tws_other_side_on_play = 0;
    for (struct list_head *e = aud_sync_head.next; e != &aud_sync_head; e = e->next) {
        struct audio_tws_sync *sync = MEDIA_HANDLE(e);
        if (sync->event_handler) {
            args[0] = tws_api_get_role_internal() == 0 ? 5 : 4;
            sync->event_handler(sync->priv, args, 8);
        }
    }
}

#ifdef UNITTEST
#include "test.h"
int CONFIG_BTCTLER_TWS_ENABLE;
struct tws_link_connection tws_link;
static struct link_connection phone_link, peer_link, peer2_link, peer3_link, other_link;
static struct lmp_connection active_conn, fixture_other_conn;
static struct ctrl_frame phone_frame, peer_frame, other_frame;
static struct lmp_connection *mock_active, *mock_phone, *mock_other;
static int multiple, fail_alloc, irq_depth, disables, enables, max_depth;
static int role, internal_role, callbacks, alternate_callbacks, callback_event, callback_len;
static int callback_args[3], callback_irq, callback_identity;
static struct audio_tws_sync *mutate_handler, *skip_next;
static int change_on_enable, change_on_role, remove_peer_on_lookup, mutate_remote;
static int offset_calls, fine_calls, clock_calls;
static u32 fine_time;
static struct link_connection *offset_links[2], *fine_link, *clock_link;

static void alternate_handler(void *priv, int *args, int len);
static void handler(void *priv, int *args, int len)
{
    ++callbacks;
    callback_identity = (int)(uintptr_t)priv;
    callback_event = args[0]; callback_len = len; callback_irq = irq_depth;
    callback_args[0] = args[0];
    if (len == 12) { callback_args[1] = args[1]; callback_args[2] = args[2]; }
    /* SDK leaves word1 undefined for 8-byte notifications: do not inspect it. */
    if (skip_next) {
        skip_next->entry.next = &aud_sync_head;
        aud_sync_head.prev = &skip_next->entry;
    }
}
static void alternate_handler(void *priv, int *args, int len)
{ ++alternate_callbacks; handler(priv, args, len); }
void local_irq_disable(void)
{ ++disables; ++irq_depth; if (irq_depth > max_depth) max_depth = irq_depth; }
void local_irq_enable(void)
{
    ++enables; --irq_depth; CHECK(irq_depth >= 0);
    if (change_on_enable && mutate_handler) {
        mutate_handler->event_handler = alternate_handler;
        mutate_handler->priv = (void *)(uintptr_t)77;
    }
}
void *zalloc(u32 size)
{ CHECK(size == sizeof(struct audio_tws_sync)); return fail_alloc ? NULL : calloc(1, size); }
/* Free is libc; all handles are closed exactly once in these fixtures. */
struct lmp_connection *lmp_conn_for_active_state(void) { return mock_active; }
int link_conn_num_more_than_one(void) { return multiple; }
struct lmp_connection *lmp_get_other_conn(struct lmp_connection *active)
{ CHECK(active == mock_active); return mock_other; }
struct lmp_connection *lmp_conn_for_address(u8 *address)
{ CHECK(address == NULL); if (remove_peer_on_lookup) tws_link.link = NULL; return mock_phone; }
u32 bredr_get_master_slot_clk(struct link_connection *link)
{ ++clock_calls; clock_link = link; return 1234; }
int bredr_link_clk_offset(struct link_connection *link)
{
    CHECK(offset_calls < 2); offset_links[offset_calls++] = link;
    if (mutate_remote && offset_calls == 1) tws_link.link = &peer2_link;
    return 100 + offset_calls;
}
u32 lc_local_slot_bitoff(struct link_connection *link)
{ ++fine_calls; fine_link = link; if (mutate_remote) tws_link.link = &peer3_link; return fine_time; }
int tws_api_get_role(void) { return role; }
int tws_api_get_role_internal(void)
{
    if (change_on_role && mutate_handler) {
        mutate_handler->event_handler = alternate_handler;
        mutate_handler->priv = (void *)(uintptr_t)88;
    }
    return internal_role;
}

static void reset_fixture(void)
{
    CHECK(aud_sync_head.next == &aud_sync_head);
    CONFIG_BTCTLER_TWS_ENABLE = 1;
    memset(&tws_link, 0, sizeof(tws_link));
    memset(&phone_link, 0, sizeof(phone_link));
    memset(&peer_link, 0, sizeof(peer_link));
    memset(&other_link, 0, sizeof(other_link));
    memset(&active_conn, 0, sizeof(active_conn));
    memset(&fixture_other_conn, 0, sizeof(fixture_other_conn));
    memset(&phone_frame, 0, sizeof(phone_frame));
    memset(&peer_frame, 0, sizeof(peer_frame));
    memset(&other_frame, 0, sizeof(other_frame));
    phone_link.frame = &phone_frame; peer_link.frame = &peer_frame; other_link.frame = &other_frame;
    active_conn.link = &phone_link; fixture_other_conn.link = &other_link;
    mock_active = mock_phone = mock_other = NULL;
    audio_sync_nettime_base = false; tws_other_side_on_play = 0; media_mmio = 0;
    multiple = fail_alloc = irq_depth = disables = enables = max_depth = 0;
    callbacks = alternate_callbacks = callback_event = callback_len = callback_irq = callback_identity = 0;
    role = internal_role = change_on_enable = change_on_role = remove_peer_on_lookup = mutate_remote = 0;
    offset_calls = fine_calls = clock_calls = 0; fine_time = 0;
    mutate_handler = skip_next = NULL;
    memset(offset_links, 0, sizeof(offset_links)); fine_link = clock_link = NULL;
}

int main(void)
{
    reset_fixture();
    CHECK(tws_media_sync.channel == 7 && tws_media_sync.attribute == 7);
    u32 out_time = 0xabc;
    CHECK(get_bt_slot_time(2, 123, (int *)&out_time, NULL) == 0 && out_time == 0xabc);
    CHECK(bt_audio_tws_lat_time() == 0); sbc_sync_packet_len(1024);
    fail_alloc = 1; CHECK(bt_media_sync_open() == NULL && disables == 0);
    fail_alloc = 0; CONFIG_BTCTLER_TWS_ENABLE = 0;
    struct audio_tws_sync *a = bt_media_sync_open();
    CHECK(a && list_empty() && MSG_FLAGS(&a->msg_pool[0]) == 0 && disables == 0);
    bt_media_sync_close(a); CHECK(disables == 0);
    CONFIG_BTCTLER_TWS_ENABLE = 1;
    bt_media_sync_close(NULL); CHECK(disables == 1 && enables == 1);

    /* Open queues a one-byte PLAY message; only two slots, no length guard. */
    reset_fixture(); tws_link.link = &peer_link;
    a = bt_media_sync_open();
    CHECK(a && aud_sync_head.next == &a->entry && a->load_index == 1);
    CHECK(MSG_FLAGS(&a->msg_pool[0]) == 129 && *(u8 *)a->msg_pool[0].msg == 1);
    u8 wire[64] = { 0 }, payload[48];
    for (int n = 0; n < 48; ++n) payload[n] = n + 10;
    CHECK(bt_send_audio_sync_data(a, payload, 48) == 48 && a->load_index == 0);
    CHECK(a->msg_pool[1].byte_len == 48 && a->msg_pool[1].type == 1);
    CHECK(bt_send_audio_sync_data(a, payload, 1) == 0);
    CHECK(tws_media_sync_tx_msg(NULL, wire, 0) == 5 && wire[0] == 0 && wire[4] == 1);
    CHECK(a->msg_pool[0].tx == 1 && tws_media_sync_tx_msg(NULL, wire, 0) == 0);
    tws_media_sync_tx_msg_suss(); CHECK(a->tx_index == 1 && !a->msg_pool[0].available);
    CHECK(tws_media_sync_tx_msg(NULL, wire, 0) == 52 && wire[0] == 1 && !memcmp(wire + 4, payload, 48));
    tws_media_sync_tx_msg_suss(); CHECK(a->tx_index == 0);
    bt_media_sync_close(a); CHECK(list_empty());
    CHECK(tws_media_sync_tx_msg(NULL, wire, 0) == 4 && wire[0] == 2 && wire[1] == 0);

    /* Every pending handle overwrites the same output, yet all are marked TX. */
    reset_fixture(); tws_link.link = &peer_link;
    a = bt_media_sync_open(); struct audio_tws_sync *b = bt_media_sync_open();
    *(u8 *)a->msg_pool[0].msg = 0xa1; *(u8 *)b->msg_pool[0].msg = 0xb2;
    CHECK(tws_media_sync_tx_msg(NULL, wire, 0) == 5 && wire[4] == 0xa1);
    CHECK(a->msg_pool[0].tx && b->msg_pool[0].tx);
    tws_media_sync_tx_msg_suss(); CHECK(a->tx_index == 1 && b->tx_index == 1);
    bt_media_sync_set_handler(a, (void *)(uintptr_t)1, handler);
    a->load_index = b->load_index = 1;
    MSG_FLAGS(&a->msg_pool[0]) = MSG_FLAGS(&b->msg_pool[0]) = 129;
    tws_media_clear_msg_pool();
    CHECK(a->load_index == 0 && MSG_FLAGS(&a->msg_pool[0]) == 0);
    CHECK(b->load_index == 1 && MSG_FLAGS(&b->msg_pool[0]) == 129);
    bt_media_sync_close(a); bt_media_sync_close(b);

    /* TX reloads index after output stores, including caller aliasing. */
    reset_fixture(); tws_link.link = &peer_link; a = bt_media_sync_open();
    MSG_FLAGS(&a->msg_pool[0]) = 129 | 4;
    MSG_FLAGS(&a->msg_pool[1]) = (3u << 7) | 9;
    memcpy(a->msg_pool[1].msg, "abc", 3);
    CHECK(tws_media_sync_tx_msg(NULL, (u8 *)a, 0) == 7);
    CHECK(a->tx_index == 1 && a->msg_pool[1].tx && !memcmp(a->msg_pool, "abc", 3));
    bt_media_sync_close(a);

    /* RX framing is unchecked, including negative payload length. */
    reset_fixture(); a = bt_media_sync_open();
    bt_media_sync_set_handler(a, (void *)(uintptr_t)3, handler);
    u8 rx[8] = { 0, 0, 0, 0, 7 };
    tws_media_sync_rx_msg(NULL, rx, 1); CHECK(bt_media_device_online(1) == 7);
    rx[0] = 1; tws_media_sync_rx_msg(NULL, rx, 2);
    CHECK(callback_event == 3 && callback_len == 12 && callback_args[2] == -2);
    CHECK(callback_args[1] == (int)(uintptr_t)(rx + 4));
    /* Unlocked event delivery reloads handler/priv after IRQ enable. */
    mutate_handler = a; change_on_enable = 1;
    CHECK(tws_media_sync_event_handler(NULL, 15, NULL) == 0);
    CHECK(alternate_callbacks == 1 && callback_event == 6 && callback_identity == 77 && callback_irq == 0);
    change_on_enable = 0; change_on_role = 1; internal_role = 0;
    a->event_handler = handler; a->priv = NULL;
    tws_media_other_offline();
    CHECK(alternate_callbacks == 2 && callback_event == 5 && callback_identity == 88 && !tws_other_side_on_play);
    internal_role = 1; tws_media_other_offline(); CHECK(callback_event == 4);
    change_on_role = 0;
    audio_sync_nettime_base = true;
    int before = disables;
    CHECK(tws_media_sync_event_handler(NULL, 46, NULL) == 0);
    CHECK(disables == before + 2 && MSG_FLAGS(&a->msg_pool[0]) == 0);
    CHECK(tws_media_sync_rx_msg_and_tx_msg(NULL, rx, 0, NULL) == 0);
    bt_media_sync_close(a);

    /* List advancement is loaded after callbacks, not snapshotted before. */
    reset_fixture(); a = bt_media_sync_open(); b = bt_media_sync_open();
    bt_media_sync_set_handler(a, NULL, handler); bt_media_sync_set_handler(b, NULL, handler);
    skip_next = b; rx[0] = 1;
    tws_media_sync_rx_msg(NULL, rx, 8);
    CHECK(callbacks == 1);
    skip_next = NULL;
    bt_media_sync_close(a); bt_media_sync_close(b);

    /* Clock routing and MMIO width/masks are taken directly from SDK IR. */
    reset_fixture(); mock_active = &active_conn; tws_link.link = &peer_link;
    mock_phone = &active_conn; mock_other = &fixture_other_conn; multiple = 1;
    media_mmio = 0xffffffffu;
    phone_frame.miscntl = peer_frame.miscntl = other_frame.miscntl = 0xffff;
    bt_audio_sync_nettime_select(0);
    CHECK(!audio_sync_nettime_base && media_mmio == 0xffffefffu);
    CHECK(phone_frame.miscntl_of_audsync == 2048 && phone_frame.miscntl == 0xffff);
    CHECK(peer_frame.miscntl_of_audsync == 0 && peer_frame.miscntl == 0xe7ff);
    CHECK(other_frame.miscntl == 0xf7ff && other_frame.miscntl_of_audsync == 0);
    CHECK(bt_audio_sync_lat_time() == 1234 && clock_link == &phone_link);
    bt_audio_sync_nettime_select(1);
    CHECK(audio_sync_nettime_base && peer_frame.miscntl_of_audsync == 4096 && media_mmio == 0xffffffffu);
    CHECK(max_depth == 2 && irq_depth == 0);
    CHECK(bt_audio_sync_lat_time() == 1234 && clock_link == &peer_link);
    peer_link.role = 1; media_mmio = 0;
    bt_audio_sync_nettime_select(1);
    CHECK(peer_frame.miscntl_of_audsync == 6144 && media_mmio == 0);
    CHECK(bt_media_sync_master(1) == 0);
    role = 0; CHECK(bt_media_sync_master(0) == 1); role = 1; CHECK(bt_media_sync_master(0) == 0);
    CHECK(bt_tws_master_slot_clk() == 1234 && clock_link == &peer_link);
    active_conn.core.state = 8; bt_audio_sync_nettime_select(3); CHECK(audio_sync_nettime_base);
    active_conn.core.state = 0; bt_audio_sync_nettime_select(3); CHECK(!audio_sync_nettime_base);
    bt_audio_sync_nettime_select(2); CHECK(!audio_sync_nettime_base);
    mock_active = NULL; bt_audio_sync_nettime_select(3); CHECK(audio_sync_nettime_base);
    remove_peer_on_lookup = 1; tws_link.link = &peer_link;
    bt_audio_sync_nettime_select(1); CHECK(tws_link.link == NULL && irq_depth == 0);
    CHECK(bt_tws_master_slot_clk() == 0 && bt_audio_sync_lat_time() == 0);

    /* Remainder is unsigned and links are reloaded between clock calls. */
    reset_fixture(); tws_link.link = &peer_link; mutate_remote = 1;
    fine_time = 628;
    s32 offset = -1; u32 fine = 99;
    get_remote_clk_bitoff(&offset, &fine);
    CHECK(offset == 102 && fine == 3 && offset_calls == 2 && fine_calls == 1);
    CHECK(offset_links[0] == &peer_link && fine_link == &peer2_link && offset_links[1] == &peer3_link);
    reset_fixture(); tws_link.link = &peer_link; fine_time = 0xffffffffu;
    get_remote_clk_bitoff(&offset, &fine);
    CHECK(fine == 0xffffffffu % 625u && offset_calls == 1);
    CONFIG_BTCTLER_TWS_ENABLE = 0; offset = -7; fine = 9;
    get_remote_clk_bitoff(&offset, &fine); CHECK(offset == -7 && fine == 9);
    CHECK(bt_media_sync_master(1) == 1 && bt_media_device_online(0) == 0);
    CHECK(bt_send_audio_sync_data(NULL, NULL, 0) == 0);
    CHECK(irq_depth == 0 && list_empty());
    return TEST_DONE("btctrler_bd19_tws_media_sync");
}
#endif
