#include <stdint.h>

#include "wakeup_code.h"

/* Bluetooth Core Specification, Baseband Specification: PN is the 64-bit
 * pseudo-random sequence and 0x37CD0EB67 is the (63,30) BCH generator
 * polynomial multiplied by (D + 1) as a polynomial over GF(2).  The vendor
 * controller library carries the same two constants in lmp_utils.c.o. */
#define WAKE_PN        UINT64_C(0x83848D96BBCC54FC)
#define WAKE_GENERATOR UINT64_C(0x585713DA9)

void wake_lap(const uint8_t address[6], uint8_t lap[3])
{
    lap[0] = address[0];
    lap[1] = address[1];
    lap[2] = address[2];
}

/* Polynomial division over GF(2); addition and subtraction are XOR. */
static uint64_t wake_gf2_remainder(uint64_t value, uint64_t divisor, int top_bit)
{
    int bit;

    for (bit = top_bit; bit >= 34; --bit) {
        if (value & (UINT64_C(1) << bit)) {
            value ^= divisor << (bit - 34);
        }
    }
    return value;
}

uint64_t wake_sync_word(const uint8_t address[6])
{
    const uint32_t lap = (uint32_t)address[0] |
                         ((uint32_t)address[1] << 8) |
                         ((uint32_t)address[2] << 16);
    /* A 6-bit Barker sequence chosen by the LAP's most significant bit makes
     * the 30-bit input to the code; the upper 30 bits of PN are then laid
     * over it before encoding. */
    const uint32_t information =
        ((lap & UINT32_C(0x800000)) ? UINT32_C(0x13000000)
                                    : UINT32_C(0x2C000000)) | lap;
    const uint32_t encoded = information ^ (uint32_t)(WAKE_PN >> 34);
    const uint64_t shifted = (uint64_t)encoded << 34;
    const uint64_t codeword = shifted ^ wake_gf2_remainder(shifted, WAKE_GENERATOR, 63);

    return codeword ^ WAKE_PN;
}

void wake_parity(const uint8_t address[6], uint8_t parity[5])
{
    const uint64_t value = wake_sync_word(address) & UINT64_C(0x3FFFFFFFF);
    unsigned i;

    for (i = 0; i < 4U; ++i) {
        parity[i] = (uint8_t)(value >> (8U * i));
    }
    parity[4] = (uint8_t)(value >> 32);
}

void wake_parity_inverted(const uint8_t address[6], uint8_t parity[5])
{
    wake_parity(address, parity);
    parity[0] ^= 0xffU;
    parity[1] ^= 0xffU;
    parity[2] ^= 0xffU;
    parity[3] ^= 0xffU;
    /* Bits 32..33 ride above the inverted 32 and are left alone. */
}

void wake_code(const uint8_t address[6], uint8_t code[4])
{
    const uint32_t value = (uint32_t)~wake_sync_word(address);
    unsigned i;

    for (i = 0; i < 4U; ++i) {
        code[i] = (uint8_t)(value >> (8U * i));
    }
}
