/* BD19 H4 UART transport recovered from h4_uart_interface_loop.c.o occurrence 1. */
#include "btctrler/glue.h"
#include <string.h>
#ifdef UNITTEST
#include <stdio.h>
#include <stdarg.h>
#endif

#define H4_CODE __attribute__((section(".hci_interface_code")))
#define H4_DATA __attribute__((section(".hci_interface_data")))
#define H4_BSS __attribute__((section(".hci_interface_bss")))
#define H4_CONST __attribute__((section(".hci_interface_const")))

static void (*packet_handler)(int, const u8 *, int);
#ifdef UNITTEST
struct thread_owner bt_task_thread;
#endif

#ifdef UNITTEST
#include "test.h"
#define local_irq_disable h4_test_irq_disable
#define local_irq_enable h4_test_irq_enable
#define thread_suspend h4_test_thread_suspend
#define thread_resume h4_test_thread_resume
#define thread_create h4_test_thread_create
#define thread_delete h4_test_thread_delete
#define h4_uart_init h4_test_uart_init
#define h4_uart_close h4_test_uart_close
#define __uart_tx h4_test_uart_tx
#define log_print h4_test_log
#define printf h4_test_printf
#define printf_buf h4_test_printf_buf
#define puts h4_test_puts
static unsigned h4_test_irq_off, h4_test_irq_on, h4_test_suspend, h4_test_resume;
static unsigned h4_test_create, h4_test_delete, h4_test_init, h4_test_close;
static int h4_test_tx_len;
static u8 h4_test_tx_bytes[64];
static int h4_test_packets, h4_test_packet_type, h4_test_packet_len;
static u8 h4_test_packet[1024];
static unsigned h4_test_log_count, h4_test_puts_count;
static int h4_test_refill_on_clear;
static int h4_test_last_level;
static const char *h4_test_last_log;
static unsigned h4_test_le_reads;
static int h4_test_change_second_length;
static int h4_test_mutate_pop_irq;
static struct uart_rx *h4_test_rx;
static struct uart_hdl *h4_test_hdl;
static void (*h4_test_created_thread)(struct thread *);
static u32 h4_test_init_baud;
static void (*h4_test_expected_old_handler)(int, const u8 *, int);
static int h4_test_register_saw_old_handler;
static int h4_test_printf(const char *fmt, ...);
static void h4_test_irq_disable(void)
{ ++h4_test_irq_off; if (h4_test_mutate_pop_irq) { h4_test_mutate_pop_irq = 0; h4_test_rx->rd_index = 1022; h4_test_rx->cnt = 2; } }
static void h4_test_irq_enable(void) { ++h4_test_irq_on; }
static void h4_test_thread_suspend(struct thread *t, int timeout) { (void)t; CHECK(timeout == 0); ++h4_test_suspend; }
static void h4_test_thread_resume(struct thread *t) { (void)t; ++h4_test_resume; }
static int h4_test_thread_create(struct thread *t, char *n, void (*f)(struct thread *), struct thread_owner *o)
{ CHECK(n && strcmp(n, "h4_uart") == 0); CHECK(o == &bt_task_thread); ++h4_test_create; h4_test_created_thread = f; t->fun = (void (*)())1; if (h4_test_hdl) h4_test_hdl->baudrate = 460800; return 0; }
static int h4_test_thread_delete(struct thread *t) { (void)t; ++h4_test_delete; return 0; }
static void h4_test_uart_init(u32 baud) { h4_test_init_baud = baud; ++h4_test_init; }
static void h4_test_uart_close(void) { ++h4_test_close; }
static void h4_test_uart_tx(u8 *p, u16 n)
{ if (h4_test_tx_len < (int)sizeof h4_test_tx_bytes) { int take = n; if (take > (int)sizeof h4_test_tx_bytes - h4_test_tx_len) take = (int)sizeof h4_test_tx_bytes - h4_test_tx_len; memcpy(h4_test_tx_bytes + h4_test_tx_len, p, (size_t)take); h4_test_tx_len += take; } }
static void h4_test_log(int level, const char *file, const char *fmt, ...)
{ (void)file; h4_test_last_level = level; h4_test_last_log = fmt; if (strcmp(fmt, "[Info]: [H4 UART]registering packet handler\r\n") == 0) h4_test_register_saw_old_handler = packet_handler == h4_test_expected_old_handler; ++h4_test_log_count; }
static int h4_test_printf(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int n = vfprintf(stderr, fmt, ap); va_end(ap); return n; }
static void h4_test_printf_buf(const void *p, int n) { (void)p; (void)n; }
static int h4_test_puts(const char *s) { (void)s; ++h4_test_puts_count;
    if (h4_test_refill_on_clear) {
        h4_test_refill_on_clear = 0;
        __data_push_more((char[]){0x61,0x62}, 2);
    }
    return 0;
}
#else
extern void local_irq_disable();
extern void local_irq_enable();
#endif

static struct uart_rx uart_rx_t H4_BSS;
static struct uart_hdl hdl H4_BSS;
static int reset_timeout H4_DATA;
static void dummy_handler(int type, const u8 *packet, int size) H4_CODE;
static void (*packet_handler)(int, const u8 *, int) H4_DATA = dummy_handler;
static void h4_uart_thread_process(struct thread *thread) H4_CODE;
static void h4_two_wire_uart_thread_process(struct thread *thread) H4_CODE;
static void uart_init(const void *config) H4_CODE;
static int uart_open(void) H4_CODE;
static int uart_close(void) H4_CODE;
static void uart_register_packet_handler(void (*handler)(int, const u8 *, int)) H4_CODE;
static int uart_can_send_now(u8 type) H4_CODE;
static int uart_send_packet(int type, const u8 *packet, int size) H4_CODE;
static int uart_set_baudrate(u32 baudrate) H4_CODE;
extern BTCTRLER_CONFIG int config_dut_protocol_mode;
extern void __uart_tx(u8 *buf, u16 len);
extern int printf(const char *format, ...);
extern void printf_buf(const void *buf, int len);
extern int puts(const char *text);
extern void log_print(int level, const char *file, const char *format, ...);

static const hci_transport_t hci_transport_uart H4_CONST = {
    .name = "H4_UART",
    .init = (void (*)(const void *))uart_init,
    .open = uart_open,
    .close = uart_close,
    .register_packet_handler = uart_register_packet_handler,
    .can_send_packet_now = uart_can_send_now,
    .send_packet = uart_send_packet,
    .set_baudrate = uart_set_baudrate,
    .reset_link = 0,
    .set_sco_config = 0,
};

void __data_reset(void) H4_CODE;
void __data_reset(void) { __builtin_memset(&uart_rx_t, 0, 1030); }

void __data_push(char data) H4_CODE;
void __data_push(char data)
{
    reset_timeout = 0;
    if (uart_rx_t.wr_index > 1023) uart_rx_t.wr_index = 0;
    u16 pos = uart_rx_t.wr_index;
    uart_rx_t.wr_index = (u16)(pos + 1);
    uart_rx_t.buf[pos] = data;
    uart_rx_t.cnt = (u16)(uart_rx_t.cnt + 1);
}

void __data_push_more(char *data, u16 len) H4_CODE;
void __data_push_more(char *data, u16 len)
{
    u16 pos = uart_rx_t.wr_index;
    u16 tail_len = len;
    u16 first_len = 0;
    if ((u32)pos + len > 1023) {
        int n = 1024 - pos;
        first_len = (u16)n;
        __builtin_memcpy(&uart_rx_t.buf[pos], data, (u32)first_len);
        uart_rx_t.wr_index = 0;
        uart_rx_t.cnt = (u16)(uart_rx_t.cnt + n);
        tail_len = (u16)((u32)len - (u32)n);
    }
    __builtin_memcpy(&uart_rx_t.buf[uart_rx_t.wr_index], data + first_len, (u32)tail_len);
    uart_rx_t.wr_index = (u16)(uart_rx_t.wr_index + tail_len);
    uart_rx_t.cnt = (u16)(uart_rx_t.cnt + tail_len);
}

void __data_check(char *buf, char offset, int len) H4_CODE;
void __data_check(char *buf, char offset, int len)
{
    int index = ((int)uart_rx_t.rd_index + (int)offset) % 1024;
    u16 pos = (u16)index;
    int first = 1024 - (int)pos;
    if (first < len) {
        __builtin_memcpy(buf, &uart_rx_t.buf[pos], (u32)first);
        buf += first;
        len -= first;
        index = 0;
    }
    __builtin_memcpy(buf, &uart_rx_t.buf[(u16)index], (u32)len);
}

int __data_pop(char *buf, int len) H4_CODE;
int __data_pop(char *buf, int len)
{
    u16 rd = uart_rx_t.rd_index;
    int first = 1024 - (int)rd;
    if (first < len) {
        __builtin_memcpy(buf, &uart_rx_t.buf[rd], (u32)first);
        u16 rd_after_copy = uart_rx_t.rd_index;
        int copied = 1024 - (int)rd_after_copy;
        buf += copied;
        len -= copied;
        local_irq_disable();
        u16 rd_before_reset = uart_rx_t.rd_index;
        u16 count_before_reset = uart_rx_t.cnt;
        uart_rx_t.cnt = (u16)(rd_before_reset - 1024 + count_before_reset);
        uart_rx_t.rd_index = 0;
        local_irq_enable();
        rd = uart_rx_t.rd_index;
    }
    __builtin_memcpy(buf, &uart_rx_t.buf[rd], (u32)len);
    local_irq_disable();
    uart_rx_t.rd_index = (u16)(uart_rx_t.rd_index + len);
    uart_rx_t.cnt = (u16)(uart_rx_t.cnt - len);
    local_irq_enable();
    return len;
}

u8 h4_data_empty(void) __asm__("__data_empty") H4_CODE;
u8 h4_data_empty(void) { return uart_rx_t.cnt == 0; }

void h4_uart_dump(void) H4_CODE;
void h4_uart_dump(void)
{
    printf("h4 uart dump : rd %x / wr %x / cnt %x\r\n", uart_rx_t.rd_index, uart_rx_t.wr_index, uart_rx_t.cnt);
    printf_buf(uart_rx_t.buf, 1024);
}
void h4_uart_clear(void) H4_CODE;
void h4_uart_clear(void)
{
    local_irq_disable();
    __builtin_memset(&uart_rx_t, 0, 1030);
    local_irq_enable();
}
void h4_uart_thread_resume(void) H4_CODE;
void h4_uart_thread_resume(void) { thread_resume(&hdl.thread); }

const hci_transport_t *hci_transport_uart_instance(void) H4_CODE;
const hci_transport_t *hci_transport_uart_instance(void) { return &hci_transport_uart; }

static void uart_init(const void *config) H4_CODE;
static void uart_init(const void *config)
{
    const u8 *p = config;
    __builtin_memset(&hdl, 0, 32);
    hdl.baudrate = *(const u32 *)(p + 4);
    hdl.flowcontrol = *(const int *)(p + 12);
    hdl.device_name = *(const char *const *)(p + 16);
    log_print(2, 0, "[Info]: [H4 UART]Baudrate : %d\r\n", hdl.baudrate);
}
static int uart_open(void) H4_CODE;
static int uart_open(void)
{
    if (config_dut_protocol_mode == 0)
        (void)thread_create(&hdl.thread, "h4_uart", h4_uart_thread_process, &bt_task_thread);
    else
        (void)thread_create(&hdl.thread, "h4_uart", h4_two_wire_uart_thread_process, &bt_task_thread);
    h4_uart_init(hdl.baudrate);
    return 0;
}
static int uart_close(void) H4_CODE;
static int uart_close(void)
{
    if (hdl.thread.fun != 0) {
        (void)thread_delete(&hdl.thread);
        hdl.thread.fun = 0;
        h4_uart_close();
    }
    return 0;
}
static void uart_register_packet_handler(void (*handler)(int, const u8 *, int)) H4_CODE;
static void uart_register_packet_handler(void (*handler)(int, const u8 *, int))
{
    log_print(2, 0, "[Info]: [H4 UART]registering packet handler\r\n");
    packet_handler = handler;
}
static int uart_can_send_now(u8 type) H4_CODE;
static int uart_can_send_now(u8 type) { (void)type; return 0; }
static int uart_send_packet(int type, const u8 *packet, int size) H4_CODE;
static int uart_send_packet(int type, const u8 *packet, int size)
{
    if (config_dut_protocol_mode == 0) __uart_tx((u8 *)&type, 1);
    __uart_tx((u8 *)packet, (u16)size);
    return 0;
}
static int uart_set_baudrate(u32 baudrate) H4_CODE;
static int uart_set_baudrate(u32 baudrate) { hdl.baudrate = baudrate; return 0; }
static void dummy_handler(int type, const u8 *packet, int size) { (void)type; (void)packet; (void)size; }

static void h4_uart_thread_process(struct thread *thread) H4_CODE;
static void h4_uart_thread_process(struct thread *thread)
{
    char type;
    u8 header[2];
    u8 packet[1024];
    int old = reset_timeout++;
    if (old > 500000) {
        reset_timeout = 0;
        local_irq_disable(); __builtin_memset(&uart_rx_t, 0, 1030); local_irq_enable();
        puts("clear\r\n");
    }
    if (uart_rx_t.cnt < 4) { thread_suspend(thread, 0); return; }
    __data_check(&type, 0, 1);
    switch ((u8)type) {
    case 1: case 2: case 5: break;
    default:
        __builtin_memset(&uart_rx_t, 0, 1030);
        log_print(2, 0, "[Info]: [H4 UART]type error:%x \r\n\r\n", (u8)type);
        thread_suspend(thread, 0); return;
    }
    int len;
    switch ((u8)type) {
    case 1: {
        __data_check((char *)header, 3, 1);
        u32 n = header[0];
        if (n + 4 > uart_rx_t.cnt) goto incomplete;
        len = (int)n + 3;
        break;
    }
    case 2:
    case 5: {
        __data_check((char *)header, 3, 2);
        u32 needed = (u32)btctler_little_endian_read_16(header, 0) + 5;
        u16 count = uart_rx_t.cnt;
        if (needed > count) goto incomplete;
        u32 n = btctler_little_endian_read_16(header, 0);
        len = (int)n + 4;
        break;
    }
    default:
        log_print(4, 0, "<Error>: [H4 UART]Unknow packet type\r\n\r\n");
        goto incomplete;
    }
    (void)__data_pop((char *)packet, 1);
    (void)__data_pop((char *)packet, len);
    packet_handler(type, packet, len);
    if (uart_rx_t.cnt == 0) thread_suspend(thread, 0);
    return;
incomplete:
    if (uart_rx_t.cnt == 0) thread_suspend(thread, 0);
}

static void h4_two_wire_uart_thread_process(struct thread *thread) H4_CODE;
static void h4_two_wire_uart_thread_process(struct thread *thread)
{
    u8 packet[1024];
    u16 old_count = uart_rx_t.cnt;
    int old = reset_timeout++;
    if (old > 500000) {
        reset_timeout = 0;
        local_irq_disable(); __builtin_memset(&uart_rx_t, 0, 1030); local_irq_enable();
        puts("clear\r\n");
    }
    if (uart_rx_t.cnt == 2) {
        int len = old_count & 255;
        (void)__data_pop((char *)packet, len);
        packet_handler(1, packet, len);
    }
    __builtin_memset(&uart_rx_t, 0, 1030);
    thread_suspend(thread, 0);
}

#ifdef UNITTEST
int config_dut_protocol_mode;
u16 btctler_little_endian_read_16(const u8 *buffer, int pos)
{
    u16 value = (u16)(buffer[pos] | ((u16)buffer[pos + 1] << 8));
    ++h4_test_le_reads;
    if (h4_test_change_second_length && h4_test_le_reads == 2) ++value;
    return value;
}
static void h4_test_packet_handler(int type, const u8 *p, int n)
{
    ++h4_test_packets; h4_test_packet_type = type; h4_test_packet_len = n;
    if (n > 0 && n <= (int)sizeof h4_test_packet) memcpy(h4_test_packet, p, (size_t)n);
}
static void h4_test_mutating_handler(int type, const u8 *p, int n)
{ (void)type; (void)p; (void)n; __data_push((char)0xee); }
int test_h4_uart_interface_loop(void)
{
    char tmp[1100];
    h4_test_rx = &uart_rx_t;
    memset(&uart_rx_t, 0xa5, sizeof uart_rx_t); reset_timeout = 47;
    __data_reset();
    CHECK(uart_rx_t.cnt == 0 && uart_rx_t.wr_index == 0 && uart_rx_t.rd_index == 0 && uart_rx_t.cnt0 == 0xa5a5 && uart_rx_t.rd_index0 == 0xa5a5 && reset_timeout == 47 && (u8)uart_rx_t.buf[7] == 0);
    CHECK(h4_data_empty() == 1);
    h4_uart_clear();
    CHECK(uart_rx_t.cnt == 0 && uart_rx_t.cnt0 == 0xa5a5 && uart_rx_t.rd_index0 == 0xa5a5 && reset_timeout == 47 && h4_test_irq_off == h4_test_irq_on);
    uart_rx_t.rd_index = 7; uart_rx_t.buf[4] = 0x5a;
    char peek;
    __data_check(&peek, -3, 1);
    CHECK((u8)peek == 0x5a);
    __data_reset();
    uart_rx_t.rd_index = 1023; uart_rx_t.cnt = 2; uart_rx_t.buf[1023] = (char)0xa1; uart_rx_t.buf[0] = (char)0xb2;
    h4_test_mutate_pop_irq = 1;
    CHECK(__data_pop(tmp, 2) == 1 && (u8)tmp[0] == 0xa1 && (u8)tmp[1] == 0xb2 && uart_rx_t.rd_index == 1 && uart_rx_t.cnt == 0xffff);
    __data_reset();
    for (unsigned i = 0; i < 1020; ++i) __data_push((char)i);
    CHECK(h4_data_empty() == 0);
    CHECK(__data_pop(tmp, 1018) == 1018 && uart_rx_t.rd_index == 1018 && uart_rx_t.cnt == 2);
    char in[] = {10,11,12,13,14,15,16,17,18,19};
    __data_push_more(in, sizeof in);
    CHECK(uart_rx_t.wr_index == 6 && uart_rx_t.cnt == 12);
    __data_check(tmp, 0, 12);
    CHECK((u8)tmp[0] == 0xfa && (u8)tmp[1] == 0xfb && (u8)tmp[2] == 10 && (u8)tmp[11] == 19);
    CHECK(__data_pop(tmp, 12) == 6 && uart_rx_t.rd_index == 6 && uart_rx_t.cnt == 0);
    CHECK(h4_test_irq_off == h4_test_irq_on);
    reset_timeout = 77; __data_push_more(in, 4); CHECK(reset_timeout == 77);
    reset_timeout = 9; __data_push('x');
    CHECK(reset_timeout == 0);
    h4_test_packets = h4_test_suspend = 0;
    h4_test_expected_old_handler = packet_handler; h4_test_register_saw_old_handler = 0;
    uart_register_packet_handler(h4_test_packet_handler);
    CHECK(h4_test_register_saw_old_handler);
    __data_reset(); __data_push_more((char[]){1,0,0,2,0xaa,0xbb}, 6);
    h4_uart_thread_process((struct thread *)0);
    CHECK(h4_test_packets == 1 && h4_test_packet_type == 1 && h4_test_packet_len == 5);
    CHECK(h4_test_packet[0] == 0 && h4_test_packet[4] == 0xbb && uart_rx_t.cnt == 0);
    __data_reset(); __data_push_more((char[]){2,0,0,2,0,0xaa,0xbb}, 7);
    h4_uart_thread_process((struct thread *)0);
    CHECK(h4_test_packets == 2 && h4_test_packet_type == 2 && h4_test_packet_len == 6);
    __data_reset(); __data_push_more((char[]){2,0,0,2,0,0xaa,0xbb,0xcc}, 8);
    h4_test_le_reads = 0; h4_test_change_second_length = 1;
    h4_uart_thread_process((struct thread *)0);
    h4_test_change_second_length = 0;
    CHECK(h4_test_le_reads == 2 && h4_test_packet_len == 7);
    __data_reset(); uart_register_packet_handler(h4_test_mutating_handler);
    __data_push_more((char[]){1,0,0,0}, 4);
    unsigned suspends_before_mutation = h4_test_suspend;
    h4_uart_thread_process((struct thread *)0);
    CHECK(uart_rx_t.cnt == 1 && h4_test_suspend == suspends_before_mutation);
    uart_register_packet_handler(h4_test_packet_handler);
    __data_reset(); __data_push_more((char[]){9,0,0,0}, 4);
    unsigned before = h4_test_log_count; h4_uart_thread_process((struct thread *)0);
    CHECK(h4_test_log_count == before + 1 && uart_rx_t.cnt == 0 && h4_test_last_level == 2 && strcmp(h4_test_last_log, "[Info]: [H4 UART]type error:%x \r\n\r\n") == 0);
    __data_reset(); __data_push_more((char[]){1,0,0,4}, 4);
    h4_uart_thread_process((struct thread *)0);
    CHECK(uart_rx_t.cnt == 4 && h4_test_suspend > 0);
    __data_reset(); __data_push_more((char[]){1,0,0,0}, 4); reset_timeout = 500001;
    unsigned clears_before = h4_test_puts_count;
    h4_uart_thread_process((struct thread *)0);
    CHECK(reset_timeout == 0 && h4_test_puts_count == clears_before + 1 && uart_rx_t.cnt == 0);
    __data_reset(); __data_push_more((char[]){0xaa,0xbb}, 2); reset_timeout = 500001;
    int packets_before_timeout = h4_test_packets; clears_before = h4_test_puts_count;
    h4_two_wire_uart_thread_process((struct thread *)0);
    CHECK(h4_test_packets == packets_before_timeout && h4_test_puts_count == clears_before + 1 && reset_timeout == 0 && uart_rx_t.cnt == 0);
    __data_reset(); __data_push_more((char[]){0xaa,0xbb}, 2); config_dut_protocol_mode = 1;
    h4_two_wire_uart_thread_process((struct thread *)0);
    CHECK(h4_test_packets == 4 && h4_test_packet_type == 1 && h4_test_packet_len == 2 && uart_rx_t.cnt == 0);
    const hci_transport_t *tr = hci_transport_uart_instance();
    CHECK(tr->name && strcmp(tr->name, "H4_UART") == 0 && tr->can_send_packet_now(0) == 0);
    CHECK(tr->set_baudrate(230400) == 0 && hdl.baudrate == 230400);
    CHECK(tr->send_packet(1, (const u8[]){0x55,0x66}, 2) == 0);
    CHECK(h4_test_tx_len == 2 && h4_test_tx_bytes[0] == 0x55);
    hdl.device_name = (const char *)1;
    struct { u32 type; u32 baud_init; u32 baud_main; int flow; const char *name; } config = {0, 115200, 230400, 1, "uart-test"};
    uart_init(&config);
    CHECK(hdl.baudrate == 115200 && hdl.flowcontrol == 1 && hdl.device_name == config.name && hdl.thread.fun == 0);
    CHECK(h4_test_last_level == 2 && strcmp(h4_test_last_log, "[Info]: [H4 UART]Baudrate : %d\r\n") == 0);
    h4_test_hdl = &hdl; hdl.baudrate = 115200; config_dut_protocol_mode = 0;
    unsigned creates = h4_test_create, inits = h4_test_init;
    CHECK(tr->open() == 0 && h4_test_create == creates + 1 && h4_test_created_thread == h4_uart_thread_process);
    CHECK(h4_test_init == inits + 1 && h4_test_init_baud == 460800);
    CHECK(tr->close() == 0 && h4_test_delete == 1 && h4_test_close == 1 && hdl.thread.fun == 0);
    config_dut_protocol_mode = 1; hdl.baudrate = 115200; inits = h4_test_init;
    CHECK(tr->open() == 0 && h4_test_created_thread == h4_two_wire_uart_thread_process && h4_test_init == inits + 1 && h4_test_init_baud == 460800);
    CHECK(tr->close() == 0 && h4_test_delete == 2 && h4_test_close == 2);
    h4_uart_thread_resume(); CHECK(h4_test_resume == 1);
    /* Type 5 shares the ACL length path but retains its own delivered type. */
    __data_reset();
    int packets_before_iso = h4_test_packets;
    __data_push_more((char[]){5,0,0,1,0,(char)0xab}, 6);
    h4_uart_thread_process((struct thread *)0);
    CHECK(h4_test_packets == packets_before_iso + 1 && h4_test_packet_type == 5);
    CHECK(h4_test_packet_len == 5 && h4_test_packet[4] == 0xab);

    /* The timeout clear can call a producer through puts. The two-wire
     * equality test uses live count 2, but the pop length uses old count 6. */
    __data_reset();
    __data_push_more((char[]){1,2,3,4,5,6}, 6);
    reset_timeout = 500001; h4_test_refill_on_clear = 1;
    int packets_before_stale = h4_test_packets;
    h4_two_wire_uart_thread_process((struct thread *)0);
    CHECK(h4_test_packets == packets_before_stale + 1 && h4_test_packet_len == 6);
    CHECK(h4_test_packet[0] == 0x61 && h4_test_packet[1] == 0x62);
    CHECK(uart_rx_t.cnt == 0 && h4_test_refill_on_clear == 0);

    config_dut_protocol_mode = 0; h4_test_tx_len = 0;
    CHECK(tr->send_packet(0x12345605, (const u8[]){0xcc,0xdd}, 2) == 0);
    CHECK(h4_test_tx_len == 3 && h4_test_tx_bytes[0] == 5 &&
          h4_test_tx_bytes[1] == 0xcc && h4_test_tx_bytes[2] == 0xdd);
    return TEST_DONE("btctrler_h4_uart_interface_loop_bd19");
}
int main(void) { return test_h4_uart_interface_loop(); }
#endif
