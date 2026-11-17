/* BR23 RF configuration and clock/power sequencing.  Preserve the SDK's
 * direct MMIO read-modify-write order, including its repeated reads. */
#include "btctrler/glue.h"
#ifdef UNITTEST
#include "test.h"
#endif
#include <string.h>

#define RF_CODE __attribute__((section(".bt_rf_code")))

extern const u8 log_tag_const_i_RF;
void log_print(int level, void *file, const char *format, ...);
void P33_CON_SET(u16 addr, u8 start, u8 len, u8 data);
void delay(u32 usec);
void bt_sfr_pop(void);
u8 low_power_get_osc_type(void);
u32 low_power_trace_drift(u32 usec);

#ifdef UNITTEST
/* The SDK's log tag: off.  Unresolved, the host link put it at address 0. */
const u8 log_tag_const_i_RF = 0;
static u32 rf_test_mmio_addr[64];
static u32 rf_test_mmio_value[64];
static unsigned rf_test_mmio_count;
struct rf_test_access { char op; u32 address; u32 value; };
static struct rf_test_access rf_test_accesses[256];
static unsigned rf_test_access_count;
static struct rf_test_access rf_test_order[256];
static unsigned rf_test_order_count;
static u8 rf_test_mutate_delay_sys;
static void rf_test_order_event(char op, u32 address, u32 value)
{
    CHECK(rf_test_order_count < sizeof(rf_test_order) / sizeof(rf_test_order[0]));
    rf_test_order[rf_test_order_count++] = (struct rf_test_access){ op, address, value };
}

static unsigned rf_test_index(u32 address)
{
    for (unsigned i = 0; i < rf_test_mmio_count; i++)
        if (rf_test_mmio_addr[i] == address) return i;
    CHECK(rf_test_mmio_count < sizeof(rf_test_mmio_addr) / sizeof(rf_test_mmio_addr[0]));
    unsigned i = rf_test_mmio_count++;
    rf_test_mmio_addr[i] = address;
    rf_test_mmio_value[i] = 0;
    return i;
}

static u32 rf_test_read32(u32 address)
{
    unsigned i = rf_test_index(address);
    CHECK(rf_test_access_count < sizeof(rf_test_accesses) / sizeof(rf_test_accesses[0]));
    rf_test_accesses[rf_test_access_count++] = (struct rf_test_access){ 'R', address, rf_test_mmio_value[i] };
    rf_test_order_event('R', address, rf_test_mmio_value[i]);
    return rf_test_mmio_value[i];
}

static void rf_test_write32(u32 address, u32 value)
{
    unsigned i = rf_test_index(address);
    CHECK(rf_test_access_count < sizeof(rf_test_accesses) / sizeof(rf_test_accesses[0]));
    rf_test_accesses[rf_test_access_count++] = (struct rf_test_access){ 'W', address, value };
    rf_test_order_event('W', address, value);
    rf_test_mmio_value[i] = value;
}

#define RF_READ32(address) rf_test_read32((u32)(address))
#define RF_WRITE32(address, value) rf_test_write32((u32)(address), (u32)(value))
#else
#define RF_READ32(address) (*(volatile u32 *)(uintptr_t)(address))
#define RF_WRITE32(address, value) (*(volatile u32 *)(uintptr_t)(address) = (u32)(value))
#endif

struct common_rf_param rf __attribute__((section(".bt_rf_data"))) = {
    .bredr_fix_pwr = 0xff,
    .ble_fix_pwr = 0xff,
    .ldos = 3,
    .fdv21 = 55,
    .fdv40 = 85,
    .fdv50 = 85,
};
static u8 bt_rf_pt_flag __attribute__((section(".bt_rf_data")));

RF_CODE void rf_bredr_para_prf(void)
{
    if (log_tag_const_i_RF)
        log_print(2, NULL, "[Info]: [RF]bredr_max_pwr:%x\n\r\n", rf.bredr_pwr_max);
}

RF_CODE void bt_production_test(u8 en)
{
    rf.production_test = en ? 1 : 0;
    P33_CON_SET(6, 0, 3, en ? 7 : 5);
}

RF_CODE void bt_max_pwr_set(u8 pwr, u8 pg_pwr, u8 iq_pwr, u8 ble_pwr)
{
    /* The SDK combines the four power nibbles in one aligned halfword store. */
    u8 page_and_power = (u8)((pg_pwr << 4) | (pwr & 15));
    u16 value = (u16)page_and_power | (u16)((iq_pwr & 15) << 8) |
                (u16)(ble_pwr << 12);
    memcpy(&rf, &value, sizeof(value));
}

RF_CODE void bt_set_fcc_dut_large_power(u8 power)
{
    (void)power;
}

RF_CODE void bredr_set_fix_pwr(u8 fix)
{
    rf.bredr_fix_pwr = fix;
    bredr_link_reset_power();
}

RF_CODE void ble_set_fix_pwr(u8 fix)
{
    rf.ble_fix_pwr = fix;
    le_hw_reset_power();
}

RF_CODE void bt_set_rxtx_status_enable(u8 en)
{
    rf.rxtx_status = en;
}

RF_CODE void bt_set_ldos(u8 mode)
{
    rf.ldos = mode ? 3 : 2;
}

RF_CODE void bt_set_fdv(u8 fdv21, u8 fdv40, u8 fdv50)
{
    rf.fdv21 = fdv21;
    rf.fdv40 = fdv40;
    rf.fdv50 = fdv50;
}

RF_CODE void bt_pll_para(u32 osc, u32 sys, u8 low_power, u8 xosc)
{
    (void)low_power;
    (void)xosc;
    rf.delay_sys = (u8)(sys / 1000000);
    rf.pll_para = (u8)(osc / 2000000 + 254);
}

RF_CODE void bt_pll_reset(u8 mode)
{
    u32 value = RF_READ32(0x1e1924);
    if (!mode) {
        RF_WRITE32(0x1e1924, value & ~128u);
        value = RF_READ32(0x1e1924);
        RF_WRITE32(0x1e1924, value & ~32u);
        value = RF_READ32(0x1e1924);
        RF_WRITE32(0x1e1924, value & ~64u);
        value = RF_READ32(0x1e1950);
        RF_WRITE32(0x1e1950, value & ~15u);
    } else {
        RF_WRITE32(0x1e1924, value | 128u);
        value = RF_READ32(0x1e1924);
        RF_WRITE32(0x1e1924, value | 32u);
        value = RF_READ32(0x1e1924);
        RF_WRITE32(0x1e1924, value | 64u);
        value = RF_READ32(0x1e1950);
        RF_WRITE32(0x1e1950, value | 14u);
    }
}

RF_CODE void RF_poweroff_recover(void)
{
    u32 value = RF_READ32(0x1f1800);
    RF_WRITE32(0x1f1800, value | 128u);
    value = RF_READ32(0x1f1800);
    RF_WRITE32(0x1f1800, (value & ~2031616u) | 1048576u);
    value = RF_READ32(0x1f1800);
    RF_WRITE32(0x1f1800, value & ~1u);
    delay(100);
    value = RF_READ32(0x1f1800);
    RF_WRITE32(0x1f1800, value | 1u);
    bt_sfr_pop();
}

RF_CODE void bt_rf_init(void)
{
    if (log_tag_const_i_RF)
        log_print(2, NULL, "[Info]: [RF]>>>>>>>>>>>>>>RF_init\r\n");

    u32 value = RF_READ32(0x1e0010);
    RF_WRITE32(0x1e0010, value & ~49152u);
    value = RF_READ32(0x1e0010);
    RF_WRITE32(0x1e0010, value & ~786432u);
    value = RF_READ32(0x1e0010);
    RF_WRITE32(0x1e0010, value & ~3145728u);
    delay((u32)rf.delay_sys * 10);

    value = RF_READ32(0x1f1800);
    RF_WRITE32(0x1f1800, value | 128u);
    value = RF_READ32(0x1f1800);
    RF_WRITE32(0x1f1800, (value & ~2031616u) | 786432u);
    delay((u32)rf.delay_sys * 10);
    value = RF_READ32(0x1f1800);
    RF_WRITE32(0x1f1800, value | 1u);
    delay((u32)rf.delay_sys * 10);

    value = RF_READ32(0x1cfc40);
    RF_WRITE32(0x1cfc40, value | 1024u);
    value = RF_READ32(0x1c0000);
    RF_WRITE32(0x1c0000, value | 0xc0000000u);
    delay((u32)rf.delay_sys * 10);
    RF_WRITE32(0x1cfc40, 65535);
    value = RF_READ32(0x1cfc40);
    RF_WRITE32(0x1cfc40, value & ~770u);
    RF_WRITE32(0x1cfc78, 15);
    value = RF_READ32(0x1cfc78);
    RF_WRITE32(0x1cfc78, value | 496u);
    RF_mdm_init();
    bt_rf_analog_init();
    value = RF_READ32(0x1c0000);
    RF_WRITE32(0x1c0000, value | 0xc0000000u);
    delay((u32)rf.delay_sys * 10);

    if (rf.rxtx_status) {
        value = RF_READ32(0x1f1800);
        RF_WRITE32(0x1f1800, value | 8u);
        value = RF_READ32(0x1f1800);
        RF_WRITE32(0x1f1800, value | 16u);
        value = RF_READ32(0x1e5008);
        RF_WRITE32(0x1e5008, value & ~1536u);
    }
    rf.start = 1;
}

RF_CODE void bt_rf_close(void)
{
    if (!rf.start) return;
    rf.start = 0;
    u32 value = RF_READ32(0x1f1800);
    RF_WRITE32(0x1f1800, value & ~31u);
    delay(1000);
    RF_WRITE32(0x1f1800, 0);
    delay(1000);
    value = RF_READ32(0x1e0010);
    RF_WRITE32(0x1e0010, value | 49152u);
    delay(1000);
}

RF_CODE void bt_clk_close(u8 mode, u8 flag)
{
    (void)mode;
    (void)flag;
    if (rf.start) {
        rf.start = 0;
        u32 value = RF_READ32(0x1f1800);
        RF_WRITE32(0x1f1800, (value & ~2031616u) | 786432u);
        value = RF_READ32(0x1f1800);
        RF_WRITE32(0x1f1800, value | 3145728u);
        value = RF_READ32(0x1f1800);
        RF_WRITE32(0x1f1800, value & ~1u);
        value = RF_READ32(0x1f1800);
        if ((value & 3u) == 0) {
            value = RF_READ32(0x1f1800);
            RF_WRITE32(0x1f1800, value & ~128u);
        }
    }
}

RF_CODE void bt_process_before_init(void) { }

RF_CODE u32 bt_get_fine_cnt(u32 usec)
{
    u32 fine = 0;
    if (low_power_get_osc_type() == 0)
        fine = low_power_trace_drift(usec);
    u32 value = RF_READ32(0x1cfc6c);
    return fine + usec + ((value & 2047u) / 12u);
}

RF_CODE int bt_rf_protect(void *priv, u32 protect)
{
    local_irq_disable();
    if (!bt_rf_pt_flag) {
        if (protect) {
            u8 value = (u8)protect;
            bt_edr_prio_settings(priv, value);
            bt_rf_pt_flag = value;
        }
    } else if (!protect) {
        bt_edr_prio_settings(priv, 0);
        bt_rf_pt_flag = 0;
    }
    local_irq_enable();
    return 0;
}

/* sdk.h describes this as bool, but retained SDK callers use zeroext i8.
 * Preserve that byte-return ABI so LTO can fold this SDK no-op. */
u8 bt_bredr_is_sniff_ir(void) __asm__("bt_bredr_is_sniff") RF_CODE;
RF_CODE u8 bt_bredr_is_sniff_ir(void) { return 0; }

#ifdef UNITTEST
static unsigned rf_test_delay_count, rf_test_last_delay, rf_test_sfr_pops;
static unsigned rf_test_irq_depth, rf_test_priority_calls;
static u8 rf_test_priority;
static u8 rf_test_osc_type;
static u32 rf_test_drift;

void delay(u32 usec)
{
    rf_test_delay_count++;
    rf_test_last_delay = usec;
    rf_test_order_event('D', 0, usec);
    if (rf_test_mutate_delay_sys) rf.delay_sys++;
}
void bt_sfr_pop(void) { rf_test_sfr_pops++; }
void RF_mdm_init(void) { rf_test_order_event('M', 0, 0); }
void bt_rf_analog_init(void) { rf_test_order_event('A', 0, 0); }
void P33_CON_SET(u16 addr, u8 start, u8 len, u8 data)
{ (void)addr; (void)start; (void)len; (void)data; }
void bredr_link_reset_power(void) { }
void le_hw_reset_power(void) { }
u8 low_power_get_osc_type(void) { return rf_test_osc_type; }
u32 low_power_trace_drift(u32 usec) { (void)usec; return rf_test_drift; }
void bt_edr_prio_settings(void *priv, u8 protect)
{ (void)priv; rf_test_priority_calls++; rf_test_priority = protect; }
void local_irq_disable(void) { rf_test_irq_depth++; }
void local_irq_enable(void) { rf_test_irq_depth--; }
void log_print(int level, void *file, const char *format, ...)
{ (void)level; (void)file; (void)format; }

static void rf_test_reset(void)
{
    rf_test_mmio_count = rf_test_access_count = 0;
    rf_test_order_count = 0;
    rf_test_delay_count = rf_test_last_delay = rf_test_sfr_pops = 0;
    rf_test_mutate_delay_sys = 0;
}

static void rf_test_expect(unsigned index, char op, u32 address, u32 value)
{
    CHECK(index < rf_test_access_count);
    CHECK(rf_test_accesses[index].op == op);
    CHECK(rf_test_accesses[index].address == address);
    CHECK(rf_test_accesses[index].value == value);
}

static void rf_test_order_expect(unsigned index, char op, u32 address, u32 value)
{
    CHECK(index < rf_test_order_count);
    CHECK(rf_test_order[index].op == op);
    CHECK(rf_test_order[index].address == address);
    CHECK(rf_test_order[index].value == value);
}

int main(void)
{
    CHECK(rf.bredr_fix_pwr == 0xff && rf.ble_fix_pwr == 0xff);
    CHECK(rf.ldos == 3 && rf.fdv21 == 55 && rf.fdv40 == 85 && rf.fdv50 == 85);
    bt_max_pwr_set(0x1a, 0x2b, 0x3c, 0x4d);
    CHECK(rf.bredr_pwr_max == 10 && rf.bredr_page_pwr_max == 11);
    CHECK(rf.bredr_inquiry_pwr_max == 12 && rf.ble_pwr_max == 13);
    bt_pll_para(24000000, 16000000, 0xff, 0xff);
    CHECK(rf.delay_sys == 16 && rf.pll_para == 10);

    /* The delay field is reloaded after each external delay call.  The test
     * delay helper mutates it so a cached C local would change later values. */
    rf_test_reset();
    rf.delay_sys = 2;
    rf.rxtx_status = 1;
    rf_test_mutate_delay_sys = 1;
    bt_rf_init();
    CHECK(rf.start == 1 && rf_test_delay_count == 5);
    CHECK(rf_test_order_count == 37);
    rf_test_order_expect(0, 'R', 0x1e0010, 0);
    rf_test_order_expect(1, 'W', 0x1e0010, 0);
    rf_test_order_expect(2, 'R', 0x1e0010, 0);
    rf_test_order_expect(3, 'W', 0x1e0010, 0);
    rf_test_order_expect(4, 'R', 0x1e0010, 0);
    rf_test_order_expect(5, 'W', 0x1e0010, 0);
    rf_test_order_expect(6, 'D', 0, 20);
    rf_test_order_expect(7, 'R', 0x1f1800, 0);
    rf_test_order_expect(8, 'W', 0x1f1800, 128);
    rf_test_order_expect(9, 'R', 0x1f1800, 128);
    rf_test_order_expect(10, 'W', 0x1f1800, 786560);
    rf_test_order_expect(11, 'D', 0, 30);
    rf_test_order_expect(12, 'R', 0x1f1800, 786560);
    rf_test_order_expect(13, 'W', 0x1f1800, 786561);
    rf_test_order_expect(14, 'D', 0, 40);
    rf_test_order_expect(15, 'R', 0x1cfc40, 0);
    rf_test_order_expect(16, 'W', 0x1cfc40, 1024);
    rf_test_order_expect(17, 'R', 0x1c0000, 0);
    rf_test_order_expect(18, 'W', 0x1c0000, 0xc0000000u);
    rf_test_order_expect(19, 'D', 0, 50);
    rf_test_order_expect(20, 'W', 0x1cfc40, 65535);
    rf_test_order_expect(21, 'R', 0x1cfc40, 65535);
    rf_test_order_expect(22, 'W', 0x1cfc40, 64765);
    rf_test_order_expect(23, 'W', 0x1cfc78, 15);
    rf_test_order_expect(24, 'R', 0x1cfc78, 15);
    rf_test_order_expect(25, 'W', 0x1cfc78, 511);
    rf_test_order_expect(26, 'M', 0, 0);
    rf_test_order_expect(27, 'A', 0, 0);
    rf_test_order_expect(28, 'R', 0x1c0000, 0xc0000000u);
    rf_test_order_expect(29, 'W', 0x1c0000, 0xc0000000u);
    rf_test_order_expect(30, 'D', 0, 60);
    rf_test_order_expect(31, 'R', 0x1f1800, 786561);
    rf_test_order_expect(32, 'W', 0x1f1800, 786569);
    rf_test_order_expect(33, 'R', 0x1f1800, 786569);
    rf_test_order_expect(34, 'W', 0x1f1800, 786585);
    rf_test_order_expect(35, 'R', 0x1e5008, 0);
    rf_test_order_expect(36, 'W', 0x1e5008, 0);

    rf_test_reset();
    rf_test_mmio_value[rf_test_index(0x1e1924)] = 0xffffffffu;
    rf_test_mmio_value[rf_test_index(0x1e1950)] = 0xffffffffu;
    bt_pll_reset(0);
    CHECK(rf_test_access_count == 8);
    rf_test_expect(0, 'R', 0x1e1924, 0xffffffffu);
    rf_test_expect(1, 'W', 0x1e1924, 0xffffff7fu);
    rf_test_expect(2, 'R', 0x1e1924, 0xffffff7fu);
    rf_test_expect(3, 'W', 0x1e1924, 0xffffff5fu);
    rf_test_expect(4, 'R', 0x1e1924, 0xffffff5fu);
    rf_test_expect(5, 'W', 0x1e1924, 0xffffff1fu);
    rf_test_expect(6, 'R', 0x1e1950, 0xffffffffu);
    rf_test_expect(7, 'W', 0x1e1950, 0xfffffff0u);

    rf_test_reset();
    rf_test_mmio_value[rf_test_index(0x1e1924)] = 0;
    rf_test_mmio_value[rf_test_index(0x1e1950)] = 0;
    bt_pll_reset(1);
    CHECK(rf_test_access_count == 8);
    rf_test_expect(0, 'R', 0x1e1924, 0);
    rf_test_expect(1, 'W', 0x1e1924, 128);
    rf_test_expect(3, 'W', 0x1e1924, 160);
    rf_test_expect(5, 'W', 0x1e1924, 224);
    rf_test_expect(7, 'W', 0x1e1950, 14);

    rf_test_reset();
    rf_test_mmio_value[rf_test_index(0x1f1800)] = 0x12345678;
    RF_poweroff_recover();
    CHECK(rf_test_access_count == 8 && rf_test_delay_count == 1);
    rf_test_expect(0, 'R', 0x1f1800, 0x12345678);
    rf_test_expect(1, 'W', 0x1f1800, 0x123456f8);
    rf_test_expect(2, 'R', 0x1f1800, 0x123456f8);
    rf_test_expect(3, 'W', 0x1f1800, 0x123056f8);
    rf_test_expect(4, 'R', 0x1f1800, 0x123056f8);
    rf_test_expect(5, 'W', 0x1f1800, 0x123056f8 & ~1u);
    rf_test_expect(6, 'R', 0x1f1800, 0x123056f8 & ~1u);
    rf_test_expect(7, 'W', 0x1f1800, 0x123056f9);
    CHECK(rf_test_last_delay == 100 && rf_test_sfr_pops == 1);

    rf_test_reset();
    rf.start = 0;
    bt_rf_close();
    CHECK(rf_test_access_count == 0);
    rf.start = 1;
    rf_test_reset();
    rf_test_mmio_value[rf_test_index(0x1f1800)] = 0xffffffffu;
    rf_test_mmio_value[rf_test_index(0x1e0010)] = 0;
    bt_rf_close();
    CHECK(!rf.start && rf_test_access_count == 5 && rf_test_delay_count == 3);
    rf_test_expect(0, 'R', 0x1f1800, 0xffffffffu);
    rf_test_expect(1, 'W', 0x1f1800, 0xffffffe0u);
    rf_test_expect(2, 'W', 0x1f1800, 0);
    rf_test_expect(3, 'R', 0x1e0010, 0);
    rf_test_expect(4, 'W', 0x1e0010, 49152);

    /* SDK clock close rereads the control register before the low-bit test,
     * and again before clearing bit 7. Both branches keep their read count. */
    rf_test_reset(); rf.start = 0;
    bt_clk_close(255, 255);
    CHECK(rf_test_access_count == 0);
    rf_test_reset(); rf.start = 1;
    rf_test_mmio_value[rf_test_index(0x1f1800)] = 0x81;
    bt_clk_close(255, 255);
    CHECK(!rf.start && rf_test_access_count == 9);
    rf_test_expect(1, 'W', 0x1f1800, 0xc0081);
    rf_test_expect(3, 'W', 0x1f1800, 0x3c0081);
    rf_test_expect(5, 'W', 0x1f1800, 0x3c0080);
    rf_test_expect(6, 'R', 0x1f1800, 0x3c0080);
    rf_test_expect(7, 'R', 0x1f1800, 0x3c0080);
    rf_test_expect(8, 'W', 0x1f1800, 0x3c0000);
    rf_test_reset(); rf.start = 1;
    rf_test_mmio_value[rf_test_index(0x1f1800)] = 0x83;
    bt_clk_close(0, 0);
    CHECK(!rf.start && rf_test_access_count == 7);
    rf_test_expect(6, 'R', 0x1f1800, 0x3c0082);

    rf_test_reset();
    rf_test_mmio_value[rf_test_index(0x1cfc6c)] = 2047;
    rf_test_osc_type = 0; rf_test_drift = 27;
    CHECK(bt_get_fine_cnt(100) == 27 + 100 + 170);
    CHECK(rf_test_access_count == 1);
    rf_test_expect(0, 'R', 0x1cfc6c, 2047);
    rf_test_reset();
    rf_test_osc_type = 1;
    rf_test_mmio_value[rf_test_index(0x1cfc6c)] = 2047;
    CHECK(bt_get_fine_cnt(100) == 100 + 170);
    CHECK(rf_test_access_count == 1);

    rf_test_priority_calls = 0; rf_test_priority = 0; rf_test_irq_depth = 0;
    CHECK(bt_rf_protect((void *)1, 0x101) == 0);
    CHECK(rf_test_priority_calls == 1 && rf_test_priority == 1 && rf_test_irq_depth == 0);
    CHECK(bt_rf_protect((void *)1, 2) == 0);
    CHECK(rf_test_priority_calls == 1 && rf_test_irq_depth == 0);
    CHECK(bt_rf_protect((void *)1, 0) == 0);
    CHECK(rf_test_priority_calls == 2 && rf_test_priority == 0 && rf_test_irq_depth == 0);
    return TEST_DONE("btctrler_RF");
}
#endif
