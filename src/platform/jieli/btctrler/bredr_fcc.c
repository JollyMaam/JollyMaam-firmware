/* The SDK's Classic FCC test mode (bredr_fcc.c): a continuous transmit or
 * receive test on one fixed channel.  bredr_fcc_init creates the test link
 * (a link_connection, its frame, a 768-byte transmit bulk and the bredr_dut
 * state) once and applies a default configuration; bredr_fcc_set_param
 * (vendor HCI command) applies mode, channel, packet type, data type, power
 * and hopping from six bytes.  Both only act when CONFIG_TEST_FCC_CODE is
 * set, which this firmware's configuration leaves at 0.
 *
 * SDK quirks kept: the packet type is looked up in a 9-entry table with the
 * parameter byte as index and no bounds check; the 0x1c000c register is
 * cleared of bits 2 and 9 and the 0x1c0000 register gets bit 19 set on every
 * configuration; the assertions of the link creation run the SDK's fall-through
 * (with config_asser set they print and call cpu_assert_debug, then carry on
 * with the null value; otherwise they reset the chip and carry on); the DUT
 * state read in fcc_cfg_analog is re-read from g_dut_hdl, not passed on. */
#include <stdio.h>
#include <string.h>
#ifdef UNITTEST
#include <stdarg.h>
#endif
#include "btctrler/glue.h"

void log_print(int level, const char *file, const char *format, ...);

#ifdef UNITTEST
extern u8 log_tag_const_i_BL;
extern int CONFIG_TEST_FCC_CODE;
#else
extern const u8 log_tag_const_i_BL;
extern const int CONFIG_TEST_FCC_CODE;
#endif

#define FCC_CODE __attribute__((section(".classic_rf_code")))
#define FCC_CONST __attribute__((section(".classic_rf_const")))

#ifdef UNITTEST
extern volatile u32 fcc_reg_c0000, fcc_reg_c000c, fcc_reg_c00d0;
#define BB_REG_A fcc_reg_c0000
#define BB_REG_C fcc_reg_c000c
#define BB_REG_B fcc_reg_c00d0
#else
#define BB_REG_A (*(volatile u32 *)(uintptr_t)0x1c0000)
#define BB_REG_C (*(volatile u32 *)(uintptr_t)0x1c000c)
#define BB_REG_B (*(volatile u32 *)(uintptr_t)0x1c00d0)
#endif

#define FCC_FILE "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/br23/baseband/classic/bredr_fcc.c"

static const u8 packet_table[9] FCC_CONST = { 0x04, 0x0b, 0x0f, 0x24, 0x2a, 0x2e, 0x28, 0x2b, 0x2f };
static const u8 local_lap[6] FCC_CONST = { 0x33, 0x8b, 0x9e, 0x00, 0x00, 0x00 };
static const u8 local_bch[5] FCC_CONST = { 0xe2, 0x3a, 0x1a, 0x33, 0x02 };

static void cpu_reset(void)
{
    p33_soft_reset();
}

/* Create the test link on first use. */
static FCC_CODE void fcc_creat_link(void)
{
    if (bredr_link_v.fcc_lc)
        return;
    struct link_connection *lc = get_link_connection();
    bredr_link_v.fcc_lc = lc;
    struct ctrl_frame *frame;
    if (config_asser) {
        if (!lc) {
            printf("file:%s, line:%d", FCC_FILE, 58);
            printf("ASSERT-FAILD: __this->fcc_lc ");
            cpu_assert_debug();
        }
        frame = bredr_bd_get_frame();
        if (!frame) {
            printf("file:%s, line:%d", FCC_FILE, 61);
            printf("ASSERT-FAILD: frame ");
            cpu_assert_debug();
        }
    } else {
        if (!lc) cpu_reset();
        frame = bredr_bd_get_frame();
        if (!frame) cpu_reset();
    }
    bredr_link_v.fcc_lc->frame = frame;
    bredr_link_v.fcc_lc->tx_bulk[0] = bredr_tx_bulk_alloc(768);
    g_dut_hdl = lmp_malloc(36);
}

/* Channel number to the baseband's hop register value. */
static FCC_CODE u8 cal_hop_fre(u8 fre)
{
    int half = fre >> 1;
    return (fre & 1) == 0 ? half : half + 40;
}

/* Program the baseband for the settings in g_dut_hdl. */
static FCC_CODE void fcc_cfg_analog(struct link_connection *lc)
{
    struct ctrl_frame *frame = lc->frame;
    struct bredr_dut *dut = g_dut_hdl;
    u8 packet_type = dut->packet_type;
    dut->packet_group = packet_type >> 4;
    u32 packet_num = packet_type & 15;
    dut->packet_num = packet_num;
    u8 miscntl = frame->miscntl;
    frame->miscntl = packet_type > 31 ? (miscntl | 8) : (miscntl & ~8);
    dut->packet_len = *dut_get_packet_max_len(packet_num, packet_type >> 4);
    struct bredr_tx_bulk *bulk = bredr_link_v.fcc_lc->tx_bulk[0];
    bulk->packet = dut->packet_num;
    bulk->len = dut->packet_len;
    bulk->llid = 2;
    BB_REG_C = BB_REG_C & ~516u;
    radio_set_eninv(0);
    BB_REG_A = BB_REG_A | 524288;
    if (dut->data_type != 0) {
        close_zeroif();
        BB_REG_B = BB_REG_B & ~1u;
        memset(bulk->data, 0xff, bulk->len);
        bt_set_single_carrier();
        bredr_freq_table_init(0);
    } else {
        certification(110);
        bt_reload_trim();
        BB_REG_B = BB_REG_B | 1;
        BB_REG_B = BB_REG_B & ~2u;
        bredr_freq_table_init(1);
    }
    __write_reg_clkoffset(frame, 0);
    __write_reg_bdaddr(frame, local_lap);
    __write_reg_bch(frame, local_bch);
    frame->linkcntl0 = frame->linkcntl0 & ~16;
    frame->linkcntl1 = frame->linkcntl1 & ~16;
    __write_reg_txinfo(frame, 0, bulk->data, bulk->len, bulk->llid, 1, bulk->packet);
    __write_reg_txinfo(frame, 1, bulk->data, bulk->len, bulk->llid, 1, bulk->packet);
    if (dut->trx_mode != 0)
        __write_reg_format(frame, 0, 3);
    else
        __write_reg_format(frame, 0, 2);
    bredr_pwr_set(lc, 0, dut->tx_power);
    u32 reg = BB_REG_A;
    if (dut->hopping_mode != 0) {
        BB_REG_A = reg & ~65536u;
    } else {
        BB_REG_A = reg | 65536;
        frame->rxfreq = cal_hop_fre(dut->rx_fre);
        frame->txfreq = cal_hop_fre(dut->tx_fre);
    }
    if (dut->trx_mode != 0) {
        bt_pll_reset(0);
    } else {
        bt_pll_reset(1);
        bredr_bd_frame_enable(frame, 0, 1);
    }
}

static FCC_CODE void set_fcc_default_info(struct link_connection *lc, u8 trx_mode, u8 fre, u8 p_type, u8 d_type,
                                          u8 tx_power, u8 hop_mode)
{
    struct bredr_dut *dut = g_dut_hdl;
    dut->trx_mode = trx_mode;
    dut->tx_fre = fre;
    dut->rx_fre = fre;
    dut->packet_type = packet_table[p_type];
    dut->data_type = d_type;
    dut->tx_power = tx_power;
    dut->hopping_mode = hop_mode;
    if (log_tag_const_i_BL) {
        log_print(2, (const char *)0, "[Info]: [BL]mode :%d \n\r\n", trx_mode);
        log_print(2, (const char *)0, "[Info]: [BL]tx_fre :%d \n\r\n", dut->tx_fre);
        log_print(2, (const char *)0, "[Info]: [BL]packet_type :%d \n\r\n", dut->packet_type);
        log_print(2, (const char *)0, "[Info]: [BL]data_type :%d \n\r\n", dut->data_type);
        log_print(2, (const char *)0, "[Info]: [BL]tx_power :%d \n\r\n", dut->tx_power);
        log_print(2, (const char *)0, "[Info]: [BL]hopping_mode :%d \n\r\n", dut->hopping_mode);
    }
    fcc_cfg_analog(lc);
}

/* Vendor command: data = trx mode, channel, packet type index, data type,
 * tx power, hopping mode. */
FCC_CODE void bredr_fcc_set_param(u8 *data)
{
    if (CONFIG_TEST_FCC_CODE == 0 || bredr_link_v.fcc_lc == 0)
        return;
    set_fcc_default_info(bredr_link_v.fcc_lc, data[0], data[1], data[2], data[3], data[4], data[5]);
}

FCC_CODE void bredr_fcc_init(u8 mode, u8 fre)
{
    if (CONFIG_TEST_FCC_CODE == 0)
        return;
    fcc_creat_link();
    if (mode == 16) {
        BB_REG_B = BB_REG_B & ~4u;
        set_fcc_default_info(bredr_link_v.fcc_lc, 0, fre, 0, 1, 15, 0);
        BB_REG_B = BB_REG_B | 4;
    } else {
        set_fcc_default_info(bredr_link_v.fcc_lc, 0, fre, 0, 1, 15, 0);
    }
}

#ifdef UNITTEST
#include "test.h"
u8 log_tag_const_i_BL = 0;
int CONFIG_TEST_FCC_CODE = 0;
int config_asser = 0;
volatile u32 fcc_reg_c0000, fcc_reg_c000c, fcc_reg_c00d0;
struct bredr_link_var bredr_link_v;
struct bredr_dut *g_dut_hdl;

static char trace[2048];
static void T(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(trace);
    va_start(ap, fmt);
    vsnprintf(trace + n, sizeof trace - n, fmt, ap);
    va_end(ap);
}
#define EXPECT(s) do { if (strcmp(trace, s)) { printf("FAIL %s:%d: trace\n  got  %s\n  want %s\n", __FILE__, __LINE__, trace, s); test_failures++; } trace[0] = 0; } while (0)

static struct link_connection lc_obj;
static struct ctrl_frame frame_obj;
static struct bredr_dut dut_obj;
static struct { struct bredr_tx_bulk bulk; u8 data[800]; } bulk_obj;
static u16 max_len = 183;
static int get_lc_null, get_frame_null;  /* stub switches; the tests keep them 0 */

void log_print(int level, const char *file, const char *format, ...)
{
    va_list ap;
    (void)file;
    va_start(ap, format);
    T("log(%d,", level);
    char buf[64];
    vsnprintf(buf, sizeof buf, format, ap);
    for (char *p = buf; *p; p++) if (*p == '\n' || *p == '\r') *p = '~';
    T("%s);", buf);
    va_end(ap);
}
void p33_soft_reset(void) { T("reset;"); }
void cpu_assert_debug(void) { T("assert;"); }
struct link_connection *get_link_connection() { T("get_lc;"); return get_lc_null ? 0 : &lc_obj; }
struct ctrl_frame *bredr_bd_get_frame() { T("get_frame;"); return get_frame_null ? 0 : &frame_obj; }
struct bredr_tx_bulk *bredr_tx_bulk_alloc(int size) { T("bulk(%d);", size); return &bulk_obj.bulk; }
void *lmp_malloc(int size) { T("malloc(%d);", size); return &dut_obj; }
const u16 *dut_get_packet_max_len(int packet, u8 group) { T("maxlen(%d,%d);", packet, group); return &max_len; }
void radio_set_eninv(int v) { T("eninv(%d,C=%x);", v, (unsigned)fcc_reg_c000c); }
void close_zeroif() { T("close_zeroif(B=%x);", (unsigned)fcc_reg_c00d0); }
void bt_set_single_carrier() { T("single_carrier;"); }
void bt_reload_trim() { T("reload_trim;"); }
void certification(u8 l) { T("cert(%d);", l); }
void bredr_freq_table_init(u8 m) { T("freq_init(%d,A=%x,B=%x);", m, (unsigned)fcc_reg_c0000, (unsigned)fcc_reg_c00d0); }
void __write_reg_clkoffset(struct ctrl_frame *f, s32 o) { CHECK(f == &frame_obj); T("clkoff(%d);", o); }
void __write_reg_bdaddr(struct ctrl_frame *f, const u8 *a) { CHECK(f == &frame_obj); T("bdaddr(%02x%02x%02x%02x%02x%02x);", a[0], a[1], a[2], a[3], a[4], a[5]); }
void __write_reg_bch(struct ctrl_frame *f, const u8 *b) { CHECK(f == &frame_obj); T("bch(%02x%02x%02x%02x%02x);", b[0], b[1], b[2], b[3], b[4]); }
void __write_reg_txinfo(struct ctrl_frame *f, int idx, void *data, int len, int llid, int flow, int packet)
{
    CHECK(f == &frame_obj && data == bulk_obj.bulk.data);
    T("txinfo(%d,len=%d,llid=%d,flow=%d,pkt=%d);", idx, len, llid, flow, packet);
}
void __write_reg_format(struct ctrl_frame *f, u8 train, u8 fmt) { CHECK(f == &frame_obj); T("format(%d,%d);", train, fmt); }
int bredr_pwr_set(struct link_connection *lc, char sel, char inc) { CHECK(lc == &lc_obj); T("pwr(%d,%d);", sel, inc); return 0; }
void bt_pll_reset(u8 m) { T("pll(%d);", m); }
void bredr_bd_frame_enable(struct ctrl_frame *f, u8 flag, u8 en) { CHECK(f == &frame_obj); T("frame_en(%d,%d);", flag, en); }

static void reset_state(void)
{
    memset(&bredr_link_v, 0, sizeof bredr_link_v);
    memset(&lc_obj, 0, sizeof lc_obj);
    memset(&frame_obj, 0, sizeof frame_obj);
    memset(&dut_obj, 0, sizeof dut_obj);
    memset(&bulk_obj, 0, sizeof bulk_obj);
    g_dut_hdl = &dut_obj;
    trace[0] = 0;
}

int main(void)
{
    /* Gated off: nothing happens, however the link looks. */
    reset_state();
    {
        u8 p[6] = { 1, 20, 3, 1, 7, 1 };
        CONFIG_TEST_FCC_CODE = 0;
        bredr_link_v.fcc_lc = &lc_obj;
        bredr_fcc_set_param(p);
        bredr_fcc_init(16, 5);
        EXPECT("");
        CHECK(bredr_link_v.fcc_lc == &lc_obj && dut_obj.trx_mode == 0);
        /* Enabled but no test link yet: set_param does nothing. */
        CONFIG_TEST_FCC_CODE = 1;
        bredr_link_v.fcc_lc = 0;
        bredr_fcc_set_param(p);
        EXPECT("");
    }

    /* set_param, receive, continuous (non-zero data type) path.  Packet type
     * index 3 is table value 0x24: group 2, number 4, above 31 so miscntl
     * bit 3 is set.  The tx bulk gets packet 4, length 183 (the stub's
     * table answer), llid 2 and 0xff fill. */
    reset_state();
    {
        u8 p[6] = { 1, 20, 3, 1, 7, 1 };
        bredr_link_v.fcc_lc = &lc_obj;
        lc_obj.frame = &frame_obj;
        lc_obj.tx_bulk[0] = &bulk_obj.bulk;
        frame_obj.miscntl = 0x01;
        frame_obj.linkcntl0 = 0x31;
        frame_obj.linkcntl1 = 0x10;
        fcc_reg_c000c = 0xffffffff;
        fcc_reg_c0000 = 0;
        fcc_reg_c00d0 = 7;
        memset(bulk_obj.data, 0, sizeof bulk_obj.data);
        bredr_fcc_set_param(p);
        EXPECT("maxlen(4,2);eninv(0,C=fffffdfb);close_zeroif(B=7);single_carrier;freq_init(0,A=80000,B=6);"
               "clkoff(0);bdaddr(338b9e000000);bch(e23a1a3302);"
               "txinfo(0,len=183,llid=2,flow=1,pkt=4);txinfo(1,len=183,llid=2,flow=1,pkt=4);"
               "format(0,3);pwr(0,7);pll(0);");
        CHECK(dut_obj.trx_mode == 1 && dut_obj.tx_fre == 20 && dut_obj.rx_fre == 20);
        CHECK(dut_obj.packet_type == 0x24 && dut_obj.data_type == 1 && dut_obj.tx_power == 7);
        CHECK(dut_obj.hopping_mode == 1 && dut_obj.packet_group == 2 && dut_obj.packet_num == 4);
        CHECK(dut_obj.packet_len == 183);
        CHECK(frame_obj.miscntl == 0x09 && frame_obj.linkcntl0 == 0x21 && frame_obj.linkcntl1 == 0x00);
        CHECK(bulk_obj.bulk.packet == 4 && bulk_obj.bulk.len == 183 && bulk_obj.bulk.llid == 2);
        CHECK(bulk_obj.bulk.data[0] == 0xff && bulk_obj.bulk.data[182] == 0xff && bulk_obj.bulk.data[183] == 0);
        CHECK(fcc_reg_c000c == 0xfffffdfb && fcc_reg_c0000 == 0x80000 && fcc_reg_c00d0 == 6);
        CHECK(frame_obj.rxfreq == 0 && frame_obj.txfreq == 0);   /* hopping on: channels untouched */
    }

    /* set_param, transmit, modulated (data type 0), fixed channel.  Index 0
     * is 0x04: group 0, number 4, miscntl bit 3 cleared.  Channel 21 is odd:
     * 21/2 + 40 = 50.  Register A bit 16 is set for the fixed channel. */
    reset_state();
    {
        u8 p[6] = { 0, 21, 0, 0, 15, 0 };
        bredr_link_v.fcc_lc = &lc_obj;
        lc_obj.frame = &frame_obj;
        lc_obj.tx_bulk[0] = &bulk_obj.bulk;
        frame_obj.miscntl = 0xff;
        frame_obj.linkcntl0 = 0xffff;
        frame_obj.linkcntl1 = 0x0010;
        fcc_reg_c000c = 0x204;
        fcc_reg_c0000 = 0x1;
        fcc_reg_c00d0 = 2;
        bredr_fcc_set_param(p);
        EXPECT("maxlen(4,0);eninv(0,C=0);cert(110);reload_trim;freq_init(1,A=80001,B=1);"
               "clkoff(0);bdaddr(338b9e000000);bch(e23a1a3302);"
               "txinfo(0,len=183,llid=2,flow=1,pkt=4);txinfo(1,len=183,llid=2,flow=1,pkt=4);"
               "format(0,2);pwr(0,15);pll(1);frame_en(0,1);");
        CHECK(dut_obj.packet_type == 0x04 && dut_obj.packet_group == 0 && dut_obj.packet_num == 4);
        CHECK(frame_obj.miscntl == 0xf7 && frame_obj.linkcntl0 == 0xffef && frame_obj.linkcntl1 == 0);
        CHECK(frame_obj.rxfreq == 50 && frame_obj.txfreq == 50);
        CHECK(fcc_reg_c0000 == 0x90001 && fcc_reg_c00d0 == 1 && fcc_reg_c000c == 0);
        /* Even channel: 20/2 = 10. */
        p[1] = 20;
        bredr_fcc_set_param(p);
        trace[0] = 0;
        CHECK(frame_obj.rxfreq == 10 && frame_obj.txfreq == 10);
        /* Highest table entry (index 8 = 0x2f) and the log lines. */
        p[2] = 8;
        log_tag_const_i_BL = 1;
        bredr_fcc_set_param(p);
        log_tag_const_i_BL = 0;
        CHECK(dut_obj.packet_type == 0x2f && dut_obj.packet_group == 2 && dut_obj.packet_num == 15);
        static const char want[] = "log(2,[Info]: [BL]mode :0 ~~~);log(2,[Info]: [BL]tx_fre :20 ~~~);"
                                    "log(2,[Info]: [BL]packet_type :47 ~~~);log(2,[Info]: [BL]data_type :0 ~~~);"
                              "log(2,[Info]: [BL]tx_power :15 ~~~);log(2,[Info]: [BL]hopping_mode :0 ~~~);maxlen(15,2);";
        CHECK(!strncmp(trace, want, sizeof want - 1));
        trace[0] = 0;
    }

    /* init: creates the link once, then applies mode 0, fre, packet 0 (0x04),
     * data 1, power 15, hopping 0 (fixed channel, 20 -> 10).  Mode 16
     * clears bit 2 of register B around the configuration (and the data
     * type 1 path clears bit 0). */
    reset_state();
    CONFIG_TEST_FCC_CODE = 1;
    config_asser = 0;
    lc_obj.tx_bulk[0] = 0;
    fcc_reg_c00d0 = 0x5;
    fcc_reg_c000c = 0;
    fcc_reg_c0000 = 0;
    bredr_fcc_init(16, 20);
    EXPECT("get_lc;get_frame;bulk(768);malloc(36);"
           "maxlen(4,0);eninv(0,C=0);close_zeroif(B=1);single_carrier;freq_init(0,A=80000,B=0);"
           "clkoff(0);bdaddr(338b9e000000);bch(e23a1a3302);"
           "txinfo(0,len=183,llid=2,flow=1,pkt=4);txinfo(1,len=183,llid=2,flow=1,pkt=4);"
           "format(0,2);pwr(0,15);pll(1);frame_en(0,1);");
    CHECK(bredr_link_v.fcc_lc == &lc_obj && lc_obj.frame == &frame_obj && lc_obj.tx_bulk[0] == &bulk_obj.bulk);
    CHECK(g_dut_hdl == &dut_obj && dut_obj.tx_fre == 20 && dut_obj.rx_fre == 20 && dut_obj.trx_mode == 0);
    CHECK(fcc_reg_c00d0 == 4 && frame_obj.rxfreq == 10 && frame_obj.txfreq == 10);
    /* Second call: the link exists; other modes leave register B bit 2 alone. */
    fcc_reg_c00d0 = 0;
    bredr_fcc_init(4, 3);
    CHECK(strstr(trace, "get_lc") == 0 && strstr(trace, "malloc") == 0);
    CHECK(dut_obj.tx_fre == 3 /* 3/2 + 40 = 41 */ && frame_obj.rxfreq == 41 && frame_obj.txfreq == 41 && fcc_reg_c00d0 == 0);
    trace[0] = 0;

    /* Link creation with config_asser set and nothing failing: no assertion
     * output.  (The failing paths continue into the configuration with a
     * null link or frame, as the SDK does, which the host cannot run.) */
    reset_state();
    config_asser = 1;
    bredr_fcc_init(4, 1);
    CHECK(!strncmp(trace, "get_lc;get_frame;bulk(768);malloc(36);", 38));
    CHECK(bredr_link_v.fcc_lc == &lc_obj && lc_obj.frame == &frame_obj);
    return TEST_DONE("btctrler_bredr_fcc");
}
#endif
