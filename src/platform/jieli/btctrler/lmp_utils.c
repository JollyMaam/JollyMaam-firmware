/* BR23 uses the byte-order reversal and access-code parity here. BD19 also
 * retains the SDK's remaining lmp_utils exports for whole-member ownership. */
#include "btctrler/glue.h"

#ifdef CONFIG_CPU_BD19
#define LMP_CODE __attribute__((section(".classic_lmp_code")))
#define LMP_INLINE inline __attribute__((always_inline))
#define LMP_BSS __attribute__((section(".classic_lmp_bss"), aligned(1)))
#else
#define LMP_CODE
#define LMP_INLINE
#define LMP_BSS
#endif

LMP_CODE void endian_change(u8 *src, u16 len)
{
    for (unsigned i = 0, j = len; i < len / 2u; i++) {
        u8 t = src[--j];
        src[j] = src[i];
        src[i] = t;
    }
}

/* Sync word parity (Core Vol 2 Part B 6.3.3.1), in the SDK's bit order:
 * the spec's sync word bit-reversed, so the 24-bit LAP and the barker bits
 * sit above the 34 parity bits.  The information bits, scrambled with the
 * PN sequence's top 30 bits, are divided by the BCH generator g(D); the
 * remainder, scrambled with the PN's low 34 bits, is the parity. */
#define BCH_GENERATOR 0x585713da9ull                 /* g(D), degree 34 */
#define PN_HIGH       0x83848d9400000000ull         /* p63..p34 */
#define PN_LOW        0x2bbcc54fcull                /* p33..p0 */
static LMP_INLINE u64 parity(u32 lap)
{
    /* Barker sequence: 001101 or 110010, by the LAP's top bit. */
    u64 a = ((u64)(lap | (lap & 0x800000 ? 0x13000000 : 0x2c000000)) << 34) ^ PN_HIGH;
    /* Polynomial division as the SDK does it: the divisor aligned under the
     * top bit, shifted down one step at a time. */
    int len = 64;
    while (!(a & 1ull << (len - 1))) len--;
    const int n = len - 35;
    const u64 top = 1ull << (len - 1);
    for (int i = 0;;) {
        a ^= BCH_GENERATOR << n;
        if (i >= n) break;
        do { a <<= 1; i++; } while (!(a & top) && i < n);
        if (!(a & top)) break;
    }
    return ((a >> n) & ((1ull << 34) - 1)) ^ PN_LOW;
}

/* access: the 34 parity bits, least significant byte first. */
LMP_CODE void __get_access_addr(u8 *access, const u8 *addr)
{
    u64 p = parity(addr[0] | addr[1] << 8 | (u32)addr[2] << 16);
    for (int i = 0; i < 5; i++) access[i] = (u8)(p >> 8 * i);
}

#ifdef CONFIG_CPU_BD19
/* This helper set is retained for whole-member BD19 ownership, including the
 * SDK's shared formatting buffer and its byte-order conventions. */
static u8 lmp_bd_addr_to_str_buffer[18] LMP_BSS;

LMP_CODE void lmp_bt_store_16(u8 *buffer, u16 pos, u16 value)
{
    buffer[pos] = (u8)value;
    buffer[(u16)(pos + 1u)] = (u8)(value >> 8);
}

LMP_CODE void lmp_bt_store_32(u8 *buffer, u16 pos, u32 value)
{
    buffer[pos] = (u8)value;
    buffer[(u16)(pos + 1u)] = (u8)(value >> 8);
    buffer[(u16)(pos + 2u)] = (u8)(value >> 16);
    buffer[(u16)(pos + 3u)] = (u8)(value >> 24);
}

LMP_CODE void lmp_net_store_16(u8 *buffer, u16 pos, u16 value)
{
    buffer[pos] = (u8)(value >> 8);
    buffer[(u16)(pos + 1u)] = (u8)value;
}

LMP_CODE void lmp_net_store_32(u8 *buffer, u16 pos, u32 value)
{
    buffer[pos] = (u8)(value >> 24);
    buffer[(u16)(pos + 1u)] = (u8)(value >> 16);
    buffer[(u16)(pos + 2u)] = (u8)(value >> 8);
    buffer[(u16)(pos + 3u)] = (u8)value;
}

LMP_CODE void lmp_bt_flip_addr(u8 *dest, u8 *src)
{
    for (unsigned i = 0; i < 6; i++) dest[i] = src[5u - i];
}

LMP_CODE char lmp_char_for_nibble(int nibble)
{
    if (nibble < 10) return (char)(nibble + 48);
    if (nibble - 10 < 6) return (char)(nibble + 55);
    return '?';
}

LMP_CODE char *lmp_bd_addr_to_str(u8 *addr)
{
    char *p = (char *)lmp_bd_addr_to_str_buffer;
    for (unsigned i = 0; i < 6; i++) {
        p[0] = lmp_char_for_nibble(addr[i] >> 4);
        p[1] = lmp_char_for_nibble(addr[i] & 0x0f);
        p[2] = ':';
        p += 3;
    }
    p[-1] = 0;
    return (char *)lmp_bd_addr_to_str_buffer;
}

LMP_CODE void lmp_bt_flip_access_addr(u8 *dest, u8 *src)
{
    for (unsigned i = 0; i < 5; i++) dest[i] = src[4u - i];
}
#endif

#ifdef UNITTEST
#include "test.h"
int main(void)
{
    u8 buf[7] = { 1, 2, 3, 4, 5, 6, 7 };
    endian_change(buf, 7); CHECK(!memcmp(buf, (u8[]){ 7, 6, 5, 4, 3, 2, 1 }, 7));
    endian_change(buf, 4); CHECK(!memcmp(buf, (u8[]){ 4, 5, 6, 7, 3, 2, 1 }, 7));
    endian_change(buf, 0); endian_change(buf, 1); CHECK(buf[0] == 4);
    /* GIAC 0x9E8B33: sync word 0x475C58CC73345E72 (Core Vol 2 Part G);
     * bit-reversed, its low 34 bits are 0x2331A3AE2. */
    u8 access[5], giac[6] = { 0x33, 0x8b, 0x9e, 0, 0, 0 };
    __get_access_addr(access, giac);
    CHECK(!memcmp(access, (u8[]){ 0xe2, 0x3a, 0x1a, 0x33, 0x02 }, 5));
#ifdef CONFIG_CPU_BD19
    u8 bytes[6] = { 0 }, flipped[6], access_flipped[5];
    lmp_bt_store_16(bytes, 1, 0x1234);
    CHECK(!memcmp(bytes, (u8[]){ 0, 0x34, 0x12, 0, 0, 0 }, 6));
    lmp_bt_store_32(bytes, 1, 0x12345678);
    CHECK(!memcmp(bytes, (u8[]){ 0, 0x78, 0x56, 0x34, 0x12, 0 }, 6));
    lmp_net_store_16(bytes, 1, 0x1234);
    CHECK(!memcmp(bytes, (u8[]){ 0, 0x12, 0x34, 0x34, 0x12, 0 }, 6));
    lmp_net_store_32(bytes, 1, 0x12345678);
    CHECK(!memcmp(bytes, (u8[]){ 0, 0x12, 0x34, 0x56, 0x78, 0 }, 6));
    static u8 wrap[65536];
    lmp_bt_store_16(wrap, 0xffff, 0xabcd);
    CHECK(wrap[0xffff] == 0xcd && wrap[0] == 0xab);
    for (unsigned i = 0; i < 6; i++) bytes[i] = (u8)(i + 1);
    lmp_bt_flip_addr(flipped, bytes);
    CHECK(!memcmp(flipped, (u8[]){ 6, 5, 4, 3, 2, 1 }, 6));
    lmp_bt_flip_addr(bytes, bytes);
    CHECK(!memcmp(bytes, (u8[]){ 6, 5, 4, 4, 5, 6 }, 6));
    lmp_bt_flip_access_addr(access_flipped, access);
    CHECK(!memcmp(access_flipped, (u8[]){ 0x02, 0x33, 0x1a, 0x3a, 0xe2 }, 5));
    CHECK(lmp_char_for_nibble(0) == '0' && lmp_char_for_nibble(9) == '9');
    CHECK(lmp_char_for_nibble(10) == 'A' && lmp_char_for_nibble(15) == 'F');
    CHECK(lmp_char_for_nibble(16) == '?' && lmp_char_for_nibble(-1) == '/');
    u8 text_addr[6] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab };
    char *text = lmp_bd_addr_to_str(text_addr);
    CHECK(text == lmp_bd_addr_to_str(text_addr));
    CHECK(!strcmp(text, "01:23:45:67:89:AB"));
#endif
    return TEST_DONE("btctrler_lmp_utils");
}
#endif
