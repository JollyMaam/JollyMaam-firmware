/* Owned LMP receive dispatch (the SDK's lmp_acl_c_handler, lmp.c): decode
 * a received LMP PDU and hand it to the SDK's per-PDU handlers.  The SDK
 * inlined its small handlers here; they are written out below.  Opcodes the
 * SDK only logged are listed and ignored. */
#include <string.h>
#include "btctrler/glue.h"
#include "btstack_qos_jieli.h"
#include "btctrler/lmp_pdu.h"

/* Error codes used in LMP_not_accepted. */
#define UNSUPPORTED_FEATURE   0x1a
#define PDU_NOT_ALLOWED       0x24
#define UNSUPPORTED_PARAMETER 0x29

static void not_accepted(struct lmp_connection *conn, int opcode, int error)
{
    lmp_response(conn, "111", NOT_ACCEPTED << 1, opcode, error);
}

static void accepted(struct lmp_connection *conn, int opcode)
{
    lmp_response(conn, "11", ACCEPTED << 1, opcode);
}

/* Escape 4: p is the PDU after the two opcode bytes. */
static void escape4(struct lmp_connection *conn, const u8 *pdu, u8 *opcode2)
{
    const u8 *p = pdu + 2;
    struct link_connection *link = conn->link;
    switch (pdu[1]) {
    case ACCEPTED_EXT:
        *opcode2 = p[1];
        if (p[1] == PACKET_TYPE_TABLE_REQ) {
            link_conn_set_ptt(conn->link, 1);
            lmp_send_aclu_en(conn, 1);
        }
        break;
    case NOT_ACCEPTED_EXT:
        *opcode2 = p[1];
        switch (p[1]) {
        case ESCO_LINK_REQ: lmp_esco_link_removed(conn, p[2]); break;
        case PACKET_TYPE_TABLE_REQ: lmp_send_aclu_en(conn, 1); break;
        case IO_CAPABILITY_REQ: if (p[2]) lmp_send_event_auth_complete(conn, p[2]); break;
        }
        break;
    case FEATURES_REQ_EXT: lmp_rx_features_req_ext(conn, (u8 *)p); break;
    case FEATURES_RES_EXT: lmp_rx_features_res_ext(conn, (u8 *)p); break;
    case PACKET_TYPE_TABLE_REQ: lmp_rx_packet_type_table_req(conn, (u8 *)p); break;
    case CHANNEL_CLASSIFICATION_REQ: lmp_rx_channel_classification_req(conn, (u8 *)p); break;
    case CHANNEL_CLASSIFICATION: memcpy(conn->afh_report.rx_channel_classification, p, 10); break;
    case ESCO_LINK_REQ: lmp_rx_esco_link_req(conn, (u8 *)p); break;
    case POWER_CONTROL_REQ:
        /* Answer with the power reached: 63 at the minimum, 42 at the
         * maximum, 21 otherwise. */
        if (p[0] == 0)
            lmp_response(conn, "111", ESCAPE_4 << 1, POWER_CONTROL_RES,
                         (bredr_pwr_set(link, 1, -1) & 0xff) == 1 ? 63 : 21);
        else if (p[0] == 1)
            lmp_response(conn, "111", ESCAPE_4 << 1, POWER_CONTROL_RES,
                         (bredr_pwr_set(link, 1, 1) & 0xff) == 2 ? 42 : 21);
        else if (p[0] == 2) {
            bredr_pwr_set(link, 0, (char)bt_get_pwr_max_level());
            lmp_response(conn, "111", ESCAPE_4 << 1, POWER_CONTROL_RES, 42);
        }
        break;
    case POWER_CONTROL_RES:
        if (p[0] == 42) link_conn_rx_remote_max_power_res(conn->link, 0);
        break;
    case IO_CAPABILITY_REQ: lmp_rx_io_capability_req(conn, (u8 *)p); break;
    case PAUSE_ENCRYPTION_REQ: lmp_rx_pause_encryption_req(conn); break;
    case RESUME_ENCRYPTION_REQ: lmp_rx_resume_encryption_req(conn); break;
    case REMOVE_ESCO_LINK_REQ: case SNIFF_SUBRATING_REQ: case IO_CAPABILITY_RES: case 30:
        break;   /* logged by the SDK only */
    default:
        lmp_response(conn, "b05", ESCAPE_4 << 1, NOT_ACCEPTED_EXT, ESCAPE_4, pdu[1], UNSUPPORTED_FEATURE);
        break;
    }
}

/* LMP_accepted / LMP_not_accepted: p[0] is the opcode answered. */
static void answer(struct lmp_connection *conn, int ok, const u8 *p)
{
    if (ok) switch (p[0]) {
    case SNIFF_REQ: lmp_rx_accepted_sniff_req(conn); break;
    case UNSNIFF_REQ: lmp_rx_accepted_unsniff_req(conn); break;
    case MAX_SLOT_REQ: lmp_rx_accepted_max_slot_req(conn); break;
    case HOST_CONNECTION_REQ: lmp_rx_accepted_host_connection_req(conn); break;
    case START_ENCRYPTION_REQ: lmp_rx_accepted_start_encryption_req(conn); break;
    case STOP_ENCRYPTION_REQ: lmp_rx_accepted_stop_encryption_req(conn); break;
    case SWITCH_REQ: lmp_rx_accepted_switch_req(conn); break;
    }
    else switch (p[0]) {
    case SNIFF_REQ: lmp_rx_not_accepted_sniff_req(conn); break;
    case UNSNIFF_REQ: lmp_rx_not_accepted_unsniff_req(conn); break;
    case HOST_CONNECTION_REQ: lmp_rx_not_accepted_host_connection_req(conn, (u8 *)p); break;
    case AU_RAND: lmp_rx_not_accept_au_rand(conn, (u8 *)p); break;
    case IN_RAND: lmp_rx_not_accepted_in_rand(conn); break;
    case DHKEY_CHECK: lmp_tx_detch(conn, 0x10); break;
    case SWITCH_REQ: lmp_rx_not_accepted_switch_req(conn); break;
    }
}

/* LMP_start_encryption_req: derive the key from the random number in p. */
static void start_encryption(struct lmp_connection *conn, const u8 *p)
{
    u8 key[16];
    if (conn->enc_timer) { sys_timer_del(conn->enc_timer); conn->enc_timer = 0; }
    function_E3_api(conn->core.link_key, (u8 *)p, conn->core.cof, conn->core.key_size, key);
    link_conn_set_encrypt_key(conn->link, key);
    link_conn_set_encrypt(conn->link, 1, 1);
    accepted(conn, START_ENCRYPTION_REQ);
    hci_send_event(0xf7, "1", 0);
    lmp_send_aclu_en(conn, 1);
    conn->core.enc_state = 4;
}

void lmp_acl_c_handler(struct lmp_connection *conn, struct bredr_rx_bulk *rx,
                       u8 *opcode0, u8 *opcode1, u8 *opcode2)
{
    u8 *pdu = rx->data, header = pdu[0], *p = pdu + 1;
    u8 opcode = header >> 1;
    *opcode0 = header & ~1;
    conn->tran_id = header & 1;
    if (opcode == 124 || opcode == 125) return;         /* escapes 1 and 2 */
    if (opcode == ESCAPE_3 || opcode == ESCAPE_4) *opcode1 = pdu[1];
    if (!lmp_cmd_is_match_size(*opcode0, pdu, (u8)rx->len)) return;
    if (opcode == ESCAPE_3) {
        lmp_escape3_command_process(conn, pdu[1], opcode2, pdu + 2);
        return;
    }
    if (opcode == ESCAPE_4) {
        escape4(conn, pdu, opcode2);
        return;
    }
    /* The QoS adapter takes the PDUs of its own transactions. */
    if (cyclone2_qos_rx(conn->core_s.handle, conn->core.role, header, p, rx->len)) return;
    switch (opcode) {
    case ACCEPTED: *opcode1 = p[0] << 1; answer(conn, 1, p); break;
    case NOT_ACCEPTED: *opcode1 = p[0] << 1; answer(conn, 0, p); break;
    case FEATURES_REQ: lmp_rx_feature_req(conn, p); break;
    case FEATURES_RES: memcpy(conn->core_s.remote_features, p, 8); break;
    case VERSION_REQ: lmp_rx_version_req(conn); break;
    case VERSION_RES: lmp_rx_version_res(conn, p); break;
    case NAME_REQ: lmp_rx_name_req(conn, p); break;
    case NAME_RES: lmp_rx_name_res(conn, p); break;
    case MAX_SLOT: lmp_rx_max_slot(conn, p); break;
    case MAX_SLOT_REQ:
        lmp_rx_max_slot_req(conn, p);
        if (btctrler_qos_poll_slots) lmp_tx_qos_req(conn);
        break;
    case INCR_POWER_REQ: lmp_rx_incr_power(conn); break;
    case DECR_POWER_REQ: lmp_rx_decr_power(conn); break;
    case MAX_POWER: lmp_rx_max_power(conn); break;
    case MIN_POWER: lmp_rx_min_power(conn); break;
    case SUPERVISION_TIMEOUT: lmp_rx_supervision_timeout(conn, p); break;
    case CLKOFFSET_REQ: lmp_rx_clkoffset_req(conn); break;
    case CLKOFFSET_RES: lmp_rx_clkoffset_res(conn, p); break;
    case DETACH: lmp_rx_detch(conn, p); break;
    case QUALITY_OF_SERVICE_REQ: lmp_rx_quality_of_service_req(conn, p); break;
    case TEMP_KEY: lmp_rx_temp_key(conn); break;
    case TIMING_ACCURACY_REQ: lmp_rx_timing_accuracy_req(conn); break;
    case SNIFF_REQ:
        if (!lmp_rx_sniff_standby(conn) || !lmp_esco_sniff_check(conn))
            not_accepted(conn, SNIFF_REQ, PDU_NOT_ALLOWED);
        else lmp_rx_sniff_req(conn, p);
        break;
    case UNSNIFF_REQ: lmp_rx_unsniff_req(conn); break;
    case SCO_LINK_REQ: lmp_rx_sco_link_req(conn, p); break;
    case TEST_ACTIVATE:
        /* The host never enables DUT mode here: refuse test mode and
         * detach, as the SDK does then. */
        if (config_btctler_mode & 1) {
            not_accepted(conn, TEST_ACTIVATE, 0x1e);
            lmp_tx_detch(conn, 0x13);
        }
        break;
    case TEST_CONTROL: break;   /* The SDK set up DUT state even unactivated. */
    case HOST_CONNECTION_REQ: lmp_rx_host_connection_req(conn); break;
    case SETUP_COMPLETE: lmp_rx_setup_complete(conn); break;
    case IN_RAND: lmp_rx_in_rand(conn, p); break;
    case COMB_KEY: lmp_rx_comb_key(conn, p); break;
    case AU_RAND: lmp_rx_au_rand(conn, p); break;
    case SRES: lmp_rx_sres(conn, p); break;
    case ENCRYPTION_MODE_REQ:
        conn->core.enc_state |= 0x20;
        accepted(conn, ENCRYPTION_MODE_REQ);
        break;
    case ENCRYPTION_KEY_SIZE_REQ:
        conn->core.key_size = p[0];
        if (p[0] == 16 || p[0] == 7) accepted(conn, ENCRYPTION_KEY_SIZE_REQ);
        else not_accepted(conn, ENCRYPTION_KEY_SIZE_REQ, UNSUPPORTED_PARAMETER);
        break;
    case ENCRYPTION_KEY_SIZE_MASK_REQ:
        if (conn->core.role == 0) not_accepted(conn, ENCRYPTION_KEY_SIZE_MASK_REQ, UNSUPPORTED_FEATURE);
        else lmp_response(conn, "12", ENCRYPTION_KEY_SIZE_MASK_RES << 1, 0xffff);
        break;
    case START_ENCRYPTION_REQ: start_encryption(conn, p); break;
    case STOP_ENCRYPTION_REQ: lmp_rx_stop_encryption_req(conn); break;
    case SLOT_OFFSET: lmp_rx_slot_offset(conn, p); break;
    case SWITCH_REQ:
        if (!lmp_rx_role_switch_standby(conn)) not_accepted(conn, SWITCH_REQ, PDU_NOT_ALLOWED);
        else lmp_rx_switch_req(conn, p);
        break;
    case ENCAPSULATED_PAYLOAD: lmp_rx_encapsulated_payload(conn, p); break;
    case SIMPLE_PAIRING_CONFIRM: lmp_rx_pairing_confirm(conn, p); break;
    case SIMPLE_PAIRING_NUMBER: memcpy(conn->simple->remote_random_num, p, 16); break;
    /* Logged by the SDK only. */
    case AUTO_RATE: case PREFERRED_RATE: case REMOVE_SCO_LINK_REQ: case TIMING_ACCURACY_RES:
    case SET_AFH: case ENCAPSULATED_HEADER: case DHKEY_CHECK: break;
    default: not_accepted(conn, opcode, UNSUPPORTED_FEATURE); break;
    }
}

#ifdef UNITTEST
#include <stdarg.h>
#include <stdio.h>
#include "test.h"
int config_btctler_modules, config_btctler_mode = 1, CONFIG_BREDR_INQUIRY, CONFIG_LMP_MASTER_ESCO_ENABLE;
u16 btctrler_qos_poll_slots;
static char last[64], response[64];
static u8 *arg;
static int qos_takes, size_ok = 1, standby = 1;
static void called(const char *n) { snprintf(last, sizeof(last), "%s", n); }
void lmp_rx_accepted_host_connection_req(struct lmp_connection *c) { (void)c; called("accepted_host_connection_req"); }
void lmp_rx_accepted_max_slot_req(struct lmp_connection *c) { (void)c; called("accepted_max_slot_req"); }
void lmp_rx_accepted_sniff_req(struct lmp_connection *c) { (void)c; called("accepted_sniff_req"); }
void lmp_rx_accepted_start_encryption_req(struct lmp_connection *c) { (void)c; called("accepted_start_encryption_req"); }
void lmp_rx_accepted_stop_encryption_req(struct lmp_connection *c) { (void)c; called("accepted_stop_encryption_req"); }
void lmp_rx_accepted_switch_req(struct lmp_connection *c) { (void)c; called("accepted_switch_req"); }
void lmp_rx_accepted_unsniff_req(struct lmp_connection *c) { (void)c; called("accepted_unsniff_req"); }
void lmp_rx_clkoffset_req(struct lmp_connection *c) { (void)c; called("clkoffset_req"); }
void lmp_rx_decr_power(struct lmp_connection *c) { (void)c; called("decr_power"); }
void lmp_rx_host_connection_req(struct lmp_connection *c) { (void)c; called("host_connection_req"); }
void lmp_rx_incr_power(struct lmp_connection *c) { (void)c; called("incr_power"); }
void lmp_rx_max_power(struct lmp_connection *c) { (void)c; called("max_power"); }
void lmp_rx_min_power(struct lmp_connection *c) { (void)c; called("min_power"); }
void lmp_rx_not_accepted_in_rand(struct lmp_connection *c) { (void)c; called("not_accepted_in_rand"); }
void lmp_rx_not_accepted_sniff_req(struct lmp_connection *c) { (void)c; called("not_accepted_sniff_req"); }
void lmp_rx_not_accepted_switch_req(struct lmp_connection *c) { (void)c; called("not_accepted_switch_req"); }
void lmp_rx_not_accepted_unsniff_req(struct lmp_connection *c) { (void)c; called("not_accepted_unsniff_req"); }
void lmp_rx_pause_encryption_req(struct lmp_connection *c) { (void)c; called("pause_encryption_req"); }
void lmp_rx_resume_encryption_req(struct lmp_connection *c) { (void)c; called("resume_encryption_req"); }
void lmp_rx_setup_complete(struct lmp_connection *c) { (void)c; called("setup_complete"); }
void lmp_rx_stop_encryption_req(struct lmp_connection *c) { (void)c; called("stop_encryption_req"); }
void lmp_rx_temp_key(struct lmp_connection *c) { (void)c; called("temp_key"); }
void lmp_rx_timing_accuracy_req(struct lmp_connection *c) { (void)c; called("timing_accuracy_req"); }
void lmp_rx_unsniff_req(struct lmp_connection *c) { (void)c; called("unsniff_req"); }
void lmp_rx_version_req(struct lmp_connection *c) { (void)c; called("version_req"); }
void lmp_rx_au_rand(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("au_rand"); }
void lmp_rx_channel_classification_req(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("channel_classification_req"); }
void lmp_rx_clkoffset_res(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("clkoffset_res"); }
void lmp_rx_comb_key(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("comb_key"); }
void lmp_rx_detch(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("detch"); }
void lmp_rx_encapsulated_payload(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("encapsulated_payload"); }
void lmp_rx_esco_link_req(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("esco_link_req"); }
void lmp_rx_feature_req(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("feature_req"); }
void lmp_rx_features_req_ext(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("features_req_ext"); }
void lmp_rx_features_res_ext(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("features_res_ext"); }
void lmp_rx_in_rand(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("in_rand"); }
void lmp_rx_io_capability_req(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("io_capability_req"); }
void lmp_rx_max_slot(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("max_slot"); }
void lmp_rx_max_slot_req(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("max_slot_req"); }
void lmp_rx_name_req(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("name_req"); }
void lmp_rx_name_res(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("name_res"); }
void lmp_rx_not_accept_au_rand(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("not_accept_au_rand"); }
void lmp_rx_not_accepted_host_connection_req(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("not_accepted_host_connection_req"); }
void lmp_rx_packet_type_table_req(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("packet_type_table_req"); }
void lmp_rx_pairing_confirm(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("pairing_confirm"); }
void lmp_rx_quality_of_service_req(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("quality_of_service_req"); }
void lmp_rx_sco_link_req(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("sco_link_req"); }
void lmp_rx_slot_offset(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("slot_offset"); }
void lmp_rx_sniff_req(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("sniff_req"); }
void lmp_rx_sres(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("sres"); }
void lmp_rx_supervision_timeout(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("supervision_timeout"); }
void lmp_rx_switch_req(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("switch_req"); }
void lmp_rx_version_res(struct lmp_connection *c, u8 *p) { (void)c; arg = p; called("version_res"); }
int lmp_response(struct lmp_connection *c, const char *format, ...)
{
    va_list ap; va_start(ap, format); int a = va_arg(ap, int), b = va_arg(ap, int), d = va_arg(ap, int);
    va_end(ap); (void)c; snprintf(response, sizeof(response), "%s %d %d %d", format, a, b, strlen(format) > 2 ? d : 0);
    return 0;
}
int hci_send_event(int e, const char *f, ...) { (void)e; (void)f; return 0; }
int cyclone2_qos_rx(u16 h, u8 role, u8 header, const u8 *p, u16 size)
{ (void)h; (void)role; (void)p; (void)size; return qos_takes && header >> 1 == 3; }
u8 lmp_cmd_is_match_size(u8 op, u8 *d, u8 len) { (void)op; (void)d; (void)len; return (u8)size_ok; }
void lmp_escape3_command_process(struct lmp_connection *c, u8 op, u8 *o, u8 *p) { (void)c; (void)op; (void)o; (void)p; called("escape3"); }
void lmp_esco_link_removed(struct lmp_connection *c, u8 r) { (void)c; (void)r; called("esco_link_removed"); }
u8 lmp_esco_sniff_check(struct lmp_connection *c) { (void)c; return 1; }
bool lmp_rx_sniff_standby(struct lmp_connection *c) { (void)c; return standby; }
bool lmp_rx_role_switch_standby(struct lmp_connection *c) { (void)c; return standby; }
void lmp_send_event_auth_complete(struct lmp_connection *c, u8 r) { (void)c; (void)r; called("auth_complete"); }
void lmp_tx_detch(struct lmp_connection *c, u8 e) { (void)c; snprintf(last, sizeof(last), "detach %d", e); }
void lmp_tx_qos_req(struct lmp_connection *c) { (void)c; called("qos_req"); }
void lmp_send_aclu_en(struct lmp_connection *c, u8 e) { (void)c; (void)e; called("aclu_en"); }
void link_conn_set_ptt(struct link_connection *l, u8 e) { (void)l; (void)e; }
void link_conn_set_encrypt_key(struct link_connection *l, u8 *k) { (void)l; (void)k; }
void link_conn_set_encrypt(struct link_connection *l, u8 a, u8 b) { (void)l; (void)a; (void)b; called("encrypt"); }
void link_conn_rx_remote_max_power_res(struct link_connection *l, u8 s) { (void)l; (void)s; called("max_power_res"); }
int bredr_pwr_set(struct link_connection *l, char sel, char inc) { (void)l; (void)sel; return inc == -1 ? 1 : 0; }
u8 bt_get_pwr_max_level(void) { return 4; }
void function_E3_api(u8 *k, u8 *r, u8 *c, u8 n, u8 *out) { (void)k; (void)r; (void)c; (void)n; memset(out, 0, 16); }
void sys_timer_del(u16 id) { (void)id; }

static struct lmp_connection conn;
static u8 op0, op1, op2;
static void rx(u8 header, u8 b1, u8 b2, u8 b3)
{
    static u8 buf[sizeof(struct bredr_rx_bulk) + 32];
    struct bredr_rx_bulk *r = (struct bredr_rx_bulk *)buf;
    last[0] = response[0] = 0; arg = 0; op1 = op2 = 0xee;
    r->len = 5; r->data[0] = header; r->data[1] = b1; r->data[2] = b2; r->data[3] = b3;
    lmp_acl_c_handler(&conn, r, &op0, &op1, &op2);
}
int main(void)
{
    /* Plain opcodes reach their SDK handlers, with the parameters after the opcode byte. */
    static const struct { u8 opcode; const char *handler; } table[] = {
        {1,"name_req"},{2,"name_res"},{5,"clkoffset_req"},{6,"clkoffset_res"},{7,"detch"},{8,"in_rand"},
        {9,"comb_key"},{11,"au_rand"},{12,"sres"},{14,"temp_key"},{18,"stop_encryption_req"},
        {19,"switch_req"},{23,"sniff_req"},{24,"unsniff_req"},{31,"incr_power"},{32,"decr_power"},
        {33,"max_power"},{34,"min_power"},{37,"version_req"},{38,"version_res"},{39,"feature_req"},
        {42,"quality_of_service_req"},{43,"sco_link_req"},{45,"max_slot"},{46,"max_slot_req"},
        {47,"timing_accuracy_req"},{49,"setup_complete"},{51,"host_connection_req"},{52,"slot_offset"},
        {55,"supervision_timeout"},{62,"encapsulated_payload"},{63,"pairing_confirm"},
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        rx((u8)(table[i].opcode << 1 | 1), 0, 0, 0);
        CHECK(!strcmp(last, table[i].handler));
        CHECK(op0 == table[i].opcode << 1 && conn.tran_id == 1 && op1 == 0xee);
    }
    /* Answers: opcode1 is the answered opcode, shifted; dispatch by it. */
    rx(3 << 1, 23, 0, 0); CHECK(!strcmp(last, "accepted_sniff_req") && op1 == 46);
    rx(4 << 1, 19, 0, 0); CHECK(!strcmp(last, "not_accepted_switch_req"));
    rx(4 << 1, 11, 0, 0); CHECK(!strcmp(last, "not_accept_au_rand"));
    rx(4 << 1, 65, 0, 0); CHECK(!strcmp(last, "detach 16"));
    /* The QoS adapter sees PDUs first. */
    qos_takes = 1; rx(3 << 1, 23, 0, 0); CHECK(!last[0]); qos_takes = 0;
    /* Escapes. */
    rx(127 << 1, 3, 0, 0); CHECK(!strcmp(last, "features_req_ext") && op1 == 3);
    rx(127 << 1, 1, 0, 11); CHECK(!strcmp(last, "aclu_en") && op2 == 11);
    rx(127 << 1, 2, 0, 12); CHECK(!strcmp(last, "esco_link_removed") && op2 == 12);
    rx(127 << 1, 31, 0, 0); CHECK(!strcmp(response, "111 254 32 63"));
    rx(127 << 1, 99, 0, 0); CHECK(!strcmp(response, "b05 254 2 127"));
    rx(126 << 1, 5, 0, 0); CHECK(!strcmp(last, "escape3") && op1 == 5);
    rx(124 << 1, 5, 0, 0); CHECK(!last[0] && op1 == 0xee);
    /* Wrong size: nothing, though escapes still report their opcode. */
    size_ok = 0; rx(127 << 1, 3, 0, 0); CHECK(!last[0] && op1 == 3);
    rx(1 << 1, 0, 0, 0); CHECK(!last[0]); size_ok = 1;
    /* Inlined SDK handlers. */
    rx(16 << 1, 16, 0, 0); CHECK(!strcmp(response, "11 6 16 0") && conn.core.key_size == 16);
    rx(16 << 1, 5, 0, 0); CHECK(!strcmp(response, "111 8 16 41"));
    rx(17 << 1, 0, 0, 0); CHECK(!strcmp(last, "aclu_en") && conn.core.enc_state == 4);
    rx(15 << 1, 0, 0, 0); CHECK(!strcmp(response, "11 6 15 0") && (conn.core.enc_state & 0x20));
    rx(40 << 1, 9, 8, 7); CHECK(conn.core_s.remote_features[0] == 9 && conn.core_s.remote_features[2] == 7);
    standby = 0; rx(23 << 1, 0, 0, 0); CHECK(!strcmp(response, "111 8 23 36")); standby = 1;
    rx(56 << 1, 0, 0, 0); CHECK(!strcmp(response, "111 8 56 30") && !strcmp(last, "detach 19"));
    btctrler_qos_poll_slots = 6; rx(46 << 1, 0, 0, 0); CHECK(!strcmp(last, "qos_req")); btctrler_qos_poll_slots = 0;
    rx(100 << 1, 0, 0, 0); CHECK(!strcmp(response, "111 8 100 26"));
    rx(61 << 1, 0, 0, 0); CHECK(!last[0] && !response[0]);
    return TEST_DONE("btctrler_lmp_rx");
}
#endif
