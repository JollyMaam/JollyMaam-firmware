/* Bluetooth AGC table upload and baseband setup. Keep the SDK's reversed
 * table walk, repeated writes to the data port, and MMIO order. */
#include "btctrler/glue.h"
#include <stdint.h>
#include <string.h>

#define AGC_RAM_ADDR 1899928u
#define AGC_RAM_DATA 1899932u

#ifndef CONFIG_CPU_BD19
const char agc_dbm_tlb[33] __attribute__((section(".bt_rf_const"))) = {
    (char)0xf7, (char)0xf9, (char)0xfb, (char)0xfd, (char)0xff,
    0x01, 0x03, 0x0a, 0x0c, 0x0e, 0x10, 0x12, 0x14, 0x16, 0x18,
    0x1a, 0x1c, 0x1e, 0x20, 0x22, 0x22, 0x24, 0x26, 0x25, 0x27,
    0x29, 0x2b, 0x2d, 0x2f, 0x31, 0x32, 0x35, 0x37,
};

static const u64 agc_tlb[64]
    __attribute__((section(".bt_rf_const"), aligned(4))) = {
    UINT64_C(0x000000000000001f), UINT64_C(0x00000000000007e0),
    UINT64_C(0x0000fffffffff800), UINT64_C(0x00002ca289248207),
    UINT64_C(0x00001c6185144103), UINT64_C(0x00000c2082041040),
    UINT64_C(0x000000000003ffbe), UINT64_C(0x0000f7cf3befaeb9),
    UINT64_C(0x0000e78e37df6db5), UINT64_C(0x0000d74d33cf2cb1),
    UINT64_C(0x0000208208208208), UINT64_C(0x0000208208208208),
    UINT64_C(0x0000208208208208), UINT64_C(0x0000208208208208),
    UINT64_C(0x0000208208208208), UINT64_C(0x0000206186186184),
    UINT64_C(0x0000104104082082), UINT64_C(0x0000082082082041),
    UINT64_C(0x00001c71c71c71c7), UINT64_C(0x00001c71c71c71c7),
    UINT64_C(0x00001c71c71c71c7), UINT64_C(0x00001c71c71c71c7),
    UINT64_C(0x00001c71c71c6144), UINT64_C(0x00000c7185103087),
    UINT64_C(0x00001851031c6144), UINT64_C(0x00000c2040000000),
    UINT64_C(0x00001c71c71c71c7), UINT64_C(0x00001c71c71c71c7),
    UINT64_C(0x00001c71c71c71c7), UINT64_C(0x00001c71c71c71c7),
    UINT64_C(0x00001c71c71c71c7), UINT64_C(0x00001c5145145144),
    UINT64_C(0x0000104104082082), UINT64_C(0x0000082082040000),
    UINT64_C(0x000000001c2b4fa0), UINT64_C(0x000000001c2b4fa0),
    UINT64_C(0x000000001c2b4fa0), UINT64_C(0x000000001c2b4fa0),
    UINT64_C(0x000000001c2b4fa0), UINT64_C(0x000000001c2b4fa0),
    UINT64_C(0x000000001c2b4fa0), UINT64_C(0x000000001c2b4fa0),
    UINT64_C(0x000000001c2b4fa0), UINT64_C(0x000000001c2b4fa0),
    UINT64_C(0x000000001c2b4fa0), UINT64_C(0x000000001c2b4fa0),
    UINT64_C(0x000000001c2b4fa0), UINT64_C(0x000000001c2b4fa0),
    UINT64_C(0x000000001c2b4fa0), UINT64_C(0x000000001c2b4fa0),
    UINT64_C(0x000000001c2b4fa0), UINT64_C(0x000000001c2b4fa0),
    UINT64_C(0x000000001c2b4fa0), UINT64_C(0x000000001c2a4fa0),
    UINT64_C(0x000000001c294fa0), UINT64_C(0x000000001c284fa0),
    UINT64_C(0x000000001c274fa3), UINT64_C(0x000000001c264ea5),
    UINT64_C(0x000000001c254da7), UINT64_C(0x000000001c244ca9),
    UINT64_C(0x000000001c234bab), UINT64_C(0x000000001c224aad),
    UINT64_C(0x000000001c2149af), UINT64_C(0x000000001c2048b1),
};
#endif

#ifdef UNITTEST
#include "test.h"

struct mmio_write { u32 address, value; };
static struct mmio_write writes[512];
static unsigned int write_count;
static unsigned int delay_calls;
static unsigned int delay_value;
static unsigned int delay_after_writes;
static unsigned int delay_write_counts[16];
static unsigned int delay_values[16];
static u32 test_regs[96];

static u32 fnv1a(const u8 *data, size_t len)
{
    u32 h = UINT32_C(2166136261);
    for (size_t i = 0; i < len; ++i) h = (h ^ data[i]) * UINT32_C(16777619);
    return h;
}

static u32 fnv1a_words(const unsigned long *data, size_t count)
{
    u32 h = UINT32_C(2166136261);
    for (size_t i = 0; i < count; ++i) {
        u32 word = (u32)data[i];
        for (unsigned int b = 0; b != 4; ++b)
            h = (h ^ (u8)(word >> (b * 8))) * UINT32_C(16777619);
    }
    return h;
}

static unsigned int reg_index(u32 address)
{
    CHECK(address >= 1899552 && address <= 1899932 && !(address & 3));
    return (address - 1899552) / 4;
}

static u32 agc_read(u32 address) { return test_regs[reg_index(address)]; }
static void agc_write(u32 address, u32 value)
{
    CHECK(write_count < sizeof(writes) / sizeof(writes[0]));
    writes[write_count++] = (struct mmio_write){ address, value };
    test_regs[reg_index(address)] = value;
}
void delay(u32 ms)
{
    if (delay_calls < sizeof(delay_values) / sizeof(delay_values[0])) {
        delay_values[delay_calls] = ms;
        delay_write_counts[delay_calls] = write_count;
    }
    ++delay_calls;
    delay_value = ms;
    delay_after_writes = write_count;
}
#define AGC_READ(address) agc_read(address)
#define AGC_WRITE(address, value) agc_write((address), (value))
#else
#define AGC_READ(address) (*(volatile u32 *)(uintptr_t)(address))
#define AGC_WRITE(address, value) (*(volatile u32 *)(uintptr_t)(address) = (value))
#endif

extern void delay(u32 ms);

#ifdef CONFIG_CPU_BD19
const char gain_adj_step[64] __attribute__((section(".bt_rf_const"), aligned(1))) = {
    (char)0xec,(char)0xed,(char)0xed,(char)0xee,(char)0xee,(char)0xef,(char)0xef,(char)0xf0,
    (char)0xf0,(char)0xf1,(char)0xf1,(char)0xf2,(char)0xf2,(char)0xf3,(char)0xf3,(char)0xf4,
    (char)0xf4,(char)0xf5,(char)0xf5,(char)0xf6,(char)0xf6,(char)0xf7,(char)0xf7,(char)0xf8,
    (char)0xf8,(char)0xf9,(char)0xf9,(char)0xfa,(char)0xfa,(char)0xfb,(char)0xfb,(char)0xfc,
    (char)0xfc,(char)0xfd,(char)0xfd,(char)0x00,(char)0x00,(char)0x00,(char)0x00,(char)0x00,
    (char)0x00,(char)0x00,(char)0x00,(char)0x00,(char)0x00,(char)0x00,(char)0x03,(char)0x03,
    (char)0x04,(char)0x04,(char)0x05,(char)0x05,(char)0x06,(char)0x06,(char)0x07,(char)0x07,
    (char)0x08,(char)0x08,(char)0x09,(char)0x09,(char)0x0a,(char)0x0a,(char)0x0b,(char)0x0b,
};
const long unsigned int anl_gain_set[56] __attribute__((section(".bt_rf_const"), aligned(4))) = {
    0x20f10d47UL,0x20f10d47UL,0x20f10d47UL,0x20f10d47UL,0x20f10d47UL,0x20f10d47UL,0x20f10d47UL,0x20f10d47UL,
    0x20f10d47UL,0x20f10d47UL,0x20f10d47UL,0x20f10d47UL,0x20f10d47UL,0x20f10d47UL,0x20f10d47UL,0x20f10d47UL,
    0x20f10d47UL,0x20f10d47UL,0x20f10d47UL,0x20f10d47UL,0x20f10d47UL,0x22f10d47UL,0x22f12d47UL,0x22f14d47UL,
    0x22f10d46UL,0x22f12d46UL,0x22f14d46UL,0x22f16d46UL,0x22f18d46UL,0x22f1ad46UL,0x22f1cd46UL,0x22f1ed46UL,
    0x28f10d44UL,0x28f12d44UL,0x28f14d44UL,0x28f16d44UL,0x28f18d44UL,0x28f1ad44UL,0x28f1cd44UL,0x28f1ed44UL,
    0x2ef10fc1UL,0x2ef12fc1UL,0x2ef14fc1UL,0x2ef16fc1UL,0x2ef18fc1UL,0x2ef1afc1UL,0x2ef1cfc1UL,0x2ef1efc1UL,
    0x2cf10dc8UL,0x2cf12dc8UL,0x2cf14dc8UL,0x2cf16dc8UL,0x2cf18dc8UL,0x2cf1adc8UL,0x2cf1cdc8UL,0x2cf1edc8UL,
};
const long unsigned int anl_gain_set_h[56] __attribute__((section(".bt_rf_const"), aligned(4))) = {
    0xf000000dUL,0xf000000dUL,0xf000000dUL,0xf000000dUL,0xf000000dUL,0xf000000dUL,0xf000000dUL,0xf000000dUL,
    0xf000000dUL,0xf000000dUL,0xf000000dUL,0xf000000dUL,0xf000000dUL,0xf000000dUL,0xf000000dUL,0xf000000dUL,
    0xf000000dUL,0xf000000dUL,0xf000000dUL,0xf000000dUL,0xf000000dUL,0xf200000dUL,0xf400000dUL,0xf600000dUL,
    0xf800000dUL,0xfa00000dUL,0xfc00000dUL,0xfe00000dUL,0x0000000dUL,0x0200000dUL,0x0400000dUL,0x0600000dUL,
    0x0800000dUL,0x0a00000dUL,0x0c00000dUL,0x0e00000dUL,0x1000000dUL,0x1200000dUL,0x1400000dUL,0x1600000dUL,
    0x1800000dUL,0x1a00000dUL,0x1c00000dUL,0x1e00000dUL,0x2000000dUL,0x2200000dUL,0x2400000dUL,0x2600000dUL,
    0x2800000dUL,0x2a00000dUL,0x2c00000dUL,0x2e00000dUL,0x3000000dUL,0x3200000dUL,0x3400000dUL,0x3600000dUL,
};
const unsigned int rx_power_set[7] __attribute__((section(".bt_rf_const"), aligned(4))) = {
    0,0,128,289,425,594,635,
};

void btagc_ram_wr(u8 agc_table_switch) __attribute__((section(".bt_rf_code")));
void set_btagc_pwr_sel(u8 sel) __attribute__((section(".bt_rf_code")));
void soft_agc(struct link_connection *lc, u8 check, u8 type) __attribute__((section(".bt_rf_code")));
void btagc_init(u8 lnav_flag, u8 agc_table_switch) __attribute__((section(".bt_rf_code")));
void pcu_init(void) __attribute__((section(".bt_rf_code")));

void btagc_ram_wr(u8 agc_table_switch)
{
    (void)agc_table_switch;
    AGC_WRITE(1899904, AGC_READ(1899904) | 4096u);
    AGC_WRITE(1899904, AGC_READ(1899904) | 8192u);
    AGC_WRITE(1899904, AGC_READ(1899904) | 32768u);
    for (u32 i = 0; i != 64; ++i) {
        AGC_WRITE(AGC_RAM_ADDR, i);
        AGC_WRITE(1899904, AGC_READ(1899904) & ~16384u);
        if (i < 56) {
            AGC_WRITE(AGC_RAM_DATA, (u32)anl_gain_set[i]);
            AGC_WRITE(1899904, AGC_READ(1899904) | 16384u);
            AGC_WRITE(AGC_RAM_DATA, (u32)anl_gain_set_h[i]);
        } else {
            u32 base = (i << 3) - 448;
            u32 word = (u8)gain_adj_step[base] |
                       ((u32)(u8)gain_adj_step[base + 1] << 8) |
                       ((u32)(u8)gain_adj_step[base + 2] << 16) |
                       ((u32)(u8)gain_adj_step[base + 3] << 24);
            AGC_WRITE(AGC_RAM_DATA, word);
            AGC_WRITE(1899904, AGC_READ(1899904) | 16384u);
            word = (u8)gain_adj_step[base + 4] |
                   ((u32)(u8)gain_adj_step[base + 5] << 8) |
                   ((u32)(u8)gain_adj_step[base + 6] << 16) |
                   ((u32)(u8)gain_adj_step[base + 7] << 24);
            AGC_WRITE(AGC_RAM_DATA, word);
        }
    }
    AGC_WRITE(1899904, AGC_READ(1899904) & ~8192u);
    AGC_WRITE(1899904, AGC_READ(1899904) & ~16384u);
    AGC_WRITE(1899904, AGC_READ(1899904) & ~4096u);
    AGC_WRITE(1899904, AGC_READ(1899904) | 32768u);
}

void set_btagc_pwr_sel(u8 sel) { (void)sel; }
void soft_agc(struct link_connection *lc, u8 check, u8 type)
{ (void)lc; (void)check; (void)type; }

void btagc_init(u8 lnav_flag, u8 agc_table_switch)
{
    (void)agc_table_switch;
    btagc_ram_wr(0);
    AGC_WRITE(1899904, (AGC_READ(1899904) & ~917505u) | 655360u);
    AGC_WRITE(1899904, AGC_READ(1899904) | 1u);
    AGC_WRITE(1899904, (AGC_READ(1899904) & ~31u) | 6u);
    AGC_WRITE(1899904, (AGC_READ(1899904) & ~960u) | 64u);
    u32 flag = lnav_flag & 1u;
    AGC_WRITE(1899904, (AGC_READ(1899904) & ~1024u) | (flag << 10));
    AGC_WRITE(1899904, AGC_READ(1899904) & ~2048u);
    AGC_WRITE(1899904, AGC_READ(1899904) & ~4096u);
    AGC_WRITE(1899904, AGC_READ(1899904) & ~8192u);
    AGC_WRITE(1899904, AGC_READ(1899904) & ~16384u);
    AGC_WRITE(1899904, AGC_READ(1899904) | 32768u);
    AGC_WRITE(1899904, AGC_READ(1899904) & ~65536u);
    AGC_WRITE(1899904, (AGC_READ(1899904) & ~1048576u) | (flag << 20));
    AGC_WRITE(1899904, (AGC_READ(1899904) & ~132120577u) | 62914560u);
    AGC_WRITE(1899904, AGC_READ(1899904) & ~134217729u);
    AGC_WRITE(1899904, AGC_READ(1899904) & ~268435457u);
    AGC_WRITE(1899908, (AGC_READ(1899908) & ~63u) | 55u);
    AGC_WRITE(1899908, (AGC_READ(1899908) & ~16128u) | 9984u);
    AGC_WRITE(1899908, (AGC_READ(1899908) & ~114688u) | 81920u);
    AGC_WRITE(1899908, (AGC_READ(1899908) & ~917504u) | 655360u);
    AGC_WRITE(1899908, AGC_READ(1899908) & ~1048576u);
    AGC_WRITE(1899908, (AGC_READ(1899908) & ~62914560u) | 12582912u);
    AGC_WRITE(1899908, (AGC_READ(1899908) & ~1006632960u) | 67108864u);
    AGC_WRITE(1899908, AGC_READ(1899908) | 1073741824u);
    AGC_WRITE(1899912, (AGC_READ(1899912) & ~63u) | 41u);
    AGC_WRITE(1899912, (AGC_READ(1899912) & ~4032u) | 2624u);
    AGC_WRITE(1899912, (AGC_READ(1899912) & ~258048u) | 167936u);
    AGC_WRITE(1899912, (AGC_READ(1899912) & ~16515072u) | 11272192u);
    AGC_WRITE(1899912, (AGC_READ(1899912) & ~1056964608u) | 754974720u);
    AGC_WRITE(1899912, AGC_READ(1899912) & 1073741823u);
    AGC_WRITE(1899916, (AGC_READ(1899916) & ~63u) | 47u);
    AGC_WRITE(1899916, (AGC_READ(1899916) & ~4032u) | 3520u);
    AGC_WRITE(1899916, AGC_READ(1899916) & ~258048u);
    AGC_WRITE(1899916, AGC_READ(1899916) & ~16515072u);
    AGC_WRITE(1899916, AGC_READ(1899916) & ~50331648u);
    AGC_WRITE(1899916, AGC_READ(1899916) & 67108863u);
    AGC_WRITE(1899904, AGC_READ(1899904) & ~32768u);
    AGC_WRITE(1899904, AGC_READ(1899904) | 32768u);
}

void pcu_init(void)
{
    AGC_WRITE(1899692, 240);
    for (u32 i = 0; i != 7; ++i) {
        AGC_WRITE(1899696, rx_power_set[i]);
        delay(2);
    }
    AGC_WRITE(1899740, 0);
    AGC_WRITE(1899736, 0);
    AGC_WRITE(1899736, (AGC_READ(1899736) & ~127u) | 50u);
    AGC_WRITE(1899736, (AGC_READ(1899736) & ~8064u) | 3840u);
    AGC_WRITE(1899736, (AGC_READ(1899736) & ~507904u) | 163840u);
    AGC_WRITE(1899736, AGC_READ(1899736) | 1048576u);
    AGC_WRITE(1899736, AGC_READ(1899736) | 16777216u);
    AGC_WRITE(1899740, (AGC_READ(1899740) & ~127u) | 50u);
    AGC_WRITE(1899740, (AGC_READ(1899740) & ~8064u) | 1920u);
    AGC_WRITE(1899740, (AGC_READ(1899740) & ~507904u) | 81920u);
    AGC_WRITE(1899740, AGC_READ(1899740) | 1048576u);
    AGC_WRITE(1899740, AGC_READ(1899740) | 8388608u);
    AGC_WRITE(1899760, (AGC_READ(1899760) & ~1835008u) | 1310720u);
    AGC_WRITE(1899760, AGC_READ(1899760) & ~33554432u);
    delay(10);
    AGC_WRITE(1899760, AGC_READ(1899760) | 33554432u);
    AGC_WRITE(1899680, AGC_READ(1899680) | 2097152u);
}
#else
void btagc_ram_init(void) __attribute__((section(".bt_rf_code")));
void btagc_init(void) __attribute__((section(".bt_rf_code")));

void btagc_ram_init(void)
{
    AGC_WRITE(AGC_RAM_ADDR, 0);
    for (u32 entry = 64; entry != 0; --entry) {
        const u8 *bytes = (const u8 *)&agc_tlb[entry - 1];
        u32 word;
        memcpy(&word, bytes, sizeof(word));
        AGC_WRITE(AGC_RAM_DATA, word);
        memcpy(&word, bytes + sizeof(word), sizeof(word));
        AGC_WRITE(AGC_RAM_DATA, word);
    }
}

void btagc_init(void)
{
    AGC_WRITE(1899600, AGC_READ(1899600) | 64);
    btagc_ram_init();
    AGC_WRITE(1899552, AGC_READ(1899552) & UINT32_C(1073741823));
    AGC_WRITE(1899552, AGC_READ(1899552) | UINT32_C(0x80000000));
    AGC_WRITE(1899904, 983176);
    AGC_WRITE(1899908, 544332564);
    AGC_WRITE(1899912, 544332629);
    AGC_WRITE(1899916, 544332544);
    AGC_WRITE(1899920, 0);
    AGC_WRITE(1899924, 0);
    delay(100);
    AGC_WRITE(1899904, AGC_READ(1899904) | 1);
    AGC_WRITE(1899600, AGC_READ(1899600) & ~UINT32_C(64));
    AGC_WRITE(1899644, AGC_READ(1899644) | 1);
}
#endif

#ifdef UNITTEST
int main(void)
{
#ifdef CONFIG_CPU_BD19
    CHECK(fnv1a((const u8 *)gain_adj_step, sizeof(gain_adj_step)) == UINT32_C(0x20d381c5));
    CHECK(fnv1a_words(anl_gain_set, 56) == UINT32_C(0x2a605be7));
    CHECK(fnv1a_words(anl_gain_set_h, 56) == UINT32_C(0x73d7c51d));
    CHECK(fnv1a((const u8 *)rx_power_set, sizeof(rx_power_set)) == UINT32_C(0x3453d18c));
    CHECK(sizeof(gain_adj_step) == 64 && sizeof(rx_power_set) == 28);

    /* Table upload: 3 control writes, 64 address/control/data groups, 4 tails. */
    memset(test_regs, 0, sizeof(test_regs));
    write_count = delay_calls = 0;
    btagc_ram_wr(0xff);
    CHECK(write_count == 327 && delay_calls == 0);
    CHECK(writes[0].address == 1899904 && writes[0].value == 4096);
    CHECK(writes[1].address == 1899904 && writes[1].value == 12288);
    CHECK(writes[2].address == 1899904 && writes[2].value == 45056);
    for (u32 i = 0; i != 64; ++i) {
        unsigned int base = 3 + i * 5;
        CHECK(writes[base].address == AGC_RAM_ADDR && writes[base].value == i);
        if (i < 56) {
            CHECK(writes[base + 2].address == AGC_RAM_DATA &&
                  writes[base + 2].value == (u32)anl_gain_set[i]);
            CHECK(writes[base + 4].address == AGC_RAM_DATA &&
                  writes[base + 4].value == (u32)anl_gain_set_h[i]);
        } else {
            u32 off = (i << 3) - 448;
            u32 low = (u8)gain_adj_step[off] | ((u32)(u8)gain_adj_step[off + 1] << 8) |
                      ((u32)(u8)gain_adj_step[off + 2] << 16) |
                      ((u32)(u8)gain_adj_step[off + 3] << 24);
            u32 high = (u8)gain_adj_step[off + 4] | ((u32)(u8)gain_adj_step[off + 5] << 8) |
                       ((u32)(u8)gain_adj_step[off + 6] << 16) |
                       ((u32)(u8)gain_adj_step[off + 7] << 24);
            CHECK(writes[base + 2].address == AGC_RAM_DATA && writes[base + 2].value == low);
            CHECK(writes[base + 4].address == AGC_RAM_DATA && writes[base + 4].value == high);
        }
    }
    CHECK(writes[323].address == 1899904 && writes[324].address == 1899904 &&
          writes[325].address == 1899904 && writes[326].address == 1899904);

    /* btagc_init forwards lnav_flag bits 0/20 and ignores table_switch. */
    memset(test_regs, 0, sizeof(test_regs));
    write_count = 0;
    btagc_init(1, 0xff);
    CHECK((test_regs[reg_index(1899904)] & (UINT32_C(1) << 10)) != 0);
    CHECK((test_regs[reg_index(1899904)] & (UINT32_C(1) << 20)) != 0);
    memset(test_regs, 0, sizeof(test_regs));
    write_count = 0;
    btagc_init(0, 1);
    CHECK((test_regs[reg_index(1899904)] & ((UINT32_C(1) << 10) | (UINT32_C(1) << 20))) == 0);

    /* PCU table writes are followed by seven 2ms waits and one 10ms wait. */
    memset(test_regs, 0, sizeof(test_regs));
    write_count = delay_calls = 0;
    pcu_init();
    CHECK(write_count == 24 && delay_calls == 8);
    CHECK(writes[0].address == 1899692 && writes[0].value == 240);
    for (u32 i = 0; i != 7; ++i) {
        CHECK(writes[i + 1].address == 1899696 && writes[i + 1].value == rx_power_set[i]);
    }
    CHECK(delay_value == 10 && delay_after_writes == 22);
    for (u32 i = 0; i != 7; ++i)
        CHECK(delay_values[i] == 2 && delay_write_counts[i] == i + 2);
    CHECK(delay_values[7] == 10 && delay_write_counts[7] == 22);
    CHECK(writes[22].address == 1899760 && writes[22].value == UINT32_C(0x02140000));
    CHECK(writes[23].address == 1899680 && writes[23].value == (UINT32_C(1) << 21));
    set_btagc_pwr_sel(0xff);
    struct link_connection dummy = { 0 };
    soft_agc(&dummy, 0xff, 0xff);
    return TEST_DONE("btctrler_btagc_bd19");
#else
    static const u8 expected_dbm[33] = {
        0xf7, 0xf9, 0xfb, 0xfd, 0xff, 0x01, 0x03, 0x0a, 0x0c, 0x0e, 0x10,
        0x12, 0x14, 0x16, 0x18, 0x1a, 0x1c, 0x1e, 0x20, 0x22, 0x22, 0x24,
        0x26, 0x25, 0x27, 0x29, 0x2b, 0x2d, 0x2f, 0x31, 0x32, 0x35, 0x37,
    };
    static const u32 expected_words[128] = {
        UINT32_C(0x1c2048b1), UINT32_C(0x00000000), UINT32_C(0x1c2149af), UINT32_C(0x00000000),
        UINT32_C(0x1c224aad), UINT32_C(0x00000000), UINT32_C(0x1c234bab), UINT32_C(0x00000000),
        UINT32_C(0x1c244ca9), UINT32_C(0x00000000), UINT32_C(0x1c254da7), UINT32_C(0x00000000),
        UINT32_C(0x1c264ea5), UINT32_C(0x00000000), UINT32_C(0x1c274fa3), UINT32_C(0x00000000),
        UINT32_C(0x1c284fa0), UINT32_C(0x00000000), UINT32_C(0x1c294fa0), UINT32_C(0x00000000),
        UINT32_C(0x1c2a4fa0), UINT32_C(0x00000000), UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000),
        UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000), UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000),
        UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000), UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000),
        UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000), UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000),
        UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000), UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000),
        UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000), UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000),
        UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000), UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000),
        UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000), UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000),
        UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000), UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000),
        UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000), UINT32_C(0x1c2b4fa0), UINT32_C(0x00000000),
        UINT32_C(0x82040000), UINT32_C(0x00000820), UINT32_C(0x04082082), UINT32_C(0x00001041),
        UINT32_C(0x45145144), UINT32_C(0x00001c51), UINT32_C(0xc71c71c7), UINT32_C(0x00001c71),
        UINT32_C(0xc71c71c7), UINT32_C(0x00001c71), UINT32_C(0xc71c71c7), UINT32_C(0x00001c71),
        UINT32_C(0xc71c71c7), UINT32_C(0x00001c71), UINT32_C(0xc71c71c7), UINT32_C(0x00001c71),
        UINT32_C(0x40000000), UINT32_C(0x00000c20), UINT32_C(0x031c6144), UINT32_C(0x00001851),
        UINT32_C(0x85103087), UINT32_C(0x00000c71), UINT32_C(0xc71c6144), UINT32_C(0x00001c71),
        UINT32_C(0xc71c71c7), UINT32_C(0x00001c71), UINT32_C(0xc71c71c7), UINT32_C(0x00001c71),
        UINT32_C(0xc71c71c7), UINT32_C(0x00001c71), UINT32_C(0xc71c71c7), UINT32_C(0x00001c71),
        UINT32_C(0x82082041), UINT32_C(0x00000820), UINT32_C(0x04082082), UINT32_C(0x00001041),
        UINT32_C(0x86186184), UINT32_C(0x00002061), UINT32_C(0x08208208), UINT32_C(0x00002082),
        UINT32_C(0x08208208), UINT32_C(0x00002082), UINT32_C(0x08208208), UINT32_C(0x00002082),
        UINT32_C(0x08208208), UINT32_C(0x00002082), UINT32_C(0x08208208), UINT32_C(0x00002082),
        UINT32_C(0x33cf2cb1), UINT32_C(0x0000d74d), UINT32_C(0x37df6db5), UINT32_C(0x0000e78e),
        UINT32_C(0x3befaeb9), UINT32_C(0x0000f7cf), UINT32_C(0x0003ffbe), UINT32_C(0x00000000),
        UINT32_C(0x82041040), UINT32_C(0x00000c20), UINT32_C(0x85144103), UINT32_C(0x00001c61),
        UINT32_C(0x89248207), UINT32_C(0x00002ca2), UINT32_C(0xfffff800), UINT32_C(0x0000ffff),
        UINT32_C(0x000007e0), UINT32_C(0x00000000), UINT32_C(0x0000001f), UINT32_C(0x00000000),
    };
    CHECK(sizeof(agc_dbm_tlb) == 33);
    CHECK(memcmp(agc_dbm_tlb, expected_dbm, sizeof(expected_dbm)) == 0);
    write_count = 0;
    btagc_ram_init();
    CHECK(write_count == 129);
    CHECK(writes[0].address == AGC_RAM_ADDR && writes[0].value == 0);
    for (unsigned int i = 0; i != 128; ++i) {
        CHECK(writes[i + 1].address == AGC_RAM_DATA);
        CHECK(writes[i + 1].value == expected_words[i]);
    }

    memset(test_regs, 0, sizeof(test_regs));
    write_count = delay_calls = 0;
    test_regs[reg_index(1899600)] = 0x12340001;
    test_regs[reg_index(1899552)] = 0xffffffff;
    test_regs[reg_index(1899904)] = 0x100;
    test_regs[reg_index(1899644)] = 0x80;
    btagc_init();
    CHECK(write_count == 141);
    CHECK(writes[0].address == 1899600 && writes[0].value == 0x12340041);
    CHECK(writes[1].address == AGC_RAM_ADDR && writes[1].value == 0);
    CHECK(writes[130].address == 1899552 && writes[130].value == 0x3fffffff);
    CHECK(writes[131].address == 1899552 && writes[131].value == 0xbfffffff);
    static const struct mmio_write expected_tail[] = {
        { 1899904, 983176 }, { 1899908, 544332564 }, { 1899912, 544332629 },
        { 1899916, 544332544 }, { 1899920, 0 }, { 1899924, 0 },
        { 1899904, 983177 }, { 1899600, 0x12340001 }, { 1899644, 0x81 },
    };
    for (unsigned int i = 0; i != sizeof(expected_tail) / sizeof(expected_tail[0]); ++i) {
        CHECK(writes[132 + i].address == expected_tail[i].address);
        CHECK(writes[132 + i].value == expected_tail[i].value);
    }
    CHECK(delay_calls == 1 && delay_value == 100 && delay_after_writes == 138);
    return TEST_DONE("btctrler_btagc");
#endif
}
#endif
