/* What is used of the SDK's lmp_utils.c: byte-order reversal and the
 * access-code parity the radio pages and scans with (bredr_link.c).  The
 * SDK's byte-store and address-printing helpers have no caller. */
#include "btctrler/glue.h"

void endian_change(u8 *src, u16 len)
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
static u64 parity(u32 lap)
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
void __get_access_addr(u8 *access, const u8 *addr)
{
    u64 p = parity(addr[0] | addr[1] << 8 | (u32)addr[2] << 16);
    for (int i = 0; i < 5; i++) access[i] = (u8)(p >> 8 * i);
}

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
    return TEST_DONE("btctrler_lmp_utils");
}
#endif
