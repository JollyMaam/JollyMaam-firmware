#ifndef CYCLONE2_WAKEUP_CODE_H
#define CYCLONE2_WAKEUP_CODE_H

/*
 * The Switch 1 wake-up access code, ported from the ESP32 reference in
 * /shared/github.com-608-NS1-Wakeup-Demo (docs/wakeup-protocol.en.md and
 * src/wake/wakeup_code.c).
 *
 * A controller that wakes a sleeping Switch does not page it.  It transmits
 * the standard BR/EDR access code for the console's address with the lower 32
 * bits of the 64-bit sync word inverted:
 *
 *     P34_wakeup = (S & 0x3FFFFFFFF) ^ 0xFFFFFFFF
 *
 * No BD_ADDR produces that sync word - the (64,30) BCH mapping from LAP to
 * sync word is injective - so this is not reachable through any page, connect
 * or inquiry command.  It needs the baseband's own access-code registers,
 * which src/wake/wake_radio.c writes.
 *
 * Address order: every address here is least significant byte first, which is
 * how this firmware stores addresses (see src/settings.c and the comment on
 * bt_set_mac_addr).  The value printed as aa:bb:cc:dd:ee:ff is the array
 * {0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa}, and its LAP - the low 24 bits of the
 * address - is address[0..2].
 */

#include <stdint.h>

/* The 24-bit LAP, least significant byte first. */
void wake_lap(const uint8_t address[6], uint8_t lap[3]);

/* The 64-bit sync word S. */
uint64_t wake_sync_word(const uint8_t address[6]);

/* The standard 34-bit BCH parity as the controller writes it: parity[0..3]
 * are the low 32 bits least significant byte first, parity[4] holds bits
 * 32..33.  This is the value __get_access_addr() produces for the same
 * address, so a comparison against a live frame checks the port.
 */
void wake_parity(const uint8_t address[6], uint8_t parity[5]);

/* The same parity with the lower 32 bits inverted: what the wake-up access
 * code needs.  This is the value handed to __write_reg_bch(). */
void wake_parity_inverted(const uint8_t address[6], uint8_t parity[5]);

/* The 4-byte wake-up code, little endian: (~S) & 0xFFFFFFFF. */
void wake_code(const uint8_t address[6], uint8_t code[4]);

#endif
