/* BTstack on the owned Jieli BR23 controller: the HCI
 * transport, the run loop, and the few controller calls the host needs.
 * BTstack headers only; the controller's entry points are declared here. */
#include <string.h>
#include "btstack_config.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_base.h"
#include "hci.h"
#include "hci_transport.h"
#include "pad.h"
#include "btstack_qos_jieli.h"
#include "hci_queue.h"
#include "uECC.h"

extern int btctrler_task_init(const void *transport, const void *config);
extern int btctrler_task_ready(void);
extern int btctrler_task_exit(void);
extern const void *hci_transport_h4_controller_instance(void);
extern void (*__h4_send_packet)(int type, const void *packet, int size);
extern void h4_hci_push_data(int type, void *packet, int size);
extern int le_controller_set_mac(const void *address);
extern void lmp_hci_write_local_address(const u8 *address);
extern void get_random_number(u8 *p, u8 len);
extern int btctrlermod_host_acl_room(void);
extern void btctrlermod_host_ready(void);

static volatile u8 rx_active;
static volatile u8 drop_logged; /* this session's first drop is in the log */
static volatile u8 held;        /* an ACL packet was kept back: wake the controller when there is room */
static void (*host_handler)(u8 type, u8 *packet, u16 size);
static btstack_data_source_t rx_source;
static u8 address[6];            /* controller order: least significant byte first */
static u8 controller_up;
static volatile u8 exit_requested;

/* ---- LE input past BTstack -------------------------------------------------
 * bt_pad.c hands an LE link's input notifications to the controller from the
 * control thread (hci_jieli_fast_send), past BTstack and the bluetooth
 * thread, whose two passes a report cost about a millisecond.  The
 * controller's Number Of Completed Packets does not say whose packets went:
 * in the order they were handed over, a queue of who sent each outstanding
 * one (a bit each, set: ours) splits the count.  BTstack hears only of its
 * own; ours go to the done callback, in the controller's context.  Handed
 * over by two threads at once the order can be off by one; the counts still
 * add up. */
static volatile u16 fast_handle = 0xffff;
static void (*fast_done)(u32 n);
static u32 fast_bits;            /* outstanding, oldest in bit 0 */
static u8 fast_count;
static volatile u8 fast_busy;    /* the control thread is in the controller */

static void fast_queue(u32 ours)
{
    plat_lock();
    if (fast_count < 32) fast_bits |= ours << fast_count++;
    plat_unlock();
}

/* Of n completed: how many were ours. */
static u32 fast_split(u32 n)
{
    u32 ours = 0;

    plat_lock();
    while (n-- && fast_count) {
        ours += fast_bits & 1;
        fast_bits >>= 1;
        fast_count--;
    }
    plat_unlock();
    return ours;
}

void hci_jieli_fast_open(u16 handle, void (*done)(u32 n))
{
    plat_lock();
    fast_bits = fast_count = 0;
    fast_done = done;
    fast_handle = handle;
    plat_unlock();
}

/* No more fast sends; one under way finishes first (it holds the controller's
 * link lock, below this thread). */
void hci_jieli_fast_close(void)
{
    fast_handle = 0xffff;
    while (fast_busy) plat_delay_ms(1);
    plat_lock();
    fast_bits = fast_count = 0;
    plat_unlock();
}

/* Outstanding on the fast link, BTstack's packets included. */
u32 hci_jieli_fast_in_flight(void)
{
    return fast_count;
}

/* An L2CAP PDU (its header first) as one ACL packet; 0 when the link is gone. */
int hci_jieli_fast_send(const u8 *pdu, u16 len)
{
    static u8 acl[4 + HCI_ACL_PAYLOAD_SIZE];
    u16 h = fast_handle;

    if (h == 0xffff || len > HCI_ACL_PAYLOAD_SIZE) return 0;
    fast_busy = 1;
    if (fast_handle != h) {      /* closed meanwhile */
        fast_busy = 0;
        return 0;
    }
    little_endian_store_16(acl, 0, (u16)(h | 0x2000));   /* first packet, not flushable */
    little_endian_store_16(acl, 2, len);
    memcpy(acl + 4, pdu, len);
    fast_queue(1);
    if (__h4_send_packet) __h4_send_packet(HCI_ACL_DATA_PACKET, acl, len + 4);
    else h4_hci_push_data(HCI_ACL_DATA_PACKET, acl, len + 4);
    fast_busy = 0;
    return 1;
}

/* Number Of Completed Packets: ours out of it; 1 when nothing is left for
 * BTstack. */
static int fast_completed(u8 *e, u16 size)
{
    u32 i, n, ours, left = 0;

    if (fast_handle == 0xffff || size < 3) return 0;
    for (i = 0; i < e[2] && 3 + 4 * i + 4 <= size; i++) {
        n = little_endian_read_16(e, 3 + 4 * i + 2);
        if ((little_endian_read_16(e, 3 + 4 * i) & 0x0fff) == fast_handle) {
            ours = fast_split(n);
            n -= ours;
            little_endian_store_16(e, 3 + 4 * i + 2, (u16)n);
            if (ours && fast_done) fast_done(ours);
        }
        left += n;
    }
    return left == 0;
}

/* ---- controller --------------------------------------------------------- */

/* A full queue: the first drop of a session goes in the log (in the caller's
 * context, the controller's task included), the rest only in the counts. */
static void dropped(u8 type, const u8 *packet, u16 size)
{
    u8 e[5] = { 0 };

    if (drop_logged) return;
    drop_logged = 1;
    memcpy(e, packet, size < sizeof(e) ? size : sizeof(e));
    if (type == HCI_EVENT_PACKET)   /* code, then the bytes of the opcode or subevent */
        util_log("hci: queue full, dropped event %02x %02x %02x %02x", e[0], e[2], e[3], e[4]);
    else
        util_log("hci: queue full, dropped type %u len %u", type, size);
}

/* ---- host flow control (BR23: our controller) ----------------------------
 * Received ACL data takes a slot only while EVENT_ROOM more stay free, so an
 * event always finds one.  Refused, the controller keeps the packet (LE: the
 * radio stops taking more once its buffers run out, so the peer resends) and
 * rx_process() wakes it once the queue is drained.  Every refusal leaves the
 * queue nearly full, so rx_process() runs after it.  The BD19 holds back LE
 * data only: its Classic ACL still drops when the queue is full. */
#ifndef EVENT_ROOM                     /* a test build may raise it to hold often */
#define EVENT_ROOM 2
#endif

int btctrlermod_host_acl_room(void)
{
    if (!rx_active || hciq_free() > EVENT_ROOM) return 1;   /* closed: handed over and dropped */
    held = 1;
    hciq_held();
    return 0;
}

/* The receive queue's drops and high-water mark since boot. */
void hci_jieli_rx_stats(struct hci_queue_stats *out)
{
    hciq_stats(out);
}

/* Called by the controller for every event and ACL packet; the packet is only
 * valid during the call. */
int hci_packet_handler(u8 type, u8 *packet, u16 size)
{
    u8 nocp[3 + 4 * 4], *copy;

#ifndef CONFIG_CPU_BD19
    if (type == HCI_EVENT_PACKET) btctrlermod_qos_link_event(packet, size);
#endif

    /* HCI Reset puts the factory address back: reapply ours before BTstack
     * reads it. */
    if (type == HCI_EVENT_PACKET && size >= 6 && packet[0] == 0x0e && packet[3] == 0x03 &&
        packet[4] == 0x0c && packet[5] == 0)
        lmp_hci_write_local_address(address);
    if (!rx_active) return 0;
    if (type == HCI_EVENT_PACKET && packet[0] == HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS &&
        size <= sizeof(nocp)) {
        memcpy(nocp, packet, size);
        if (fast_completed(nocp, size)) return 0;
        packet = nocp;
    }
    if (type == HCI_EVENT_PACKET && packet[0] == HCI_EVENT_DISCONNECTION_COMPLETE && size >= 5 &&
        (little_endian_read_16(packet, 3) & 0x0fff) == fast_handle) {
        plat_lock();
        fast_bits = fast_count = 0;  /* the controller dropped what was outstanding */
        plat_unlock();
    }
#ifndef CONFIG_CPU_BD19
    /* LE data asked first (RF_ble.c), so a refusal here is Classic's, which
     * retries (lmp_acl.c). */
    if (type == HCI_ACL_DATA_PACKET && !btctrlermod_host_acl_room()) return 1;
#endif
    copy = hciq_put(type, packet, size);
    if (!copy) {
        dropped(type, packet, size);
        return 0;
    }
    /* Supported Commands: the P-256 commands, which p256_command() answers
     * (octet 34, bits 1 and 2); BTstack asserts on them. */
    if (type == HCI_EVENT_PACKET && size >= 6 + 35 && packet[0] == 0x0e && packet[3] == 0x02 &&
        packet[4] == 0x10)
        copy[6 + 34] |= 0x06;
    btstack_run_loop_poll_data_sources_from_irq();
    return 0;
}

/* An event of our own for BTstack: queued with interrupts off, since the
 * controller's task, above every caller of this, queues its own. */
static void inject(u8 *event, u16 size)
{
    int ok;

    if (!rx_active) return;
    plat_lock();
    ok = hciq_put(HCI_EVENT_PACKET, event, size) != 0;
    plat_unlock();
    if (!ok) dropped(HCI_EVENT_PACKET, event, size);
    btstack_run_loop_poll_data_sources_from_irq();
}

/* ---- P-256: the controller's commands are stubs that never answer -------
 * LE Read Local P-256 Public Key and LE Generate DHKey are answered here by
 * micro-ecc (the SDK library's) on a thread below the main loop: a key takes
 * about 0.75 s, which on the bluetooth thread held BTstack and, above it, the
 * main loop.  The thread keeps the next key pair ready, so BTstack, which asks
 * for a fresh one at every start and after every pairing, gets it at once;
 * each pair is handed out once.  HCI carries each coordinate least
 * significant byte first, micro-ecc most significant first.  A DHKey result
 * for a session that has since ended is dropped. */
static struct {
    u8 public[64], private[32];  /* the next pair, while ready */
    volatile u8 ready;           /* set by the thread, cleared by the taker */
    volatile u8 key_wanted;      /* BTstack asked while none was ready */
    volatile u8 dhkey_wanted;
    volatile u32 dhkey_session;  /* the session the DHKey request came in */
    u8 peer[64];                 /* DHKey: the peer's key */
    u8 private_out[32];          /* the private half of the key BTstack has */
    u8 have_out;
} p256;
static volatile u32 session;     /* counts transport closes */

static void reverse_into(u8 *out, const u8 *in, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) out[i] = in[n - 1 - i];
}

static int p256_random(uint8_t *dest, unsigned size)
{
    while (size) {
        u8 n = size > 255 ? 255 : (u8)size;
        get_random_number(dest, n);
        dest += n;
        size -= n;
    }
    return 1;
}

/* The ready pair to BTstack: the caller has seen ready set. */
static void p256_hand_out(void)
{
    u8 event[2 + 1 + 1 + 64];

    memcpy(p256.private_out, p256.private, 32);
    p256.have_out = 1;
    event[0] = HCI_EVENT_LE_META;
    event[1] = 1 + 1 + 64;
    event[2] = HCI_SUBEVENT_LE_READ_LOCAL_P256_PUBLIC_KEY_COMPLETE;
    event[3] = 0;
    reverse_into(event + 4, p256.public, 32);
    reverse_into(event + 36, p256.public + 32, 32);
    p256.ready = 0;
    inject(event, sizeof(event));
}

static void p256_dhkey(void)
{
    u8 event[2 + 1 + 1 + 32], secret[32];
    u32 from = p256.dhkey_session;
    int ok = p256.have_out && uECC_valid_public_key(p256.peer, uECC_secp256r1()) &&
             uECC_shared_secret(p256.peer, p256.private_out, secret, uECC_secp256r1());

    event[0] = HCI_EVENT_LE_META;
    event[1] = 1 + 1 + 32;
    event[2] = HCI_SUBEVENT_LE_GENERATE_DHKEY_COMPLETE;
    event[3] = ok ? 0 : ERROR_CODE_INVALID_HCI_COMMAND_PARAMETERS;
    if (ok) reverse_into(event + 4, secret, 32);
    else memset(event + 4, 0xff, 32);
    p256.dhkey_wanted = 0;
    if (from == session) inject(event, sizeof(event));
}

static void p256_body(void)
{
    uECC_set_rng(p256_random);
    for (;;) {
        if (p256.dhkey_wanted) {
            p256_dhkey();
        } else if (!p256.ready) {
            if (uECC_make_key(p256.public, p256.private, uECC_secp256r1())) p256.ready = 1;
        } else if (p256.key_wanted) {
            p256.key_wanted = 0;
            p256_hand_out();
        } else {
            plat_p256_wait();
        }
    }
}

/* Command Status now, the result when there is one. */
static void p256_command(u8 *packet, int size)
{
    u16 opcode = little_endian_read_16(packet, 0);
    u8 status[] = { HCI_EVENT_COMMAND_STATUS, 4, 0, 1, packet[0], packet[1] };

    if (opcode == HCI_OPCODE_HCI_LE_GENERATE_DHKEY) {
        if (p256.dhkey_wanted) status[2] = ERROR_CODE_CONTROLLER_BUSY;
        else if (size < 3 + 64) status[2] = ERROR_CODE_INVALID_HCI_COMMAND_PARAMETERS;
        inject(status, sizeof(status));
        if (status[2]) return;
        reverse_into(p256.peer, packet + 3, 32);
        reverse_into(p256.peer + 32, packet + 3 + 32, 32);
        p256.dhkey_session = session;
        p256.dhkey_wanted = 1;
    } else {
        inject(status, sizeof(status));
        if (p256.ready) {
            p256_hand_out();
        } else {
            p256.key_wanted = 1;
        }
    }
    plat_p256_wake();
}

/* ---- anchors: when the central's packets arrive ---------------------------
 * The controller calls btctrlermod_air_rx for each packet a central sends on a
 * connection (interrupt context).  The first packet more than half the
 * link's period (hci_jieli_anchor_period; ANCHOR_GAP_US when not known)
 * after the last anchor starts a new one: the others belong to the same
 * connection event or sniff anchor (a 251-byte LE PDU at 1M takes 2.1 ms).
 * A slave's packet goes out in answer to the central's, so a report has to
 * be in the controller before the anchor it is meant for. */
#define ANCHOR_GAP_US 1000
#define ANCHORS       8                  /* a power of two */

static volatile u32 anchor_at[ANCHORS], anchor_n, anchor_gap_us = ANCHOR_GAP_US;

void btctrlermod_air_rx(void)
{
    u32 now = plat_us();

    if (!anchor_n || now - anchor_at[anchor_n % ANCHORS] >= anchor_gap_us) {
        anchor_at[(anchor_n + 1) % ANCHORS] = now;
        anchor_n++;
        util_trace(TR_ANCHOR, 0);
    }
}

/* The link's anchor period (us), 0 when it has none. */
void hci_jieli_anchor_period(u32 us)
{
    anchor_gap_us = us ? us / 2 : ANCHOR_GAP_US;
}

/* Anchors so far; *at: when the newest one was. */
u32 hci_jieli_anchor(u32 *at)
{
    u32 n;

    plat_lock();
    n = anchor_n;
    *at = anchor_at[n % ANCHORS];
    plat_unlock();
    return n;
}

/* When anchor n was: 0 if it is not one of the last few. */
int hci_jieli_anchor_at(u32 n, u32 *at)
{
    int ok;

    plat_lock();
    ok = n && n <= anchor_n && anchor_n - n < ANCHORS - 1;
    if (ok) *at = anchor_at[n % ANCHORS];
    plat_unlock();
    return ok;
}

void hci_jieli_set_address(const u8 a[6])
{
    memcpy(address, a, 6);
    if (!controller_up) return;
    le_controller_set_mac(address);
    lmp_hci_write_local_address(address);
}

/* Drop the link without the HCI halt, which waits for a peer that may never
 * answer.  The next session opens a fresh controller. */
void hci_jieli_drop_link(void)
{
    if (!rx_active) return;
    hci_jieli_fast_close();
    rx_active = 0;
    btctrler_task_exit();
#ifndef CONFIG_CPU_BD19
    btctrlermod_qos_reset();
#endif
    controller_up = 0;
}

/* ---- transport ---------------------------------------------------------- */

static void rx_process(btstack_data_source_t *ds, btstack_data_source_callback_type_t t)
{
    u8 type, *data;
    u16 size;

    (void)ds;
    (void)t;
    while (hciq_peek(&type, &data, &size)) {
#ifdef HCI_LOG
        if (type == HCI_EVENT_PACKET) {
            const u8 *e = data;
            if (e[0] == 0x0e && little_endian_read_16(e, 3) == 0x1005)
                util_log("< cc 1005 acl %u x%u", little_endian_read_16(e, 6), little_endian_read_16(e, 9));
            else if (e[0] == 0x0e) util_log("< cc %04x %02x", little_endian_read_16(e, 3), e[5]);
            else if (e[0] == 0x0f) util_log("< cs %04x %02x", little_endian_read_16(e, 4), e[2]);
            else util_log("< ev %02x %02x %02x %02x", e[0], e[2], e[3], e[4]);
        } else if (type == HCI_ACL_DATA_PACKET) {
            util_log("< acl len %u", size);
        }
#endif
        host_handler(type, data, size);
        hciq_pop();
    }
    if (held) {
        held = 0;
        btctrlermod_host_ready();
    }
}

static void transport_init(const void *config)
{
    (void)config;
    hciq_clear();
}

/* Classic sniff receive windows in microseconds (the SDK HID app's values):
 * the window a slave opens at an anchor after hearing the central (init),
 * widened by inc for each anchor it misses; mode 0 keeps the BT oscillator.
 * Without this table the controller reads it from address 0 (all 0xff):
 * windows of 65535 us, and in 3.75 ms sniff the pad stopped hearing the
 * central within seconds until the link timed out. */
struct lp_ws_t {                        /* the controller's layout */
    u16 lrc_ws_inc, lrc_ws_init, bt_osc_ws_inc, bt_osc_ws_init;
    u8 osc_change_mode;
};
extern void lp_winsize_init(struct lp_ws_t *lp);

static struct lp_ws_t sniff_windows = {
    .lrc_ws_inc = 480,
    .lrc_ws_init = 160,
    .bt_osc_ws_inc = 100,
    .bt_osc_ws_init = 140,
    .osc_change_mode = 0,
};

/* The controller alone, without the host: the Switch 1 wake transmits on it
 * before a session, which then takes it over. */
int hci_jieli_open(void)
{
    if (!controller_up) {
        lp_winsize_init(&sniff_windows);
        if (btctrler_task_init(hci_transport_h4_controller_instance(), 0)) return -1;
        while (!btctrler_task_ready()) plat_delay_ms(20);
        controller_up = 1;
    }
    return 0;
}

void hci_jieli_close(void)
{
    if (controller_up) btctrler_task_exit();
    controller_up = 0;
#ifndef CONFIG_CPU_BD19
    btctrlermod_qos_reset();
#endif
}

/* The P-256 thread, for LE Secure Connections (btstack_config.h). */
void hci_jieli_init(void)
{
#if !defined(CONFIG_CPU_BD19) && defined(ENABLE_LE_SECURE_CONNECTIONS)
    plat_p256_thread(p256_body);
#endif
}

static int transport_open(void)
{
    if (hci_jieli_open()) return -1;
    drop_logged = 0;
    rx_active = 1;
    return 0;
}

static int transport_close(void)
{
    hci_jieli_fast_close();
    rx_active = 0;
    session++;                   /* a DHKey still being made is not this session's */
    p256.key_wanted = 0;
    hci_jieli_close();
    return 0;
}

static void transport_register(void (*handler)(u8 type, u8 *packet, u16 size))
{
    host_handler = handler;
    btstack_run_loop_set_data_source_handler(&rx_source, rx_process);
    btstack_run_loop_enable_data_source_callbacks(&rx_source, DATA_SOURCE_CALLBACK_POLL);
    btstack_run_loop_add_data_source(&rx_source);
}

static int transport_send(u8 type, u8 *packet, int size)
{
#ifdef HCI_LOG
    if (type == HCI_COMMAND_DATA_PACKET) util_log("> %04x", little_endian_read_16(packet, 0));
    if (type == HCI_ACL_DATA_PACKET) util_log("> acl %04x len %u", little_endian_read_16(packet, 0), size);
#endif
    if (type == HCI_SCO_DATA_PACKET) return -1;
    if (type == HCI_ACL_DATA_PACKET && size >= 2 && (little_endian_read_16(packet, 0) & 0x0fff) == fast_handle)
        fast_queue(0);
    if (type == HCI_COMMAND_DATA_PACKET && size >= 3 &&
        little_endian_read_16(packet, 0) == HCI_OPCODE_HCI_QOS_SETUP &&
        (packet[2] != 20 || size != 23)) {
        u8 ev[] = { HCI_EVENT_COMMAND_STATUS, 4, 0x12, 1, packet[0], packet[1] };
        inject(ev, sizeof(ev));
        return 0;
    }

    /* The controller lists Read Encryption Key Size as supported but never
     * answers it, and BTstack holds every secured channel until it does:
     * answer for it (E0 always uses the full 16-byte key). */
    if (type == HCI_COMMAND_DATA_PACKET && size >= 5 &&
        little_endian_read_16(packet, 0) == HCI_OPCODE_HCI_READ_ENCRYPTION_KEY_SIZE) {
        u8 ev[9] = { HCI_EVENT_COMMAND_COMPLETE, 7, 1, packet[0], packet[1], 0,
                     packet[3], packet[4], 16 };
        inject(ev, sizeof(ev));
        return 0;
    }
    if (type == HCI_COMMAND_DATA_PACKET && size >= 3 &&
        (little_endian_read_16(packet, 0) == HCI_OPCODE_HCI_LE_READ_LOCAL_P256_PUBLIC_KEY ||
         little_endian_read_16(packet, 0) == HCI_OPCODE_HCI_LE_GENERATE_DHKEY)) {
        p256_command(packet, size);
        return 0;
    }
    if (__h4_send_packet) __h4_send_packet(type, packet, size);
    else h4_hci_push_data(type, packet, size);
    return 0;
}

static const hci_transport_t transport = {
    .name = "jieli",
    .init = transport_init,
    .open = transport_open,
    .close = transport_close,
    .register_packet_handler = transport_register,
    .send_packet = transport_send,
};

const hci_transport_t *hci_jieli_transport(void)
{
    return &transport;
}

/* ---- run loop: sleeps until woken, never on a timer tick ---------------- */

static u32 now_ms(void)
{
    return plat_ms();
}

static void set_timer(btstack_timer_source_t *t, u32 ms)
{
    t->timeout = now_ms() + ms + 1;
}

/* A wake while a pass runs leaves the semaphore posted: the next wait
 * returns at once, so no second pass is needed. */
static void wake(void)
{
    plat_bt_wake();
}

static volatile u32 loop_runs, loop_us;

/* The run loop's passes and the time in them, for bt_stats(). */
void hci_jieli_loop_stats(u32 *runs, u32 *us)
{
    *runs = loop_runs;
    *us = loop_us;
}

static void execute(void)
{
    u32 t = plat_us();
    s32 next;

    while (!exit_requested) {
#ifndef CONFIG_CPU_BD19
        btctrlermod_qos_tick();
#endif
        btstack_run_loop_base_poll_data_sources();
        btstack_run_loop_base_execute_callbacks();
        btstack_run_loop_base_process_timers(now_ms());
        loop_runs++;
        next = btstack_run_loop_base_get_time_until_timeout(now_ms());
        if (next == 0) continue;             /* a timer is due */
        loop_us += plat_us() - t;
        util_trace(TR_BT_SLEEP, 0);
        /* until woken or the next BTstack timer, at most 50 ms: bt_pad.c's stall
         * check runs without events too.  The OS waits in 10 ms ticks. */
        plat_bt_wait(next < 0 || next > 50 ? 50 : (u32)next);
        util_trace(TR_BT_WAKE, 0);
        t = plat_us();
    }
}

static void loop_init(void)
{
    btstack_run_loop_base_init();
    exit_requested = 0;
}

static void on_main_thread(btstack_context_callback_registration_t *cb)
{
    btstack_run_loop_base_add_callback(cb);
    wake();
}

static void trigger_exit(void)
{
    exit_requested = 1;
    wake();
}

static const btstack_run_loop_t run_loop = {
    .init = loop_init,
    .add_data_source = btstack_run_loop_base_add_data_source,
    .remove_data_source = btstack_run_loop_base_remove_data_source,
    .enable_data_source_callbacks = btstack_run_loop_base_enable_data_source_callbacks,
    .disable_data_source_callbacks = btstack_run_loop_base_disable_data_source_callbacks,
    .set_timer = set_timer,
    .add_timer = btstack_run_loop_base_add_timer,
    .remove_timer = btstack_run_loop_base_remove_timer,
    .execute = execute,
    .dump_timer = btstack_run_loop_base_dump_timer,
    .get_time_ms = now_ms,
    .poll_data_sources_from_irq = wake,
    .execute_on_main_thread = on_main_thread,
    .trigger_exit = trigger_exit,
};

const btstack_run_loop_t *hci_jieli_run_loop(void)
{
    return &run_loop;
}

/* ---- link policy, and hooks the controller library calls ----------------- */

extern void lmp_set_sniff_establish_by_remote(u8 enable);
extern void lmp_sniff_t_slot_attemp_reset(u16 slots, u16 attempt);
extern void edr_vendor_keep_connect_for_keymiss(u8 enable);
extern void bt_max_pwr_set(u8 pwr, u8 pg_pwr, u8 iq_pwr, u8 ble_pwr);
extern void set_bt_data_rate_acl_3mbs_mode(u8 en);
extern void bt_production_test(u8 en);
extern void P33_CON_SET(u16 addr, u8 start, u8 len, u8 data);
extern u8 p33_rx_1byte(u16 addr);

static u16 sniff_slots = 24, sniff_attempt = 1;

/* Before a session: the profile's sniff parameters, and keep the link when a
 * host's key is missing (it re-pairs instead of dropping). */
void hci_jieli_prepare(u16 slots, u16 attempt, int edr3)
{
    if (slots) sniff_slots = slots;
    if (attempt) sniff_attempt = attempt;
    edr_vendor_keep_connect_for_keymiss(1);
    /* The controller leaves 3 Mbit/s EDR out of its features unless told
     * (and BTstack then never allows 3-DH1).  Off by default (slot setting
     * edr3): with it, 83-byte PlayStation reports on a Linux host stalled
     * for 20-60 ms more often than as two 2-DH1 packets.  Before the host
     * stack reads the features. */
    set_bt_data_rate_acl_3mbs_mode(edr3 ? 1 : 0);
}

/* Transmit power for the next session (docs/jieli/radio.md): the controller uses
 * the smallest of the step, its normal-mode cap 4 and any fixed level, from
 * the normal table or, after bt_production_test(1), the pro one.  That call
 * also sets the radio supply VDC13 (P3_ANA_CON6 bits 0-2): step 7 going in,
 * 5 (about 1 dB weaker) going out.  The supply is put back as booted. */
#define P3_ANA_CON6 0x06

void hci_jieli_tx_power(u8 step, u8 pro)
{
#ifndef CONFIG_CPU_BD19
    static u8 was_pro;
#endif

    bt_max_pwr_set(step, 5, 8, step);
#ifndef CONFIG_CPU_BD19
    if (pro || was_pro) {
        u8 vdc13 = p33_rx_1byte(P3_ANA_CON6) & 0x07;

        bt_production_test(pro);
        P33_CON_SET(P3_ANA_CON6, 0, 3, vdc13);
    }
    was_pro = pro;
#else
    (void)pro;
#endif
}

/* What sniff_negotiation_hook answers with while our own request is out. */
void hci_jieli_sniff_slots(u16 slots)
{
    if (slots) sniff_slots = slots;
}

/* Once the stack works: a HID device must accept the host's sniff request. */
void hci_jieli_link_policy(void)
{
    lmp_set_sniff_establish_by_remote(1);
    lmp_sniff_t_slot_attemp_reset(sniff_slots, sniff_attempt);
}

void sniff_negotiation_hook(u16 *t_sniff, u16 *attempt, u16 *timeout, u8 negotiation)
{
    (void)negotiation;
    if (!t_sniff || !attempt || !timeout) return;
    *t_sniff = sniff_slots;
    *attempt = sniff_attempt;
    *timeout = 0;
}

u32 user_send_cmd_prepare(int cmd, u16 len, u8 *param)
{
    (void)cmd;
    (void)len;
    (void)param;
    return 0;
}

void btstack_assert_failed(const char *file, u16 line)
{
    util_log("btstack assert %s:%u", file, line);
    plat_reboot();               /* the log keeps the line */
}

