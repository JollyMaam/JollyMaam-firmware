/* CRC wrappers and the BR23 random-byte reader from misc_hw.c. The 32-bit
 * CRC keeps the SDK's unprototyped CRC16 return width and its raw OR. */
#include "btctrler/glue.h"
#include <string.h>

/* The SDK IR calls CRC16 through an unprototyped declaration and consumes an
 * i32 result, even though other SDK headers give CRC16 a u16 return type. */
extern int CRC16(const void *src, int len);

u32 crc_get_32bit(const char *src)
{
    u32 len = (u32)strlen(src);
    u32 crc = (u32)CRC16(src, (int)len);
    u8 reversed[len];
    btctler_reverse_bytes((const u8 *)src, reversed, (int)len);
    return ((u32)CRC16(reversed, (int)len) << 16) | crc;
}

u16 crc_get_16bit(const void *src, u32 len)
{
    return (u16)CRC16(src, (int)len);
}

#ifdef UNITTEST
static const u32 test_rng_values[] = { 0x12345678, 0x1234565a, 0x123456bc };
static unsigned int test_rng_reads;
static u32 misc_hw_random_word(void)
{
    return test_rng_values[test_rng_reads++];
}
#define MISC_HW_RANDOM_WORD() misc_hw_random_word()
#else
#define MISC_HW_RANDOM_WORD() (*(volatile u32 *)(uintptr_t)0x1e3b00)
#endif

void pseudo_random_genrate(uint8_t *dest, unsigned int size)
{
    for (unsigned int i = 0; i != size; ++i)
        dest[i] = (u8)MISC_HW_RANDOM_WORD();
}

#ifdef UNITTEST
#include "test.h"

static const void *crc_srcs[2];
static int crc_lens[2];
static int crc_results[3];
static uint8_t reversed_seen[3];
static unsigned int crc_calls;

int CRC16(const void *src, int len)
{
    CHECK(crc_calls < 2);
    crc_srcs[crc_calls] = src;
    crc_lens[crc_calls] = len;
    if (crc_calls == 1 && len == (int)sizeof(reversed_seen))
        memcpy(reversed_seen, src, sizeof(reversed_seen));
    return crc_results[crc_calls++];
}

void btctler_reverse_bytes(const uint8_t *src, uint8_t *dst, int len)
{
    for (int i = 0; i < len; ++i) dst[i] = src[len - i - 1];
}

int main(void)
{
    static const char text[] = "abc";
    static const uint8_t reversed[] = { 'c', 'b', 'a' };
    crc_results[0] = 0x00010002;
    crc_results[1] = 0x00005678;
    CHECK(crc_get_32bit(text) == 0x56790002u);
    CHECK(crc_calls == 2 && crc_srcs[0] == text && crc_lens[0] == 3);
    CHECK(crc_lens[1] == 3 && memcmp(reversed_seen, reversed, sizeof(reversed)) == 0);

    crc_calls = 0;
    crc_results[0] = 0x12345678;
    CHECK(crc_get_16bit(text, 0) == 0x5678);
    CHECK(crc_calls == 1 && crc_srcs[0] == text && crc_lens[0] == 0);
    crc_calls = 0;
    crc_results[0] = 0x0001abcd;
    CHECK(crc_get_16bit(text, 0xffffffffu) == 0xabcd);
    CHECK(crc_calls == 1 && crc_srcs[0] == text && crc_lens[0] == -1);

    uint8_t out[3] = { 0xaa, 0xaa, 0xaa };
    test_rng_reads = 0;
    pseudo_random_genrate(out, 0);
    CHECK(test_rng_reads == 0 && out[0] == 0xaa);
    pseudo_random_genrate(out, sizeof(out));
    CHECK(test_rng_reads == 3);
    CHECK(out[0] == 0x78 && out[1] == 0x5a && out[2] == 0xbc);
    return TEST_DONE("btctrler_misc_hw");
}
#endif
