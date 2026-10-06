/* Adaptive frequency hopping policy, recovered from SDK lmp.c.
 * Channel measurements, map construction and radio instants stay in the SDK. */
#include <string.h>
#include "btctrler/glue.h"
#include "btctrler/lmp_pdu.h"

extern BTCTRLER_CONFIG int config_bt_function;
static u8 last_map[10]; /* SDK callback's static map; one ACL slot. */

#ifndef UNITTEST
_Static_assert(offsetof(struct lmp_connection, link) == 220 &&
               offsetof(struct lmp_connection, afh_report.timer) == 248 &&
               offsetof(struct lmp_connection, afh_report.tx_channel_classification) == 256,
               "SDK timer callback connection offsets");
#endif

void lmp_tx_channel_classification_timeout(void *priv)
{
    struct lmp_connection *conn = priv;
    if (!conn || !conn->link || !conn->afh_report.timer) return;
    conn->afh_report.timer = 0;
    u8 *map = conn->afh_report.tx_channel_classification;
    if (!memcmp(map, last_map, sizeof(last_map))) return;
    conn->tran_id &= ~1;
    lmp_response(conn, "11C010", PDU(ESCAPE_4), CHANNEL_CLASSIFICATION, map);
    memcpy(last_map, map, sizeof(last_map));
}

void lmp_tx_channel_classification(u8 *map)
{
    if (!(hci_param.features[4] & 0x10)) return;
    for (struct lmp_connection *conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        struct lmp_afh_report *report = &conn->afh_report;
        if (!conn->core_s.handle || !report->mode || conn->rs_process || conn->core.role != 1 ||
            !(conn->core_s.remote_features[4] & 0x10) || report->timer) continue;
        memcpy(report->tx_channel_classification, map, 10);
        u32 slots = 1792;
        if (report->min_interval > slots) slots = report->min_interval;
        else if (report->max_interval < slots) slots = report->max_interval;
        /* The SDK truncates slots * 0.625 to milliseconds. Integer maths
         * preserves that result without the BR23's emulated FPU. */
        report->timer = sys_timeout_add(conn, lmp_tx_channel_classification_timeout, slots * 5 / 8);
    }
}

void lmp_master_tx_set_afh_user(u8 *map)
{
    for (struct lmp_connection *conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++)
        if (conn->core_s.handle && conn->core.role == 0) lmp_tx_set_afh_req(conn, map);
}

void lmp_rx_channel_classification(struct lmp_connection *conn, u8 *rx)
{ memcpy(conn->afh_report.rx_channel_classification, rx, 10); }

void lmp_tx_channel_classification_req(struct lmp_connection *conn, u8 en)
{
    conn->afh_report.mode = en;
    lmp_request(conn, "11122", PDU(ESCAPE_4), CHANNEL_CLASSIFICATION_REQ, en, 1600, 48000);
}

void lmp_rx_channel_classification_req(struct lmp_connection *conn, u8 *rx)
{
    struct lmp_afh_report *report = &conn->afh_report;
    report->mode = rx[0];
    /* Fix: after the two opcode bytes the payload is mode, min LE16, max
     * LE16. The SDK skipped the first interval byte and read rx[5]. */
    report->min_interval = rx[1] | (rx[2] << 8);
    report->max_interval = rx[3] | (rx[4] << 8);
    if (!report->mode && report->timer) {
        sys_timeout_del((u16)report->timer);
        report->timer = 0;
    }
}

void lmp_channel_classification_close(struct lmp_connection *conn)
{
    conn->afh_report.mode = 0;
    if (conn->afh_report.timer) {
        sys_timeout_del((u16)conn->afh_report.timer);
        conn->afh_report.timer = 0;
    }
}

void lmp_rx_not_accepted_afh_req(struct lmp_connection *conn) { (void)conn; }
void __lmp_enable_afh(struct lmp_connection *conn) { (void)conn; }

void lmp_tx_set_afh(struct lmp_connection *conn, u8 mode)
{
    if (!(config_bt_function & 2) || conn->core.role != 0) return;
    u8 map[10];
    if (bredr_link_update_afh(conn->link, map, conn->afh_report.tx_channel_classification, mode))
        lmp_tx_set_afh_req(conn, map);
}

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
struct lmp_connection lmp_acl_link[1];
struct hci_parameter hci_param;
int config_bt_function;
static struct link_connection link;
static int timers, deleted, outputs, afh_sent, update_calls, update_result;
static u32 delay;
static u16 deleted_id;
static void *timer_priv;
static void (*timer_func)(void *);
static int v[5];
static u8 copied[10];
u16 sys_timeout_add(void *priv, void (*func)(void *), u32 msec)
{ timers++; timer_priv = priv; timer_func = func; delay = msec; return 123; }
void sys_timeout_del(u16 id) { deleted++; deleted_id = id; }
int lmp_response(struct lmp_connection *c, const char *format, ...)
{
    CHECK(c == lmp_acl_link && !strcmp(format, "11C010")); outputs++;
    va_list ap; va_start(ap, format); v[0] = va_arg(ap, int); v[1] = va_arg(ap, int);
    memcpy(copied, va_arg(ap, u8 *), 10); va_end(ap); return 0;
}
int lmp_request(struct lmp_connection *c, const char *format, ...)
{
    CHECK(c == lmp_acl_link && !strcmp(format, "11122")); outputs++;
    va_list ap; va_start(ap, format); for (int i = 0; i < 5; i++) v[i] = va_arg(ap, int);
    va_end(ap); return 0;
}
void lmp_tx_set_afh_req(struct lmp_connection *c, u8 *map)
{ CHECK(c == lmp_acl_link); afh_sent++; memcpy(copied, map, 10); }
int bredr_link_update_afh(struct link_connection *lc, u8 *map, u8 *classification, u8 mode)
{
    CHECK(lc == &link && classification == lmp_acl_link[0].afh_report.tx_channel_classification && mode == 1);
    update_calls++; memset(map, 0x5a, 10); return update_result;
}
static void reset(void) { timers = deleted = outputs = afh_sent = update_calls = 0; }
int main(void)
{
    struct lmp_connection *c = lmp_acl_link;
    u8 map[10]; memset(map, 0x33, sizeof(map));
    /* Each independent scheduling gate must prevent timer creation. */
    for (int gate = -1; gate < 7; gate++) {
        reset(); memset(c, 0, sizeof(*c)); c->link = &link;
        hci_param.features[4] = c->core_s.remote_features[4] = 0x10;
        c->core_s.handle = 1; c->core.role = 1; c->afh_report.mode = 1;
        c->afh_report.min_interval = 1600; c->afh_report.max_interval = 48000;
        switch (gate) {
        case 0: hci_param.features[4] = 0; break;
        case 1: c->core_s.handle = 0; break;
        case 2: c->afh_report.mode = 0; break;
        case 3: c->rs_process = (struct lmp_role_switch *)&link; break;
        case 4: c->core.role = 0; break;
        case 5: c->core_s.remote_features[4] = 0; break;
        case 6: c->afh_report.timer = 77; break;
        }
        lmp_tx_channel_classification(map);
        CHECK(timers == (gate == -1));
        if (timers) {
            CHECK(delay == 1120 && timer_priv == c && timer_func == lmp_tx_channel_classification_timeout);
            CHECK(c->afh_report.timer == 123 && !memcmp(c->afh_report.tx_channel_classification, map, 10));
            c->tran_id = 0x81; timer_func(timer_priv);
            CHECK(outputs == 1 && c->tran_id == 0x80 && c->afh_report.timer == 0);
            CHECK(v[0] == 254 && v[1] == CHANNEL_CLASSIFICATION && !memcmp(copied, map, 10));
            c->afh_report.timer = 123; timer_func(timer_priv);
            CHECK(outputs == 1 && c->afh_report.timer == 0); /* unchanged map suppression */
        }
    }
    memset(c, 0, sizeof(*c)); c->link = &link; c->core_s.handle = 1; c->core.role = 1;
    c->afh_report.mode = 1; hci_param.features[4] = c->core_s.remote_features[4] = 0x10;
    /* Full 16-bit interval range: clamp policy and exact truncated conversion. */
    for (u32 interval = 0; interval <= 65535; interval++) {
        c->afh_report.timer = 0; c->afh_report.min_interval = interval; c->afh_report.max_interval = 65535;
        reset(); lmp_tx_channel_classification(map);
        CHECK(timers == 1 && delay == (interval > 1792 ? interval : 1792) * 5 / 8);
        c->afh_report.timer = 0; c->afh_report.min_interval = 0; c->afh_report.max_interval = interval;
        reset(); lmp_tx_channel_classification(map);
        CHECK(timers == 1 && delay == (interval < 1792 ? interval : 1792) * 5 / 8);
    }
    reset(); c->afh_report.timer = 0;
    lmp_tx_channel_classification_timeout(0); lmp_tx_channel_classification_timeout(c); CHECK(!outputs);
    c->afh_report.timer = 123; c->link = 0; lmp_tx_channel_classification_timeout(c);
    CHECK(!outputs && c->afh_report.timer == 123); c->link = &link;
    /* Exact five-byte payload catches the SDK's one-byte overread in ASan. */
    u8 req[] = {1, 0x40, 0x06, 0x80, 0xbb}; reset();
    lmp_rx_channel_classification_req(c, req);
    CHECK(c->afh_report.min_interval == 1600 && c->afh_report.max_interval == 48000 && !deleted);
    req[0] = 0; lmp_rx_channel_classification_req(c, req);
    CHECK(c->afh_report.mode == 0 && !c->afh_report.timer && deleted == 1 && deleted_id == 123);
    lmp_channel_classification_close(c); CHECK(deleted == 1);
    c->afh_report.mode = 1; c->afh_report.timer = 456; lmp_channel_classification_close(c);
    CHECK(!c->afh_report.mode && !c->afh_report.timer && deleted == 2 && deleted_id == 456);
    reset(); lmp_tx_channel_classification_req(c, 1);
    CHECK(c->afh_report.mode == 1 && v[0] == 254 && v[1] == CHANNEL_CLASSIFICATION_REQ &&
          v[2] == 1 && v[3] == 1600 && v[4] == 48000);
    lmp_rx_channel_classification(c, map); CHECK(!memcmp(c->afh_report.rx_channel_classification, map, 10));
    reset(); c->core.role = 1; lmp_master_tx_set_afh_user(map); CHECK(!afh_sent);
    c->core.role = 0; lmp_master_tx_set_afh_user(map); CHECK(afh_sent == 1 && !memcmp(copied, map, 10));
    c->core_s.handle = 0; lmp_master_tx_set_afh_user(map); CHECK(afh_sent == 1);
    reset(); config_bt_function = 0; lmp_tx_set_afh(c, 1); CHECK(!update_calls);
    config_bt_function = 2; c->core.role = 1; lmp_tx_set_afh(c, 1); CHECK(!update_calls);
    c->core.role = 0; update_result = 0; lmp_tx_set_afh(c, 1); CHECK(update_calls == 1 && !afh_sent);
    update_result = 1; lmp_tx_set_afh(c, 1); CHECK(update_calls == 2 && afh_sent == 1 && copied[0] == 0x5a);
    reset(); lmp_rx_not_accepted_afh_req(c); __lmp_enable_afh(c); CHECK(!outputs && !timers);
    return TEST_DONE("btctrler_lmp_afh");
}
#endif
