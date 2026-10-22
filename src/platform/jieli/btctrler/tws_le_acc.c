/* The SDK's TWS LE access-address helpers.  Keep its 32-bit LFSR arithmetic,
 * bit-pattern scoring, and four separate little-endian address-byte loads. */
#include "btctrler/glue.h"

/* ble_sync_generation examines overlapping 7-bit windows and transition
 * counts from the 32-bit SDK word.  Use u32 explicitly: host UNITTEST builds
 * have a 64-bit unsigned long, while the SDK's pi32v2 long is 32 bits. */
unsigned char ble_sync_generation(long unsigned int sword)
{
    u32 word = (u32)sword;
    unsigned int score = 0;
    unsigned int count = 0;

    for (unsigned int bit = 0; bit < 25; ++bit) {
        u32 window = word >> bit;
        if ((window & 0x7fu) == 0 || (window & 0x7fu) == 0x7fu)
            ++count;
    }
    if (count != 0)
        score |= 1;

    const u32 sync_word = 0x8e89bed6u;
    if (word == sync_word)
        score |= 2;

    count = 0;
    u32 difference = word ^ sync_word;
    for (unsigned int bit = 0; bit < 32; ++bit)
        count += (difference & ((u32)1 << bit)) != 0;
    if (count == 1)
        score |= 4;

    /* Keep the SDK's odd unnormalized comparisons: the shift-and-mask yields
     * bits 8..15, while the later operands retain their original positions. */
    u32 shifted = (word << 8) & 0xff00u;
    if (shifted == (word & 0xff00u) &&
        shifted == (word & 0x00ff0000u) &&
        shifted == (word & 0xff000000u))
        score |= 8;

    count = 0;
    for (unsigned int bit = 0; bit < 31; ++bit)
        count += ((word >> bit) & 1u) != ((word >> (bit + 1)) & 1u);
    if (count > 24)
        score |= 16;

    count = 0;
    for (unsigned int bit = 26; bit < 31; ++bit)
        count += ((word >> bit) & 1u) != ((word >> (bit + 1)) & 1u);
    if (count < 2)
        score |= 32;

    count = 0;
    for (unsigned int bit = 0; bit < 8; ++bit)
        count += ((word >> bit) & 1u) != 0;
    if (count < 3)
        score |= 64;

    count = 0;
    for (unsigned int bit = 0; bit < 15; ++bit)
        count += ((word >> bit) & 1u) != ((word >> (bit + 1)) & 1u);
    if (count > 11)
        score |= 128;

    return (unsigned char)score;
}

long unsigned int acc_rand(long unsigned int seed)
{
    u32 word = (u32)seed;
    u32 shifted = word << 1;

    /* The SDK tests the shifted value for zero as well as the old top bit;
     * both cases take the polynomial-XOR path, including seed zero. */
    if (shifted == 0 || (word & 0x80000000u) != 0)
        shifted ^= 0x04c11db7u;
    return (long unsigned int)shifted;
}

long unsigned int tws_le_acc_generation(u8 *addr)
{
    /* Keep the four byte reads and their little-endian assembly as in the
     * SDK, rather than using a possibly unaligned or host-endian word load. */
    u32 word = (u32)addr[0] |
               ((u32)addr[1] << 8) |
               ((u32)addr[2] << 16) |
               ((u32)addr[3] << 24);
    unsigned char status = 1;

    while (status != 0) {
        word = (u32)acc_rand(word);
        status = ble_sync_generation(word);
    }
    return (long unsigned int)word;
}

#ifdef UNITTEST
#include "test.h"
int main(void)
{
    /* Hand-derived from the SDK IR's 32-bit shifts, masks and comparisons. */
    CHECK(ble_sync_generation(0x00000000u) == 0x69);
    CHECK(ble_sync_generation(0xffffffffu) == 0x21);
    CHECK(ble_sync_generation(0x8e89bed6u) == 0x02);
    CHECK(ble_sync_generation(0x8e89bed7u) == 0x04);
    CHECK(ble_sync_generation(0x01010101u) == 0x61);
    CHECK(ble_sync_generation(0xaaaaaaaau) == 0x90);
    CHECK(ble_sync_generation(0x12345678u) == 0x00);

    CHECK(acc_rand(0) == 0x04c11db7u); /* shifted-zero polynomial path */
    CHECK(acc_rand(1) == 2);
    CHECK(acc_rand(0x7fffffffu) == 0xfffffffeu);
    CHECK(acc_rand(0x80000000u) == 0x04c11db7u);
    CHECK(acc_rand(0xffffffffu) == 0xfb3ee249u);

    const u8 first[4] = { 0x78, 0x56, 0x34, 0x12 };
    const u8 walk[4] = { 0x04, 0x03, 0x02, 0x01 };
    const u8 zero[4] = { 0, 0, 0, 0 };
    CHECK(tws_le_acc_generation((u8 *)first) == 0x2468acf0u);
    CHECK(tws_le_acc_generation((u8 *)walk) == 0x1b0866dcu);
    CHECK(tws_le_acc_generation((u8 *)zero) == 0x09823b6eu);
    return TEST_DONE("btctrler_tws_le_acc");
}
#endif
