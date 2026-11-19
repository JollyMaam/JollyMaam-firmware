/* micro-ecc curve specializations for the BD19 crypto_toolbox ROM.
 *
 * Copyright (c) 2015, Kenneth MacKay
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The BD19 integration provides the uECC_vli_* operations externally.  Curve
 * descriptors include exactly the three callbacks used by the ROM ABI.
 */
#ifndef BTCTRLER_BD19_UECC_CURVES_INC
#define BTCTRLER_BD19_UECC_CURVES_INC

#define num_bytes_secp160r1 20
#define num_bytes_secp192r1 24
#define num_bytes_secp224r1 28
#define num_bytes_secp256r1 32
#define num_bytes_secp256k1 32

#define num_words_secp160r1 5
#define num_words_secp192r1 6
#define num_words_secp224r1 7
#define num_words_secp256r1 8
#define num_words_secp256k1 8

#define BYTES_TO_WORDS_8(a, b, c, d, e, f, g, h) 0x##d##c##b##a, 0x##h##g##f##e
#define BYTES_TO_WORDS_4(a, b, c, d) 0x##d##c##b##a

uECC_word_t uECC_vli_isZero(const uECC_word_t *vli, wordcount_t num_words);
uECC_word_t uECC_vli_testBit(const uECC_word_t *vli, bitcount_t bit);
void uECC_vli_set(uECC_word_t *dest, const uECC_word_t *src,
                  wordcount_t num_words);
void uECC_vli_rshift1(uECC_word_t *vli, wordcount_t num_words);
uECC_word_t uECC_vli_add(uECC_word_t *result, const uECC_word_t *left,
                         const uECC_word_t *right, wordcount_t num_words);
uECC_word_t uECC_vli_sub(uECC_word_t *result, const uECC_word_t *left,
                         const uECC_word_t *right, wordcount_t num_words);
s8 uECC_vli_cmp_unsafe(const uECC_word_t *left, const uECC_word_t *right,
                       wordcount_t num_words);
void uECC_vli_clear(uECC_word_t *vli, wordcount_t num_words);
void uECC_vli_modSquare_fast(uECC_word_t *result, const uECC_word_t *left,
                             uECC_Curve curve);
void uECC_vli_modMult_fast(uECC_word_t *result, const uECC_word_t *left,
                           const uECC_word_t *right, uECC_Curve curve);
void uECC_vli_modAdd(uECC_word_t *result, const uECC_word_t *left,
                     const uECC_word_t *right, const uECC_word_t *mod,
                     wordcount_t num_words);
void uECC_vli_modSub(uECC_word_t *result, const uECC_word_t *left,
                     const uECC_word_t *right, const uECC_word_t *mod,
                     wordcount_t num_words);

static void double_jacobian_default(uECC_word_t *X1, uECC_word_t *Y1,
                                    uECC_word_t *Z1, uECC_Curve curve)
{
    uECC_word_t t4[8];
    uECC_word_t t5[8];
    wordcount_t num_words = curve->num_words;

    if (uECC_vli_isZero(Z1, num_words))
        return;

    uECC_vli_modSquare_fast(t4, Y1, curve);
    uECC_vli_modMult_fast(t5, X1, t4, curve);
    uECC_vli_modSquare_fast(t4, t4, curve);
    uECC_vli_modMult_fast(Y1, Y1, Z1, curve);
    uECC_vli_modSquare_fast(Z1, Z1, curve);
    uECC_vli_modAdd(X1, X1, Z1, curve->p, num_words);
    uECC_vli_modAdd(Z1, Z1, Z1, curve->p, num_words);
    uECC_vli_modSub(Z1, X1, Z1, curve->p, num_words);
    uECC_vli_modMult_fast(X1, X1, Z1, curve);
    uECC_vli_modAdd(Z1, X1, X1, curve->p, num_words);
    uECC_vli_modAdd(X1, X1, Z1, curve->p, num_words);
    if (uECC_vli_testBit(X1, 0)) {
        uECC_word_t carry = uECC_vli_add(X1, X1, curve->p, num_words);
        uECC_vli_rshift1(X1, num_words);
        X1[num_words - 1] |= carry << 31;
    } else {
        uECC_vli_rshift1(X1, num_words);
    }
    uECC_vli_modSquare_fast(Z1, X1, curve);
    uECC_vli_modSub(Z1, Z1, t5, curve->p, num_words);
    uECC_vli_modSub(Z1, Z1, t5, curve->p, num_words);
    uECC_vli_modSub(t5, t5, Z1, curve->p, num_words);
    uECC_vli_modMult_fast(X1, X1, t5, curve);
    uECC_vli_modSub(t4, X1, t4, curve->p, num_words);
    uECC_vli_set(X1, Z1, num_words);
    uECC_vli_set(Z1, Y1, num_words);
    uECC_vli_set(Y1, t4, num_words);
}

static void x_side_default(uECC_word_t *result, const uECC_word_t *x,
                           uECC_Curve curve)
{
    uECC_word_t _3[8] = {3};
    wordcount_t num_words = curve->num_words;
    uECC_vli_modSquare_fast(result, x, curve);
    uECC_vli_modSub(result, result, _3, curve->p, num_words);
    uECC_vli_modMult_fast(result, result, x, curve);
    uECC_vli_modAdd(result, result, curve->b, curve->p, num_words);
}

static void vli_mmod_fast_secp160r1(uECC_word_t *result,
                                    uECC_word_t *product);
static void omega_mult_secp160r1(uECC_word_t *result,
                                 const uECC_word_t *right);

static const struct uECC_Curve_t curve_secp160r1 = {
    num_words_secp160r1,
    num_bytes_secp160r1,
    161,
    { BYTES_TO_WORDS_8(FF, FF, FF, 7F, FF, FF, FF, FF),
      BYTES_TO_WORDS_8(FF, FF, FF, FF, FF, FF, FF, FF),
      BYTES_TO_WORDS_4(FF, FF, FF, FF) },
    { BYTES_TO_WORDS_8(57, 22, 75, CA, D3, AE, 27, F9),
      BYTES_TO_WORDS_8(C8, F4, 01, 00, 00, 00, 00, 00),
      BYTES_TO_WORDS_8(00, 00, 00, 00, 01, 00, 00, 00) },
    { BYTES_TO_WORDS_8(82, FC, CB, 13, B9, 8B, C3, 68),
      BYTES_TO_WORDS_8(89, 69, 64, 46, 28, 73, F5, 8E),
      BYTES_TO_WORDS_4(68, B5, 96, 4A),
      BYTES_TO_WORDS_8(32, FB, C5, 7A, 37, 51, 23, 04),
      BYTES_TO_WORDS_8(12, C9, DC, 59, 7D, 94, 68, 31),
      BYTES_TO_WORDS_4(55, 28, A6, 23) },
    { BYTES_TO_WORDS_8(45, FA, 65, C5, AD, D4, D4, 81),
      BYTES_TO_WORDS_8(9F, F8, AC, 65, 8B, 7A, BD, 54),
      BYTES_TO_WORDS_4(FC, BE, 97, 1C) },
    &double_jacobian_default,
    &x_side_default,
    &vli_mmod_fast_secp160r1
};

uECC_Curve uECC_secp160r1(void)
{
    return &curve_secp160r1;
}

static void vli_mmod_fast_secp160r1(uECC_word_t *result,
                                    uECC_word_t *product)
{
    uECC_word_t tmp[2 * num_words_secp160r1];
    uECC_word_t carry;
    uECC_vli_clear(tmp, num_words_secp160r1);
    uECC_vli_clear(tmp + num_words_secp160r1, num_words_secp160r1);
    omega_mult_secp160r1(tmp, product + num_words_secp160r1);
    carry = uECC_vli_add(result, product, tmp, num_words_secp160r1);
    uECC_vli_clear(product, num_words_secp160r1);
    omega_mult_secp160r1(product, tmp + num_words_secp160r1);
    carry += uECC_vli_add(result, result, product, num_words_secp160r1);
    while (carry > 0) {
        --carry;
        uECC_vli_sub(result, result, curve_secp160r1.p,
                     num_words_secp160r1);
    }
    if (uECC_vli_cmp_unsafe(result, curve_secp160r1.p,
                            num_words_secp160r1) > 0)
        uECC_vli_sub(result, result, curve_secp160r1.p,
                     num_words_secp160r1);
}

static void omega_mult_secp160r1(uECC_word_t *result,
                                 const uECC_word_t *right)
{
    uECC_word_t carry;
    unsigned i;
    uECC_vli_set(result + 1, right, num_words_secp160r1);
    uECC_vli_rshift1(result + 1, num_words_secp160r1);
    result[0] = right[0] << 31;
    carry = uECC_vli_add(result, result, right, num_words_secp160r1);
    for (i = num_words_secp160r1; carry; ++i) {
        u64 sum = (u64)result[i] + carry;
        result[i] = (u32)sum;
        carry = (u32)(sum >> 32);
    }
}

static void vli_mmod_fast_secp192r1(uECC_word_t *result,
                                    uECC_word_t *product);

static const struct uECC_Curve_t curve_secp192r1 = {
    num_words_secp192r1,
    num_bytes_secp192r1,
    192,
    { BYTES_TO_WORDS_8(FF, FF, FF, FF, FF, FF, FF, FF),
      BYTES_TO_WORDS_8(FE, FF, FF, FF, FF, FF, FF, FF),
      BYTES_TO_WORDS_8(FF, FF, FF, FF, FF, FF, FF, FF) },
    { BYTES_TO_WORDS_8(31, 28, D2, B4, B1, C9, 6B, 14),
      BYTES_TO_WORDS_8(36, F8, DE, 99, FF, FF, FF, FF),
      BYTES_TO_WORDS_8(FF, FF, FF, FF, FF, FF, FF, FF) },
    { BYTES_TO_WORDS_8(12, 10, FF, 82, FD, 0A, FF, F4),
      BYTES_TO_WORDS_8(00, 88, A1, 43, EB, 20, BF, 7C),
      BYTES_TO_WORDS_8(F6, 90, 30, B0, 0E, A8, 8D, 18),
      BYTES_TO_WORDS_8(11, 48, 79, 1E, A1, 77, F9, 73),
      BYTES_TO_WORDS_8(D5, CD, 24, 6B, ED, 11, 10, 63),
      BYTES_TO_WORDS_8(78, DA, C8, FF, 95, 2B, 19, 07) },
    { BYTES_TO_WORDS_8(B1, B9, 46, C1, EC, DE, B8, FE),
      BYTES_TO_WORDS_8(49, 30, 24, 72, AB, E9, A7, 0F),
      BYTES_TO_WORDS_8(E7, 80, 9C, E5, 19, 05, 21, 64) },
    &double_jacobian_default,
    &x_side_default,
    &vli_mmod_fast_secp192r1
};

uECC_Curve uECC_secp192r1(void)
{
    return &curve_secp192r1;
}

static void vli_mmod_fast_secp192r1(uECC_word_t *result,
                                    uECC_word_t *product)
{
    uECC_word_t tmp[num_words_secp192r1];
    int carry;
    uECC_vli_set(result, product, num_words_secp192r1);
    uECC_vli_set(tmp, &product[6], num_words_secp192r1);
    carry = uECC_vli_add(result, result, tmp, num_words_secp192r1);
    tmp[0] = tmp[1] = 0;
    tmp[2] = product[6];
    tmp[3] = product[7];
    tmp[4] = product[8];
    tmp[5] = product[9];
    carry += uECC_vli_add(result, result, tmp, num_words_secp192r1);
    tmp[0] = tmp[2] = product[10];
    tmp[1] = tmp[3] = product[11];
    tmp[4] = tmp[5] = 0;
    carry += uECC_vli_add(result, result, tmp, num_words_secp192r1);
    while (carry || uECC_vli_cmp_unsafe(curve_secp192r1.p, result,
                                        num_words_secp192r1) != 1)
        carry -= uECC_vli_sub(result, result, curve_secp192r1.p,
                              num_words_secp192r1);
}

static void vli_mmod_fast_secp224r1(uECC_word_t *result,
                                    uECC_word_t *product);

static const struct uECC_Curve_t curve_secp224r1 = {
    num_words_secp224r1,
    num_bytes_secp224r1,
    224,
    { BYTES_TO_WORDS_8(01, 00, 00, 00, 00, 00, 00, 00),
      BYTES_TO_WORDS_8(00, 00, 00, 00, FF, FF, FF, FF),
      BYTES_TO_WORDS_8(FF, FF, FF, FF, FF, FF, FF, FF),
      BYTES_TO_WORDS_4(FF, FF, FF, FF) },
    { BYTES_TO_WORDS_8(3D, 2A, 5C, 5C, 45, 29, DD, 13),
      BYTES_TO_WORDS_8(3E, F0, B8, E0, A2, 16, FF, FF),
      BYTES_TO_WORDS_8(FF, FF, FF, FF, FF, FF, FF, FF),
      BYTES_TO_WORDS_4(FF, FF, FF, FF) },
    { BYTES_TO_WORDS_8(21, 1D, 5C, 11, D6, 80, 32, 34),
      BYTES_TO_WORDS_8(22, 11, C2, 56, D3, C1, 03, 4A),
      BYTES_TO_WORDS_8(B9, 90, 13, 32, 7F, BF, B4, 6B),
      BYTES_TO_WORDS_4(BD, 0C, 0E, B7),
      BYTES_TO_WORDS_8(34, 7E, 00, 85, 99, 81, D5, 44),
      BYTES_TO_WORDS_8(64, 47, 07, 5A, A0, 75, 43, CD),
      BYTES_TO_WORDS_8(E6, DF, 22, 4C, FB, 23, F7, B5),
      BYTES_TO_WORDS_4(88, 63, 37, BD) },
    { BYTES_TO_WORDS_8(B4, FF, 55, 23, 43, 39, 0B, 27),
      BYTES_TO_WORDS_8(BA, D8, BF, D7, B7, B0, 44, 50),
      BYTES_TO_WORDS_8(56, 32, 41, F5, AB, B3, 04, 0C),
      BYTES_TO_WORDS_4(85, 0A, 05, B4) },
    &double_jacobian_default,
    &x_side_default,
    &vli_mmod_fast_secp224r1
};

uECC_Curve uECC_secp224r1(void)
{
    return &curve_secp224r1;
}

static void vli_mmod_fast_secp224r1(uECC_word_t *result,
                                    uECC_word_t *product)
{
    uECC_word_t tmp[num_words_secp224r1];
    int carry;
    uECC_vli_set(result, product, num_words_secp224r1);
    tmp[0] = tmp[1] = tmp[2] = 0;
    tmp[3] = product[7];
    tmp[4] = product[8];
    tmp[5] = product[9];
    tmp[6] = product[10];
    carry = uECC_vli_add(result, result, tmp, num_words_secp224r1);
    tmp[3] = product[11];
    tmp[4] = product[12];
    tmp[5] = product[13];
    tmp[6] = 0;
    carry += uECC_vli_add(result, result, tmp, num_words_secp224r1);
    tmp[0] = product[7];
    tmp[1] = product[8];
    tmp[2] = product[9];
    tmp[3] = product[10];
    tmp[4] = product[11];
    tmp[5] = product[12];
    tmp[6] = product[13];
    carry -= uECC_vli_sub(result, result, tmp, num_words_secp224r1);
    tmp[0] = product[11];
    tmp[1] = product[12];
    tmp[2] = product[13];
    tmp[3] = tmp[4] = tmp[5] = tmp[6] = 0;
    carry -= uECC_vli_sub(result, result, tmp, num_words_secp224r1);
    if (carry < 0) {
        do {
            carry += uECC_vli_add(result, result, curve_secp224r1.p,
                                  num_words_secp224r1);
        } while (carry < 0);
    } else {
        while (carry || uECC_vli_cmp_unsafe(curve_secp224r1.p, result,
                                            num_words_secp224r1) != 1)
            carry -= uECC_vli_sub(result, result, curve_secp224r1.p,
                                  num_words_secp224r1);
    }
}

static void vli_mmod_fast_secp256r1(uECC_word_t *result,
                                    uECC_word_t *product);

static const struct uECC_Curve_t curve_secp256r1 = {
    num_words_secp256r1,
    num_bytes_secp256r1,
    256,
    { BYTES_TO_WORDS_8(FF, FF, FF, FF, FF, FF, FF, FF),
      BYTES_TO_WORDS_8(FF, FF, FF, FF, 00, 00, 00, 00),
      BYTES_TO_WORDS_8(00, 00, 00, 00, 00, 00, 00, 00),
      BYTES_TO_WORDS_8(01, 00, 00, 00, FF, FF, FF, FF) },
    { BYTES_TO_WORDS_8(51, 25, 63, FC, C2, CA, B9, F3),
      BYTES_TO_WORDS_8(84, 9E, 17, A7, AD, FA, E6, BC),
      BYTES_TO_WORDS_8(FF, FF, FF, FF, FF, FF, FF, FF),
      BYTES_TO_WORDS_8(00, 00, 00, 00, FF, FF, FF, FF) },
    { BYTES_TO_WORDS_8(96, C2, 98, D8, 45, 39, A1, F4),
      BYTES_TO_WORDS_8(A0, 33, EB, 2D, 81, 7D, 03, 77),
      BYTES_TO_WORDS_8(F2, 40, A4, 63, E5, E6, BC, F8),
      BYTES_TO_WORDS_8(47, 42, 2C, E1, F2, D1, 17, 6B),
      BYTES_TO_WORDS_8(F5, 51, BF, 37, 68, 40, B6, CB),
      BYTES_TO_WORDS_8(CE, 5E, 31, 6B, 57, 33, CE, 2B),
      BYTES_TO_WORDS_8(16, 9E, 0F, 7C, 4A, EB, E7, 8E),
      BYTES_TO_WORDS_8(9B, 7F, 1A, FE, E2, 42, E3, 4F) },
    { BYTES_TO_WORDS_8(4B, 60, D2, 27, 3E, 3C, CE, 3B),
      BYTES_TO_WORDS_8(F6, B0, 53, CC, B0, 06, 1D, 65),
      BYTES_TO_WORDS_8(BC, 86, 98, 76, 55, BD, EB, B3),
      BYTES_TO_WORDS_8(E7, 93, 3A, AA, D8, 35, C6, 5A) },
    &double_jacobian_default,
    &x_side_default,
    &vli_mmod_fast_secp256r1
};

uECC_Curve uECC_secp256r1(void)
{
    return &curve_secp256r1;
}

static void vli_mmod_fast_secp256r1(uECC_word_t *result,
                                    uECC_word_t *product)
{
    uECC_word_t tmp[num_words_secp256r1];
    int carry;
    uECC_vli_set(result, product, num_words_secp256r1);
    tmp[0] = tmp[1] = tmp[2] = 0;
    tmp[3] = product[11];
    tmp[4] = product[12];
    tmp[5] = product[13];
    tmp[6] = product[14];
    tmp[7] = product[15];
    carry = uECC_vli_add(tmp, tmp, tmp, num_words_secp256r1);
    carry += uECC_vli_add(result, result, tmp, num_words_secp256r1);
    tmp[3] = product[12];
    tmp[4] = product[13];
    tmp[5] = product[14];
    tmp[6] = product[15];
    tmp[7] = 0;
    carry += uECC_vli_add(tmp, tmp, tmp, num_words_secp256r1);
    carry += uECC_vli_add(result, result, tmp, num_words_secp256r1);
    tmp[0] = product[8];
    tmp[1] = product[9];
    tmp[2] = product[10];
    tmp[3] = tmp[4] = tmp[5] = 0;
    tmp[6] = product[14];
    tmp[7] = product[15];
    carry += uECC_vli_add(result, result, tmp, num_words_secp256r1);
    tmp[0] = product[9];
    tmp[1] = product[10];
    tmp[2] = product[11];
    tmp[3] = product[13];
    tmp[4] = product[14];
    tmp[5] = product[15];
    tmp[6] = product[13];
    tmp[7] = product[8];
    carry += uECC_vli_add(result, result, tmp, num_words_secp256r1);
    tmp[0] = product[11];
    tmp[1] = product[12];
    tmp[2] = product[13];
    tmp[3] = tmp[4] = tmp[5] = 0;
    tmp[6] = product[8];
    tmp[7] = product[10];
    carry -= uECC_vli_sub(result, result, tmp, num_words_secp256r1);
    tmp[0] = product[12];
    tmp[1] = product[13];
    tmp[2] = product[14];
    tmp[3] = product[15];
    tmp[4] = tmp[5] = 0;
    tmp[6] = product[9];
    tmp[7] = product[11];
    carry -= uECC_vli_sub(result, result, tmp, num_words_secp256r1);
    tmp[0] = product[13];
    tmp[1] = product[14];
    tmp[2] = product[15];
    tmp[3] = product[8];
    tmp[4] = product[9];
    tmp[5] = product[10];
    tmp[6] = 0;
    tmp[7] = product[12];
    carry -= uECC_vli_sub(result, result, tmp, num_words_secp256r1);
    tmp[0] = product[14];
    tmp[1] = product[15];
    tmp[2] = 0;
    tmp[3] = product[9];
    tmp[4] = product[10];
    tmp[5] = product[11];
    tmp[6] = 0;
    tmp[7] = product[13];
    carry -= uECC_vli_sub(result, result, tmp, num_words_secp256r1);
    if (carry < 0) {
        do {
            carry += uECC_vli_add(result, result, curve_secp256r1.p,
                                  num_words_secp256r1);
        } while (carry < 0);
    } else {
        while (carry || uECC_vli_cmp_unsafe(curve_secp256r1.p, result,
                                            num_words_secp256r1) != 1)
            carry -= uECC_vli_sub(result, result, curve_secp256r1.p,
                                  num_words_secp256r1);
    }
}

static void double_jacobian_secp256k1(uECC_word_t *X1, uECC_word_t *Y1,
                                      uECC_word_t *Z1, uECC_Curve curve);
static void x_side_secp256k1(uECC_word_t *result, const uECC_word_t *x,
                             uECC_Curve curve);
static void vli_mmod_fast_secp256k1(uECC_word_t *result,
                                    uECC_word_t *product);
static void omega_mult_secp256k1(uECC_word_t *result,
                                 const uECC_word_t *right);

static const struct uECC_Curve_t curve_secp256k1 = {
    num_words_secp256k1,
    num_bytes_secp256k1,
    256,
    { BYTES_TO_WORDS_8(2F, FC, FF, FF, FE, FF, FF, FF),
      BYTES_TO_WORDS_8(FF, FF, FF, FF, FF, FF, FF, FF),
      BYTES_TO_WORDS_8(FF, FF, FF, FF, FF, FF, FF, FF),
      BYTES_TO_WORDS_8(FF, FF, FF, FF, FF, FF, FF, FF) },
    { BYTES_TO_WORDS_8(41, 41, 36, D0, 8C, 5E, D2, BF),
      BYTES_TO_WORDS_8(3B, A0, 48, AF, E6, DC, AE, BA),
      BYTES_TO_WORDS_8(FE, FF, FF, FF, FF, FF, FF, FF),
      BYTES_TO_WORDS_8(FF, FF, FF, FF, FF, FF, FF, FF) },
    { BYTES_TO_WORDS_8(98, 17, F8, 16, 5B, 81, F2, 59),
      BYTES_TO_WORDS_8(D9, 28, CE, 2D, DB, FC, 9B, 02),
      BYTES_TO_WORDS_8(07, 0B, 87, CE, 95, 62, A0, 55),
      BYTES_TO_WORDS_8(AC, BB, DC, F9, 7E, 66, BE, 79),
      BYTES_TO_WORDS_8(B8, D4, 10, FB, 8F, D0, 47, 9C),
      BYTES_TO_WORDS_8(19, 54, 85, A6, 48, B4, 17, FD),
      BYTES_TO_WORDS_8(A8, 08, 11, 0E, FC, FB, A4, 5D),
      BYTES_TO_WORDS_8(65, C4, A3, 26, 77, DA, 3A, 48) },
    { BYTES_TO_WORDS_8(07, 00, 00, 00, 00, 00, 00, 00),
      BYTES_TO_WORDS_8(00, 00, 00, 00, 00, 00, 00, 00),
      BYTES_TO_WORDS_8(00, 00, 00, 00, 00, 00, 00, 00),
      BYTES_TO_WORDS_8(00, 00, 00, 00, 00, 00, 00, 00) },
    &double_jacobian_secp256k1,
    &x_side_secp256k1,
    &vli_mmod_fast_secp256k1
};

uECC_Curve uECC_secp256k1(void)
{
    return &curve_secp256k1;
}

static void double_jacobian_secp256k1(uECC_word_t *X1, uECC_word_t *Y1,
                                      uECC_word_t *Z1, uECC_Curve curve)
{
    uECC_word_t t4[num_words_secp256k1];
    uECC_word_t t5[num_words_secp256k1];
    if (uECC_vli_isZero(Z1, num_words_secp256k1))
        return;
    uECC_vli_modSquare_fast(t5, Y1, curve);
    uECC_vli_modMult_fast(t4, X1, t5, curve);
    uECC_vli_modSquare_fast(X1, X1, curve);
    uECC_vli_modSquare_fast(t5, t5, curve);
    uECC_vli_modMult_fast(Z1, Y1, Z1, curve);
    uECC_vli_modAdd(Y1, X1, X1, curve->p, num_words_secp256k1);
    uECC_vli_modAdd(Y1, Y1, X1, curve->p, num_words_secp256k1);
    if (uECC_vli_testBit(Y1, 0)) {
        uECC_word_t carry =
            uECC_vli_add(Y1, Y1, curve->p, num_words_secp256k1);
        uECC_vli_rshift1(Y1, num_words_secp256k1);
        Y1[num_words_secp256k1 - 1] |= carry << 31;
    } else {
        uECC_vli_rshift1(Y1, num_words_secp256k1);
    }
    uECC_vli_modSquare_fast(X1, Y1, curve);
    uECC_vli_modSub(X1, X1, t4, curve->p, num_words_secp256k1);
    uECC_vli_modSub(X1, X1, t4, curve->p, num_words_secp256k1);
    uECC_vli_modSub(t4, t4, X1, curve->p, num_words_secp256k1);
    uECC_vli_modMult_fast(Y1, Y1, t4, curve);
    uECC_vli_modSub(Y1, Y1, t5, curve->p, num_words_secp256k1);
}

static void x_side_secp256k1(uECC_word_t *result, const uECC_word_t *x,
                             uECC_Curve curve)
{
    uECC_vli_modSquare_fast(result, x, curve);
    uECC_vli_modMult_fast(result, result, x, curve);
    uECC_vli_modAdd(result, result, curve->b, curve->p,
                    num_words_secp256k1);
}

static void vli_mmod_fast_secp256k1(uECC_word_t *result,
                                    uECC_word_t *product)
{
    uECC_word_t tmp[2 * num_words_secp256k1];
    uECC_word_t carry;
    uECC_vli_clear(tmp, num_words_secp256k1);
    uECC_vli_clear(tmp + num_words_secp256k1, num_words_secp256k1);
    omega_mult_secp256k1(tmp, product + num_words_secp256k1);
    carry = uECC_vli_add(result, product, tmp, num_words_secp256k1);
    uECC_vli_clear(product, num_words_secp256k1);
    omega_mult_secp256k1(product, tmp + num_words_secp256k1);
    carry += uECC_vli_add(result, result, product, num_words_secp256k1);
    while (carry > 0) {
        --carry;
        uECC_vli_sub(result, result, curve_secp256k1.p,
                     num_words_secp256k1);
    }
    if (uECC_vli_cmp_unsafe(result, curve_secp256k1.p,
                            num_words_secp256k1) > 0)
        uECC_vli_sub(result, result, curve_secp256k1.p,
                     num_words_secp256k1);
}

static void omega_mult_secp256k1(uECC_word_t *result,
                                 const uECC_word_t *right)
{
    uECC_word_t carry = 0;
    wordcount_t k;
    for (k = 0; k < num_words_secp256k1; ++k) {
        u64 product = (u64)0x3D1 * right[k] + carry;
        result[k] = (u32)product;
        carry = (u32)(product >> 32);
    }
    result[num_words_secp256k1] = carry;
    result[1 + num_words_secp256k1] =
        uECC_vli_add(result + 1, result + 1, right, num_words_secp256k1);
}

#endif
