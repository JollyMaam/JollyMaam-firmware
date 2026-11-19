/* The SDK's driver for the BR23 AES block (JL_AES at 0x1e4300, aes_hw.c):
 * single-block AES-128 (aes128_start_enc) and AES-CCM for the BLE link
 * (ccm_memory_v2).  Register accesses are volatile and in the SDK's order,
 * including one register read per result byte.  aes128_start_enc loads key
 * and plaintext as big-endian words from byte 15 down; ccm_memory_v2 loads
 * little-endian words from byte 0 up and returns the MIC big-endian.
 * AES IRQ setup and the SDK's hardware self-test are also retained. */
#include <stdint.h>
#include "btctrler/glue.h"

void printf_buf(const u8 *buf, int size);
void log_print(int level, const char *file, const char *format, ...);
#ifdef UNITTEST
extern u8 log_tag_const_i_AES;
#else
extern const u8 log_tag_const_i_AES;
#endif

#define AES_LOG(...) do { if (log_tag_const_i_AES) log_print(2, (const char *)0, __VA_ARGS__); } while (0)

extern void request_irq(u8 index, u8 priority, void (*handler)(void), u8 cpu_id);

#ifdef UNITTEST
/* The test supplies a model of the register file. */
u32 aes_reg_read(unsigned offset);
void aes_reg_write(unsigned offset, u32 value);
#define AES_READ(field) aes_reg_read(offsetof(JL_AES_TypeDef, field))
#define AES_WRITE(field, value) aes_reg_write(offsetof(JL_AES_TypeDef, field), (value))
#else
#define JL_AES ((JL_AES_TypeDef *)0x1e4300)
#define AES_READ(field) (JL_AES->field)
#define AES_WRITE(field, value) (JL_AES->field = (value))
#endif

_Static_assert(sizeof(((JL_AES_TypeDef *)0)->CON) == 4, "AES register width");
_Static_assert(offsetof(JL_AES_TypeDef, CON) == 0, "AES CON");
_Static_assert(offsetof(JL_AES_TypeDef, DATIN) == 4, "AES DATIN");
_Static_assert(offsetof(JL_AES_TypeDef, KEY) == 8, "AES KEY");
_Static_assert(offsetof(JL_AES_TypeDef, ENCRES0) == 12, "AES ENCRES0");
_Static_assert(offsetof(JL_AES_TypeDef, ENCRES3) == 24, "AES ENCRES3");
_Static_assert(offsetof(JL_AES_TypeDef, NONCE) == 28, "AES NONCE");
_Static_assert(offsetof(JL_AES_TypeDef, HEADER) == 32, "AES HEADER");
_Static_assert(offsetof(JL_AES_TypeDef, SRCADR) == 36, "AES SRCADR");
_Static_assert(offsetof(JL_AES_TypeDef, DSTADR) == 40, "AES DSTADR");
_Static_assert(offsetof(JL_AES_TypeDef, CTCNT) == 44, "AES CTCNT");
_Static_assert(offsetof(JL_AES_TypeDef, TAGLEN) == 48, "AES TAGLEN");
_Static_assert(offsetof(JL_AES_TypeDef, TAGRES0) == 52, "AES TAGRES0");
_Static_assert(offsetof(JL_AES_TypeDef, TAGRES3) == 64, "AES TAGRES3");

/* Status of the last CCM run; the SDK clears it and never sets it. */
u8 ccm_status;

#ifdef UNITTEST
#define AES_IRQ
#else
#define AES_IRQ __attribute__((interrupt))
#endif

static void aes_irq_handler(void) AES_IRQ;
static void aes_irq_handler(void)
{
    u32 status = AES_READ(CON);
    if ((s32)status < 0)
        AES_WRITE(CON, AES_READ(CON) | 0x40000000u);
}

void aes_init(void)
{
    request_irq(40, 0, aes_irq_handler, 0);
}

/* The SDK diagnostic drives the block's CCM DMA path directly.  Its test
 * vectors and register order are preserved; callers must provide idle HW. */
void aes_hw_test(void)
{
    u8 source[27], destination[27];
    for (unsigned i = 0; i < sizeof source; ++i) source[i] = 0x54;
    for (unsigned i = 0; i < sizeof destination; ++i) destination[i] = 0x54;

    AES_WRITE(CTCNT, 27);
    AES_WRITE(KEY, 0x87e2fa74u);
    AES_LOG("[Info]: [AES]key 0 : 0x%x\r\n", 0x87e2fa74u);
    AES_WRITE(KEY, 0x36c3ca9du);
    AES_LOG("[Info]: [AES]key 1 : 0x%x\r\n", 0x36c3ca9du);
    AES_WRITE(KEY, 0x212776b7u);
    AES_LOG("[Info]: [AES]key 2 : 0x%x\r\n", 0x212776b7u);
    AES_WRITE(KEY, 0xbd493d5eu);
    AES_LOG("[Info]: [AES]key 3 : 0x%x\r\n", 0xbd493d5eu);
    AES_WRITE(CON, 0x810);
    while (AES_READ(CON) & 32) { }
    AES_WRITE(TAGLEN, 4);
    AES_WRITE(NONCE, 0x0000057c);
    AES_LOG("[Info]: [AES]nonce 0 : 0x%x\r\n", 0x0000057c);
    AES_WRITE(NONCE, 0xc1bb0600u);
    AES_LOG("[Info]: [AES]nonce 1 : 0x%x\r\n", 0xc1bb0600u);
    AES_WRITE(NONCE, 0x763bc635u);
    AES_LOG("[Info]: [AES]nonce 2 : 0x763bc635u\r\n", 0x763bc635u);
    AES_WRITE(NONCE, 0x5b);
    AES_LOG("[Info]: [AES]nonce 3 : 0x%x\r\n", 0x5bu);
    AES_WRITE(HEADER, 1);
    AES_WRITE(SRCADR, (u32)(uintptr_t)source);
    AES_WRITE(DSTADR, (u32)(uintptr_t)destination);
    AES_WRITE(CON, AES_READ(CON) | 0x10000);
    while ((s32)AES_READ(CON) >= 0) { }
    AES_WRITE(CON, AES_READ(CON) | 0x40000000u);
    if (log_tag_const_i_AES) {
        log_print(2, NULL, "[Info]: [AES]MIC : 0x%x\r\n", AES_READ(TAGRES0));
        printf_buf(destination, sizeof destination);
    }
    if (AES_READ(TAGRES0) != 0x5981008au && log_tag_const_i_AES)
        log_print(2, NULL, "[Info]: [AES]>>>>>>>>>>>>>>>>>>>>Error\r\n");

    AES_WRITE(SRCADR, (u32)(uintptr_t)destination);
    AES_WRITE(DSTADR, (u32)(uintptr_t)source);
    AES_WRITE(CON, AES_READ(CON) | 0x40000);
    while ((s32)AES_READ(CON) >= 0) { }
    AES_WRITE(CON, AES_READ(CON) | 0x40000000u);
    if (AES_READ(TAGRES0) != 0x5981008au && log_tag_const_i_AES)
        log_print(2, NULL, "[Info]: [AES]>>>>>>>>>>>>>>>>>Error2\r\n");
}

/* b[0] least significant. */
static u32 word_le(const u8 *b) { return (u32)b[2] << 16 | (u32)b[3] << 24 | (u32)b[1] << 8 | b[0]; }

int aes128_start_enc(unsigned char *key, unsigned char *plaintext, unsigned char *encrypt)
{
    if (log_tag_const_i_AES) {
        log_print(2, (const char *)0, "[Info]: [AES]key\r\n");
        printf_buf(key, 16);
    }
    u32 w;
    w = (u32)key[13] << 16 | (u32)key[12] << 24 | (u32)key[14] << 8 | key[15];
    AES_WRITE(KEY, w);
    AES_LOG("[Info]: [AES]Key 0 : %x \r\n", w);
    w = (u32)key[9] << 16 | (u32)key[8] << 24 | (u32)key[10] << 8 | key[11];
    AES_WRITE(KEY, w);
    AES_LOG("[Info]: [AES]Key 1 : %x \r\n", w);
    w = (u32)key[5] << 16 | (u32)key[4] << 24 | (u32)key[6] << 8 | key[7];
    AES_WRITE(KEY, w);
    AES_LOG("[Info]: [AES]Key 2 : %x \r\n", w);
    w = (u32)key[1] << 16 | (u32)key[0] << 24 | (u32)key[2] << 8 | key[3];
    AES_WRITE(KEY, w);
    AES_LOG("[Info]: [AES]Key 3 : %x \r\n", w);
    AES_WRITE(CON, 16);
    while (AES_READ(CON) & 32) ;
    if (log_tag_const_i_AES) {
        log_print(2, (const char *)0, "[Info]: [AES]plaintext\r\n");
        printf_buf(plaintext, 16);
    }
    w = (u32)plaintext[13] << 16 | (u32)plaintext[12] << 24 | (u32)plaintext[14] << 8 | plaintext[15];
    AES_WRITE(DATIN, w);
    AES_LOG("[Info]: [AES]pt 0 : %x \r\n", w);
    w = (u32)plaintext[9] << 16 | (u32)plaintext[8] << 24 | (u32)plaintext[10] << 8 | plaintext[11];
    AES_WRITE(DATIN, w);
    AES_LOG("[Info]: [AES]pt 1 : %x \r\n", w);
    w = (u32)plaintext[5] << 16 | (u32)plaintext[4] << 24 | (u32)plaintext[6] << 8 | plaintext[7];
    AES_WRITE(DATIN, w);
    AES_LOG("[Info]: [AES]pt 2 : %x \r\n", w);
    w = (u32)plaintext[1] << 16 | (u32)plaintext[0] << 24 | (u32)plaintext[2] << 8 | plaintext[3];
    AES_WRITE(DATIN, w);
    AES_LOG("[Info]: [AES]pt 3 : %x \r\n", w);
    AES_WRITE(CON, 1);
    while (AES_READ(CON) & 4) ;
    /* Each result byte is a separate register read, as in the SDK. */
    encrypt[0] = (u8)AES_READ(ENCRES0);
    encrypt[1] = (u8)(AES_READ(ENCRES0) >> 8);
    encrypt[2] = (u8)(AES_READ(ENCRES0) >> 16);
    encrypt[3] = (u8)(AES_READ(ENCRES0) >> 24);
    encrypt[4] = (u8)AES_READ(ENCRES1);
    encrypt[5] = (u8)(AES_READ(ENCRES1) >> 8);
    encrypt[6] = (u8)(AES_READ(ENCRES1) >> 16);
    encrypt[7] = (u8)(AES_READ(ENCRES1) >> 24);
    encrypt[8] = (u8)AES_READ(ENCRES2);
    encrypt[9] = (u8)(AES_READ(ENCRES2) >> 8);
    encrypt[10] = (u8)(AES_READ(ENCRES2) >> 16);
    encrypt[11] = (u8)(AES_READ(ENCRES2) >> 24);
    encrypt[12] = (u8)AES_READ(ENCRES3);
    encrypt[13] = (u8)(AES_READ(ENCRES3) >> 8);
    encrypt[14] = (u8)(AES_READ(ENCRES3) >> 16);
    encrypt[15] = (u8)(AES_READ(ENCRES3) >> 24);
    if (log_tag_const_i_AES) {
        log_print(2, (const char *)0, "[Info]: [AES]encrypt\r\n");
        printf_buf(encrypt, 16);
    }
    return 0;
}

void ccm_memory_v2(const unsigned char *key, const unsigned char *nonce, const unsigned char *header,
                   long unsigned int headerlen, unsigned char *pt, unsigned char ptlen,
                   unsigned char *ct, unsigned char *tag, unsigned char taglen, unsigned char direction)
{
    local_irq_disable();
#ifndef UNITTEST
    __asm__ volatile("csync;");
#endif
    if (log_tag_const_i_AES) {
        log_print(2, (const char *)0, "[Info]: [AES]key\r\n");
        printf_buf(key, 16);
    }
    AES_WRITE(CTCNT, ptlen);
    AES_LOG("[Info]: [AES]CNT : %x \r\n", (u32)ptlen);
    u32 w;
    w = word_le(key);
    AES_WRITE(KEY, w);
    AES_LOG("[Info]: [AES]Key 0 : %x \r\n", w);
    w = word_le(key + 4);
    AES_WRITE(KEY, w);
    AES_LOG("[Info]: [AES]Key 1 : %x \r\n", w);
    w = word_le(key + 8);
    AES_WRITE(KEY, w);
    AES_LOG("[Info]: [AES]Key 2 : %x \r\n", w);
    w = word_le(key + 12);
    AES_WRITE(KEY, w);
    AES_LOG("[Info]: [AES]Key 3 : %x \r\n", w);
    AES_WRITE(CON, 16);
    if (headerlen != 0) {
        /* The SDK asserts and carries on with the header flag set. */
        if (headerlen != 1) btctrler_assert_failed("headerlen == 1 Header length exceed", 0);
        AES_WRITE(CON, AES_READ(CON) | 2048);
    } else {
        AES_WRITE(CON, AES_READ(CON) & -2049);
    }
    while (AES_READ(CON) & 32) ;
    if (log_tag_const_i_AES) {
        log_print(2, (const char *)0, "[Info]: [AES]nonce\r\n");
        printf_buf(nonce, 13);
    }
    AES_WRITE(TAGLEN, taglen);
    AES_LOG("[Info]: [AES]TAGLEN : %x \r\n", (u32)taglen);
    w = word_le(nonce);
    AES_WRITE(NONCE, w);
    AES_LOG("[Info]: [AES]Nonce 0 : %x \r\n", w);
    w = word_le(nonce + 4);
    AES_WRITE(NONCE, w);
    AES_LOG("[Info]: [AES]Nonce 1 : %x \r\n", w);
    w = word_le(nonce + 8);
    AES_WRITE(NONCE, w);
    AES_LOG("[Info]: [AES]Nonce 2 : %x \r\n", w);
    w = nonce[12];
    AES_WRITE(NONCE, w);
    AES_LOG("[Info]: [AES]Nonce 3 : %x \r\n", w);
    AES_WRITE(HEADER, header[0]);       /* read even when headerlen is 0 */
    AES_LOG("[Info]: [AES]Header : %x \r\n", (u32)header[0]);
    ccm_status = 0;
    u32 command;
    if (direction == 0) {
        AES_LOG("[Info]: [AES]CCM_ENCRYPT\r\n");
        if (log_tag_const_i_AES) printf_buf(pt, ptlen);
        AES_WRITE(SRCADR, (u32)(uintptr_t)pt);
        if (log_tag_const_i_AES) printf_buf(ct, ptlen);
        AES_WRITE(DSTADR, (u32)(uintptr_t)ct);
        command = 65536;
    } else {
        AES_LOG("[Info]: [AES]CCM_DECRYPT\r\n");
        if (log_tag_const_i_AES) printf_buf(ct, ptlen);
        AES_WRITE(SRCADR, (u32)(uintptr_t)ct);
        if (log_tag_const_i_AES) printf_buf(pt, ptlen);
        AES_WRITE(DSTADR, (u32)(uintptr_t)pt);
        command = 262144;
    }
    AES_WRITE(CON, AES_READ(CON) | command);
    while ((int)AES_READ(CON) >= 0) ;
    AES_WRITE(CON, AES_READ(CON) | 1073741824);
    /* The MIC is read one byte per register read, most significant first. */
    tag[0] = (u8)(AES_READ(TAGRES0) >> 24);
    tag[1] = (u8)(AES_READ(TAGRES0) >> 16);
    tag[2] = (u8)(AES_READ(TAGRES0) >> 8);
    tag[3] = (u8)AES_READ(TAGRES0);
    tag[4] = (u8)(AES_READ(TAGRES1) >> 24);
    tag[5] = (u8)(AES_READ(TAGRES1) >> 16);
    tag[6] = (u8)(AES_READ(TAGRES1) >> 8);
    tag[7] = (u8)AES_READ(TAGRES1);
    tag[8] = (u8)(AES_READ(TAGRES2) >> 24);
    tag[9] = (u8)(AES_READ(TAGRES2) >> 16);
    tag[10] = (u8)(AES_READ(TAGRES2) >> 8);
    tag[11] = (u8)AES_READ(TAGRES2);
    tag[12] = (u8)(AES_READ(TAGRES3) >> 24);
    tag[13] = (u8)(AES_READ(TAGRES3) >> 16);
    tag[14] = (u8)(AES_READ(TAGRES3) >> 8);
    tag[15] = (u8)AES_READ(TAGRES3);
    if (log_tag_const_i_AES) {
        log_print(2, (const char *)0, "[Info]: [AES]MIC\r\n");
        printf_buf(tag, 16);
    }
    local_irq_enable();
}

#ifdef UNITTEST
#include <stdio.h>
#include <string.h>
#include "test.h"

/* Software AES-128, the reference for the register model. */
static u8 sbox[256];
static void make_sbox(void)
{
    u8 p = 1, q = 1;
    do {
        p = p ^ (u8)(p << 1) ^ (p & 0x80 ? 0x1b : 0);
        q ^= q << 1; q ^= q << 2; q ^= q << 4; q ^= q & 0x80 ? 0x09 : 0;
        u8 x = q ^ (u8)(q << 1 | q >> 7) ^ (u8)(q << 2 | q >> 6) ^ (u8)(q << 3 | q >> 5) ^ (u8)(q << 4 | q >> 4);
        sbox[p] = x ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
}
static u8 xt(u8 x) { return (u8)(x << 1) ^ (x & 0x80 ? 0x1b : 0); }
static void aes_ref(const u8 key[16], const u8 in[16], u8 out[16])
{
    u8 rk[176], s[16];
    memcpy(rk, key, 16);
    for (int i = 16, rc = 1; i < 176; i += 4) {
        u8 t[4]; memcpy(t, rk + i - 4, 4);
        if (i % 16 == 0) {
            u8 c = t[0]; t[0] = sbox[t[1]] ^ rc; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[c];
            rc = xt(rc);
        }
        for (int j = 0; j < 4; j++) rk[i + j] = rk[i - 16 + j] ^ t[j];
    }
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (int r = 1; r <= 10; r++) {
        u8 t[16];
        for (int i = 0; i < 16; i++) t[i] = sbox[s[i]];
        for (int c = 0; c < 4; c++) for (int rw = 0; rw < 4; rw++) s[c * 4 + rw] = t[((c + rw) % 4) * 4 + rw];
        if (r != 10)
            for (int c = 0; c < 4; c++) {
                u8 *a = s + c * 4, a0 = a[0], all = a[0] ^ a[1] ^ a[2] ^ a[3];
                a[0] ^= all ^ xt(a[0] ^ a[1]); a[1] ^= all ^ xt(a[1] ^ a[2]);
                a[2] ^= all ^ xt(a[2] ^ a[3]); a[3] ^= all ^ xt(a[3] ^ a0);
            }
        for (int i = 0; i < 16; i++) s[i] ^= rk[r * 16 + i];
    }
    memcpy(out, s, 16);
}

/* The register file model.  Key and data words arrive as the SDK writes
 * them (aes128_start_enc: word n carries bytes 12-4n..15-4n, big-endian);
 * a CON write of 1 runs the reference AES; ENCRESn reads little-endian
 * words of the result.  CON reads back as written, with bit 31 (CCM done)
 * once a CCM command bit is set. */
static struct { int write; unsigned off; u32 value; } trace[64];
static int traced, key_writes, dat_writes;
static u8 model_key[16], model_dat[16], model_ct[16];
static u32 con, tagres[4];
static int reads[17];
u32 aes_reg_read(unsigned offset)
{
    reads[offset / 4]++;
    if (offset == 0) return con | (con & 0x50000 ? 0x80000000u : 0);
    if (offset >= 12 && offset < 28) {
        unsigned n = (offset - 12) / 4;
        return model_ct[n * 4] | model_ct[n * 4 + 1] << 8 | model_ct[n * 4 + 2] << 16 | (u32)model_ct[n * 4 + 3] << 24;
    }
    if (offset >= 52) return tagres[(offset - 52) / 4];
    return 0;
}
void aes_reg_write(unsigned offset, u32 value)
{
    CHECK(traced < 64);
    trace[traced].write = 1; trace[traced].off = offset; trace[traced++].value = value;
    if (offset == 0) {
        con = value;
        if (value == 1) aes_ref(model_key, model_dat, model_ct);
    } else if (offset == 8 || offset == 4) {
        u8 *dst = offset == 8 ? model_key : model_dat;
        int *n = offset == 8 ? &key_writes : &dat_writes;
        int base = 12 - 4 * (*n)++;
        for (int i = 0; i < 4; i++) dst[base + i] = (u8)(value >> (24 - 8 * i));
    }
}
static int irq_depth, irq_enabled, asserts, printed, logged;
static void (*installed_irq)(void);
static int installed_irq_index, installed_irq_priority, installed_irq_cpu;
void request_irq(u8 index, u8 priority, void (*handler)(void), u8 cpu_id)
{
    installed_irq_index = index; installed_irq_priority = priority;
    installed_irq = handler; installed_irq_cpu = cpu_id;
}
void local_irq_disable(void) { CHECK(!irq_depth); irq_depth = 1; }
void local_irq_enable(void) { CHECK(irq_depth); irq_depth = 0; irq_enabled++; }
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; asserts++; }
void printf_buf(const u8 *buf, int size) { (void)buf; (void)size; printed++; }
void log_print(int level, const char *file, const char *format, ...) { (void)level; (void)file; (void)format; logged++; }
u8 log_tag_const_i_AES;

static void restart(void)
{
    traced = key_writes = dat_writes = 0; memset(reads, 0, sizeof(reads)); con = 0;
}
static int wrote(int index, unsigned off, u32 value)
{
    return index < traced && trace[index].write && trace[index].off == off && trace[index].value == value;
}

int main(void)
{
    make_sbox();
    /* Reference check, FIPS-197 appendix B. */
    static const u8 fkey[16] = { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c };
    static const u8 fpt[16] = { 0x32, 0x43, 0xf6, 0xa8, 0x88, 0x5a, 0x30, 0x8d, 0x31, 0x31, 0x98, 0xa2, 0xe0, 0x37, 0x07, 0x34 };
    static const u8 fct[16] = { 0x39, 0x25, 0x84, 0x1d, 0x02, 0xdc, 0x09, 0xfb, 0xdc, 0x11, 0x85, 0x97, 0x19, 0x6a, 0x0b, 0x32 };
    u8 out[16] = { 0 };
    aes_ref(fkey, fpt, out);
    CHECK(!memcmp(out, fct, 16));

    /* aes128_start_enc: words most significant byte first, from byte 12 down. */
    restart();
    memset(out, 0xee, sizeof(out));
    u8 key[16], pt[16];
    memcpy(key, fkey, 16); memcpy(pt, fpt, 16);
    CHECK(aes128_start_enc(key, pt, out) == 0);
    CHECK(!memcmp(out, fct, 16));
    CHECK(traced == 10);
    CHECK(wrote(0, 8, 0x09cf4f3c) && wrote(1, 8, 0xabf71588) && wrote(2, 8, 0x28aed2a6) && wrote(3, 8, 0x2b7e1516));
    CHECK(wrote(4, 0, 16));
    CHECK(wrote(5, 4, 0xe0370734) && wrote(6, 4, 0x313198a2) && wrote(7, 4, 0x885a308d) && wrote(8, 4, 0x3243f6a8));
    CHECK(wrote(9, 0, 1));
    /* One register read per result byte; CON polled once per wait. */
    CHECK(reads[3] == 4 && reads[4] == 4 && reads[5] == 4 && reads[6] == 4 && reads[0] == 2);
    CHECK(!printed && !logged);

    /* The public aes128_test arrays are the SDK's IR data (not derived from
     * the implementation); validate the AES block vector independently. */
    static const u8 sdk_key[16] = {0x4c,0x68,0x38,0x41,0x39,0xf5,0x74,0xd8,0x36,0xbc,0xf3,0x4e,0x9d,0xfb,0x01,0xbf};
    static const u8 sdk_pt[16] = {0x02,0x13,0x24,0x35,0x46,0x57,0x68,0x79,0xac,0xbd,0xce,0xdf,0xe0,0xf1,0x02,0x13};
    static const u8 sdk_ct[16] = {0x99,0xad,0x1b,0x52,0x26,0xa3,0x7e,0x3e,0x05,0x8e,0x3b,0x8e,0x27,0xc2,0xc6,0x66};
    u8 sdk_out[16];
    restart();
    aes128_start_enc((u8 *)sdk_key, (u8 *)sdk_pt, sdk_out);
    CHECK(!memcmp(sdk_out, sdk_ct, 16));

    /* SDK init registers IRQ 40 at priority 0; the handler acknowledges the
     * completion bit only when the peripheral status is negative. */
    aes_init();
    CHECK(installed_irq && installed_irq_index == 40 && installed_irq_priority == 0 && installed_irq_cpu == 0);
    con = 0;
    installed_irq();
    CHECK(con == 0);
    con = 0x80000000u;
    installed_irq();
    CHECK(con == 0xc0000000u);

    /* The direct CCM self-test uses the SDK's expected MIC vector and two
     * DMA directions.  Values derive from the BD19 IR constants. */
    restart();
    tagres[0] = 0x5981008au;
    aes_hw_test();
    CHECK(wrote(0, 44, 27) && wrote(1, 8, 0x87e2fa74u));
    CHECK(wrote(5, 0, 0x810) && wrote(6, 48, 4));
    CHECK(wrote(traced - 1, 0, 0xc0050810u));

    /* ccm_memory_v2, encrypt, one header byte. */
    u8 k[16], n[13], h[2] = { 0x7f, 0x55 }, p[8] = { 0 }, c[8] = { 0 }, t[16];
    for (int i = 0; i < 16; i++) k[i] = i + 1;
    for (int i = 0; i < 13; i++) n[i] = 0x21 + i;
    for (int i = 0; i < 4; i++) tagres[i] = 0x01020304 * (i + 1) + 0x0a0b0c0d;
    restart();
    ccm_memory_v2(k, n, h, 1, p, 5, c, t, 4, 0);
    CHECK(irq_enabled == 1 && !irq_depth && !asserts);
    int i = 0;
    CHECK(wrote(i++, 44, 5));
    CHECK(wrote(i++, 8, 0x04030201) && wrote(i++, 8, 0x08070605) && wrote(i++, 8, 0x0c0b0a09) && wrote(i++, 8, 0x100f0e0d));
    CHECK(wrote(i++, 0, 16) && wrote(i++, 0, 16 | 0x800));
    CHECK(wrote(i++, 48, 4));
    CHECK(wrote(i++, 28, 0x24232221) && wrote(i++, 28, 0x28272625) && wrote(i++, 28, 0x2c2b2a29) && wrote(i++, 28, 0x2d));
    CHECK(wrote(i++, 32, 0x7f));
    CHECK(wrote(i++, 36, (u32)(uintptr_t)p) && wrote(i++, 40, (u32)(uintptr_t)c));
    CHECK(wrote(i++, 0, 0x810 | 0x10000) && wrote(i++, 0, 0xc0010810));
    CHECK(i == traced);
    /* MIC bytes: each register word read four times, most significant byte first. */
    for (int w = 0; w < 4; w++) {
        CHECK(t[w * 4] == (u8)(tagres[w] >> 24) && t[w * 4 + 1] == (u8)(tagres[w] >> 16));
        CHECK(t[w * 4 + 2] == (u8)(tagres[w] >> 8) && t[w * 4 + 3] == (u8)tagres[w]);
        CHECK(reads[13 + w] == 4);
    }

    /* Decrypt swaps source and destination and sets the other command bit. */
    restart();
    ccm_memory_v2(k, n, h, 1, p, 8, c, t, 16, 1);
    i = 0;
    CHECK(wrote(i++, 44, 8));
    i += 4;
    CHECK(wrote(i++, 0, 16) && wrote(i++, 0, 16 | 0x800));
    CHECK(wrote(i++, 48, 16));
    i += 5;
    CHECK(wrote(i++, 36, (u32)(uintptr_t)c) && wrote(i++, 40, (u32)(uintptr_t)p));
    CHECK(wrote(i++, 0, 0x810 | 0x40000) && wrote(i++, 0, 0xc0040810));
    CHECK(i == traced && !asserts);

    /* No header: the header flag is cleared (and the header byte is still read). */
    restart(); con = 0xffffffff;
    ccm_memory_v2(k, n, h, 0, p, 1, c, t, 4, 0);
    CHECK(wrote(5, 0, 16) && wrote(6, 0, 16) && wrote(12, 32, 0x7f) && !asserts);

    /* A header length other than 0 or 1 asserts once and the flag is still set. */
    restart();
    ccm_memory_v2(k, n, h, 2, p, 1, c, t, 4, 0);
    CHECK(asserts == 1 && wrote(6, 0, 16 | 0x800));
    CHECK(ccm_status == 0);

    /* Logging on: key, nonce, payload buffers are printed. */
    {
        log_tag_const_i_AES = 1;
        printed = logged = 0; restart();
        ccm_memory_v2(k, n, h, 1, p, 1, c, t, 4, 0);
        CHECK(printed == 5 && logged == 15);   /* key CNT Key0-3 nonce TAGLEN Nonce0-3 Header ENCRYPT MIC */
        printed = 0; restart(); aes128_start_enc(key, pt, out);
        CHECK(printed == 3);
    }
    return TEST_DONE("btctrler_aes_hw");
}
#endif
