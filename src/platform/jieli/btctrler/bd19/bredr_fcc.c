/* BD19 Classic FCC test mode, recovered from occurrence 1 of bredr_fcc.c.o.
 * Preserve volatile register widths/order, unchecked packet-table indexing,
 * callback reloads and assertion fall-through. BR23 has its own source. */
#include <stdio.h>
#include <string.h>
#ifdef UNITTEST
#include <stdarg.h>
#endif
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG int CONFIG_TEST_FCC_CODE;
#define FCC_CODE __attribute__((section(".classic_rf_code")))
#define FCC_CONST __attribute__((section(".classic_rf_const")))
#ifdef UNITTEST
static u32 fcc_read32(u32 address);
static void fcc_write32(u32 address, u32 value);
#define READ32(address) fcc_read32(address)
#define WRITE32(address, value) fcc_write32(address, value)
#else
#define READ32(address) (*(volatile u32 *)(uintptr_t)(address))
#define WRITE32(address, value) (*(volatile u32 *)(uintptr_t)(address) = (value))
#endif

#define FCC_FILE "/jks/workspace/data_trans_sdk_export_gitlab/data_trans_sdk/lib/btctrler/port/bd19/baseband/classic_new/bredr_fcc.c"

static const u8 packet_table[6] FCC_CONST = { 0x04, 0x0b, 0x0f, 0x24, 0x2a, 0x2e };
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
            printf("file:%s, line:%d", FCC_FILE, 52);
            printf("ASSERT-FAILD: __this->fcc_lc ");
            cpu_assert_debug();
        }
        frame = bredr_bd_get_frame();
        if (!frame) {
            printf("file:%s, line:%d", FCC_FILE, 55);
            printf("ASSERT-FAILD: frame ");
            cpu_assert_debug();
        }
    } else {
        if (!lc) cpu_reset();
        frame = bredr_bd_get_frame();
        if (!frame) cpu_reset();
    }
    bredr_link_v.fcc_lc->frame = frame;
    struct bredr_tx_bulk *bulk = bredr_tx_bulk_alloc(768);
    bredr_link_v.fcc_lc->tx_bulk[0] = bulk;
    g_dut_hdl = lmp_malloc(36);
    for (int i = 0; i < 2; ++i) link_conn_set_rx_bulk_in_irq(bredr_link_v.fcc_lc, i);
    bredr_frame_set_irq_handler(frame, bredr_link_v.fcc_lc, 0);
}

/* Channel number to the baseband's hop register value. */
static inline __attribute__((always_inline)) u8 cal_hop_fre(u8 fre)
{
    int half = fre >> 1;
    return (fre & 1) == 0 ? half : half + 40;
}

/* Program the baseband for the settings in g_dut_hdl. */
static FCC_CODE void fcc_cfg_analog(struct link_connection *lc, u8 packet_index)
{
    struct ctrl_frame *frame = lc->frame;
    struct bredr_dut *dut = g_dut_hdl;
    u8 packet_type = dut->packet_type;
    dut->packet_group = packet_type >> 4;
    u32 packet_num = packet_type & 15;
    dut->packet_num = packet_num;
    u16 miscntl = frame->miscntl;
    frame->miscntl = (u32)((int)packet_index - 3) < 3 ? (miscntl | 8) : (miscntl & ~8);
    dut->packet_len = *dut_get_packet_max_len(packet_num, packet_type >> 4);
    struct bredr_tx_bulk *bulk = bredr_link_v.fcc_lc->tx_bulk[0];
    bulk->packet = dut->packet_num;
    bulk->len = dut->packet_len;
    bulk->llid = 2;
    WRITE32(0x1c001cu, READ32(0x1c001cu) & ~255u);
    WRITE32(0x1c0108u, READ32(0x1c0108u) & ~255u);
    bredr_bd_frame_disable(frame);
    bredr_pwr_set(lc, 0, dut->tx_power);
    frame->optcntl |= 1;
    if (dut->data_type != 0) {
        WRITE32(0x1c0100u, READ32(0x1c0100u) & ~8u);
        __builtin_memset(bulk->data, 0xff, bulk->len);
        bt_set_single_carrier();
        bredr_freq_table_init((u8 *)&frame->freq_table[0][0], 0);
    } else {
        bt_reload_trim();
        WRITE32(0x1c0100u, READ32(0x1c0100u) | 8u);
        WRITE32(0x1c0100u, READ32(0x1c0100u) & ~4u);
        bredr_freq_table_init((u8 *)&frame->freq_table[0][0], 1);
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
    WRITE32(0x1c0110u, 0);
    WRITE32(0x1c0114u, 0);
    frame->miscntl &= ~4u;
    wind_align_set(103);
    windset(frame, 103, 0, 0);
    WRITE32(0x1c0008u, (READ32(0x1c0008u) & ~4095u) | 188u);
    if (dut->hopping_mode != 0) {
        frame->format &= ~128u;
    } else {
        frame->format |= 128u;
        WRITE32(0x1c011cu, 0);
        u32 tx = cal_hop_fre(dut->tx_fre);
        WRITE32(0x1c011cu, READ32(0x1c011cu) | tx);
        u32 rx = (u32)cal_hop_fre(dut->rx_fre) << 7;
        WRITE32(0x1c011cu, READ32(0x1c011cu) | rx);
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
    fcc_cfg_analog(lc, p_type);
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
        WRITE32(0x1c0100u, READ32(0x1c0100u) & ~16u);
        set_fcc_default_info(bredr_link_v.fcc_lc, 0, fre, 0, 1, 15, 0);
        WRITE32(0x1c0100u, READ32(0x1c0100u) | 16u);
    } else {
        set_fcc_default_info(bredr_link_v.fcc_lc, 0, fre, 0, 1, 15, 0);
    }
}

#ifdef UNITTEST
#include "test.h"
#include <setjmp.h>

int CONFIG_TEST_FCC_CODE, config_asser;
struct bredr_link_var bredr_link_v;
struct bredr_dut *g_dut_hdl;
static struct link_connection links[3];
static struct ctrl_frame frame;
static struct bredr_dut dut;
static struct { struct bredr_tx_bulk bulk; u8 storage[800]; } tx;
/* Address the backing allocation rather than indexing the SDK's GNU data[0]. */
static u8 *test_payload(void)
{ return (u8 *)&tx + offsetof(struct bredr_tx_bulk, data); }
static u32 registers[8];
static const u32 addresses[] = {0x1c001c, 0x1c0108, 0x1c0100, 0x1c0110,
                               0x1c0114, 0x1c0008, 0x1c011c};
static char trace[4096];
static unsigned trace_length;
static int packet_seen, group_seen, power_seen, format_seen, mutate_links, mutate_frequencies;
static int missing_link, missing_frame, fail_malloc, stop_on_assert, fault_calls;
static jmp_buf fault_return;
static u16 packet_length = 183;
static void record(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(trace + trace_length, sizeof(trace) - trace_length, fmt, ap);
    va_end(ap); CHECK(n >= 0 && (unsigned)n < sizeof(trace) - trace_length);
    if (n >= 0) trace_length += (unsigned)n;
}
static unsigned reg_index(u32 address)
{
    for (unsigned i = 0; i < sizeof(addresses) / sizeof(*addresses); ++i)
        if (addresses[i] == address) return i;
    CHECK(0); return 7;
}
static u32 fcc_read32(u32 address)
{
    u32 value = registers[reg_index(address)];
    record("r%04x=%08x;", address & 0xffff, value); return value;
}
static void fcc_write32(u32 address, u32 value)
{
    record("w%04x=%08x;", address & 0xffff, value);
    registers[reg_index(address)] = value;
}
static void expect(const char *want)
{
    if (strcmp(trace, want)) {
        printf("FAIL FCC trace\n got: %s\nwant: %s\n", trace, want); ++test_failures;
    }
    trace_length = 0; trace[0] = 0;
}
void p33_soft_reset(void)
{
    record("reset;"); ++fault_calls;
    if (stop_on_assert) longjmp(fault_return, 1);
    bredr_link_v.fcc_lc = &links[0];
}
void cpu_assert_debug(void)
{
    record("assert;"); ++fault_calls;
    if (stop_on_assert) longjmp(fault_return, 1);
    bredr_link_v.fcc_lc = &links[0];
}
struct link_connection *get_link_connection(void)
{ record("link;"); return missing_link ? NULL : &links[0]; }
struct ctrl_frame *bredr_bd_get_frame(void)
{ record("frame;"); return missing_frame ? NULL : &frame; }
struct bredr_tx_bulk *bredr_tx_bulk_alloc(int size)
{
    CHECK(size == 768 && links[0].frame == &frame); record("alloc768;");
    if (mutate_links) bredr_link_v.fcc_lc = &links[1];
    return &tx.bulk;
}
void *lmp_malloc(int size)
{
    CHECK(size == 36 && bredr_link_v.fcc_lc->tx_bulk[0] == &tx.bulk);
    record("malloc36;"); return fail_malloc ? NULL : &dut;
}
int link_conn_set_rx_bulk_in_irq(struct link_connection *lc, int index)
{
    CHECK(lc == (mutate_links ? &links[index + 1] : &links[0]));
    record("rx%d;", index);
    if (mutate_links && index == 0) bredr_link_v.fcc_lc = &links[2];
    return -1; /* The SDK ignores the allocation result. */
}
void bredr_frame_set_irq_handler(struct ctrl_frame *f, void *priv, void (*handler)(void *))
{
    CHECK(f == &frame && priv == (mutate_links ? &links[2] : &links[0]) && !handler);
    record("irq;");
}
const u16 *dut_get_packet_max_len(int packet, u8 group)
{ packet_seen = packet; group_seen = group; record("length;"); return &packet_length; }
void bredr_bd_frame_disable(struct ctrl_frame *f)
{ CHECK(f == &frame && frame.optcntl == 0x2200); record("disable;"); }
int bredr_pwr_set(struct link_connection *lc, char sel, char inc)
{
    CHECK(lc == &links[0] && !sel && frame.optcntl == 0x2200);
    power_seen = inc; record("power;"); return -1;
}
void bt_set_single_carrier(void)
{
    CHECK(frame.optcntl == 0x2201 && test_payload()[0] == 0xff && test_payload()[182] == 0xff);
    CHECK(test_payload()[183] == 0xa5); record("carrier;");
}
void bt_reload_trim(void)
{ CHECK(frame.optcntl == 0x2201 && test_payload()[0] == 0xa5); record("trim;"); }
void bredr_freq_table_init(u8 *table, u8 mode)
{ CHECK(table == (u8 *)&frame.freq_table[0][0]); record("freq%d;", mode); }
void __write_reg_clkoffset(struct ctrl_frame *f, u32 offset)
{ CHECK(f == &frame && !offset); record("clock;"); }
void __write_reg_bdaddr(struct ctrl_frame *f, const u8 *address)
{
    static const u8 want[] = {0x33, 0x8b, 0x9e, 0, 0, 0};
    CHECK(f == &frame && !memcmp(address, want, 6)); record("address;");
}
void __write_reg_bch(struct ctrl_frame *f, const u8 *bch)
{
    static const u8 want[] = {0xe2, 0x3a, 0x1a, 0x33, 2};
    CHECK(f == &frame && !memcmp(bch, want, 5)); record("bch;");
}
void __write_reg_txinfo(struct ctrl_frame *f, int index, void *data,
                       int length, int llid, int flow, int packet)
{
    CHECK(f == &frame && data == tx.bulk.data && length == 183 && llid == 2 && flow == 1);
    CHECK(packet == packet_seen && frame.linkcntl0 == 0xf02f && frame.linkcntl1 == 0x8000);
    record("tx%d;", index);
}
void __write_reg_format(struct ctrl_frame *f, u8 train, u8 format)
{
    CHECK(f == &frame && !train); format_seen = format;
    frame.format = 0xbee0; /* A callback's format update must survive the final bit change. */
    record("format;");
}
void wind_align_set(int size)
{ CHECK(size == 103 && !(frame.miscntl & 4)); record("align;"); }
void windset(struct ctrl_frame *f, u16 normal, u16 wide, u8 enable)
{
    CHECK(f == &frame && normal == 103 && !wide && !enable); record("window;");
    if (mutate_frequencies) { dut.tx_fre = 3; dut.rx_fre = 20; }
}
void bt_pll_reset(u8 mode) { record("pll%d;", mode); }
void bredr_bd_frame_enable(struct ctrl_frame *f, u8 flag, u8 enable)
{ CHECK(f == &frame && !flag && enable == 1); record("enable;"); }

static void reset(void)
{
    memset(&bredr_link_v, 0, sizeof(bredr_link_v));
    memset(links, 0, sizeof(links)); memset(&frame, 0, sizeof(frame));
    memset(&dut, 0, sizeof(dut)); memset(&tx, 0xa5, sizeof(tx));
    memset(registers, 0, sizeof(registers));
    g_dut_hdl = &dut; CONFIG_TEST_FCC_CODE = 1; config_asser = 0;
    trace_length = 0; trace[0] = 0;
    packet_seen = group_seen = power_seen = format_seen = -1;
    mutate_links = mutate_frequencies = missing_link = missing_frame = fail_malloc = stop_on_assert = fault_calls = 0;
    links[0].frame = &frame; links[0].tx_bulk[0] = &tx.bulk;
    frame.miscntl = 0xa5fc; frame.linkcntl0 = 0xf03f; frame.linkcntl1 = 0x8010;
    frame.optcntl = 0x2200; frame.format = 0xfefe;
    registers[0] = 0x89abcdef; registers[1] = 0x765432ff;
    registers[2] = 0x123456ff; registers[3] = 0xeeeeeeee; registers[4] = 0xffffffff;
    registers[5] = 0xabcdefff; registers[6] = 0x87654321;
}
/* Literal trace fragments follow original BD19 IR order, not BR23 expectations. */
#define PREFIX "length;r001c=89abcdef;w001c=89abcd00;r0108=765432ff;w0108=76543200;disable;power;"
#define CONTINUOUS "r0100=123456ff;w0100=123456f7;carrier;freq0;"
#define MODULATED "trim;r0100=123456ff;w0100=123456ff;r0100=123456ff;w0100=123456fb;freq1;"
#define COMMON "clock;address;bch;tx0;tx1;format;w0110=00000000;w0114=00000000;align;window;r0008=abcdefff;w0008=abcde0bc;"
#define FIXED21 "w011c=00000000;r011c=00000000;w011c=00000032;r011c=00000032;w011c=00001932;"

int main(void)
{
    reset(); CONFIG_TEST_FCC_CODE = 0;
    bredr_fcc_init(16, 21); bredr_fcc_set_param(NULL); expect("");
    CONFIG_TEST_FCC_CODE = 1; bredr_fcc_set_param(NULL); expect("");

    reset(); bredr_link_v.fcc_lc = &links[0];
    u8 parameters[] = {1, 21, 3, 1, 0xf1, 1};
    bredr_fcc_set_param(parameters);
    expect(PREFIX CONTINUOUS COMMON "pll0;");
    CHECK(packet_seen == 4 && group_seen == 2 && power_seen == -15 && format_seen == 3);
    CHECK(frame.miscntl == 0xa5f8 && frame.optcntl == 0x2201 && frame.format == 0xbe60);
    CHECK(registers[6] == 0x87654321 && dut.packet_len == 183 && tx.bulk.packet == 4);

    reset(); bredr_link_v.fcc_lc = &links[0];
    parameters[0] = 0; parameters[2] = 0; parameters[3] = 0; parameters[5] = 0;
    bredr_fcc_set_param(parameters);
    expect(PREFIX MODULATED COMMON FIXED21 "pll1;enable;");
    CHECK(frame.miscntl == 0xa5f0 && frame.format == 0xbee0 && format_seen == 2);

    /* Every valid index must preserve upper bits and use the index-based EDR bit. */
    static const u8 packets[] = {4, 11, 15, 4, 10, 14};
    for (unsigned i = 0; i < 6; ++i) {
        reset(); bredr_link_v.fcc_lc = &links[0];
        parameters[2] = i; bredr_fcc_set_param(parameters);
        CHECK(packet_seen == packets[i] && group_seen == (i >= 3 ? 2 : 0));
        CHECK(frame.miscntl == (i >= 3 ? 0xa5f8 : 0xa5f0));
    }
    for (unsigned channel = 0; channel < 256; ++channel) {
        reset(); bredr_link_v.fcc_lc = &links[0]; parameters[1] = channel;
        bredr_fcc_set_param(parameters);
        unsigned encoded = channel / 2 + (channel & 1 ? 40 : 0);
        CHECK(registers[6] == (encoded | (encoded << 7)));
    }

    reset(); bredr_link_v.fcc_lc = &links[0]; mutate_frequencies = 1;
    parameters[2] = 0; bredr_fcc_set_param(parameters);
    CHECK(registers[6] == 0x529); /* TX encoded in low bits; RX shifted seven. */

    /* Creation reloads the global link after allocations and each RX callback. */
    reset(); mutate_links = 1; fcc_creat_link();
    expect("link;frame;alloc768;malloc36;rx0;rx1;irq;");
    CHECK(links[0].frame == &frame && links[0].tx_bulk[0] == &tx.bulk);
    CHECK(links[1].tx_bulk[0] == &tx.bulk && bredr_link_v.fcc_lc == &links[2]);
    CHECK(g_dut_hdl == &dut);
    fcc_creat_link(); expect("");
    reset(); config_asser = 1; fcc_creat_link();
    expect("link;frame;alloc768;malloc36;rx0;rx1;irq;");
    reset(); fail_malloc = 1; fcc_creat_link();
    expect("link;frame;alloc768;malloc36;rx0;rx1;irq;"); CHECK(!g_dut_hdl);

    /* Link failure hooks repair the global to let us check SDK fall-through. */
    for (int assertions = 0; assertions < 2; ++assertions) {
        reset(); config_asser = assertions; missing_link = 1; fcc_creat_link();
        expect(assertions ? "link;assert;frame;alloc768;malloc36;rx0;rx1;irq;" :
                            "link;reset;frame;alloc768;malloc36;rx0;rx1;irq;");
        CHECK(fault_calls == 1);
        reset(); config_asser = assertions; missing_frame = stop_on_assert = 1;
        if (!setjmp(fault_return)) { fcc_creat_link(); CHECK(0); }
        expect(assertions ? "link;frame;assert;" : "link;frame;reset;");
        CHECK(fault_calls == 1);
    }

    reset(); bredr_fcc_init(16, 21);
    expect("link;frame;alloc768;malloc36;rx0;rx1;irq;"
           "r0100=123456ff;w0100=123456ef;" PREFIX
           "r0100=123456ef;w0100=123456e7;carrier;freq0;" COMMON FIXED21
           "pll1;enable;r0100=123456e7;w0100=123456f7;");
    CHECK(registers[2] == 0x123456f7 && power_seen == 15);
    reset(); bredr_link_v.fcc_lc = &links[0]; bredr_fcc_init(4, 21);
    expect(PREFIX CONTINUOUS COMMON FIXED21 "pll1;enable;");
    return TEST_DONE("btctrler_bredr_fcc_bd19");
}
#endif
