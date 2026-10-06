/* Classic connection suspend/resume and transmit queue draining, from SDK
 * lmp.c. Bulk allocation and radio scheduling remain SDK APIs. */
#include "btctrler/glue.h"

_Static_assert(!BTCFG_TWS, "suspend policy requires the configured TWS-disabled build");
#ifndef UNITTEST
_Static_assert(offsetof(struct bredr_tx_bulk, entry) == 16 &&
               offsetof(struct bredr_rx_bulk, entry) == 16,
               "SDK bulk list offsets");
#endif

static int empty(const struct list_head *head) { return head->next == head; }
static void remove_entry(struct list_head *entry)
{ entry->next->prev = entry->prev; entry->prev->next = entry->next; }
static struct bredr_rx_bulk *rx_for_entry(struct list_head *entry)
{ return (struct bredr_rx_bulk *)((u8 *)entry - offsetof(struct bredr_rx_bulk, entry)); }
static struct list_head *tx_head(struct lmp_connection *conn, int user)
{ return user ? &conn->link->tx_acl_u : &conn->link->tx_acl_c; }
static void drain_tx(struct lmp_connection *conn, int user)
{
    struct list_head *entry = tx_head(conn, user)->next;
    while (entry != tx_head(conn, user)) {
        /* The SDK allocator unlinks and releases the transmit bulk. */
        struct list_head *next = entry->next;
        struct bredr_tx_bulk *bulk = (void *)((u8 *)entry - offsetof(struct bredr_tx_bulk, entry));
        bredr_tx_bulk_free(bulk);
        entry = next;
    }
}
void lmp_tx_suspend(void)
{
    local_irq_disable();
    for (struct lmp_connection *conn = lmp_acl_link; conn < lmp_acl_link + 1; ++conn) {
        if (!conn->link) continue;
        drain_tx(conn, 1);
        drain_tx(conn, 0);
    }
    local_irq_enable();
}
int lmp_conn_suspend(u8 suspend_rx_bulk)
{
    if (!(config_btctler_modules & 1)) return 0;
    link_conn_rx_bulk_avaliable(NULL, NULL);
    local_irq_disable();
    for (struct lmp_connection *conn = lmp_acl_link; conn < lmp_acl_link + 1; ++conn) {
        if (!conn->link) continue;
        conn->core.suspend = suspend_rx_bulk;
        tws_phone_link_state_change(conn, NULL, NULL);
        if (conn->sbc_channel) {
            lmp_private_abandon_sbc_data(conn, &conn->link->rx_head, 0);
            struct list_head *head = &conn->rx_acl_u;
            for (struct list_head *entry = head->next, *next; entry != head; entry = next) {
                next = entry->next;
                struct bredr_rx_bulk *bulk = rx_for_entry(entry);
                if (bulk->delete || bulk->sbc_flag == 1) {
                    remove_entry(entry);
                    bredr_rx_bulk_free(bulk);
                }
            }
        }
        /* Preserve the SDK's OR of the two queue-empty checks. */
        if (empty(&conn->link->rx_head) || empty(&conn->rx_acl_u) ||
            (!conn->simple && !conn->encrypt && !conn->rs_process))
            bredr_link_event(conn->link, 1, 5);
    }
    local_irq_enable();
    return 0;
}
void lmp_conn_resume(void)
{
    local_irq_disable();
    for (struct lmp_connection *conn = lmp_acl_link; conn < lmp_acl_link + 1; ++conn) {
        if (!conn->link) continue;
        conn->core.suspend = 0;
        tws_phone_link_state_change(conn, NULL, NULL);
        link_conn_rx_bulk_avaliable(conn->link, NULL);
    }
    local_irq_enable();
}

#ifdef UNITTEST
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct lmp_connection lmp_acl_link[1];
int config_btctler_modules;
static struct link_connection link, replacement;
static struct lmp_simple_pair pairing;
static struct lmp_encrypt encryption;
static struct lmp_role_switch role_switch;
static int irq_depth, state_calls, private_calls, events, rx_freed, tx_freed, change_link;
static unsigned expected_suspend;
static u8 tx_order[16];
static char trace[64];
static unsigned at;
static void record(char c) { assert(at + 1 < sizeof(trace)); trace[at++] = c; trace[at] = 0; }
static void init_list(struct list_head *head) { head->next = head->prev = head; }
static void append(struct list_head *head, struct list_head *entry)
{
    entry->next = head; entry->prev = head->prev;
    head->prev->next = entry; head->prev = entry;
}
static struct bredr_rx_bulk *add_rx(struct list_head *head, int deleted, int sbc)
{
    struct bredr_rx_bulk *bulk = calloc(1, sizeof(*bulk)); assert(bulk);
    bulk->delete = deleted; bulk->sbc_flag = sbc; append(head, &bulk->entry); return bulk;
}
static void add_tx(struct list_head *head, int id)
{
    struct bredr_tx_bulk *bulk = calloc(1, sizeof(*bulk)); assert(bulk);
    bulk->enc_prob = id; append(head, &bulk->entry);
}
static void clear_rx(struct list_head *head)
{
    while (!empty(head)) {
        struct list_head *entry = head->next; remove_entry(entry); free(rx_for_entry(entry));
    }
}
void local_irq_disable(void) { assert(!irq_depth); irq_depth = 1; record('D'); }
void local_irq_enable(void) { assert(irq_depth == 1); irq_depth = 0; record('E'); }
void tws_phone_link_state_change(struct lmp_connection *conn, struct bredr_rx_bulk *rx, int *cmd)
{
    assert(irq_depth && conn == lmp_acl_link && !rx && !cmd);
    assert(conn->core.suspend == expected_suspend); ++state_calls; record('S');
    if (change_link) conn->link = &replacement;
}
void link_conn_rx_bulk_avaliable(struct link_connection *lc, struct bredr_rx_bulk *bulk)
{
    assert(!bulk);
    if (!lc) { assert(!irq_depth); record('A'); }
    else { assert(irq_depth && lc == lmp_acl_link->link); record('R'); }
}
int lmp_private_abandon_sbc_data(struct lmp_connection *conn, struct list_head *head, u16 seq)
{
    assert(irq_depth && conn == lmp_acl_link && head == &conn->link->rx_head && !seq);
    ++private_calls; record('P'); return -1;
}
void bredr_rx_bulk_free(struct bredr_rx_bulk *bulk)
{
    assert(irq_depth && (bulk->delete || bulk->sbc_flag == 1));
    ++rx_freed; record('f'); free(bulk);
}
void bredr_tx_bulk_free(struct bredr_tx_bulk *bulk)
{
    assert(irq_depth && tx_freed < 16); tx_order[tx_freed++] = bulk->enc_prob;
    remove_entry(&bulk->entry); free(bulk);
}
int bredr_link_event(void *lc, int argc, ...)
{
    assert(irq_depth && lc == lmp_acl_link->link && argc == 1);
    va_list args; va_start(args, argc); assert(va_arg(args, int) == 5); va_end(args);
    ++events; record('Q'); return 21; /* Queue-full result is ignored by SDK policy. */
}
static void reset(void)
{
    assert(!irq_depth);
    memset(lmp_acl_link, 0, sizeof(lmp_acl_link)); memset(&link, 0, sizeof(link));
    memset(&replacement, 0, sizeof(replacement));
    init_list(&link.rx_head); init_list(&link.tx_acl_u); init_list(&link.tx_acl_c);
    init_list(&replacement.rx_head); init_list(&lmp_acl_link->rx_acl_u);
    lmp_acl_link->link = &link; config_btctler_modules = 3;
    state_calls = private_calls = events = rx_freed = tx_freed = change_link = 0;
    expected_suspend = 0; at = 0; trace[0] = 0;
}
int main(void)
{
    for (int modules = 0; modules < 8; ++modules) {
        reset(); config_btctler_modules = modules; lmp_acl_link->link = NULL;
        lmp_acl_link->core.suspend = 17;
        assert(!lmp_conn_suspend(23));
        assert(!strcmp(trace, modules & 1 ? "ADE" : "") && lmp_acl_link->core.suspend == 17);
        at = 0; trace[0] = 0; lmp_conn_resume();
        assert(!strcmp(trace, "DE") && lmp_acl_link->core.suspend == 17);
        at = 0; trace[0] = 0; lmp_tx_suspend(); assert(!strcmp(trace, "DE"));
    }
    for (unsigned mode = 0; mode < 256; ++mode)
        for (int busy = 0; busy < 4; ++busy)
            for (int state = 0; state < 8; ++state) {
                reset(); expected_suspend = mode;
                lmp_acl_link->core.state = 0xa5a55a5a;
                lmp_acl_link->simple = state & 1 ? &pairing : NULL;
                lmp_acl_link->encrypt = state & 2 ? &encryption : NULL;
                lmp_acl_link->rs_process = state & 4 ? &role_switch : NULL;
                if (busy & 1) add_rx(&link.rx_head, 0, 0);
                if (busy & 2) add_rx(&lmp_acl_link->rx_acl_u, 0, 0);
                assert(!lmp_conn_suspend(mode));
                int scheduled = busy != 3 || !state;
                assert(!strcmp(trace, scheduled ? "ADSQE" : "ADSE"));
                assert(events == scheduled && state_calls == 1 && !private_calls && !rx_freed);
                assert(lmp_acl_link->core.suspend == mode && lmp_acl_link->core.state == 0xa5a55a5a);
                clear_rx(&link.rx_head); clear_rx(&lmp_acl_link->rx_acl_u);
                expected_suspend = 0; at = 0; trace[0] = 0;
                lmp_conn_resume(); assert(!strcmp(trace, "DSRE") && state_calls == 2);
                assert(!lmp_acl_link->core.suspend && lmp_acl_link->core.state == 0xa5a55a5a);
            }
    for (int sbc_channel = 0; sbc_channel < 2; ++sbc_channel)
        for (int deleted = 0; deleted < 2; ++deleted)
            for (int flag = 0; flag < 4; ++flag) {
                reset(); lmp_acl_link->sbc_channel = sbc_channel ? 0x1234 : 0;
                lmp_acl_link->simple = &pairing;
                add_rx(&link.rx_head, 0, 0);
                add_rx(&lmp_acl_link->rx_acl_u, deleted, flag);
                int removed = sbc_channel && (deleted || flag == 1);
                assert(!lmp_conn_suspend(0));
                assert(private_calls == sbc_channel && rx_freed == removed && events == removed);
                assert(empty(&lmp_acl_link->rx_acl_u) == removed);
                clear_rx(&link.rx_head); clear_rx(&lmp_acl_link->rx_acl_u);
            }
    /* Adjacent frees exercise traversal after the allocator releases each node. */
    reset(); lmp_acl_link->sbc_channel = 1;
    add_rx(&lmp_acl_link->rx_acl_u, 1, 0); add_rx(&lmp_acl_link->rx_acl_u, 0, 1);
    add_rx(&lmp_acl_link->rx_acl_u, 0, 2); add_rx(&lmp_acl_link->rx_acl_u, 1, 3);
    lmp_conn_suspend(0); assert(rx_freed == 3 && private_calls == 1);
    assert(lmp_acl_link->rx_acl_u.next == lmp_acl_link->rx_acl_u.prev);
    clear_rx(&lmp_acl_link->rx_acl_u);
    for (int user = 0; user <= 4; ++user)
        for (int control = 0; control <= 4; ++control) {
            reset();
            for (int i = 0; i < user; ++i) add_tx(&link.tx_acl_u, i + 1);
            for (int i = 0; i < control; ++i) add_tx(&link.tx_acl_c, i + 9);
            lmp_tx_suspend();
            assert(!strcmp(trace, "DE") && tx_freed == user + control);
            assert(empty(&link.tx_acl_u) && empty(&link.tx_acl_c));
            for (int i = 0; i < user; ++i) assert(tx_order[i] == i + 1);
            for (int i = 0; i < control; ++i) assert(tx_order[user + i] == i + 9);
        }
    /* State callbacks may update the link: subsequent calls use the new one. */
    reset(); change_link = 1; lmp_acl_link->core.suspend = 255;
    lmp_conn_resume(); assert(!strcmp(trace, "DSRE") && lmp_acl_link->link == &replacement);
    reset(); change_link = 1; lmp_acl_link->sbc_channel = 1;
    lmp_conn_suspend(0); assert(!strcmp(trace, "ADSPQE") && lmp_acl_link->link == &replacement);
    puts("btctrler connection suspend tests passed");
}
#endif
