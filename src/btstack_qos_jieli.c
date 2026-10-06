/* Narrow HCI QoS implementation for the Jieli Classic peripheral. Owned C
 * builds the request using the retained SDK packet formatter; this file owns
 * validation and truthful completion.
 * Best Effort only: no bandwidth reservation or Guaranteed service claim. */
#include <string.h>
#include "pad.h"
#include "btstack_qos_jieli.h"

extern int hci_packet_handler(u8 type, u8 *packet, u16 size);
extern u8 cyclone2_lmp_qos_send(u16 handle, u16 slots);
extern u8 cyclone2_lmp_qos_answer(u16 handle, u8 status);

#define QOS_OPCODE 0x0807
#define QOS_TIMEOUT_MS 2000
static struct {
    u32 started;
    u16 handle, slots;
    u8 pending, timed_out;
} qos;

static u16 read16(const u8 *p) { return (u16)(p[0] | ((u16)p[1] << 8)); }
static u32 read32(const u8 *p) { return (u32)read16(p) | ((u32)read16(p + 2) << 16); }
static void put32(u8 *p, u32 n)
{
    p[0] = (u8)n; p[1] = (u8)(n >> 8); p[2] = (u8)(n >> 16); p[3] = (u8)(n >> 24);
}

/* Caller holds plat_lock, so this and the SDK's event producer cannot race
 * over the transport's receive queue. Event buffers are copied immediately. */
static void command_status(u8 status)
{
    u8 event[] = { 0x0f, 4, status, 1, QOS_OPCODE & 255, QOS_OPCODE >> 8 };
    hci_packet_handler(4, event, sizeof(event));
}
static void complete(u16 handle, u8 status, u16 slots)
{
    u8 event[23] = { 0x0d, 21, status, (u8)handle, (u8)(handle >> 8), 0, 1 };
    /* Token rate and peak bandwidth are unspecified. Return the actual
     * accepted poll interval, not the unrounded HCI request. */
    put32(event + 15, status ? 0 : (u32)slots * 625);
    put32(event + 19, 0xffffffffu);
    hci_packet_handler(4, event, sizeof(event));
}

void cyclone2_qos_reset(void)
{
    plat_lock();
    memset(&qos, 0, sizeof(qos));
    plat_unlock();
}

void cyclone2_qos_setup(const u8 *p)
{
    u16 handle = read16(p), slots;
    u32 latency = read32(p + 12);
    u8 status = 0;
    int started = 0;
    /* An interval consists of slot pairs. Round down to remain within the
     * requested access latency. Six slots is the SDK's supported minimum.
     * The app's zero setting skips this command entirely. */
    if (handle > 0x0eff || p[2] || latency < 3750 || latency > 2560000u)
        status = 0x12;                 /* invalid HCI parameters */
    else if (p[3] != 1 || read32(p + 4) || read32(p + 8) || read32(p + 16) != 0xffffffffu)
        status = 0x11;                 /* unsupported feature/parameter */
    slots = (u16)((latency / 625) & ~1u);
    plat_lock();
    if (!status && (qos.pending || qos.timed_out)) status = 0x0c;
    if (!status) {
        qos.handle = handle;
        qos.slots = slots;
        qos.started = plat_ms();
        qos.pending = 1;
        started = 1;
    }
    plat_unlock();
    if (!status) status = cyclone2_lmp_qos_send(handle, slots);
    plat_lock();
    if (status && started) qos.pending = 0;
    command_status(status);
    plat_unlock();
}

int cyclone2_qos_rx(u16 handle, u8 role, u8 header, const u8 *p, u16 size)
{
    u8 opcode = header >> 1;
    int consumed = 0;
    /* role 1 is peripheral. Our request has peripheral transaction ID 1;
     * a central notification has ID 0. Other LMP traffic remains SDK-owned.
     * This hook is after the SDK's PDU-length validation. */
    if (role != 1) return 0;
    if (((opcode == 41 || opcode == 42) && size != 4) || (opcode == 3 && size != 2) ||
        (opcode == 4 && size != 3)) return 0;
    plat_lock();
    if (opcode == 41 && !(header & 1)) {
        u16 slots = read16(p);
        if (slots) complete(handle, 0, slots);
        consumed = 1;                 /* notification has no LMP reply */
    } else if (opcode == 42 && !(header & 1)) {
        u16 slots = read16(p);
        u8 status;
        plat_unlock();                 /* SDK response enqueue owns its locks */
        status = cyclone2_lmp_qos_answer(handle, slots ? 0 : 0x1e);
        plat_lock();
        if (slots && !status) complete(handle, 0, slots);
        consumed = 1;                 /* central request: reply before reporting */
    } else if ((opcode == 3 || opcode == 4) && p[0] == 42 && (header & 1)) {
        if (qos.pending && qos.handle == handle) {
            qos.pending = 0;
            complete(handle, opcode == 3 ? 0 : p[1], qos.slots);
        }
        consumed = 1;
    }
    plat_unlock();
    return consumed;
}

void cyclone2_qos_link_event(const u8 *event, u16 size)
{
    /* New connection, disconnection, and HCI reset delimit the lifetime of
     * a handle. In particular, late replies must not complete a new session. */
    if ((size >= 13 && event[0] == 3 && event[2] == 0) ||
        (size >= 6 && event[0] == 5 && read16(event + 3) == qos.handle) ||
        (size >= 6 && event[0] == 0x0e && read16(event + 3) == 0x0c03 && event[5] == 0))
        cyclone2_qos_reset();
}

void cyclone2_qos_tick(void)
{
    plat_lock();
    if (qos.pending && (u32)(plat_ms() - qos.started) >= QOS_TIMEOUT_MS) {
        qos.pending = 0;
        /* The wire reply has no request serial number. Do not retry on the
         * same link after timeout: a late acceptance would be ambiguous. */
        qos.timed_out = 1;
        complete(qos.handle, 0x22, 0);  /* LMP response timeout; keep the link */
    }
    plat_unlock();
}
#ifdef UNITTEST
static u32 test_now;
static u8 test_status, test_event[23];
static int test_events, test_sends, test_answers, test_locked;
static u16 test_handle, test_slots;
u32 plat_ms(void) { return test_now; }
void plat_lock(void) { CHECK(!test_locked); test_locked = 1; }
void plat_unlock(void) { CHECK(test_locked); test_locked = 0; }
int hci_packet_handler(u8 type, u8 *packet, u16 size)
{
    CHECK(type == 4 && test_locked && size <= sizeof(test_event));
    memcpy(test_event, packet, size);
    test_events++;
    return 0;
}
u8 cyclone2_lmp_qos_send(u16 handle, u16 slots)
{
    test_sends++; test_handle = handle; test_slots = slots;
    return test_status;
}
u8 cyclone2_lmp_qos_answer(u16 handle, u8 status)
{
    CHECK(!test_locked && handle == 256);
    test_answers++; test_status = status;
    return 0;
}
static void request(u32 us, u8 service)
{
    u8 p[20] = { 0x00, 0x01, 0, service };
    put32(p + 12, us); put32(p + 16, 0xffffffffu);
    cyclone2_qos_setup(p);
}
int main(void)
{
    u8 accepted[] = { 42 }, rejected[] = { 42, 0x20 }, other[] = { 46 };
    u8 notification[] = { 16, 0, 2 };
    u8 disconnected[] = { 5, 4, 0, 0, 1, 0x13 };
    int n;
    cyclone2_qos_reset();
    request(3750, 1);
    CHECK(test_sends == 1 && test_handle == 256 && test_slots == 6);
    CHECK(test_events == 1 && test_event[0] == 0x0f && !test_event[2] && qos.pending);
    n = test_events;
    CHECK(!cyclone2_qos_rx(256, 1, 7, other, 2));
    CHECK(!cyclone2_qos_rx(256, 1, 7, accepted, 1));
    cyclone2_qos_rx(257, 1, 7, accepted, 2);
    cyclone2_qos_rx(256, 1, 6, accepted, 2);  /* wrong transaction ID */
    CHECK(test_events == n && qos.pending);
    request(3750, 1);                       /* duplicate must not cancel */
    CHECK(test_event[2] == 0x0c && qos.pending && test_sends == 1);
    request(1250, 1);                       /* impossible latency */
    CHECK(test_event[2] == 0x12 && qos.pending);
    cyclone2_qos_rx(256, 1, 7, accepted, 2);
    CHECK(!qos.pending && test_event[0] == 0x0d && !test_event[2]);
    CHECK(read32(test_event + 15) == 3750 && read32(test_event + 7) == 0);
    request(5100, 1);
    CHECK(test_slots == 8 && qos.pending);
    cyclone2_qos_rx(256, 1, 9, rejected, 3);
    CHECK(!qos.pending && test_event[2] == 0x20 && !read32(test_event + 15));
    request(3750, 2);                       /* no Guaranteed claim */
    CHECK(test_event[0] == 0x0f && test_event[2] == 0x11 && !qos.pending);
    test_status = 2; request(3750, 1);       /* unknown handle */
    CHECK(test_event[2] == 2 && !qos.pending);
    test_status = 0;
    CHECK(cyclone2_qos_rx(256, 1, 82, notification, 4));
    CHECK(test_event[0] == 0x0d && read32(test_event + 15) == 10000);
    CHECK(cyclone2_qos_rx(256, 1, 84, notification, 4));
    CHECK(test_answers == 1 && !test_status && read32(test_event + 15) == 10000);
    notification[0] = 0;
    n = test_events;
    CHECK(cyclone2_qos_rx(256, 1, 84, notification, 4));
    CHECK(test_answers == 2 && test_status == 0x1e && test_events == n);
    test_status = 0;
    CHECK(!cyclone2_qos_rx(256, 0, 82, notification, 4));
    CHECK(test_events == n);
    test_now = 0xfffffff0u; request(3750, 1);
    test_now += QOS_TIMEOUT_MS - 1; cyclone2_qos_tick(); CHECK(qos.pending);
    test_now++; cyclone2_qos_tick();
    CHECK(!qos.pending && qos.timed_out && test_event[2] == 0x22);
    n = test_events;
    cyclone2_qos_tick(); cyclone2_qos_rx(256, 1, 7, accepted, 2);
    CHECK(test_events == n);                /* no late false success */
    n = test_sends; request(3750, 1);
    CHECK(test_event[2] == 0x0c && test_sends == n);
    cyclone2_qos_link_event(disconnected, sizeof(disconnected));
    CHECK(!qos.pending && !qos.timed_out);
    request(3750, 1); CHECK(qos.pending && test_sends == n + 1);
    cyclone2_qos_reset(); CHECK(!qos.pending);
    return TEST_DONE("jieli Classic QoS");
}
#endif
