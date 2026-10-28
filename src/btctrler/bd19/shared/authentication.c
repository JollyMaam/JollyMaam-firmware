/* The SDK's authentication.c: the Bluetooth Classic legacy security
 * functions (Core Vol 2 Part H): E1 authentication, E21 and E22 key
 * generation, E3 encryption key and the key length reduction, all built on
 * the SAFER+ cipher Ar and its variant A'r.  btencry_task.c calls the four
 * public functions; the rest are the SDK's file-static helpers.  Key,
 * random and address buffers are in the order the SDK's code reads them,
 * byte 0 first. */
#include "btctrler/glue.h"

#include <string.h>
#ifdef UNITTEST
#include <stdlib.h>
#else
void *malloc(int size);
void free(void *ptr);
#endif

/* Same sections as the SDK's code and constants. */
#define AUTH_CODE  __attribute__((section(".classic_lmp_auth_code")))
#define AUTH_CONST __attribute__((section(".classic_lmp_auth_const")))

/* The big-number library the key length reduction uses (crypto_toolbox, outside
 * btctrler.a): polynomials over GF(2), read and written most significant byte
 * first. */
void bi_initialize(BI_CTX *ctx, char *mem_pool);
bigint *bi_read_from_byte(BI_CTX *ctx, u8 *data, int size);
bigint *bi_poly_mod2(BI_CTX *ctx, bigint *a, bigint *b);
bigint *bi_poly_mul(BI_CTX *ctx, bigint *a, bigint *b);
bigint *trim(bigint *bi);
void bi_wirte_to_byte(bigint *bi, u8 *data);
void bi_free(BI_CTX *ctx, bigint *bi);
void bi_terminate(BI_CTX *ctx);

/* Subkey bias, in the SDK's layout: B[p][k] = e(e(17(p+1)+(15-k)+1)) with
 * the exponentiations mod 257 (Core Vol 2 Part H 4.1: B_(p+1)[15-k]); the
 * round key builder reads it backwards, byte j adding B[p][15-j].  The unit
 * test recomputes the table. */
static const u8 B[17][16] AUTH_CONST = {
    { 0x77, 0xf1, 0x56, 0x24, 0x7e, 0x47, 0x1b, 0x86, 0xbd, 0x70, 0x8e, 0x1e, 0x3b, 0x73, 0x16, 0x03 },
    { 0x64, 0xac, 0x28, 0x5a, 0xc9, 0xb3, 0x37, 0xc5, 0x0a, 0x10, 0xb7, 0xa3, 0xba, 0xb1, 0x97, 0x46 },
    { 0x3d, 0x05, 0xdc, 0x66, 0x6e, 0xf6, 0x9a, 0xf8, 0x0d, 0x58, 0x95, 0x67, 0xc6, 0xaa, 0xab, 0xec },
    { 0xa0, 0x68, 0x9b, 0x96, 0xd4, 0xeb, 0xbf, 0x43, 0x49, 0x36, 0xe9, 0x6a, 0x89, 0xd8, 0xc3, 0x8a },
    { 0x94, 0x63, 0x99, 0xbc, 0x7b, 0xbe, 0xc1, 0x22, 0xbb, 0x5c, 0x71, 0xd5, 0x1f, 0x92, 0x57, 0x5d },
    { 0x8f, 0x44, 0x41, 0x1d, 0x51, 0xe6, 0x40, 0x17, 0xfb, 0xfd, 0x19, 0x32, 0x34, 0xb8, 0x61, 0x2a },
    { 0xca, 0x23, 0x6f, 0xda, 0x39, 0xf7, 0xa2, 0x01, 0x7f, 0xd6, 0x31, 0xe7, 0xde, 0x80, 0x04, 0xdd },
    { 0x2c, 0x59, 0x82, 0xaf, 0xa8, 0xe0, 0x0f, 0xcd, 0xa1, 0x12, 0x3e, 0x30, 0xd1, 0x1c, 0xd0, 0x3a },
    { 0x33, 0x72, 0x2e, 0x4f, 0x90, 0x02, 0x13, 0x06, 0x75, 0xce, 0x87, 0xc2, 0xef, 0xb2, 0xad, 0x7d },
    { 0x38, 0x15, 0xe1, 0x52, 0x9f, 0x7a, 0x6c, 0x2f, 0x27, 0xc4, 0xe2, 0x81, 0xa9, 0xcf, 0x8d, 0xc0 },
    { 0xd7, 0xdf, 0xff, 0x60, 0x76, 0x14, 0x8c, 0x5e, 0x55, 0x09, 0xe4, 0x08, 0xc7, 0x42, 0x20, 0xfc },
    { 0xd2, 0x50, 0x91, 0xd9, 0x4c, 0x62, 0x9e, 0xe8, 0xb9, 0xa6, 0xf9, 0x1a, 0x00, 0x21, 0x0b, 0xfa },
    { 0x35, 0x9c, 0x4e, 0x4b, 0x69, 0x48, 0xcb, 0x0e, 0xc8, 0xa4, 0x5b, 0xea, 0x84, 0x07, 0xb4, 0x18 },
    { 0xf4, 0xae, 0x6b, 0xdb, 0xa7, 0xcc, 0x3f, 0x8b, 0x4a, 0x0c, 0x3c, 0x25, 0xe5, 0x54, 0x4d, 0x45 },
    { 0x83, 0xed, 0x11, 0xf0, 0xb0, 0x53, 0x93, 0xf2, 0x74, 0x26, 0xb5, 0x9d, 0x6d, 0x7c, 0xf3, 0x2d },
    { 0xf1, 0x56, 0x24, 0x7e, 0x47, 0x1b, 0x86, 0xbd, 0x70, 0x8e, 0x1e, 0x3b, 0x73, 0x16, 0x03, 0xb6 },
    { 0xac, 0x28, 0x5a, 0xc9, 0xb3, 0x37, 0xc5, 0x0a, 0x10, 0xb7, 0xa3, 0xba, 0xb1, 0x97, 0x46, 0x88 },
};

/* e(x) = (45^x mod 257) mod 256 and its inverse l (the SAFER+ S-boxes). */
static const u8 etable[256] AUTH_CONST = {
    0x01, 0x2d, 0xe2, 0x93, 0xbe, 0x45, 0x15, 0xae, 0x78, 0x03, 0x87, 0xa4, 0xb8, 0x38, 0xcf, 0x3f,
    0x08, 0x67, 0x09, 0x94, 0xeb, 0x26, 0xa8, 0x6b, 0xbd, 0x18, 0x34, 0x1b, 0xbb, 0xbf, 0x72, 0xf7,
    0x40, 0x35, 0x48, 0x9c, 0x51, 0x2f, 0x3b, 0x55, 0xe3, 0xc0, 0x9f, 0xd8, 0xd3, 0xf3, 0x8d, 0xb1,
    0xff, 0xa7, 0x3e, 0xdc, 0x86, 0x77, 0xd7, 0xa6, 0x11, 0xfb, 0xf4, 0xba, 0x92, 0x91, 0x64, 0x83,
    0xf1, 0x33, 0xef, 0xda, 0x2c, 0xb5, 0xb2, 0x2b, 0x88, 0xd1, 0x99, 0xcb, 0x8c, 0x84, 0x1d, 0x14,
    0x81, 0x97, 0x71, 0xca, 0x5f, 0xa3, 0x8b, 0x57, 0x3c, 0x82, 0xc4, 0x52, 0x5c, 0x1c, 0xe8, 0xa0,
    0x04, 0xb4, 0x85, 0x4a, 0xf6, 0x13, 0x54, 0xb6, 0xdf, 0x0c, 0x1a, 0x8e, 0xde, 0xe0, 0x39, 0xfc,
    0x20, 0x9b, 0x24, 0x4e, 0xa9, 0x98, 0x9e, 0xab, 0xf2, 0x60, 0xd0, 0x6c, 0xea, 0xfa, 0xc7, 0xd9,
    0x00, 0xd4, 0x1f, 0x6e, 0x43, 0xbc, 0xec, 0x53, 0x89, 0xfe, 0x7a, 0x5d, 0x49, 0xc9, 0x32, 0xc2,
    0xf9, 0x9a, 0xf8, 0x6d, 0x16, 0xdb, 0x59, 0x96, 0x44, 0xe9, 0xcd, 0xe6, 0x46, 0x42, 0x8f, 0x0a,
    0xc1, 0xcc, 0xb9, 0x65, 0xb0, 0xd2, 0xc6, 0xac, 0x1e, 0x41, 0x62, 0x29, 0x2e, 0x0e, 0x74, 0x50,
    0x02, 0x5a, 0xc3, 0x25, 0x7b, 0x8a, 0x2a, 0x5b, 0xf0, 0x06, 0x0d, 0x47, 0x6f, 0x70, 0x9d, 0x7e,
    0x10, 0xce, 0x12, 0x27, 0xd5, 0x4c, 0x4f, 0xd6, 0x79, 0x30, 0x68, 0x36, 0x75, 0x7d, 0xe4, 0xed,
    0x80, 0x6a, 0x90, 0x37, 0xa2, 0x5e, 0x76, 0xaa, 0xc5, 0x7f, 0x3d, 0xaf, 0xa5, 0xe5, 0x19, 0x61,
    0xfd, 0x4d, 0x7c, 0xb7, 0x0b, 0xee, 0xad, 0x4b, 0x22, 0xf5, 0xe7, 0x73, 0x23, 0x21, 0xc8, 0x05,
    0xe1, 0x66, 0xdd, 0xb3, 0x58, 0x69, 0x63, 0x56, 0x0f, 0xa1, 0x31, 0x95, 0x17, 0x07, 0x3a, 0x28,
};
static const u8 ltable[256] AUTH_CONST = {
    0x80, 0x00, 0xb0, 0x09, 0x60, 0xef, 0xb9, 0xfd, 0x10, 0x12, 0x9f, 0xe4, 0x69, 0xba, 0xad, 0xf8,
    0xc0, 0x38, 0xc2, 0x65, 0x4f, 0x06, 0x94, 0xfc, 0x19, 0xde, 0x6a, 0x1b, 0x5d, 0x4e, 0xa8, 0x82,
    0x70, 0xed, 0xe8, 0xec, 0x72, 0xb3, 0x15, 0xc3, 0xff, 0xab, 0xb6, 0x47, 0x44, 0x01, 0xac, 0x25,
    0xc9, 0xfa, 0x8e, 0x41, 0x1a, 0x21, 0xcb, 0xd3, 0x0d, 0x6e, 0xfe, 0x26, 0x58, 0xda, 0x32, 0x0f,
    0x20, 0xa9, 0x9d, 0x84, 0x98, 0x05, 0x9c, 0xbb, 0x22, 0x8c, 0x63, 0xe7, 0xc5, 0xe1, 0x73, 0xc6,
    0xaf, 0x24, 0x5b, 0x87, 0x66, 0x27, 0xf7, 0x57, 0xf4, 0x96, 0xb1, 0xb7, 0x5c, 0x8b, 0xd5, 0x54,
    0x79, 0xdf, 0xaa, 0xf6, 0x3e, 0xa3, 0xf1, 0x11, 0xca, 0xf5, 0xd1, 0x17, 0x7b, 0x93, 0x83, 0xbc,
    0xbd, 0x52, 0x1e, 0xeb, 0xae, 0xcc, 0xd6, 0x35, 0x08, 0xc8, 0x8a, 0xb4, 0xe2, 0xcd, 0xbf, 0xd9,
    0xd0, 0x50, 0x59, 0x3f, 0x4d, 0x62, 0x34, 0x0a, 0x48, 0x88, 0xb5, 0x56, 0x4c, 0x2e, 0x6b, 0x9e,
    0xd2, 0x3d, 0x3c, 0x03, 0x13, 0xfb, 0x97, 0x51, 0x75, 0x4a, 0x91, 0x71, 0x23, 0xbe, 0x76, 0x2a,
    0x5f, 0xf9, 0xd4, 0x55, 0x0b, 0xdc, 0x37, 0x31, 0x16, 0x74, 0xd7, 0x77, 0xa7, 0xe6, 0x07, 0xdb,
    0xa4, 0x2f, 0x46, 0xf3, 0x61, 0x45, 0x67, 0xe3, 0x0c, 0xa2, 0x3b, 0x1c, 0x85, 0x18, 0x04, 0x1d,
    0x29, 0xa0, 0x8f, 0xb2, 0x5a, 0xd8, 0xa6, 0x7e, 0xee, 0x8d, 0x53, 0x4b, 0xa1, 0x9a, 0xc1, 0x0e,
    0x7a, 0x49, 0xa5, 0x2c, 0x81, 0xc4, 0xc7, 0x36, 0x2b, 0x7f, 0x43, 0x95, 0x33, 0xf2, 0x6c, 0x68,
    0x6d, 0xf0, 0x02, 0x28, 0xce, 0xdd, 0x9b, 0xea, 0x5e, 0x99, 0x7c, 0x14, 0x86, 0xcf, 0xe5, 0x42,
    0xb8, 0x40, 0x78, 0x2d, 0x3a, 0xe9, 0x64, 0x1f, 0x92, 0x90, 0x7d, 0x39, 0x6f, 0xe0, 0x89, 0x30,
};

/* Key length reduction (Core Vol 2 Part H 4.5.3.1): row L-1 holds g1 and g2
 * for an effective key length of L bytes, as 16 bytes most significant first. */
static const u8 g1_tab[15][16] AUTH_CONST = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x1d },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x3f },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xdb },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xaf },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xaf },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x91 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x95 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1b },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x09 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x15 },
    { 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x3b },
    { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xdd },
    { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xdd },
    { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xdd },
    { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe7 },
};
static const u8 g2_tab[15][16] AUTH_CONST = {
    { 0x00, 0xe2, 0x75, 0xa0, 0xab, 0xd2, 0x18, 0xd4, 0xcf, 0x92, 0x8b, 0x9b, 0xbf, 0x6c, 0xb0, 0x8f },
    { 0x00, 0x01, 0xe3, 0xf6, 0x3d, 0x76, 0x59, 0xb3, 0x7f, 0x18, 0xc2, 0x58, 0xcf, 0xf6, 0xef, 0xef },
    { 0x00, 0x00, 0x01, 0xbe, 0xf6, 0x6c, 0x6c, 0x3a, 0xb1, 0x03, 0x0a, 0x5a, 0x19, 0x19, 0x80, 0x8b },
    { 0x00, 0x00, 0x00, 0x01, 0x6a, 0xb8, 0x99, 0x69, 0xde, 0x17, 0x46, 0x7f, 0xd3, 0x73, 0x6a, 0xd9 },
    { 0x00, 0x00, 0x00, 0x01, 0x6a, 0xb8, 0x99, 0x69, 0xde, 0x17, 0x46, 0x7f, 0xd3, 0x73, 0x6a, 0xd9 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x93, 0x52, 0xaa, 0x6c, 0xc0, 0x54, 0x46, 0x83, 0x11 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb3, 0xf7, 0xff, 0xfc, 0xe2, 0x79, 0xf3, 0xa0, 0x73 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa1, 0xab, 0x81, 0x5b, 0xc7, 0xec, 0x80, 0x25 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xc9, 0x80, 0x11, 0xd8, 0xb0, 0x4d },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x8e, 0x24, 0xf9, 0xa4, 0xbb },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0xa7, 0x60, 0x24, 0xd7 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x9c, 0x26, 0xb9 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x9c, 0x26, 0xb9 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x9c, 0x26, 0xb9 },
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x89 },
};

/* The SDK's ASSERT() on an allocation: report, and carry on (the SDK then
 * dereferences the null pointer; with config_asser clear the chip resets). */
#define CHECK_ALLOC(p, what) do { if (!(p)) btctrler_assert_failed(what, 0); } while (0)

/* The octets SAFER+ combines with xor in its first key layer and adds in the
 * second (and the reverse for the other eight): 0, 3, 4, 7, 8, 11, 12, 15. */
static int xor_position(int m)
{
    return m == 0 || m == 3 || m == 4 || m == 7 || m == 8 || m == 11 || m == 12 || m == 15;
}

/* Pseudo-Hadamard transform of eight byte pairs: (a, b) -> (2a + b, a + b). */
AUTH_CODE static void pht(u8 *data)
{
    for (int i = 0; i < 8; i++, data += 2) {
        u8 a = data[0], b = data[1];
        data[1] = b + a;
        data[0] = (a << 1) + b;
    }
}

/* The "Armenian shuffle", SAFER+'s byte permutation. */
AUTH_CODE static void premute(u8 *data)
{
    u8 t[16];
    for (int i = 0; i < 16; i++) t[i] = data[i];
    data[0] = t[8];  data[1] = t[11]; data[2] = t[12]; data[3] = t[15];
    data[4] = t[2];  data[5] = t[1];  data[6] = t[6];  data[7] = t[5];
    data[8] = t[10]; data[9] = t[9];  data[10] = t[14]; data[11] = t[13];
    data[12] = t[0]; data[13] = t[7]; data[14] = t[4]; data[15] = t[3];
}

/* The 17 round keys of SAFER+ for a 16-byte key: key 0 is the key itself;
 * key p is the 17-byte key (the 16 bytes and their xor) rotated left 3 bits
 * in every byte p times, 16 bytes taken cyclically from position p, plus the
 * bias. */
AUTH_CODE static void roundkeygenerate(const u8 *inputkey, u8 roundkey[17][16])
{
    u8 key[17], tmp_key[17];
    memset(roundkey, 0, 272);
    key[16] = 0;
    for (int i = 0; i < 16; i++) {
        key[i] = inputkey[i];
        key[16] ^= inputkey[i];
    }
    for (int i = 0; i < 16; i++) {
        roundkey[0][i] = key[i];
        tmp_key[i] = key[i];
    }
    tmp_key[16] = key[16];
    for (int p = 1; p <= 16; p++) {
        for (int m = 0; m < 17; m++)
            tmp_key[m] = (u8)(tmp_key[m] << 3 | tmp_key[m] >> 5);
        const u8 *q = tmp_key + p;
        for (int j = 0; j < 16; j++) {
            roundkey[p][j] = *q++ + B[p][15 - j];
            if (q > tmp_key + 16) q = tmp_key;
        }
    }
}

/* SAFER+ with a 128-bit key, encrypting rand in place.  mode 0 is Ar; mode 1
 * is A'r, which adds the input again (xor or add by position) before round 3. */
AUTH_CODE static void function_Ar01(u8 *rand, const u8 roundkey[17][16], u8 mode)
{
    u8 input[16];
    for (int i = 0; i < 16; i++) input[i] = rand[i];
    for (int r = 0; r < 8; r++) {
        if (mode && r == 2)
            for (int m = 0; m < 16; m++)
                rand[m] = xor_position(m) ? input[m] ^ rand[m] : input[m] + rand[m];
        for (int m = 0; m < 16; m++)
            rand[m] = xor_position(m) ? rand[m] ^ roundkey[2 * r][m] : rand[m] + roundkey[2 * r][m];
        for (int m = 0; m < 16; m++)
            rand[m] = xor_position(m) ? etable[rand[m]] : ltable[rand[m]];
        for (int m = 0; m < 16; m++)
            rand[m] = xor_position(m) ? rand[m] + roundkey[2 * r + 1][m] : rand[m] ^ roundkey[2 * r + 1][m];
        pht(rand); premute(rand); pht(rand); premute(rand); pht(rand); premute(rand); pht(rand);
    }
    for (int m = 0; m < 16; m++)
        rand[m] = xor_position(m) ? rand[m] ^ roundkey[16][m] : rand[m] + roundkey[16][m];
}

/* The hash shared by E1 and E3: result = A'r(K~, (Ar(K, RAND) xor RAND) +
 * the L bytes at address_cof repeated to 16), where K~ is K with the offsets
 * 233, 229, 223, 193, 179, 167, 149, 131 added or xored (alternating; the
 * second eight with the operations swapped). */
AUTH_CODE static void function_E13(const u8 *address_cof, const u8 *RAND, const u8 *K, u8 L, u8 *result)
{
    u8 address128[16], K1[16];
    u8 (*roundkey)[16] = malloc(272);
    CHECK_ALLOC(roundkey, "roundkey");
    const u8 *p = address_cof;
    for (int i = 0; i < 16; i++) {
        address128[i] = *p++;
        if (p > address_cof + L - 1) p = address_cof;
    }
    for (int i = 0; i < 16; i++) result[i] = RAND[i];
    roundkeygenerate(K, roundkey);
    function_Ar01(result, roundkey, 0);
    for (int i = 0; i < 16; i++) result[i] = address128[i] + (result[i] ^ RAND[i]);
    K1[0] = K[0] + 233;   K1[1] = K[1] ^ 229;   K1[2] = K[2] + 223;   K1[3] = K[3] ^ 193;
    K1[4] = K[4] + 179;   K1[5] = K[5] ^ 167;   K1[6] = K[6] + 149;   K1[7] = K[7] ^ 131;
    K1[8] = K[8] ^ 233;   K1[9] = K[9] + 229;   K1[10] = K[10] ^ 223; K1[11] = K[11] + 193;
    K1[12] = K[12] ^ 179; K1[13] = K[13] + 167; K1[14] = K[14] ^ 149; K1[15] = K[15] + 131;
    roundkeygenerate(K1, roundkey);
    function_Ar01(result, roundkey, 1);
    free(roundkey);
}

/* E1 (authentication): result[0..3] is SRES, result[4..15] the ACO. */
AUTH_CODE void function_E1(u8 *address, u8 *RAND, u8 *K, u8 *result)
{
    function_E13(address, RAND, K, 6, result);
}

/* E21 (unit and combination key part): A'r with the key RAND (byte 15 xor 6)
 * on the 6-byte address repeated to 16. */
AUTH_CODE void function_E21(u8 *address, u8 *RAND, u8 *reslut)
{
    u8 rand[16];
    u8 (*roundkey)[16] = (config_btctler_modules & 1) ? lmp_malloc(272) : malloc(272);
    CHECK_ALLOC(roundkey, "roundkey");
    const u8 *p = address;
    for (int i = 0; i < 16; i++) {
        reslut[i] = *p++;
        rand[i] = RAND[i];
        if (p > address + 5) p = address;
    }
    rand[15] ^= 6;
    roundkeygenerate(rand, roundkey);
    function_Ar01(reslut, roundkey, 1);
    if (config_btctler_modules & 1) lmp_free(roundkey);
    else free(roundkey);
}

/* E22 (initialisation key): A'r with the key the PIN, augmented with the
 * address up to 16 bytes and repeated to 16, on RAND with byte 15 xor
 * min(L + 6, 16).  The SDK always allocates the round keys with lmp_malloc
 * but frees them with free() when the Classic module bit is clear. */
AUTH_CODE void function_E22(u8 *pin, u8 *RAND, u8 L, u8 *address, u8 *reslut)
{
    u8 data[16];
    u8 (*roundkey)[16] = lmp_malloc(272);
    CHECK_ALLOC(roundkey, "roundkey");
    int n = 0;
    for (; n < L; n++) data[n] = pin[n];
    for (int a = 0; a < 6 && n < 16; a++, n++) data[n] = address[a];
    int total = n;
    const u8 *p = data;
    for (; n < 16; n++) {
        data[n] = *p++;
        if (p > data + total - 1) p = data;
    }
    int augmented = L + 6;
    for (int i = 0; i < 16; i++) reslut[i] = RAND[i];
    reslut[15] ^= (u8)(augmented < 16 ? augmented : 16);
    roundkeygenerate(data, roundkey);
    function_Ar01(reslut, roundkey, 1);
    if (config_btctler_modules & 1) lmp_free(roundkey);
    else free(roundkey);
}

/* Effective key length reduction: Kc' = g2 * (Kc mod g1), polynomials over
 * GF(2) with the key as a number, least significant byte first in the
 * buffer; Len (1 to 15) is the key length in bytes. */
AUTH_CODE static void Entrypt_Key_Length_Change(u8 *Key, u8 Len)
{
    BI_CTX temp_ctx;
    char *pool = (config_btctler_modules & 1) ? lmp_malloc(800) : malloc(800);
    CHECK_ALLOC(pool, "e3_mem_pool");
    bi_initialize(&temp_ctx, pool);
    endian_change(Key, 16);
    bigint *g1 = bi_read_from_byte(&temp_ctx, (u8 *)g1_tab[Len - 1], 16);
    bigint *g2 = bi_read_from_byte(&temp_ctx, (u8 *)g2_tab[Len - 1], 16);
    bigint *k = bi_read_from_byte(&temp_ctx, Key, 16);
    bigint *r = bi_poly_mod2(&temp_ctx, k, g1);
    bigint *product = bi_poly_mul(&temp_ctx, g2, r);
    trim(product);
    bi_wirte_to_byte(product, Key);
    endian_change(Key, 16);
    bi_free(&temp_ctx, product);
    bi_terminate(&temp_ctx);
    if (config_btctler_modules & 1) lmp_free(pool);
    else free(pool);
}

/* E3 (encryption key): the E1 hash of K, RAND and the 12-byte COF, reduced
 * to Len bytes of effective key length unless that is the full 16. */
AUTH_CODE void function_E3(u8 *K, u8 *RAND, u8 *COF, u8 Len, u8 *result)
{
    function_E13(COF, RAND, K, 12, result);
    if (Len != 16) Entrypt_Key_Length_Change(result, Len);
}

#ifdef CONFIG_CPU_BD19
/* The BD19 authentication member also contains the curve-specialization
 * portion of micro-ecc.  Its arithmetic backend lives in crypto_toolbox ROM;
 * these objects expose the five curve descriptors that backend consumes. */
#include "btctrler/bd19/uECC_curves.inc"
#endif

#ifdef UNITTEST
#include "test.h"

/* Host stand-ins for the SDK's allocator and helpers. */
int config_btctler_modules = 3;
int config_asser;
static int asserted, lmp_allocs;
void btctrler_assert_failed(const char *what, int value) { (void)what; (void)value; asserted++; }
void *lmp_malloc(int size) { lmp_allocs++; return malloc(size); }
void lmp_free(void *ptr) { lmp_allocs--; free(ptr); }
void endian_change(u8 *src, u16 len)
{
    for (unsigned i = 0, j = len; i < len / 2u; i++) { u8 t = src[--j]; src[j] = src[i]; src[i] = t; }
}

/* The crypto_toolbox's polynomial arithmetic as the SDK uses it, modelled
 * here: 256-bit polynomials over GF(2); bi_read_from_byte takes the bytes
 * most significant first, bi_wirte_to_byte writes the low 16 bytes. */
/* A number is 32 little-endian bytes behind comps. */
#define BIT(a) ((u8 *)(a)->comps)
static bigint *new_bigint(void)
{
    bigint *b = calloc(1, sizeof(*b));
    b->comps = calloc(1, 32);
    return b;
}
void bi_initialize(BI_CTX *ctx, char *mem_pool) { (void)ctx; (void)mem_pool; }
bigint *bi_read_from_byte(BI_CTX *ctx, u8 *data, int size)
{
    (void)ctx;
    bigint *b = new_bigint();
    for (int i = 0; i < size; i++) BIT(b)[size - 1 - i] = data[i];
    return b;
}
static int degree(const bigint *a)
{
    for (int i = 255; i >= 0; i--) if (BIT(a)[i / 8] >> (i % 8) & 1) return i;
    return -1;
}
bigint *bi_poly_mod2(BI_CTX *ctx, bigint *a, bigint *b)
{
    (void)ctx;
    bigint *r = new_bigint();
    memcpy(BIT(r), BIT(a), 32);
    int db = degree(b);
    for (int d; (d = degree(r)) >= db;)
        for (int i = 0; i <= db; i++)
            if (BIT(b)[i / 8] >> (i % 8) & 1) BIT(r)[(i + d - db) / 8] ^= 1 << ((i + d - db) % 8);
    return r;
}
bigint *bi_poly_mul(BI_CTX *ctx, bigint *a, bigint *b)
{
    (void)ctx;
    bigint *r = new_bigint();
    for (int i = 0; i < 128; i++)
        if (BIT(a)[i / 8] >> (i % 8) & 1)
            for (int j = 0; j < 128; j++)
                if (BIT(b)[j / 8] >> (j % 8) & 1) BIT(r)[(i + j) / 8] ^= 1 << ((i + j) % 8);
    return r;
}
bigint *trim(bigint *bi) { return bi; }
void bi_wirte_to_byte(bigint *bi, u8 *data) { for (int i = 0; i < 16; i++) data[i] = BIT(bi)[15 - i]; }
void bi_free(BI_CTX *ctx, bigint *bi) { (void)ctx; free(bi->comps); free(bi); }
void bi_terminate(BI_CTX *ctx) { (void)ctx; }

/* Hex string to bytes, in the order written. */
static void hex(u8 *out, const char *s)
{
    for (; s[0]; s += 2) { unsigned v; sscanf(s, "%2x", &v); *out++ = (u8)v; }
}
static int same(const u8 *a, const char *expected, int len)
{
    u8 e[16];
    hex(e, expected);
    return !memcmp(a, e, len);
}

/* Modular power for the table checks. */
static unsigned powmod(unsigned base, unsigned exp, unsigned mod)
{
    unsigned r = 1;
    while (exp--) r = r * base % mod;
    return r;
}

/* Test vectors: Bluetooth Core Specification, the legacy security sample
 * data (Vol 2 Part G "Four tests of E1", "Four tests of E21", "Three tests
 * of E22", "Tests of E22 with PIN augmenting", "Four tests of E3"), and for
 * the key length reduction the current Vol 2 Part G 1.1.1 "Generating
 * K_session from K_enc" (L = 1 to 16).  The legacy data is as quoted by
 * github.com/bobwenstudy/BluetoothCryptographicToolboxClassic; the
 * K_session data is from the specification text itself.
 *
 * Byte order: the specification's vectors are printed as hex strings, and the
 * SDK takes them with byte 0 the first byte of the string (K, RAND, address,
 * PIN, COF, SRES, ACO, results alike).  Only the key length reduction takes
 * a number: Kc is the least significant byte first in the SDK's buffer, so
 * the specification's K_enc and K_session (most significant first) are the
 * hex string reversed. */
static const struct { const char *rand, *address, *k, *sres, *aco; } e1_vectors[] = {
    { "00000000000000000000000000000000", "000000000000", "00000000000000000000000000000000", "056c0fe6", "48afcdd4bd40fef76693b113" },
    { "bc3f30689647c8d7c5a03ca80a91eceb", "7ca89b233c2d", "159dd9f43fc3d328efba0cd8a861fa57", "8d5205c5", "3ed75df4abd9af638d144e94" },
    { "0891caee063f5da1809577ff94ccdcfb", "c62f19f6ce98", "45298d06e46bac21421ddfbed94c032b", "00507e5f", "2a5f19fbf60907e69f39ca9f" },
    { "0ecd61782b4128480c05dc45542b1b8c", "f428f0e624b3", "35949a914225fabad91995d226de1d92", "80e5629c", "a6fe4dcde3924611d3cc6ba1" },
};
static const struct { const char *rand, *address, *ka; } e21_vectors[] = {
    { "00000000000000000000000000000000", "000000000000", "d14ca028545ec262cee700e39b5c39ee" },
    { "2dd9a550343191304013b2d7e1189d09", "cac4364303b6", "e62f8bac609139b3999aedbc9d228042" },
    { "dab3cffe9d5739d1b7bf4a667ae5ee24", "02f8fd4cd661", "b0376d0a9b338c2e133c32b69cb816b3" },
    { "13ecad08ad63c37f8a54dc56e82f4dc1", "9846c5ead4d9", "5b61e83ad04d23e9d1c698851fa30447" },
};
static const struct { const char *rand, *pin, *address, *kinit; int len; } e22_vectors[] = {
    { "001de169248850245a5f7cc7f0d6d633", "d5a51083a04a1971f18649ea8b79311a", "000000000000", "539e4f2732e5ae2de1e0401f0813bd0d", 16 },
    { "67ed56bfcf99825f0c6b349369da30ab", "7885b515e84b1f082cc499976f1725ce", "000000000000", "04435771e03a9daceb8bb1a493ee9bd8", 16 },
    { "40a94509238664f244ff8e3d13b119d3", "1ce44839badde30396d03c4c36f23006", "000000000000", "9cde4b60f9b5861ed9df80858bac6f7f", 16 },
    /* PIN augmenting: the address fills what the PIN leaves of 16 bytes. */
    { "24b101fd56117d42c0545a4247357048", "fd397c7f5c1f937cdf82d8816cc377e2", "000000000000", "a5f2adf328e4e6a2b42f19c8b74ba884", 16 },
    { "321964061ac49a436f9fb9824ac63f8b", "ad955d58b6b8857820ac1262d617a6", "0314c0642543", "c0ec1a5694e2b48d54297911e6c98b8f", 15 },
};
static const struct { const char *rand, *cof, *k, *kc; } e3_vectors[] = {
    { "00000000000000000000000000000000", "48afcdd4bd40fef76693b113", "00000000000000000000000000000000", "cc802aecc7312285912e90af6a1e1154" },
    { "950e604e655ea3800fe3eb4a28918087", "68f4f472b5586ac5850f5f74", "34e86915d20c485090a6977931f96df5", "c1beafea6e747e304cf0bd7734b0a9e2" },
    { "6a8ebcf5e6e471505be68d5eb8a3200c", "658d791a9554b77c0b2f7b9f", "35cf77b333c294671d426fa79993a133", "a3032b4df1cceba8adc1a04427224299" },
    { "5ecd6d75db322c75b6afbd799cb18668", "63f701c7013238bbf88714ee", "b9f90c53206792b1826838b435b87d4d", "ea520cfc546b00eb7c3a6cea3ecb39ed" },
};
static const struct { const char *k_enc, *k_session; } session_vectors[16] = {
    { "a2b230a493f281bb61a85b82a9d4a30e", "7aa16f3959836ba322049a7b87f1d8a5" },
    { "64e7df78bb7ccaa4614331235b3222ad", "142057bb0bceac4c58bd142e1e710a50" },
    { "575e5156ba685dc6112124acedb2c179", "d56d0adb8216cb397fe3c5911ff95618" },
    { "8917b4fc403b6db21596b86d1cb8adab", "91910128b0e2f5eda132a03eaf3d8cda" },
    { "785c915bdd25b9c60102ab00b6cd2a68", "6fb5651ccb80c8d7ea1ee56df1ec5d02" },
    { "5e77d19f55ccd7d5798f9a323b83e5d8", "16096bcbafcf8def1d226a1b4d3f9a3d" },
    { "05454e038ddcfbe3ed024b2d92b7f54c", "50f9c0d4e3178da94a09fe0d34f67b0e" },
    { "7ce149fcf4b38ad72a5d8a41eb15ba31", "532c36d45d0954e0922989b6826f78dc" },
    { "5eeff7ca84fc27829c0517263df6f36e", "016313f60d3771cf7f8e4bb94aa6827d" },
    { "7b13846e88beb4de34e7160afd44dc65", "023bc1ec34a0029ef798dcfb618ba58d" },
    { "bda6de6c6e7d757e8dfe2d499a181193", "022e08a93aa51d8d2f93fa7885cc1f87" },
    { "e6483b1c2cdb10409a658f97c4efd90d", "030d752b216fe29bb880275cd7e6f6f9" },
    { "d79d281da22668476b223c46dc0ab9ee", "03f111389cebf91900b938084ac158aa" },
    { "cad9a65b9fca1c1da2320fcf7c4ae48e", "284840fdf1305f3c529f570376adf7cf" },
    { "21f0cc31049b7163d375e9e106029809", "7f10b53b6df84b94f22e566a3754a37e" },
    { "35ec8fc3d50ccd325f2fd907bde206de", "35ec8fc3d50ccd325f2fd907bde206de" },
};

/* The specification's number, most significant byte first, as the SDK's
 * least-significant-first buffer. */
static void number(u8 *out, const char *s) { hex(out, s); endian_change(out, 16); }

int main(void)
{
#ifdef CONFIG_CPU_BD19
    uECC_Curve secp160 = uECC_secp160r1();
    uECC_Curve secp192 = uECC_secp192r1();
    uECC_Curve secp224 = uECC_secp224r1();
    uECC_Curve secp256 = uECC_secp256r1();
    uECC_Curve secp256k1 = uECC_secp256k1();
    CHECK(secp160 && secp160->num_words == 5 && secp160->num_bytes == 20 &&
          secp160->num_n_bits == 161);
    CHECK(secp192 && secp192->num_words == 6 && secp192->num_bytes == 24 &&
          secp192->num_n_bits == 192);
    CHECK(secp224 && secp224->num_words == 7 && secp224->num_bytes == 28 &&
          secp224->num_n_bits == 224);
    CHECK(secp256 && secp256->num_words == 8 && secp256->num_bytes == 32 &&
          secp256->num_n_bits == 256);
    CHECK(secp256k1 && secp256k1->num_words == 8 &&
          secp256k1->num_bytes == 32 && secp256k1->num_n_bits == 256);
    CHECK(secp256->double_jacobian && secp256->x_side && secp256->mmod_fast);
#endif

    /* The SDK's tables are the specification's formulas. */
    for (unsigned x = 0; x < 256; x++) {
        CHECK(etable[x] == powmod(45, x, 257) % 256);
        CHECK(ltable[etable[x]] == x);
    }
    for (unsigned p = 0; p < 17; p++)
        for (unsigned k = 0; k < 16; k++) {
            unsigned inner = powmod(45, 17 * (p + 1) + (15 - k) + 1, 257);
            CHECK(B[p][k] == powmod(45, inner, 257) % 256);
        }

    u8 out[16], address[6], rand[16], k[16], cof[12], pin[16];
    for (unsigned i = 0; i < sizeof(e1_vectors) / sizeof(*e1_vectors); i++) {
        hex(address, e1_vectors[i].address); hex(rand, e1_vectors[i].rand); hex(k, e1_vectors[i].k);
        function_E1(address, rand, k, out);
        CHECK(same(out, e1_vectors[i].sres, 4));
        CHECK(same(out + 4, e1_vectors[i].aco, 12));
    }
    for (unsigned i = 0; i < sizeof(e21_vectors) / sizeof(*e21_vectors); i++) {
        hex(address, e21_vectors[i].address); hex(rand, e21_vectors[i].rand);
        function_E21(address, rand, out);
        CHECK(same(out, e21_vectors[i].ka, 16));
    }
    for (unsigned i = 0; i < sizeof(e22_vectors) / sizeof(*e22_vectors); i++) {
        hex(address, e22_vectors[i].address); hex(rand, e22_vectors[i].rand); hex(pin, e22_vectors[i].pin);
        function_E22(pin, rand, (u8)e22_vectors[i].len, address, out);
        CHECK(same(out, e22_vectors[i].kinit, 16));
    }
    for (unsigned i = 0; i < sizeof(e3_vectors) / sizeof(*e3_vectors); i++) {
        hex(rand, e3_vectors[i].rand); hex(cof, e3_vectors[i].cof); hex(k, e3_vectors[i].k);
        function_E3(k, rand, cof, 16, out);
        CHECK(same(out, e3_vectors[i].kc, 16));
    }
    /* Key length reduction, on the number's byte order.  The SDK's g1/g2
     * rows for L = 5, 13 and 14 are not the specification's: row 5 repeats
     * row 4 and rows 13 and 14 repeat row 12.  The SDK's behaviour is kept,
     * so for those lengths the check is that the result is the repeated
     * row's reduction and differs from the specification's. */
    for (int len = 1; len <= 16; len++) {
        u8 key[16], want[16], repeated[16];
        number(key, session_vectors[len - 1].k_enc);
        number(want, session_vectors[len - 1].k_session);
        if (len != 16) Entrypt_Key_Length_Change(key, (u8)len);
        int repeats = len == 5 ? 4 : len == 13 || len == 14 ? 12 : 0;
        if (!repeats) {
            CHECK(!memcmp(key, want, 16));
        } else {
            number(repeated, session_vectors[len - 1].k_enc);
            Entrypt_Key_Length_Change(repeated, (u8)repeats);
            CHECK(!memcmp(key, repeated, 16));
            CHECK(memcmp(key, want, 16));
        }
    }
    /* E3 applies it to its result. */
    {
        u8 full[16];
        hex(rand, e3_vectors[1].rand); hex(cof, e3_vectors[1].cof); hex(k, e3_vectors[1].k);
        function_E3(k, rand, cof, 16, full);
        for (int len = 1; len < 16; len++) {
            Entrypt_Key_Length_Change(full, (u8)len);
            function_E3(k, rand, cof, (u8)len, out);
            CHECK(!memcmp(out, full, 16));
            function_E3(k, rand, cof, 16, full);
        }
    }
    /* With the Classic module bit clear the SDK's own allocator is used. */
    config_btctler_modules = 0;
    hex(address, e21_vectors[1].address); hex(rand, e21_vectors[1].rand);
    function_E21(address, rand, out);
    CHECK(same(out, e21_vectors[1].ka, 16));
    config_btctler_modules = 3;
    CHECK(!asserted && !lmp_allocs);
    return TEST_DONE("btctrler_authentication");
}
#endif
