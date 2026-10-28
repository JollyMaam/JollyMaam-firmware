/* BD19's second tws_pair.c.o occurrence: classic pairing API. */
#include <stddef.h>
#include "btctrler/glue.h"

/* These are private initialized globals in BD19 occurrence 2. */
static u8 wait_pair_addr[6] = { 0x14, 0x30, 0xe3, 0x11, 0x08, 0x3a };
static u8 handle_create __attribute__((aligned(4)));
static u8 handle_timer __attribute__((aligned(4)));

extern u8 get_esco_coder_busy_flag(void);
extern void put_buf(const u8 *buf, int len);
extern void tws_conn_send_event(int code, const char *format, ...);
extern int link_inquiry_scan_enable(int clk_base);
extern void link_inquiry_scan_disable(void);
extern int link_page_scan_enable(int clk_base);
extern void link_page_scan_disable(void);
extern int __page_scan_enable(int clk_base, u8 *local_addr, u8 rand_enable,
                              u8 quick_mode, u8 *quick_conn_addr);
extern int __page_enable(u8 *remote_addr, u8 remote_sr, int clk_offset,
                         u8 *local_addr, u16 timeout, u8 rand_enable,
                         u8 quick_mode, u8 *quick_conn_addr,
                         void (*completed)(u8 *, int));
extern void link_page_disable(int reason);
extern void __quick_page_scan_disable(void);

static void tws_sys_timeout_del(void);
static void tws_create_conneciton_timeout(void *priv);
static void tws_search_sibling_timeout(void *priv);

static void tws_sys_timeout_del(void)
{
    local_irq_disable();
    if (handle_timer != 0xff) {
        slot_timer_put(handle_timer);
        handle_timer = 0xff;
    }
    local_irq_enable();
}

static void tws_create_conneciton_timeout(void *priv)
{
    struct lmp_connection *conn;
    (void)priv;
    slot_timer_put(handle_timer);
    handle_timer = 0xff;
    if (tws_link.link == NULL) {
        conn = lmp_conn_for_address(NULL);
        tws_pair_cancle_create_connection();
        tws_conn_send_event(3, "111", 0, conn != NULL, handle_create);
    }
}

static void tws_search_sibling_timeout(void *priv)
{
    (void)priv;
    slot_timer_put(handle_timer);
    handle_timer = 0xff;
    link_page_scan_disable();
    tws_conn_send_event(1, NULL);
}

static inline __attribute__((always_inline)) void
arm_timer(void (*callback)(void *), int timeout_ms)
{
    u8 timer;
    local_irq_disable();
    timer = handle_timer;
    if (timer == 0xff) {
        timer = slot_timer_get();
        handle_timer = timer;
    }
    if (timer != 0xff)
        slot_timer_set(timer, callback, NULL, (timeout_ms * 1000) / 625);
    local_irq_enable();
}

int tws_pair_wait_connection(int timeout_ms);
int tws_pair_wait_connection(int timeout_ms)
{
    struct lmp_connection *conn = lmp_conn_for_address(NULL);
    int err;
    (void)timeout_ms;
    if (conn == NULL || tws_link.link != NULL)
        return -22;
    if ((conn->core.state & 8) != 0)
        return -22;
    if (get_esco_coder_busy_flag() != 0)
        return -22;

    put_buf(gbredr_local_dev.internal_addr_local, 6);
    link_inquiry_disable();
    link_inquiry_scan_disable();
    link_page_disable(254);
    link_page_scan_disable();
    err = __page_scan_enable(0, gbredr_local_dev.internal_addr_local,
                             0, 1, tws_api_get_quick_connect_addr());
    if (err == 0)
        handle_create = 5;
    return err;
}

int tws_pair_cancle_create_connection(void);
int tws_pair_cancle_create_connection(void)
{
    tws_sys_timeout_del();
    if (handle_create == 3) {
        link_page_disable(254);
        link_page_scan_disable();
    } else if (handle_create == 5) {
        link_page_scan_disable();
    }
    handle_create = 0;
    return 0;
}

int tws_pair_create_connection(int timeout_ms);
int tws_pair_create_connection(int timeout_ms)
{
    struct lmp_connection *conn;
    int tries = 0, err;
    conn = lmp_conn_for_address(NULL);
    while (conn != NULL && (conn->core.state & 8) != 0) {
        os_time_dly(4);
        ++tries;
        if (tries >= 10)
            break;
        conn = lmp_conn_for_address(NULL);
    }
    if (conn != NULL)
        return -14;
    if (tws_link.link != NULL)
        return -22;

    link_inquiry_disable();
    link_page_disable(254);
    __quick_page_scan_disable();
    err = __page_enable(gbredr_local_dev.internal_addr_remote, 0, 0,
                        gbredr_local_dev.internal_addr_local, 0, 1, 1,
                        tws_api_get_quick_connect_addr(), NULL);
    if (err != 0)
        return err;
    err = __page_scan_enable(0, gbredr_local_dev.internal_addr_local,
                             1, 1, tws_api_get_quick_connect_addr());
    if (err != 0) {
        link_page_disable(254);
        return err;
    }
    if (timeout_ms != 0)
        arm_timer(tws_create_conneciton_timeout, timeout_ms);
    handle_create = 3;
    return 0;
}

int tws_pair_search_sibling_by_code(int code, int timeout_ms);
int tws_pair_search_sibling_by_code(int code, int timeout_ms)
{
    int err;
    if (tws_link.link != NULL)
        return -22;
    wait_pair_addr[0] = (u8)code;
    wait_pair_addr[1] = (u8)((unsigned)code >> 8);
    link_inquiry_disable();
    link_inquiry_scan_disable();
    link_page_disable(254);
    link_page_scan_disable();
    err = __page_scan_enable(0, gbredr_local_dev.internal_addr_local,
                             1, 1, wait_pair_addr);
    if (err != 0)
        return err;
    if (timeout_ms != 0)
        arm_timer(tws_search_sibling_timeout, timeout_ms);
    handle_create = 2;
    return 0;
}

int tws_pair_wait_pair_by_code(int code, const char *timed_out);
int tws_pair_wait_pair_by_code(int code, const char *timed_out)
{
    struct lmp_connection *conn;
    int tries = 0, err = 0, timed_out_busy = 0;
    for (;;) {
        conn = lmp_conn_for_address(NULL);
        if (conn == NULL) break;
        if ((conn->core.state & 8) == 0) return -14;
        os_time_dly(4);
        if (++tries == 10) { timed_out_busy = 1; break; }
    }
    wait_pair_addr[0] = (u8)code;
    wait_pair_addr[1] = (u8)((unsigned)code >> 8);
    link_inquiry_disable();
    link_inquiry_scan_disable();
    link_page_disable(254);
    link_page_scan_disable();
    struct link_connection *link = tws_link.link;
    if ((link != NULL && (timed_out_busy || tws_conn.role == 1)) ||
        tws_conn.monitor_start != 0)
        return -22;
    if (timed_out != NULL) {
        err = link_inquiry_scan_enable(0);
        if (err != 0)
            return err;
        err = link_page_scan_enable(0);
        if (err != 0) {
            link_inquiry_scan_disable();
            return err;
        }
    }
    if (tws_link.link == NULL) {
        err = __page_enable(gbredr_local_dev.internal_addr_remote, 0, 0,
                            gbredr_local_dev.internal_addr_local, 0, 1, 1,
                            wait_pair_addr, NULL);
        if (err != 0) {
            link_page_scan_disable();
            link_inquiry_scan_disable();
            return err;
        }
    }
    handle_create = 1;
    return 0;
}

int tws_pair_cancle_wait_pair(void);
int tws_pair_cancle_wait_pair(void)
{
    if (handle_create == 1) {
        handle_create = 0;
        link_inquiry_scan_disable();
        link_page_scan_disable();
    }
    link_inquiry_disable();
    link_inquiry_scan_disable();
    link_page_disable(254);
    link_page_scan_disable();
    return 0;
}

int tws_pair_wait_connect_in_esco(void);
int tws_pair_wait_connect_in_esco(void)
{
    int err;
    if (tws_link.link != NULL)
        return -22;
    err = __page_scan_enable(0, gbredr_local_dev.internal_addr_local,
                             0, 1, tws_api_get_quick_connect_addr());
    if (err == 0)
        handle_create = 5;
    return err;
}

int tws_pair_cancle_wait_connect_in_esco(void);
int tws_pair_cancle_wait_connect_in_esco(void) { return 0; }

void tws_pair_conntion_suss_irq(int reason);
void tws_pair_conntion_suss_irq(int reason)
{
    (void)reason;
    handle_create = 0;
    tws_sys_timeout_del();
}

void tws_pair_init(void);
void tws_pair_init(void)
{
    handle_create = 0;
    handle_timer = 0xff;
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#include <string.h>

struct tws_link_connection tws_link;
struct tws_connection tws_conn;
struct bredr_local_device gbredr_local_dev;
static struct lmp_connection test_lmp;
static struct lmp_connection *test_connection;
static int test_busy, test_page_result, test_scan_result, test_delay_count;
static int test_lookup_count;
static int test_page_calls, test_scan_calls, test_page_disable_count;
static int test_scan_disable_count, test_inquiry_disable_count;
static int test_irq_off, test_irq_on, test_timer_get, test_timer_put;
static int test_timer_set, test_timer_slots, test_event_count, test_last_event;
static u8 test_timer_id;
static int test_clear_on_tenth_delay, test_inquiry_scan_result, test_page_scan_result;
static int test_quick_calls, test_event_args[3], test_mutate_put;
static int test_last_irq_state;
static u8 *test_last_page_address, *test_last_scan_address;
static struct link_connection test_link_object;
static void (*test_timeout_callback)(void *);

struct lmp_connection *lmp_conn_for_address(u8 *addr)
{ CHECK(addr == NULL); ++test_lookup_count; return test_connection; }
u8 get_esco_coder_busy_flag(void) { return (u8)test_busy; }
void put_buf(const u8 *buf, int len) { CHECK(buf == gbredr_local_dev.internal_addr_local && len == 6); }
void link_inquiry_disable(void) { ++test_inquiry_disable_count; }
void link_inquiry_scan_disable(void) { ++test_scan_disable_count; }
int link_inquiry_scan_enable(int base) { CHECK(base == 0); return test_inquiry_scan_result; }
void link_page_scan_disable(void) { ++test_scan_disable_count; }
int link_page_scan_enable(int base) { CHECK(base == 0); return test_page_scan_result; }
void link_page_disable(int reason) { CHECK(reason == 254); ++test_page_disable_count; }
void __quick_page_scan_disable(void) { }
int __page_scan_enable(int base, u8 *local, u8 random, u8 quick, u8 *address)
{ CHECK(base == 0 && local == gbredr_local_dev.internal_addr_local); CHECK(random <= 1 && quick == 1); test_last_scan_address=address; ++test_scan_calls; return test_scan_result; }
int __page_enable(u8 *remote, u8 sr, int offset, u8 *local, u16 timeout,
                  u8 random, u8 quick, u8 *address, void (*done)(u8 *, int))
{ CHECK(remote == gbredr_local_dev.internal_addr_remote && sr == 0 && offset == 0); CHECK(local == gbredr_local_dev.internal_addr_local && timeout == 0 && random == 1 && quick == 1); CHECK(done == NULL); test_last_page_address=address; ++test_page_calls; return test_page_result; }
u8 *tws_api_get_quick_connect_addr(void) { return gbredr_local_dev.internal_addr_remote + (test_quick_calls++ & 1); }
void os_time_dly(int ticks) { CHECK(ticks == 4); ++test_delay_count; if(test_clear_on_tenth_delay && test_delay_count==10) test_lmp.core.state=0; }
void local_irq_disable(void) { ++test_irq_off; test_last_irq_state=handle_create; }
void local_irq_enable(void) { ++test_irq_on; }
u8 slot_timer_get(void) { ++test_timer_get; return test_timer_id; }
void slot_timer_put(u8 id) { CHECK(id == test_timer_id); ++test_timer_put; if(test_mutate_put) handle_timer=123; }
void slot_timer_set(u8 id, void (*cb)(void *), void *priv, int slots)
{ CHECK(id == test_timer_id && priv == NULL); test_timeout_callback=cb; test_timer_slots=slots; ++test_timer_set; }
void tws_conn_send_event(int code, const char *format, ...)
{
    ++test_event_count; test_last_event=code;
    if(format) {
        CHECK(strcmp(format,"111")==0);
        va_list args; va_start(args,format);
        for(unsigned i=0;i<3;i++) test_event_args[i]=va_arg(args,int);
        va_end(args);
    }
}

int main(void)
{
    memset(&tws_link, 0, sizeof(tws_link));
    memset(&tws_conn, 0, sizeof(tws_conn));
    memset(&gbredr_local_dev, 0, sizeof(gbredr_local_dev));
    memset(&test_lmp, 0, sizeof(test_lmp));
    test_connection = NULL;
    CHECK(wait_pair_addr[0] == 0x14 && wait_pair_addr[1] == 0x30 &&
          wait_pair_addr[2] == 0xe3 && wait_pair_addr[3] == 0x11 &&
          wait_pair_addr[4] == 0x08 && wait_pair_addr[5] == 0x3a);
    tws_pair_init();
    CHECK(handle_create == 0 && handle_timer == 0xff);

    /* IR returns -22 unless all wait-connection gates pass. */
    CHECK(tws_pair_wait_connection(123) == -22);
    test_connection = &test_lmp;
    test_lmp.core.state = 8;
    CHECK(tws_pair_wait_connection(123) == -22);
    test_lmp.core.state = 0;
    test_busy = 1;
    CHECK(tws_pair_wait_connection(123) == -22);
    test_busy = 0;
    test_scan_result = 0;
    CHECK(tws_pair_wait_connection(123) == 0 && handle_create == 5);

    /* Page and scan are two distinct calls, with the second address lookup. */
    test_connection = NULL;
    test_page_result = 0; test_scan_result = 0; test_timer_id = 7;
    CHECK(tws_pair_create_connection(625) == 0);
    CHECK(test_page_calls == 1 && test_scan_calls == 2);
    CHECK(test_timer_get == 1 && test_timer_set == 1 && test_timer_slots == 1000);
    CHECK(test_timeout_callback == tws_create_conneciton_timeout && handle_create == 3);
    test_timeout_callback(NULL);
    CHECK(test_timer_put == 1 && test_event_count == 1 && test_last_event == 3);
    CHECK(handle_create == 0 && handle_timer == 0xff);

    /* Allocation sentinel 255 skips arming but retains operation success. */
    tws_pair_init(); test_timer_id=7; test_timer_set=0;
    CHECK(tws_pair_search_sibling_by_code(0x1234, 625) == 0);
    CHECK(wait_pair_addr[0] == 0x34 && wait_pair_addr[1] == 0x12);
    CHECK(test_timer_set == 1 && handle_create == 2 && test_timer_slots == 1000);
    tws_pair_init(); test_timer_id=0xff; test_timer_set=0;
    CHECK(tws_pair_search_sibling_by_code(0x1234, 625) == 0);
    CHECK(test_timer_set == 0 && handle_create == 2);

    /* Non-null second argument enables scans; failed scan rolls back inquiry. */
    test_scan_result=0; test_page_result=0;
    CHECK(tws_pair_wait_pair_by_code(0x2211, "timeout") == 0);
    CHECK(handle_create == 1 && test_page_calls == 2);
    test_inquiry_scan_result=-7;
    int inquiry_disables = test_inquiry_disable_count;
    CHECK(tws_pair_wait_pair_by_code(0x2211, "timeout") == -7);
    CHECK(test_inquiry_disable_count == inquiry_disables + 1);

    /* Busy lookup retries ten times; it still provisions without a link. */
    test_connection=&test_lmp; test_lmp.core.state=8; test_delay_count=0; test_lookup_count=0;
    test_scan_result=0; test_page_result=0; test_inquiry_scan_result=0;
    CHECK(tws_pair_wait_pair_by_code(0x4455, NULL) == 0);
    CHECK(test_delay_count == 10 && test_lookup_count == 10 && handle_create == 1);
    test_lmp.core.state=0; test_delay_count=0; test_lookup_count=0;
    CHECK(tws_pair_wait_pair_by_code(0x4455, NULL) == -14);
    CHECK(test_delay_count == 0 && test_lookup_count == 1);

    /* Connection callback clears state before timeout deletion; IRQ ordering. */
    tws_pair_conntion_suss_irq(0);
    CHECK(handle_create == 0 && test_irq_off == test_irq_on);
    /* Existing TWS links allow waiting only without exhaustion, with role!=1
     * and monitor==0. The argument is only a boolean, not a readable string. */
    test_clear_on_tenth_delay=0; test_scan_result=test_page_result=0;
    test_inquiry_scan_result=test_page_scan_result=0;
    for(unsigned linked=0;linked<2;linked++) for(unsigned role=0;role<3;role++)
    for(unsigned monitor=0;monitor<3;monitor++) for(unsigned busy=0;busy<2;busy++) {
        tws_pair_init(); tws_link.link=linked ? &test_link_object : NULL;
        tws_conn.role=role; tws_conn.monitor_start=monitor;
        test_lmp.core.state=8; test_connection=busy ? &test_lmp : NULL;
        test_delay_count=test_lookup_count=0;
        int expected=(monitor || (linked && (role==1 || busy))) ? -22 : 0;
        CHECK(tws_pair_wait_pair_by_code(0xabcd,NULL)==expected);
        CHECK(test_delay_count==(busy ? 10 : 0) && test_lookup_count==(busy ? 10 : 1));
    }
    tws_link.link=NULL; tws_conn.monitor_start=0; tws_conn.role=0;
    test_connection=&test_lmp; test_lmp.core.state=8; test_clear_on_tenth_delay=1;
    test_lookup_count=test_delay_count=0;
    CHECK(tws_pair_wait_pair_by_code(1,NULL)==0 && test_lookup_count==10);
    test_lmp.core.state=8; test_lookup_count=test_delay_count=0;
    CHECK(tws_pair_create_connection(0)==-14 && test_lookup_count==10);
    test_clear_on_tenth_delay=0; test_connection=NULL;

    /* Inquiry failure, page-scan failure and page failure retain distinct
     * rollback paths. Initial disable calls are included in these deltas. */
    tws_pair_init(); test_inquiry_scan_result=-6;
    int disabled=test_scan_disable_count;
    CHECK(tws_pair_wait_pair_by_code(1,(const char *)1)==-6);
    CHECK(test_scan_disable_count==disabled+2);
    test_inquiry_scan_result=0; test_page_scan_result=-7; disabled=test_scan_disable_count;
    CHECK(tws_pair_wait_pair_by_code(1,(const char *)1)==-7);
    CHECK(test_scan_disable_count==disabled+3);
    test_page_scan_result=0; test_page_result=-8; disabled=test_scan_disable_count;
    CHECK(tws_pair_wait_pair_by_code(1,NULL)==-8);
    CHECK(test_scan_disable_count==disabled+4);
    test_page_result=0; test_scan_result=-9;
    int page_disables=test_page_disable_count;
    CHECK(tws_pair_create_connection(0)==-9 && test_page_disable_count==page_disables+2);
    test_scan_result=0; tws_pair_init(); test_timer_id=7; test_quick_calls=0;
    CHECK(tws_pair_create_connection(-1)==0 && test_timer_slots==-1 && test_quick_calls==2);
    CHECK(test_last_page_address==gbredr_local_dev.internal_addr_remote &&
          test_last_scan_address==gbredr_local_dev.internal_addr_remote+1);
    int sets_before=test_timer_set, gets_before=test_timer_get;
    CHECK(tws_pair_search_sibling_by_code(-1,0)==0 && test_timer_set==sets_before && test_timer_get==gets_before);
    CHECK(wait_pair_addr[0]==255 && wait_pair_addr[1]==255 && wait_pair_addr[2]==0xe3);
    test_mutate_put=1; tws_pair_conntion_suss_irq(55);
    CHECK(handle_timer==255 && test_last_irq_state==0); test_mutate_put=0;
    /* Cancellation no-op and duplicate disable sequences remain literal. */
    handle_create=5; disabled=test_scan_disable_count;
    CHECK(tws_pair_cancle_wait_connect_in_esco()==0 && handle_create==5 && test_scan_disable_count==disabled);
    handle_create=1; disabled=test_scan_disable_count;
    CHECK(tws_pair_cancle_wait_pair()==0 && handle_create==0 && test_scan_disable_count==disabled+4);
    tws_pair_init(); test_timer_id=7;
    CHECK(tws_pair_search_sibling_by_code(1,1)==0);
    tws_search_sibling_timeout(NULL);
    CHECK(handle_create==2 && handle_timer==255 && test_last_event==1);
    tws_pair_init(); test_connection=NULL;
    CHECK(tws_pair_create_connection(1)==0);
    test_connection=&test_lmp; tws_create_conneciton_timeout(NULL);
    CHECK(test_last_event==3 && test_event_args[0]==0 && test_event_args[1]==1 && test_event_args[2]==0);
        return TEST_DONE("bd19 tws_pair_api");
}
#endif
