#ifndef CYCLONE2_WAKEUP_CODE_H
#define CYCLONE2_WAKEUP_CODE_H

/* Access-code maths follows /shared/github.com-608-NS1-Wakeup-Demo.
 * Its worked examples already invert the lower sync-word bits. */

#include <stdint.h>

/* The 24-bit LAP, least significant byte first. */
void wake_lap(const uint8_t address[6], uint8_t lap[3]);

uint64_t wake_sync_word(const uint8_t address[6]);

/* The standard 34-bit BCH parity as the controller writes it: parity[0..3]
 * are the low 32 bits least significant byte first, parity[4] holds bits
 * 32..33.  This is the value __get_access_addr() produces for the same
 * address, so a comparison against a live frame checks the port.
 */
void wake_parity(const uint8_t address[6], uint8_t parity[5]);

void wake_parity_inverted(const uint8_t address[6], uint8_t parity[5]);

/* The 4-byte wake-up code, little endian: (~S) & 0xFFFFFFFF. */
void wake_code(const uint8_t address[6], uint8_t code[4]);

#endif
