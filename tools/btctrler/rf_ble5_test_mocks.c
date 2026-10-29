/* Host-only link mocks for the RF_ble5 UNITTEST fixture.
 *
 * Pure helpers and the stateful test hooks live beside the implementation.
 * These definitions close the SDK link surface so the fixture can be linked
 * with --gc-sections. Any hardware path that unexpectedly survives section
 * collection fails loudly instead of inventing controller behavior.
 */
#include "btctrler/glue.h"

#include <stdio.h>
#include <stdlib.h>

int config_asser;
struct ble_baseband_layer_handle *bb_hdl;

/* Tests can set the return value and assert which BLE5 hardware frame was
 * sampled without replacing the SDK's differently typed declaration. */
unsigned rf_ble5_mock_rssi_calls;
struct ble5_param *rf_ble5_mock_rssi_frame;
int rf_ble5_mock_rssi_value;
unsigned rf_ble5_mock_anchor_disable_calls;
struct ble_hw *rf_ble5_mock_anchor_disable_hw;
u32 rf_ble5_mock_ble_to_phy_result;
unsigned rf_ble5_mock_ble_to_phy_calls;
u32 rf_ble5_mock_ble_to_phy_input;

static void unexpected_call(const char *name)
{
    fprintf(stderr, "unexpected RF_ble5 test mock call: %s\n", name);
    abort();
}

/* Allocators are real host allocations because owned code may legitimately
 * allocate buffers. Memory contents remain uninitialized, as with malloc. */
void *btctler_nv_memory_malloc(int size) { return malloc((size_t)size); }
void btctler_nv_memory_free(void *p) { free(p); }
void *__bt_malloc(int size) { return malloc((size_t)size); }
void __bt_free(void *p) { free(p); }

/* The recovered source declares the BD19 SDK symbol with its alternate
 * frame type and int return. Give that assembler symbol a distinct C name so
 * this translation unit can retain the generated SDK prototype as well. */
int rf_ble5_mock_analog_get_rssi(struct ble5_param *frame)
    __asm__("ble_analog_get_rssi");
int rf_ble5_mock_analog_get_rssi(struct ble5_param *frame)
{
    ++rf_ble5_mock_rssi_calls;
    rf_ble5_mock_rssi_frame = frame;
    return rf_ble5_mock_rssi_value;
}

u8 HW_ID(struct ble_hw *hw) { (void)hw; unexpected_call("HW_ID"); return 0; }
u32 PHY_TO_BLE(u8 *phy) { (void)phy; unexpected_call("PHY_TO_BLE"); return 0; }
u32 BLE_TO_PHY(u32 addr)
{
    ++rf_ble5_mock_ble_to_phy_calls;
    rf_ble5_mock_ble_to_phy_input = addr;
    return rf_ble5_mock_ble_to_phy_result;
}

void __disable_adv_random(struct ble_hw *hw) { (void)hw; unexpected_call("__disable_adv_random"); }
void __set_adv_channel_index(struct ble_hw *hw, u8 map) { (void)hw; (void)map; unexpected_call("__set_adv_channel_index"); }
void __set_anchor_cnt(struct ble_hw *hw, u32 slot) { (void)hw; (void)slot; unexpected_call("__set_anchor_cnt"); }
void __set_anchor_cnt_disable(struct ble_hw *hw)
{
    ++rf_ble5_mock_anchor_disable_calls;
    rf_ble5_mock_anchor_disable_hw = hw;
}
void __set_ble_hw_irq_init(struct ble_hw *hw) { (void)hw; unexpected_call("__set_ble_hw_irq_init"); }
void __set_conn_dead_times(struct ble_hw *hw) { (void)hw; unexpected_call("__set_conn_dead_times"); }
void __set_interval(struct ble_hw *hw, int interval, int evt_en) { (void)hw; (void)interval; (void)evt_en; unexpected_call("__set_interval"); }
void __set_widen(struct ble4_hw *hw, int widen) { (void)hw; (void)widen; unexpected_call("__set_widen"); }
void __set_winsize(struct ble4_hw *hw, int winsize, int nwinsize) { (void)hw; (void)winsize; (void)nwinsize; unexpected_call("__set_winsize"); }

void ble4_hw_only_alloc_dma_ram(struct ble_hw *hw) { (void)hw; unexpected_call("ble4_hw_only_alloc_dma_ram"); }
char ble5_agc_normal_set(void *fp, char sel, char inc) { (void)fp; (void)sel; (void)inc; unexpected_call("ble5_agc_normal_set"); return 0; }
char ble5_txpwr_normal_set(void *fp, char sel, char inc) { (void)fp; (void)sel; (void)inc; unexpected_call("ble5_txpwr_normal_set"); return 0; }
struct ble_hw *ble_hw_alloc(void) { unexpected_call("ble_hw_alloc"); return NULL; }
void ble_hw_disable(struct ble_hw *hw) { (void)hw; unexpected_call("ble_hw_disable"); }
void ble_hw_enable(struct ble_hw *hw, int slot) { (void)hw; (void)slot; unexpected_call("ble_hw_enable"); }
void ble_hw_inst_set(u8 id, void *fp) { (void)id; (void)fp; unexpected_call("ble_hw_inst_set"); }
void ble_hw_rx(struct ble_hw *hw, struct ble_rx *rx, int index) { (void)hw; (void)rx; (void)index; unexpected_call("ble_hw_rx"); }
void ble_power_get(struct ble_hw *hw) { (void)hw; unexpected_call("ble_power_get"); }
void ble_power_put(struct ble_hw *hw) { (void)hw; unexpected_call("ble_power_put"); }
void ble_rx_init(struct ble4_hw *hw) { (void)hw; unexpected_call("ble_rx_init"); }
void ble_tx_init(struct ble4_hw *hw) { (void)hw; unexpected_call("ble_tx_init"); }
u8 bt_get_pwr_max_level(void) { unexpected_call("bt_get_pwr_max_level"); return 0; }
void hw_dma_clear(struct _dma_head *head) { (void)head; unexpected_call("hw_dma_clear"); }
void *hw_dma_pop(struct _dma_head *head) { (void)head; unexpected_call("hw_dma_pop"); return NULL; }
int hw_dma_empty(struct _dma_head *head) { (void)head; unexpected_call("hw_dma_empty"); return 1; }
void *hw_dma_alloc(u8 type, u32 size) { (void)type; return malloc((size_t)size); }
void *hw_entity_alloc(void) { unexpected_call("hw_entity_alloc"); return NULL; }
void hw_entity_free(void *hw) { (void)hw; unexpected_call("hw_entity_free"); }
void le_hw_close(struct ble_hw *hw) { (void)hw; unexpected_call("le_hw_close"); }
int le_hw_initiating_common(struct ble_hw *hw, struct ble_conn *conn) { (void)hw; (void)conn; unexpected_call("le_hw_initiating_common"); return 0; }
int le_hw_scanning_common(struct ble_hw *hw, struct ble_scan *scan) { (void)hw; (void)scan; unexpected_call("le_hw_scanning_common"); return 0; }
void p33_soft_reset(void) { unexpected_call("p33_soft_reset"); }
void cpu_assert_debug(void) { unexpected_call("cpu_assert_debug"); }
struct ble_rx *hw_alloc_rx(int len) { (void)len; unexpected_call("hw_alloc_rx"); return NULL; }
u8 check_event_pending(void) { unexpected_call("check_event_pending"); return 0; }
uint32_t btctler_get_rand_from_assign_range(uint32_t rand, uint32_t min,
                                             uint32_t max)
{
    (void)rand; (void)min; (void)max;
    unexpected_call("btctler_get_rand_from_assign_range");
    return 0;
}
void pseudo_random_genrate(uint8_t *dest, unsigned int size)
{
    (void)dest; (void)size;
    unexpected_call("pseudo_random_genrate");
}
