/* Owned lmp_name_req.c: the short connection made only to read a remote
 * device's name (features, extended features, name, detach). */
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

_Static_assert(BTCFG_NAME_REQ, "remote name requests are enabled");

int lmp_name_stage_conn(struct lmp_connection *conn, u8 opcode0, u8 opcode1, u8 opcode2,
                        struct bredr_rx_bulk *rx, int *hci_cmd)
{
    (void)opcode2; (void)rx; (void)hci_cmd;
    switch (conn->core.step) {
    case LMP_TX_FEATURES_REQ:
        lmp_tx_features_req(conn);
        conn->core.step = LMP_WAIT_FEATURES_RES;
        break;
    case LMP_WAIT_FEATURES_RES:
        if (opcode0 != PDU(FEATURES_RES)) break;
        /* fall through */
    case LMP_TX_FEATURES_REQ_EXT:
        lmp_tx_features_req_ext(conn, 1);
        conn->core.step = LMP_WAIT_FEATURES_RES_EXT;
        break;
    case LMP_WAIT_FEATURES_RES_EXT:
        if (opcode1 != FEATURES_RES_EXT) break;
        /* fall through */
    case LMP_TX_NAME_REQ:
        lmp_tx_name_req(conn);
        conn->core.step = LMP_WAIT_NAME_RES;
        break;
    case LMP_WAIT_NAME_RES:
        /* Detach once the whole name has arrived (lmp_rx_name_res). */
        if (opcode0 != PDU(NAME_RES) || conn->remote_name) break;
        /* fall through */
    case LMP_TX_DETACH:
        lmp_tx_detch(conn, 0x13);
        conn->core.step = LMP_TX_NULL;
        break;
    }
    return 0;
}

int lmp_name_req_machine(struct lmp_connection *conn, struct bredr_rx_bulk *rx, int *hci_cmd)
{
    u8 opcode0 = 0, opcode1 = 0, opcode2 = 0;
    (void)hci_cmd;
    if (rx) lmp_acl_c_handler(conn, rx, &opcode0, &opcode1, &opcode2);
    if (conn->core.stage == LMP_STAGE_CONN) lmp_name_stage_conn(conn, opcode0, opcode1, 0, 0, 0);
    return 0;
}

void lmp_name_req_init(struct lmp_connection *conn)
{
    conn->core.stage = LMP_STAGE_CONN;
    conn->core.step = LMP_TX_FEATURES_REQ;
    lmp_name_req_machine(conn, 0, 0);
}
