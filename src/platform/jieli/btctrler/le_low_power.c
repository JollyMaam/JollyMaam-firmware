/* BLE low-power callbacks and connection-clock compensation.  Keep the SDK's
 * anchor arithmetic, register ordering, repeated reads, and unsigned wraps. */
#include "btctrler/glue.h"

#include <stdint.h>

#define BLE_CODE __attribute__((section(".ble_rf_code")))

extern void *low_power_get(void *priv, const struct low_power_operation *ops);
extern void low_power_put(void *priv);
extern void low_power_on(void);
extern void low_power_request(void);
#ifdef CONFIG_CPU_BD19
extern BTCTRLER_CONFIG int config_btctler_le_hw_nums;
extern BTCTRLER_CONFIG u8 lp_bt_sniff_use_btosc;
static __attribute__((section(".ble_rf_data"))) u32 ble_finetimecnt_poweroff_sfr;
#endif

#define BLE_RF_BASE 0x1c8000u
#define BLE_RF_REG(off) (BLE_RF_BASE + (off))
#define BLE_DEEPSL_CON BLE_RF_REG(0x38)
#define BLE_FINE_CNT  BLE_RF_REG(0x3c)
#define BLE_CLKN_CNT  BLE_RF_REG(0x40)
#define BLE_CLKN_HI   BLE_RF_REG(0x44)
#define BLE_PHASE     BLE_RF_REG(0x48)
#define BLE_FINE_DIFF BLE_RF_REG(0x0c)
#define BLE_FINE_ADJ  BLE_RF_REG(0x14)
#define BLE_POWER_CTL 0x1cfc6cu
#define BLE_WAKE_CTL  0x1cfc70u

#ifdef UNITTEST
static volatile u32 test_mmio[16];
#ifdef CONFIG_CPU_BD19
static u8 test_deepsleep_sfr[352];
struct deep_event { u32 address, value; };
static struct deep_event deep_events[512];
static unsigned deep_event_count;
static void deep_record(u32 address, u32 value)
{ deep_events[deep_event_count++] = (struct deep_event){address, value}; }
#endif
static volatile u32 *test_reg(u32 address)
{
    switch (address) {
    case 0x1c8000u: return &test_mmio[10];
    case 0x1c801cu: return &test_mmio[11];
    case 0x1c8020u: return &test_mmio[12];
    case 0x1c8024u: return &test_mmio[13];
    case BLE_DEEPSL_CON: return &test_mmio[0];
    case BLE_FINE_CNT: return &test_mmio[1];
    case BLE_CLKN_CNT: return &test_mmio[2];
    case BLE_CLKN_HI: return &test_mmio[3];
    case BLE_PHASE: return &test_mmio[4];
    case BLE_FINE_DIFF: return &test_mmio[5];
    case BLE_FINE_ADJ: return &test_mmio[6];
    case BLE_POWER_CTL: return &test_mmio[7];
    case BLE_WAKE_CTL: return &test_mmio[8];
    default: return &test_mmio[15];
    }
}
#define REG32(address) (*test_reg(address))
#define DEEPSLEEP_SFR test_deepsleep_sfr
#ifdef CONFIG_CPU_BD19
#define DEEP_WRITE(address, value) do { u32 v_ = (value); deep_record(address, v_); REG32(address) = v_; } while (0)
#define DEEP_SYNC() deep_record(0, 0)
#define DEEP_READ(address) (deep_record(address, REG32(address)), REG32(address))
#endif
#else
#define REG32(address) (*(volatile u32 *)(uintptr_t)(address))
#define DEEPSLEEP_SFR ((void *)(uintptr_t)0x1c8000u)
#define DEEP_WRITE(address, value) (REG32(address) = (value))
#define DEEP_SYNC() __asm__ volatile("csync")
#define DEEP_READ(address) REG32(address)
#endif

static u16 __get_latency_cnt(struct ble_hw *hw) BLE_CODE;
static u16 __get_instant_count(struct ble_hw *hw) BLE_CODE;
static u32 __power_get_timeout(void *priv) BLE_CODE;
static void __power_suspend_probe(void *priv) BLE_CODE;
static void __power_suspend_post(void *priv, u32 usec) BLE_CODE;
static void __power_resume(void *priv, u32 usec) BLE_CODE;
static void __power_resume_post(void *priv, u32 usec) BLE_CODE;

BLE_CODE u32 __get_clkn(struct ble_hw *hw)
{
    u32 lo = __get_ble_anchor_con(hw, 0);
    u32 hi = __get_ble_anchor_con(hw, 14);
    return ((hi << 16) & 0x00ff0000u) | (lo & 0xffffu);
}

BLE_CODE u32 __get_interval(struct ble_hw *hw)
{
    u32 lo = __get_ble_anchor_con(hw, 1);
    u32 hi = __get_ble_anchor_con(hw, 15);
    return ((hi << 16) & 0x00ff0000u) | (lo & 0x7fffu);
}

BLE_CODE u16 __get_conn_event_count(struct ble_hw *hw)
{
    return (u16)(__get_ble_anchor_con(hw, 3) + 0xffffu);
}

static u16 __get_latency_cnt(struct ble_hw *hw)
{
    return (u16)__get_ble_anchor_con(hw, 9) >> 7;
}

BLE_CODE int __get_latency_valid(struct ble_hw *hw)
{
    struct ble4_hw *ble = hw->ble4_hw;
    if (ble->latency_en == 0 || ble->latency_substate == 1) return 0;
    return __get_latency_cnt(hw) != 0;
}

static u16 __get_instant_count(struct ble_hw *hw)
{
    return (u16)__get_ble_anchor_con(hw, 5);
}

BLE_CODE u32 __power_get_timeout_post(struct ble_hw *hw)
{
    REG32(BLE_DEEPSL_CON) |= 0x100;
    REG32(BLE_RF_REG(0x54)) = (REG32(BLE_RF_REG(0x54)) & ~8191u) | 150u;
    REG32(BLE_DEEPSL_CON) |= 0x200;
    while ((REG32(BLE_DEEPSL_CON) & ~1023u) != 6144u) {}

    u32 fine_anchor = REG32(BLE_PHASE);
    u32 clkn = __get_clkn(hw);
    u32 anchor7 = __get_ble_anchor_con(hw, 7);
    int signed_anchor7 = (int)(int16_t)anchor7;
    u32 phase = ((signed_anchor7 & 2048) == 0)
        ? anchor7 : (u32)(signed_anchor7 | 63488);
    u32 fine = 625u - (REG32(BLE_FINE_DIFF) & 1023u)
        - (REG32(BLE_FINE_ADJ) & 255u) + phase;
    int16_t phase16 = (int16_t)(u16)fine;
    int32_t phase_scaled = (int32_t)((u32)fine << 16);
    if (phase_scaled > 40894464)
        phase = (u32)((int32_t)phase16 - 625);
    else if (phase_scaled < 0)
        phase = (u32)((int32_t)phase16 + 625);
    else
        phase = fine;
    return (325u - (fine_anchor & 0xffffu))
        + ((clkn & 0xffffu) * 625u) + (u32)(int32_t)(int16_t)phase;
}

static u32 __power_get_timeout(void *priv)
{
    struct ble_hw *hw = priv;
    u8 *instant_valid = &hw->ble4_hw->instant_valid;

    for (;;) {
        if ((REG32(BLE_DEEPSL_CON) & 2u) || check_event_pending()) return 0;
        if (*instant_valid == 1) {
            u32 current = __get_conn_event_count(hw);
            u32 instant = __get_instant_count(hw);
            /* The IR zero-extends instant before subtracting one in i32. */
            if (current == instant - 1u) return 0;
        }

        u32 clkn = __get_clkn(hw);
        u32 clkn_count = clkn;
        if (__get_latency_valid(hw)) {
            u32 latency_count = __get_latency_cnt(hw);
            u32 interval = __get_interval(hw);
            u16 instant = __get_instant_count(hw);
            u16 current = __get_conn_event_count(hw);
            clkn_count = latency_count * interval + clkn;
            if (*instant_valid != 0) {
                u16 delta = (u16)(instant - current);
                if (delta > 1 && latency_count + 1u >= delta)
                    clkn_count = ((u32)delta - 2u) * interval + clkn;
            }
        }

        u32 clkn_again = __get_clkn(hw);
        if (clkn == 0 && clkn_again > clkn) continue;
        if (clkn_again < 5) return 0;
        if ((REG32(BLE_DEEPSL_CON) & 2u) || check_event_pending()) return 0;
        return clkn_count < 4 ? 0 : clkn_count * 625u - 2500u;
    }
}

static void __power_suspend_probe(void *priv)
{
    (void)priv;
#ifdef CONFIG_CPU_BD19
    REG32(BLE_WAKE_CTL) |= 2u;
#endif
    REG32(BLE_DEEPSL_CON) &= ~4u;
    REG32(BLE_DEEPSL_CON) |= 32u;
    REG32(BLE_DEEPSL_CON) |= 1u;
    if ((REG32(BLE_DEEPSL_CON) & 256u)
        && (REG32(BLE_DEEPSL_CON) & ~1023u) != 6144u)
        btctrler_assert_failed("(BLE_DEEPSL_CON >> 10) == 6 Reserve time pre not enough!!!", 0);
}

static void __power_suspend_post(void *priv, u32 usec)
{
    struct ble_hw *hw = priv;
    struct ble4_hw *ble = hw->ble4_hw;
    while ((REG32(BLE_DEEPSL_CON) & 8u) == 0) {}
    while (REG32(BLE_POWER_CTL) & 2048u) {}
    u32 fine_count = (((REG32(BLE_POWER_CTL) & 2047u) / 12u) + usec) / 625u + 1u;

    u32 clkn = __get_clkn(hw);
    u32 clkn_count = clkn;
    if (__get_latency_valid(hw)) {
        u32 latency_count = __get_latency_cnt(hw);
        u32 interval = __get_interval(hw);
        u16 instant = __get_instant_count(hw);
        u16 current = __get_conn_event_count(hw);
        clkn_count = latency_count * interval + clkn;
        if (ble->instant_valid != 0) {
            u16 delta = (u16)(instant - current);
            if (delta > 1 && latency_count + 1u >= delta)
                clkn_count = ((u32)delta - 2u) * interval + clkn;
        }
    }
    if (clkn_count <= fine_count) {
        if (config_asser) (void)__get_clkn(hw);
        btctrler_assert_failed("(clkn_cnt > fine_cnt) %d > %d timer %d %d  %s", (int)clkn_count);
    }
}

static void __power_resume(void *priv, u32 usec)
{
    struct ble_hw *hw = priv;
    struct ble4_hw *ble = hw->ble4_hw;
    u32 fine = bt_get_fine_cnt(usec);
#ifdef CONFIG_CPU_BD19
    fine += ble_finetimecnt_poweroff_sfr;
#endif
    u32 clkn = __get_clkn(hw);
    if (clkn == 0)
        btctrler_assert_failed("clkn_cnt >= 1 %d resume fatal ", 0);
    u32 interval = __get_interval(hw);
    u8 state = ble->state;
    u32 clkn_corr = fine / 625u;
    if (state == 7) clkn_corr %= interval;
    clkn_corr++;
    if (clkn < clkn_corr) clkn_corr -= interval;
    REG32(BLE_CLKN_CNT) = clkn_corr;
    REG32(BLE_CLKN_HI) = (u32)((int32_t)clkn_corr >> 16);
    REG32(BLE_FINE_CNT) = fine % 625u;

    if ((state & (u8)~1u) == 6) {
        u32 interval_sleep = __get_interval(hw);
        u32 start = (625u - REG32(BLE_PHASE)) + (clkn & 0xffffu) * 625u;
        u16 sleep_count = 0;
        if (((clkn - clkn_corr) & 0xffffu) == interval_sleep - 1u) {
            u32 phase = REG32(BLE_PHASE);
            u32 fine_count = REG32(BLE_FINE_CNT);
            sleep_count = fine_count + phase < 625u;
        }
#ifdef CONFIG_CPU_BD19
        u32 elapsed = fine;
#else
        u32 elapsed = usec;
#endif
        if (elapsed >= start)
            sleep_count = (u16)((((elapsed - start) / 625u) / interval_sleep)
                                + (u16)(sleep_count + 1u));

        u32 phase_count = sleep_count;
        if (__get_latency_valid(hw)) {
            u32 anchor6 = __get_ble_anchor_con(hw, 6);
            phase_count = sleep_count;
            __set_ble_anchor_con(hw, 6,
                (((((anchor6 >> 8) & 31u) * phase_count + (anchor6 & 63u)) % 37u) & 63u)
                 | (anchor6 & 65472u));
        }

        ble = hw->ble4_hw;
        u32 anchor9 = __get_ble_anchor_con(hw, 9);
        u32 latency_count = (anchor9 >> 7) & 511u;
        u32 recovered = latency_count < phase_count ? ble->latency + latency_count : latency_count;
        u32 new_latency = recovered - phase_count;
        u32 anchor2 = __get_ble_anchor_con(hw, 2);
        u32 anchor2_high = (u16)anchor2 & ~511u;
        __set_ble_anchor_con(hw, 2, anchor2_high | ((u16)new_latency & 511u));
        __set_ble_anchor_con(hw, 9, (anchor9 & 63u) | ((new_latency << 7) & 65408u) | 64u);
        __set_ble_anchor_con(hw, 2, (ble->latency & 511u) | anchor2_high);
        u32 anchor3 = __get_ble_anchor_con(hw, 3);
        __set_ble_anchor_con(hw, 3, (anchor3 & 65535u) + phase_count);
    }
    REG32(BLE_DEEPSL_CON) |= 4u;
#ifdef CONFIG_CPU_BD19
    if (lp_bt_sniff_use_btosc) bt_run_clk_sel(1);
#endif
}

static void __power_resume_post(void *priv, u32 usec)
{
    (void)usec;
    struct ble_hw *hw = priv;
    while (REG32(BLE_DEEPSL_CON) & 8u) {}
    (void)__get_clkn(hw);
    if ((int8_t)REG32(BLE_DEEPSL_CON) < 0)
        REG32(BLE_WAKE_CTL) |= 2u;
}

const struct low_power_operation ble_power_ops
    __attribute__((section(".ble_rf_const"), aligned(4))) = {
        .name = "ble",
        .get_timeout = __power_get_timeout,
        .suspend_probe = __power_suspend_probe,
        .suspend_post = __power_suspend_post,
        .resume = __power_resume,
        .resume_post = __power_resume_post,
#ifndef CONFIG_CPU_BD19
        .off_probe = NULL,
        .off_post = NULL,
        .on = NULL,
#endif
    };

BLE_CODE void ble_power_get(struct ble_hw *hw)
{
    hw->power_ctrl = low_power_get(hw, &ble_power_ops);
    if (!hw->power_ctrl) btctrler_assert_failed("hw->power_ctrl", 0);
}

BLE_CODE void ble_power_on(struct ble_hw *hw)
{
#ifdef CONFIG_CPU_BD19
    (void)hw;
#else
    if (hw->power_ctrl) low_power_on();
#endif
}

BLE_CODE void ble_power_off(struct ble_hw *hw)
{
    if (hw->power_ctrl) low_power_request();
}

BLE_CODE void ble_power_put(struct ble_hw *hw)
{
    if (hw->power_ctrl) {
        low_power_put(hw->power_ctrl);
        hw->power_ctrl = NULL;
    }
}

#ifdef CONFIG_CPU_BD19
BLE_CODE void ble_enter_deepsleep(struct BLE_PWR_OFF_STORE *store)
{
    __builtin_memcpy(store, DEEPSLEEP_SFR, 92);
    for (int hw = 0; hw < config_btctler_le_hw_nums; ++hw)
        for (int i = 0; i < 16; ++i) {
            DEEP_WRITE(0x1c801cu, (((u32)hw << 4) & 4080u) | ((u32)i << 10) | 2u);
            DEEP_SYNC();
            store->bt_anchor_con[hw][i] = (u16)DEEP_READ(0x1c8024u);
        }
    store->ble_finetimecnt_sfr = REG32(0x1c8048u);
    ble_finetimecnt_poweroff_sfr = 0;
}

BLE_CODE void ble_exit_deepsleep(struct BLE_PWR_OFF_STORE *store)
{
    __builtin_memcpy(DEEPSLEEP_SFR, store, 92);
    REG32(0x1c8038u) |= 1u;
    REG32(0x1c8038u) |= 4u;
    for (int hw = 0; hw < config_btctler_le_hw_nums; ++hw) {
        /* Restore ordinary anchors first, then commit the coupled 2/9 pair. */
        for (int i = 0; i < 16; ++i) {
            if (i == 2 || i == 9) continue;
            DEEP_WRITE(0x1c8020u, store->bt_anchor_con[hw][i]);
            DEEP_SYNC();
            DEEP_WRITE(0x1c801cu, (((u32)hw << 4) & 4080u) | ((u32)i << 10) | 5u);
        }
        u8 id = (u8)hw;
        u16 anchor9 = store->bt_anchor_con[id][9];
        u16 anchor2 = store->bt_anchor_con[id][2];
        u16 value = (anchor2 & (u16)~511u) | (anchor9 >> 7);
        DEEP_WRITE(0x1c8020u, value);
        DEEP_SYNC();
        DEEP_WRITE(0x1c801cu, ((u32)id << 4) | 2053u);
        DEEP_WRITE(0x1c8020u, anchor9 | 64u);
        DEEP_SYNC();
        DEEP_WRITE(0x1c801cu, ((u32)id << 4) | 9221u);
    }
    ble_finetimecnt_poweroff_sfr = store->ble_finetimecnt_sfr;
}
#endif

#ifdef UNITTEST
#include "test.h"

struct anchor_write { u8 index; u32 value; };
static u32 test_anchors[16];
static struct anchor_write anchor_writes[16];
static u32 anchor_write_count, pending, fine_value, get_calls, put_calls, on_calls, requests;
static u16 conn_reads[16];
static u32 conn_read_count;
static const struct low_power_operation *registered_ops;
static void *registered_priv;
static int force_get_failure, assert_count;
int config_asser;
int config_btctler_le_hw_nums;
#ifdef CONFIG_CPU_BD19
u8 lp_bt_sniff_use_btosc;
static unsigned clock_select_calls;
void bt_run_clk_sel(u8 source)
{
    CHECK(source == 1 && (REG32(BLE_DEEPSL_CON) & 4u));
    ++clock_select_calls;
}
#endif

void btctrler_assert_failed(const char *what, int value)
{ (void)what; (void)value; assert_count++; }

u32 __get_ble_anchor_con(struct ble_hw *hw, u8 index)
{
    (void)hw;
    if (index == 3 && conn_read_count < 16) conn_reads[conn_read_count++] = (u16)test_anchors[index];
    return test_anchors[index];
}

void __set_ble_anchor_con(struct ble_hw *hw, u8 index, u32 value)
{
    (void)hw;
    if (anchor_write_count < 16) {
        anchor_writes[anchor_write_count].index = index;
        anchor_writes[anchor_write_count].value = value;
    }
    anchor_write_count++;
    test_anchors[index] = value;
}

u8 check_event_pending(void) { return (u8)pending; }
u32 bt_get_fine_cnt(u32 usec) { (void)usec; return fine_value; }
void *low_power_get(void *priv, const struct low_power_operation *ops)
{ registered_priv = priv; registered_ops = ops; get_calls++; return force_get_failure ? NULL : (void *)0x1234; }
void low_power_put(void *priv) { (void)priv; put_calls++; }
void low_power_on(void) { on_calls++; }
void low_power_request(void) { requests++; }

static void reset_test(void)
{
    memset((void *)test_mmio, 0, sizeof(test_mmio));
    memset(test_anchors, 0, sizeof(test_anchors));
    memset(anchor_writes, 0, sizeof(anchor_writes));
    anchor_write_count = pending = fine_value = get_calls = put_calls = on_calls = requests = 0;
    conn_read_count = 0;
    registered_ops = NULL;
    registered_priv = NULL;
    force_get_failure = assert_count = 0;
    config_asser = 0;
#ifdef CONFIG_CPU_BD19
    deep_event_count = 0; ble_finetimecnt_poweroff_sfr = 0;
    lp_bt_sniff_use_btosc = 0; clock_select_calls = 0;
#endif
}

int main(void)
{
    struct ble4_hw ble = {0};
    struct ble_hw hw = { .ble4_hw = &ble };
    reset_test();

    test_anchors[0] = 0x4567; test_anchors[14] = 0xab;
    test_anchors[1] = 0xffff; test_anchors[15] = 0x34;
    test_anchors[3] = 0;
    CHECK(__get_clkn(&hw) == 0xab4567u);
    CHECK(__get_interval(&hw) == 0x347fffu);
    CHECK(__get_conn_event_count(&hw) == 0xffffu);

    /* Instant zero is widened before subtracting; it does not match event 65535. */
    reset_test();
    ble.instant_valid = 1;
    test_anchors[0] = 10; test_anchors[3] = 0; test_anchors[5] = 0;
    CHECK(ble_power_ops.get_timeout(&hw) == 3750u);
    CHECK(conn_read_count >= 1);

    reset_test();
    pending = 1;
    CHECK(ble_power_ops.get_timeout(&hw) == 0);
    CHECK(conn_read_count == 0);

    reset_test();
    REG32(BLE_DEEPSL_CON) = 2;
    test_anchors[0] = 10;
    CHECK(ble_power_ops.get_timeout(&hw) == 0);

    reset_test();
    test_anchors[0] = 3;
    CHECK(ble_power_ops.get_timeout(&hw) == 0);

    reset_test();
    ble.instant_valid = 0;
    test_anchors[0] = 10;
    ble.latency_en = 1; test_anchors[9] = 3u << 7;
    test_anchors[1] = 4;
    CHECK(ble_power_ops.get_timeout(&hw) == 11250u);

    reset_test();
    ble.instant_valid = 1; ble.latency_en = 1;
    test_anchors[0] = 10; test_anchors[1] = 4; test_anchors[9] = 3u << 7;
    test_anchors[3] = 5; test_anchors[5] = 8; /* event count 4, instant delta 4 */
    CHECK(ble_power_ops.get_timeout(&hw) == 8750u);

    reset_test();
    ble.instant_valid = 1; ble.latency_en = 1;
    test_anchors[0] = 10; test_anchors[1] = 4; test_anchors[9] = 3u << 7;
    test_anchors[3] = 0; test_anchors[5] = 2; /* wrapped event delta is 3 */
    CHECK(ble_power_ops.get_timeout(&hw) == 6250u);

    reset_test();
    REG32(BLE_DEEPSL_CON) = 0x1800;
    REG32(BLE_PHASE) = 100;
    test_anchors[0] = 7; test_anchors[14] = 0;
    test_anchors[7] = 0;
    CHECK(__power_get_timeout_post(&hw) == 4600u);
    CHECK((REG32(BLE_DEEPSL_CON) & 0x300u) == 0x300u);
    CHECK((REG32(BLE_RF_REG(0x54)) & 8191u) == 150u);

    reset_test();
    REG32(BLE_DEEPSL_CON) = 0x1800;
    test_anchors[0] = 10; test_anchors[7] = 0;
    REG32(BLE_FINE_DIFF) = 2;
    CHECK(__power_get_timeout_post(&hw) == 7198u); /* phase 623 */
    REG32(BLE_FINE_DIFF) = 1;
    CHECK(__power_get_timeout_post(&hw) == 7199u); /* phase 624 */
    REG32(BLE_FINE_DIFF) = 0;
    CHECK(__power_get_timeout_post(&hw) == 6575u); /* phase 625 wraps by one slot */

    reset_test();
    REG32(BLE_DEEPSL_CON) = 0x1800;
    test_anchors[0] = 10; test_anchors[7] = 0xfff0;
    REG32(BLE_FINE_DIFF) = 1023; REG32(BLE_FINE_ADJ) = 255;
    CHECK(__power_get_timeout_post(&hw) == 6531u); /* negative signed phase */

    reset_test();
    ble.state = 0; test_anchors[0] = 100; test_anchors[1] = 10;
    fine_value = 0x12345u * 625u + 77u;
    ble_power_ops.resume(&hw, 123456u);
    CHECK(REG32(BLE_CLKN_CNT) == 0x1233cu);
    CHECK(REG32(BLE_CLKN_HI) == 1u);
    CHECK(REG32(BLE_FINE_CNT) == 77u);
    CHECK((REG32(BLE_DEEPSL_CON) & 4u) != 0);

    reset_test();
    ble.state = 7; test_anchors[0] = 100; test_anchors[1] = 10;
    fine_value = 15u * 625u;
    ble_power_ops.resume(&hw, 10);
    CHECK(REG32(BLE_CLKN_CNT) == 6u); /* (15 % 10) + 1 */

    reset_test();
    ble.state = 0; test_anchors[0] = 1; test_anchors[1] = 4;
    fine_value = 1250;
    ble_power_ops.resume(&hw, 1250);
    CHECK(REG32(BLE_CLKN_CNT) == 0xffffffffu);
    CHECK(REG32(BLE_CLKN_HI) == 0xffffffffu); /* arithmetic high correction */

    reset_test();
    ble.state = 6; ble.latency_en = 1; ble.latency_substate = 0; ble.latency = 20;
    test_anchors[0] = 50; test_anchors[1] = 4; test_anchors[3] = 65530;
    test_anchors[6] = (2u << 8) | 1u;
    test_anchors[9] = (5u << 7) | 3u;
    test_anchors[2] = 0xa800;
    REG32(BLE_PHASE) = 100; fine_value = 3000000;
    ble_power_ops.resume(&hw, 3000000);
    CHECK(anchor_write_count == 5);
    CHECK(anchor_writes[0].index == 6);
    CHECK(anchor_writes[1].index == 2);
    CHECK(anchor_writes[2].index == 9);
    CHECK(anchor_writes[3].index == 2);
    CHECK(anchor_writes[4].index == 3);
    CHECK(anchor_writes[4].value == 66718u); /* setter receives the untruncated sum */
    CHECK(anchor_writes[0].value == 0x209u);
    CHECK(anchor_writes[1].value == 0xa975u);
    CHECK(anchor_writes[2].value == 0xbac3u);
    CHECK(anchor_writes[3].value == 0xa814u);

    reset_test();
    REG32(BLE_DEEPSL_CON) = 0x1800;
    ble_power_ops.suspend_probe(&hw);
    CHECK(REG32(BLE_DEEPSL_CON) == 0x1821u);
#ifdef CONFIG_CPU_BD19
    CHECK(REG32(BLE_WAKE_CTL) == 2u);
#endif

    reset_test();
    REG32(BLE_DEEPSL_CON) = 0x1808;
    REG32(BLE_POWER_CTL) = 1200;
    test_anchors[0] = 10;
    ble_power_ops.suspend_post(&hw, 500);
    CHECK(assert_count == 0);

    reset_test();
    REG32(BLE_DEEPSL_CON) = 0;
    test_anchors[0] = 10;
    ble_power_ops.resume_post(&hw, 0);
    CHECK(REG32(BLE_WAKE_CTL) == 0);
    REG32(BLE_DEEPSL_CON) = 0x80;
    ble_power_ops.resume_post(&hw, 0);
    CHECK(REG32(BLE_WAKE_CTL) == 2);

    reset_test();
    ble_power_get(&hw);
    CHECK(get_calls == 1 && registered_priv == &hw && registered_ops == &ble_power_ops);
    ble_power_on(&hw); ble_power_off(&hw); ble_power_put(&hw);
#ifdef CONFIG_CPU_BD19
    CHECK(!on_calls && requests == 1 && put_calls == 1);
#else
    CHECK(on_calls == 1 && requests == 1 && put_calls == 1);
#endif
    CHECK(hw.power_ctrl == NULL);

    reset_test();
    hw.power_ctrl = NULL;
    ble_power_on(&hw); ble_power_off(&hw); ble_power_put(&hw);
    CHECK(on_calls == 0 && requests == 0 && put_calls == 0);

    reset_test();
    force_get_failure = 1;
    ble_power_get(&hw);
    CHECK(hw.power_ctrl == NULL && assert_count == 1);

#ifdef CONFIG_CPU_BD19
    reset_test();
    config_btctler_le_hw_nums = 2;
    for (unsigned i = 0; i < sizeof(test_deepsleep_sfr); ++i)
        test_deepsleep_sfr[i] = (u8)(i * 3);
    test_mmio[13] = 0x1234;
    test_mmio[4] = 0;
    REG32(0x1c8048u) = 0x55aa;
    struct BLE_PWR_OFF_STORE store = {0};
    ble_enter_deepsleep(&store);
    CHECK(!memcmp(&store, test_deepsleep_sfr, 92));
    CHECK(deep_event_count == 2 * 16 * 3);
    for (unsigned hw_id = 0, e = 0; hw_id < 2; ++hw_id)
        for (unsigned i = 0; i < 16; ++i) {
            CHECK(deep_events[e].address == 0x1c801cu &&
                  deep_events[e++].value == ((hw_id << 4) | (i << 10) | 2u));
            CHECK(deep_events[e++].address == 0);
            CHECK(deep_events[e].address == 0x1c8024u && deep_events[e++].value == 0x1234);
        }
    CHECK(store.bt_anchor_con[0][0] == 0x1234 &&
          store.bt_anchor_con[1][15] == 0x1234);
    CHECK(store.ble_finetimecnt_sfr == 0x55aa &&
          ble_finetimecnt_poweroff_sfr == 0);
    store.bt_anchor_con[0][2] = 0xa800;
    store.bt_anchor_con[0][9] = (5u << 7) | 3u;
    store.ble_finetimecnt_sfr = 0xdeadbeef;
    test_mmio[11] = test_mmio[12] = test_mmio[14] = 0;
    for (unsigned h = 0; h < 2; ++h)
        for (unsigned i = 0; i < 16; ++i)
            store.bt_anchor_con[h][i] = (u16)(0xa000u + h * 256u + i * 17u);
    deep_event_count = 0;
    ble_exit_deepsleep(&store);
    CHECK(deep_event_count == 2 * 16 * 3);
    unsigned e = 0;
    for (unsigned h = 0; h < 2; ++h) {
        for (unsigned i = 0; i < 16; ++i) {
            if (i == 2 || i == 9) continue;
            CHECK(deep_events[e].address == 0x1c8020u &&
                  deep_events[e++].value == store.bt_anchor_con[h][i]);
            CHECK(deep_events[e++].address == 0);
            CHECK(deep_events[e].address == 0x1c801cu &&
                  deep_events[e++].value == ((h << 4) | (i << 10) | 5u));
        }
        CHECK(deep_events[e].address == 0x1c8020u &&
              deep_events[e++].value == ((store.bt_anchor_con[h][2] & 0xfe00u) |
                                         (store.bt_anchor_con[h][9] >> 7)));
        CHECK(deep_events[e++].address == 0);
        CHECK(deep_events[e].address == 0x1c801cu && deep_events[e++].value == ((h << 4) | 2053u));
        CHECK(deep_events[e].address == 0x1c8020u &&
              deep_events[e++].value == (store.bt_anchor_con[h][9] | 64u));
        CHECK(deep_events[e++].address == 0);
        CHECK(deep_events[e].address == 0x1c801cu && deep_events[e++].value == ((h << 4) | 9221u));
    }
    CHECK(!memcmp(test_deepsleep_sfr, &store, 92));
    CHECK((REG32(0x1c8038u) & 5u) == 5u);
    CHECK(REG32(0x1c801cu) == ((1u << 4) | 9221u));
    CHECK(ble_finetimecnt_poweroff_sfr == 0xdeadbeef);
    reset_test();
    ble.state = 0; test_anchors[0] = 100; test_anchors[1] = 10;
    ble_finetimecnt_poweroff_sfr = 600; fine_value = 50;
    ble_power_ops.resume(&hw, 50);
    CHECK(REG32(BLE_CLKN_CNT) == 2 && REG32(BLE_FINE_CNT) == 25);
    ble_finetimecnt_poweroff_sfr = 0xfffffff0u; fine_value = 40;
    ble_power_ops.resume(&hw, 40);
    CHECK(REG32(BLE_CLKN_CNT) == 1 && REG32(BLE_FINE_CNT) == 24);
    CHECK(clock_select_calls == 0);
    reset_test();
    ble.state = 6; ble.latency_en = 0;
    test_anchors[0] = 2; test_anchors[1] = 4; test_anchors[3] = 10;
    fine_value = 500; ble_finetimecnt_poweroff_sfr = 2000;
    lp_bt_sniff_use_btosc = 1;
    ble_power_ops.resume(&hw, 0);
    CHECK(test_anchors[3] == 11 && clock_select_calls == 1);
#endif

    return TEST_DONE("le_low_power");
}
#endif
