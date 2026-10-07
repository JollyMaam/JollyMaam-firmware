/* Bluetooth AGC table upload and baseband setup. Keep the SDK's reversed
 * table walk, repeated writes to the data port, and MMIO order. */
#include "btctrler/glue.h"
#include <stdint.h>
#include <string.h>

#define AGC_RAM_ADDR 1899928u
#define AGC_RAM_DATA 1899932u

const char agc_dbm_tlb[33] __attribute__((section(".bt_rf_const"))) = {
    (char)0xf7, (char)0xf9, (char)0xfb, (char)0xfd, (char)0xff,
    0x01, 0x03, 0x0a, 0x0c, 0x0e, 0x10, 0x12, 0x14, 0x16, 0x18,
    0x1a, 0x1c, 0x1e, 0x20, 0x22, 0x22, 0x24, 0x26, 0x25, 0x27,
    0x29, 0x2b, 0x2d, 0x2f, 0x31, 0x33, 0x35, 0x37,
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

#ifdef UNITTEST
#include "test.h"

struct mmio_write { u32 address, value; };
static struct mmio_write writes[141];
static unsigned int write_count;
static unsigned int delay_calls;
static unsigned int delay_value;
static unsigned int delay_after_writes;
static u32 test_regs[96];

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
void delay(u32 ms) { ++delay_calls; delay_value = ms; delay_after_writes = write_count; }
#define AGC_READ(address) agc_read(address)
#define AGC_WRITE(address, value) agc_write((address), (value))
#else
#define AGC_READ(address) (*(volatile u32 *)(uintptr_t)(address))
#define AGC_WRITE(address, value) (*(volatile u32 *)(uintptr_t)(address) = (value))
#endif

extern void delay(u32 ms);

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

#ifdef UNITTEST
int main(void)
{
    static const u8 expected_dbm[33] = {
        0xf7, 0xf9, 0xfb, 0xfd, 0xff, 0x01, 0x03, 0x0a, 0x0c, 0x0e, 0x10,
        0x12, 0x14, 0x16, 0x18, 0x1a, 0x1c, 0x1e, 0x20, 0x22, 0x22, 0x24,
        0x26, 0x25, 0x27, 0x29, 0x2b, 0x2d, 0x2f, 0x31, 0x33, 0x35, 0x37,
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
}
#endif
