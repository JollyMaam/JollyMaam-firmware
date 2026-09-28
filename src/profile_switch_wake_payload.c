#include <stdint.h>

#include "profile_switch_wake_payload.h"
#include "profile_switch_wake_code.h"

/* The 32 whitening states the reference walks, in the order it walks them.
 * The order matters: successive bursts are meant to land on successive
 * entries.  Copied unchanged from the reference's k_whitening_seed_cycle. */
static const uint8_t wake_seed_cycle[WAKE_WHITEN_STATES] = {
    0x69, 0x01, 0x31, 0x7d, 0x05, 0x5b, 0x6b, 0x03,
    0x5f, 0x37, 0x07, 0x59, 0x21, 0x6d, 0x5d, 0x35,
    0x7b, 0x13, 0x23, 0x6f, 0x17, 0x49, 0x79, 0x11,
    0x4d, 0x25, 0x15, 0x4b, 0x33, 0x7f, 0x4f, 0x27,
};

uint8_t wake_whiten_seed(uint8_t index)
{
    return wake_seed_cycle[index % WAKE_WHITEN_STATES];
}

/* The whitening generator: a 7-bit LFSR whose output is its low bit, with
 * feedback from bits 0 and 4.  Bits are shifted out least significant first,
 * which is why the cancel below indexes bits within a byte the same way. */
static uint8_t wake_whiten_advance(uint8_t reg, uint8_t *out_bit)
{
    const uint8_t feedback = (uint8_t)((reg ^ (reg >> 4)) & 1U);

    if (out_bit) {
        *out_bit = (uint8_t)(reg & 1U);
    }
    return (uint8_t)((reg >> 1) | (feedback << 6));
}

void wake_whiten_cancel(uint8_t *body, uint16_t length, uint8_t seed, uint8_t skip)
{
    uint8_t reg = (uint8_t)(seed & 0x7fU);
    uint32_t bit;
    uint32_t count = (uint32_t)length * 8U;

    for (bit = 0; bit < skip; ++bit) {
        reg = wake_whiten_advance(reg, NULL);
    }
    for (bit = 0; bit < count; ++bit) {
        uint8_t value = 0;

        reg = wake_whiten_advance(reg, &value);
        if (value) {
            body[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
        }
    }
}

void wake_build_phase1(const uint8_t address[6], uint8_t *body, uint16_t length)
{
    uint8_t code[4];
    uint32_t value;
    uint32_t rotated;
    uint8_t repeated[4];
    uint16_t i;

    wake_code(address, code);
    value = (uint32_t)code[0] | ((uint32_t)code[1] << 8) |
            ((uint32_t)code[2] << 16) | ((uint32_t)code[3] << 24);
    rotated = (value >> 10) | (value << 22);
    repeated[0] = (uint8_t)(rotated & 0xffU);
    repeated[1] = (uint8_t)((rotated >> 8) & 0xffU);
    repeated[2] = (uint8_t)((rotated >> 16) & 0xffU);
    repeated[3] = (uint8_t)((rotated >> 24) & 0xffU);

    for (i = 0; i < length; ++i) {
        body[i] = repeated[i % 4U];
    }
    /* The reference clears the last three bytes of a full-length body; it is
     * reproduced here so this port and the reference put identical bits on
     * the air.  Why is not established - a capture of a real controller
     * would say, and the sweep below will show it either way. */
    if (length >= WAKE_BODY_MAX) {
        body[WAKE_BODY_MAX - 3U] = 0;
        body[WAKE_BODY_MAX - 2U] = 0;
        body[WAKE_BODY_MAX - 1U] = 0;
    }
}
