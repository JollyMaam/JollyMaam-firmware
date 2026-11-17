/* BLE AGC, RSSI, and power controls. Keep the SDK's raw field offsets,
 * volatile RSSI accesses, signed-byte clamps, and repeated power lookups. */
#include "btctrler/glue.h"
#include <stddef.h>

#define BLE_RF_CODE __attribute__((section(".ble_rf_code")))

#ifndef UNITTEST
_Static_assert(offsetof(struct ble4_hw, pwr_set) == 471, "SDK BLE power offset");
_Static_assert(offsetof(struct ble4_hw, agc_set) == 472, "SDK BLE AGC offset");
_Static_assert(offsetof(struct ble4_hw, ble_fp) + offsetof(struct ble_param, RSSI2) == 316,
               "SDK BLE RSSI2 offset");
_Static_assert(offsetof(struct ble4_hw, ble_fp) + offsetof(struct ble_param, RSSI3) == 318,
               "SDK BLE RSSI3 offset");
_Static_assert(offsetof(struct ble5_hw, ble5_fp) + offsetof(struct ble5_param, EXT_PLL_COMP) == 276,
               "SDK BLE5 PLL compensation offset");
_Static_assert(offsetof(struct ble5_hw, agc_pwr) + offsetof(struct ext_agc_pwr, pwr_set) == 338,
               "SDK BLE5 power offset");
_Static_assert(offsetof(struct ble5_hw, agc_pwr) + offsetof(struct ext_agc_pwr, agc_set) == 339,
               "SDK BLE5 AGC offset");
#endif

char BLE_RF_CODE ble_agc_normal_set(void *fp, char sel, char inc)
{
    (void)sel;
    (void)inc;
    u8 *bytes = fp;
    u16 rssi3 = *(volatile u16 *)(bytes + 318);
    u16 rssi2 = *(volatile u16 *)(bytes + 316);
    (void)rssi3;
    u8 agc = (u8)((rssi2 >> 8) & 63);
    bytes[472] = agc;
    return (char)(agc < 9 ? 1 : (agc > 28 ? 2 : 0));
}

char BLE_RF_CODE ble5_agc_normal_set(void *fp, char sel, char inc)
{
    (void)sel;
    (void)inc;
    u8 *bytes = fp;
    *(volatile u32 *)(bytes + 276) = 0;
    u16 rssi3 = *(volatile u16 *)(bytes + 290);
    u16 rssi2 = *(volatile u16 *)(bytes + 288);
    (void)rssi3;
    u8 agc = (u8)((rssi2 >> 8) & 63);
    bytes[339] = agc;
    return (char)(agc < 9 ? 1 : (agc > 28 ? 2 : 0));
}

char BLE_RF_CODE ble_analog_get_rssi(struct ble_param *frame)
{
    u16 rssi3 = frame->RSSI3;
    u16 rssi2 = frame->RSSI2;
    (void)rssi3;
    u32 index = (rssi2 >> 8) & 63;
    int value = 21 - (rssi2 & 63) - (u8)agc_dbm_tlb[index];
    return (char)(s8)value;
}

char BLE_RF_CODE ble_txpwr_normal_set(void *fp, char sel, char inc)
{
    u8 *bytes = fp;
    char *agc_pwr = (char *)(bytes + 471);
    char status = 0;
    if (sel == 0) {
        *agc_pwr = inc;
    } else {
        u8 step = inc != 0 ? 1 : 255;
        *agc_pwr = (char)(u8)((u8)*agc_pwr + step);
    }

    if (*agc_pwr < 0) {
        *agc_pwr = 0;
        status = 1;
    } else if ((int)*agc_pwr > (int)bt_get_pwr_max_level()) {
        *agc_pwr = (char)bt_get_pwr_max_level();
        status = 2;
    }

    u8 fixed = rf.ble_fix_pwr;
    if (fixed != 255 && (int)*agc_pwr > (int)fixed)
        *agc_pwr = (char)fixed;

    s32 power = (s8)*agc_pwr;
    u8 power_max = rf.ble_pwr_max;
    if (power >= power_max)
        power = power_max;
    *agc_pwr = (char)power;

    u8 level = (u8)*agc_pwr;
    u32 txpwr = bt_get_txpwr_tb(level);
    *(volatile u32 *)(bytes + 288) = txpwr;
    u32 txset = bt_get_txset_tb(level);
    *(volatile u32 *)(bytes + 296) = txset;
    return status;
}

char BLE_RF_CODE ble5_txpwr_normal_set(void *fp, char sel, char inc)
{
    u8 *bytes = fp;
    *(volatile u32 *)(bytes + 276) = 0;
    char *agc_pwr = (char *)(bytes + 338);
    char status = 0;
    if (sel == 0) {
        *agc_pwr = inc;
    } else {
        u8 step = inc != 0 ? 1 : 255;
        *agc_pwr = (char)(u8)((u8)*agc_pwr + step);
    }

    if (*agc_pwr < 0) {
        *agc_pwr = 0;
        status = 1;
    } else if ((int)*agc_pwr > (int)bt_get_pwr_max_level()) {
        *agc_pwr = (char)bt_get_pwr_max_level();
        status = 2;
    }

    u8 fixed = rf.ble_fix_pwr;
    if (fixed != 255 && (int)*agc_pwr > (int)fixed)
        *agc_pwr = (char)fixed;

    s32 power = (s8)*agc_pwr;
    u8 power_max = rf.ble_pwr_max;
    if (power >= power_max)
        power = power_max;
    *agc_pwr = (char)power;

    u8 level = (u8)*agc_pwr;
    u32 txpwr = bt_get_txpwr_tb(level);
    *(volatile u32 *)(bytes + 260) = txpwr;
    u32 txset = bt_get_txset_tb(level);
    *(volatile u32 *)(bytes + 268) = txset;
    return status;
}

#ifdef UNITTEST
#include "test.h"

struct common_rf_param rf;
const char agc_dbm_tlb[33] = {
    (char)0xf7, (char)0xf9, (char)0xfb, (char)0xfd, (char)0xff,
    0x01, 0x03, 0x0a, 0x0c, 0x0e, 0x10, 0x12, 0x14, 0x16, 0x18,
    0x1a, 0x1c, 0x1e, 0x20, 0x22, 0x22, 0x24, 0x26, 0x25, 0x27,
    0x29, 0x2b, 0x2d, 0x2f, 0x31, 0x32, 0x35, 0x37,
};

static u8 max_levels[8];
static unsigned int max_reads;
static unsigned int txpwr_calls, txset_calls;
static u8 txpwr_level, txset_level;
static u32 txpwr_result, txset_result;
static u8 *mutate_power_after_txpwr;
static u8 mutated_power_value;

u8 bt_get_pwr_max_level(void)
{
    CHECK(max_reads < sizeof(max_levels));
    return max_levels[max_reads++];
}

u32 bt_get_txpwr_tb(u8 level)
{
    ++txpwr_calls;
    txpwr_level = level;
    if (mutate_power_after_txpwr) {
        *mutate_power_after_txpwr = mutated_power_value;
        mutate_power_after_txpwr = 0;
    }
    return txpwr_result;
}

u32 bt_get_txset_tb(u8 level)
{
    ++txset_calls;
    txset_level = level;
    return txset_result;
}

static void reset_lookups(void)
{
    max_reads = txpwr_calls = txset_calls = 0;
    txpwr_result = UINT32_C(0x12345678);
    txset_result = UINT32_C(0x87654321);
}

int main(void)
{
    _Alignas(4) u8 ble4[624] = {0};
    _Alignas(4) u8 ble5[400] = {0};
    struct ble_param frame = {0};

    *(u16 *)(ble4 + 316) = 8u << 8;
    CHECK(ble_agc_normal_set(ble4, 0, 0) == 1 && ble4[472] == 8);
    *(u16 *)(ble4 + 316) = 9u << 8;
    CHECK(ble_agc_normal_set(ble4, 0, 0) == 0 && ble4[472] == 9);
    *(u16 *)(ble4 + 316) = 29u << 8;
    CHECK(ble_agc_normal_set(ble4, 0, 0) == 2 && ble4[472] == 29);

    *(u32 *)(ble5 + 276) = UINT32_C(0xdeadbeef);
    *(u16 *)(ble5 + 288) = 28u << 8;
    CHECK(ble5_agc_normal_set(ble5, 0, 0) == 0);
    CHECK(*(u32 *)(ble5 + 276) == 0 && ble5[339] == 28);
    *(u16 *)(ble5 + 288) = 29u << 8;
    CHECK(ble5_agc_normal_set(ble5, 0, 0) == 2 && ble5[339] == 29);

    frame.RSSI2 = (1u << 8) | 5;
    CHECK(ble_analog_get_rssi(&frame) == 23);
    frame.RSSI2 = (32u << 8) | 3;
    CHECK(ble_analog_get_rssi(&frame) == -37);
    frame.RSSI2 = (30u << 8);
    CHECK(ble_analog_get_rssi(&frame) == -29);

    rf.ble_fix_pwr = 255;
    rf.ble_pwr_max = 15;
    reset_lookups();
    max_levels[0] = 99;
    CHECK(ble_txpwr_normal_set(ble4, 0, -1) == 1);
    CHECK((s8)ble4[471] == 0 && max_reads == 0);
    CHECK(txpwr_level == 0 && txset_level == 0 && txpwr_calls == 1 && txset_calls == 1);
    CHECK(*(u32 *)(ble4 + 288) == txpwr_result && *(u32 *)(ble4 + 296) == txset_result);

    reset_lookups();
    max_levels[0] = 5;
    max_levels[1] = 3;
    CHECK(ble_txpwr_normal_set(ble4, 0, 8) == 2);
    CHECK((s8)ble4[471] == 3 && max_reads == 2);
    CHECK(txpwr_level == 3 && txset_level == 3);

    rf.ble_fix_pwr = 2;
    reset_lookups();
    max_levels[0] = 15;
    CHECK(ble_txpwr_normal_set(ble4, 0, 8) == 0);
    CHECK((s8)ble4[471] == 2 && max_reads == 1);
    CHECK(txpwr_level == 2 && txset_level == 2);

    rf.ble_fix_pwr = 255;
    reset_lookups();
    max_levels[0] = 7;
    max_levels[1] = 7;
    CHECK(ble5_txpwr_normal_set(ble5, 0, 8) == 2);
    CHECK((s8)ble5[338] == 7 && *(u32 *)(ble5 + 276) == 0);
    CHECK(max_reads == 2 && txpwr_level == 7 && txset_level == 7);
    CHECK(*(u32 *)(ble5 + 260) == txpwr_result && *(u32 *)(ble5 + 268) == txset_result);

    reset_lookups();
    max_levels[0] = 15;
    ble5[338] = 1;
    CHECK(ble5_txpwr_normal_set(ble5, 1, 0) == 0);
    CHECK((s8)ble5[338] == 0 && max_reads == 1);
    CHECK(txpwr_level == 0 && txset_level == 0);

    reset_lookups();
    rf.ble_fix_pwr = 255;
    max_levels[0] = 15;
    mutate_power_after_txpwr = &ble4[471];
    mutated_power_value = 12;
    CHECK(ble_txpwr_normal_set(ble4, 0, 4) == 0);
    CHECK(txpwr_level == 4 && txset_level == 4 && ble4[471] == 12);
    return TEST_DONE("btctrler_analog_ble");
}
#endif
