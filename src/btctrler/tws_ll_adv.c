/* The SDK's TWS advertiser (tws_ll_adv.c): the parameter, scan response,
 * open and close entry points that retained tws_le_link names, with the
 * advertiser's state and its SCAN_REQ handler.  The member's debug dump and
 * link getter have no callers and go with it.  SDK quirks kept: the
 * advertiser state (44 bytes) is allocated lazily by whichever entry point
 * runs first; the scan response copy trusts the caller's length byte (up to
 * 255 bytes into a 31-byte field); with an open link the scan response is
 * pushed to the radio from the caller's buffer rather than the stored copy;
 * close returns 0 when the state is already gone and 2 when only the link
 * is missing; open returns 12 when already open or out of memory and 58
 * when the radio has no hardware. */
#include <stddef.h>
#include <string.h>
#include "btctrler/glue.h"

#define LL_CODE __attribute__((section(".ble_ll_code")))

extern BTCTRLER_CONFIG u8 log_tag_const_i_LL_TWS_ADV, log_tag_const_e_LL_TWS_ADV;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(const void *buffer, int size);

static struct ll_adv_hdl_tws_ll_adv *adv_hdl __attribute__((section(".ble_ll_data")));

static void adv_rx_probe_handler(void *priv, struct ble_rx *rx) LL_CODE;
static const struct ble_handler adv_handler __attribute__((section(".ble_ll_const"))) = {
    .tws_rx_probe_handler = adv_rx_probe_handler,
};

static void LL_CODE info(const char *s) { if (log_tag_const_i_LL_TWS_ADV) log_print(2, NULL, s); }
static void LL_CODE error(const char *s) { if (log_tag_const_e_LL_TWS_ADV) log_print(4, NULL, s); }
static void LL_CODE info_arg(const char *s, unsigned v) { if (log_tag_const_i_LL_TWS_ADV) log_print(2, NULL, s, v); }

static void ll_adv_init(void) LL_CODE;
static void ll_adv_init(void)
{
    info("[Info]: [LL_TWS_ADV]LL ADV - init\r\n");
    adv_hdl = __bt_malloc(44);
}

int tws_ll_adv_set_params(const u8 *data) LL_CODE;
int tws_ll_adv_set_params(const u8 *data)
{
    if (!adv_hdl)
        ll_adv_init();
    memcpy(&adv_hdl->adv_param, data, 5);
    return 0;
}

/* data[0] is the length, data[1..] the scan response. */
int tws_ll_adv_set_scan_rsp_data(const u8 *data) LL_CODE;
int tws_ll_adv_set_scan_rsp_data(const u8 *data)
{
    struct le_adv_link *link;

    if (!adv_hdl)
        ll_adv_init();
    link = adv_hdl->adv_link;
    memset(&adv_hdl->scan_resp_data, 0, 32);
    adv_hdl->scan_resp_data.length = data[0];
    memcpy(adv_hdl->scan_resp_data.data, data + 1, data[0]);
    if (log_tag_const_i_LL_TWS_ADV) {
        log_print(2, NULL, "[Info]: [LL_TWS_ADV]SCAN RSP data : %d\r\n", (int)data[0]);
        printf_buf(adv_hdl->scan_resp_data.data, adv_hdl->scan_resp_data.length);
    }
    if (link && link->hw)
        __ble_ops->ioctrl(link->hw, 22, data);
    return 0;
}

int tws_ll_adv_open(void) LL_CODE;
int tws_ll_adv_open(void)
{
    struct le_adv_link *link;
    struct ble_tws_adv adv;
    void *hw;
    int i;

    if (!adv_hdl)
        ll_adv_init();
    info("[Info]: [LL_TWS_ADV]ll_adv_open\r\n");
    local_irq_disable();
    if (adv_hdl->adv_link) {
        error("<Error>: [LL_TWS_ADV]adv link already open\r\n");
        local_irq_enable();
        return 12;
    }
    link = __bt_malloc(24);
    if (!link) {
        error("<Error>: [LL_TWS_ADV]adv link NULL\r\n");
        local_irq_enable();
        return 12;
    }
    adv_hdl->adv_link = link;
    local_irq_enable();
    hw = __ble_ops->open();
    link->hw = hw;
    if (!hw) {
        error("<Error>: [LL_TWS_ADV]adv hw NULL\r\n");
        return 58;
    }
    __ble_ops->handler_register(hw, link, &adv_handler);

    /* The SDK's __set_ll_adv_state, inlined: the channel map decides how
     * many of the three advertising channels are used. */
    info("[Info]: [LL_TWS_ADV]>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\r\n");
    adv.channel_map = adv_hdl->adv_param.Advertising_Channel_Map;
    adv.pkt_cnt = 0;
    for (i = 0; i < 3; i++) {
        if ((1 << i) & adv.channel_map)
            adv.pkt_cnt++;
    }
    adv.aa = adv_hdl->adv_param.AA;
    info_arg("[Info]: [LL_TWS_ADV]adv AA : 0x%x\r\n", adv.aa);
    info_arg("[Info]: [LL_TWS_ADV]adv channel map: 0x%x\r\n", adv.channel_map);
    __set_ll_local_addr(link->hw, &link->local, 0);
    info("[Info]: [LL_TWS_ADV]>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\r\n");
    __ble_ops->ioctrl(link->hw, 22, &adv_hdl->scan_resp_data);
    __ble_ops->tws_advertising(link->hw, &adv);
    return 0;
}

int tws_ll_adv_close(void) LL_CODE;
int tws_ll_adv_close(void)
{
    struct le_adv_link *link;

    info("[Info]: [LL_TWS_ADV]ll_adv_close\r\n");
    local_irq_disable();
    if (!adv_hdl) {
        error("<Error>: [LL_TWS_ADV]LL Adv already destroy\r\n");
        local_irq_enable();
        return 0;
    }
    link = adv_hdl->adv_link;
    if (!link) {
        error("<Error>: [LL_TWS_ADV]adv link NULL\r\n");
        local_irq_enable();
        return 2;
    }
    adv_hdl->adv_link = NULL;
    local_irq_enable();
    if (link->hw)
        __ble_ops->close(link->hw);
    __bt_free(link);

    /* The SDK's ll_adv_destroy, inlined. */
    info("[Info]: [LL_TWS_ADV]LL ADV - destroy\r\n");
    if (adv_hdl) {
        __bt_free(adv_hdl);
        adv_hdl = NULL;
    }
    return 0;
}

/* A scan request (type 3) is logged; every received PDU goes to the TWS
 * probe handler, then is marked consumed: length 0, llid 1, invalid set. */
static void adv_rx_probe_handler(void *priv, struct ble_rx *rx)
{
    if (rx->type == 3 && log_tag_const_i_LL_TWS_ADV)
        log_print(2, NULL, "[Info]: [LL_TWS_ADV]SCAN_REQ\r\n");
    tws_rx_probe_adv_pdu_handler(priv, rx);
    rx->len = 0;
    rx->llid = 1;
    rx->invalid = 1;
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#include <stdlib.h>

u8 log_tag_const_i_LL_TWS_ADV, log_tag_const_e_LL_TWS_ADV;
static struct ble_operation ops;
const struct ble_operation *__ble_ops = &ops;

static char trace[256];
static char logs[16][96];
static int log_count, printf_buf_len, malloc_fail, mallocs, frees, malloc_size[8], tws_calls, probe_calls;
static void *freed_ptr[4], *alloc_ptr[4], *open_result, *closed_hw, *addr_hw, *ioctrl_hw[4], *ioctrl_arg[4];
static int ioctrl_cmd[4], ioctrl_count;
static void *tws_hw, *handler_hw, *handler_priv;
static const struct ble_handler *handler_table;
static struct ble_tws_adv tws_seen;
static void *probe_priv; static struct ble_rx *probe_rx;
static u8 printf_buf_data[32];

static void tr(char c) { size_t n = strlen(trace); trace[n] = c; trace[n + 1] = 0; }
void log_print(int level, const char *tag, const char *format, ...)
{
    int idx = log_count < 16 ? log_count : 15, n;
    va_list ap; va_start(ap, format); (void)tag;
    n = snprintf(logs[idx], 96, "%d:", level);
    vsnprintf(logs[idx] + n, 96 - n, format, ap);
    log_count++; va_end(ap); tr('L');
}
void printf_buf(const void *p, int n) { printf_buf_len = n; memcpy(printf_buf_data, p, n > 31 ? 31 : n); tr('P'); }
void local_irq_disable(void) { tr('d'); }
void local_irq_enable(void) { tr('e'); }
void *__bt_malloc(int size)
{
    void *p;
    malloc_size[mallocs < 8 ? mallocs : 7] = size; mallocs++;
    if (malloc_fail) return NULL;
    p = calloc(1, 64); alloc_ptr[mallocs < 5 ? mallocs - 1 : 3] = p; return p;
}
void __bt_free(void *p) { freed_ptr[frees < 4 ? frees : 3] = p; frees++; tr('f'); }
static void *ops_open(void) { tr('o'); return open_result; }
static void ops_close(void *hw) { closed_hw = hw; tr('c'); }
static void ops_handler_register(void *hw, void *priv, const struct ble_handler *h) { handler_hw = hw; handler_priv = priv; handler_table = h; tr('h'); }
static void ops_ioctrl(void *hw, int cmd, ...)
{
    va_list ap; va_start(ap, cmd);
    if (ioctrl_count < 4) { ioctrl_hw[ioctrl_count] = hw; ioctrl_cmd[ioctrl_count] = cmd; ioctrl_arg[ioctrl_count] = va_arg(ap, void *); }
    ioctrl_count++; va_end(ap); tr('i');
}
static int ops_tws_advertising(void *hw, struct ble_tws_adv *a) { tws_hw = hw; tws_seen = *a; tws_calls++; tr('t'); return 0; }
void __set_ll_local_addr(void *hw, struct le_addr *addr, u8 type) { addr_hw = hw; CHECK(type == 0); (void)addr; tr('a'); }
void tws_rx_probe_adv_pdu_handler(struct le_adv_link *link, struct ble_rx *rx) { probe_priv = link; probe_rx = rx; probe_calls++; tr('r'); }

static void reset(void)
{
    if (adv_hdl) { free(adv_hdl); adv_hdl = NULL; }
    memset(trace, 0, sizeof(trace)); memset(logs, 0, sizeof(logs)); log_count = 0; malloc_fail = 0; mallocs = 0; frees = 0;
    memset(malloc_size, 0, sizeof(malloc_size)); tws_calls = 0; probe_calls = 0; ioctrl_count = 0; handler_table = NULL;
    memset(alloc_ptr, 0, sizeof(alloc_ptr)); memset(freed_ptr, 0, sizeof(freed_ptr));
    open_result = (void *)0x1234; closed_hw = addr_hw = tws_hw = NULL;
    log_tag_const_i_LL_TWS_ADV = log_tag_const_e_LL_TWS_ADV = 0;
    ops.open = ops_open; ops.close = ops_close; ops.handler_register = ops_handler_register;
    ops.ioctrl = ops_ioctrl; ops.tws_advertising = ops_tws_advertising;
}

int main(void)
{
    u8 params[5] = { 0x78, 0x56, 0x34, 0x12, 0x05 };
    u8 rsp[32] = { 3, 0xA1, 0xA2, 0xA3 };

    /* The handler table has only the TWS probe handler. */
    CHECK(adv_handler.tws_rx_probe_handler == adv_rx_probe_handler && !adv_handler.rx_probe_handler && !adv_handler.tx_probe_handler);
    CHECK(!adv_handler.event_probe_handler && !adv_handler.sync_param_handler && !adv_handler.echo_req_handler);

    /* set_params allocates the 44-byte state once, copies 5 bytes, returns 0. */
    reset();
    CHECK(tws_ll_adv_set_params(params) == 0 && mallocs == 1 && malloc_size[0] == 44 && adv_hdl == alloc_ptr[0]);
    CHECK(adv_hdl->adv_param.AA == 0x12345678u && adv_hdl->adv_param.Advertising_Channel_Map == 5);
    CHECK(tws_ll_adv_set_params(params) == 0 && mallocs == 1);
    CHECK(log_count == 0);
    reset(); log_tag_const_i_LL_TWS_ADV = 1;
    tws_ll_adv_set_params(params);
    CHECK(log_count == 1 && !strcmp(logs[0], "2:[Info]: [LL_TWS_ADV]LL ADV - init\r\n"));

    /* scan response: cleared, length and bytes stored, no link: nothing else. */
    reset(); memset(rsp + 4, 0x55, 28);
    CHECK(tws_ll_adv_set_scan_rsp_data(rsp) == 0 && mallocs == 1 && ioctrl_count == 0);
    CHECK(adv_hdl->scan_resp_data.length == 3 && adv_hdl->scan_resp_data.data[0] == 0xA1 && adv_hdl->scan_resp_data.data[2] == 0xA3 && adv_hdl->scan_resp_data.data[3] == 0);
    /* a shorter one clears what the previous left (a 31-byte response fits) */
    {
        u8 full[32]; memset(full, 0x77, sizeof(full)); full[0] = 31;
        tws_ll_adv_set_scan_rsp_data(full);
        CHECK(adv_hdl->scan_resp_data.length == 31 && adv_hdl->scan_resp_data.data[30] == 0x77);
        tws_ll_adv_set_scan_rsp_data(rsp);
        CHECK(adv_hdl->scan_resp_data.data[3] == 0);
        CHECK(adv_hdl->scan_resp_data.data[30] == 0);
    }
    /* logging with the info tag: the count, then the buffer */
    reset(); log_tag_const_i_LL_TWS_ADV = 1;
    tws_ll_adv_set_scan_rsp_data(rsp);
    CHECK(log_count == 2 && !strcmp(logs[1], "2:[Info]: [LL_TWS_ADV]SCAN RSP data : 3\r\n") && printf_buf_len == 3 && !strcmp(trace, "LLP"));
    /* with an open link that has hardware: ioctrl 22 with the CALLER's buffer */
    reset(); tws_ll_adv_set_params(params);
    adv_hdl->adv_link = calloc(1, 64); adv_hdl->adv_link->hw = (void *)0x77;
    tws_ll_adv_set_scan_rsp_data(rsp);
    CHECK(ioctrl_count == 1 && ioctrl_hw[0] == (void *)0x77 && ioctrl_cmd[0] == 22 && ioctrl_arg[0] == rsp);
    adv_hdl->adv_link->hw = NULL; ioctrl_count = 0;
    tws_ll_adv_set_scan_rsp_data(rsp);
    CHECK(ioctrl_count == 0);
    free(adv_hdl->adv_link); adv_hdl->adv_link = NULL;

    /* open: already open -> 12 with error log and irq bracket. */
    reset(); log_tag_const_e_LL_TWS_ADV = 1; tws_ll_adv_set_params(params); mallocs = 0;
    adv_hdl->adv_link = (void *)&params;
    CHECK(tws_ll_adv_open() == 12 && !strcmp(trace, "dLe") && !strcmp(logs[0], "4:<Error>: [LL_TWS_ADV]adv link already open\r\n"));
    adv_hdl->adv_link = NULL;
    /* out of memory -> 12 (the link allocation is 24 bytes) */
    reset(); log_tag_const_e_LL_TWS_ADV = 1; tws_ll_adv_set_params(params); mallocs = 0; malloc_fail = 1;
    CHECK(tws_ll_adv_open() == 12 && malloc_size[0] == 24 && !strcmp(trace, "dLe") && !strcmp(logs[0], "4:<Error>: [LL_TWS_ADV]adv link NULL\r\n") && adv_hdl->adv_link == NULL);
    /* the radio gives no hardware -> 58, the link stays allocated and recorded */
    reset(); log_tag_const_e_LL_TWS_ADV = 1; tws_ll_adv_set_params(params); mallocs = 0; open_result = NULL;
    CHECK(tws_ll_adv_open() == 58 && adv_hdl->adv_link != NULL && adv_hdl->adv_link->hw == NULL && !strcmp(trace, "deoL") && !strcmp(logs[0], "4:<Error>: [LL_TWS_ADV]adv hw NULL\r\n"));
    free(adv_hdl->adv_link);
    /* open: success path; channel map 5 uses two channels. */
    reset(); tws_ll_adv_set_params(params); mallocs = 0;
    CHECK(tws_ll_adv_open() == 0);
    CHECK(!strcmp(trace, "deohait"));
    CHECK(adv_hdl->adv_link->hw == (void *)0x1234 && handler_hw == (void *)0x1234 && handler_priv == adv_hdl->adv_link && handler_table == &adv_handler);
    CHECK(tws_seen.aa == 0x12345678u && tws_seen.channel_map == 5 && tws_seen.pkt_cnt == 2 && tws_hw == (void *)0x1234 && addr_hw == (void *)0x1234);
    CHECK(ioctrl_count == 1 && ioctrl_cmd[0] == 22 && ioctrl_arg[0] == &adv_hdl->scan_resp_data && ioctrl_hw[0] == (void *)0x1234);
    free(adv_hdl->adv_link); adv_hdl->adv_link = NULL;
    /* channel maps: 0 -> 0, 1 -> 1, 7 -> 3, 0xf8 -> 0 (only the low three bits count) */
    {
        static const u8 maps[4] = { 0, 1, 7, 0xf8 }, counts[4] = { 0, 1, 3, 0 };
        for (int m = 0; m < 4; m++) {
            u8 p[5] = { 1, 2, 3, 4, maps[m] };
            reset(); tws_ll_adv_set_params(p); mallocs = 0;
            tws_ll_adv_open();
            CHECK(tws_seen.pkt_cnt == counts[m] && tws_seen.channel_map == maps[m]);
            free(adv_hdl->adv_link); adv_hdl->adv_link = NULL;
        }
    }
    /* open with state not yet created allocates it first (44) then the link (24) */
    reset();
    CHECK(tws_ll_adv_open() == 0 && malloc_size[0] == 44 && malloc_size[1] == 24 && mallocs == 2);
    free(adv_hdl->adv_link); adv_hdl->adv_link = NULL;
    /* info logging order in open */
    reset(); log_tag_const_i_LL_TWS_ADV = 1; tws_ll_adv_set_params(params); log_count = 0; memset(logs, 0, sizeof(logs)); mallocs = 0;
    tws_ll_adv_open();
    CHECK(log_count == 5);
    CHECK(!strcmp(logs[0], "2:[Info]: [LL_TWS_ADV]ll_adv_open\r\n"));
    CHECK(!strcmp(logs[1], "2:[Info]: [LL_TWS_ADV]>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\r\n"));
    CHECK(!strcmp(logs[2], "2:[Info]: [LL_TWS_ADV]adv AA : 0x12345678\r\n"));
    CHECK(!strcmp(logs[3], "2:[Info]: [LL_TWS_ADV]adv channel map: 0x5\r\n"));
    CHECK(!strcmp(logs[4], "2:[Info]: [LL_TWS_ADV]>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\r\n"));
    free(adv_hdl->adv_link); adv_hdl->adv_link = NULL;

    /* close: no state -> 0 (init not forced); link missing -> 2; normal -> state and link freed. */
    reset(); log_tag_const_e_LL_TWS_ADV = 1;
    CHECK(tws_ll_adv_close() == 0 && !strcmp(trace, "dLe") && mallocs == 0 && !strcmp(logs[0], "4:<Error>: [LL_TWS_ADV]LL Adv already destroy\r\n"));
    reset(); log_tag_const_e_LL_TWS_ADV = 1; tws_ll_adv_set_params(params);
    CHECK(tws_ll_adv_close() == 2 && !strcmp(trace, "dLe") && adv_hdl != NULL && frees == 0 && !strcmp(logs[0], "4:<Error>: [LL_TWS_ADV]adv link NULL\r\n"));
    reset(); tws_ll_adv_set_params(params); mallocs = 0;
    tws_ll_adv_open(); memset(trace, 0, sizeof(trace));
    {
        void *l = adv_hdl->adv_link, *state = adv_hdl;
        CHECK(tws_ll_adv_close() == 0 && adv_hdl == NULL);
        CHECK(!strcmp(trace, "decff") && closed_hw == (void *)0x1234 && freed_ptr[0] == l && freed_ptr[1] == state);
        free(l); state = NULL;
    }
    /* link without hardware: not closed in the radio. */
    reset(); tws_ll_adv_set_params(params); mallocs = 0; open_result = NULL; tws_ll_adv_open(); memset(trace, 0, sizeof(trace)); closed_hw = (void *)1;
    {
        void *l = adv_hdl->adv_link;
        CHECK(tws_ll_adv_close() == 0 && !strcmp(trace, "deff") && closed_hw == (void *)1);
        free(l);
    }
    /* a second close finds nothing */
    CHECK(tws_ll_adv_close() == 0);
    /* close logs */
    reset(); log_tag_const_i_LL_TWS_ADV = 1; tws_ll_adv_set_params(params); mallocs = 0; tws_ll_adv_open(); log_count = 0; memset(logs, 0, sizeof(logs));
    {
        void *l = adv_hdl->adv_link;
        tws_ll_adv_close();
        CHECK(log_count == 2 && !strcmp(logs[0], "2:[Info]: [LL_TWS_ADV]ll_adv_close\r\n") && !strcmp(logs[1], "2:[Info]: [LL_TWS_ADV]LL ADV - destroy\r\n"));
        free(l);
    }

    /* probe handler: scan request logged only with the info tag; always forwards and marks the PDU. */
    {
        struct ble_rx rx;
        memset(&rx, 0, sizeof(rx)); rx.type = 3; rx.len = 9; rx.llid = 3; rx.nesn = 1; rx.invalid = 0;
        reset(); adv_rx_probe_handler((void *)0x55, &rx);
        CHECK(log_count == 0 && probe_calls == 1 && probe_priv == (void *)0x55 && probe_rx == &rx);
        CHECK(rx.len == 0 && rx.llid == 1 && rx.invalid == 1 && rx.type == 3 && rx.nesn == 1);
        memset(&rx, 0, sizeof(rx)); rx.type = 3; rx.len = 9;
        reset(); log_tag_const_i_LL_TWS_ADV = 1; adv_rx_probe_handler(NULL, &rx);
        CHECK(log_count == 1 && !strcmp(logs[0], "2:[Info]: [LL_TWS_ADV]SCAN_REQ\r\n") && !strcmp(trace, "Lr"));
        memset(&rx, 0, sizeof(rx)); rx.type = 4;
        reset(); log_tag_const_i_LL_TWS_ADV = 1; adv_rx_probe_handler(NULL, &rx);
        CHECK(log_count == 0 && probe_calls == 1);
    }
    return TEST_DONE("btctrler_tws_ll_adv");
}
#endif
