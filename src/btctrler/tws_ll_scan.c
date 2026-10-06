/* TWS LE scan link setup, teardown, and receive probe.  Retain the SDK's
 * 4-byte access to the AA, IRQ boundaries, allocation failures, and RX flag
 * rewrite after forwarding each PDU. */
#include "btctrler/glue.h"
#include <string.h>

extern BTCTRLER_CONFIG u8 log_tag_const_i_LL_TWS_SCAN;
extern BTCTRLER_CONFIG u8 log_tag_const_e_LL_TWS_SCAN;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(u8 *data, int len);

#define TWS_LL_CODE __attribute__((section(".ble_ll_code")))

static struct tws_ll_scan_hdl *scan_hdl
    __attribute__((section(".ble_ll_data"), aligned(4)));

static void scan_rx_probe_handler(void *priv, struct ble_rx *rx) TWS_LL_CODE;
static const struct ble_handler scan_handler
    __attribute__((section(".ble_ll_const"), aligned(4))) = {
        .tws_rx_probe_handler = scan_rx_probe_handler,
    };

static void cpu_reset(void)
{
    p33_soft_reset();
}

int tws_ll_scan_open(const u8 *data) TWS_LL_CODE;
int tws_ll_scan_open(const u8 *data)
{
    struct ble_tws_scan scan;
    u8 info_tag = log_tag_const_i_LL_TWS_SCAN;

    if (scan_hdl == NULL) {
        if (info_tag)
            log_print(2, NULL, "[Info]: [LL_TWS_SCAN]LL Scan- init\r\n");
        scan_hdl = __bt_malloc(8);
        if (config_asser) {
            if (scan_hdl == NULL)
                btctrler_assert_failed("__this LL SCAN HDL NULL", 127);
        } else if (scan_hdl == NULL) {
            cpu_reset();
        }
    }

    if (info_tag)
        log_print(2, NULL, "[Info]: [LL_TWS_SCAN]ll_scan_open\r\n");
    local_irq_disable();
    if (scan_hdl->scan_link != NULL) {
        if (log_tag_const_e_LL_TWS_SCAN)
            log_print(4, NULL, "<Error>: [LL_TWS_SCAN]scan link open twice\r\n");
        local_irq_enable();
        return 12;
    }

    struct le_scan_link *link = __bt_malloc(20);
    if (link == NULL) {
        if (log_tag_const_e_LL_TWS_SCAN)
            log_print(4, NULL, "<Error>: [LL_TWS_SCAN]scan link NULL\r\n");
        local_irq_enable();
        return 12;
    }
    scan_hdl->scan_link = link;
    local_irq_enable();

    link->hw = __ble_ops->open();
    if (link->hw == NULL) {
        if (log_tag_const_e_LL_TWS_SCAN)
            log_print(4, NULL, "<Error>: [LL_TWS_SCAN]scan hw NULL\r\n");
        return 58;
    }
    __ble_ops->handler_register(link->hw, link, &scan_handler);

    u32 aa;
    memcpy(&aa, data, sizeof(aa));
    scan_hdl->scan_param.AA = aa;

    if (info_tag) {
        log_print(2, NULL, "[Info]: [LL_TWS_SCAN]>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\r\n");
        u32 logged_aa = scan_hdl->scan_param.AA;
        log_print(2, NULL, "[Info]: [LL_TWS_SCAN]scan AA: %x\r\n", logged_aa);
        scan.aa = logged_aa;
        __set_ll_local_addr(link->hw, &link->local, 0);
        log_print(2, NULL, "[Info]: [LL_TWS_SCAN]>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\r\n");
    } else {
        scan.aa = aa;
        __set_ll_local_addr(link->hw, &link->local, 0);
    }

    __ble_ops->tws_scanning(link->hw, &scan);
    return 0;
}

int tws_ll_scan_close(void) TWS_LL_CODE;
int tws_ll_scan_close(void)
{
    local_irq_disable();
    struct tws_ll_scan_hdl *handle = scan_hdl;
    if (handle == NULL) {
        if (log_tag_const_e_LL_TWS_SCAN)
            log_print(4, NULL, "<Error>: [LL_TWS_SCAN]LL Scan already destroy\r\n");
        local_irq_enable();
        return 0;
    }

    struct le_scan_link *link = handle->scan_link;
    if (link == NULL) {
        local_irq_enable();
        return 2;
    }
    handle->scan_link = NULL;
    local_irq_enable();

    void *hw = link->hw;
    if (hw != NULL)
        __ble_ops->close(hw);
    __bt_free(link);

    handle = scan_hdl;
    if (handle != NULL) {
        if (log_tag_const_i_LL_TWS_SCAN) {
            log_print(2, NULL, "[Info]: [LL_TWS_SCAN]LL Scan - destroy\r\n");
            handle = scan_hdl;
        }
        __bt_free(handle);
        scan_hdl = NULL;
    }
    return 0;
}

struct le_scan_link *tws_ll_scan_link_get(void) TWS_LL_CODE;
struct le_scan_link *tws_ll_scan_link_get(void)
{
    struct tws_ll_scan_hdl *handle = scan_hdl;
    if (handle == NULL) {
        if (log_tag_const_e_LL_TWS_SCAN)
            log_print(4, NULL, "<Error>: [LL_TWS_SCAN]LL Scan not init yet\r\n");
        return NULL;
    }
    return handle->scan_link;
}

static void scan_rx_probe_handler(void *priv, struct ble_rx *rx)
{
    switch (rx->type) {
    case 0:
        if (log_tag_const_i_LL_TWS_SCAN)
            log_print(2, NULL, "[Info]: [LL_TWS_SCAN]ADV_IND : %d\r\n", rx->channel);
        break;
    case 4:
        if (log_tag_const_i_LL_TWS_SCAN) {
            log_print(2, NULL, "[Info]: [LL_TWS_SCAN]SCAN_RSP : %d\r\n", rx->channel);
            printf_buf(rx->data, rx->len);
        }
        break;
    default:
        break;
    }

    tws_rx_probe_scan_pdu_handler(priv, rx);
    rx->len = 0;
    _Static_assert(offsetof(struct ble_rx, len) >= 2, "ble_rx bitfield word offset");
    u16 *flags = (u16 *)(void *)((u8 *)rx + offsetof(struct ble_rx, len) - 2);
    *flags = (u16)((*flags & (u16)-2097) | 2064);
}

#ifdef UNITTEST
#include "test.h"
#include <stdlib.h>

u8 log_tag_const_i_LL_TWS_SCAN = 1;
u8 log_tag_const_e_LL_TWS_SCAN = 1;
int config_asser;
const struct ble_operation *__ble_ops;

static struct ble_operation test_ops;
static struct le_scan_link *registered_link;
static const struct ble_handler *registered_handler;
static void *registered_hw;
static int irq_depth, irq_disables, irq_enables;
static int malloc_count, free_count, fail_allocation;
static int radio_open_fails;
static int malloc_sizes[4];
static int close_count, register_count, scanning_count;
static int local_addr_count, pdu_count, buf_count;
static u32 scanned_aa;
static u8 buf_len;
static u8 buf_data[16];

void log_print(int level, const char *tag, const char *format, ...)
{ (void)level; (void)tag; (void)format; }

void * __bt_malloc(int size)
{
    if (malloc_count < (int)(sizeof(malloc_sizes) / sizeof(malloc_sizes[0])))
        malloc_sizes[malloc_count] = size;
    malloc_count++;
    if (fail_allocation == malloc_count) return NULL;
    /* The SDK's target pointers are 32-bit; reserve host padding so the same
     * structs remain addressable in this 64-bit test executable. */
    return calloc(1, (size_t)size + 16);
}

void __bt_free(void *p)
{
    free_count++;
    free(p);
}

void local_irq_disable(void) { irq_disables++; irq_depth++; }
void local_irq_enable(void) { irq_enables++; irq_depth--; }
void p33_soft_reset(void) { }
void cpu_assert_debug(void) { }
void btctrler_assert_failed(const char *what, int value)
{ (void)what; (void)value; }

static void *test_open(void) { return radio_open_fails ? NULL : (void *)0x1234; }
static void test_close(void *hw) { CHECK(hw == (void *)0x1234); close_count++; }
static void test_register(void *hw, void *priv, const struct ble_handler *handler)
{
    registered_hw = hw;
    registered_link = priv;
    registered_handler = handler;
    register_count++;
}
static int test_scanning(void *hw, struct ble_tws_scan *scan)
{
    CHECK(hw == (void *)0x1234);
    scanned_aa = scan->aa;
    scanning_count++;
    return -99; /* SDK ignores this result. */
}
void __set_ll_local_addr(void *hw, struct le_addr *addr, u8 type)
{
    CHECK(hw == (void *)0x1234 && type == 0);
    memset(addr, 0xa5, sizeof(*addr));
    local_addr_count++;
}
void tws_rx_probe_scan_pdu_handler(struct le_scan_link *link, struct ble_rx *rx)
{
    CHECK(link == (struct le_scan_link *)0x5678 || link == registered_link);
    CHECK(rx != NULL);
    pdu_count++;
    rx->nesn = 1;
}
void printf_buf(u8 *data, int len)
{
    buf_count++;
    buf_len = (u8)len;
    if (len > 0 && len <= (int)sizeof(buf_data)) memcpy(buf_data, data, (size_t)len);
}

static void reset_test(void)
{
    if (scan_hdl != NULL) {
        if (scan_hdl->scan_link != NULL) free(scan_hdl->scan_link);
        free(scan_hdl);
        scan_hdl = NULL;
    }
    memset(&test_ops, 0, sizeof(test_ops));
    test_ops.open = test_open;
    test_ops.close = test_close;
    test_ops.handler_register = test_register;
    test_ops.tws_scanning = test_scanning;
    __ble_ops = &test_ops;
    registered_link = NULL; registered_handler = NULL; registered_hw = NULL;
    irq_depth = irq_disables = irq_enables = 0;
    malloc_count = free_count = fail_allocation = 0;
    memset(malloc_sizes, 0, sizeof(malloc_sizes));
    radio_open_fails = 0;
    close_count = register_count = scanning_count = 0;
    local_addr_count = pdu_count = buf_count = 0;
    scanned_aa = 0; buf_len = 0; memset(buf_data, 0, sizeof(buf_data));
    log_tag_const_i_LL_TWS_SCAN = 0;
    log_tag_const_e_LL_TWS_SCAN = 0;
    config_asser = 0;
}

struct rx_fixture { u8 bytes[offsetof(struct ble_rx, data) + 8]; };

int main(void)
{
    u8 aa[4] = {0x78, 0x56, 0x34, 0x12};

    reset_test();
    CHECK(tws_ll_scan_link_get() == NULL);
    CHECK(tws_ll_scan_close() == 0);
    CHECK(irq_disables == 1 && irq_enables == 1 && irq_depth == 0);

    /* Open takes the 4-byte AA, passes the link handler, sets its local
     * address, and ignores the radio scanning callback's return value. */
    reset_test();
    CHECK(tws_ll_scan_open(aa) == 0);
    CHECK(malloc_count == 2 && register_count == 1 && scanning_count == 1);
    CHECK(malloc_sizes[0] == 8 && malloc_sizes[1] == 20);
    CHECK(registered_hw == (void *)0x1234 && registered_link == tws_ll_scan_link_get());
    CHECK(registered_handler != NULL && registered_handler->rx_probe_handler == NULL);
    CHECK(registered_handler->tws_rx_probe_handler != NULL);
    CHECK(registered_handler->tx_probe_handler == NULL &&
          registered_handler->event_probe_handler == NULL &&
          registered_handler->sync_param_handler == NULL &&
          registered_handler->echo_req_handler == NULL);
    CHECK(scanned_aa == 0x12345678 && tws_ll_scan_link_get()->local.addr_type == 0xa5);
    CHECK(local_addr_count == 1 && irq_disables == 1 && irq_enables == 1 && irq_depth == 0);
    CHECK(tws_ll_scan_open(aa) == 12 && malloc_count == 2);
    CHECK(tws_ll_scan_close() == 0 && close_count == 1 && free_count == 2);
    CHECK(tws_ll_scan_link_get() == NULL);

    /* With no link in an allocated handle, close returns 2 and leaves that
     * handle allocated, matching the SDK's early return. */
    reset_test();
    fail_allocation = 2;
    CHECK(tws_ll_scan_open(aa) == 12 && scan_hdl != NULL && scan_hdl->scan_link == NULL);
    CHECK(tws_ll_scan_close() == 2 && scan_hdl != NULL && free_count == 0);
    reset_test();

    /* A null radio handle returns 58 and leaves the allocated link for close. */
    reset_test();
    radio_open_fails = 1;
    CHECK(tws_ll_scan_open(aa) == 58 && scan_hdl->scan_link != NULL);
    CHECK(tws_ll_scan_close() == 0 && free_count == 2 && close_count == 0);

    /* RX forwarding runs for all types; after it returns the SDK clears len
     * and rewrites llid/invalid in the packed 16-bit header. */
    reset_test();
    struct rx_fixture fixture = {0};
    struct ble_rx *rx = (struct ble_rx *)(void *)fixture.bytes;
    rx->type = 4; rx->len = 3; rx->channel = 37;
    rx->rxadd = 1; rx->txadd = 1; rx->sn = 1; rx->md = 1;
    rx->data[0] = 0xde; rx->data[1] = 0xad; rx->data[2] = 0xbe;
    scan_handler.tws_rx_probe_handler((void *)0x5678, rx);
    CHECK(pdu_count == 1 && rx->len == 0);
    CHECK(rx->nesn == 1 && rx->llid == 1 && rx->invalid == 1);
    CHECK(rx->rxadd == 1 && rx->txadd == 1 && rx->sn == 1 && rx->md == 1);
    CHECK(buf_count == 0); /* logging is disabled */

    log_tag_const_i_LL_TWS_SCAN = 1;
    rx->type = 0; rx->len = 3;
    scan_handler.tws_rx_probe_handler((void *)0x5678, rx);
    CHECK(pdu_count == 2 && rx->len == 0);
    rx->type = 4; rx->len = 3;
    scan_handler.tws_rx_probe_handler((void *)0x5678, rx);
    CHECK(pdu_count == 3 && rx->len == 0 && buf_count == 1);
    CHECK(buf_len == 3 && buf_data[0] == 0xde && buf_data[2] == 0xbe);
    rx->type = 7; rx->len = 8;
    scan_handler.tws_rx_probe_handler((void *)0x5678, rx);
    CHECK(pdu_count == 4 && rx->len == 0 && buf_count == 1);
    return TEST_DONE("btctrler_tws_ll_scan");
}
#endif
