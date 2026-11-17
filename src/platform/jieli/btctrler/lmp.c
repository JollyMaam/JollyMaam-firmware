/* Connection teardown from the SDK's lmp.c (Classic link manager).  The
 * rest of lmp.c is spread over src/platform/jieli/btctrler/lmp_*.c. */
#include "btctrler/glue.h"

/* lmp_connection.core.state bits used here. */
#define STATE_CONNECTING   (1u << 0)    /* connection complete not sent yet */
#define STATE_CONNECTED    (1u << 1)    /* host knows the handle */
#define STATE_DETACHING    (1u << 3)
#define STATE_SILENT       (1u << 21)   /* tear down without host events */

static int list_empty(const struct list_head *head) { return head->next == head; }

/* Tear down every connection marked for detaching.  Called from the LMP
 * task loop.  The SDK's TWS and eSCO-forwarding branches are left out:
 * both are disabled in this firmware (platform_jieli_sdk.c). */
void lmp_detach_check(void)
{
    for (struct lmp_connection *conn = lmp_acl_link; conn < lmp_acl_link + 1; conn++) {
        if (!conn->core_s.handle || !(conn->core.state & STATE_DETACHING)) continue;
        lmp_rx_handler(conn);
        local_irq_disable();
        if (conn->sbc_open) os_sem_post(&conn->sbc_sem);
        local_irq_enable();
        if (conn->simple) {
            if (conn->simple->ecdh) lmp_free(conn->simple->ecdh);
            lmp_free(conn->simple);
            conn->simple = 0;
        }
        lmp_free_encrypt(conn);
        lmp_channel_classification_close(conn);
        if (conn->enc_timer) { sys_timer_del(conn->enc_timer); conn->enc_timer = 0; }
        if (conn->timer) { sys_timer_del(conn->timer); conn->timer = 0; }
        if (conn->wf_det_time) { sys_timer_del(conn->wf_det_time); conn->wf_det_time = 0; }
        if (conn->remote_name) { lmp_free(conn->remote_name); conn->remote_name = 0; }
        if (conn->link && conn->link->esco) bredr_esco_link_close(conn->link);
        lmp_esco_link_removed(conn, 0x13);
        lmp_sniff_detach(conn);
        if (conn->change_rx_bulk_timer != 0xff) {
            slot_timer_put(conn->change_rx_bulk_timer);
            conn->change_rx_bulk_timer = 0xff;
        }
        local_irq_disable();
        lmp_private_abandon_sbc_data(conn, &conn->rx_acl_u, 0);
        local_irq_enable();

        /* Fix: the SDK waits for the received-ACL queue to empty, but its
         * sender frees a delivered packet only on its next call, and after
         * the peer's LMP_detach there may be none: the last packet (the
         * host's L2CAP disconnect request) stayed queued and the link went
         * only at supervision timeout, reason 0x08.  Hand over and free
         * what is queued here. */
        for (int i = 0; i < 16 && !list_empty(&conn->rx_acl_u); i++)
            lmp_send_acl_u_packet_to_host(conn);
        if (!list_empty(&conn->rx_acl_u)) {
            os_time_dly(2);
            if (conn->err_code != 0x08) continue;    /* try again next pass */
        }

        u32 state = conn->core.state;
        if (state & STATE_SILENT) {
            conn->lmp_detch_start = 1;
            conn->core.state = 0;
        } else if (state & STATE_CONNECTED) {
            conn->core.state = state & ~STATE_CONNECTED;
            conn->lmp_detch_start = 1;
            hci_send_event(0x05, "1H1", 0, conn->core_s.handle, conn->err_code);  /* Disconnection Complete */
            os_time_dly(2);
        } else if (state & STATE_CONNECTING) {
            conn->lmp_detch_start = 1;
            conn->core.state = state & ~STATE_CONNECTING;
            lmp_send_event_connection_complete(conn, conn->err_code, 0, 1);
        }

        struct link_connection *link = conn->link;
        if (!link || !list_empty(&link->rx_head)) continue;
        if (!list_empty(&link->tx_acl_c) || !list_empty(&link->tx_acl_u)) {
            os_time_dly(40);
            link = conn->link;
        }
        conn->link = 0;
        link_conn_close(link);
        lmp_role_switch_detach(conn);
        local_irq_disable();
        conn->core.state = 0;
        conn->core_s.handle = 0;
        conn->link = 0;
        conn->lmp_detch_start = 0;
        local_irq_enable();
        wifibox_bt_freq_finish_post();
        if (config_btctler_mode & 2) lmp_scan_enable(3);
        lmp_scan_enable(hci_param.scan_enable);
        hci_send_event(0xfe, "11", 0, 0);
    }
}

#ifdef UNITTEST
#include <stdarg.h>
#include <stdlib.h>
#include "test.h"
struct lmp_connection lmp_acl_link[1];
struct hci_parameter hci_param;
int config_btctler_modules, config_btctler_mode, CONFIG_BREDR_INQUIRY, CONFIG_LMP_MASTER_ESCO_ENABLE;
static char trace[512];
static int queued, delivered, sent_events;
static void note(const char *s) { strcat(trace, s); strcat(trace, " "); }
/* LMP and HCI output. */
int lmp_response(struct lmp_connection *c, const char *format, ...)
{
    va_list ap; va_start(ap, format); int op = va_arg(ap, int); va_end(ap);
    char s[16]; snprintf(s, sizeof(s), "r%d", op); note(s); (void)c; return 0;
}
int hci_send_event(int event, const char *format, ...)
{ char s[16]; snprintf(s, sizeof(s), "e%x", event); note(s); sent_events++; (void)format; return 0; }
void lmp_send_event_link_key_notification(struct lmp_connection *c, int ret) { note("key"); (void)c; (void)ret; }
void lmp_send_event_connection_complete(struct lmp_connection *c, u8 e, u16 h, u8 t) { note("cc"); (void)c; (void)e; (void)h; (void)t; }
/* Pairing helpers. */
void lmp_authenticated_by_using_one_of_three_algorithms(struct lmp_connection *c) { (void)c; }
void lmp_ecdh_publickey(struct lmp_connection *c) { (void)c; }
void make_rand_num(u8 *b) { memset(b, 7, 16); }
void f1_function_api(u8 *u, u8 *v, u8 *x, u8 *z, u8 *re) { note("f1"); (void)u; (void)v; (void)x; (void)z; memset(re, 1, 16); }
void f2_function_api(u8 *w, u8 *n1, u8 *n2, u8 *a1, u8 *a2, u8 *re) { note("f2"); (void)w; (void)n1; (void)n2; (void)a1; (void)a2; memset(re, 2, 16); }
u32 g_function_api(u8 *u, u8 *v, u8 *x, u8 *y) { (void)u; (void)v; (void)x; (void)y; return 123456; }
void lmp_dhkey_check_accept(struct lmp_connection *c) { note("accept"); (void)c; }
void p33_soft_reset(void) { note("RESET"); }
void lmp_free(void *p) { free(p); }
/* Detach helpers. */
void lmp_rx_handler(struct lmp_connection *c) { (void)c; }
void local_irq_disable(void) {}
void local_irq_enable(void) {}
int os_sem_post(OS_SEM *s) { (void)s; return 0; }
void os_time_dly(int t) { (void)t; note("wait"); }
void sys_timer_del(u16 id) { (void)id; }
void lmp_free_encrypt(struct lmp_connection *c) { (void)c; }
void lmp_channel_classification_close(struct lmp_connection *c) { (void)c; }
void bredr_esco_link_close(struct link_connection *l) { (void)l; }
void lmp_esco_link_removed(struct lmp_connection *c, u8 r) { (void)c; (void)r; }
void lmp_sniff_detach(struct lmp_connection *c) { (void)c; }
void slot_timer_put(u8 i) { (void)i; }
int lmp_private_abandon_sbc_data(struct lmp_connection *c, struct list_head *h, u16 n) { (void)c; (void)h; (void)n; return 0; }
void lmp_send_acl_u_packet_to_host(struct lmp_connection *c)
{
    /* The SDK's sender frees a delivered packet on its next call. */
    if (delivered < queued) delivered++;
    else if (queued) { queued--; delivered--; }
    if (!queued) c->rx_acl_u.next = &c->rx_acl_u;
}
void link_conn_close(struct link_connection *l) { note("close"); (void)l; }
void lmp_role_switch_detach(struct lmp_connection *c) { (void)c; }
int wifibox_bt_freq_finish_post(void) { return 0; }
void lmp_scan_enable(u8 e) { (void)e; }

int main(void)
{
    struct lmp_connection *c = &lmp_acl_link[0];
    struct link_connection link;
    /* A delivered-but-unfreed packet no longer blocks teardown. */
    c->core_s.handle = 0x10; c->core.state = (1u << 3) | (1u << 1); c->link = &link;
    c->change_rx_bulk_timer = 0xff; c->err_code = 0x13;
    link.rx_head.next = &link.rx_head; link.tx_acl_c.next = &link.tx_acl_c; link.tx_acl_u.next = &link.tx_acl_u;
    link.esco = 0;
    static struct list_head packet;
    c->rx_acl_u.next = &packet; queued = 1; delivered = 1;
    lmp_detach_check();
    CHECK(!c->core_s.handle && !c->link && !queued);
    CHECK(!strcmp(trace, "e5 wait close efe "));
    return TEST_DONE("btctrler_lmp");
}
#endif
