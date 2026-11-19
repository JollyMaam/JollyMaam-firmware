/* BD19's SDK HCI controller member, recovered from the pinned IR. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG int CONFIG_WIFI_DETECT_ENABLE;
extern BTCTRLER_CONFIG u8 log_tag_const_e_HCI_STD;
extern BTCTRLER_CONFIG u8 log_tag_const_i_HCI_STD;
extern BTCTRLER_CONFIG u8 log_tag_const_d_HCI_STD;
extern BTCTRLER_CONFIG int config_btctler_hci_standard;
void log_print(int level, const char *format, const char *message, ...);
void printf_buf(u8 *packet, int size);

#define HCI_BSS __attribute__((section(".hci_controller_bss")))
#ifdef UNITTEST
#define HCI_CODE __attribute__((noinline))
#else
#define HCI_CODE __attribute__((section(".hci_controller_code")))
#endif
typedef struct __attribute__((packed, may_alias)) { u64 value; } hci_unaligned_u64;
#ifdef UNITTEST
/* Oversized backing lets tests observe the SDK's wrapped allocation request
 * while keeping its subsequent oversized copy inside the fixture. */
static u8 test_heap[65540];
static u32 test_alloc_size;
static void *test_malloc(size_t size)
{ test_alloc_size = size; return test_heap; }
#define malloc test_malloc
#endif
HCI_BSS h4_transport_t __h4_send_packet;
static HCI_BSS struct layer_handle hdl;
static HCI_BSS u8 pHci_buffer_static[2560];

/* ---- to the host ---------------------------------------------------- */

static __attribute__((always_inline)) inline int to_host(int type, u8 *packet, int size)
{
    const hci_transport_t *transport = h4_transport;
    if (!transport) return 0;
    int (*send)(int, const u8 *, int) = transport->send_packet;
    if (!send) return 0;
    return send(type, packet, size);
}

/* The SDK's host-side command formatter. */
HCI_CODE int hci_send_cmd(const hci_cmd_t *cmd, ...)
{
    u8 packet[256];
    u8 *out = packet;
    u32 length = 3;
    int variable_length;
    if (!config_btctler_hci_standard)
        btctrler_assert_failed("BT_HCI_STANDARD_IS_SUPPORT no!", 0);
    va_list ap;
    va_start(ap, cmd);
    u16 opcode = cmd->opcode;
    packet[0] = opcode;
    packet[1] = opcode >> 8;
    const char *fmt = cmd->format;
    while (*fmt) {
        char c = *fmt++;
        int n;
        u32 v;
        const u8 *p;
        switch (c) {
        case '1': case '2': case 'H':
            v = va_arg(ap, int); packet[length++] = v;
            if (c == '2' || c == 'H') packet[length++] = v >> 8;
            break;
        case '3': case '4':
            v = va_arg(ap, int); packet[length++] = v; packet[length++] = v >> 8;
            packet[length++] = v >> 16;
            if (c == '4') packet[length++] = v >> 24;
            break;
        case 'B':
            p = va_arg(ap, const u8 *);
            for (n = 5; n >= 0; n--) packet[length++] = p[n];
            break;
        case 'D':
            p = va_arg(ap, const u8 *);
            ((hci_unaligned_u64 *)(out + length))->value = ((const hci_unaligned_u64 *)p)->value;
            length += 8;
            break;
        case 'E': p = va_arg(ap, const u8 *); __builtin_memcpy(out + length, p, 240); length += 240; break;
        case 'N':
            p = (const u8 *)va_arg(ap, const char *);
            n = (u16)strlen((const char *)p); if (n > 248) n = 248;
            __builtin_memcpy(out + length, p, n); __builtin_memset(out + length + n, 0, 248 - n); length += 248;
            break;
        case 'P': p = va_arg(ap, const u8 *); __builtin_memcpy(out + length, p, 16); length += 16; break;
        case 'A': p = va_arg(ap, const u8 *); __builtin_memcpy(out + length, p, 31); length += 31; break;
        case 'L': variable_length = va_arg(ap, int); break;
        case 'V': p = va_arg(ap, const u8 *); __builtin_memcpy(out + length, p, variable_length); length += variable_length; break;
        case 'X': p = va_arg(ap, const u8 *); variable_length = (int)((u32)variable_length << 2); n = variable_length; __builtin_memcpy(out + length, p, n); length += n; break;
        case 'Y': variable_length = va_arg(ap, int); packet[length++] = variable_length; break;
        case 'Z': p = va_arg(ap, const u8 *); __builtin_memcpy(out + length, p, variable_length); length += variable_length; break;
        default: break;
        }
    }
    packet[2] = length + 253;
    va_end(ap);
    u16 packet_size = (u16)length;
    if (packet_size > 256) btctrler_assert_failed("size <= 256 CMD over limit", packet_size);
    __h4_send_packet(1, packet, packet_size);
    return packet_size;
}

HCI_CODE int hci_send_cmd_payload(u16 opcode, u8 *payload, u16 size)
{
    u16 packet_size = size + 4;
    u8 *packet = malloc(packet_size);
    *(u16 *)packet = opcode;
    packet[2] = size;
    __builtin_memcpy(packet + 3, payload, size);
    __h4_send_packet(1, packet, packet_size);
    return size;
}

HCI_CODE int le_hci_send_acl_packet(u8 *packet, u16 size)
{ if (log_tag_const_d_HCI_STD) { log_print(1, 0, "[Debug]: [HCI_STD]ACL ->\r\n"); printf_buf(packet, size); } __h4_send_packet(2, packet, size); return 0; }
HCI_CODE int hci_send_acl_packet(const u8 *packet, u16 size)
{ if (log_tag_const_d_HCI_STD) { log_print(1, 0, "[Debug]: [HCI_STD]ACL ->\r\n"); printf_buf((u8 *)packet, size); } __h4_send_packet(2, packet, size); return 0; }
HCI_CODE int hci_send_iso_packet(const u8 *packet, u16 size)
{ if (log_tag_const_d_HCI_STD) { log_print(1, 0, "[Debug]: [HCI_STD]ISO ->\r\n"); printf_buf((u8 *)packet, size); } __h4_send_packet(5, packet, size); return 0; }

HCI_CODE int hci_send_sco_data(u8 *packet, u16 size)
{ if (log_tag_const_i_HCI_STD) { log_print(2, 0, "[Info]: [HCI_STD]SCO <\r\n"); printf_buf(packet, size); } return to_host(3, packet, size); }
HCI_CODE int hci_send_iso_data(u8 *packet, u16 size)
{ return to_host(5, packet, size); }

HCI_CODE int hci_send_acl_data(u8 *packet, u16 size)
{ if (log_tag_const_i_HCI_STD) { log_print(2, 0, "[Info]: [HCI_STD]ACL < \r\n"); printf_buf(packet, size); } return to_host(2, packet, size); }

/* Event packets: code, length, parameters (__vsprintf's format letters). */
HCI_CODE int hci_send_event(int opcode, const char *format, ...)
{
    struct hci_event_hci_controller evt;
    evt.event = opcode;
    va_list args; va_start(args, format);
    int len = __vsprintf(evt.data, format, args);
    if (len >= 256) btctrler_assert_failed("event length", len);
    evt.length = len;
    va_end(args);
    return to_host(4, &evt.event, evt.length + 2);
}

/* The SDK's extended events: a 16-bit length (LE). */
HCI_CODE int hci_send_ext_event(int opcode, const char *format, ...)
{
    struct hci_event_hci_controller evt;
    evt.event = opcode;
    va_list args; va_start(args, format);
    int len = __vsprintf(evt.ext_data, format, args);
    if (len >= 512) btctrler_assert_failed("extended event length", len);
    evt.ext_len = len;
    va_end(args);
    return to_host(4, &evt.event, (u16)(evt.ext_len + 2));
}

HCI_CODE int hci_send_event_payload(int opcode, u8 *payload, u8 size)
{
    struct hci_event_hci_controller evt;
    evt.event = opcode;
    evt.length = size;
    __builtin_memcpy(evt.data, payload, size);
    return to_host(4, &evt.event, size + 2);
}

/* Command Complete: one command packet allowed, the opcode, the result. */
HCI_CODE int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    struct hci_event_hci_controller evt;
    evt.event = 0x0e;
    va_list args; va_start(args, format);
    int len = __vsprintf(evt.data + 3, format, args);
    if (len >= 256) btctrler_assert_failed("command complete length", len);
    evt.length = len + 3;
    va_end(args);
    evt.data[0] = 1;
    evt.data[1] = opcode;
    evt.data[2] = opcode >> 8;
    return to_host(4, &evt.event, evt.length + 2);
}

HCI_CODE int hci_send_event_cmd_complete_payload(int opcode, u8 *payload, u8 size)
{
    struct hci_event_hci_controller evt;
    evt.event = 0x0e;
    evt.length = size + 3;
    evt.data[0] = 1;
    evt.data[1] = opcode;
    evt.data[2] = opcode >> 8;
    __builtin_memcpy(evt.data + 3, payload, size);
    return to_host(4, &evt.event, evt.length + 2);
}

/* ---- from the host --------------------------------------------------- */

static HCI_CODE void hci_h4_download_data(int type, const u8 *packet, int size)
{
    switch (type) {
    case 1: {   /* command: queued for the HCI thread */
        if (log_tag_const_i_HCI_STD) { log_print(2, 0, "[Info]: [HCI_STD]CMD >\r\n"); printf_buf((u8 *)packet, size); }
        if (size >= 255) btctrler_assert_failed("command size", size);
        u16 *cmd = lbuf_alloc_btctrler(hdl.pCMD, (u32)size + 2);
        if (!cmd) {
            if (log_tag_const_e_HCI_STD) log_print(4, 0, "<Error>: [HCI_STD]No HCI CMD buf\r\n");
            return;
        }
        cmd[0] = size;
        __builtin_memcpy(cmd + 1, packet, size);
        lbuf_push_btctrler(cmd);
        thread_resume(&hdl.thread);
        break;
    }
    case 2: {   /* ACL data: to the LE or Classic link with the handle */
        if (log_tag_const_i_HCI_STD) { log_print(2, 0, "[Info]: [HCI_STD]ACL >\r\n"); printf_buf((u8 *)packet, size); }
        int handle = btctler_little_endian_read_16((u8 *)packet, 0) & 0x0fff;
        int modules = config_btctler_modules;
        if ((modules & 2) && ll_link_for_handle(handle)) {
            if (config_btctler_le_roles & 3) ll_push_acl_data(packet, size);
        } else if (modules & 1) {
            if (lmp_conn_for_handle(handle)) lmp_hci_send_packet_standard(packet, size);
            else if (!lmp_esco_conn_for_handle(handle) && log_tag_const_e_HCI_STD)
                log_print(4, 0, "<Error>: [HCI_STD]handle:0x%x\r\n", handle);
        }
        break;
    }
    default:
        if (log_tag_const_e_HCI_STD) log_print(4, 0, "<Error>: [HCI_STD]HCI Packet Type Mismatch 0x%x\r\n", type);
        break;
    }
}

static HCI_CODE void bd19_hci_command_process(u8 *packet, int size)
{
    (void)size;
    u16 opcode = packet[0] | ((u16)packet[1] << 8);
    u8 *params = packet + 3;
    int result = 0;
    int ogf = packet[1] >> 2;
    if (ogf & 0x20) ogf -= 0x40;
    switch (ogf) {
    case 1:
        if (log_tag_const_i_HCI_STD) log_print(2, 0, "[Info]: [HCI_STD]OGF_LINK_CONTROL\r\n");
        result = hci_link_control_cmd_handler(opcode, params, packet[2]);
        if (!result) result = hci_lmp_link_control_cmd_handler(opcode, params, packet[2]);
        break;
    case 2:
        if (log_tag_const_i_HCI_STD) log_print(2, 0, "[Info]: [HCI_STD]OGF_LINK_POLICY\r\n");
        result = hci_link_policy_cmd_handler(opcode, params, packet[2]);
        if (!result) result = hci_lmp_link_policy_cmd_handler(opcode, params, packet[2]);
        break;
    case 3:
        if (log_tag_const_i_HCI_STD) log_print(2, 0, "[Info]: [HCI_STD]OGF_CONTROLLER_BASEBAND\r\n");
        result = hci_controller_baseband_cmd_handler(opcode, params, packet[2]);
        if (!result) result = hci_lmp_controller_baseband_cmd_handler(opcode, params, packet[2]);
        break;
    case 4:
        if (log_tag_const_i_HCI_STD) log_print(2, 0, "[Info]: [HCI_STD]OGF_INFORMATIONAL_PARAMETERS\r\n");
        result = hci_informational_parameters_cmd_handler(opcode, params, packet[2]);
        if (!result) result = hci_lmp_informational_parameters_cmd_handler(opcode, params, packet[2]);
        break;
    case 5:
        if (log_tag_const_i_HCI_STD) log_print(2, 0, "[Info]: [HCI_STD]OOGF_STATUS_PARAMETERS\r\n");
        result = hci_status_parameters_cmd_handler(opcode, params, packet[2]);
        if (!result) result = hci_lmp_status_parameters_cmd_handler(opcode, params, packet[2]);
        break;
    case 6:
        if (log_tag_const_i_HCI_STD) log_print(2, 0, "[Info]: [HCI_STD]OGF_TESTING_COMMANDS\r\n");
        result = hci_testing_cmd_handler(opcode, params, packet[2]);
        if (!result) result = hci_lmp_testing_cmd_handler(opcode, params, packet[2]);
        break;
    case 8:
        if (log_tag_const_i_HCI_STD) log_print(2, 0, "[Info]: [HCI_STD]OGF_LE_CONTROLLER\r\n");
        if (config_btctler_modules & 2) result = hci_le_controller_cmd_handler(opcode, params, packet[2]);
        break;
    case -1:
        if (log_tag_const_i_HCI_STD) log_print(2, 0, "[Info]: [HCI_STD]OGF_VENDOR\r\n");
        result = hci_vendor_cmd_handler(opcode, params, packet[2]);
        if (!result) result = hci_lmp_vendor_cmd_handler(opcode, params, packet[2]);
        break;
    case -2:
        if (log_tag_const_i_HCI_STD) log_print(2, 0, "[Info]: [HCI_STD]OGF_VENDOR\r\n");
        result = ogf_vendor_le_controller(opcode, params, packet[2]);
        break;
    default: btctrler_assert_failed("Controller unknown commands", opcode); break;
    }
    if (result > 1) {
        if (log_tag_const_e_HCI_STD) printf_buf(packet, size);
        btctrler_assert_failed("Match more than once", opcode);
        return;
    }
    if (!result) {
        if (log_tag_const_e_HCI_STD) {
            log_print(4, 0, "<Error>: [HCI_STD]Mismatch commands 0x%x\r\n", opcode);
            printf_buf(packet, size);
        }
        hci_send_event_cmd_complete(opcode, "1", 1);
    }
}

HCI_CODE void h4_hci_push_data(int type, const u8 *packet, int size)
{
    if (type == 1) bd19_hci_command_process((u8 *)packet, size);
    else if (type == 2) {
        if (log_tag_const_i_HCI_STD) log_print(2, 0, "[Info]: [HCI_STD]<- HCI_ACL_DATA_PACKET\r\n");
        u16 raw = btctler_little_endian_read_16((u8 *)packet, 0);
        int handle = raw & 0x0fff;
        int modules = config_btctler_modules;
        struct le_link *link = 0;
        if ((modules & 2) && (config_btctler_le_roles & 3)) link = ll_link_for_handle(handle);
        if (link) ll_push_acl_data(packet, size);
        if ((modules & 1) && lmp_conn_for_handle(handle))
            lmp_hci_send_packet_standard(packet, size);
    } else if (type == 5) {
        if (log_tag_const_i_HCI_STD) log_print(2, 0, "[Info]: [HCI_STD]<- HCI_ISO_DATA_PACKET\r\n");
        hci_push_iso_sdu((u8 *)packet, (u16)size);
    }
}

static HCI_CODE void hci_thread_process(struct thread *th)
{
    if (!config_btctler_hci_standard) return;
    u16 *cmd = lbuf_pop_btctrler(hdl.pCMD);
    if (cmd) {
        bd19_hci_command_process((u8 *)(cmd + 1), cmd[0]);
        lbuf_free_btctrler(cmd);
    }
    if (lbuf_empty_btctrler(hdl.pCMD)) thread_suspend(th, 0);
}

/* ---- start and stop -------------------------------------------------- */

HCI_CODE void hci_interface_setup(void)
{
    if (log_tag_const_i_HCI_STD) log_print(2, 0, "[Info]: [HCI_STD]H4 Setup\r\n");
    const hci_transport_t *transport = h4_transport;
    if (!transport) return;
    if (transport->init) {
        transport->init(h4_config);
        transport = h4_transport;
    }
    if (transport->open) {
        transport->open();
        transport = h4_transport;
    }
    if (transport->register_packet_handler)
        transport->register_packet_handler(hci_h4_download_data);
}

HCI_CODE void hci_controller_run_loop(void) { thread_run(&bt_task_thread); }

HCI_CODE __attribute__((noinline)) void hci_controller_init(void)
{
    if (CONFIG_WIFI_DETECT_ENABLE == 1) set_bt_afh_classs_enc(1);
    int log_enabled = log_tag_const_i_HCI_STD;
    if (log_enabled) log_print(2, 0, "[Info]: [HCI_STD]>>>RF setup\r\n");
    bt_rf_init();
    thread_owner_init(&bt_task_thread);
    if (log_enabled) log_print(2, 0, "[Info]: [HCI_STD]>>>NV Memory  setup\r\n");
    btctler_nv_memory_apply();
    if (log_enabled) log_print(2, 0, "[Info]: [HCI_STD]>>>HCI setup\r\n");
    hci_interface_setup();
    if (config_btctler_hci_standard) {
        if (log_enabled) log_print(2, 0, "[Info]: [HCI_STD]HCI controller Data Path Setup\r\n");
        hdl.pHci_buffer = pHci_buffer_static;
        hdl.pCMD = lbuf_init_btctrler(pHci_buffer_static, 512);
        if (!hdl.pCMD) btctrler_assert_failed("__this->pCMD", 0);
        thread_create(&hdl.thread, "hci_controller", hci_thread_process, &bt_task_thread);
    }
    int modules = config_btctler_modules;
    if (modules & 1) {
        if (log_enabled) log_print(2, 0, "[Info]: [HCI_STD]>>>Classic setup\r\n");
        link_bulk_init();
        bredr_bd_init();
        bredr_link_init();
        lmp_init();
        int mode = config_btctler_mode;
        if (mode & 4) bredr_fcc_init(4, 0);
        else if (mode & 16) bredr_fcc_init(16, 0);
    }
    if (modules & 2) {
        if (config_btctler_mode & 1) {
            if (log_enabled) log_print(2, 0, "[Info]: [HCI_STD]>>>LE setup\r\n");
            ll_init();
        }
        if (config_btctler_mode & 4) {
            if (log_enabled) log_print(2, 0, "[Info]: [HCI_STD]>>>LE DUT - FCC\r\n");
            dut_le_hw_open();
        }
        if (config_btctler_mode & 16) {
            if (log_enabled) log_print(2, 0, "[Info]: [HCI_STD]>>>LE DUT - BT_FIX_FRE\r\n");
            dut_le_hw_open();
        }
    }
    bt_analog_reset();
}

HCI_CODE void hci_controller_destory(void)
{
    if (config_btctler_hci_standard) lbuf_clear_btctrler(hdl.pCMD);
    h4_transport->close();
    int modules = config_btctler_modules;
    if (modules & 1) {
        link_task_close_all();
        bredr_link_close();
        bredr_bd_close();
    }
    if (modules & 2) {
        ll_ext_destroy();
        ll_destroy();
    }
    bt_rf_close();
}

#ifdef UNITTEST
#include "test.h"
#define CHECK_RETURN(condition, code) do { int test_ok = !!(condition); CHECK(test_ok); if (!test_ok) return (code); } while (0)
static u8 test_packet[300];
static int test_type, test_size, test_asserts;
static int alloc_size, alloc_fail, alloc_count, push_count, resume_count;
static int suspend_count, le_sent, classic_sent, mutate_modules_on_link, mutate_modules_on_le_send;
static u8 test_lbuf[300];
static void (*registered_download)(int, const u8 *, int);
static int interface_stage;
static int lifecycle_active, lifecycle_log_calls, lifecycle_queue_fail, lifecycle_queue_size;
static char lifecycle_trace[64];
static unsigned int lifecycle_trace_pos;
static int dispatch_primary, dispatch_primary_result, dispatch_fallback, dispatch_fallback_len, mutate_command_length;
static int vsprintf_override = -1;
static int vsprintf_write_size = -1;
static int transport_return = 73;
const hci_transport_t *h4_transport;
const void *h4_config;
int config_btctler_modules = 3, config_btctler_mode, config_btctler_le_roles = 3;
u8 log_tag_const_e_HCI_STD = 0, log_tag_const_i_HCI_STD = 0, log_tag_const_d_HCI_STD = 0;
int CONFIG_WIFI_DETECT_ENABLE;
struct thread_owner bt_task_thread;
static void lifecycle_mark(char c)
{ if (lifecycle_active && lifecycle_trace_pos < sizeof(lifecycle_trace) - 1) lifecycle_trace[lifecycle_trace_pos++] = c; }
static void test_send(int type, const u8 *packet, int size)
{
    test_type = type;
    test_size = size;
    if (size > (int)sizeof(test_packet)) return;
    __builtin_memcpy(test_packet, packet, size);
}
static int test_transport_send(int type, const u8 *packet, int size)
{ test_send(type, packet, size); return transport_return; }
static hci_transport_t test_transport = { .send_packet = test_transport_send };
static hci_transport_t transport_a, transport_b, transport_c;
static void transport_init_a(const void *config)
{ if (config == (const void *)0x1234) { interface_stage = 1; h4_transport = &transport_b; } }
static int transport_open_b(void)
{ if (interface_stage == 1) { interface_stage = 2; h4_transport = &transport_c; } return 0; }
static void transport_register_c(void (*handler)(int, const u8 *, int))
{ if (interface_stage == 2) { interface_stage = 3; registered_download = handler; } }
static void test_transport_chain(void)
{
    transport_a.init = transport_init_a;
    transport_b.open = transport_open_b;
    transport_c.register_packet_handler = transport_register_c;
    h4_transport = &transport_a;
    h4_config = (const void *)0x1234;
    hci_interface_setup();
}
void log_print(int level, const char *file, const char *format, ...)
{ (void)level; (void)file; (void)format; if (lifecycle_active) { lifecycle_log_calls++; log_tag_const_i_HCI_STD = 0; } }
void printf_buf(u8 *packet, int size) { (void)packet; (void)size; }
struct lbuff_head_btctrler *lbuf_init_btctrler(void *buffer, u32 size)
{ if (lifecycle_active) { lifecycle_mark('Q'); lifecycle_queue_size = size; CHECK(buffer == pHci_buffer_static); return lifecycle_queue_fail ? 0 : (struct lbuff_head_btctrler *)1; } (void)buffer; (void)size; return (struct lbuff_head_btctrler *)1; }
void *lbuf_alloc_btctrler(struct lbuff_head_btctrler *head, u32 size)
{ (void)head; alloc_size = size; alloc_count++; return alloc_fail ? 0 : test_lbuf; }
void lbuf_push_btctrler(void *packet) { if (packet == test_lbuf) push_count++; }
void *lbuf_pop_btctrler(struct lbuff_head_btctrler *head) { (void)head; return 0; }
void lbuf_free_btctrler(void *packet) { (void)packet; }
int lbuf_empty_btctrler(struct lbuff_head_btctrler *head) { (void)head; return 1; }
void lbuf_clear_btctrler(struct lbuff_head_btctrler *head)
{ if (lifecycle_active) { CHECK(head == 0); lifecycle_mark('C'); } }
void thread_resume(struct thread *th) { (void)th; resume_count++; }
void thread_suspend(struct thread *th, int timeout) { (void)th; (void)timeout; suspend_count++; }
u16 btctler_little_endian_read_16(const u8 *packet, int offset)
{ return packet[offset] | ((u16)packet[offset + 1] << 8); }
struct le_link *ll_link_for_handle(int handle)
{ if (mutate_modules_on_link) config_btctler_modules = 0; return handle == 0x30 ? (struct le_link *)1 : 0; }
int ll_push_acl_data(const u8 *packet, int size)
{ (void)packet; (void)size; le_sent++; if (mutate_modules_on_le_send) config_btctler_modules = 0; return 0; }
struct lmp_connection *lmp_conn_for_handle(int handle)
{ return handle == 0x30 || handle == 0x31 ? (struct lmp_connection *)1 : 0; }
struct lmp_connection *lmp_esco_conn_for_handle(int handle) { (void)handle; return 0; }
int lmp_hci_send_packet_standard(const u8 *packet, int size) { (void)packet; (void)size; classic_sent++; return 0; }
void hci_push_iso_sdu(u8 *packet, u16 size) { (void)packet; (void)size; }
void set_bt_afh_classs_enc(u8 mode) { CHECK(mode == 1); lifecycle_mark('W'); log_tag_const_i_HCI_STD = 1; }
void bt_rf_init(void) { lifecycle_mark('R'); }
void bt_rf_close(void) { lifecycle_mark('Z'); }
int thread_owner_init(struct thread_owner *owner) { CHECK(owner == &bt_task_thread); lifecycle_mark('O'); return 0; }
void btctler_nv_memory_apply(void) { lifecycle_mark('N'); }
int thread_create(struct thread *th, char *name, void (*fun)(struct thread *), struct thread_owner *owner)
{ CHECK(th == &hdl.thread && !strcmp(name, "hci_controller") && fun == hci_thread_process && owner == &bt_task_thread); lifecycle_mark('T'); return 0; }
int link_bulk_init(void) { lifecycle_mark('b'); config_btctler_modules = 0; return 0; }
void bredr_bd_init(void) { lifecycle_mark('B'); }
void bredr_bd_close(void) { lifecycle_mark('b'); }
void bredr_link_init(void) { lifecycle_mark('D'); }
void bredr_link_close(void) { lifecycle_mark('l'); }
void link_task_close_all(void) { lifecycle_mark('c'); }
void lmp_init(void) { lifecycle_mark('l'); config_btctler_mode = 16; }
void bredr_fcc_init(u8 mode, u8 fre)
{ CHECK(mode == 16 && fre == 0); lifecycle_mark('F'); config_btctler_mode = 5; }
void ll_init(void) { lifecycle_mark('I'); config_btctler_mode = 20; }
void ll_destroy(void) { lifecycle_mark('d'); }
void ll_ext_destroy(void) { lifecycle_mark('e'); }
void dut_le_hw_open(void) { lifecycle_mark('U'); config_btctler_mode = 16; }
void bt_analog_reset(void) { lifecycle_mark('A'); }
static int destroy_close(void)
{ lifecycle_mark('X'); config_btctler_modules = 3; return 0; }
static hci_transport_t destroy_transport = { .close = destroy_close };
int hci_link_control_cmd_handler(u16 opcode, const u8 *data, u32 len)
{ (void)opcode; (void)len; dispatch_primary++; if (mutate_command_length) ((u8 *)data)[-1] = 7; return dispatch_primary_result; }
int hci_lmp_link_control_cmd_handler(u16 opcode, u8 *data, u32 len)
{ (void)opcode; (void)data; dispatch_fallback++; dispatch_fallback_len = len; return 1; }
#define TEST_HANDLER(name, quals, len_type) \
    int name(u16 opcode, quals u8 *data, len_type len) \
    { (void)opcode; (void)data; (void)len; return 0; }
TEST_HANDLER(hci_link_policy_cmd_handler, const, u32)
TEST_HANDLER(hci_lmp_link_policy_cmd_handler, , u32)
TEST_HANDLER(hci_controller_baseband_cmd_handler, const, u32)
TEST_HANDLER(hci_lmp_controller_baseband_cmd_handler, const, u32)
TEST_HANDLER(hci_informational_parameters_cmd_handler, const, u32)
TEST_HANDLER(hci_lmp_informational_parameters_cmd_handler, const, u32)
TEST_HANDLER(hci_status_parameters_cmd_handler, const, u32)
TEST_HANDLER(hci_lmp_status_parameters_cmd_handler, const, u32)
TEST_HANDLER(hci_testing_cmd_handler, const, u32)
TEST_HANDLER(hci_lmp_testing_cmd_handler, const, u32)
TEST_HANDLER(hci_vendor_cmd_handler, const, u32)
TEST_HANDLER(hci_lmp_vendor_cmd_handler, , u32)
TEST_HANDLER(hci_le_controller_cmd_handler, const, u32)
TEST_HANDLER(ogf_vendor_le_controller, const, int)
#undef TEST_HANDLER
int __vsprintf(u8 *data, const char *format, va_list ap)
{
    if (vsprintf_override >= 0) {
        int n = vsprintf_write_size < 0 ? vsprintf_override : vsprintf_write_size;
        __builtin_memset(data, 0x5a, n);
        return vsprintf_override;
    }
    int n = 0;
    for (; *format; format++) if (*format == '1') data[n++] = va_arg(ap, int);
    return n;
}

int config_btctler_hci_standard = 1;
void btctrler_assert_failed(const char *what, int value)
{ (void)what; (void)value; test_asserts++; }

int main(void)
{
    static const u8 data[] = { 0x91, 0x82, 0x73 };
    static const hci_cmd_t cmd = { 0x1234, "1YV" };
    static const hci_cmd_t short_cmd = { 0x0102, "2H" };
    static const hci_cmd_t xcmd = { 0x0103, "LXZ" };
    static const hci_cmd_t ncmd = { 0x0104, "N" };
    static u8 xdata[16], long_name[65537];
    static u8 huge_payload[65535];
    __h4_send_packet = test_send;
    CHECK_RETURN(!(hci_send_cmd(&cmd, 0xaa, 3, data) != 8 || test_type != 1 || test_size != 8), 1);
    CHECK_RETURN(!(memcmp(test_packet, (u8[]){ 0x34, 0x12, 5, 0xaa, 3, 0x91, 0x82, 0x73 }, 8)), 2);
    CHECK_RETURN(!(test_asserts), 3);
    config_btctler_hci_standard = 0;
    CHECK_RETURN(!(hci_send_cmd(&cmd, 0xaa, 3, data) != 8 || test_asserts != 1 || test_size != 8), 40);
    config_btctler_hci_standard = 1;
    test_asserts = 0;
    CHECK_RETURN(!(hci_send_cmd(&short_cmd, 0x1234, 0x5678) != 7 || test_size != 7), 6);
    CHECK_RETURN(!(memcmp(test_packet, (u8[]){ 2, 1, 4, 0x34, 0x12, 0x78, 0x56 }, 7)), 7);
    for (unsigned int i = 0; i < sizeof(xdata); i++) xdata[i] = i + 1;
    CHECK_RETURN(!(hci_send_cmd(&xcmd, 2, xdata, xdata) != 19 || test_size != 19), 8);
    CHECK_RETURN(!(test_packet[2] != 16 || memcmp(test_packet + 3, xdata, 8) || memcmp(test_packet + 11, xdata, 8)), 9);
    __builtin_memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = 0;
    CHECK_RETURN(!(hci_send_cmd(&ncmd, long_name) != 251 || test_size != 251), 10);
    for (unsigned int i = 3; i < 251; i++) CHECK(test_packet[i] == 0);
    CHECK_RETURN(!(hci_send_cmd_payload(0x4567, (u8 *)data, 3) != 3 || test_type != 1 || test_size != 7), 4);
    CHECK_RETURN(!(memcmp(test_packet, (u8[]){ 0x67, 0x45, 3, 0x91, 0x82, 0x73 }, 6)), 5);
    CHECK_RETURN(!(hci_send_cmd_payload(0x4567, huge_payload, 65532) != 65532 || test_alloc_size != 0 || test_size != 0), 38);
    CHECK_RETURN(!(hci_send_cmd_payload(0x4567, huge_payload, 65535) != 65535 || test_alloc_size != 3 || test_size != 3), 39);
    h4_transport = &test_transport;
    CHECK_RETURN(!(hci_send_event(0x05, "1", 0x99) != transport_return || test_type != 4 || test_size != 3), 12);
    CHECK_RETURN(!(memcmp(test_packet, (u8[]){ 5, 1, 0x99 }, 3)), 13);
    CHECK_RETURN(!(hci_send_event_payload(0x3e, (u8 *)data, 3) != transport_return || test_size != 5), 14);
    CHECK_RETURN(!(memcmp(test_packet, (u8[]){ 0x3e, 3, 0x91, 0x82, 0x73 }, 5)), 15);
    vsprintf_override = 0;
    CHECK_RETURN(!(hci_send_event(0x05, "") != transport_return || test_size != 2 || test_packet[1] != 0), 35);
    CHECK_RETURN(!(hci_send_ext_event(0xff, "") != transport_return || test_size != 2 || test_packet[1] != 0), 36);
    vsprintf_override = 255;
    CHECK_RETURN(!(hci_send_event(0x05, "") != transport_return || test_size != 257 || test_packet[1] != 255 || test_packet[3] != 0x5a), 37);
    vsprintf_override = 255;
    CHECK_RETURN(!(hci_send_event_cmd_complete(0x1234, "") != transport_return || test_size != 4), 27);
    CHECK_RETURN(!(memcmp(test_packet, (u8[]){ 0x0e, 2, 1, 0x34 }, 4)), 28);
    CHECK_RETURN(!(hci_send_ext_event(0xff, "") != transport_return || test_size != 257), 29);
    vsprintf_override = 65535; vsprintf_write_size = 0;
    CHECK_RETURN(!(hci_send_ext_event(0xff, "") != transport_return || test_size != 1), 42);
    vsprintf_override = -1; vsprintf_write_size = -1;
    CHECK_RETURN(!(test_packet[0] != 0xff || test_packet[1] != 0xff || test_packet[2] != 0 || test_packet[3] != 0x5a), 30);
    vsprintf_override = -1;
    u8 payload[255]; for (unsigned int i = 0; i < sizeof(payload); i++) payload[i] = i;
    static const u8 complete_lengths[] = { 0, 252, 253, 255 };
    static const u16 complete_wire[] = { 5, 257, 2, 4 };
    static const u8 complete_headers[] = { 3, 255, 0, 2 };
    for (unsigned int i = 0; i < sizeof(complete_lengths); i++) {
        u8 len = complete_lengths[i];
        CHECK_RETURN(!(hci_send_event_cmd_complete_payload(0x3456, payload, len) != transport_return || test_size != complete_wire[i]), 31);
        CHECK(test_packet[0] == 0x0e && test_packet[1] == complete_headers[i]);
        if (complete_wire[i] >= 3) CHECK(test_packet[2] == 1);
        if (complete_wire[i] >= 4) CHECK(test_packet[3] == 0x56);
        if (complete_wire[i] >= 5) CHECK(test_packet[4] == 0x34);
    }
    test_transport_chain();
    CHECK_RETURN(!(interface_stage != 3 || !registered_download), 16);
    registered_download(1, data, 0);
    CHECK_RETURN(!(alloc_size != 2 || push_count != 1 || resume_count != 1), 17);
    u8 boundary[255]; for (unsigned int i = 0; i < sizeof(boundary); i++) boundary[i] = i;
    registered_download(1, boundary, 254);
    CHECK_RETURN(!(alloc_size != 256 || ((u16 *)test_lbuf)[0] != 254 || memcmp(test_lbuf + 2, boundary, 254)), 18);
    alloc_fail = 1;
    int old_push = push_count, old_resume = resume_count;
    registered_download(1, boundary, 2);
    CHECK_RETURN(!(push_count != old_push || resume_count != old_resume), 19);
    alloc_fail = 0;
    int asserts_before_overflow = test_asserts;
    registered_download(1, boundary, 255);
    CHECK_RETURN(!(test_asserts != asserts_before_overflow + 1 || alloc_size != 257 || push_count != old_push + 1 || resume_count != old_resume + 1), 20);
    u8 acl[] = { 0x30, 0x00, 1, 0, 0x55 };
    config_btctler_modules = 3; config_btctler_le_roles = 3;
    int old_le = le_sent, old_classic = classic_sent;
    h4_hci_push_data(2, acl, sizeof(acl));
    CHECK_RETURN(!(le_sent != old_le + 1 || classic_sent != old_classic + 1), 21);
    mutate_modules_on_le_send = 1;
    h4_hci_push_data(2, acl, sizeof(acl));
    mutate_modules_on_le_send = 0;
    CHECK_RETURN(!(classic_sent != old_classic + 2), 33);
    config_btctler_modules = 3;
    u8 command[] = { 0x01, 0x04, 2, 0xaa, 0xbb };
    dispatch_primary = dispatch_fallback = 0; mutate_command_length = 1;
    h4_hci_push_data(1, command, sizeof(command));
    CHECK_RETURN(!(dispatch_primary != 1 || dispatch_fallback != 1 || dispatch_fallback_len != 7), 26);
    mutate_command_length = 0;
    h4_transport = &test_transport;
    dispatch_primary = dispatch_fallback = 0; dispatch_primary_result = 1;
    int old_type = test_type, old_size = test_size;
    h4_hci_push_data(1, command, sizeof(command));
    CHECK_RETURN(!(dispatch_primary != 1 || dispatch_fallback || test_type != old_type || test_size != old_size), 41);
    dispatch_primary_result = 0;
    u8 unknown[] = { 0, 0, 0 };
    int old_asserts = test_asserts;
    h4_hci_push_data(1, unknown, sizeof(unknown));
    CHECK(test_asserts == old_asserts + 1);
    CHECK(test_type == 4);
    CHECK(test_size == 6);
    CHECK(!memcmp(test_packet, (u8[]){ 0x0e, 4, 1, 0, 0, 1 }, 6));
    old_le = le_sent; old_classic = classic_sent;
    registered_download(2, acl, sizeof(acl));
    CHECK_RETURN(!(le_sent != old_le + 1 || classic_sent != old_classic), 22);
    u8 unowned_acl[] = { 0x31, 0x00, 1, 0, 0x55 };
    mutate_modules_on_link = 1;
    old_classic = classic_sent;
    registered_download(2, unowned_acl, sizeof(unowned_acl));
    mutate_modules_on_link = 0;
    CHECK_RETURN(!(classic_sent != old_classic + 1), 34);
    config_btctler_modules = 3;
    config_btctler_le_roles = 0;
    old_le = le_sent; old_classic = classic_sent;
    registered_download(2, acl, sizeof(acl));
    CHECK_RETURN(!(le_sent != old_le || classic_sent != old_classic), 23);
    config_btctler_le_roles = 3;
    config_btctler_hci_standard = 0;
    hci_thread_process((struct thread *)1);
    CHECK_RETURN(!(suspend_count), 24);
    config_btctler_hci_standard = 1;
    hci_thread_process((struct thread *)1);
    CHECK_RETURN(!(suspend_count != 1), 25);
    lifecycle_active = 1; lifecycle_trace_pos = 0; lifecycle_log_calls = 0; lifecycle_queue_fail = 1;
    config_btctler_modules = 3; config_btctler_mode = 4; config_btctler_hci_standard = 1;
    CONFIG_WIFI_DETECT_ENABLE = 1; log_tag_const_i_HCI_STD = 0; h4_transport = 0;
    old_asserts = test_asserts;
    hci_controller_init();
    lifecycle_trace[lifecycle_trace_pos] = 0;
    CHECK_RETURN(strcmp(lifecycle_trace, "WRONQTbBDlFIUUA") == 0, 43);
    CHECK_RETURN(lifecycle_queue_size == 512 && lifecycle_log_calls == 8 && test_asserts == old_asserts + 1, 44);
    lifecycle_trace_pos = 0; lifecycle_log_calls = 0; lifecycle_active = 1;
    h4_transport = &destroy_transport; config_btctler_modules = 0;
    hci_controller_destory();
    lifecycle_trace[lifecycle_trace_pos] = 0;
    CHECK_RETURN(strcmp(lifecycle_trace, "CXclbedZ") == 0, 45);
    lifecycle_trace_pos = 0; lifecycle_log_calls = 0; config_btctler_hci_standard = 0;
    h4_transport = &destroy_transport; config_btctler_modules = 0;
    hci_controller_destory();
    lifecycle_trace[lifecycle_trace_pos] = 0;
    CHECK_RETURN(strcmp(lifecycle_trace, "XclbedZ") == 0, 46);
    lifecycle_active = 0;
    return TEST_DONE("bd19_hci_controller");
}
#endif
