/* The controller's side of HCI, from the SDK's hci_controller.c and
 * h4_controller_interface.c: event packets to the host, host packets in,
 * the HCI command thread, and controller start and stop.
 *
 * Packets to the host go through the transport registered at
 * btctrler_task_init: the SDK's in-chip transport below, which hands them
 * to the host stack (hci_packet_handler, btstack_hci_jieli.c).  The host's
 * packets come back through the handler that transport registers
 * (__h4_send_packet): commands are queued for the HCI thread, ACL data goes
 * straight to its link.  The SDK's host-side senders (hci_send_cmd and
 * friends, for its own host stack) and voice/ISO delivery have no caller. */
#include <stdarg.h>
#include <string.h>
#include "btctrler/glue.h"

_Static_assert(BTCFG_HCI_STANDARD, "a standard HCI host: queued commands, events to the transport");

int hci_packet_handler(u8 type, u8 *packet, u16 size);    /* btstack_hci_jieli.c */
extern BTCTRLER_CONFIG int CONFIG_WIFI_DETECT_ENABLE;

#define HCI_BSS __attribute__((section(".hci_controller_bss")))
HCI_BSS void (*__h4_send_packet)(int type, const u8 *packet, int size);
static HCI_BSS struct layer_handle hdl;
/* The command queue: the SDK reserved 2560 bytes and used 512. */
static HCI_BSS u8 command_buffer[512] __attribute__((aligned(4)));

/* ---- to the host ---------------------------------------------------- */

static int to_host(u8 type, u8 *packet, u16 size)
{
    if (!h4_transport || !h4_transport->send_packet) return 0;
    return h4_transport->send_packet(type, packet, size);
}

int hci_send_acl_data(u8 *packet, u16 size) { return to_host(2, packet, size); }

/* Event packets: code, length, parameters (__vsprintf's format letters). */
int hci_send_event(int opcode, const char *format, ...)
{
    struct hci_event_hci_controller evt;
    evt.event = opcode;
    va_list args; va_start(args, format);
    int len = __vsprintf(evt.data, format, args);
    va_end(args);
    if (len >= 256) btctrler_assert_failed("event length", len);
    evt.length = len;
    return to_host(4, &evt.event, evt.length + 2);
}

/* The SDK's extended events: a 16-bit length (LE). */
int hci_send_ext_event(int opcode, const char *format, ...)
{
    struct hci_event_hci_controller evt;
    evt.event = opcode;
    va_list args; va_start(args, format);
    int len = __vsprintf(evt.ext_data, format, args);
    va_end(args);
    if (len >= 512) btctrler_assert_failed("extended event length", len);
    evt.ext_len = len;
    return to_host(4, &evt.event, evt.ext_len + 2);
}

int hci_send_event_payload(int opcode, u8 *payload, u8 size)
{
    struct hci_event_hci_controller evt;
    evt.event = opcode;
    evt.length = size;
    memcpy(evt.data, payload, size);
    return to_host(4, &evt.event, size + 2);
}

/* Command Complete: one command packet allowed, the opcode, the result. */
int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    struct hci_event_hci_controller evt;
    evt.event = 0x0e;
    va_list args; va_start(args, format);
    int len = __vsprintf(evt.data + 3, format, args);
    va_end(args);
    if (len >= 256) btctrler_assert_failed("command complete length", len);
    evt.length = len + 3;
    evt.data[0] = 1;
    evt.data[1] = opcode;
    evt.data[2] = opcode >> 8;
    return to_host(4, &evt.event, evt.length + 2);
}

int hci_send_event_cmd_complete_payload(int opcode, u8 *payload, u8 size)
{
    struct hci_event_hci_controller evt;
    evt.event = 0x0e;
    evt.length = size + 3;
    evt.data[0] = 1;
    evt.data[1] = opcode;
    evt.data[2] = opcode >> 8;
    memcpy(evt.data + 3, payload, size);
    return to_host(4, &evt.event, evt.length + 2);
}

/* ---- from the host --------------------------------------------------- */

static void hci_h4_download_data(int type, const u8 *packet, int size)
{
    switch (type) {
    case 1: {   /* command: queued for the HCI thread */
        if (size >= 255) btctrler_assert_failed("command size", size);
        u16 *cmd = lbuf_alloc_btctrler(hdl.pCMD, size + 2);
        if (!cmd) return;               /* no queue space: the command is lost */
        cmd[0] = size;
        memcpy(cmd + 1, packet, size);
        lbuf_push_btctrler(cmd);
        thread_resume(&hdl.thread);
        break;
    }
    case 2: {   /* ACL data: to the LE or Classic link with the handle */
        int handle = (packet[0] | packet[1] << 8) & 0x0fff;
        if ((config_btctler_modules & 2) && ll_link_for_handle(handle)) {
            if (config_btctler_le_roles & 3) ll_push_acl_data(packet, size);
        } else if ((config_btctler_modules & 1) && lmp_conn_for_handle(handle)) {
            lmp_hci_send_packet_standard(packet, size);
        }
        break;
    }
    }
}

static void hci_thread_process(struct thread *th)
{
    u16 *cmd = lbuf_pop_btctrler(hdl.pCMD);
    if (cmd) {
        hci_command_process((u8 *)(cmd + 1), cmd[0]);
        lbuf_free_btctrler(cmd);
    }
    if (lbuf_empty_btctrler(hdl.pCMD)) thread_suspend(th, 0);
}

/* ---- the in-chip transport (h4_controller_interface.c) ---------------- */

static void h4_controller_init(const void *config) { (void)config; }
static int h4_controller_open(void) { return 0; }
static int h4_controller_close(void) { return 0; }
static void h4_controller_register_packet_handler(void (*handler)(int, const u8 *, int))
{ __h4_send_packet = handler; }
static int h4_controller_can_send_now(uint8_t type) { (void)type; return 0; }
/* Non-zero: the host has no room for this ACL packet now (host flow
 * control, glue.h). */
static int h4_controller_send_packet(int type, const u8 *packet, int size)
{
    return hci_packet_handler(type, (u8 *)packet, size);
}

/* Ours: the host has room again.  LE: the link layer's thread takes what
 * waits; Classic: a Host Number Of Completed Packets to the connection, which
 * hands over what waits (lmp_commands.c).  Any thread. */
void btctrlermod_host_ready(void)
{
    if (config_btctler_modules & 2) ll_thread_resume();
    if (config_btctler_modules & 1) {
        for (struct lmp_connection *conn = lmp_acl_link; conn < lmp_acl_link + 1; ++conn)
            if (conn->link) lmp_hci_cmd_to_conn(conn, 2, 0x0c35, 0);
    }
}

static const hci_transport_t hci_transport_controller
    __attribute__((section(".hci_controller_const"))) = {
    .name = "H4_CONTROLLER",
    .init = h4_controller_init,
    .open = h4_controller_open,
    .close = h4_controller_close,
    .register_packet_handler = h4_controller_register_packet_handler,
    .can_send_packet_now = h4_controller_can_send_now,
    .send_packet = h4_controller_send_packet,
};

const hci_transport_t *hci_transport_h4_controller_instance(void) { return &hci_transport_controller; }

/* ---- start and stop -------------------------------------------------- */

void hci_interface_setup(void)
{
    if (!h4_transport) return;
    if (h4_transport->init) h4_transport->init(h4_config);
    if (h4_transport->open) h4_transport->open();
    if (h4_transport->register_packet_handler) h4_transport->register_packet_handler(hci_h4_download_data);
}

void hci_controller_run_loop(void) { thread_run(&bt_task_thread); }

void hci_controller_init(void)
{
    if (CONFIG_WIFI_DETECT_ENABLE == 1) set_bt_afh_classs_enc(1);
    bt_rf_init();
    thread_owner_init(&bt_task_thread);
    btctler_nv_memory_apply();
    hci_interface_setup();
    hdl.pHci_buffer = command_buffer;
    hdl.pCMD = lbuf_init_btctrler(command_buffer, sizeof(command_buffer));
    if (!hdl.pCMD) btctrler_assert_failed("HCI command queue", 0);
    thread_create(&hdl.thread, "hci_controller", hci_thread_process, &bt_task_thread);
    if (config_btctler_modules & 1) {
        link_bulk_init();
        bredr_bd_init();
        bredr_link_init();
        lmp_init();
        if (config_btctler_mode & 4) bredr_fcc_init(4, 0);
        else if (config_btctler_mode & 16) bredr_fcc_init(16, 0);
    }
    if (config_btctler_modules & 2) {
        if (config_btctler_mode & 1) ll_init();
        if (config_btctler_mode & 4) dut_le_hw_open();
        if (config_btctler_mode & 16) dut_le_hw_open();
    }
    bt_analog_reset();
}

void hci_controller_destory(void)
{
    lbuf_clear_btctrler(hdl.pCMD);
    h4_transport->close();
    if (config_btctler_modules & 1) {
        link_task_close_all();
        bredr_link_close();
        bredr_bd_close();
    }
    if (config_btctler_modules & 2) {
        ll_ext_destroy();
        ll_destroy();
    }
    bt_rf_close();
}

#ifdef UNITTEST
#include <stdarg.h>
#include <stdio.h>
#include "test.h"
int config_btctler_modules = 3, config_btctler_mode = 1, config_btctler_le_roles = 6, CONFIG_WIFI_DETECT_ENABLE;
struct thread_owner bt_task_thread;
const hci_transport_t *h4_transport;
const void *h4_config;
static u8 delivered[600], delivered_type;
static int delivered_size, asserts, routed_le, routed_classic, resumed, suspended, processed;
static int le_handle = -1, classic_handle = -1, queue_full;
static u8 queue[8][300]; static int queued, popped;
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; asserts++; }
int hci_packet_handler(u8 type, u8 *packet, u16 size)
{ delivered_type = type; delivered_size = size; memcpy(delivered, packet, size); return 0; }
/* "1": one byte; "2": two bytes LE; "c" count then bytes. */
int __vsprintf(u8 *data, const char *format, va_list ap)
{
    int n = 0;
    for (; *format; format++) {
        if (*format == '1') data[n++] = va_arg(ap, int);
        else if (*format == '2') { int v = va_arg(ap, int); data[n++] = v; data[n++] = v >> 8; }
        else if (*format == 'L') { int len = va_arg(ap, int); memset(data + n, 0xab, len); n += len; }
    }
    return n;
}
struct le_link *ll_link_for_handle(int handle) { return handle == le_handle ? (struct le_link *)1 : 0; }
int ll_push_acl_data(const u8 *packet, int len) { (void)packet; routed_le = len; return 0; }
struct lmp_connection *lmp_conn_for_handle(int handle) { return handle == classic_handle ? (struct lmp_connection *)1 : 0; }
int lmp_hci_send_packet_standard(const u8 *packet, int len) { (void)packet; routed_classic = len; return 0; }
void *lbuf_alloc_btctrler(struct lbuff_head_btctrler *head, u32 len)
{ (void)head; CHECK(len <= 300); return queue_full ? 0 : queue[queued % 8]; }
void lbuf_push_btctrler(void *lbuf) { CHECK(lbuf == queue[queued % 8]); queued++; }
void *lbuf_pop_btctrler(struct lbuff_head_btctrler *head) { (void)head; return popped < queued ? queue[popped++ % 8] : 0; }
void lbuf_free_btctrler(void *lbuf) { (void)lbuf; }
int lbuf_empty_btctrler(struct lbuff_head_btctrler *head) { (void)head; return popped == queued; }
void thread_resume(struct thread *th) { CHECK(th == &hdl.thread); resumed++; }
void thread_suspend(struct thread *th, int timeout) { (void)th; (void)timeout; suspended++; }
static u8 command_seen[300]; static int command_size;
void hci_command_process(u8 *cmd, int size) { memcpy(command_seen, cmd, size); command_size = size; processed++; }
static void (*registered)(int, const u8 *, int);
static void init(const void *c) { CHECK(c == (void *)0x1234); }
static int open_(void) { return 0; }
static void reg(void (*h)(int, const u8 *, int)) { registered = h; }
int main(void)
{
    /* No transport yet: nothing is sent. */
    CHECK(hci_send_event(0x05, "1", 1) == 0 && !delivered_size);
    h4_transport = hci_transport_h4_controller_instance();

    hci_send_event(0x05, "121", 0, 0x0030, 0x16);
    CHECK(delivered_type == 4 && delivered_size == 6);
    CHECK(!memcmp(delivered, (u8[]){ 0x05, 4, 0, 0x30, 0x00, 0x16 }, 6));
    hci_send_event_cmd_complete(0x0c03, "1", 0);
    CHECK(delivered_size == 6 && !memcmp(delivered, (u8[]){ 0x0e, 4, 1, 0x03, 0x0c, 0 }, 6));
    u8 payload[3] = { 7, 8, 9 };
    hci_send_event_cmd_complete_payload(0x1001, payload, 3);
    CHECK(delivered_size == 8 && !memcmp(delivered, (u8[]){ 0x0e, 6, 1, 0x01, 0x10, 7, 8, 9 }, 8));
    hci_send_event_payload(0x3e, payload, 3);
    CHECK(delivered_size == 5 && !memcmp(delivered, (u8[]){ 0x3e, 3, 7, 8, 9 }, 5));
    hci_send_ext_event(0xff, "L", 300);
    CHECK(delivered_size == 302 && delivered[0] == 0xff && delivered[1] == (300 & 0xff) && delivered[2] == 1);
    CHECK(!asserts); hci_send_event(0x05, "L", 256); CHECK(asserts == 1); asserts = 0;
    u8 acl[5] = { 0x30, 0x20, 1, 0, 9 };
    hci_send_acl_data(acl, 5); CHECK(delivered_type == 2 && delivered_size == 5);

    /* Host side: the transport registers the download handler. */
    hci_transport_t t = { .init = init, .open = open_, .register_packet_handler = reg };
    h4_transport = &t; h4_config = (void *)0x1234;
    hci_interface_setup(); CHECK(registered == hci_h4_download_data);
    h4_transport = hci_transport_h4_controller_instance(); h4_config = 0;
    hci_interface_setup(); CHECK(__h4_send_packet == hci_h4_download_data);

    u8 cmd[4] = { 0x03, 0x0c, 0 };
    __h4_send_packet(1, cmd, 3); CHECK(queued == 1 && resumed == 1 && !processed);
    hci_thread_process(&hdl.thread);
    CHECK(processed == 1 && command_size == 3 && !memcmp(command_seen, cmd, 3) && suspended == 1);
    queue_full = 1; __h4_send_packet(1, cmd, 3); CHECK(queued == 1 && resumed == 1); queue_full = 0;

    le_handle = 0x030; __h4_send_packet(2, acl, 5); CHECK(routed_le == 5 && !routed_classic);
    routed_le = 0; config_btctler_le_roles = 0; __h4_send_packet(2, acl, 5); CHECK(!routed_le && !routed_classic);
    le_handle = -1; classic_handle = 0x030; __h4_send_packet(2, acl, 5); CHECK(routed_classic == 5 && !routed_le);
    routed_classic = 0; config_btctler_modules = 2; __h4_send_packet(2, acl, 5); CHECK(!routed_classic);
    return TEST_DONE("btctrler_hci_transport");
}
#endif
