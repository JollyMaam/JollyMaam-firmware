#ifndef CYCLONE2_AES128_H
#define CYCLONE2_AES128_H

/* AES-128 encryption of one block (FIPS-197 byte order), for the few key
 * derivations the profiles do.  Byte-oriented and slow, but a fraction of the
 * size of BTstack's table-driven rijndael.c; tools/aes128_test.c checks it. */

#include <stdint.h>

void aes128_encrypt(const uint8_t key[16], const uint8_t in[16],
                    uint8_t out[16]);

#endif
