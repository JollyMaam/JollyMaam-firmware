/*
 * Host check for the compact AES-128 in src/aes128.c.
 *
 *     make aes128-test
 *
 * Checks the FIPS-197 appendix C.1 vector, then compares random keys and
 * blocks against BTstack's table-driven rijndael.c, which the firmware used
 * before.  It never touches the pad.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aes128.h"
#include "rijndael.h"

int main(void)
{
    static const uint8_t key[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    static const uint8_t plain[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    static const uint8_t cipher[16] = {
        0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
        0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a,
    };
    uint8_t out[16], ref[16], k[16], p[16];
    int failures = 0;

    aes128_encrypt(key, plain, out);
    if (memcmp(out, cipher, 16) != 0) {
        printf("FAIL FIPS-197 C.1\n");
        failures++;
    }

    srand(1);
    for (int n = 0; n < 10000; n++) {
        uint32_t rk[RKLENGTH(128)];
        for (int i = 0; i < 16; i++) {
            k[i] = (uint8_t)rand();
            p[i] = (uint8_t)rand();
        }
        rijndaelEncrypt(rk, rijndaelSetupEncrypt(rk, k, 128), p, ref);
        aes128_encrypt(k, p, out);
        if (memcmp(out, ref, 16) != 0) {
            printf("FAIL random vector %d\n", n);
            failures++;
            break;
        }
    }

    printf("aes128: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
