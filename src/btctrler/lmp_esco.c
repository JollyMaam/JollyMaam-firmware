/* Owned lmp_esco.c: synchronous (voice) links.  The pad has none: SCO and
 * eSCO link requests are refused as unsupported, so conn->esco_conn stays
 * NULL and the queries below find nothing.  In the SDK a request went on to
 * the host, whose reject the controller then dropped (link_control.c, voice
 * disabled), leaving the link manager waiting. */
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

_Static_assert(!BTCFG_MASTER_ESCO && !BTCFG_ESCO_FORWARD && !BTCFG_TWS, "no voice links");

#define UNSUPPORTED_FEATURE 0x1a

void lmp_rx_sco_link_req(struct lmp_connection *conn, u8 *rx)
{
    (void)rx;
    lmp_response(conn, "111", PDU(NOT_ACCEPTED), SCO_LINK_REQ, UNSUPPORTED_FEATURE);
}

void lmp_rx_esco_link_req(struct lmp_connection *conn, u8 *rx)
{
    (void)rx;
    lmp_response(conn, "b05", PDU(ESCAPE_4), NOT_ACCEPTED_EXT, ESCAPE_4, ESCO_LINK_REQ, UNSUPPORTED_FEATURE);
}

/* Never entered now (lmp_slave_stage_connected); back to connected. */
int lmp_slave_sco_conn_by_remote(struct lmp_connection *conn, u8 opcode0, u8 opcode1, u8 opcode2,
                                 struct bredr_rx_bulk *rx, int *hci_cmd)
{
    (void)opcode0; (void)opcode1; (void)opcode2; (void)rx; (void)hci_cmd;
    conn->core.step = LMP_TX_NULL;
    conn->core.stage = LMP_STAGE_CONNECTED;
    return 0;
}

int lmp_slave_esco_conn_by_remote(struct lmp_connection *conn, u8 opcode0, u8 opcode1, u8 opcode2,
                                  struct bredr_rx_bulk *rx, int *hci_cmd)
{
    return lmp_slave_sco_conn_by_remote(conn, opcode0, opcode1, opcode2, rx, hci_cmd);
}

void lmp_esco_link_removed(struct lmp_connection *conn, u8 reason)
{
    (void)conn; (void)reason;   /* there is no synchronous link to remove */
}

u8 lmp_get_esco_conn_statu(void)
{
    for (struct lmp_connection *conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++)
        if (conn->esco_conn) return 1;
    return 0;
}

struct lmp_connection *lmp_esco_conn_for_handle(int handle)
{
    for (struct lmp_connection *conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++)
        if (conn->esco_conn && conn->esco_conn->hci_handle == handle) return conn;
    return 0;
}

bool lmp_connection_esco_open(void)
{
    for (struct lmp_connection *conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++)
        if (conn->core_s.handle && conn->esco_conn) return 1;
    return 0;
}

/* HCI Disconnect on a synchronous handle: there are none (0: not found). */
int lmp_hci_disconnect_sync_connection(int handle, u8 reason)
{
    (void)reason;
    return handle == 0;
}
