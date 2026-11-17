/* Classic feature, version and name exchange, recovered from SDK lmp.c.
 * Receive callers have already checked the complete LMP PDU length. */
#include <string.h>
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

_Static_assert(BTCFG_NAME_REQ, "remote name exchange is enabled");

static const u8 *local_features(unsigned page)
{
    return page == 0 ? hci_param.features : page <= 2 ? hci_param.ext_features + (page - 1) * 8 : 0;
}

void lmp_tx_features_req(struct lmp_connection *conn)
{ lmp_request(conn, "1C008", PDU(FEATURES_REQ), hci_param.features); }

int lmp_tx_features_req_ext(struct lmp_connection *conn, int page_num)
{
    const u8 *features = local_features((unsigned)page_num);
    /* Fix: the SDK indexed outside ext_features for arbitrary page numbers. */
    if (!features) return 0;
    lmp_request(conn, "b04C008", PDU(ESCAPE_4), FEATURES_REQ_EXT, page_num, 1, features);
    return 1;
}

void lmp_rx_feature_req(struct lmp_connection *conn, u8 *rx)
{
    memcpy(conn->core_s.remote_features, rx, 8);
    lmp_response(conn, "1C008", PDU(FEATURES_RES), hci_param.features);
}

void lmp_rx_features_res(struct lmp_connection *conn, u8 *rx)
{ memcpy(conn->core_s.remote_features, rx, 8); }

void lmp_rx_features_req_ext(struct lmp_connection *conn, u8 *rx)
{
    unsigned page = rx[0];
    const u8 *features = local_features(page);
    /* Fix: reject out-of-range pages before copying into the connection.
     * Page zero must reply with standard features, not bytes before ext_features. */
    if (!features) {
        lmp_response(conn, "b05", PDU(ESCAPE_4), NOT_ACCEPTED_EXT, ESCAPE_4, FEATURES_REQ_EXT, 0x1e);
        return;
    }
    memcpy(page == 0 ? conn->core_s.remote_features : conn->core_s.remote_ext_features + (page - 1) * 8,
           rx + 2, 8);
    u8 page2 = 0;
    for (unsigned i = 8; i < 16; i++) page2 |= hci_param.ext_features[i];
    lmp_response(conn, "b04C008", PDU(ESCAPE_4), FEATURES_RES_EXT, page, page2 ? 2 : 1, features);
}

void lmp_rx_features_res_ext(struct lmp_connection *conn, u8 *rx)
{
    /* Preserve the SDK's ignored page-zero response; bound the extended pages. */
    if (rx[0] == 0 || rx[0] > 2) return;
    memcpy(conn->core_s.remote_ext_features + (rx[0] - 1) * 8, rx + 2, 8);
    if (!(conn->core.state & 1))
        hci_send_event(0x3d, "AC008", conn->core_s.remote_addr, conn->core_s.remote_ext_features);
}

void lmp_tx_version_req(struct lmp_connection *conn)
{
    lmp_request(conn, "1122", PDU(VERSION_REQ), hci_param.lmp_pal_version,
                hci_param.manufacturer_name, hci_param.lmp_pal_subversion);
}

void lmp_rx_version_req(struct lmp_connection *conn)
{
    lmp_response(conn, "1122", PDU(VERSION_RES), hci_param.lmp_pal_version,
                 hci_param.manufacturer_name, hci_param.lmp_pal_subversion);
}

void lmp_rx_version_res(struct lmp_connection *conn, u8 *rx)
{
    hci_send_event(0x0c, "1H122", 0, conn->core_s.handle, rx[0],
                   rx[1] | (rx[2] << 8), rx[3] | (rx[4] << 8));
}

int lmp_tx_name_req(struct lmp_connection *conn)
{ lmp_request(conn, "11", PDU(NAME_REQ), 0); return 1; }

void lmp_rx_name_req(struct lmp_connection *conn, u8 *rx)
{
    unsigned len = 0, offset = rx[0];
    while (len < sizeof(gbredr_local_dev.local_name) && gbredr_local_dev.local_name[len]) len++;
    conn->core.state |= 1u << 22;
    /* Fix: the SDK sent fourteen bytes from an unchecked local-name offset,
     * potentially exposing adjacent controller storage. */
    if (offset > len) {
        lmp_response(conn, "111", PDU(NOT_ACCEPTED), NAME_REQ, 0x1e);
        return;
    }
    u8 fragment[14] = {0};
    unsigned count = len - offset;
    if (count > sizeof(fragment)) count = sizeof(fragment);
    memcpy(fragment, gbredr_local_dev.local_name + offset, count);
    lmp_response(conn, "111C014", PDU(NAME_RES), offset, len, fragment);
}

void lmp_rx_name_res(struct lmp_connection *conn, u8 *rx)
{
    unsigned offset = rx[0], len = rx[1];
    /* Fix: invalid totals/offsets made the SDK's copy overflow, or underflow
     * its length. Fail the exchange and release any partial name. */
    if (len > 248 || offset > len) {
        static const u8 empty[32];
        hci_send_event(0x07, "1AC032", 0x1e, conn->core_s.remote_addr, empty);
        if (conn->remote_name) lmp_free(conn->remote_name);
        conn->remote_name = 0;
        return;
    }
    if (!conn->remote_name) {
        conn->remote_name = lmp_malloc(248);
        if (!conn->remote_name) { btctrler_assert_failed("conn->remote_name", 0); return; }
        memset(conn->remote_name, 0, 248);
    }
    unsigned count = len - offset;
    if (count > 14) count = 14;
    memcpy(conn->remote_name + offset, rx + 2, count);
    if (len > offset + 14) {
        lmp_response(conn, "11", PDU(NAME_REQ), offset + 14);
    } else {
        /* Preserve the SDK's 32-byte event payload and synchronous copy/free. */
        hci_send_event(0x07, "1AC032", 0, conn->core_s.remote_addr, conn->remote_name);
        lmp_free(conn->remote_name);
        conn->remote_name = 0;
    }
}

void lmp_tx_connection_req(struct lmp_connection *conn)
{
    conn->core.state |= 1;
    lmp_request(conn, "1", PDU(HOST_CONNECTION_REQ));
}

void lmp_setup_complete(struct lmp_connection *conn)
{
    if (conn->core.state & 4) return;
    conn->core.state |= 4;
    lmp_request(conn, "1", PDU(SETUP_COMPLETE));
    u8 map[10];
    memset(map, 0xff, sizeof(map));
    map[9] = 0x7f; /* 79 Classic channels; bit 79 is reserved. */
    lmp_tx_set_afh_req(conn, map);
    lmp_tx_name_req(conn);
}

void lmp_tx_setup_complete(struct lmp_connection *conn)
{ lmp_setup_complete(conn); }

#ifdef UNITTEST
#include <stdarg.h>
#include <stdlib.h>
#include "test.h"
struct hci_parameter hci_param;
struct bredr_local_device gbredr_local_dev;
static struct lmp_connection conn;
static int outputs, event_id, v[6], allocations, fail_alloc, asserts, afh_calls;
static u8 copied[32];
static const char *last_format;
static char sequence[32];
static void pdu(const char *format, va_list ap)
{
    outputs++; last_format = format; memset(v, 0, sizeof(v)); memset(copied, 0, sizeof(copied));
    int n = !strcmp(format, "b05") ? 5 : !strcmp(format, "b04C008") ? 4 :
            !strcmp(format, "1122") ? 4 : !strcmp(format, "111C014") ? 3 :
            !strcmp(format, "111") ? 3 : !strcmp(format, "11") ? 2 : 1;
    for (int i = 0; i < n; i++) v[i] = va_arg(ap, int);
    if (strstr(format, "C008")) memcpy(copied, va_arg(ap, const u8 *), 8);
    if (strstr(format, "C014")) memcpy(copied, va_arg(ap, const u8 *), 14);
}
int lmp_request(struct lmp_connection *c, const char *format, ...)
{
    CHECK(c == &conn); strcat(sequence, "q");
    va_list ap; va_start(ap, format); pdu(format, ap); va_end(ap); return 0;
}
int lmp_response(struct lmp_connection *c, const char *format, ...)
{
    CHECK(c == &conn); strcat(sequence, "r");
    va_list ap; va_start(ap, format); pdu(format, ap); va_end(ap); return 0;
}
int hci_send_event(int id, const char *format, ...)
{
    outputs++; event_id = id; last_format = format;
    va_list ap; va_start(ap, format);
    if (id == 0x0c) {
        for (int i = 0; i < 5; i++) v[i] = va_arg(ap, int);
    } else {
        if (id == 0x07) v[0] = va_arg(ap, int);
        CHECK(va_arg(ap, u8 *) == conn.core_s.remote_addr);
        memcpy(copied, va_arg(ap, const u8 *), id == 0x07 ? 32 : 8);
    }
    va_end(ap); return 0;
}
void *lmp_malloc(int size)
{ CHECK(size == 248); if (fail_alloc) return 0; allocations++; return malloc(size); }
void lmp_free(void *ptr) { CHECK(ptr != 0); allocations--; free(ptr); }
void btctrler_assert_failed(const char *what, int value)
{ (void)what; (void)value; asserts++; }
void lmp_tx_set_afh_req(struct lmp_connection *c, u8 *map)
{
    CHECK(c == &conn);
    for (int i = 0; i < 9; i++) CHECK(map[i] == 0xff);
    CHECK(map[9] == 0x7f);
    afh_calls++; strcat(sequence, "a");
}
static void reset(void)
{ outputs = event_id = 0; sequence[0] = 0; memset(copied, 0, sizeof(copied)); }
int main(void)
{
    for (int i = 0; i < 8; i++) hci_param.features[i] = 0x80 + i;
    for (int i = 0; i < 16; i++) hci_param.ext_features[i] = 0x20 + i;
    u8 ext[10] = {0, 2, 1, 2, 3, 4, 5, 6, 7, 8};
    for (int page = 0; page < 256; page++) {
        reset(); ext[0] = page;
        struct lmp_connection before = conn;
        lmp_rx_features_req_ext(&conn, ext);
        CHECK(outputs == 1);
        if (page <= 2) {
            CHECK(v[0] == 254 && v[1] == FEATURES_RES_EXT && v[2] == page && v[3] == 2);
            CHECK(!memcmp(copied, local_features(page), 8));
            CHECK(!memcmp(page ? conn.core_s.remote_ext_features + (page - 1) * 8 :
                          conn.core_s.remote_features, ext + 2, 8));
        } else {
            CHECK(v[1] == NOT_ACCEPTED_EXT && v[3] == FEATURES_REQ_EXT && v[4] == 0x1e);
            CHECK(!memcmp(&conn, &before, sizeof(conn)));
        }
        reset(); before = conn;
        lmp_rx_features_res_ext(&conn, ext);
        if (page == 1 || page == 2) CHECK(event_id == 0x3d && outputs == 1);
        else CHECK(!outputs && !memcmp(&conn, &before, sizeof(conn)));
        reset();
        CHECK(lmp_tx_features_req_ext(&conn, page) == (page <= 2));
        CHECK(outputs == (page <= 2));
        if (page <= 2) CHECK(v[1] == FEATURES_REQ_EXT && !memcmp(copied, local_features(page), 8));
    }
    reset(); CHECK(lmp_tx_features_req_ext(&conn, -1) == 0 && !outputs);
    memset(hci_param.ext_features + 8, 0, 8); ext[0] = 0;
    reset(); lmp_rx_features_req_ext(&conn, ext); CHECK(v[3] == 1);
    conn.core.state = 1; ext[0] = 1;
    reset(); lmp_rx_features_res_ext(&conn, ext); CHECK(!outputs);
    reset(); lmp_rx_feature_req(&conn, ext + 2);
    CHECK(v[0] == PDU(FEATURES_RES) && !memcmp(copied, hci_param.features, 8));
    lmp_rx_features_res(&conn, hci_param.features);
    CHECK(!memcmp(conn.core_s.remote_features, hci_param.features, 8));

    hci_param.lmp_pal_version = 13; hci_param.manufacturer_name = 0x05d6; hci_param.lmp_pal_subversion = 0x4321;
    reset(); lmp_tx_version_req(&conn);
    CHECK(v[0] == PDU(VERSION_REQ) && v[1] == 13 && v[2] == 0x05d6 && v[3] == 0x4321);
    reset(); lmp_rx_version_req(&conn);
    CHECK(v[0] == PDU(VERSION_RES) && v[2] == 0x05d6 && v[3] == 0x4321);
    conn.core_s.handle = 0xabc;
    u8 ver[] = {12, 0xd6, 5, 0x21, 0x43};
    reset(); lmp_rx_version_res(&conn, ver);
    CHECK(event_id == 0x0c && v[0] == 0 && v[1] == 0xabc && v[2] == 12 && v[3] == 0x05d6 && v[4] == 0x4321);

    /* Sweep every offset against empty, short and unterminated local names. */
    const int lengths[] = {0, 7, 34, sizeof(gbredr_local_dev.local_name)};
    for (unsigned k = 0; k < 4; k++) {
        memset(gbredr_local_dev.local_name, 0, sizeof(gbredr_local_dev.local_name));
        memset(gbredr_local_dev.local_name, 'x', lengths[k]);
        for (int offset = 0; offset < 256; offset++) {
            u8 p = offset; reset(); lmp_rx_name_req(&conn, &p);
            if (offset <= lengths[k]) {
                CHECK(v[0] == PDU(NAME_RES) && v[1] == offset && v[2] == lengths[k]);
                for (int i = 0; i < 14; i++) CHECK(copied[i] == (offset + i < lengths[k] ? 'x' : 0));
            } else CHECK(v[0] == PDU(NOT_ACCEPTED) && v[1] == NAME_REQ && v[2] == 0x1e);
        }
    }
    /* Every possible remote offset/total: no negative or overrun copy, and
     * partial buffers are released on completion or malformed replies. */
    u8 name[16]; memset(name + 2, 'n', 14);
    for (int len = 0; len < 256; len++) {
        for (int offset = 0; offset < 256; offset++) {
            name[0] = offset; name[1] = len; reset(); lmp_rx_name_res(&conn, name);
            if (len > 248 || offset > len) CHECK(event_id == 7 && v[0] == 0x1e && !conn.remote_name);
            else if (len > offset + 14) CHECK(v[0] == PDU(NAME_REQ) && v[1] == offset + 14 && conn.remote_name);
            else CHECK(event_id == 7 && v[0] == 0 && !conn.remote_name);
            if (conn.remote_name) { lmp_free(conn.remote_name); conn.remote_name = 0; }
            CHECK(allocations == 0);
        }
    }
    name[1] = 248;
    for (int offset = 0; offset < 248; offset += 14) {
        name[0] = offset; reset(); lmp_rx_name_res(&conn, name);
    }
    CHECK(!conn.remote_name && allocations == 0 && event_id == 7);
    for (int i = 0; i < 32; i++) CHECK(copied[i] == 'n');
    name[0] = 0; name[1] = 28; reset(); lmp_rx_name_res(&conn, name);
    CHECK(conn.remote_name && allocations == 1);
    name[0] = 255; reset(); lmp_rx_name_res(&conn, name); CHECK(!conn.remote_name && allocations == 0);
    name[0] = 0; fail_alloc = 1; reset(); lmp_rx_name_res(&conn, name);
    CHECK(asserts == 1 && !outputs && !conn.remote_name);

    conn.core.state = 0x100;
    reset(); lmp_tx_connection_req(&conn); CHECK(conn.core.state == 0x101 && v[0] == PDU(HOST_CONNECTION_REQ));
    reset(); lmp_setup_complete(&conn);
    CHECK(conn.core.state == 0x105 && !strcmp(sequence, "qaq") && afh_calls == 1);
    reset(); lmp_tx_setup_complete(&conn); CHECK(!outputs && afh_calls == 1);
    return TEST_DONE("btctrler_lmp_info");
}
#endif
