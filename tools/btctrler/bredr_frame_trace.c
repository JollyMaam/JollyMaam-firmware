/* Freestanding i386 differential fixture: no libc or 32-bit sysroot required.
 * The SDK and recovered q32s LLVM execute against the SAME 32-bit ABI buffers.
 * Register and volatile frame accesses record ordered address/value events.
 * External services are deterministic mocks; callback addresses normalize by
 * function name. Six callback scenarios cover stable services, callback pointer
 * changes, task-state changes, IRQ-enable frame replacement, feature-off/DUT,
 * and a CRC failure. This models behavior, not hardware timing or concurrency.
 *
 * Buffer offsets come from SDK IR and sdk_bd19.h target layout assertions.
 * The 94 helper cases use valid indices, nonzero divisors and successful heap
 * allocation. The SDK's allocation-failure invalid write is NOT executed.
 * Entry stack alignment is explicit because Linux enters _start without a
 * normal C call frame. Syscall EAX outputs must be declared to the compiler.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;
#define ALIGN __attribute__((aligned(16)))
u8 bd_base[4096] ALIGN, rf[4096] ALIGN, hci_param[4096] ALIGN, gbredr_local_dev[4096] ALIGN,
    UPDATA_BREDR_BASE_BEG[4096] ALIGN;
u32 CONFIG_BTCTLER_TWS_ENABLE = 1, CONFIG_TEST_DUT_CODE = 1, config_asser = 1, g_dut_hdl;
static u8 page[256] ALIGN, scan[256] ALIGN, inq[256] ALIGN, link[512] ALIGN, frame[640] ALIGN,
    frame2[640] ALIGN, fhs[256] ALIGN;
static u32 regs[1024], trace[4096], nt, caseid, checks, mode, crc_result, mutation, mutation_once;
void *memset(void *p, int x, u32 n) {
    u8 *q = p;
    while (n--)
        *q++ = (u8)x;
    return p;
}
void *memcpy(void *p, const void *s, u32 n) {
    u8 *q = p;
    const u8 *r = s;
    while (n--)
        *q++ = *r++;
    return p;
}
static void output(const char *p, u32 n) {
    u32 result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(4), "b"(1), "c"(p), "d"(n) : "memory");
}
static void number(u32 n) {
    char b[16];
    u32 k = 0;
    do {
        b[k++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (k)
        output(&b[--k], 1);
    output(" ", 1);
}
static void die(u32 x, u32 y, u32 z) {
    output("FAIL ", 5);
    number(caseid);
    number(x);
    number(y);
    number(z);
    output("\n", 1);
    __asm__ volatile("int $0x80" ::"a"(1), "b"(1));
    __builtin_unreachable();
}
void abort(void) { die(999, 0, 0); }
#define CALLBACKS(X)                                                                               \
    X(page_rx_handler)                                                                             \
    X(page_scan_rx_handler)                                                                        \
    X(inquiry_rx_handler) X(role_switch_page_scan_rx_handler) X(quick_page_step2)                  \
        X(quick_page_scan_step2) X(page_timeout) X(role_switch_page_scan_timeout)
#define DECL(n)                                                                                    \
    extern void sdk_##n(void *);                                                                   \
    extern void owned_##n(void *);
CALLBACKS(DECL)
extern void sdk_bredr_rx_irq_handler(void), owned_bredr_rx_irq_handler(void);
static void sdk_irq_model(void *p) {
    (void)p;
    sdk_bredr_rx_irq_handler();
}
static void owned_irq_model(void *p) {
    (void)p;
    owned_bredr_rx_irq_handler();
}
extern void sdk_bredr_frame_page_resume(void *), owned_bredr_frame_page_resume(void *),
    sdk_bredr_frame_page_scan_resume(void *), owned_bredr_frame_page_scan_resume(void *),
    sdk_bredr_frame_conn_rx_irq_handler(void *), owned_bredr_frame_conn_rx_irq_handler(void *);
static u32 normal(u32 p) {
    u32 id = 0;
#define NORM(n)                                                                                    \
    ++id;                                                                                          \
    if (p == (u32)sdk_##n || p == (u32)owned_##n)                                                  \
        return 0xf0000000 + id;
    CALLBACKS(NORM)
    NORM(bredr_rx_irq_handler)
    return p;
}
static void event(u32 x) {
    if (nt == 4096)
        abort();
    trace[nt++] = normal(x);
}
static u32 *reg(u32 a) {
    if (a == 1899588)
        return &regs[1024 - 1];
    if (a == 1899584)
        return &regs[1024 - 2];
    if (a == 1982724)
        return &regs[1024 - 3];
    if (a < 1835008 || a >= 1835008 + 4096 || a % 4)
        die(998, a, 0);
    return &regs[(a - 1835008) / 4];
}
u16 model_read16(u16 *p) {
    u16 v = *p;
    event(16);
    event((u32)p);
    event(v);
    return v;
}
u32 model_read32(u32 *p) {
    u32 a = (u32)p;
    u32 v = a < 0x400000 ? *reg(a) : *p;
    event(32);
    event(a);
    event(v);
    return v;
}
void model_write16(u16 *p, u16 v) {
    event(116);
    event((u32)p);
    event(v);
    *p = v;
}
void model_write32(u32 *p, u32 v) {
    u32 a = (u32)p;
    event(132);
    event(a);
    event(v);
    if (a < 0x400000)
        *reg(a) = v;
    else
        *p = v;
}
int printf(const char *fmt, ...) {
    event(200);
    for (const char *p = fmt; *p; p++)
        event((u8)*p);
    __builtin_va_list va;
    __builtin_va_start(va, fmt);
    for (const char *p = fmt; *p; p++)
        if (*p == '%' && p[1]) {
            ++p;
            if (*p == 's') {
                const char *s = __builtin_va_arg(va, const char *);
                while (*s)
                    event((u8)*s++);
            } else
                event(__builtin_va_arg(va, u32));
        }
    __builtin_va_end(va);
    return 0;
}
int puts(const char *p) {
    event(201);
    while (*p)
        event((u8)*p++);
    return 0;
}
int putchar(int c) {
    event(202);
    event(c);
    if (mutation == 1) {
        *(u32 *)(page + 164) = 0;
        *(u32 *)(page + 160) = (u32)frame2;
    }
    return c;
}
u8 crc8(u8 *p, u16 n) {
    event(203);
    event((u32)p);
    event(n);
    return (u8)crc_result, mutation, mutation_once;
}
int tws_host_channel_match(signed char c) {
    event(204);
    event(c);
    return 1;
}
void __get_access_addr(u8 *d, u8 *s) {
    event(205);
    event((u32)d);
    event((u32)s);
    for (int i = 0; i < 5; i++)
        d[i] = s[i] ^ 0x5a;
}
void __read_fhs_packet(void *d, void *s) {
    event(206);
    event((u32)d);
    event((u32)s);
}
void link_task_try_run_slots(void *p, int n) {
    event(207);
    event((u32)p);
    event(n);
    if (mutation == 2) {
        page[4] = 3;
        scan[1] = 2;
    }
}
void slot_timer_reset(u8 t, int n) {
    event(208);
    event(t);
    event(n);
}
void slot_timer_disable(u8 t) {
    event(209);
    event(t);
}
void *create_link_connection(void *f, void *r, u8 m, u8 q) {
    event(210);
    event((u32)f);
    event((u32)r);
    event(m);
    event(q);
    return link;
}
void slot_timer_set(u8 t, void (*fn)(void *), void *p, int n) {
    event(211);
    event(t);
    event((u32)fn);
    event((u32)p);
    event(n);
}
void slot_timer_set_finecnt(u8 t, void (*fn)(void *), void *p, int n, signed short fine) {
    event(212);
    event(t);
    event((u32)fn);
    event((u32)p);
    event(n);
    event(fine);
}
void local_irq_disable(void) { event(213); }
void local_irq_enable(void) {
    event(214);
    if (mutation == 3 && !mutation_once++) {
        *(u32 *)(link + 208) = (u32)frame2;
    }
}
void link_page_scan_timeout(void *p) {
    event(215);
    event((u32)p);
}
int get_esco_conn_bitoff(u16 *p) {
    event(216);
    *p = (u16)(mode % 1250);
    return mode & 1;
}
void link_inquiry_rx_packet_handler(int n) {
    event(217);
    event(n);
}
void dut_rx_handler(void *p) {
    event(218);
    event((u32)p);
}
void tws_slave_plink_premaxval_settings(void *p) {
    event(219);
    event((u32)p);
}
void link_conn_rx_handler(void *p, void *h, int n) {
    event(220);
    event((u32)p);
    event(n);
    u8 *b = h;
    event(b[0]);
    event(*(u16 *)(b + 6));
    event(b[10]);
    event(*(u16 *)(b + 16));
}
void link_role_switch_page_scan_completed(void *p, int n) {
    event(221);
    event((u32)p);
    event(n);
}
u8 slot_timer_get(void) {
    event(222);
    return 3;
}
void link_page_timeout(void *p) {
    event(223);
    event((u32)p);
}
int bredr_clkn_after(u32 a, u32 b) {
    event(224);
    event(a);
    event(b);
    return (s32)(a - b) > 0;
}
void bredr_link_head_err_irq_handler(void *p) {
    event(225);
    event((u32)p);
}
void bredr_link_esco_rx_irq_handler(int n) {
    event(226);
    event(n);
}
void tws_slave_rx_switch_conn_irq_handler(void *p) {
    event(227);
    event((u32)p);
}
static void irq_cb(void *p) {
    event(245);
    event((u32)p);
}
static void completed(void *p, int n) {
    event(228);
    event((u32)p);
    event(n);
    event(*(u32 *)(page + 160));
    event(*(u32 *)(page + 164));
    event(*(u16 *)(frame + 84));
    event(*(u16 *)(frame + 86));
    if (mutation == 2) {
        *(u16 *)(frame + 84) = 0;
        *(u16 *)(frame + 86) = 0;
    }
}
void request_irq(u8 a, u8 b, void *p, u8 c) {
    event(229);
    event(a);
    event(b);
    event((u32)p);
    event(c);
}
void unrequest_irq(u8 a) {
    event(230);
    event(a);
}
void delay(u32 n) {
    event(231);
    event(n);
}
void slot_timer_init(void) { event(232); }
void slot_timer_uninit(void) { event(233); }
void bredr_power_get(void) { event(234); }
void bredr_power_put(void) { event(235); }
void bredr_link_check_frame_used(void) { event(236); }
void bredr_normal_pwr_set(void *p, u8 n) {
    event(237);
    event((u32)p);
    event(n);
}
void cpu_assert_debug(void) { event(238); }
void p33_soft_reset(void) { event(239); }
void __BT_UPDATA_JUMP(u32 n) {
    event(240);
    event(n);
}
void bit_clr_ie(u8 n) {
    event(241);
    event(n);
}
void bit_set_ie(u8 n) {
    event(242);
    event(n);
}
static u8 pool[2048] ALIGN;
void *zalloc(u32 n) {
    event(243);
    event(n);
    memset(pool, 0, n);
    return pool;
}
void free(void *p) {
    event(244);
    event((u32)p);
}
static void setup(u32 state, u32 sub, u32 quick, u32 packet, u32 check, u32 toggle) {
    memset(fhs, 0, sizeof(fhs));
    memset(pool, 0, sizeof(pool));
    memset(UPDATA_BREDR_BASE_BEG, 0, 4096);
    memset(page, 0, sizeof(page));
    memset(scan, 0, sizeof(scan));
    memset(inq, 0, sizeof(inq));
    memset(link, 0, sizeof(link));
    memset(frame, 0, sizeof(frame));
    memset(frame2, 0, sizeof(frame2));
    memset(bd_base, 0, sizeof(bd_base));
    memset(regs, 0, sizeof(regs));
    memset(gbredr_local_dev, 0, 4096);
    nt = 0;
    mutation_once = 0;
    *(u32 *)(page + 160) = (u32)frame;
    *(u32 *)(page + 164) = (u32)completed;
    page[3] = state;
    page[4] = sub;
    page[1] = 5;
    page[10] = quick;
    page[9] = 3;
    *(u32 *)(page + 36) = 1;
    *(u32 *)(scan + 152) = (u32)frame;
    *(u32 *)(scan + 156) = (u32)fhs;
    *(u32 *)(scan + 160) = (u32)link;
    scan[0] = state;
    scan[1] = sub;
    scan[10] = quick;
    scan[9] = 3;
    *(u32 *)(scan + 140) = 1400;
    scan[118] = 4;
    scan[22] = sub;
    *(u32 *)(scan + 40) = quick ? 1 : 0;
    *(u32 *)(link + 208) = (u32)frame;
    link[43] = (state & 1) ? 8 : 0;
    link[65] = sub;
    link[98] = 7;
    link[102] = 9;
    *(u32 *)(inq + 128) = (u32)frame;
    *(u32 *)(inq + 8) = (u32)fhs;
    inq[1] = state;
    inq[2] = sub;
    inq[4] = 3;
    inq[149] = quick;
    *(u32 *)(inq + 172) = 12345;
    for (int i = 0; i < 8; i++)
        *(u32 *)(bd_base + 4 * i) = (u32)frame;
    *(u32 *)(frame + 576) = (u32)page;
    *(u32 *)(frame + 580) = quick ? (u32)irq_cb : 0;
    *(u16 *)(frame + 16) = toggle;
    *(u16 *)(frame + 48) = *(u16 *)(frame + 50) = check << 7;
    *(u16 *)(frame + 78) = *(u16 *)(frame + 80) = (13 << 3) | 3;
    *(u16 *)(frame + 246) = *(u16 *)(frame + 248) = packet << 3;
    *(u16 *)(frame + 70) = 1000;
    *(u16 *)(frame + 72) = 1;
    *(u16 *)(frame + 68) = 500;
    *(u16 *)(frame + 74) = 1000;
    *(u16 *)(frame + 76) = 1;
    *(u16 *)(frame + 84) = *(u16 *)(frame + 86) = 0;
    *(u16 *)(frame + 88) = *(u16 *)(frame + 90) = 1;
    *(u16 *)(frame + 566) = 1061;
    *(u32 *)(frame + 572) = 1000;
    frame[564] = 0;
    *reg(1835032) = packet | 0x1700;
    *reg(1835036) = sub | 0x1700;
    *reg(1835268) = packet;
    *reg(1835272) = sub;
    *reg(1835008) = 0;
    *reg(1835012) = 0;
    *reg(1835188) = 3000;
    *reg(1835192) = 1;
    *reg(1835196) = 7;
    *(u16 *)(frame2 + 566) = 0xabcd;
    *(u32 *)(frame2 + 572) = 0x87654321;
    g_dut_hdl = mutation == 4 ? (u32)frame : 0;
    CONFIG_BTCTLER_TWS_ENABLE = mutation == 4 ? 0 : 1;
    CONFIG_TEST_DUT_CODE = (sub & 1);
    crc_result = mutation == 5 ? 1 : 0;
    mode = caseid;
}
static u8 saved_base[4096], saved_updata[4096], saved_fhs[256], saved_frame2[640], saved_pool[2048],
    saved_inq[256], saved_page[256], saved_scan[256], saved_link[512], saved_frame[640];
static u32 saved_regs[1024], saved_trace[4096], saved_nt;
static void compare(const void *a, const void *b, u32 n, u32 tag) {
    const u8 *x = a, *y = b;
    for (u32 i = 0; i < n; i++) {
        if (!(i % 4) && i + 4 <= n &&
            normal(*(const u32 *)(x + i)) == normal(*(const u32 *)(y + i))) {
            i += 3;
            continue;
        }
        if (x[i] != y[i])
            die(tag * 10000 + i, x[i], y[i]);
    }
}
static void save(void) {
    memcpy(saved_base, bd_base, 4096);
    memcpy(saved_updata, UPDATA_BREDR_BASE_BEG, 4096);
    memcpy(saved_fhs, fhs, 256);
    memcpy(saved_frame2, frame2, 640);
    memcpy(saved_pool, pool, 2048);
    memcpy(saved_inq, inq, 256);
    memcpy(saved_page, page, 256);
    memcpy(saved_scan, scan, 256);
    memcpy(saved_link, link, 512);
    memcpy(saved_frame, frame, 640);
    memcpy(saved_regs, regs, sizeof(regs));
    memcpy(saved_trace, trace, sizeof(trace));
    saved_nt = nt;
}
static void check(void) {
    for (u32 i = 0; i < nt && i < saved_nt; i++)
        if (trace[i] != saved_trace[i])
            die(i + 100, saved_trace[i], trace[i]);
    if (nt != saved_nt)
        die(1, saved_nt, nt);
    compare(saved_base, bd_base, 4096, 7);
    compare(saved_updata, UPDATA_BREDR_BASE_BEG, 4096, 8);
    compare(saved_fhs, fhs, 256, 9);
    compare(saved_frame2, frame2, 640, 10);
    compare(saved_pool, pool, 2048, 11);
    compare(saved_inq, inq, 256, 6);
    compare(saved_page, page, 256, 1);
    compare(saved_scan, scan, 256, 2);
    compare(saved_link, link, 512, 3);
    compare(saved_frame, frame, 640, 4);
    compare(saved_regs, regs, sizeof(regs), 5);
    ++checks;
}
static void run(void (*a)(void *), void (*b)(void *), void *p) {
    for (u32 state = 0; state < 5; state++)
        for (u32 sub = 0; sub < 6; sub++)
            for (u32 quick = 0; quick < 2; quick++)
                for (u32 packet = 0; packet < 16; packet++)
                    for (u32 ck = 0; ck < 4; ck++)
                        for (u32 toggle = 0; toggle < 2; toggle++) {
                            u32 cv = ck == 0 ? 0 : ck == 1 ? 127 : ck == 2 ? 255 : 511;
                            ++caseid;
                            setup(state, sub, quick, packet, cv, toggle);
                            a(p);
                            save();
                            setup(state, sub, quick, packet, cv, toggle);
                            b(p);
                            check();
                        }
}

void sdk_bredr_frame_set_irq_handler(void *frame, void *priv, void (*handler)(void *));
void owned_bredr_frame_set_irq_handler(void *frame, void *priv, void (*handler)(void *));
u16 sdk___read_reg_packet_seqn(void *frame, int index);
u16 owned___read_reg_packet_seqn(void *frame, int index);
void sdk___write_reg_seqn(void *frame, int index, u8 seqn);
void owned___write_reg_seqn(void *frame, int index, u8 seqn);
void sdk___write_reg_rxint(void *frame, int index, u8 enable);
void owned___write_reg_rxint(void *frame, int index, u8 enable);
void sdk___write_reg_packet_type(void *frame, int index, int packet);
void owned___write_reg_packet_type(void *frame, int index, int packet);
void sdk___write_reg_txinfo_sync_train(void *frame, int index, void *data, int len, int llid,
                                       int seqn, int flow, int packet);
void owned___write_reg_txinfo_sync_train(void *frame, int index, void *data, int len, int llid,
                                         int seqn, int flow, int packet);
void sdk___write_reg_txinfo(void *frame, int index, void *data, int len, int llid, int flow,
                            int packet);
void owned___write_reg_txinfo(void *frame, int index, void *data, int len, int llid, int flow,
                              int packet);
u32 sdk_READ_SLOT_CLK();
u32 owned_READ_SLOT_CLK();
u32 sdk_READ_HALF_SLOT_CLK();
u32 owned_READ_HALF_SLOT_CLK();
u32 sdk_GET_FINETIMECNT();
u32 owned_GET_FINETIMECNT();
u32 sdk_READ_SLOT_CLK2();
u32 owned_READ_SLOT_CLK2();
void sdk___write_reg_bdaddr(void *frame, const u8 *addr);
void owned___write_reg_bdaddr(void *frame, const u8 *addr);
void sdk___write_reg_bch(void *frame, const u8 *bch);
void owned___write_reg_bch(void *frame, const u8 *bch);
void sdk___write_reg_clkoffset(void *frame, u32 clk_offset);
void owned___write_reg_clkoffset(void *frame, u32 clk_offset);
s32 sdk___read_reg_clkoffset(void *frame);
s32 owned___read_reg_clkoffset(void *frame);
void sdk___write_reg_txphdr(void *frame, int index, u16 val);
void owned___write_reg_txphdr(void *frame, int index, u16 val);
void sdk___write_reg_fullsta_packet(void *frame, int val);
void owned___write_reg_fullsta_packet(void *frame, int val);
void sdk___write_reg_linkcntl(void *frame, int index, int val);
void owned___write_reg_linkcntl(void *frame, int index, int val);
int sdk___read_reg_linkcntl(void *frame, int index);
int owned___read_reg_linkcntl(void *frame, int index);
void sdk_bredr_hw_write_lt_addr(void *frame, u8 lt_addr);
void owned_bredr_hw_write_lt_addr(void *frame, u8 lt_addr);
void sdk___write_reg_format(void *frame, u8 train, u8 format);
void owned___write_reg_format(void *frame, u8 train, u8 format);
void sdk__write_reg_esco_clkalign(void *frame, u8 en);
void owned__write_reg_esco_clkalign(void *frame, u8 en);
u32 sdk___read_reg_clkn(void *frame);
u32 owned___read_reg_clkn(void *frame);
void sdk___write_reg_escoportcntl(u8 channel, u8 T_sco);
void owned___write_reg_escoportcntl(u8 channel, u8 T_sco);
void sdk___write_reg_escoltcntl(u8 channel, u8 edr, u8 type, u8 lt_addr);
void owned___write_reg_escoltcntl(u8 channel, u8 edr, u8 type, u8 lt_addr);
void sdk___write_reg_escotrcntl(u8 channel, u8 type_m2s, u16 len_m2s, u8 type_s2m, u16 len_s2m);
void owned___write_reg_escotrcntl(u8 channel, u8 type_m2s, u16 len_m2s, u8 type_s2m, u16 len_s2m);
void sdk___pcm_out_disable(u8 channel);
void owned___pcm_out_disable(u8 channel);
void sdk___write_reg_encry_disable(void *frame);
void owned___write_reg_encry_disable(void *frame);
void sdk___write_reg_encry(void *frame, u8 rx_enc, u8 tx_enc);
void owned___write_reg_encry(void *frame, u8 rx_enc, u8 tx_enc);
void sdk___write_reg_lmprxptr(void *frame, int index, u8 *addr);
void owned___write_reg_lmprxptr(void *frame, int index, u8 *addr);
void sdk___clean_reg_rxfull(void *frame, int index);
void owned___clean_reg_rxfull(void *frame, int index);
void sdk___clean_reg_txempty(void *frame, int index);
void owned___clean_reg_txempty(void *frame, int index);
void sdk___set_reg_flow(void *frame, u8 en);
void owned___set_reg_flow(void *frame, u8 en);
void sdk___write_reg_rxptr(void *frame, int index, u8 *addr);
void owned___write_reg_rxptr(void *frame, int index, u8 *addr);
void sdk___write_reg_txericntl(int len, int type, void *addr);
void owned___write_reg_txericntl(int len, int type, void *addr);
void sdk___set_default_sco_rx_buf();
void owned___set_default_sco_rx_buf();
void sdk_wind_align_set(int winsz_normal);
void owned_wind_align_set(int winsz_normal);
void sdk_windset(void *frame, unsigned short winsz_normal, unsigned short winsz_wide,
                 unsigned char win_wide);
void owned_windset(void *frame, unsigned short winsz_normal, unsigned short winsz_wide,
                   unsigned char win_wide);
void sdk_bredr_frame_inquiry_scan_init(void *inquiry_scan, void *eir);
void owned_bredr_frame_inquiry_scan_init(void *inquiry_scan, void *eir);
void sdk_bredr_frame_page_scan_rx_config(void *page_scan);
void owned_bredr_frame_page_scan_rx_config(void *page_scan);
int sdk___check_tws_info(void *info);
int owned___check_tws_info(void *info);
void sdk_page_scan_rx_handler(void *priv);
void owned_page_scan_rx_handler(void *priv);
void sdk_bredr_frame_page_scan_resume(void *page_scan);
void owned_bredr_frame_page_scan_resume(void *page_scan);
void sdk_bredr_frame_page_scan_init(void *page_scan);
void owned_bredr_frame_page_scan_init(void *page_scan);
void sdk_inquiry_rx_handler(void *priv);
void owned_inquiry_rx_handler(void *priv);
void sdk_bredr_frame_inquiry_init(void *inquiry);
void owned_bredr_frame_inquiry_init(void *inquiry);
void sdk_bredr_frame_page_resume(void *page);
void owned_bredr_frame_page_resume(void *page);
void sdk_bredr_frame_page_init(void *page);
void owned_bredr_frame_page_init(void *page);
void sdk_bredr_frame_get_empty_txbuf_index(void *frame, u8 *update_index);
void owned_bredr_frame_get_empty_txbuf_index(void *frame, u8 *update_index);
void sdk_bredr_frame_get_rx_payload_head(void *frame, void *head);
void owned_bredr_frame_get_rx_payload_head(void *frame, void *head);
int sdk_audio_sync_lat_hopclkn(void *frame);
int owned_audio_sync_lat_hopclkn(void *frame);
void sdk_bredr_frame_conn_rx_irq_handler(void *_link);
void owned_bredr_frame_conn_rx_irq_handler(void *_link);
void sdk_role_switch_page_scan_rx_handler(void *priv);
void owned_role_switch_page_scan_rx_handler(void *priv);
void sdk_bredr_frame_rs_page_scan_enable(void *rs_page_scan);
void owned_bredr_frame_rs_page_scan_enable(void *rs_page_scan);
unsigned char sdk_clkn0_phase(short bitphase);
unsigned char owned_clkn0_phase(short bitphase);
void sdk_preserve_bitoff_cal(void *frame, u32 bitoff_reg, u32 *preserve_clknoff,
                             unsigned short *preserve_bitoff, unsigned short preserve_time);
void owned_preserve_bitoff_cal(void *frame, u32 bitoff_reg, u32 *preserve_clknoff,
                               unsigned short *preserve_bitoff, unsigned short preserve_time);
long unsigned int sdk_get_mclkn_to_local_clkn(long unsigned int soft_clknoff,
                                              unsigned short soft_bitoff, u32 *local_clkn,
                                              u8 read_local_clkn);
long unsigned int owned_get_mclkn_to_local_clkn(long unsigned int soft_clknoff,
                                                unsigned short soft_bitoff, u32 *local_clkn,
                                                u8 read_local_clkn);
u32 sdk_bredr_get_anchor_point_usec(u16 D_offset, u16 interval);
u32 owned_bredr_get_anchor_point_usec(u16 D_offset, u16 interval);
long unsigned int sdk_get_mclkn(long unsigned int soft_clknoff, unsigned short soft_bitoff,
                                u32 *local_clkn);
long unsigned int owned_get_mclkn(long unsigned int soft_clknoff, unsigned short soft_bitoff,
                                  u32 *local_clkn);
void sdk_bredr_set_frame_bitoff(void *frame, u16 bitoff);
void owned_bredr_set_frame_bitoff(void *frame, u16 bitoff);
void sdk___rxoffset_cal(void *frame, int target_clkn, int rxclkn, short rxbit, int enable_bitoff);
void owned___rxoffset_cal(void *frame, int target_clkn, int rxclkn, short rxbit, int enable_bitoff);
void sdk_rxoffset_cal(void *frame, int enable_bitoff);
void owned_rxoffset_cal(void *frame, int enable_bitoff);
void sdk_m2s_offset_cal(void *frame);
void owned_m2s_offset_cal(void *frame);
void sdk_s2m_offset_cal(void *frame, long int target_clkn, long int rxclkn, short rxbit);
void owned_s2m_offset_cal(void *frame, long int target_clkn, long int rxclkn, short rxbit);
void sdk_s_sniffer_offset_cal_start(void *forward_frame, void *sniffer_frame, u32 *sniffer_clkoff,
                                    u16 *sniffer_bitoff, u32 forward_clkoff, u16 forward_bitoff);
void owned_s_sniffer_offset_cal_start(void *forward_frame, void *sniffer_frame, u32 *sniffer_clkoff,
                                      u16 *sniffer_bitoff, u32 forward_clkoff, u16 forward_bitoff);
void sdk_s_sniffer_offset_cal(void *frame, u16 bitoff);
void owned_s_sniffer_offset_cal(void *frame, u16 bitoff);
void sdk_s_forward_offset_cal(void *frame, u32 *forward_clkoff, u16 *forward_bitoff,
                              u16 sniffer_bitoff, u8 cal_mode);
void owned_s_forward_offset_cal(void *frame, u32 *forward_clkoff, u16 *forward_bitoff,
                                u16 sniffer_bitoff, u8 cal_mode);
void sdk_tws_forward_offset_cal(void *frame, u32 *forward_clkoff, u16 *forward_bitoff,
                                u16 sniffer_bitoff, u8 cal_mode);
void owned_tws_forward_offset_cal(void *frame, u32 *forward_clkoff, u16 *forward_bitoff,
                                  u16 sniffer_bitoff, u8 cal_mode);
void sdk_m_tws_offset_cal_start(void *frame, u16 bitoff, u8 cal_mode);
void owned_m_tws_offset_cal_start(void *frame, u16 bitoff, u8 cal_mode);
void sdk_esco_tws_offset_cal_start(void *tws_frame, u16 tws_bitoff);
void owned_esco_tws_offset_cal_start(void *tws_frame, u16 tws_bitoff);
void sdk_m_tws_offset_cal(void *frame, u16 bitoff, u8 cal_mode);
void owned_m_tws_offset_cal(void *frame, u16 bitoff, u8 cal_mode);
void sdk_bredr_radiopwrupdn_reset();
void owned_bredr_radiopwrupdn_reset();
void sdk_bredr_freq_table_init(u8 *freq_table, u8 mode);
void owned_bredr_freq_table_init(u8 *freq_table, u8 mode);
void sdk_bredr_audio_sync_lat_con_config(void *frame);
void owned_bredr_audio_sync_lat_con_config(void *frame);
u8 sdk_get_bredr_is_init();
u8 owned_get_bredr_is_init();
void sdk_bredr_bd_init();
void owned_bredr_bd_init();
void sdk_bredr_bd_close();
void owned_bredr_bd_close();
void sdk_bd_frame_odd_even(void *frame, u8 flag);
void owned_bd_frame_odd_even(void *frame, u8 flag);
void sdk_exchg_table_prf(void);
void owned_exchg_table_prf(void);
void sdk_bredr_bd_close_frame(void);
void owned_bredr_bd_close_frame(void);
void sdk_bredr_bd_open_frame(void);
void owned_bredr_bd_open_frame(void);
void sdk_bredr_frame_suspend_control(u8 en);
void owned_bredr_frame_suspend_control(u8 en);
void sdk_bredr_bd_frame_enable(void *frame, u8 flag, u8 set_en);
void owned_bredr_bd_frame_enable(void *frame, u8 flag, u8 set_en);
void sdk_bredr_bd_frame_disable(void *frame);
void owned_bredr_bd_frame_disable(void *frame);
void sdk_bredr_bd_frame_dump();
void owned_bredr_bd_frame_dump();
void sdk_bd_set_prio(void *frame, u8 prio, u8 step);
void owned_bd_set_prio(void *frame, u8 prio, u8 step);
void *sdk_bredr_bd_get_frame();
void *owned_bredr_bd_get_frame();
void sdk_bredr_pll_comp_reset(void *frame);
void owned_bredr_pll_comp_reset(void *frame);
void sdk_bredr_bd_put_frame(void *frame);
void owned_bredr_bd_put_frame(void *frame);
void sdk_bdhw_set_frq_table(u8 *frq_table, u8 *channel_map, u8 *out_map);
void owned_bdhw_set_frq_table(u8 *frq_table, u8 *channel_map, u8 *out_map);
void sdk_bdhw_disable_afh(void *frame);
void owned_bdhw_disable_afh(void *frame);
void sdk_bdhw_set_afh(void *frame, u8 index, u8 *channel_map);
void owned_bdhw_set_afh(void *frame, u8 index, u8 *channel_map);
void sdk_bredr_frame_role_switch_2_slave(void *frame);
void owned_bredr_frame_role_switch_2_slave(void *frame);
void sdk_bredr_frame_role_switch_2_master(void *frame);
void owned_bredr_frame_role_switch_2_master(void *frame);
int sdk_bredr_frame_agc_set(void *frame, char sel, char step);
int owned_bredr_frame_agc_set(void *frame, char sel, char step);
char sdk_bredr_analog_get_rssi(void *frame);
char owned_bredr_analog_get_rssi(void *frame);
int sdk_bredr_frame_get_agc(void *frame);
int owned_bredr_frame_get_agc(void *frame);
void sdk_tws_monitor_link_txpwr_cpy(void *frame, void *tws_frame);
void owned_tws_monitor_link_txpwr_cpy(void *frame, void *tws_frame);
void sdk_tws_link_monitor_slave_enable(void *frame);
void owned_tws_link_monitor_slave_enable(void *frame);
unsigned char sdk_sync_generation(long unsigned int sword);
unsigned char owned_sync_generation(long unsigned int sword);
long unsigned int sdk_acc_rand_1(long unsigned int seed);
long unsigned int owned_acc_rand_1(long unsigned int seed);
long unsigned int sdk_tws_acc_generation(u16 addr);
long unsigned int owned_tws_acc_generation(u16 addr);
void sdk___bt_updata_save_curr_used_frame(void *frame, void *updata_info);
void owned___bt_updata_save_curr_used_frame(void *frame, void *updata_info);
void *sdk___bt_updata_bd_get_frame_rusume();
void *owned___bt_updata_bd_get_frame_rusume();
void sdk_delay_2slot_rise(u32 cnt);
void owned_delay_2slot_rise(u32 cnt);
u8 sdk_check_update_param_len(void);
u8 owned_check_update_param_len(void);
void sdk___bt_updata_reset_bt_bredrexm_addr(void);
void owned___bt_updata_reset_bt_bredrexm_addr(void);
void sdk___bt_updata_resume_bredr_rx_irq_handler();
void owned___bt_updata_resume_bredr_rx_irq_handler();
u8 sdk_radio_get_exchg_table();
u8 owned_radio_get_exchg_table();
void sdk_dut_bredr_close_recall();
void owned_dut_bredr_close_recall();
void sdk_dut_bredr_init_recall();
void owned_dut_bredr_init_recall();
static u32 offset_bit, offset_mode, offset_kind;
static void offset_run(int own) {
    switch (offset_kind) {
    case 0:
        if (own)
            owned_m2s_offset_cal(frame);
        else
            sdk_m2s_offset_cal(frame);
        break;
    case 1:
        if (own)
            owned_s2m_offset_cal(frame, 100000, 70000, (signed short)offset_bit);
        else
            sdk_s2m_offset_cal(frame, 100000, 70000, (signed short)offset_bit);
        break;
    case 2:
        if (own)
            owned___rxoffset_cal(frame, 100000, 70000, (signed short)offset_bit, offset_mode);
        else
            sdk___rxoffset_cal(frame, 100000, 70000, (signed short)offset_bit, offset_mode);
        break;
    case 3:
        if (own)
            owned_m_tws_offset_cal(frame, offset_bit, offset_mode);
        else
            sdk_m_tws_offset_cal(frame, offset_bit, offset_mode);
        break;
    case 4:
        if (own)
            owned_m_tws_offset_cal_start(frame, offset_bit, offset_mode);
        else
            sdk_m_tws_offset_cal_start(frame, offset_bit, offset_mode);
        break;
    case 5:
        if (own)
            owned_s_sniffer_offset_cal(frame, offset_bit);
        else
            sdk_s_sniffer_offset_cal(frame, offset_bit);
        break;
    }
}
static void offsets(void) {
    for (offset_kind = 0; offset_kind < 6; offset_kind++)
        for (offset_bit = 0; offset_bit < 65536; offset_bit++)
            for (offset_mode = 0; offset_mode < 3; offset_mode++) {
                ++caseid;
                setup(2, 1, 0, 2, 127, 1);
                *(u16 *)(frame + 566) = offset_bit;
                *(u32 *)(frame + 572) = 0xdeadbeef;
                offset_run(0);
                save();
                setup(2, 1, 0, 2, 127, 1);
                *(u16 *)(frame + 566) = offset_bit;
                *(u32 *)(frame + 572) = 0xdeadbeef;
                offset_run(1);
                check();
            }
}

#define DO(name, ...)                                                                              \
    do {                                                                                           \
        if (own)                                                                                   \
            owned_##name(__VA_ARGS__);                                                             \
        else                                                                                       \
            sdk_##name(__VA_ARGS__);                                                               \
    } while (0)
#define RET(name, ...)                                                                             \
    do {                                                                                           \
        *(u32 *)(link + 500) =                                                                     \
            own ? (u32)owned_##name(__VA_ARGS__) : (u32)sdk_##name(__VA_ARGS__);                   \
    } while (0)
static void helpers(int own, u32 which, u32 n) {
    switch (which) {
    case 91:
        if (n & 1)
            *(u32 *)(bd_base + 4 * (n % 8)) = 0;
        RET(bredr_bd_get_frame);
        break;
    case 92:
        DO(bredr_bd_put_frame, frame);
        break;
    case 93:
        *(u32 *)(link + 300) = (u32)frame;
        fhs[1] = n & 1;
        fhs[2] = n;
        fhs[3] = n & 15;
        DO(bredr_frame_inquiry_scan_init, link, fhs);
        break;

    case 0:
        DO(bredr_frame_set_irq_handler, frame, page, irq_cb);
        break;
    case 1:
        RET(__read_reg_packet_seqn, frame, n & 1);
        break;
    case 2:
        DO(__write_reg_seqn, frame, n & 1, n);
        break;
    case 3:
        DO(__write_reg_rxint, frame, n & 1, n);
        break;
    case 4:
        DO(__write_reg_packet_type, frame, n & 1, n);
        break;
    case 5:
        DO(__write_reg_txinfo, frame, n & 1, fhs, n, n & 3, n & 1, n & 15);
        break;
    case 6:
        DO(__write_reg_txinfo_sync_train, frame, n & 1, fhs, n, 3, 1, 1, 15);
        break;
    case 7:
        RET(READ_SLOT_CLK);
        break;
    case 8:
        RET(READ_HALF_SLOT_CLK);
        break;
    case 9:
        RET(GET_FINETIMECNT);
        break;
    case 10:
        RET(READ_SLOT_CLK2);
        break;
    case 11:
        DO(__write_reg_bdaddr, frame, fhs);
        break;
    case 12:
        DO(__write_reg_bch, frame, fhs);
        break;
    case 13:
        DO(__write_reg_clkoffset, frame, n * 0x1234567u);
        break;
    case 14:
        RET(__read_reg_clkoffset, frame);
        break;
    case 15:
        DO(__write_reg_txphdr, frame, n & 1, n);
        break;
    case 16:
        DO(__write_reg_fullsta_packet, frame, n);
        break;
    case 17:
        DO(__write_reg_linkcntl, frame, n & 1, n);
        break;
    case 18:
        RET(__read_reg_linkcntl, frame, n & 1);
        break;
    case 19:
        DO(bredr_hw_write_lt_addr, frame, n);
        break;
    case 20:
        DO(__write_reg_format, frame, n, n >> 3);
        break;
    case 21:
        DO(_write_reg_esco_clkalign, frame, n);
        break;
    case 22:
        RET(__read_reg_clkn, frame);
        break;
    case 23:
        DO(__write_reg_escoportcntl, n & 3, n);
        break;
    case 24:
        DO(__write_reg_escoltcntl, n & 3, n, n, n);
        break;
    case 25:
        DO(__write_reg_escotrcntl, n & 3, n, n, n, n);
        break;
    case 26:
        DO(__pcm_out_disable, n & 3);
        break;
    case 27:
        DO(__write_reg_encry_disable, frame);
        break;
    case 28:
        DO(__write_reg_encry, frame, n, n >> 3);
        break;
    case 29:
        DO(__write_reg_lmprxptr, frame, n & 1, fhs);
        break;
    case 30:
        DO(__clean_reg_rxfull, frame, n & 1);
        break;
    case 31:
        DO(__clean_reg_txempty, frame, n & 1);
        break;
    case 32:
        DO(__set_reg_flow, frame, n);
        break;
    case 33:
        DO(__write_reg_rxptr, frame, n & 1, fhs);
        break;
    case 34:
        DO(__write_reg_txericntl, n, n, fhs);
        break;
    case 35:
        DO(__set_default_sco_rx_buf);
        break;
    case 36:
        DO(wind_align_set, n);
        break;
    case 37:
        DO(windset, frame, n, n * 7, n);
        break;
    case 38:
        DO(bredr_frame_page_scan_rx_config, scan);
        break;
    case 39:
        DO(bredr_frame_role_switch_2_master, frame);
        break;
    case 40:
        RET(__check_tws_info, fhs);
        break;
    case 41:
        RET(bredr_frame_agc_set, frame, n, n);
        break;
    case 42:
        RET(bredr_analog_get_rssi, frame);
        break;
    case 43:
        DO(bredr_frame_inquiry_init, inq);
        break;
    case 44:
        DO(bredr_frame_page_init, page);
        break;
    case 45:
        DO(bredr_frame_page_scan_init, scan);
        break;
    case 46:
        RET(audio_sync_lat_hopclkn, frame);
        break;
    case 47:
        DO(bredr_frame_rs_page_scan_enable, scan);
        break;
    case 48:
        DO(bredr_set_frame_bitoff, frame, n);
        break;
    case 49:
        DO(rxoffset_cal, frame, n & 1);
        break;
    case 50:
        DO(bredr_radiopwrupdn_reset);
        break;
    case 51:
        DO(bredr_freq_table_init, fhs, n);
        break;
    case 52:
        DO(bredr_audio_sync_lat_con_config, frame);
        break;
    case 53:
        RET(get_bredr_is_init);
        break;
    case 54:
        DO(bredr_bd_init);
        break;
    case 55:
        DO(bredr_bd_close);
        break;
    case 56:
        DO(bd_frame_odd_even, frame, n);
        break;
    case 57:
        DO(exchg_table_prf);
        break;
    case 58:
        DO(bredr_bd_close_frame);
        break;
    case 59:
        DO(bredr_bd_open_frame);
        break;
    case 60:
        DO(bredr_frame_suspend_control, n);
        break;
    case 61:
        DO(bredr_bd_frame_enable, frame, n, n);
        break;
    case 62:
        DO(bredr_bd_frame_disable, frame);
        break;
    case 63:
        DO(bredr_bd_frame_dump);
        break;
    case 64:
        DO(bd_set_prio, frame, n, n);
        break;
    case 65:
        DO(bredr_pll_comp_reset, frame);
        break;
    case 66:
        DO(bdhw_disable_afh, frame);
        break;
    case 67:
        DO(bdhw_set_afh, frame, n % 3, fhs);
        break;
    case 68:
        DO(bredr_frame_role_switch_2_slave, frame);
        break;
    case 69:
        RET(bredr_frame_get_agc, frame);
        break;
    case 70:
        DO(tws_monitor_link_txpwr_cpy, frame, frame2);
        break;
    case 71:
        DO(tws_link_monitor_slave_enable, frame);
        break;
    case 72:
        DO(delay_2slot_rise, n);
        break;
    case 73:
        RET(check_update_param_len);
        break;
    case 74:
        RET(__bt_updata_bd_get_frame_rusume);
        break;
    case 75:
        DO(__bt_updata_resume_bredr_rx_irq_handler);
        break;
    case 76:
        RET(radio_get_exchg_table);
        break;
    case 77:
        DO(dut_bredr_close_recall);
        break;
    case 78:
        DO(dut_bredr_init_recall);
        break;
    case 79:
        DO(__bt_updata_save_curr_used_frame, frame, link + (n & 3));
        break;
    case 80:
        DO(__bt_updata_reset_bt_bredrexm_addr);
        break;
    case 81:
        RET(bredr_get_anchor_point_usec, n, n % 4096 + 1);
        break;
    case 82:
        RET(get_mclkn, n, n, (u32 *)link);
        break;
    case 83:
        RET(get_mclkn_to_local_clkn, n, n, (u32 *)link, n & 1);
        break;
    case 84:
        DO(s_sniffer_offset_cal_start, frame, frame2, (u32 *)link, (u16 *)(link + 4), n, n);
        break;
    case 85:
        DO(tws_forward_offset_cal, frame, (u32 *)link, (u16 *)(link + 4), n, n);
        break;
    case 86:
        DO(s_forward_offset_cal, frame, (u32 *)link, (u16 *)(link + 4), n, n);
        break;
    case 87:
        DO(preserve_bitoff_cal, frame, n, (u32 *)link, (u16 *)(link + 4), n);
        break;
    case 88:
        DO(esco_tws_offset_cal_start, frame, n);
        break;
    case 89:
        DO(bredr_frame_get_empty_txbuf_index, frame, link);
        break;
    case 90:
        DO(bredr_frame_get_rx_payload_head, frame, link);
        break;
    }
}
static void helper_tests(void) {
    for (u32 which = 0; which < 94; which++)
        for (u32 sample = 0; sample < 512; sample++) {
            u32 n = (sample < 256 ? sample : (sample * 31337u) & 65535);
            ++caseid;
            for (int own = 0; own < 2; own++) {
                setup(2, n % 6, n & 1, n & 15, 127, n & 1);
                for (u32 i = 0; i < 564; i++)
                    frame[i] = (u8)(n + i * 17);
                frame[564] = 0;
                *(u16 *)(frame + 566) = n;
                *(u32 *)(frame + 572) = 0xdeadbeef;
                for (int i = 0; i < 80; i++)
                    fhs[i] = (u8)(n + i * 13);
                config_asser = n & 1;
                bd_base[84] = n % 3;
                *(u32 *)(link + 4) = n;
                helpers(own, which, n);
                if (!own)
                    save();
                else
                    check();
            }
        }
}
__attribute__((force_align_arg_pointer)) void _start(void) {
    for (mutation = 0; mutation < 6; mutation++) {
        run(sdk_page_rx_handler, owned_page_rx_handler, page);
        run(sdk_page_scan_rx_handler, owned_page_scan_rx_handler, scan);
        run(sdk_role_switch_page_scan_rx_handler, owned_role_switch_page_scan_rx_handler, scan);
        run(sdk_inquiry_rx_handler, owned_inquiry_rx_handler, inq);
        run(sdk_bredr_frame_conn_rx_irq_handler, owned_bredr_frame_conn_rx_irq_handler, link);
        run(sdk_role_switch_page_scan_timeout, owned_role_switch_page_scan_timeout, scan);
        run(sdk_quick_page_step2, owned_quick_page_step2, page);
        run(sdk_quick_page_scan_step2, owned_quick_page_scan_step2, scan);
        run(sdk_bredr_frame_page_resume, owned_bredr_frame_page_resume, page);
        run(sdk_bredr_frame_page_scan_resume, owned_bredr_frame_page_scan_resume, scan);
        run(sdk_irq_model, owned_irq_model, frame);
    }
    mutation = 0;
    offsets();
    helper_tests();
    output("PASS ", 5);
    number(checks);
    output("\n", 1);
    __asm__ volatile("int $0x80" ::"a"(1), "b"(0));
    __builtin_unreachable();
}
