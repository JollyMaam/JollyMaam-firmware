/* BD19 RF control member. The BD19 radio has a different parameter record,
 * clock tree, power sequence and power-off save area from BR23; keep this
 * implementation separate so the existing BR23 RF path stays untouched. */
#include "btctrler/glue.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define RF_CODE __attribute__((section(".bt_rf_code")))
#ifdef UNITTEST
#include "test.h"
#include <setjmp.h>
struct rf_model_entry { u32 address, value; };
struct rf_model_access { char op; u32 address, value; };
static struct rf_model_entry rf_model[256];
static struct rf_model_access rf_access[1024];
static unsigned rf_model_count, rf_access_count;
static u8 rf_fake_wla[160], rf_fake_bt_ahb[320], rf_fake_bt_sfr[528];
static u32 rf_model_get(u32 address)
{
    for (unsigned i = 0; i < rf_model_count; ++i)
        if (rf_model[i].address == address) return rf_model[i].value;
    CHECK(rf_model_count < sizeof(rf_model) / sizeof(rf_model[0]));
    rf_model[rf_model_count] = (struct rf_model_entry){address, 0};
    return rf_model[rf_model_count++].value;
}
static void rf_model_set(u32 address, u32 value)
{
    (void)rf_model_get(address);
    for (unsigned i = 0; i < rf_model_count; ++i)
        if (rf_model[i].address == address) { rf_model[i].value = value; break; }
}
static u32 rf_test_read32(u32 address)
{
    u32 value = rf_model_get(address);
    CHECK(rf_access_count < sizeof(rf_access) / sizeof(rf_access[0]));
    rf_access[rf_access_count++] = (struct rf_model_access){'R', address, value};
    return value;
}
static void rf_test_write32(u32 address, u32 value)
{
    CHECK(rf_access_count < sizeof(rf_access) / sizeof(rf_access[0]));
    rf_access[rf_access_count++] = (struct rf_model_access){'W', address, value};
    rf_model_set(address, value);
}
static void rf_test_copy_from(void *dst, u32 address, size_t size)
{
    const void *src = NULL;
    if (address == 0x1e1900 && size == sizeof rf_fake_wla) src = rf_fake_wla;
    else if (address == 0x1c0000 && size == sizeof rf_fake_bt_ahb) src = rf_fake_bt_ahb;
    else if (address == 0x1cfc00 && size == sizeof rf_fake_bt_sfr) src = rf_fake_bt_sfr;
    CHECK(src != NULL);
    memcpy(dst, src, size);
}
static void rf_test_copy_to(u32 address, const void *src, size_t size)
{
    void *dst = NULL;
    if (address == 0x1e1900 && size == sizeof rf_fake_wla) dst = rf_fake_wla;
    else if (address == 0x1c0000 && size == sizeof rf_fake_bt_ahb) dst = rf_fake_bt_ahb;
    else if (address == 0x1cfc00 && size == sizeof rf_fake_bt_sfr) dst = rf_fake_bt_sfr;
    CHECK(dst != NULL);
    memcpy(dst, src, size);
}
static void rf_test_store32(u32 address, u32 value)
{
    CHECK(address >= 0x1e1900 && address < 0x1e19a0 && (address & 3) == 0);
    memcpy(rf_fake_wla + (address - 0x1e1900), &value, sizeof value);
}
#define RF_READ32(a) rf_test_read32((u32)(a))
#define RF_WRITE32(a, v) rf_test_write32((u32)(a), (u32)(v))
#define RF_COPY_FROM(dst, address, size) rf_test_copy_from((dst), (u32)(address), (size))
#define RF_COPY_TO(address, src, size) rf_test_copy_to((u32)(address), (src), (size))
#define RF_NONVOLATILE_STORE32(address, value) rf_test_store32((u32)(address), (u32)(value))
#else
#define RF_READ32(a) (*(volatile u32 *)(uintptr_t)(a))
#define RF_WRITE32(a, v) (RF_READ32(a) = (u32)(v))
#define RF_COPY_FROM(dst, address, size) memcpy((dst), (const void *)(uintptr_t)(address), (size))
#define RF_COPY_TO(address, src, size) memcpy((void *)(uintptr_t)(address), (src), (size))
#define RF_NONVOLATILE_STORE32(address, value) (*(u32 *)(uintptr_t)(address) = (u32)(value))
#endif
#ifdef UNITTEST
static unsigned test_malloc_calls, test_malloc_fail_at;
static int test_malloc_config_value;
static unsigned test_assert_debug, test_soft_reset;
static jmp_buf test_assert_jmp;
static int test_jump_on_assert;
static void *rf_test_malloc(size_t size)
{
    ++test_malloc_calls;
    if (test_malloc_calls == test_malloc_fail_at) {
        config_asser = test_malloc_config_value;
        return NULL;
    }
    return malloc(size);
}
#define RF_MALLOC(size) rf_test_malloc(size)
void cpu_assert_debug(void)
{
    ++test_assert_debug;
    if (test_jump_on_assert) longjmp(test_assert_jmp, 1);
}
void p33_soft_reset(void) { ++test_soft_reset; }
#else
#define RF_MALLOC(size) malloc(size)
#endif
#define RF_RMW(a, clear, set) do { u32 _v = RF_READ32(a); RF_WRITE32(a, (_v & ~(u32)(clear)) | (u32)(set)); } while (0)

_Static_assert(sizeof(struct common_rf_param) == 1024, "BD19 RF parameter storage");
_Static_assert(offsetof(struct common_rf_param, delay_sys) == 6, "BD19 RF delay field");
_Static_assert(offsetof(struct common_rf_param, pll_config_tab) == 52, "BD19 RF table offset");
_Static_assert(sizeof(struct RF_PWR_OFF_INFO) == 1020, "BD19 RF save image");
_Static_assert(sizeof(struct BLE_PWR_OFF_STORE) == 352, "BD19 BLE save image");

#ifdef UNITTEST
extern u8 log_tag_const_i_RF;
#else
extern const u8 log_tag_const_i_RF;
#endif
extern void log_print(int level, void *file, const char *format, ...);
extern void delay(u32 usec);
extern void bt_sfr_pop(void);
extern void RF_mdm_init(void);
extern void bt_rf_analog_init(void);
extern void RF_analog_recover(void);
extern u8 low_power_get_osc_type(void);
extern u32 low_power_trace_drift(u32 usec);
extern bool link_sniff_sta_detect_all(void);
extern int gpio_set_fun_output_port(u32 gpio, u32 fun, u32 dir, u32 pull, u32 hd);
extern int gpio_direction_output(u32 gpio, int value);

struct common_rf_param rf __attribute__((section(".bt_rf_data"), aligned(4))) = {
    .bredr_fix_pwr = 0xff,
    .ble_fix_pwr = 0xff,
};
struct RF_PWR_OFF_INFO *rf_pwr_off_info_p __attribute__((section(".bt_rf_bss"), aligned(4)));
struct BLE_PWR_OFF_STORE *ble_pwr_off_store __attribute__((section(".bt_rf_bss"), aligned(4)));
static u16 pa_tx_io __attribute__((section(".bt_rf_data"), aligned(2))) = 0xffff;
static u16 pa_rx_io __attribute__((section(".bt_rf_data"), aligned(2))) = 0xffff;

static void bt_enter_deepsleep(void) RF_CODE;
static void bt_exit_deepsleep(void) RF_CODE;
static RF_CODE void rf_assert_failed_with_snapshot(int asser, const char *what)
{
    if (asser) {
        printf("ASSERT-FAILD: %s 0x0\n", what);
        cpu_assert_debug();
    } else {
        p33_soft_reset();
    }
}
const struct deepsleep_target bt __attribute__((section(".deepsleep_target"), aligned(4), used)) = {
    .name = "bt",
    .enter = (u8 (*)(void))bt_enter_deepsleep,
    .exit = (u8 (*)(void))bt_exit_deepsleep,
};

RF_CODE void rf_bredr_para_prf(void)
{
    if (log_tag_const_i_RF)
        log_print(2, NULL, "[Info]: [RF]bredr_max_pwr:%x\n\r\n", rf.bredr_pwr_max);
}

RF_CODE void bt_max_pwr_set(u8 pwr, u8 pg_pwr, u8 iq_pwr, u8 ble_pwr)
{
    u8 max_level = bt_get_pwr_max_level();
    if (pwr != 0xff) rf.bredr_pwr_max = pwr < max_level ? pwr : max_level;
    if (pg_pwr != 0xff) rf.bredr_page_pwr_max = pg_pwr < max_level ? pg_pwr : max_level;
    if (iq_pwr != 0xff) rf.bredr_inquiry_pwr_max = iq_pwr < max_level ? iq_pwr : max_level;
    if (ble_pwr != 0xff) rf.ble_pwr_max = ble_pwr < max_level ? ble_pwr : max_level;
}

RF_CODE void bt_max_pwr_get(u8 *pwr, u8 *pg_pwr, u8 *iq_pwr, u8 *ble_pwr)
{
    if (pwr) *pwr = rf.bredr_pwr_max;
    if (pg_pwr) *pg_pwr = rf.bredr_page_pwr_max;
    if (iq_pwr) *iq_pwr = rf.bredr_inquiry_pwr_max;
    if (ble_pwr) *ble_pwr = rf.ble_pwr_max;
}

RF_CODE void bredr_set_fix_pwr(u8 fix) { rf.bredr_fix_pwr = fix; bredr_link_reset_power(); }
RF_CODE void ble_set_fix_pwr(u8 fix) { rf.ble_fix_pwr = fix; le_hw_reset_power(); }
RF_CODE void bt_rf_PA_control_io_remap(u16 tx_io, u16 rx_io) { pa_tx_io = tx_io; pa_rx_io = rx_io; }
RF_CODE void bt_set_rxtx_status_enable(u8 enable) { rf.rxtx_status = enable; }
RF_CODE void bt_set_osc_cap(u8 cap_l, u8 cap_r) { rf.cap_l = cap_l; rf.cap_r = cap_r; }

RF_CODE void bt_pll_para(u32 osc, u32 sys, u8 low_power, u8 xosc)
{
    (void)low_power;
    rf.bt_osc = xosc > 15 ? 0 : xosc;
    rf.delay_sys = (u8)(sys / 1000000u);
    rf.pll_para = (u8)(osc / 2000000u + 254u);
}

RF_CODE void bt_pll_reset(u8 mode) { (void)mode; }

RF_CODE void RF_poweroff_recover(void)
{
    rf.bt_osc = 0;
    rf.delay_sys = 48;
    rf.pll_para = 10;
    RF_RMW(0x1f2100, 0, 128);
    RF_RMW(0x1f2100, 2031616, 1048576);
    RF_RMW(0x1f2100, 1, 0);
    delay(100);
    RF_RMW(0x1f2100, 0, 1);
    bt_sfr_pop();
}

RF_CODE void bredr_rf_init(void)
{
    RF_RMW(0x1cfc98, 0x0000ff00u, 0x00000a00u);
    RF_RMW(0x1cfc98, 0xff000000u, 0x0a000000u);
    RF_RMW(0x1cfc98, 0x000000ffu, 0x00000050u);
    RF_RMW(0x1cfc98, 0x00ff0000u, 0x00500000u);
    RF_RMW(0x1cfc9c, 0x00ff0000u, 0x003c0000u);
    RF_RMW(0x1cfc9c, 0xff000000u, 0x37000000u);
    RF_RMW(0x1cfca0, 0x000000ffu, 0x0000000fu);
    RF_RMW(0x1cfca0, 0x0000ff00u, 0x00000f00u);
    RF_RMW(0x1cfc04, 0, 0x40000000u);
    RF_RMW(0x1cfc24, 0, 0x70000000u);
    RF_RMW(0x1cfc24, 0, 0x00060000u);
}

RF_CODE void bt_process_before_init(void) { }

RF_CODE u32 bt_get_fine_cnt(u32 usec)
{
    u32 fine = low_power_get_osc_type() == 0 ? low_power_trace_drift(usec) : 0;
    return fine + usec + ((RF_READ32(0x1cfc6c) & 2047u) / 12u);
}

RF_CODE bool bt_bredr_is_sniff(void)
{
    if ((config_btctler_modules & 1) && link_sniff_sta_detect_all()) {
        putchar('Z');
        return 1;
    }
    return 0;
}

static void bt_enter_deepsleep(void)
{
    if (!rf.start) return;
    rf_pwr_off_info_p = RF_MALLOC(sizeof(*rf_pwr_off_info_p));
    int asser = config_asser;
    if (!rf_pwr_off_info_p)
        rf_assert_failed_with_snapshot(asser, "rf_pwr_off_info_p != NULL rf_pwr_off_info_p != NULL");

    /* The SDK loads the saved-image pointer once after the first assertion
     * path and reuses it across all three copies and register snapshots. */
    struct RF_PWR_OFF_INFO *save = rf_pwr_off_info_p;

    /* These three ranges and the three following registers are the SDK's
     * BD19 save image; their byte counts match the recovered struct layout. */
    RF_COPY_FROM(save->wla_sfr, 0x1e1900, 160);
    RF_COPY_FROM(save->bt_ahb_sfr, 0x1c0000, 320);
    RF_COPY_FROM(save->bt_sfr, 0x1cfc00, 528);
    save->bredr_finecnt_sfr = RF_READ32(0x1c00bc);
    save->bredr_clkncntl_sfr = RF_READ32(0x1c00b4);
    save->bredr_clkncnth_sfr = RF_READ32(0x1c00b8);

    if (config_btctler_modules & 2) {
        ble_pwr_off_store = RF_MALLOC(sizeof(*ble_pwr_off_store));
        if (!ble_pwr_off_store)
            rf_assert_failed_with_snapshot(asser, "ble_pwr_off_store != NULL ble_pwr_off_store != NULL");
        struct BLE_PWR_OFF_STORE *ble_save = ble_pwr_off_store;
        ble_enter_deepsleep(ble_save);
    }
}

static void bt_exit_deepsleep(void)
{
    if (!rf.start) return;
    RF_WRITE32(0x1f2200, 1);
    RF_RMW(0x1e0010, 49152, 16384);
    delay((u32)rf.delay_sys * 10);
    RF_RMW(0x1f2100, 0, 128);
    RF_RMW(0x1f2100, 2031616, 786432);
    delay((u32)rf.delay_sys * 10);
    RF_RMW(0x1f2100, 1, 0);
    delay((u32)rf.delay_sys * 10);
    RF_RMW(0x1f2100, 0, 1);
    delay((u32)rf.delay_sys * 10);
    RF_RMW(0x1cfc7c, 0, 64);
    RF_RMW(0x1c012c, 0, 1);

    struct RF_PWR_OFF_INFO *save = rf_pwr_off_info_p;
    for (u32 i = 0; i < 40; i++) {
        if ((i & ~1u) != 8) RF_NONVOLATILE_STORE32(0x1e1900 + 4 * i, save->wla_sfr[i]);
    }
    /* The IR reloads the global after the WLA loop, then keeps this pointer
     * across the AHB copy and SFR loop. */
    save = rf_pwr_off_info_p;
    RF_COPY_TO(0x1c0000, save->bt_ahb_sfr, 320);
    for (u32 i = 0; i < 132; i++) {
        if (i != 43 && i != 44) RF_WRITE32(0x1cfc00 + 4 * i, save->bt_sfr[i]);
    }
    save = rf_pwr_off_info_p;
    RF_WRITE32(0x1c0124, save->bredr_clkncntl_sfr);
    RF_WRITE32(0x1c0128, save->bredr_clkncnth_sfr);
    RF_WRITE32(0x1c012c, save->bredr_finecnt_sfr);
    RF_RMW(0x1c0124, 0, 32768);
    RF_RMW(0x1c0128, 0, 32768);
    RF_analog_recover();
    free(rf_pwr_off_info_p);
    if (config_btctler_modules & 2) {
        ble_exit_deepsleep(ble_pwr_off_store);
        free(ble_pwr_off_store);
    }
}

RF_CODE void bt_rf_init(void)
{
    if (log_tag_const_i_RF) log_print(2, NULL, "[Info]: [RF]>>>>>>>>>>>>>>RF_init\r\n");
    RF_WRITE32(0x1f2200, 1);
    rf.bt_osc = 0;
    rf.delay_sys = 24;
    rf.pll_para = 10;
    RF_RMW(0x1e0010, 49152, 16384);
    delay(240);
    RF_RMW(0x1f2100, 0, 128);
    RF_RMW(0x1f2100, 2031616, 786432);
    delay((u32)rf.delay_sys * 10);
    RF_RMW(0x1f2100, 1, 1);
    delay((u32)rf.delay_sys * 10);
    RF_RMW(0x1cfc40, 0, 1024);
    bt_rf_analog_init();
    RF_RMW(0x1cfc7c, 0, 64);
    if (rf.rxtx_status) {
        if (pa_tx_io != 0xffff && pa_rx_io != 0xffff) {
            if (log_tag_const_i_RF) log_print(2, NULL, "[Info]: [RF]pa_remap\n\r\n");
            RF_RMW(0x1e5414, 0, 134217728);
            RF_RMW(0x1e5420, 31, 16);
            RF_RMW(0x1e5420, 993, 544);
            gpio_set_fun_output_port(pa_tx_io, 6, 1, 1, 0);
            gpio_set_fun_output_port(pa_rx_io, 7, 1, 1, 0);
            gpio_direction_output(pa_tx_io, 0);
            gpio_direction_output(pa_rx_io, 0);
        } else {
            if (log_tag_const_i_RF) log_print(2, NULL, "[Info]: [RF]pa_unmap\n\r\n");
            RF_RMW(0x1f2100, 0, 8);
            RF_RMW(0x1f2100, 0, 16);
            RF_RMW(0x1e5008, 385, 0);
        }
    }
    rf.start = 1;
}

RF_CODE void bt_rf_close(void)
{
    if (!rf.start) return;
    rf.start = 0;
    RF_RMW(0x1f2100, 31, 0);
    delay(1000);
    RF_WRITE32(0x1f2100, 0);
    delay(1000);
    RF_RMW(0x1e0010, 0, 49152);
    delay(1000);
}

RF_CODE void bt_clk_close(u8 mode, u8 flag)
{
    (void)mode; (void)flag;
    if (!rf.start) return;
    rf.start = 0;
    RF_RMW(0x1f2100, 2031616, 786432);
    RF_RMW(0x1f2100, 0, 3145728);
    RF_RMW(0x1f2100, 1, 0);
    if ((RF_READ32(0x1f2100) & 3u) == 0) RF_RMW(0x1f2100, 128, 0);
}

#ifdef UNITTEST
u8 log_tag_const_i_RF;
int config_asser, config_btctler_modules;
static u8 test_max_level = 11;
static unsigned test_delay_count, test_analog_init, test_sfr_pop;
static u32 test_delays[16];
static unsigned test_gpio_port_count, test_gpio_direction_count;
static u32 test_gpio[8];
static unsigned test_ble_enter, test_ble_exit, test_analog_recover;
u8 bt_get_pwr_max_level(void) { return test_max_level; }
u8 low_power_get_osc_type(void) { return 1; }
u32 low_power_trace_drift(u32 usec) { (void)usec; return 0; }
void bredr_link_reset_power(void) { }
void le_hw_reset_power(void) { }
void delay(u32 us) { CHECK(test_delay_count < 16); test_delays[test_delay_count++] = us; }
void bt_sfr_pop(void) { test_sfr_pop++; }
void bt_rf_analog_init(void) { test_analog_init++; }
void RF_analog_recover(void) { test_analog_recover++; }
void ble_enter_deepsleep(struct BLE_PWR_OFF_STORE *store) { CHECK(store != NULL); test_ble_enter++; }
void ble_exit_deepsleep(struct BLE_PWR_OFF_STORE *store) { CHECK(store != NULL); test_ble_exit++; }
bool link_sniff_sta_detect_all(void) { return false; }
int gpio_set_fun_output_port(u32 gpio, u32 fun, u32 dir, u32 pull, u32 hd)
{ (void)fun; (void)dir; (void)pull; (void)hd; test_gpio[test_gpio_port_count++] = gpio; return 0; }
int gpio_direction_output(u32 gpio, int value)
{ (void)value; test_gpio_direction_count++; test_gpio[test_gpio_port_count++] = gpio; return 0; }
void log_print(int level, void *file, const char *format, ...)
{ (void)level; (void)file; (void)format; }

static void test_reset_model(void)
{
    rf_model_count = rf_access_count = 0;
    test_delay_count = test_analog_init = test_sfr_pop = test_analog_recover = 0;
    test_ble_enter = test_ble_exit = 0;
    test_gpio_port_count = test_gpio_direction_count = 0;
}
static void test_access(unsigned i, char op, u32 addr, u32 val)
{
    CHECK(i < rf_access_count);
    CHECK(rf_access[i].op == op && rf_access[i].address == addr && rf_access[i].value == val);
}

int main(void)
{
    CHECK(rf.bredr_pwr_max == 0 && rf.bredr_page_pwr_max == 0);
    CHECK(rf.bredr_fix_pwr == 0xff && rf.ble_fix_pwr == 0xff);
    CHECK(rf.cap_l == 0 && rf.cap_r == 0 && rf.delay_sys == 0 && rf.pll_para == 0);
    test_max_level = 11;
    rf.bredr_pwr_max = 1; rf.bredr_page_pwr_max = 2;
    rf.bredr_inquiry_pwr_max = 3; rf.ble_pwr_max = 4;
    bt_max_pwr_set(15, 0xff, 8, 10);
    CHECK(rf.bredr_pwr_max == 11 && rf.bredr_page_pwr_max == 2);
    CHECK(rf.bredr_inquiry_pwr_max == 8 && rf.ble_pwr_max == 10);
    u8 a = 0xff, b = 0xff, c = 0xff, d = 0xff;
    bt_max_pwr_get(&a, &b, NULL, &d);
    CHECK(a == 11 && b == 2 && d == 10);
    bt_max_pwr_get(NULL, NULL, &c, NULL);
    CHECK(c == 8);
    bt_max_pwr_set(0xff, 0xff, 0xff, 0xff);
    CHECK(rf.bredr_pwr_max == 11 && rf.bredr_inquiry_pwr_max == 8);
    /* SDK masks put the four power levels in successive low-to-high nibbles. */
    CHECK(((const u8 *)&rf)[0] == 0x2b && ((const u8 *)&rf)[1] == 0xa8);

    bt_set_osc_cap(21, 42);
    bt_set_rxtx_status_enable(1);
    bt_pll_para(24000000, 16000000, 3, 16);
    CHECK(rf.cap_l == 21 && rf.cap_r == 42 && rf.rxtx_status == 1);
    CHECK(rf.bt_osc == 0 && rf.delay_sys == 16 && rf.pll_para == 10);
    bt_pll_para(24000000, 16000000, 0, 15);
    CHECK(rf.bt_osc == 15);
    rf.bredr_fix_pwr = 0xff; bredr_set_fix_pwr(3); CHECK(rf.bredr_fix_pwr == 3);
    rf.ble_fix_pwr = 0xff; ble_set_fix_pwr(4); CHECK(rf.ble_fix_pwr == 4);

    /* IR-derived BD19 RF startup order, including its 240 us initial clock
     * delay, three fresh delay_sys reads and analog-init boundary. */
    rf.start = 0; rf.delay_sys = 7; rf.rxtx_status = 0;
    test_reset_model();
    bt_rf_init();
    CHECK(rf.start == 1 && test_analog_init == 1 && test_delay_count == 3);
    CHECK(test_delays[0] == 240 && test_delays[1] == 240 && test_delays[2] == 240);
    CHECK(rf_access_count == 13);
    test_access(0, 'W', 0x1f2200, 1);
    test_access(1, 'R', 0x1e0010, 0); test_access(2, 'W', 0x1e0010, 16384);
    test_access(3, 'R', 0x1f2100, 0); test_access(4, 'W', 0x1f2100, 128);
    test_access(5, 'R', 0x1f2100, 128); test_access(6, 'W', 0x1f2100, 786560);
    test_access(7, 'R', 0x1f2100, 786560); test_access(8, 'W', 0x1f2100, 786561);
    test_access(9, 'R', 0x1cfc40, 0); test_access(10, 'W', 0x1cfc40, 1024);
    test_access(11, 'R', 0x1cfc7c, 0); test_access(12, 'W', 0x1cfc7c, 64);

    test_reset_model();
    rf.start = 0;
    rf.rxtx_status = 1;
    bt_rf_PA_control_io_remap(17, 18);
    bt_rf_init();
    CHECK(test_gpio_port_count == 4 && test_gpio_direction_count == 2);
    CHECK(test_gpio[0] == 17 && test_gpio[1] == 18 && test_gpio[2] == 17 && test_gpio[3] == 18);
    CHECK(rf_model_get(0x1e5414) == 134217728);
    CHECK(rf_model_get(0x1e5420) == 560);

    test_reset_model(); rf.start = 0;
    bt_rf_close(); CHECK(rf_access_count == 0);
    rf.start = 1; test_reset_model();
    bt_rf_close();
    CHECK(rf.start == 0 && test_delay_count == 3 && rf_access_count == 5);
    test_access(0, 'R', 0x1f2100, 0); test_access(1, 'W', 0x1f2100, 0);
    test_access(2, 'W', 0x1f2100, 0); test_access(3, 'R', 0x1e0010, 0);
    test_access(4, 'W', 0x1e0010, 49152);

    test_reset_model(); rf.start = 1;
    rf_model_set(0x1f2100, 0x81);
    bt_clk_close(0, 0);
    CHECK(!rf.start && rf_access_count == 9);
    test_access(1, 'W', 0x1f2100, 0xc0081);
    test_access(3, 'W', 0x1f2100, 0x3c0081);
    test_access(5, 'W', 0x1f2100, 0x3c0080);
    test_access(8, 'W', 0x1f2100, 0x3c0000);

    test_reset_model();
    rf_model_set(0x1cfc6c, 2047);
    CHECK(bt_get_fine_cnt(100) == 270);
    CHECK(rf_access_count == 1);
    test_access(0, 'R', 0x1cfc6c, 2047);

    /* Exercise the BD19 RF/BLE save image and restore exclusions using fake
     * memory regions; expected locations and skipped word indexes come from
     * the original IR loops (WLA 8/9, BT SFR 43/44). */
    test_reset_model();
    for (u32 i = 0; i < 40; ++i) { u32 v = 0xa0000000u + i; memcpy(rf_fake_wla + 4 * i, &v, 4); }
    for (u32 i = 0; i < 80; ++i) { u32 v = 0xb0000000u + i; memcpy(rf_fake_bt_ahb + 4 * i, &v, 4); }
    for (u32 i = 0; i < 132; ++i) { u32 v = 0xc0000000u + i; memcpy(rf_fake_bt_sfr + 4 * i, &v, 4); }
    rf_model_set(0x1c00bc, 0x1111); rf_model_set(0x1c00b4, 0x2222); rf_model_set(0x1c00b8, 0x4444);
    rf.start = 1; config_btctler_modules = 2; config_asser = 1;
    bt_enter_deepsleep();
    CHECK(rf_pwr_off_info_p != NULL && ble_pwr_off_store != NULL && test_ble_enter == 1);
    CHECK(rf_pwr_off_info_p->wla_sfr[0] == 0xa0000000u);
    CHECK(rf_pwr_off_info_p->bt_ahb_sfr[79] == 0xb000004fu);
    CHECK(rf_pwr_off_info_p->bt_sfr[131] == 0xc0000083u);
    CHECK(rf_pwr_off_info_p->bredr_finecnt_sfr == 0x1111);
    CHECK(rf_pwr_off_info_p->bredr_clkncntl_sfr == 0x2222);
    CHECK(rf_pwr_off_info_p->bredr_clkncnth_sfr == 0x4444);
    memset(rf_fake_wla, 0, sizeof rf_fake_wla);
    memset(rf_fake_bt_ahb, 0, sizeof rf_fake_bt_ahb);
    memset(rf_fake_bt_sfr, 0, sizeof rf_fake_bt_sfr);
    rf_access_count = test_delay_count = 0;
    bt_exit_deepsleep();
    CHECK(test_ble_exit == 1 && test_analog_recover == 1 && test_delay_count == 4);
    for (u32 i = 0; i < 40; ++i) {
        u32 v; memcpy(&v, rf_fake_wla + 4 * i, 4);
        CHECK(v == ((i == 8 || i == 9) ? 0 : 0xa0000000u + i));
    }
    for (u32 i = 0; i < 80; ++i) { u32 v; memcpy(&v, rf_fake_bt_ahb + 4 * i, 4); CHECK(v == 0xb0000000u + i); }
    for (u32 i = 0; i < 132; ++i) {
        u32 v = rf_model_get(0x1cfc00 + 4 * i);
        CHECK(v == ((i == 43 || i == 44) ? 0 : 0xc0000000u + i));
    }
    CHECK(rf_model_get(0x1c0124) == (0x2222u | 32768u));
    CHECK(rf_model_get(0x1c0128) == (0x4444u | 32768u));
    CHECK(rf_model_get(0x1c012c) == 0x1111u);
    config_btctler_modules = 0;

    /* IR-derived cached-policy case: config_asser is read after the first
     * allocation and reused after the second. Mutate it inside that second
     * allocation; the cached enabled policy must call the debug stop path. */
    rf_pwr_off_info_p = NULL;
    ble_pwr_off_store = NULL;
    rf.start = 1;
    config_btctler_modules = 2;
    config_asser = 1;
    test_malloc_calls = 0;
    test_malloc_fail_at = 2;
    test_malloc_config_value = 0;
    test_assert_debug = test_soft_reset = 0;
    test_ble_enter = 0;
    test_jump_on_assert = 1;
    if (setjmp(test_assert_jmp) == 0) bt_enter_deepsleep();
    test_jump_on_assert = 0;
    CHECK(test_malloc_calls == 2 && config_asser == 0);
    CHECK(test_assert_debug == 1 && test_soft_reset == 0);
    CHECK(test_ble_enter == 0);
    free(rf_pwr_off_info_p);
    rf_pwr_off_info_p = NULL;
    ble_pwr_off_store = NULL;
    test_malloc_fail_at = 0;
    config_btctler_modules = 0;

    test_reset_model();
    bredr_rf_init();
    CHECK(rf_access_count == 22);
    static const struct rf_model_access bredr_table[] = {
        {'R', 0x1cfc98, 0}, {'W', 0x1cfc98, 0x00000a00},
        {'R', 0x1cfc98, 0x00000a00}, {'W', 0x1cfc98, 0x0a000a00},
        {'R', 0x1cfc98, 0x0a000a00}, {'W', 0x1cfc98, 0x0a000a50},
        {'R', 0x1cfc98, 0x0a000a50}, {'W', 0x1cfc98, 0x0a500a50},
        {'R', 0x1cfc9c, 0}, {'W', 0x1cfc9c, 0x003c0000},
        {'R', 0x1cfc9c, 0x003c0000}, {'W', 0x1cfc9c, 0x373c0000},
        {'R', 0x1cfca0, 0}, {'W', 0x1cfca0, 0x0000000f},
        {'R', 0x1cfca0, 0x0000000f}, {'W', 0x1cfca0, 0x00000f0f},
        {'R', 0x1cfc04, 0}, {'W', 0x1cfc04, 0x40000000},
        {'R', 0x1cfc24, 0}, {'W', 0x1cfc24, 0x70000000},
        {'R', 0x1cfc24, 0x70000000}, {'W', 0x1cfc24, 0x70060000},
    };
    for (unsigned i = 0; i < sizeof bredr_table / sizeof bredr_table[0]; ++i)
        test_access(i, bredr_table[i].op, bredr_table[i].address, bredr_table[i].value);
    test_reset_model();
    rf_model_set(0x1f2100, 0x12345678);
    RF_poweroff_recover();
    CHECK(rf_access_count == 8 && test_delay_count == 1 && test_sfr_pop == 1);
    test_access(0, 'R', 0x1f2100, 0x12345678);
    test_access(1, 'W', 0x1f2100, 0x123456f8);
    test_access(3, 'W', 0x1f2100, 0x123056f8);
    test_access(5, 'W', 0x1f2100, 0x123056f8 & ~1u);
    test_access(7, 'W', 0x1f2100, 0x123056f9);
    bt_pll_reset(1);
    CHECK(rf_access_count == 8);
    return TEST_DONE("btctrler_bd19_RF");
}
#endif
