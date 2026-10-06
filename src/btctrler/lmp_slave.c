/* Owned lmp_slave.c: the link manager of a connection the peer paged
 * (we start as peripheral).  lmp_slave_machine runs the stage functions
 * until one returns other than 1 ("go on with the new stage"). */
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

_Static_assert(BTCFG_HCI_STANDARD && !BTCFG_BREDR_MASTER && !BTCFG_TWS,
               "written for a standard HCI host, no forced central, no TWS");

void lmp_slave_init(struct lmp_connection *conn)
{
    conn->core.stage = LMP_STAGE_CONN;
    conn->core.step = LMP_WAIT_HOST_CONNECTION_REQ;
}

static int stage_conn(struct lmp_connection *conn, u8 opcode0, u8 opcode1, int *hci_cmd)
{
    switch (conn->core.step) {
    case LMP_WAIT_HOST_CONNECTION_REQ:
        if (opcode0 != PDU(HOST_CONNECTION_REQ)) break;
        lmp_send_event_connection_request(conn, 1);
        conn->core.step = LMP_WAIT_HOST_ACCEPTED_CONNECTION_REQ;
        break;
    case LMP_WAIT_HOST_ACCEPTED_CONNECTION_REQ:
        if (hci_cmd[0] != 0x0409) break;           /* Accept Connection Request */
        if (hci_cmd[1] == 0) {                      /* the host wants to be central */
            lmp_tx_role_switch_req(conn);
            break;
        }
        conn->tran_id = conn->core.role == 0;
        lmp_response(conn, "11", PDU(ACCEPTED), HOST_CONNECTION_REQ);
        conn->core.step = LMP_WAIT_AUTH;
        lmp_setup_complete(conn);
        if (link_conn_get_ptt(conn->link) != 1) lmp_tx_packet_type_table_req(conn);
        break;
    case LMP_WAIT_AUTH:
        if (opcode1 == IO_CAPABILITY_REQ) {
            conn->core.step = LMP_WAIT_IO_CAPABILITY_REPLY;
            conn->core.stage = LMP_STAGE_AUTH_WITH_SIMPLE_PAIR_BY_REMOTE;
            return 1;
        }
        if (opcode0 == PDU(IN_RAND)) {
            conn->core.step = LMP_TX_PIN_CODE_REQ;
            conn->core.stage = LMP_STAGE_AUTH_WITH_PIN_CODE_BY_REMOTE;
            return 1;
        }
        if (opcode0 == PDU(AU_RAND)) {
            conn->core.step = LMP_TX_HCI_EVENT_LINK_KEY_REQ;
            conn->core.stage = LMP_STAGE_AUTH_WITH_LINK_KEY_BY_REMOTE;
            return 1;
        }
        break;
    }
    return 0;
}

int lmp_slave_stage_conn(struct lmp_connection *conn, u8 opcode0, u8 opcode1, u8 opcode2,
                         struct bredr_rx_bulk *rx, int *hci_cmd)
{
    (void)opcode2; (void)rx;
    return stage_conn(conn, opcode0, opcode1, hci_cmd);
}

/* Legacy pairing the peer started: PIN from the host, combination keys. */
int lmp_slave_stage_auth_with_pin_code(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                       u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    (void)opcode1; (void)opcode2;
    switch (conn->core.step) {
    case LMP_TX_PIN_CODE_REQ:
        hci_send_event(0x16, "A", conn->core_s.remote_addr);       /* PIN Code Request */
        conn->core.step = LMP_WAIT_PIN_CODE_REQ_REPLY;
        break;
    case LMP_WAIT_PIN_CODE_REQ_REPLY:
        if (hci_cmd[0] != 0x040d) break;                           /* PIN Code Request Reply */
        lmp_in_rand_res(conn, (u8 *)(hci_cmd + 2), (u8)hci_cmd[1]);
        conn->core.step = LMP_WAIT_COMB_KEY;
        break;
    case LMP_WAIT_COMB_KEY:
        if (opcode0 != PDU(COMB_KEY)) break;
        lmp_response_comb_key(conn, rx->data + 1);
        conn->core.step = LMP_WAIT_AU_RAND;
        break;
    case LMP_WAIT_AU_RAND:
        if (opcode0 != PDU(AU_RAND)) break;
        make_rand_num(conn->encrypt->rand_num);
        lmp_response(conn, "1c16", PDU(AU_RAND), conn->encrypt->rand_num);
        conn->core.step = LMP_WAIT_SRES;
        break;
    case LMP_WAIT_SRES:
        if (opcode0 != PDU(SRES)) break;
        if (conn->encrypt->sres_mach == 1) {
            conn->core.step = LMP_WAIT_ENCRYPTION_MODE_REQ;
            conn->core.stage = LMP_STAGE_ENC_START_BY_REMOTE;
            lmp_send_event_link_key_notification(conn, 0);
        }
        lmp_free_encrypt(conn);
        break;
    }
    return 0;
}

int lmp_slave_stage_connected(struct lmp_connection *conn, u8 opcode0, u8 opcode1, u8 opcode2,
                              struct bredr_rx_bulk *rx, int *hci_cmd)
{
    (void)opcode1; (void)opcode2; (void)hci_cmd;
    if (conn->core.role_switch) {       /* the host asked for a role switch */
        conn->core.role_switch = 0;
        if (conn->core.enc_state & 4) {     /* encrypted: pause encryption first */
            conn->core.step = LMP_TX_PAUSE_ENCRYPTION_REQ;
            conn->core.stage = LMP_STAGE_ROLE_SWITCH_ON_ENC;
            return 1;
        }
        lmp_master_tx_role_switch_req(conn);
        conn->core.step = LMP_WAIT_SLOT_OFFSET;
        conn->core.stage = LMP_STAGE_ROLE_SWITCH_ON_ENC;
    }
    switch (opcode0) {
    case PDU(AU_RAND):
        lmp_free_encrypt(conn);
        break;
    case PDU(COMB_KEY):
        /* Sic: the slave machine has no stage for this; it asserts. */
        conn->core.step = LMP_TX_COMB_KEY;
        conn->core.stage = LMP_STAGE_CHANGE_LINK_KEY_BY_REMOTE;
        return 1;
    /* SCO and eSCO link requests are refused on receipt (lmp_esco.c). */
    case PDU(ESCAPE_4):
        if (rx->data[1] == NOT_ACCEPTED_EXT && rx->data[2] == ESCO_LINK_REQ) {
            conn->core.step = LMP_WAIT_REMOVE_ESCO_LINK_RES;
            conn->core.stage = LMP_STAGE_CONNECTED;
        }
        break;
    }
    return 0;
}

/* A role switch the host asked for on an encrypted link: pause encryption,
 * switch, resume. */
int lmp_slave_stage_role_switch_on_enc(struct lmp_connection *conn, u8 opcode0, u8 opcode1,
                                       u8 opcode2, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    struct lmp_role_switch *rs;
    (void)opcode2; (void)rx; (void)hci_cmd;
    switch (conn->core.step) {
    case LMP_TX_PAUSE_ENCRYPTION_REQ:
        conn->core.enc_state |= 0x40;
        lmp_tx_pause_encryption_req(conn);
        conn->core.step = LMP_WAIT_STOP_ENCRYPTION_REQ;
        break;
    case LMP_WAIT_STOP_ENCRYPTION_REQ:
        if (opcode0 != PDU(STOP_ENCRYPTION_REQ)) break;
        /* fall through */
    case LMP_TX_ROLE_SWITCH:
        lmp_tx_role_switch_req(conn);
        break;
    case LMP_WAIT_ACCEPT_ROLE_SWITCH:
        if (opcode0 == PDU(NOT_ACCEPTED) && opcode1 == PDU(SWITCH_REQ)) {
            lmp_tx_resume_encryption_req(conn);
            conn->core.step = LMP_TX_NULL;
            conn->core.stage = LMP_STAGE_CONNECTED;
        }
        break;
    case LMP_WAIT_START_ENCRYPTION_REQ:
        if (opcode0 != PDU(START_ENCRYPTION_REQ)) break;
        conn->core.step = LMP_TX_NULL;
        conn->core.stage = LMP_STAGE_CONNECTED;
        lmp_send_event_encryption_change(conn);
        rs = conn->rs_process;
        if (rs && rs->over_role_switch) {
            rs->over_role_switch = 0;
            lmp_send_event_role_change(conn, rs->state == ROLE_SWITCH_SUSS ? 0 : 0x35);
            lmp_role_switch_misc_free(conn);
        }
        break;
    }
    return 0;
}

int lmp_slave_machine(struct lmp_connection *conn, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    u8 opcode0 = 0, opcode1 = 0, opcode2 = 0;
    int ret = 0;
    if (rx) lmp_acl_c_handler(conn, rx, &opcode0, &opcode1, &opcode2);
    do {
        switch (conn->core.stage) {
        case LMP_STAGE_CONN:
            ret = stage_conn(conn, opcode0, opcode1, hci_cmd);
            break;
        case LMP_STAGE_AUTH_WITH_PIN_CODE_BY_REMOTE:
            lmp_slave_stage_auth_with_pin_code(conn, opcode0, 0, 0, rx, hci_cmd);
            return 0;
        case LMP_STAGE_AUTH_WITH_LINK_KEY_BY_REMOTE:
            ret = lmp_stage_auth_with_link_key_by_remote(conn, opcode0, 0, 0, 0, hci_cmd);
            break;
        case LMP_STAGE_AUTH_WITH_SIMPLE_PAIR_BY_REMOTE:
            lmp_stage_auth_with_simple_pair_by_remote(conn, opcode0, opcode1, 0, rx, hci_cmd);
            return 0;
        case LMP_STAGE_CALCULATE_LINK_KEY:
            lmp_stage_calc_link_key(conn, opcode0, 0, 0, 0, 0);
            return 0;
        case LMP_STAGE_ENC_START_BY_REMOTE:
            ret = lmp_slave_stage_enc_start_by_remote(conn, opcode0, opcode1, 0, 0, 0);
            break;
        case LMP_STAGE_CONNECTED:
            ret = lmp_slave_stage_connected(conn, opcode0, 0, 0, rx, 0);
            break;
        case LMP_STAGE_SCO_CONN_BY_REMOTE:
            lmp_slave_sco_conn_by_remote(conn, opcode0, opcode1, 0, rx, hci_cmd);
            return 0;
        case LMP_STAGE_ESCO_CONN_BY_REMOTE:
            lmp_slave_esco_conn_by_remote(conn, 0, opcode1, opcode2, rx, hci_cmd);
            return 0;
        case LMP_STAGE_ROLE_SWITCH_ON_ENC:
            lmp_slave_stage_role_switch_on_enc(conn, opcode0, opcode1, 0, 0, 0);
            return 0;
        case LMP_STAGE_AUTH_WITH_LINK_KEY_BY_LOCAL:
            lmp_stage_auth_with_link_key_by_local(conn, opcode0, 0, 0, 0, 0);
            return 0;
        case LMP_STAGE_ENC_START_BY_LOCAL:
            ret = lmp_master_stage_enc_start_by_local(conn, opcode0, opcode1, opcode2, 0, hci_cmd);
            break;
        case LMP_STAGE_DISCONN:
            /* Fix: the SDK kept ret here and spun forever after a 1. */
            ret = 0;
            break;
        default:
            btctrler_assert_failed("slave_stage", conn->core.stage);
            ret = 0;
            break;
        }
    } while (ret == 1);
    return ret;
}
