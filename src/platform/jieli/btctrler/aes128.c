/* The SDK's AES-128 block encrypt entry (aes128.c): runs the hardware
 * engine (aes_hw.c, aes128_start_enc) with interrupts off.  Key, plaintext
 * and result are 16 bytes, most significant byte first.  The SDK's self
 * tests (aes128_test, aes_ccm_test) have no caller and are not carried over. */
#include "btctrler/glue.h"

void aes128_start(u8 *key, u8 *plaintext, u8 *encrypt)
{
    local_irq_disable();
    aes128_start_enc(key, plaintext, encrypt);
    local_irq_enable();
}

#ifdef UNITTEST
#include "test.h"
static int step;
static u8 *seen_key, *seen_plain, *seen_enc;
void local_irq_disable(void) { CHECK(step == 0); step = 1; }
void local_irq_enable(void) { CHECK(step == 2); step = 3; }
int aes128_start_enc(unsigned char *key, unsigned char *plaintext, unsigned char *encrypt)
{
    CHECK(step == 1); step = 2;
    seen_key = key; seen_plain = plaintext; seen_enc = encrypt;
    return 0;
}
int main(void)
{
    u8 k[16], p[16], e[16];
    /* The engine runs inside exactly one interrupt-off section. */
    aes128_start(k, p, e);
    CHECK(step == 3 && seen_key == k && seen_plain == p && seen_enc == e);
    return TEST_DONE("btctrler_aes128");
}
#endif
