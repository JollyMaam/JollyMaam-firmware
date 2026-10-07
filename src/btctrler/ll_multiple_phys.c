/* BLE PHY negotiation and HCI PHY control. Keeps the SDK's feature masks,
 * opcode sequencing, instant arithmetic, pointer-mask quirk and Coded PHY
 * window-size values. */
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern BTCTRLER_CONFIG u8 log_tag_const_i_LL_PHY, log_tag_const_e_LL_PHY;
extern BTCTRLER_CONFIG int config_vendor_le_bb;
extern u8 ll_set_code_type;
extern void log_print(int level, const char *tag, const char *format, ...);

#define LL_CODE __attribute__((section(".ble_ll_code")))
#define LL_CONST __attribute__((section(".ble_ll_const")))

#ifndef UNITTEST
_Static_assert(offsetof(struct le_link, phy_param) == 310, "SDK link PHY params offset");
_Static_assert(offsetof(struct ble_phy_update, M_TO_S_PHY) == 0, "SDK M_TO_S PHY offset");
_Static_assert(offsetof(struct ble_phy_update, S_TO_M_PHY) == 1, "SDK S_TO_M PHY offset");
#endif

static const u16 master_passivity_accept_phy_update_steps[3] LL_CONST = {24, (u16)-32768, 4863};
static const u16 slave_passivity_accept_phy_update_steps[3] LL_CONST = {23, 16408, 5375};
static const u16 master_initiative_request_phy_update_steps[5] LL_CONST = {22, 16407, 24, (u16)-32768, 4607};
static const u16 slave_initiative_request_phy_update_steps[3] LL_CONST = {22, 16408, 5119};

static u8 get_if_MasterInitiativePhyUpdate(const struct le_link *link) LL_CODE;
static void __phy_update(struct le_link *link) LL_CODE;
static void __master_ll_phy_update_procedure_done(struct le_link *link) LL_CODE;
static void __master_ll_send_phy_update_ind_callback(void *priv) LL_CODE;
static u8 get_if_master_phy_change(const struct le_link *link) LL_CODE;
static void phy_negotiation(struct le_link *link, const u8 *data) LL_CODE;
static u8 get_if_SlaveInitiativePhyUpdate(const struct le_link *link) LL_CODE;
static void __slave_ll_phy_update_procedure_done(struct le_link *link) LL_CODE;

u8 llValidatePhySupport(u8 txPhys, u8 rxPhys) LL_CODE;
u8 ll_get_preferred_phy(u8 phys) LL_CODE;
void __master_ll_phy_update_done(struct le_link *link, struct ble_rx *rx) LL_CODE;
void __master_ll_send_phy_update_ind(struct le_link *link) LL_CODE;
void master_ll_receive_phy_rsp(struct le_link *link, u8 *data) LL_CODE;
void master_ll_receive_phy_req(struct le_link *link, struct ble_rx *rx) LL_CODE;
void __slave_ll_phy_update_done(struct le_link *link, struct ble_rx *rx) LL_CODE;
void slave_ll_receive_phy_req(struct le_link *link, struct ble_rx *rx) LL_CODE;
void __reset_CodedPHY_conn_winsize(struct le_link *link) LL_CODE;
void refresh_phy_param(struct le_link *link, u8 *TX_PHY, u8 *RX_PHY) LL_CODE;
void __ll_send_phy_req(struct le_link *link) LL_CODE;
void ll_set_default_prefers_phys(void) LL_CODE;
u8 ll_read_phy(u16 connection_handle, u8 *tx_phy, u8 *rx_phy) LL_CODE;
u8 ll_set_phy(u16 connection_handle) LL_CODE;

u8 llValidatePhySupport(u8 txPhys, u8 rxPhys)
{
    u64 features = config_btctler_le_features;
    if ((features & 256) == 0 && ((rxPhys | txPhys) & 2)) return 0;
    if ((features & 2048) == 0 && ((rxPhys | txPhys) & 4)) return 0;
    return 1;
}

u8 ll_get_preferred_phy(u8 phys)
{
    if (phys & 2) return 2;
    return (phys & 1) ? 1 : (phys & 4);
}

static u8 get_if_MasterInitiativePhyUpdate(const struct le_link *link)
{
    return link->procedure_fifo[link->fifo_rd].steps_ptr == master_initiative_request_phy_update_steps;
}

static u8 get_if_master_phy_change(const struct le_link *link)
{
    return link->phy_param.M_TO_S_PHY != 0 || link->phy_param.S_TO_M_PHY != 0;
}

static void __master_ll_phy_update_procedure_done(struct le_link *link)
{
    le_phy_update_complete_event(link, 0, 1);
}

static void __master_ll_send_phy_update_ind_callback(void *priv)
{
    __ble_ops->ioctrl(*(void **)priv, 18, 1);
}

static void __phy_update(struct le_link *link)
{
    u8 coded_s = ll_set_code_type;
    if (ext_le_param.phy_update_param.phy_options == 1) coded_s = 1;
    else if (ext_le_param.phy_update_param.phy_options == 2) coded_s = 0;
    u8 m_to_s = link->phy_param.S_TO_M_PHY;
    u8 s_to_m = link->phy_param.M_TO_S_PHY;
    if (link->role == 1)
        __ble5_ops->ext_ioctrl(link->hw, 19, m_to_s, s_to_m, coded_s);
    else
        __ble5_ops->ext_ioctrl(link->hw, 19, s_to_m, m_to_s, coded_s);
    __ble_ops->ioctrl(link->hw, 18, 0);
}

void __master_ll_phy_update_done(struct le_link *link, struct ble_rx *rx)
{
    if ((config_btctler_le_features & 2304) == 0) return;
    if (rx && rx->data[0] == 7) {
        le_phy_update_complete_event(link, 0, get_if_MasterInitiativePhyUpdate(link));
        return;
    }
    if (!get_if_master_phy_change(link)) {
        le_phy_update_complete_event(link, 0, get_if_MasterInitiativePhyUpdate(link));
        if (log_tag_const_i_LL_PHY)
            log_print(2, NULL, "[Info]: [LL_PHY]master shall not change phy\n\r\n");
        return;
    }
    u16 instant = link->instant;
    __ble_ops->ioctrl(link->hw, 4, instant, 2);
    __event_oneshot_add(link, __phy_update, instant - 1);
    __event_oneshot_post_add(link, __master_ll_phy_update_procedure_done, instant);
}

void __master_ll_send_phy_update_ind(struct le_link *link)
{
    u16 latency = link->ll_data.latency;
    u16 event_count = link->event_cnt;
    u32 delay = latency ? (u32)latency * 6 : (*(volatile u32 *)1981184 & 3);
    u32 candidate = (u32)event_count + 6 + delay;
    u32 adjusted = candidate + (config_vendor_le_bb == 0 ? 0 : 10);
    u8 change = get_if_master_phy_change(link);
    u16 instant = change ? (u16)adjusted : 0;
    link->instant = instant;
    ll_send_control_data_callback(link, change ? __master_ll_send_phy_update_ind_callback : NULL,
                                  24, "112", link->phy_param.M_TO_S_PHY,
                                  link->phy_param.S_TO_M_PHY, instant);
}

static void phy_negotiation(struct le_link *link, const u8 *data)
{
    u8 master_tx = data[0], master_rx = data[1];
    if (log_tag_const_i_LL_PHY) {
        log_print(2, NULL, "[Info]: [LL_PHY]self prefers_tx_phys=0x%x\n\r\n", ext_le_param.phy_update_param.prefers_tx_phys);
        log_print(2, NULL, "[Info]: [LL_PHY]self prefers_rx_phys=0x%x\n\r\n", ext_le_param.phy_update_param.prefers_rx_phys);
        log_print(2, NULL, "[Info]: [LL_PHY]slave prefers_tx_phys=0x%x\n\r\n", master_tx);
        log_print(2, NULL, "[Info]: [LL_PHY]slave prefers_rx_phys=0x%x\n\r\n", master_rx);
    }
    u8 m_to_s = ext_le_param.phy_update_param.prefers_tx_phys & master_rx;
    u8 s_to_m = ext_le_param.phy_update_param.prefers_rx_phys & master_tx;
    for (u8 bit = 0; bit != 3; ++bit) {
        u8 mask = (u8)(1u << bit);
        if (m_to_s & mask) { m_to_s = mask; break; }
    }
    for (u8 bit = 0; bit != 3; ++bit) {
        u8 mask = (u8)(1u << bit);
        if (s_to_m & mask) { s_to_m = mask; break; }
    }
    if (master_rx == master_tx && ext_le_param.phy_update_param.prefers_tx_phys != ext_le_param.phy_update_param.prefers_rx_phys) {
        u8 count = 0;
        for (u8 bit = 0; bit != 3; ++bit) count += (master_tx >> bit) & 1;
        if (count == 1) {
            link->phy_param.M_TO_S_PHY = 0;
            link->phy_param.S_TO_M_PHY = 0;
            return;
        }
    }
    link->phy_param.M_TO_S_PHY = (m_to_s == link->phy_param.M_TO_S_PHY) ? 0 : m_to_s;
    link->phy_param.S_TO_M_PHY = (s_to_m == link->phy_param.S_TO_M_PHY) ? 0 : s_to_m;
}

void master_ll_receive_phy_rsp(struct le_link *link, u8 *data)
{
    if (config_btctler_le_features & 2304) phy_negotiation(link, data);
}

void master_ll_receive_phy_req(struct le_link *link, struct ble_rx *rx)
{
    if (config_btctler_le_features & 2304) {
        phy_negotiation(link, &rx->data[1]);
        ll_control_data_step_start(link, master_passivity_accept_phy_update_steps);
    } else {
        ll_send_control_data(link, 7, "1", rx->data[0]);
        if (log_tag_const_i_LL_PHY) log_print(2, NULL, "[Info]: [LL_PHY]Master Tx - LL_UNKNOWN_RSP\r\n");
    }
}

static u8 get_if_SlaveInitiativePhyUpdate(const struct le_link *link)
{
    return link->procedure_fifo[link->fifo_rd].steps_ptr == slave_initiative_request_phy_update_steps;
}

static void __slave_ll_phy_update_procedure_done(struct le_link *link)
{
    le_phy_update_complete_event(link, 0, 1);
}

void __slave_ll_phy_update_done(struct le_link *link, struct ble_rx *rx)
{
    if ((config_btctler_le_features & 2304) == 0) return;
    if (rx->data[0] == 7) {
        le_phy_update_complete_event(link, 0, get_if_SlaveInitiativePhyUpdate(link));
        return;
    }
    u8 *params = &rx->data[1];
    u16 instant = btctler_little_endian_read_16(params, 2);
    if (log_tag_const_i_LL_PHY) {
        log_print(2, NULL, "[Info]: [LL_PHY]instant=%d\r\n", instant);
        log_print(2, NULL, "[Info]: [LL_PHY]self prefers_tx_phys=0x%x\n\r\n", ext_le_param.phy_update_param.prefers_tx_phys);
        log_print(2, NULL, "[Info]: [LL_PHY]self prefers_rx_phys=0x%x\n\r\n", ext_le_param.phy_update_param.prefers_rx_phys);
        log_print(2, NULL, "[Info]: [LL_PHY]master prefers_tx_phys=0x%x\n\r\n", params[0]);
        log_print(2, NULL, "[Info]: [LL_PHY]master prefers_rx_phys=0x%x\n\r\n", params[1]);
    }
    u8 tx = params[0], rx_phy = params[1];
    if ((tx | rx_phy) == 0 ||
        (tx == link->phy_param.M_TO_S_PHY && rx_phy == link->phy_param.S_TO_M_PHY) ||
        (tx && (ext_le_param.phy_update_param.prefers_rx_phys & tx) == 0) ||
        (rx_phy && (ext_le_param.phy_update_param.prefers_tx_phys & rx_phy) == 0)) {
        le_phy_update_complete_event(link, 0, get_if_SlaveInitiativePhyUpdate(link));
        if (log_tag_const_i_LL_PHY) log_print(2, NULL, "[Info]: [LL_PHY]slave shall not change phy\r\n");
        return;
    }
    link->phy_param.M_TO_S_PHY = tx;
    if (__instant_link_lost(instant, rx->event_count) == 1) {
        if (log_tag_const_e_LL_PHY) log_print(4, NULL, "<Error>: [LL_PHY]- INSTANT_PASSED\r\n");
        __phy_update(link);
        __slave_ll_phy_update_procedure_done(link);
        return;
    }
    __ble_ops->ioctrl(link->hw, 4, instant, 2);
    __event_oneshot_add(link, __phy_update, instant - 1);
    __event_oneshot_post_add(link, __slave_ll_phy_update_procedure_done, instant);
}

void slave_ll_receive_phy_req(struct le_link *link, struct ble_rx *rx)
{
    if ((config_btctler_le_features & 2304) && rx->len == 3) {
        ll_send_control_data(link, 23, "11", ext_le_param.phy_update_param.prefers_tx_phys,
                             ext_le_param.phy_update_param.prefers_rx_phys);
        ll_control_data_step_start(link, slave_passivity_accept_phy_update_steps);
        ll_response_timeout_start(link);
        if (log_tag_const_i_LL_PHY) log_print(2, NULL, "[Info]: [LL_PHY]Slave Tx - LL_PHY_RSP\r\n");
    } else {
        ll_send_control_data(link, 7, "1", rx->data[0]);
        if (log_tag_const_i_LL_PHY) log_print(2, NULL, "[Info]: [LL_PHY]Slave Tx - LL_UNKNOWN_RSP\r\n");
    }
}

void __reset_CodedPHY_conn_winsize(struct le_link *link)
{
    if ((config_btctler_le_features & 2048) == 0) return;
    u8 rx_phy = 0;
    __ble5_ops->ext_ioctrl(link->hw, 18, 0, &rx_phy);
    if (rx_phy != 2) return;
    if (link->role == 1)
        __ble_ops->ioctrl(link->hw, 5, 520, (link->ll_data.widening << 1) + 200);
    else
        __ble_ops->ioctrl(link->hw, 5, 390, 0);
}

void refresh_phy_param(struct le_link *link, u8 *TX_PHY, u8 *RX_PHY)
{
    if ((config_btctler_le_features & 2304) == 0) return;
    u8 tx_phy = 0, rx_phy = 0;
    __ble5_ops->ext_ioctrl(link->hw, 18, &tx_phy, &rx_phy);
    if ((((u32)(uintptr_t)TX_PHY & 65535) & (u32)(uintptr_t)RX_PHY) != 0) {
        *TX_PHY = tx_phy;
        *RX_PHY = rx_phy;
    }
    u8 tx_value = tx_phy, rx_value = rx_phy;
    link->phy_param.M_TO_S_PHY = (u8)(1 << (link->role == 1 ? rx_value : tx_value));
    link->phy_param.S_TO_M_PHY = (u8)(1 << (link->role == 1 ? tx_value : rx_value));
    ext_le_param.phy_update_param.prefers_tx_phys = ext_le_param.default_prefers_phys.tx_phys;
    ext_le_param.phy_update_param.prefers_rx_phys = ext_le_param.default_prefers_phys.rx_phys;
}

void __ll_send_phy_req(struct le_link *link)
{
    if (config_btctler_le_features & 2304)
        ll_send_control_data(link, 22, "11", ext_le_param.phy_update_param.prefers_tx_phys,
                             ext_le_param.phy_update_param.prefers_rx_phys);
}

void ll_set_default_prefers_phys(void)
{
    u64 features = config_btctler_le_features;
    if ((features & 2304) == 0) return;
    u8 phy = (features & 256) ? 3 : 1;
    if (features & 2048) phy |= 4;
    ext_le_param.default_prefers_phys.tx_phys = phy;
    ext_le_param.default_prefers_phys.rx_phys = phy;
    ext_le_param.phy_update_param.prefers_tx_phys = phy;
    ext_le_param.phy_update_param.prefers_rx_phys = phy;
}

u8 ll_read_phy(u16 connection_handle, u8 *tx_phy, u8 *rx_phy)
{
    struct le_link *link = ll_link_for_handle(connection_handle);
    if (!link) {
        if (log_tag_const_e_LL_PHY) log_print(4, NULL, "<Error>: [LL_PHY]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
        return 2;
    }
    __ble5_ops->ext_ioctrl(link->hw, 18, tx_phy, rx_phy);
    return 0;
}

u8 ll_set_phy(u16 connection_handle)
{
    struct le_link *link = ll_link_for_handle(connection_handle);
    if (!link) {
        if (log_tag_const_e_LL_PHY) log_print(4, NULL, "<Error>: [LL_PHY]UNKNOWN_CONNECTION_IDENTIFIER\r\n");
        return 2;
    }
    ll_response_timeout_start(link);
    if (link->role == 1) {
        if (log_tag_const_i_LL_PHY) log_print(2, NULL, "[Info]: [LL_PHY]\n--func=%s\n\r\n", "le_slave_phy_update");
        ll_control_data_step_start(link, slave_initiative_request_phy_update_steps);
    } else {
        if (log_tag_const_i_LL_PHY) log_print(2, NULL, "[Info]: [LL_PHY]\n--func=%s\n\r\n", "le_master_phy_update");
        ll_control_data_step_start(link, master_initiative_request_phy_update_steps);
    }
    return 0;
}

#ifdef UNITTEST
#include "test.h"
u64 config_btctler_le_features;
struct ext_le_parameter ext_le_param;
int main(void)
{
    config_btctler_le_features = 0;
    CHECK(llValidatePhySupport(1, 1) == 1);
    CHECK(llValidatePhySupport(2, 0) == 0);
    CHECK(llValidatePhySupport(4, 0) == 0);
    config_btctler_le_features = 256;
    CHECK(llValidatePhySupport(2, 0) == 1);
    CHECK(llValidatePhySupport(4, 0) == 0);
    config_btctler_le_features = 2048;
    CHECK(llValidatePhySupport(4, 0) == 1);
    CHECK(ll_get_preferred_phy(7) == 2);
    CHECK(ll_get_preferred_phy(5) == 1);
    CHECK(ll_get_preferred_phy(4) == 4);
    CHECK(ll_get_preferred_phy(0) == 0);

    config_btctler_le_features = 2304;
    ll_set_default_prefers_phys();
    CHECK(ext_le_param.default_prefers_phys.tx_phys == 7);
    CHECK(ext_le_param.default_prefers_phys.rx_phys == 7);
    CHECK(ext_le_param.phy_update_param.prefers_tx_phys == 7);
    CHECK(ext_le_param.phy_update_param.prefers_rx_phys == 7);
    config_btctler_le_features = 0;
    ll_set_default_prefers_phys();
    CHECK(ext_le_param.default_prefers_phys.tx_phys == 7);
    return TEST_DONE("btctrler_ll_multiple_phys");
}
#endif
