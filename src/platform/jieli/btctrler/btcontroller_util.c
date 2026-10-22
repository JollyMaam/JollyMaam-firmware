/* The SDK's small utility functions (btcontroller_util.c, a stripped copy of
 * BTstack's btstack_util): little-endian accessors, byte reversal, the
 * address-to-text helper, the RFCOMM-style CRC-8 and a few helpers.  Only the
 * functions something else still calls are carried over.  Kept as the SDK has
 * them: btctler_bd_addr_to_str returns one shared static buffer and its last
 * ':' is overwritten by the terminator; btctler_crc8_check returns 0 for a
 * match (like BTstack's btstack_crc8_check: crc8 ^ check_sum == 0xff is the
 * same as BTstack's table lookup comparing against 0xcf); the CRC-8 is not
 * complemented; btctler_get_rand_from_assign_range returns max when max <=
 * min and takes the remainder unsigned. */
#include "btctrler/glue.h"

/* The 07.10 / RFCOMM FCS table: CRC-8, polynomial x^8 + x^2 + x + 1, reflected. */
static const u8 crc8table[256] = {
    0x00, 0x91, 0xe3, 0x72, 0x07, 0x96, 0xe4, 0x75, 0x0e, 0x9f, 0xed, 0x7c, 0x09, 0x98, 0xea, 0x7b,
    0x1c, 0x8d, 0xff, 0x6e, 0x1b, 0x8a, 0xf8, 0x69, 0x12, 0x83, 0xf1, 0x60, 0x15, 0x84, 0xf6, 0x67,
    0x38, 0xa9, 0xdb, 0x4a, 0x3f, 0xae, 0xdc, 0x4d, 0x36, 0xa7, 0xd5, 0x44, 0x31, 0xa0, 0xd2, 0x43,
    0x24, 0xb5, 0xc7, 0x56, 0x23, 0xb2, 0xc0, 0x51, 0x2a, 0xbb, 0xc9, 0x58, 0x2d, 0xbc, 0xce, 0x5f,
    0x70, 0xe1, 0x93, 0x02, 0x77, 0xe6, 0x94, 0x05, 0x7e, 0xef, 0x9d, 0x0c, 0x79, 0xe8, 0x9a, 0x0b,
    0x6c, 0xfd, 0x8f, 0x1e, 0x6b, 0xfa, 0x88, 0x19, 0x62, 0xf3, 0x81, 0x10, 0x65, 0xf4, 0x86, 0x17,
    0x48, 0xd9, 0xab, 0x3a, 0x4f, 0xde, 0xac, 0x3d, 0x46, 0xd7, 0xa5, 0x34, 0x41, 0xd0, 0xa2, 0x33,
    0x54, 0xc5, 0xb7, 0x26, 0x53, 0xc2, 0xb0, 0x21, 0x5a, 0xcb, 0xb9, 0x28, 0x5d, 0xcc, 0xbe, 0x2f,
    0xe0, 0x71, 0x03, 0x92, 0xe7, 0x76, 0x04, 0x95, 0xee, 0x7f, 0x0d, 0x9c, 0xe9, 0x78, 0x0a, 0x9b,
    0xfc, 0x6d, 0x1f, 0x8e, 0xfb, 0x6a, 0x18, 0x89, 0xf2, 0x63, 0x11, 0x80, 0xf5, 0x64, 0x16, 0x87,
    0xd8, 0x49, 0x3b, 0xaa, 0xdf, 0x4e, 0x3c, 0xad, 0xd6, 0x47, 0x35, 0xa4, 0xd1, 0x40, 0x32, 0xa3,
    0xc4, 0x55, 0x27, 0xb6, 0xc3, 0x52, 0x20, 0xb1, 0xca, 0x5b, 0x29, 0xb8, 0xcd, 0x5c, 0x2e, 0xbf,
    0x90, 0x01, 0x73, 0xe2, 0x97, 0x06, 0x74, 0xe5, 0x9e, 0x0f, 0x7d, 0xec, 0x99, 0x08, 0x7a, 0xeb,
    0x8c, 0x1d, 0x6f, 0xfe, 0x8b, 0x1a, 0x68, 0xf9, 0x82, 0x13, 0x61, 0xf0, 0x85, 0x14, 0x66, 0xf7,
    0xa8, 0x39, 0x4b, 0xda, 0xaf, 0x3e, 0x4c, 0xdd, 0xa6, 0x37, 0x45, 0xd4, 0xa1, 0x30, 0x42, 0xd3,
    0xb4, 0x25, 0x57, 0xc6, 0xb3, 0x22, 0x50, 0xc1, 0xba, 0x2b, 0x59, 0xc8, 0xbd, 0x2c, 0x5e, 0xcf,
};

static char bd_addr_to_str_buffer[18];

uint16_t btctler_little_endian_read_16(const uint8_t *buffer, int pos)
{
    return (uint16_t)(buffer[pos + 1] << 8 | buffer[pos]);
}

uint32_t btctler_little_endian_read_24(const uint8_t *buffer, int pos)
{
    return (uint32_t)buffer[pos + 1] << 8 | buffer[pos] | (uint32_t)buffer[pos + 2] << 16;
}

uint32_t btctler_little_endian_read_32(const uint8_t *buffer, int pos)
{
    return (uint32_t)buffer[pos + 1] << 8 | buffer[pos] | (uint32_t)buffer[pos + 2] << 16 | (uint32_t)buffer[pos + 3] << 24;
}

void btctler_little_endian_store_16(uint8_t *buffer, uint16_t pos, uint16_t value)
{
    buffer[pos] = (uint8_t)value;
    buffer[pos + 1] = (uint8_t)(value >> 8);
}

void btctler_reverse_bytes(const uint8_t *src, uint8_t *dst, int len)
{
    for (int i = 0; i < len; i++) dst[len - 1 - i] = src[i];
}

void btctler_reverse_128(const uint8_t *src, uint8_t *dst)
{
    btctler_reverse_bytes(src, dst, 16);
}

char btctler_char_for_nibble(int nibble)
{
    if (nibble < 10) return (char)(nibble + 48);
    if (nibble - 10 < 6) return (char)(nibble + 55);
    return 63;
}

static char btctler_char_for_high_nibble(int value) { return btctler_char_for_nibble((value >> 4) & 15); }
static char btctler_char_for_low_nibble(int value) { return btctler_char_for_nibble(value & 15); }

/* "AA:BB:CC:DD:EE:FF" of a 6-byte address stored least significant byte
 * first, in a buffer shared by all callers. */
char *btctler_bd_addr_to_str(const uint8_t *addr)
{
    char *p = bd_addr_to_str_buffer;
    for (int i = 6; i > 0; i--) {
        int value = addr[i - 1];
        *p = btctler_char_for_high_nibble(value);
        p[1] = btctler_char_for_low_nibble(value);
        p[2] = ':';
        p += 3;
    }
    bd_addr_to_str_buffer[17] = 0;
    return bd_addr_to_str_buffer;
}

static uint8_t crc8(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0xff;
    for (uint16_t i = 0; i != len; i++) crc = crc8table[data[i] ^ crc];
    return crc;
}

uint8_t btctler_crc8(uint8_t *data, uint16_t len)
{
    return crc8(data, len);
}

uint8_t btctler_crc8_check(uint8_t *data, uint16_t len, uint8_t check_sum)
{
    return (uint8_t)((crc8(data, len) ^ check_sum) != 0xff);
}

uint32_t btctler_get_rand_from_assign_range(uint32_t rand, uint32_t min, uint32_t max)
{
    if (max > min) return rand % (max - min) + min;
    return max;
}

/* 1 if any byte is non-zero, 0 if all are zero. */
uint8_t btctler_memcmp_zero(const void *target, uint32_t size)
{
    const uint8_t *p = target;
    while (size != 0) {
        size--;
        if (*p++ != 0) return 1;
    }
    return 0;
}

#ifdef UNITTEST
#include <string.h>
#include "test.h"
int main(void)
{
    static const uint8_t b[8] = { 0x11, 0x22, 0x83, 0xf4, 0x05, 0x66, 0x77, 0x88 };
    /* little-endian reads at an offset */
    CHECK(btctler_little_endian_read_16(b, 0) == 0x2211);
    CHECK(btctler_little_endian_read_16(b, 2) == 0xf483);
    CHECK(btctler_little_endian_read_24(b, 1) == 0xf48322);
    CHECK(btctler_little_endian_read_32(b, 0) == 0xf4832211u);
    CHECK(btctler_little_endian_read_32(b, 3) == 0x776605f4u);
    CHECK(btctler_little_endian_read_32(b, 4) == 0x88776605u);
    uint8_t out[6] = { 0xee, 0xee, 0xee, 0xee, 0xee, 0xee };
    btctler_little_endian_store_16(out, 1, 0xa1b2);
    CHECK(out[0] == 0xee && out[1] == 0xb2 && out[2] == 0xa1 && out[3] == 0xee);
    btctler_little_endian_store_16(out, 0, 0x0102);
    CHECK(out[0] == 2 && out[1] == 1);
    /* reverse */
    uint8_t rev[16], src[16];
    for (int i = 0; i < 16; i++) src[i] = i;
    btctler_reverse_128(src, rev);
    for (int i = 0; i < 16; i++) CHECK(rev[i] == 15 - i);
    memset(rev, 0xee, 16);
    btctler_reverse_bytes(src, rev, 3);
    CHECK(rev[0] == 2 && rev[1] == 1 && rev[2] == 0 && rev[3] == 0xee);
    btctler_reverse_bytes(src, rev, 0); btctler_reverse_bytes(src, rev, -4);
    CHECK(rev[0] == 2 && rev[3] == 0xee);
    btctler_reverse_bytes(src, rev, 1); CHECK(rev[0] == 0);
    /* nibbles: digits, upper-case letters, '?' beyond 15 (and below 0 the digit rule applies) */
    CHECK(btctler_char_for_nibble(0) == '0' && btctler_char_for_nibble(9) == '9');
    CHECK(btctler_char_for_nibble(10) == 'A' && btctler_char_for_nibble(15) == 'F');
    CHECK(btctler_char_for_nibble(16) == '?' && btctler_char_for_nibble(255) == '?');
    CHECK(btctler_char_for_nibble(-1) == '/');
    /* address text: bytes printed from the last to the first */
    {
        static const uint8_t a[6] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab };
        char *s = btctler_bd_addr_to_str(a);
        CHECK(!strcmp(s, "AB:89:67:45:23:01") && strlen(s) == 17);
        static const uint8_t z[6] = { 0 };
        char *t = btctler_bd_addr_to_str(z);
        CHECK(t == s && !strcmp(s, "00:00:00:00:00:00"));      /* one shared buffer */
        static const uint8_t f[6] = { 0xff, 0xfe, 0xfd, 0xfc, 0xfb, 0xfa };
        CHECK(!strcmp(btctler_bd_addr_to_str(f), "FA:FB:FC:FD:FE:FF"));
    }
    /* CRC-8: the 07.10 frames, F9 03 3F 01 1C F9 (SABM, DLCI 0) and F9 03 73 01 D7 F9 (UA), FCS = 0xff - crc */
    {
        uint8_t sabm[3] = { 0x03, 0x3f, 0x01 }, ua[3] = { 0x03, 0x73, 0x01 };
        CHECK(btctler_crc8(sabm, 3) == (uint8_t)(0xff - 0x1c));
        CHECK(btctler_crc8(ua, 3) == (uint8_t)(0xff - 0xd7));
        CHECK(btctler_crc8_check(sabm, 3, 0x1c) == 0);
        CHECK(btctler_crc8_check(ua, 3, 0xd7) == 0);
        CHECK(btctler_crc8_check(sabm, 3, 0xd7) == 1 && btctler_crc8_check(sabm, 3, 0x1d) == 1);
        CHECK(btctler_crc8(sabm, 0) == 0xff);
        /* one byte i from the initial 0xff goes through table[i ^ 0xff] */
        for (int i = 0; i < 256; i++) {
            uint8_t one = (uint8_t)i;
            CHECK(btctler_crc8(&one, 1) == crc8table[i ^ 0xff]);
        }
        /* the table against the bitwise definition: MSB-first CRC-8 (x^8+x^2+x+1) over
         * bit-reversed input and output */
        for (int i = 0; i < 256; i++) {
            int in = 0, c;
            for (int k = 0; k < 8; k++) in |= ((i >> k) & 1) << (7 - k);
            c = in;
            for (int k = 0; k < 8; k++) c = c & 0x80 ? ((c << 1) ^ 0x07) & 0xff : (c << 1) & 0xff;
            int r = 0;
            for (int k = 0; k < 8; k++) r |= ((c >> k) & 1) << (7 - k);
            CHECK(crc8table[i] == r);
        }
    }
    /* range */
    CHECK(btctler_get_rand_from_assign_range(100, 10, 20) == 10);
    CHECK(btctler_get_rand_from_assign_range(107, 10, 20) == 17);
    CHECK(btctler_get_rand_from_assign_range(0xffffffffu, 5, 8) == 0xffffffffu % 3 + 5);
    CHECK(btctler_get_rand_from_assign_range(7, 20, 20) == 20);
    CHECK(btctler_get_rand_from_assign_range(7, 30, 20) == 20);
    CHECK(btctler_get_rand_from_assign_range(7, 0, 1) == 0);
    /* memcmp_zero */
    {
        uint8_t z[5] = { 0, 0, 0, 0, 0 };
        CHECK(btctler_memcmp_zero(z, 5) == 0 && btctler_memcmp_zero(z, 0) == 0);
        z[4] = 1; CHECK(btctler_memcmp_zero(z, 5) == 1 && btctler_memcmp_zero(z, 4) == 0);
        z[0] = 0x80; CHECK(btctler_memcmp_zero(z, 1) == 1);
    }
    return TEST_DONE("btctrler_btcontroller_util");
}
#endif
