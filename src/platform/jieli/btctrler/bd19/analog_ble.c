/* BD19 BLE AGC, RSSI, and power controls.
 *
 * The BD19 implementation differs substantially from BR23: legacy BLE AGC
 * setup is a no-op, reset only advances the scan AGC selector, RSSI is decoded
 * from agc_dat_wr, and TX power stores the selected level rather than the
 * table values.  Keep the raw field offsets and signed-byte clamps.
 */
#include "btctrler/glue.h"

#include <stddef.h>

#define BLE_RF_CODE __attribute__((section(".ble_rf_code")))

extern BTCTRLER_CONFIG int config_force_bt_pwr_tab_using_normal_level;
extern u8 bt_get_pwr_max_level(void);

#ifndef UNITTEST
_Static_assert(offsetof(struct ble4_hw, scan_agc_set) == 470,
               "SDK BLE scan AGC offset");
_Static_assert(offsetof(struct ble4_hw, pwr_set) == 471,
               "SDK BLE power offset");
_Static_assert(offsetof(struct ble_param, agc_dat_wr) == 300,
               "SDK BLE AGC data offset");
_Static_assert(offsetof(struct ble5_hw, agc_pwr) + offsetof(struct ext_agc_pwr, pwr_set) == 338,
               "SDK BLE5 power offset");
#endif

void BLE_RF_CODE ble_agc_reset(void *fp)
{
    u8 *scan_agc_set = (u8 *)fp + 470;
    u8 value = (u8)(*scan_agc_set + 1);
    *scan_agc_set = value > 5 ? 0 : value;
}

char BLE_RF_CODE ble_agc_normal_set(void *fp, char sel, char inc)
{
    (void)fp;
    (void)sel;
    (void)inc;
    return 0;
}

char BLE_RF_CODE ble5_agc_normal_set(void *fp, char sel, char inc)
{
    (void)fp;
    (void)sel;
    (void)inc;
    return 0;
}

char BLE_RF_CODE ble_analog_get_rssi(struct ble_param *frame)
{
    u32 value = frame->agc_dat_wr;
    return (char)(s8)((((value >> 8) | 192) ^ 63) - (value >> 16));
}

static __inline__ __attribute__((always_inline)) char set_power(
    void *fp, int power_offset, int value_offset, char sel, char inc)
{
    s8 *agc_pwr = (s8 *)((u8 *)fp + power_offset);
    s8 value;
    if (sel == 0) {
        *agc_pwr = (s8)inc;
        int forced = config_force_bt_pwr_tab_using_normal_level;
        if (forced != 0) {
            *agc_pwr = (s8)forced;
            value = *agc_pwr;
        } else {
            value = inc;
        }
    } else {
        value = (s8)(*agc_pwr + (inc != 0 ? 1 : -1));
        *agc_pwr = value;
    }

    char status = 0;
    if (value < 0) {
        *agc_pwr = 0;
        status = 1;
    } else if ((int)value > (int)bt_get_pwr_max_level()) {
        *agc_pwr = (s8)bt_get_pwr_max_level();
        status = 2;
    }

    value = *agc_pwr;
    u8 fixed = rf.ble_fix_pwr;
    if (fixed != 255 && (int)value > (int)fixed)
        value = (s8)fixed;

    int max = rf.ble_pwr_max;
    if ((int)value >= max)
        value = (s8)max;
    *agc_pwr = value;
    *(volatile u32 *)((u8 *)fp + value_offset) = value_offset == 260 ? (u32)(u8)value : (u32)(s32)value;
    return status;
}

char BLE_RF_CODE ble_txpwr_normal_set(void *fp, char sel, char inc)
{
    return set_power(fp, 471, 288, sel, inc);
}

char BLE_RF_CODE ble5_txpwr_normal_set(void *fp, char sel, char inc)
{
    return set_power(fp, 338, 260, sel, inc);
}

#ifdef UNITTEST
#include "test.h"

struct common_rf_param rf;
int config_force_bt_pwr_tab_using_normal_level;

static u8 max_levels[8];
static unsigned int max_reads;
static s8 *callback_power;
static s8 callback_value;

u8 bt_get_pwr_max_level(void)
{
    CHECK(max_reads < sizeof(max_levels));
    if (callback_power) *callback_power = callback_value;
    return max_levels[max_reads++];
}

int main(void)
{
    _Alignas(4) u8 ble4[624] = {0};
    _Alignas(4) u8 ble5[3200] = {0};

    ble4[470] = 4;
    ble_agc_reset(ble4);
    CHECK(ble4[470] == 5);
    ble_agc_reset(ble4);
    CHECK(ble4[470] == 0);

    ble4[472] = 0x5a;
    ble5[339] = 0x5a;
    CHECK(ble_agc_normal_set(ble4, 1, 1) == 0 && ble4[472] == 0x5a);
    CHECK(ble5_agc_normal_set(ble5, 1, 1) == 0 && ble5[339] == 0x5a);

    struct ble_param frame = {0};
    frame.agc_dat_wr = (7u << 16) | (5u << 8);
    CHECK(ble_analog_get_rssi(&frame) == (char)(s8)(((5 | 192) ^ 63) - 7));

    rf.ble_fix_pwr = 255;
    rf.ble_pwr_max = 15;
    max_reads = 0;
    max_levels[0] = 9;
    CHECK(ble_txpwr_normal_set(ble4, 0, -1) == 1);
    CHECK((s8)ble4[471] == 0 && max_reads == 0);
    CHECK(*(volatile u32 *)(ble4 + 288) == 0);

    max_reads = 0;
    max_levels[0] = 5;
    max_levels[1] = 3;
    CHECK(ble_txpwr_normal_set(ble4, 0, 8) == 2);
    CHECK((s8)ble4[471] == 3 && max_reads == 2);
    CHECK(*(volatile u32 *)(ble4 + 288) == 3);

    config_force_bt_pwr_tab_using_normal_level = 2;
    max_reads = 0;
    max_levels[0] = 15;
    CHECK(ble5_txpwr_normal_set(ble5, 0, 8) == 0);
    CHECK((s8)ble5[338] == 2 && max_reads == 1);
    CHECK(*(volatile u32 *)(ble5 + 260) == 2);

    config_force_bt_pwr_tab_using_normal_level = 0;
    max_reads = 0;
    max_levels[0] = 9;
    CHECK(ble5_txpwr_normal_set(ble5, 1, 0) == 0);
    CHECK((s8)ble5[338] == 1 && max_reads == 1);
    CHECK(*(volatile u32 *)(ble5 + 260) == 1);
    /* SDK reload after callback; BLE4 sign extends, BLE5 zero extends. */
    callback_power = (s8 *)ble4 + 471;
    callback_value = -2;
    max_reads = 0;
    max_levels[0] = 15;
    CHECK(ble_txpwr_normal_set(ble4, 0, 2) == 0);
    CHECK(ble4[471] == 254 && *(u32 *)(ble4 + 288) == 0xfffffffeu);
    callback_power = (s8 *)ble5 + 338;
    max_reads = 0;
    CHECK(ble5_txpwr_normal_set(ble5, 0, 2) == 0);
    CHECK(ble5[338] == 254 && *(u32 *)(ble5 + 260) == 254);
    callback_power = 0;
    return TEST_DONE("btctrler_bd19_analog_ble");
}
#endif
