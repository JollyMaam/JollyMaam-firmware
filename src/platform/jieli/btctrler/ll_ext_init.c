/* BLE 5 extended initiating: open the primary (legacy) and secondary
 * (extended) radio links for an LE Extended Create Connection, hand the
 * connection request to the legacy link layer when the auxiliary connect
 * response arrives, and tear it all down.  SDK quirks kept: the connection
 * window offset reads a free-running hardware counter and takes it modulo
 * half the interval (zero when the interval is 0 or 1); the parameter copy
 * takes all 32 bytes of the sample connection parameters including the
 * afh pointer; a failed primary open leaves the secondary link registered. */
#include "btctrler/glue.h"
#include <string.h>

extern BTCTRLER_CONFIG u8 log_tag_const_e_LL_EXT_INIT, log_tag_const_i_LL_EXT_INIT;
extern void log_print(int level, const char *tag, const char *format, ...);
extern void printf_buf(const void *buffer, int size);
extern struct ext_le_parameter ext_le_param;

#define LL_CODE __attribute__((section(".ble_ll_code")))

#ifdef UNITTEST
extern volatile u32 ll_ext_init_random_counter;
#define RANDOM_COUNTER ll_ext_init_random_counter
#else
#define RANDOM_COUNTER (*(volatile u32 *)0x1e3b00)
#endif

#ifndef UNITTEST
_Static_assert(sizeof(struct ble_conn_param) == 32, "SDK connection parameter size");
_Static_assert(sizeof(struct ble_ext_init) == 48, "SDK extended init size");
_Static_assert(offsetof(struct le_ext_init_link, extended_init) == 12, "SDK init link offsets");
_Static_assert(offsetof(struct le_ext_init_link, state) == 60, "SDK init link state");
_Static_assert(offsetof(struct ble_ext_init, ext_init) == 8, "SDK ext_init pointer");
_Static_assert(offsetof(struct ble_ext_init, conn_param) == 12, "SDK conn_param offset");
_Static_assert(offsetof(struct ble_conn_param, widening) == 22, "SDK widening offset");
#endif
_Static_assert(offsetof(struct __init_phy_param, Conn_Interval_Max) == 6, "SDK phy param interval");

static void ll_ext_init_rx_probe_handler(void *priv, struct ble_ext_rx *rx) LL_CODE;
static void ll_ext_init_rx_probe_handler(void *priv, struct ble_ext_rx *rx)
{
    struct le_ext_init_link *link = priv;
    if ((config_btctler_le_roles & 1) == 0) return;
    if (rx->type == 8) {
        __ble5_ops->legacy_open(link->hw);
        __ble5_ops->ext_ioctrl(link->hw, 16);
        struct __ext_init *init = link->extended_init.ext_init;
        __ble_ops->ioctrl(link->hw, 9, init->Peer_Address_Type, init->Peer_Address);
        link->connect_link = ll_create_conn_link(link->hw, &link->extended_init.local, 0, rx->txadd,
                                                 &rx->data[2], &link->extended_init.conn_param);
    }
    if (rx->len != 0) ll_thread_resume();
}

static void ll_ext_init_event_probe_handler(void *priv, u32 param) LL_CODE;
static void ll_ext_init_event_probe_handler(void *priv, u32 param)
{
    if (param == 12) {
        ((struct le_ext_init_link *)priv)->state = 17;
        ll_thread_resume();
    }
}

static void ll_ext_init_rx_post(void *priv, struct ble_ext_rx *rx) LL_CODE;
static void ll_ext_init_rx_post(void *priv, struct ble_ext_rx *rx)
{
    struct le_ext_init_link *link = priv;
    if (log_tag_const_i_LL_EXT_INIT)
        log_print(2, NULL, "[Info]: [LL_EXT_INIT]rx post rx->type=0x%x\r\n", (int)rx->type);
    if (rx->type == 8) {
        if (log_tag_const_i_LL_EXT_INIT)
            log_print(2, NULL, "[Info]: [LL_EXT_INIT]Init post rx AUX_CONNECT_RSP\r\n");
        ll_ext_create_legacy_connection(link->connect_link);
        link->state = 18;
    }
}

const struct ble_ext_handler ll_ext_init_handler __attribute__((section(".ble_ll_const"), aligned(4))) = {
    ll_ext_init_rx_probe_handler,
    ll_ext_init_event_probe_handler,
};

static void ll_ext_init_close(void) LL_CODE;
static void ll_ext_init_close(void)
{
    struct le_ext_init_link *link = ext_ll_hdl.ext_init_link;
    if (!link) return;
    __ble5_ops->legacy_close(link->primary_legacy_hw);
    link = ext_ll_hdl.ext_init_link;
    if (link->connect_link) {
        ll_close_conn_link(link->connect_link);
        link = ext_ll_hdl.ext_init_link;
    }
    __ll_ext_link_close(link, link->hw);
    ext_ll_hdl.ext_init_link = NULL;
}

void destroy_le_ext_init(void) LL_CODE;
void destroy_le_ext_init(void)
{
    if (ext_le_param.ext_init) {
        __bt_free(&ext_le_param.ext_init->Initiating_Filter_Policy);
        ext_le_param.ext_init = NULL;
    }
    ll_ext_init_close();
}

void ll_ext_init_rx_post_process(int *empty) LL_CODE;
void ll_ext_init_rx_post_process(int *empty)
{
    struct le_ext_init_link *link = ext_ll_hdl.ext_init_link;
    if (!((config_btctler_le_roles & 1) != 0 && link != NULL)) return;
    __ble5_ops->ext_upload_data(link->hw, ll_ext_init_rx_post);
    link = ext_ll_hdl.ext_init_link;
    switch (link->state) {
    case 17:
        if (log_tag_const_e_LL_EXT_INIT)
            log_print(4, NULL, "<Error>: [LL_EXT_INIT]init post LL_EXT_INITIATING_FAIL\r\n");
        ll_ext_init_close();
        ll_ext_init_open();
        break;
    case 18:
        if (log_tag_const_i_LL_EXT_INIT) {
            log_print(2, NULL, "[Info]: [LL_EXT_INIT]init post LL_EXT_CONNECTION_CREATE\r\n");
            log_print(2, NULL, "[Info]: [LL_EXT_INIT]--func=%s\r\n", "ll_exit_ext_init_status");
        }
        ext_ll_hdl.ext_init_link->connect_link = NULL;
        destroy_le_ext_init();
        break;
    default:
        __ble5_ops->ext_ioctrl(link->hw, 11, empty);
        break;
    }
}

int ll_ext_init_open(void) LL_CODE;
int ll_ext_init_open(void)
{
    struct le_ext_init_link *link = __new_ext_link(64);
    if (link) {
        link->hw = __ble5_ops->ext_open();
        if (!link->hw) {
            __bt_free(link);
            link = NULL;
        }
    }
    if (!link) {
        if (log_tag_const_e_LL_EXT_INIT)
            log_print(4, NULL, "<Error>: [LL_EXT_INIT]ext init secondary create error\r\n");
        return 7;
    }
    ext_ll_hdl.ext_init_link = link;
    __ble5_ops->ext_handler_register(link->hw, link, &ll_ext_init_handler);

    link->primary_legacy_hw = __ble_ops->open();
    if (!link->primary_legacy_hw) {
        if (log_tag_const_e_LL_EXT_INIT)
            log_print(4, NULL, "<Error>: [LL_EXT_INIT]ext init primary create error\r\n");
        return 7;
    }
    __ble5_ops->ext_adv_ctrl(link->hw, 6, link->primary_legacy_hw);
    __ble5_ops->ext_adv_ctrl(link->primary_legacy_hw, 6, link->hw);

    /* __set_ll_ext_init_state */
    struct ble_ext_init *ext = &link->extended_init;
    struct ble_conn_param *cp = &ext->conn_param;
    if (log_tag_const_i_LL_EXT_INIT)
        log_print(2, NULL, "[Info]: [LL_EXT_INIT]--func=%s\r\n", "__set_ll_ext_init_state");
    struct __ext_init *init = ext_le_param.ext_init;
    ext->ext_init = init;
    __set_ll_local_addr(link->primary_legacy_hw, &ext->local, init->Own_Address_Type);
    memcpy(cp, &sample_conn_param, 32);
    access_addr_generate(cp->aa);
    init = ext->ext_init;
    cp->latency = init->init_phy_param[0].Conn_Latency;
    cp->timeout = init->init_phy_param[0].Supervision_Timeout;
    cp->interval = init->init_phy_param[0].Conn_Interval_Max;
    cp->sca = 0;
    cp->widening = (u16)(((((u32)cp->interval * ((u32)cp->latency + 1) * ((u32)master_sca[0] + 500)) / 800) << 1) + 2);
    cp->hop = hop_generarate() & 31;
    u32 half = (u16)(cp->interval >> 1);
    cp->winoffset = (u16)(RANDOM_COUNTER % half + half);
    if (log_tag_const_i_LL_EXT_INIT) {
        log_print(2, NULL, "[Info]: [LL_EXT_INIT]init AA = \r\n");
        printf_buf(cp->aa, 4);
        log_print(2, NULL, "[Info]: [LL_EXT_INIT]init CRCInit = \r\n");
        printf_buf(cp->crcinit, 3);
        log_print(2, NULL, "[Info]: [LL_EXT_INIT]init WinSize: 0x%x\r\n", (int)cp->winsize);
        log_print(2, NULL, "[Info]: [LL_EXT_INIT]init WinOffset: 0x%x\r\n", (int)cp->winoffset);
        log_print(2, NULL, "[Info]: [LL_EXT_INIT]init interval: 0x%x\r\n", (int)cp->interval);
        log_print(2, NULL, "[Info]: [LL_EXT_INIT]init latency: 0x%x\r\n", (int)cp->latency);
        log_print(2, NULL, "[Info]: [LL_EXT_INIT]init timeout: 0x%x\r\n", (int)cp->timeout);
        log_print(2, NULL, "[Info]: [LL_EXT_INIT]init channel = \r\n");
        printf_buf(cp->channel, 5);
        log_print(2, NULL, "[Info]: [LL_EXT_INIT]init hop: 0x%x\r\n", (int)cp->hop);
        log_print(2, NULL, "[Info]: [LL_EXT_INIT]init sca: 0x%x\r\n", (int)cp->sca);
    }

    __set_ll_ext_local_addr(link->hw, &ext->local, ext->ext_init->Own_Address_Type);
    init = ext->ext_init;
    __ble5_ops->ext_ioctrl(link->hw, 10, init->Peer_Address_Type, init->Peer_Address);
    link->state = 16;
    link->extended_init.coded_s = ll_set_code_type;
    ll_afh_open(cp);
    __ble5_ops->ext_initiating(link->hw, link->primary_legacy_hw, ext);
    return 0;
}

#ifdef UNITTEST
#include "test.h"
#include <stdarg.h>
#include <stdlib.h>

struct ext_le_parameter ext_le_param;
struct ext_link_layer_handle ext_ll_hdl;
const struct ble5_operation *__ble5_ops;
const struct ble_operation *__ble_ops;
const struct ble_conn_param sample_conn_param = { { 1, 2, 3, 4 }, { 5, 6, 7 }, 8, 9, 10, 11, 12, { 1, 2, 3, 4, 5 }, .hop = 0x17, .sca = 5, 99, 98, (void *)0x1234 };
const u16 master_sca[8] = { 100, 0, 0, 0, 0, 0, 0, 0 };
u8 ll_set_code_type = 3;
u8 log_tag_const_e_LL_EXT_INIT = 1, log_tag_const_i_LL_EXT_INIT = 0;
volatile u32 ll_ext_init_random_counter;
int config_btctler_le_roles = 1;

static char trace[512];
static void T(const char *s) { strcat(trace, s); strcat(trace, " "); }
static int hop_value, resume_count, errors, infos;
static int new_link_fails;
static void *ext_open_result = (void *)0xe000;
static void *primary_open_result = (void *)0xb000;
static struct le_link conn_link;
static void *freed[8];
static int nfreed;
static u8 ioctrl_peer_type;
static const u8 *ioctrl_peer;
static void *last_create_hw;
static u8 last_create_txadd;
static u8 *last_create_peer;
static void *last_create_data;
static int last_ext_ioctrl_cmd;
static void *uploaded;
static int *ioctrl_empty;
static const struct ble_ext_handler *registered;
static u8 local_addr_type[2];
static int closes_conn;

void log_print(int level, const char *tag, const char *format, ...) { (void)tag; (void)format; if (level == 4) errors++; else infos++; }
void printf_buf(const void *b, int n) { (void)b; (void)n; T("pbuf"); }
void ll_thread_resume(void) { resume_count++; }
void __bt_free(void *p) { freed[nfreed++] = p; }
void *__new_ext_link(u32 len)
{
    CHECK(len == 64);
    if (new_link_fails) return NULL;
    return calloc(1, sizeof(struct le_ext_init_link));  /* the host struct is larger than the 64-byte SDK one */
}
void __ll_ext_link_close(void *link, void *hw) { T("linkclose"); CHECK(hw == ((struct le_ext_init_link *)link)->hw); }
void ll_close_conn_link(struct le_link *l) { CHECK(l == &conn_link); closes_conn++; T("closeconn"); }
void ll_ext_create_legacy_connection(struct le_link *l) { CHECK(l == &conn_link); T("legacyconn"); }
struct le_link *ll_create_conn_link(void *hw, struct le_addr *local, u8 role, u8 txadd, u8 *peer, void *data)
{
    (void)local;
    CHECK(role == 0);
    last_create_hw = hw; last_create_txadd = txadd; last_create_peer = peer; last_create_data = data;
    T("createconn");
    return &conn_link;
}
void __set_ll_local_addr(void *hw, struct le_addr *a, u8 t) { (void)hw; (void)a; local_addr_type[0] = t; T("localaddr"); }
void __set_ll_ext_local_addr(void *hw, struct le_addr *a, u8 t) { (void)hw; (void)a; local_addr_type[1] = t; T("extlocaladdr"); }
void access_addr_generate(u8 *aa) { T("aa"); aa[0] = 0xaa; }
u8 hop_generarate(void) { return (u8)hop_value; }
void ll_afh_open(struct ble_conn_param *cp) { (void)cp; T("afh"); }

static void m_legacy_open(void *hw) { (void)hw; T("lopen"); }
static void m_legacy_close(void *hw) { CHECK(hw == (void *)0xb000); T("lclose"); }
static void *m_ext_open(void) { T("eopen"); return ext_open_result; }
static void m_ext_ioctrl(void *hw, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    last_ext_ioctrl_cmd = cmd;
    if (cmd == 10) { ioctrl_peer_type = (u8)va_arg(ap, int); ioctrl_peer = va_arg(ap, const u8 *); }
    if (cmd == 11) ioctrl_empty = va_arg(ap, int *);
    va_end(ap);
    (void)hw;
    T("eioctrl");
}
static void m_ext_adv_ctrl(void *hw, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    void *o = va_arg(ap, void *);
    va_end(ap);
    CHECK(cmd == 6);
    T(hw == (void *)0xe000 ? "advctrl-e" : "advctrl-p");
    CHECK((hw == (void *)0xe000 && o == (void *)0xb000) || (hw == (void *)0xb000 && o == (void *)0xe000));
}
static void m_ext_initiating(void *hw, void *p, struct ble_ext_init *e) { CHECK(hw == (void *)0xe000 && p == (void *)0xb000 && e != NULL); T("initiating"); }
static void m_ext_handler_register(void *hw, void *priv, const struct ble_ext_handler *h) { CHECK(hw == (void *)0xe000); CHECK(priv == ext_ll_hdl.ext_init_link); registered = h; T("handlerreg"); }
static void m_ext_upload(void *hw, void (*cb)(void *, struct ble_ext_rx *)) { CHECK(hw == (void *)0xe000); uploaded = (void *)cb; T("upload"); }
static void *m_open(void) { T("popen"); return primary_open_result; }
static void m_ioctrl(void *hw, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    CHECK(cmd == 9);
    CHECK(va_arg(ap, int) == 0x55);
    CHECK(va_arg(ap, u8 *) != NULL);
    va_end(ap);
    (void)hw;
    T("pioctrl");
}

static struct ble5_operation ops5 = { m_legacy_open, m_legacy_close, m_ext_open, NULL, m_ext_ioctrl, m_ext_adv_ctrl, NULL, m_ext_initiating, m_ext_handler_register, m_ext_upload };
static struct ble_operation ops;

static struct __ext_init *make_init(void)
{
    struct __ext_init *i = calloc(1, sizeof(*i) + sizeof(struct __init_phy_param));
    i->Own_Address_Type = 2;
    i->Peer_Address_Type = 0x55;
    for (int k = 0; k < 6; k++) i->Peer_Address[k] = (u8)(0x10 + k);
    i->Initiating_PHYs = 1;
    i->init_phy_param[0].Conn_Interval_Max = 24;
    i->init_phy_param[0].Conn_Latency = 4;
    i->init_phy_param[0].Supervision_Timeout = 400;
    return i;
}

int main(void)
{
    ops.open = m_open;
    ops.ioctrl = m_ioctrl;
    __ble5_ops = &ops5;
    __ble_ops = &ops;
    ext_le_param.ext_init = make_init();

    /* Open: widening = ((24 * 5 * 600) / 800 << 1) + 2 = 182 (master_sca[0] = 100);
     * window offset = counter % (24 >> 1) + 12. */
    hop_value = 0x2d;                     /* masked to 5 bits: 0x0d */
    ll_ext_init_random_counter = 30;      /* 30 % 12 + 12 = 18 */
    CHECK(ll_ext_init_open() == 0);
    struct le_ext_init_link *l = ext_ll_hdl.ext_init_link;
    CHECK(l && l->hw == (void *)0xe000 && l->primary_legacy_hw == (void *)0xb000);
    CHECK(registered == &ll_ext_init_handler);
    CHECK(l->state == 16 && l->extended_init.coded_s == 3);
    CHECK(l->extended_init.ext_init == ext_le_param.ext_init);
    struct ble_conn_param *cp = &l->extended_init.conn_param;
    CHECK(cp->aa[0] == 0xaa && cp->aa[1] == 2 && cp->crcinit[0] == 5 && cp->winsize == 8 && cp->channel[4] == 5);
    CHECK(cp->interval == 24 && cp->latency == 4 && cp->timeout == 400);
    CHECK(cp->widening == 182 && cp->widening_offset == 98);   /* the host's afh pointer sits past the 32 copied bytes */
    CHECK(cp->winoffset == 18 && cp->hop == 0x0d && cp->sca == 0);
    CHECK(local_addr_type[0] == 2 && local_addr_type[1] == 2);
    CHECK(ioctrl_peer_type == 0x55 && ioctrl_peer == ext_le_param.ext_init->Peer_Address);
    CHECK(!strcmp(trace, "eopen handlerreg popen advctrl-e advctrl-p localaddr aa extlocaladdr eioctrl afh initiating "));

    /* Rx: only type 8 starts the legacy connection; any non-empty rx wakes the thread. */
    struct { struct ble_ext_rx rx; u8 pdu[16]; } r;
    memset(&r, 0, sizeof(r));
    r.rx.type = 8; r.rx.txadd = 1; r.rx.len = 5;
    trace[0] = 0;
    ll_ext_init_handler.ext_event_probe_handler(l, 7);
    CHECK(l->state == 16 && !resume_count);
    ll_ext_init_handler.ext_event_probe_handler(l, 12);
    CHECK(l->state == 17 && resume_count == 1);
    l->state = 16;
    ll_ext_init_handler.ext_rx_probe_handler(l, &r.rx);
    CHECK(l->connect_link == &conn_link && last_create_txadd == 1 && last_create_peer == r.rx.data + 2);
    CHECK(last_create_data == &l->extended_init.conn_param && last_create_hw == l->hw && resume_count == 2);
    CHECK(!strcmp(trace, "lopen eioctrl pioctrl createconn "));
    r.rx.type = 3; r.rx.len = 0; trace[0] = 0; resume_count = 0;
    ll_ext_init_handler.ext_rx_probe_handler(l, &r.rx);
    CHECK(!trace[0] && !resume_count);
    r.rx.len = 1;
    ll_ext_init_handler.ext_rx_probe_handler(l, &r.rx);
    CHECK(!trace[0] && resume_count == 1);
    config_btctler_le_roles = 2;           /* no initiator role: ignored */
    r.rx.type = 8;
    ll_ext_init_handler.ext_rx_probe_handler(l, &r.rx);
    CHECK(!trace[0] && resume_count == 1);
    config_btctler_le_roles = 3;
    ll_ext_init_rx_post(l, &r.rx);
    CHECK(l->state == 18 && !strcmp(trace, "legacyconn "));
    l->state = 16; r.rx.type = 5; trace[0] = 0;
    ll_ext_init_rx_post(l, &r.rx);
    CHECK(l->state == 16 && !trace[0]);

    /* Post-process: state 16 asks the radio for more; 18 detaches the connect link and destroys. */
    int empty = 0;
    trace[0] = 0;
    ll_ext_init_rx_post_process(&empty);
    CHECK(uploaded == (void *)ll_ext_init_rx_post && last_ext_ioctrl_cmd == 11 && ioctrl_empty == &empty);
    CHECK(!strcmp(trace, "upload eioctrl "));
    config_btctler_le_roles = 2; trace[0] = 0;
    ll_ext_init_rx_post_process(&empty);
    CHECK(!trace[0]);
    config_btctler_le_roles = 1;
    l->state = 18; l->connect_link = &conn_link; trace[0] = 0; nfreed = 0;
    void *old_init = ext_le_param.ext_init;
    ll_ext_init_rx_post_process(&empty);
    CHECK(!strcmp(trace, "upload lclose linkclose "));     /* connect link cleared first: no closeconn */
    CHECK(closes_conn == 0 && ext_ll_hdl.ext_init_link == NULL && ext_le_param.ext_init == NULL);
    CHECK(nfreed == 1 && freed[0] == old_init);
    free(l);

    /* Failure event: close (with connect link), then reopen from the new ext_init. */
    ext_le_param.ext_init = make_init();
    CHECK(ll_ext_init_open() == 0);
    l = ext_ll_hdl.ext_init_link;
    l->state = 17; l->connect_link = &conn_link; trace[0] = 0; errors = 0;
    ll_ext_init_rx_post_process(&empty);
    CHECK(errors == 1 && closes_conn == 1);
    CHECK(!strncmp(trace, "upload lclose closeconn linkclose eopen handlerreg popen ", 55));
    CHECK(ext_ll_hdl.ext_init_link != NULL && ext_ll_hdl.ext_init_link != l && ext_ll_hdl.ext_init_link->state == 16);
    free(l);

    /* destroy: frees ext_init and closes; with nothing open it does nothing. */
    destroy_le_ext_init();
    CHECK(ext_ll_hdl.ext_init_link == NULL && ext_le_param.ext_init == NULL);
    trace[0] = 0;
    destroy_le_ext_init();
    CHECK(!trace[0]);

    /* Open failures: secondary allocation, secondary radio, primary radio. */
    errors = 0; new_link_fails = 1; trace[0] = 0;
    CHECK(ll_ext_init_open() == 7 && errors == 1 && ext_ll_hdl.ext_init_link == NULL && !trace[0]);
    new_link_fails = 0; ext_open_result = NULL; nfreed = 0;
    CHECK(ll_ext_init_open() == 7 && errors == 2 && nfreed == 1 && ext_ll_hdl.ext_init_link == NULL);
    ext_open_result = (void *)0xe000; primary_open_result = NULL; trace[0] = 0;
    ext_le_param.ext_init = make_init();
    CHECK(ll_ext_init_open() == 7 && errors == 3 && ext_ll_hdl.ext_init_link != NULL);
    CHECK(!strcmp(trace, "eopen handlerreg popen "));  /* the secondary link stays registered */
    CHECK(ext_ll_hdl.ext_init_link->primary_legacy_hw == NULL);
    CHECK(infos == 0);
    return TEST_DONE("btctrler_ll_ext_init");
}
#endif
