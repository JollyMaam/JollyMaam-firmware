#ifndef TEST_H
#define TEST_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>


static int test_failures;

#define CHECK(c) do { \
    if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); test_failures++; } \
} while (0)

/* An ATT database as BTstack's compile_gatt.py lays it out: the 16-bit UUID
 * of a handle's entry, 0x10000 for a 128-bit one, 0 for no such handle. */
static uint32_t test_att_uuid(const uint8_t *db, uint16_t handle)
{
    const uint8_t *p = db + 1;

    for (;;) {
        uint16_t size = (uint16_t)(p[0] | p[1] << 8), flags = (uint16_t)(p[2] | p[3] << 8);

        if (!size) return 0;
        if ((p[4] | p[5] << 8) == handle) return flags & 0x0200 ? 0x10000 : (uint32_t)(p[6] | p[7] << 8);
        p += size;
    }
}

#define TEST_DONE(name) \
    (test_failures ? 1 : (printf("%s: ok\n", name), 0))

#endif
