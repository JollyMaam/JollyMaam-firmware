/* BD19 H4 UART hardware path recovered from h4_uart_hw.c.o occurrence 1. */
#include "btctrler/glue.h"
#include <string.h>

#define UART_CODE __attribute__((section(".hci_interface_code")))
#define UART_DATA __attribute__((section(".hci_interface_bss"), aligned(1)))

#ifdef UNITTEST
#include "test.h"
static u32 uart_test_regs[12];
static unsigned uart_test_reg_reads[12], uart_test_reg_writes[12];
static u32 uart_test_clk;
static u8 uart_test_irq, uart_test_irq_priority, uart_test_irq_flags;
static void (*uart_test_irq_fn)(void);
static unsigned uart_test_reset_count, uart_test_push_count, uart_test_resume_count;
static char uart_test_order[64];
static unsigned uart_test_order_len;
static u16 uart_test_rx_len;
static u32 uart_test_con0_script[16];
static unsigned uart_test_con0_script_count, uart_test_con0_script_pos;
static int uart_test_mutate_clock_on_gpio, uart_test_rearm_before_push;
static int uart_test_gpio_direction_output(u32, int);
static int uart_test_gpio_set_fun_output_port(u32, u32, u8, u8);
static int uart_test_gpio_direction_input(u32);
static int uart_test_gpio_set_pull_up(u32, int);
static int uart_test_gpio_set_die(u32, int);
static int uart_test_gpio_set_fun_input_port(u32, u32);
static int uart_test_clk_get(const char *);
static void uart_test_request_irq(u8, u8, void (*)(void), u8);
static void uart_test_data_reset(void);
static void uart_test_data_push_more(char *, u16);
static void uart_test_resume(void);
#define gpio_direction_output uart_test_gpio_direction_output
#define gpio_set_fun_output_port uart_test_gpio_set_fun_output_port
#define gpio_direction_input uart_test_gpio_direction_input
#define gpio_set_pull_up uart_test_gpio_set_pull_up
#define gpio_set_die uart_test_gpio_set_die
#define gpio_set_fun_input_port uart_test_gpio_set_fun_input_port
#define clk_get uart_test_clk_get
#define request_irq uart_test_request_irq
#define __data_reset uart_test_data_reset
#define __data_push_more uart_test_data_push_more
#define h4_uart_thread_resume uart_test_resume
static void uart_test_event(char c)
{
    CHECK(uart_test_order_len < sizeof uart_test_order);
    uart_test_order[uart_test_order_len++] = c;
}
static u32 uart_test_read(u32 addr)
{
    unsigned i = (addr - 1974528u) / 4;
    CHECK(addr >= 1974528u && i < 12);
    ++uart_test_reg_reads[i]; uart_test_event('r');
    if (addr == 1974528u && uart_test_con0_script_pos < uart_test_con0_script_count)
        return uart_test_con0_script[uart_test_con0_script_pos++];
    return uart_test_regs[i];
}
static void uart_test_write(u32 addr, u32 value)
{
    unsigned i = (addr - 1974528u) / 4;
    CHECK(addr >= 1974528u && i < 12);
    ++uart_test_reg_writes[i]; uart_test_regs[i] = value; uart_test_event('w');
}
#define UART_REG(addr) uart_test_read((u32)(addr))
#define UART_SET(addr, value) uart_test_write((u32)(addr), (u32)(value))
#define UART_SYNC() uart_test_event('s')
#define UART_IRQ_ATTR
#else
#define UART_REG(addr) (*(volatile u32 *)(uintptr_t)(addr))
#define UART_SET(addr, value) (UART_REG(addr) = (u32)(value))
#define UART_SYNC() __asm__ volatile("csync" :::)
#define UART_IRQ_ATTR __attribute__((interrupt))
#endif

enum {
    UART1_BASE = 1974528,
    UART_CON0 = UART1_BASE + 0,
    UART_BAUD = UART1_BASE + 8,
    UART_BUF = UART1_BASE + 12,
    UART_OTCNT = UART1_BASE + 16,
    UART_RXSADR = UART1_BASE + 28,
    UART_RXEADR = UART1_BASE + 32,
    UART_RXCNT = UART1_BASE + 36,
    UART_HRXCNT = UART1_BASE + 40,
};

static u8 uart_rbuf[64] UART_DATA;
static void uart_irq_handle(void) UART_IRQ_ATTR;

extern int gpio_direction_output(u32 gpio, int value);
extern int gpio_set_fun_output_port(u32 gpio, u32 fun, u8 dir, u8 pull);
extern int gpio_direction_input(u32 gpio);
extern int gpio_set_pull_up(u32 gpio, int value);
extern int gpio_set_die(u32 gpio, int value);
extern int gpio_set_fun_input_port(u32 gpio, u32 fun);
extern int clk_get(const char *name);
extern void request_irq(u8 irq, u8 priority, void (*handler)(void), u8 flags);
extern void __data_reset(void);
extern void __data_push_more(char *data, u16 len);
extern void h4_uart_thread_resume(void);

void __uart_tx(u8 *buf, u16 len) UART_CODE;
void __uart_tx(u8 *buf, u16 len)
{
    while (len) {
        UART_SET(UART_BUF, *buf);
        UART_SYNC();
        while ((int16_t)UART_REG(UART_CON0) >= 0) { }
        UART_SET(UART_CON0, UART_REG(UART_CON0) | 8192u);
        --len;
        ++buf;
    }
}

__attribute__((weak)) void h4_uart1_io_init_for_override(u32 tx_io, u32 rx_io) UART_CODE;
__attribute__((weak)) void h4_uart1_io_init_for_override(u32 tx_io, u32 rx_io)
{
    (void)gpio_direction_output(tx_io, 1);
    (void)gpio_set_fun_output_port(tx_io, 44, 1, 1);
    (void)gpio_direction_input(rx_io);
    (void)gpio_set_pull_up(rx_io, 1);
    (void)gpio_set_die(rx_io, 1);
    (void)gpio_set_fun_input_port(rx_io, 1988520);
}

void h4_uart_init(uint32_t baudrate) UART_CODE;
void h4_uart_init(uint32_t baudrate)
{
    int uart_clk = clk_get("uart");
    h4_uart1_io_init_for_override(49, 50);
    UART_SET(UART_BAUD, ((u32)uart_clk / baudrate >> 2) - 1u);
    UART_SET(UART_CON0, 12288);
    __data_reset();
    request_irq(19, 3, uart_irq_handle, 0);
    __builtin_memset(uart_rbuf, 0, sizeof uart_rbuf);
    UART_SET(UART_RXSADR, (u32)(uintptr_t)uart_rbuf);
    UART_SET(UART_RXEADR, (u32)(uintptr_t)(uart_rbuf + sizeof uart_rbuf));
    UART_SET(UART_RXCNT, 64);
    UART_SET(UART_OTCNT, (u32)(uart_clk / 1000));
    UART_SET(UART_CON0, UART_REG(UART_CON0) | 32u);
    UART_SET(UART_CON0, UART_REG(UART_CON0) | 64u);
    UART_SET(UART_CON0, UART_REG(UART_CON0) | 2u);
    UART_SET(UART_CON0, UART_REG(UART_CON0) | 11u);
}

static void uart_irq_handle(void) UART_CODE UART_IRQ_ATTR;
static void uart_irq_handle(void)
{
    if ((UART_REG(UART_CON0) & 32u) == 0) return;
    if ((UART_REG(UART_CON0) & 2048u) == 0) return;
    UART_SET(UART_CON0, UART_REG(UART_CON0) | 128u);
    UART_SET(UART_CON0, UART_REG(UART_CON0) | 1024u);
    UART_SET(UART_CON0, UART_REG(UART_CON0) | 4096u);
    UART_SYNC();
    UART_SYNC();
    u16 uart_rlen = (u16)UART_REG(UART_HRXCNT);
    __data_push_more((char *)uart_rbuf, uart_rlen);
    UART_SET(UART_RXSADR, (u32)(uintptr_t)uart_rbuf);
    UART_SET(UART_RXEADR, (u32)(uintptr_t)(uart_rbuf + sizeof uart_rbuf));
    UART_SET(UART_RXCNT, 64);
    h4_uart_thread_resume();
}

void h4_uart_close(void) UART_CODE;
void h4_uart_close(void) { UART_SET(UART_CON0, 0); }

#ifdef UNITTEST
static int uart_test_gpio_direction_output(u32 gpio, int value)
{ CHECK(gpio == 49 && value == 1); uart_test_event('a'); if (uart_test_mutate_clock_on_gpio) uart_test_clk = 123; return 0; }
static int uart_test_gpio_set_fun_output_port(u32 gpio, u32 fun, u8 dir, u8 pull)
{ CHECK(gpio == 49 && fun == 44 && dir == 1 && pull == 1); uart_test_event('b'); return 0; }
static int uart_test_gpio_direction_input(u32 gpio)
{ CHECK(gpio == 50); uart_test_event('c'); return 0; }
static int uart_test_gpio_set_pull_up(u32 gpio, int value)
{ CHECK(gpio == 50 && value == 1); uart_test_event('d'); return 0; }
static int uart_test_gpio_set_die(u32 gpio, int value)
{ CHECK(gpio == 50 && value == 1); uart_test_event('e'); return 0; }
static int uart_test_gpio_set_fun_input_port(u32 gpio, u32 fun)
{ CHECK(gpio == 50 && fun == 1988520); uart_test_event('f'); return 0; }
static int uart_test_clk_get(const char *name) { CHECK(strcmp(name, "uart") == 0); uart_test_event('g'); return (int)uart_test_clk; }
static void uart_test_request_irq(u8 irq, u8 priority, void (*handler)(void), u8 flags)
{ uart_test_irq = irq; uart_test_irq_priority = priority; uart_test_irq_fn = handler; uart_test_irq_flags = flags; uart_test_event('i'); }
static void uart_test_data_reset(void) { ++uart_test_reset_count; uart_test_event('x'); }
static void uart_test_data_push_more(char *data, u16 len)
{
    CHECK(data == (char *)uart_rbuf);
    if (uart_test_push_count == 0)
        uart_test_rearm_before_push = uart_test_regs[7] == 11 && uart_test_regs[8] == 22 && uart_test_regs[9] == 33;
    uart_test_rx_len = len; ++uart_test_push_count; uart_test_event('p');
}
static void uart_test_resume(void) { ++uart_test_resume_count; uart_test_event('t'); }

void test_h4_uart_hw(void)
{
    u8 bytes[] = { 0x21, 0x00, 0xfe };
    memset(uart_test_regs, 0, sizeof uart_test_regs);
    memset(uart_test_reg_reads, 0, sizeof uart_test_reg_reads);
    memset(uart_test_reg_writes, 0, sizeof uart_test_reg_writes);
    uart_test_order_len = uart_test_reset_count = uart_test_push_count = uart_test_resume_count = 0;
    h4_uart1_io_init_for_override(49, 50);
    CHECK(uart_test_order_len == 6 && memcmp(uart_test_order, "abcdef", 6) == 0);
    __uart_tx(bytes, 0);
    CHECK(uart_test_reg_writes[3] == 0 && uart_test_reg_reads[0] == 0);
    uart_test_order_len = 0; uart_test_clk = 48000000; uart_test_mutate_clock_on_gpio = 1;
    uart_test_regs[0] = 0x8000;
    h4_uart_init(115200);
    uart_test_mutate_clock_on_gpio = 0;
    CHECK(uart_test_order[0] == 'g' && uart_test_irq == 19 && uart_test_irq_priority == 3 && uart_test_irq_flags == 0);
    CHECK(uart_test_irq_fn == uart_irq_handle && uart_test_reset_count == 1);
    CHECK(uart_test_regs[2] == (((48000000u / 115200u) >> 2) - 1u));
    CHECK(uart_test_regs[4] == 48000 && uart_test_regs[9] == 64);
    CHECK(uart_test_regs[0] == (12288u | 32u | 64u | 2u | 11u));
    /* The baud divisor is unsigned, while OTCNT divides the same clock as signed. */
    uart_test_clk = (u32)-4000;
    h4_uart_init(1000);
    CHECK(uart_test_regs[2] == 1073739u && uart_test_regs[4] == 0xfffffffcu);
    memset(uart_test_reg_reads, 0, sizeof uart_test_reg_reads);
    memset(uart_test_reg_writes, 0, sizeof uart_test_reg_writes);
    uart_test_order_len = 0; uart_test_regs[0] = 0x8000;
    __uart_tx(bytes, sizeof bytes);
    CHECK(uart_test_reg_writes[3] == 3 && uart_test_regs[3] == 0xfe);
    CHECK(uart_test_reg_writes[0] == 3 && uart_test_regs[0] == 0xa000);
    CHECK(uart_test_reg_reads[0] == 6);
    memset(uart_test_reg_reads, 0, sizeof uart_test_reg_reads);
    memset(uart_test_reg_writes, 0, sizeof uart_test_reg_writes);
    uart_test_con0_script[0] = 0;
    uart_test_con0_script[1] = 0x8000;
    uart_test_con0_script[2] = 0x8000;
    uart_test_con0_script_count = 3; uart_test_con0_script_pos = 0;
    __uart_tx(bytes, 1);
    uart_test_con0_script_count = uart_test_con0_script_pos = 0;
    CHECK(uart_test_reg_reads[0] == 3 && uart_test_regs[0] == 0xa000 && uart_test_regs[3] == 0x21);
    memset(uart_test_reg_reads, 0, sizeof uart_test_reg_reads);
    memset(uart_test_reg_writes, 0, sizeof uart_test_reg_writes);
    uart_test_order_len = 0; uart_test_regs[0] = 32 | 2048;
    uart_test_regs[7] = 11; uart_test_regs[8] = 22; uart_test_regs[9] = 33;
    uart_test_regs[10] = 0x10040; uart_irq_handle();
    CHECK(uart_test_push_count == 1 && uart_test_rx_len == 64 && uart_test_resume_count == 1);
    CHECK(uart_test_rearm_before_push);
    CHECK(uart_test_reg_writes[0] == 3 && uart_test_regs[0] == (32u | 2048u | 128u | 1024u | 4096u));
    CHECK(uart_test_regs[7] == (u32)(uintptr_t)uart_rbuf);
    CHECK(uart_test_regs[8] == (u32)(uintptr_t)(uart_rbuf + 64) && uart_test_regs[9] == 64);
    uart_test_regs[0] = 32; uart_irq_handle();
    CHECK(uart_test_push_count == 1 && uart_test_resume_count == 1);
    uart_test_regs[0] = 0; uart_irq_handle();
    CHECK(uart_test_push_count == 1);
    h4_uart_close(); CHECK(uart_test_regs[0] == 0);
}

int main(void)
{
    test_h4_uart_hw();
    return TEST_DONE("btctrler_h4_uart_hw_bd19");
}
#endif
