/* Shared plumbing for the BLE 5 extended roles (advertising, scanning,
 * initiating): the common link-layer handle, link allocation and close, the
 * promotion of an auxiliary connect response into a legacy connection, and
 * the combined destroy and receive-post hooks.  SDK quirks kept: a link is
 * counted in ext_number only when its allocation succeeds and the count is a
 * byte; the receive-post hook returns the low byte of the and of three int
 * flags, not a normalised boolean. */
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG u8 log_tag_const_e_LL_EXT_ADV, log_tag_const_i_LL_EXT_ADV;
extern BTCTRLER_CONFIG u64 config_btctler_le_features;
extern void log_print(int level, const char *tag, const char *format, ...);
extern struct ext_le_parameter ext_le_param;

#define LL_CODE __attribute__((section(".ble_ll_code")))

_Static_assert(offsetof(struct ext_link_layer_handle, ext_number) == 0, "SDK ext_number offset");
#ifndef UNITTEST
_Static_assert(sizeof(struct ext_link_layer_handle) == 32, "SDK ext_ll_hdl size");
_Static_assert(offsetof(struct ext_link_layer_handle, ext_adv_links) == 4, "SDK ext_adv_links offset");
_Static_assert(offsetof(struct ext_link_layer_handle, ext_periodic_adv_links) == 12, "SDK periodic links offset");
#endif

struct ext_link_layer_handle ext_ll_hdl __attribute__((section(".ble_ll_bss"), aligned(4)));

static void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

void __set_ll_ext_local_addr(void *hw, struct le_addr *addr, u8 own_addr_type) LL_CODE;
void __set_ll_ext_local_addr(void *hw, struct le_addr *addr, u8 own_addr_type)
{
    __set_ll_local_addr_common(addr, own_addr_type);
    __ble5_ops->ext_ioctrl(hw, 9, addr->addr_type, addr->addr);
}

void *__new_ext_link(u32 len) LL_CODE;
void *__new_ext_link(u32 len)
{
    void *link = __bt_malloc(len);
    if (!link) {
        if (log_tag_const_e_LL_EXT_ADV)
            log_print(4, NULL, "<Error>: [LL_EXT_ADV]ext link alloc err\r\n");
        return NULL;
    }
    ext_ll_hdl.ext_number = (u8)(ext_ll_hdl.ext_number + 1);
    return link;
}

void __ll_ext_link_close(void *link, void *hw) LL_CODE;
void __ll_ext_link_close(void *link, void *hw)
{
    if (hw) __ble5_ops->ext_close(hw);
    __bt_free(link);
    ext_ll_hdl.ext_number = (u8)(ext_ll_hdl.ext_number - 1);
}

void ll_ext_create_legacy_connection(struct le_link *legacy_link) LL_CODE;
void ll_ext_create_legacy_connection(struct le_link *legacy_link)
{
    if (log_tag_const_i_LL_EXT_ADV)
        log_print(2, NULL, "[Info]: [LL_EXT_ADV]ext LL_CONNECTION_CREATE\r\n");
    ll_create_conn_link_post(legacy_link);
    ll_supervision_timeout_start(legacy_link, legacy_link->ll_data.timeout * 10);
    if (legacy_link->role == 1)
        le_connection_complete_event_emit(legacy_link);
    else
        le_enhanced_connection_complete_event(legacy_link, 0);
    le_channel_selection_algorithm_event(legacy_link, 1);
}

void ll_ext_destroy(void) LL_CODE;
void ll_ext_destroy(void)
{
    if ((config_btctler_le_features & 4096) == 0) return;
    int roles = config_btctler_le_roles;
    if (roles & 2) le_ext_adv_destroy();
    if (roles & 1) {
        destroy_le_ext_scan();
        destroy_le_ext_init();
    }
}

/* The SDK returns the low byte of the and of the three flags; sdk.h calls
 * this a bool, but the IR (and the link layer's caller) use an unsigned byte. */
u8 ll_ext_rx_post_process_ir(void) __asm__("ll_ext_rx_post_process") LL_CODE;
u8 ll_ext_rx_post_process_ir(void)
{
    int adv_rx_empty, scan_rx_empty, init_rx_empty;
    adv_rx_empty = 1;
    ll_ext_adv_rx_post_process(&adv_rx_empty);
    scan_rx_empty = 1;
    ll_ext_scan_rx_post_process(&scan_rx_empty);
    init_rx_empty = 1;
    ll_ext_init_rx_post_process(&init_rx_empty);
    return (u8)((scan_rx_empty & adv_rx_empty) & init_rx_empty);
}

void ll_ext_handle_malloc(void) LL_CODE;
void ll_ext_handle_malloc(void)
{
    if ((config_btctler_le_features & 4096) == 0) return;
    INIT_LIST_HEAD(&ext_le_param.ext_adv_list.head);
    INIT_LIST_HEAD(&ext_ll_hdl.ext_adv_links);
    INIT_LIST_HEAD(&ext_ll_hdl.ext_periodic_adv_links);
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#include <stdlib.h>

struct ext_le_parameter ext_le_param;
const struct ble5_operation *__ble5_ops;
u8 log_tag_const_e_LL_EXT_ADV = 1, log_tag_const_i_LL_EXT_ADV = 0;
u64 config_btctler_le_features = 4096;
int config_btctler_le_roles = 3;

static char trace[256];
static void T(const char *s) { strcat(trace, s); strcat(trace, " "); }
static int errors, malloc_fail, adv_value = 1, scan_value = 1, init_value = 1;
static void *freed;
static void *closed_hw;
static void *ioctrl_addr;
static int ioctrl_cmd, ioctrl_type;
static u32 timeout_arg;
static u8 common_type;

void log_print(int level, const char *tag, const char *format, ...) { (void)tag; (void)format; if (level == 4) errors++; }
void *__bt_malloc(int size) { return malloc_fail ? NULL : calloc(1, (size_t)size); }
void __bt_free(void *p) { freed = p; }
void __set_ll_local_addr_common(struct le_addr *addr, u8 t) { (void)addr; common_type = t; T("common"); }
void ll_create_conn_link_post(struct le_link *l) { (void)l; T("post"); }
void ll_supervision_timeout_start(struct le_link *l, u32 t) { (void)l; timeout_arg = t; T("svto"); }
void le_connection_complete_event_emit(struct le_link *l) { (void)l; T("complete"); }
void le_enhanced_connection_complete_event(struct le_link *l, u8 s) { (void)l; CHECK(s == 0); T("enhanced"); }
void le_channel_selection_algorithm_event(struct le_link *l, u8 a) { (void)l; CHECK(a == 1); T("csa"); }
void le_ext_adv_destroy(void) { T("advdestroy"); }
void destroy_le_ext_scan(void) { T("scandestroy"); }
void destroy_le_ext_init(void) { T("initdestroy"); }
void ll_ext_adv_rx_post_process(int *e) { T("advpost"); *e = adv_value; }
void ll_ext_scan_rx_post_process(int *e) { T("scanpost"); *e = scan_value; }
void ll_ext_init_rx_post_process(int *e) { T("initpost"); *e = init_value; }
static void m_ext_close(void *hw) { closed_hw = hw; T("eclose"); }
static void m_ext_ioctrl(void *hw, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    CHECK(hw == (void *)0x77);
    ioctrl_cmd = cmd;
    ioctrl_type = va_arg(ap, int);
    ioctrl_addr = va_arg(ap, void *);
    va_end(ap);
}

int main(void)
{
    struct ble5_operation ops = { .ext_close = m_ext_close, .ext_ioctrl = m_ext_ioctrl };
    __ble5_ops = &ops;

    /* Link allocation counts only successes; close counts down. */
    void *a = __new_ext_link(40);
    CHECK(a && ext_ll_hdl.ext_number == 1);
    malloc_fail = 1;
    CHECK(__new_ext_link(40) == NULL && errors == 1 && ext_ll_hdl.ext_number == 1);
    malloc_fail = 0;
    void *b = __new_ext_link(8);
    CHECK(b && ext_ll_hdl.ext_number == 2);
    trace[0] = 0;
    __ll_ext_link_close(a, (void *)0x55);
    CHECK(closed_hw == (void *)0x55 && freed == a && ext_ll_hdl.ext_number == 1 && !strcmp(trace, "eclose "));
    trace[0] = 0;
    __ll_ext_link_close(b, NULL);
    CHECK(freed == b && ext_ll_hdl.ext_number == 0 && !trace[0]);
    __ll_ext_link_close(b, NULL);                /* the count is a byte and wraps */
    CHECK(ext_ll_hdl.ext_number == 255);
    ext_ll_hdl.ext_number = 255;
    __new_ext_link(8);
    CHECK(ext_ll_hdl.ext_number == 0);

    /* Local address: common setup first, then the radio command 9 (type, address). */
    struct le_addr addr = { 1, { 1, 2, 3, 4, 5, 6 } };
    trace[0] = 0;
    __set_ll_ext_local_addr((void *)0x77, &addr, 2);
    CHECK(common_type == 2 && ioctrl_cmd == 9 && ioctrl_type == 1 && ioctrl_addr == addr.addr);

    /* Connection create: master role (1) emits the plain event, others the enhanced one. */
    struct le_link link;
    memset(&link, 0, sizeof(link));
    link.ll_data.timeout = 300;
    link.role = 1;
    trace[0] = 0;
    ll_ext_create_legacy_connection(&link);
    CHECK(timeout_arg == 3000 && !strcmp(trace, "post svto complete csa "));
    link.role = 0;
    trace[0] = 0;
    ll_ext_create_legacy_connection(&link);
    CHECK(!strcmp(trace, "post svto enhanced csa "));
    link.ll_data.timeout = 0xffff;
    ll_ext_create_legacy_connection(&link);
    CHECK(timeout_arg == 655350);

    /* Destroy: feature bit 12 gates both; roles bit 1 = advertiser, bit 0 = scanner + initiator. */
    trace[0] = 0;
    ll_ext_destroy();
    CHECK(!strcmp(trace, "advdestroy scandestroy initdestroy "));
    config_btctler_le_roles = 2; trace[0] = 0;
    ll_ext_destroy();
    CHECK(!strcmp(trace, "advdestroy "));
    config_btctler_le_roles = 1; trace[0] = 0;
    ll_ext_destroy();
    CHECK(!strcmp(trace, "scandestroy initdestroy "));
    config_btctler_le_features = 0xefff; trace[0] = 0;
    ll_ext_destroy();
    CHECK(!trace[0]);
    config_btctler_le_features = 4096;

    /* Receive post: and of the three flags, truncated to a byte. */
    trace[0] = 0;
    CHECK(ll_ext_rx_post_process_ir() == 1 && !strcmp(trace, "advpost scanpost initpost "));
    scan_value = 0;
    CHECK(ll_ext_rx_post_process_ir() == 0);
    scan_value = 3; adv_value = 0x102; init_value = 0xff;
    CHECK(ll_ext_rx_post_process_ir() == 2);
    scan_value = 0x100; adv_value = 0x100; init_value = 0x100;
    CHECK(ll_ext_rx_post_process_ir() == 0);

    /* Handle init: empty rings only with the feature bit. */
    config_btctler_le_features = 0;
    ext_ll_hdl.ext_adv_links.next = NULL;
    ll_ext_handle_malloc();
    CHECK(ext_ll_hdl.ext_adv_links.next == NULL);
    config_btctler_le_features = 4096;
    ll_ext_handle_malloc();
    CHECK(ext_le_param.ext_adv_list.head.next == &ext_le_param.ext_adv_list.head && ext_le_param.ext_adv_list.head.prev == &ext_le_param.ext_adv_list.head);
    CHECK(ext_ll_hdl.ext_adv_links.next == &ext_ll_hdl.ext_adv_links && ext_ll_hdl.ext_adv_links.prev == &ext_ll_hdl.ext_adv_links);
    CHECK(ext_ll_hdl.ext_periodic_adv_links.next == &ext_ll_hdl.ext_periodic_adv_links && ext_ll_hdl.ext_periodic_adv_links.prev == &ext_ll_hdl.ext_periodic_adv_links);
    return TEST_DONE("btctrler_ll_extended_advertising");
}
#endif
