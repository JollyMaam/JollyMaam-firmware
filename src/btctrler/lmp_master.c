/* Owned lmp_master.c: the link manager of a connection we paged (we start
 * as central; a host usually switches roles).  lmp_master_machine runs the
 * stage functions until one returns other than 1. */
#include <string.h>
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

_Static_assert(BTCFG_HCI_STANDARD && !BTCFG_BREDR_MASTER && !BTCFG_FAST_CONNECT &&
               !BTCFG_MASTER_ESCO && !BTCFG_TWS,
               "written for a standard HCI host without forced central, fast connect, voice or TWS");

/* Connection setup: features, version, extended features, host connection
 * request, setup complete, link parameters, then authentication. */
int lmp_master_stage_conn(struct lmp_connection *conn, u8 opcode0, u8 opcode1, u8 opcode2,
                          struct bredr_rx_bulk *rx, int *hci_cmd)
{
    struct lmp_core_data *core = &conn->core;
    (void)opcode2; (void)rx;
    switch (core->step) {
    case LMP_TX_GET_REMOTE_ADDR_REQ:        /* fast connect only */
    case LMP_WAIT_GET_REMOTE_ADDR_RES:
    case LMP_TX_FEATURES_REQ:
        lmp_tx_features_req(conn);
        core->step = LMP_WAIT_FEATURES_RES;
        return 0;
    case LMP_WAIT_FEATURES_RES:
        if (opcode0 != PDU(FEATURES_RES)) return 0;
        /* fall through */
    case LMP_TX_VERSION_REQ:
        lmp_tx_version_req(conn);
        core->step = LMP_WAIT_VERSION_RES;
        return 0;
    case LMP_WAIT_VERSION_RES:
        if (opcode0 != PDU(VERSION_RES)) return 0;
        if (!(conn->core_s.remote_features[7] & 0x80)) {    /* no extended features */
            core->step = LMP_TX_CONNECTION_REQ;
            return 1;
        }
        /* fall through */
    case LMP_TX_FEATURES_REQ_EXT:
        lmp_tx_features_req_ext(conn, 1);
        core->step = LMP_WAIT_FEATURES_RES_EXT;
        return 0;
    case LMP_WAIT_FEATURES_RES_EXT:
        if (opcode1 != FEATURES_RES_EXT) return 0;
        /* fall through */
    case LMP_TX_CONNECTION_REQ:
        lmp_tx_connection_req(conn);
        core->step = LMP_WAIT_ACCEPT_CONNECTION_REQ;
        return 0;
    case LMP_WAIT_ACCEPT_CONNECTION_REQ:
        if (opcode1 != PDU(HOST_CONNECTION_REQ)) return 0;
        if (opcode0 == PDU(NOT_ACCEPTED)) return 0;
        core->state |= 0x4000;
        /* fall through */
    case LMP_WAIT_SETUP_COMPLETE:
        if (core->step == LMP_WAIT_SETUP_COMPLETE) {
            if (opcode0 != PDU(SETUP_COMPLETE)) return 0;
            lmp_tx_max_slot(conn);
            lmp_send_event_link_request(conn);
            core->step = LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY;
            return 0;
        }
        /* fall through */
    case LMP_TX_SUPERVISION_TIMEOUT:
        lmp_tx_supervision_timeout(conn);
        /* fall through */
    case LMP_TX_TIMING_ACCURACY:
        lmp_tx_timing_accuracy_req(conn);
        core->step = LMP_WAIT_TIMING_ACCURACY_RES;
        return 0;
    case LMP_WAIT_TIMING_ACCURACY_RES:      /* sic: any PDU moves on */
    case LMP_TX_MAX_SLOT:
        lmp_tx_max_slot(conn);
        /* fall through */
    case LMP_TX_MAX_SLOT_REQ:
        lmp_tx_max_slot_req(conn, conn->link->core.max_slot);
        core->step = LMP_WAIT_ACCEPT_MAX_SLOT_REQ;
        return 0;
    case LMP_WAIT_ACCEPT_MAX_SLOT_REQ:
        if (opcode1 != PDU(MAX_SLOT_REQ)) return 0;
        /* fall through */
    case LMP_TX_PACKET_TYPE_TABLE_REQ:
        if (link_conn_get_ptt(conn->link) == 1) {
            core->step = LMP_TX_HCI_EVENT_LINK_KEY_REQ;
            return 1;
        }
        lmp_tx_packet_type_table_req(conn);
        core->step = LMP_WAIT_ACCEPT_PACKET_TYPE_TABLE_REQ;
        return 0;
    case LMP_WAIT_ACCEPT_PACKET_TYPE_TABLE_REQ:     /* sic: any PDU moves on */
        core->step = LMP_TX_HCI_EVENT_LINK_KEY_REQ;
        /* fall through */
    case LMP_TX_HCI_EVENT_LINK_KEY_REQ:
        if (core->role_switch) {            /* the host wants us peripheral */
            core->role_switch = 0;
            if (!conn->rs_process || !conn->rs_process->instant) {
                lmp_master_tx_role_switch_req(conn);
                core->step = LMP_WAIT_ROLE_SWITCH_COMPLETE;
            }
            return 0;
        }
        if (!conn->rs_process) *hci_cmd = 0x0411;  /* as if the host asked to authenticate */
        if (opcode0 == PDU(NOT_ACCEPTED) && opcode1 == PDU(SWITCH_REQ)) {
            core->enc_state |= 0x100;
            hci_send_event(0x17, "A", conn->core_s.remote_addr);   /* Link Key Request */
            core->step = LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY;
        }
        if (!conn->rs_process && *hci_cmd == 0x0411) {
            lmp_send_event_link_request(conn);
            core->step = LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY;
        }
        return 0;
    case LMP_WAIT_ROLE_SWITCH_COMPLETE:
        if (opcode0 == PDU(NOT_ACCEPTED) && opcode1 == PDU(SWITCH_REQ)) {
            core->enc_state |= 0x100;
            hci_send_event(0x17, "A", conn->core_s.remote_addr);
            core->step = LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY;
        }
        return 0;
    case LMP_WAIT_HCI_EVENT_LINK_KEY_REPLY:
        if (*hci_cmd == 0x040b) {               /* Link Key Request Reply */
            u16 was = core->enc_state;
            core->enc_state |= 1;
            memcpy(core->link_key, hci_cmd + 1, 16);
            if (was & 0x80) {
                core->step = LMP_TX_SRES;
                core->stage = LMP_STAGE_AUTH_WITH_LINK_KEY_BY_REMOTE;
            } else {
                core->step = LMP_TX_AU_RAND;
                core->stage = LMP_STAGE_AUTH_WITH_LINK_KEY_BY_LOCAL;
            }
            return 1;
        }
        if (*hci_cmd == 0x040c) {               /* negative reply: pair */
            if (lmp_simple_pair_enbale(conn)) {
                core->step = LMP_TX_IO_CAPABILITY_REQ;
                core->stage = LMP_STAGE_AUTH_WITH_SIMPLE_PAIR_BY_LOCAL;
            } else {
                core->step = LMP_TX_PIN_CODE_REQ;
                core->stage = LMP_STAGE_AUTH_WITH_PIN_CODE_BY_LOCAL;
            }
            return 1;
        }
        return 0;
    }
    return 0;
}

int lmp_master_stage_connected(struct lmp_connection *conn, u8 opcode0, u8 opcode1, u8 opcode2,
                               struct bredr_rx_bulk *rx, int *hci_cmd)
{
    struct lmp_core_data *core = &conn->core;
    (void)opcode2; (void)rx; (void)hci_cmd;
    switch (core->step) {
    case LMP_TX_NULL:
        if (core->role_switch) {
            core->role_switch = 0;
            if (core->enc_state & 4) {
                core->step = LMP_TX_PAUSE_ENCRYPTION_REQ;
                core->stage = LMP_STAGE_ROLE_SWITCH_ON_ENC;
                return 1;
            }
            lmp_master_tx_role_switch_req(conn);
            core->step = LMP_WAIT_SLOT_OFFSET;
        }
        /* Fix: an SCO link request returned 1 without a new stage, which
         * spun the machine forever when voice links are disabled. */
        if (opcode0 == PDU(AU_RAND)) lmp_free_encrypt(conn);
        break;
    case LMP_WAIT_SLOT_OFFSET:
        if (opcode0 == PDU(SLOT_OFFSET)) {
            core->step = LMP_WAIT_ACCEPT_ROLE_SWITCH;
        } else if (opcode0 == PDU(NOT_ACCEPTED) && opcode1 == PDU(SWITCH_REQ)) {
            core->role_switch = 0;
            core->step = LMP_TX_NULL;
        }
        break;
    case LMP_TX_STOP_ENCRYPTION_REQ:
        lmp_tx_stop_encryption_req(conn);
        core->step = LMP_WAIT_ACCEPT_STOP_ENCRYPTION_REQ;
        break;
    case LMP_WAIT_ACCEPT_STOP_ENCRYPTION_REQ:
        if (opcode1 != PDU(STOP_ENCRYPTION_REQ)) {
            core->step = LMP_TX_NULL;
        } else if (opcode0 == PDU(NOT_ACCEPTED)) {
            btctrler_assert_failed("stop encryption refused", 0);
        }
        break;
    }
    return 0;
}

int lmp_master_machine(struct lmp_connection *conn, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    u8 opcode0 = 0, opcode1 = 0, opcode2 = 0;
    int ret = 0;
    if (rx) lmp_acl_c_handler(conn, rx, &opcode0, &opcode1, &opcode2);
    do {
        switch (conn->core.stage) {
        case LMP_STAGE_CONN:
            ret = lmp_master_stage_conn(conn, opcode0, opcode1, 0, 0, hci_cmd);
            break;
        case LMP_STAGE_AUTH_WITH_PIN_CODE_BY_LOCAL:
            lmp_stage_auth_with_pin_code(conn, opcode0, opcode1, 0, rx, hci_cmd);
            return 0;
        case LMP_STAGE_AUTH_WITH_SIMPLE_PAIR_BY_LOCAL:
            ret = lmp_stage_auth_with_simple_pair_by_local(conn, opcode0, opcode1, 0, rx, hci_cmd);
            break;
        case LMP_STAGE_AUTH_WITH_SIMPLE_PAIR_BY_REMOTE:
            lmp_stage_auth_with_simple_pair_by_remote(conn, opcode0, opcode1, 0, rx, hci_cmd);
            return 0;
        case LMP_STAGE_AUTH_WITH_LINK_KEY_BY_LOCAL:
            lmp_stage_auth_with_link_key_by_local(conn, opcode0, 0, 0, 0, 0);
            return 0;
        case LMP_STAGE_ENC_START_BY_LOCAL:
            ret = lmp_master_stage_enc_start_by_local(conn, opcode0, opcode1, opcode2, 0, hci_cmd);
            break;
        case LMP_STAGE_AUTH_WITH_LINK_KEY_BY_REMOTE:
            ret = lmp_stage_auth_with_link_key_by_remote(conn, opcode0, 0, 0, 0, hci_cmd);
            break;
        case LMP_STAGE_ENC_START_BY_REMOTE:
            ret = lmp_slave_stage_enc_start_by_remote(conn, opcode0, opcode1, 0, 0, 0);
            break;
        case LMP_STAGE_CONNECTED:
            ret = lmp_master_stage_connected(conn, opcode0, opcode1, 0, rx, hci_cmd);
            break;
        case LMP_STAGE_ROLE_SWITCH_ON_ENC:
            lmp_master_stage_role_switch_on_enc(conn, opcode0, opcode1, 0, 0, 0);
            return 0;
        case LMP_STAGE_REFRESHING_ENCRYPTION_KEY_BY_LOCAL:
            lmp_master_stage_refreshing_the_encryption_key(conn, opcode0, opcode1, 0, 0, 0);
            return 0;
        default:
            /* Fix: the SDK kept ret here and spun forever after a 1
             * (voice stages, link key change). */
            ret = 0;
            break;
        }
    } while (ret == 1);
    return ret;
}

void lmp_master_init(struct lmp_connection *conn)
{
    int hci_cmd = 0;
    conn->core.stage = LMP_STAGE_CONN;
    conn->core.step = LMP_TX_FEATURES_REQ;
    lmp_master_machine(conn, 0, &hci_cmd);
}

/* LMP_set_AFH at an instant 200 slots ahead (even, within the clock's
 * half range), if the peripheral supports AFH. */
void lmp_tx_set_afh_req(struct lmp_connection *conn, u8 *channel_map)
{
    struct link_connection *link = conn->link;
    bredr_read_slot_clk();
    u32 clk = bredr_get_master_slot_clk(link);
    if (conn->core.role != 0 || !(conn->core_s.remote_features[4] & 8)) return;
    u32 instant = (clk + 200) & 0x0ffffffe;
    if (instant > 0x7ffffff) instant = (instant + 0x8000000) & 0x0ffffffe;
    link->afh.en = 1;
    link->afh.instant = instant;
    memcpy(link->channel_map, channel_map, 10);
    lmp_request(conn, "A141C010", PDU(SET_AFH), instant, 1, channel_map);
}
