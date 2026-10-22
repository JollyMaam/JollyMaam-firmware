/* The SDK's weak LE public-key and DH-key hooks are unavailable stubs. */
#include "btctrler/glue.h"

u8 __attribute__((weak)) ll_read_local_p256_public_key(u8 *key)
{
    (void)key;
    return 0;
}

u8 __attribute__((weak)) ll_generate_dh_key(const u8 *data, u8 *dhkey)
{
    (void)data;
    (void)dhkey;
    return 0;
}

#ifdef UNITTEST
#include "test.h"

int main(void)
{
    u8 key[64];
    for (unsigned i = 0; i < sizeof(key); i++) key[i] = (u8)i;
    u8 before[sizeof(key)];
    memcpy(before, key, sizeof(key));

    CHECK(ll_read_local_p256_public_key(key) == 0);
    CHECK(memcmp(key, before, sizeof(key)) == 0);
    CHECK(ll_generate_dh_key(key, key + 32) == 0);
    CHECK(memcmp(key, before, sizeof(key)) == 0);
    CHECK(ll_read_local_p256_public_key(NULL) == 0);
    CHECK(ll_generate_dh_key(NULL, NULL) == 0);
    return TEST_DONE("btctrler_ll_keys");
}
#endif
