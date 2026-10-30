#ifndef JM_JIELI_BANKCB_H
#define JM_JIELI_BANKCB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Transform BANKCB in place, checking each bank header CRC and data bounds.
 * False means invalid input; the buffer may already be partially transformed.
 * The caller must discard it on failure. No allocations or device access. */
bool jm_jieli_scramble_bankcb(uint8_t *data, size_t size);
bool jm_jieli_descramble_bankcb(uint8_t *data, size_t size);
#endif
