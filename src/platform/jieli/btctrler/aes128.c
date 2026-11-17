/* AES-128 block entry and the SDK's diagnostic vectors. */
#include "btctrler/glue.h"
#include <string.h>

extern void printf_buf(const u8 *buf, int size);
extern void log_print(int level, const char *file, const char *format, ...);

void aes128_start(u8 *key, u8 *plaintext, u8 *encrypt)
{
    local_irq_disable();
    aes128_start_enc(key, plaintext, encrypt);
    local_irq_enable();
}

static const u8 aes128_test_key[16] = {
    0x4c,0x68,0x38,0x41,0x39,0xf5,0x74,0xd8,0x36,0xbc,0xf3,0x4e,0x9d,0xfb,0x01,0xbf
};
static const u8 aes128_test_plaintext[16] = {
    0x02,0x13,0x24,0x35,0x46,0x57,0x68,0x79,0xac,0xbd,0xce,0xdf,0xe0,0xf1,0x02,0x13
};
static const u8 aes128_test_ciphertext[16] = {
    0x99,0xad,0x1b,0x52,0x26,0xa3,0x7e,0x3e,0x05,0x8e,0x3b,0x8e,0x27,0xc2,0xc6,0x66
};
static const u8 aes128_test_irk[16] = {
    0xec,0x12,0x34,0xa3,0x57,0xc8,0xad,0x05,0x34,0x10,0x10,0xa6,0x0a,0x39,0x7d,0x9b
};
static const u8 aes128_test_prand[16] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0x70,0x81,0x94
};
static const u8 aes128_test_ah[16] = {
    0x15,0x9d,0x5f,0xb7,0x2e,0xbe,0x23,0x11,0xa4,0x8c,0x1b,0xdc,0xc4,0x0d,0xfb,0xaa
};

void aes128_test(void)
{
    u8 key[16], plaintext[16], encrypt[16], _encrypt[16], _key[16], _plaintext[16];
    log_print(2, NULL, "[Info]: [AES128]aes128_test\r\n\r\n");
    memcpy(key, aes128_test_key, 16);
    memcpy(plaintext, aes128_test_plaintext, 16);
    btctler_reverse_128(key, _key);
    btctler_reverse_128(plaintext, _plaintext);
    local_irq_disable();
    aes128_start_enc(_key, _plaintext, encrypt);
    local_irq_enable();
    btctler_reverse_128(encrypt, _key);
    if (memcmp(aes128_test_ciphertext, _key, 16) != 0)
        btctrler_assert_failed("!memcmp(enc, _key, 16) AES128 encode err!", 0);

    /* The original diagnostic compares a second stack buffer that it never
     * initializes.  Keep the check and its assertion path as in the SDK. */
    btctler_reverse_128(_encrypt, _key);
    if (memcmp(plaintext, _key, 16) != 0)
        btctrler_assert_failed("!memcmp(plaintext, _key, 16) AES128 decode err!", 0);
    log_print(2, NULL, "[Info]: [AES128]ah test\r\n\r\n");
    btctler_reverse_128(aes128_test_irk, _key);
    btctler_reverse_128(aes128_test_prand, _plaintext);
    local_irq_disable();
    aes128_start_enc(_key, _plaintext, encrypt);
    local_irq_enable();
    btctler_reverse_128(encrypt, _key);
    if (memcmp(aes128_test_ah, _key, 16) != 0)
        btctrler_assert_failed("!memcmp(ah, _key, 16) ah encode err!", 0);
    for (;;) { }
}

void aes_ccm_test(void)
{
    u8 tag[16] = {0}, nonce[13] __attribute__((aligned(4))) = {0}, payload[251] = {0};
    u8 head = 3, skd[16] = {
        0x66,0xc6,0xc2,0x27,0x8e,0x3b,0x8e,0x05,0x3e,0x7e,0xa3,0x26,0x52,0x1b,0xad,0x99
    };
    static const u8 mic1[4] = {0xcd,0xa7,0xf4,0x48};
    static const u8 mic2[4] = {0x4c,0x13,0xa4,0x15};
    u8 cipher1 = 0x9f, cipher2 = 0xa3;
    u8 key[16];
    nonce[4] = 0x80;
    nonce[5] = 0x24; nonce[6] = 0xab; nonce[7] = 0xdc; nonce[8] = 0xba;
    nonce[9] = 0xbe; nonce[10] = 0xba; nonce[11] = 0xaf; nonce[12] = 0xde;
    log_print(2, NULL, "[Info]: [AES128]aes-ccm_test\r\n\r\n");
    btctler_reverse_128(skd, key);
    ccm_memory_v2(key, nonce, &head, 1, payload, 1, &cipher1, tag, 4, 1);
    log_print(2, NULL, "[Info]: [AES128]MIC\r\n");
    printf_buf(tag, 4);
    if (memcmp(tag, mic1, 4) == 0) log_print(2, NULL, "[Info]: [AES128]--------Decrypt MIC Correct\r\n");
    log_print(2, NULL, "[Info]: [AES128]PT\r\n");
    printf_buf(payload, 1);
    if (payload[0] == 6) log_print(2, NULL, "[Info]: [AES128]--------Decrypt Data Correct\r\n");

    memset(nonce, 0, 5);
    nonce[5] = 0x24; nonce[6] = 0xab; nonce[7] = 0xdc; nonce[8] = 0xba;
    nonce[9] = 0xbe; nonce[10] = 0xba; nonce[11] = 0xaf; nonce[12] = 0xde;
    ccm_memory_v2(key, nonce, &head, 1, payload, 1, &cipher2, tag, 4, 1);
    log_print(2, NULL, "[Info]: [AES128]MIC\r\n");
    printf_buf(tag, 4);
    if (memcmp(tag, mic2, 4) == 0) log_print(2, NULL, "[Info]: [AES128]--------Decrypt MIC Correct\r\n");
    log_print(2, NULL, "[Info]: [AES128]PT\r\n");
    printf_buf(payload, 1);
    if (payload[0] == 6) log_print(2, NULL, "[Info]: [AES128]--------Decrypt Data Correct\r\n");
    for (;;) { }
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
