/* BD19 CTE direction finding SDK member occurrence 1. */
#include "btctrler/glue.h"
#include <stdlib.h>
#include <stdio.h>
extern void log_print(int, const char *, const char *, ...);

const s16 arctan_table[257] = {0, 1, 3, 4, 5, 6, 8, 9, 10, 11, 13, 14, 15, 17, 18, 19, 20, 22, 23, 24, 25, 27, 28, 29, 30, 32, 33, 34, 36, 37, 38, 39, 41, 42, 43, 44, 46, 47, 48, 49, 51, 52, 53, 54, 55, 57, 58, 59, 60, 62, 63, 64, 65, 67, 68, 69, 70, 71, 73, 74, 75, 76, 77, 79, 80, 81, 82, 83, 85, 86, 87, 88, 89, 91, 92, 93, 94, 95, 96, 98, 99, 100, 101, 102, 103, 104, 106, 107, 108, 109, 110, 111, 112, 114, 115, 116, 117, 118, 119, 120, 121, 122, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 188, 189, 190, 191, 192, 193, 194, 195, 195, 196, 197, 198, 199, 200, 201, 201, 202, 203, 204, 205, 206, 206, 207, 208, 209, 210, 211, 211, 212, 213, 214, 215, 215, 216, 217, 218, 219, 219, 220, 221, 222, 222, 223, 224, 225, 225, 226, 227, 228, 228, 229, 230, 231, 231, 232, 233, 234, 234, 235, 236, 236, 237, 238, 239, 239, 240, 241, 241, 242, 243, 243, 244, 245, 245, 246, 247, 248, 248, 249, 250, 250, 251, 251, 252, 253, 253, 254, 255, 255, 256};
const s16 sin_table[128] = {0, 6, 13, 19, 25, 31, 38, 44, 50, 56, 63, 69, 75, 81, 88, 94, 100, 106, 112, 118, 124, 130, 137, 143, 149, 155, 161, 167, 172, 178, 184, 190, 196, 202, 207, 213, 219, 225, 230, 236, 241, 247, 252, 258, 263, 269, 274, 279, 284, 290, 295, 300, 305, 310, 315, 320, 325, 330, 334, 339, 344, 348, 353, 358, 362, 366, 371, 375, 379, 384, 388, 392, 396, 400, 404, 407, 411, 415, 419, 422, 426, 429, 433, 436, 439, 442, 445, 449, 452, 454, 457, 460, 463, 465, 468, 471, 473, 475, 478, 480, 482, 484, 486, 488, 490, 492, 493, 495, 497, 498, 500, 501, 502, 503, 504, 505, 506, 507, 508, 509, 510, 510, 511, 511, 511, 512, 512, 512};
const u16 acos_lut[257] = {2048, 2043, 2038, 2033, 2028, 2023, 2017, 2012, 2007, 2002, 1997, 1992, 1987, 1982, 1977, 1972, 1966, 1961, 1956, 1951, 1946, 1941, 1936, 1931, 1926, 1920, 1915, 1910, 1905, 1900, 1895, 1890, 1885, 1879, 1874, 1869, 1864, 1859, 1854, 1849, 1843, 1838, 1833, 1828, 1823, 1818, 1812, 1807, 1802, 1797, 1792, 1787, 1781, 1776, 1771, 1766, 1760, 1755, 1750, 1745, 1740, 1734, 1729, 1724, 1719, 1713, 1708, 1703, 1697, 1692, 1687, 1682, 1676, 1671, 1666, 1660, 1655, 1650, 1644, 1639, 1634, 1628, 1623, 1618, 1612, 1607, 1601, 1596, 1590, 1585, 1580, 1574, 1569, 1563, 1558, 1552, 1547, 1541, 1536, 1530, 1525, 1519, 1514, 1508, 1503, 1497, 1491, 1486, 1480, 1475, 1469, 1463, 1458, 1452, 1446, 1441, 1435, 1429, 1423, 1418, 1412, 1406, 1400, 1395, 1389, 1383, 1377, 1371, 1365, 1359, 1354, 1348, 1342, 1336, 1330, 1324, 1318, 1312, 1306, 1300, 1294, 1287, 1281, 1275, 1269, 1263, 1257, 1251, 1244, 1238, 1232, 1225, 1219, 1213, 1206, 1200, 1194, 1187, 1181, 1174, 1168, 1161, 1155, 1148, 1141, 1135, 1128, 1121, 1115, 1108, 1101, 1094, 1087, 1081, 1074, 1067, 1060, 1053, 1046, 1038, 1031, 1024, 1017, 1010, 1002, 995, 988, 980, 973, 965, 958, 950, 942, 935, 927, 919, 911, 903, 895, 887, 879, 871, 862, 854, 846, 837, 829, 820, 811, 803, 794, 785, 776, 767, 757, 748, 739, 729, 719, 710, 700, 690, 680, 669, 659, 648, 638, 627, 615, 604, 593, 581, 569, 557, 544, 532, 519, 505, 492, 478, 463, 449, 433, 417, 401, 384, 366, 347, 327, 306, 283, 258, 231, 200, 163, 115, 0};
const u32 log_table[81] = {20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 71, 80, 89, 100, 112, 126, 142, 159, 178, 200, 224, 252, 283, 317, 356, 399, 448, 502, 564, 632, 710, 796, 893, 1002, 1125, 1262, 1416, 1589, 1783, 2000, 2244, 2518, 2825, 3170, 3557, 3991, 4477, 5024, 5637, 6325, 7096, 7962, 8934, 10024, 11247, 12619, 14159, 15887, 17825, 20000, 22440, 25179, 28251, 31698, 35566, 39905, 44774, 50238, 56368, 63246, 70963, 79621, 89337, 100237, 112468, 126191, 141589, 158866, 178250, 200000};
const float m_table[21] = {0x1.43c9ee0000000p-2f, 0x1.6b50b00000000p-2f, 0x1.97a7860000000p-2f, 0x1.c96bba0000000p-2f, 0x1.009d4a0000000p-1f, 0x1.1fe5ca0000000p-1f, 0x1.43126e0000000p-1f, 0x1.6a71de0000000p-1f, 0x1.96ae7e0000000p-1f, 0x1.c8587a0000000p-1f, 0x1.0000000000000p+0f, 0x1.1f3b640000000p+0f, 0x1.4247460000000p+0f, 0x1.69999a0000000p+0f, 0x1.95bc020000000p+0f, 0x1.c73eac0000000p+0f, 0x1.fecbfc0000000p+0f, 0x1.1e8db80000000p+1f, 0x1.4185f00000000p+1f, 0x1.68c1540000000p+1f, 0x1.94c6400000000p+1f};
const u8 cte_data[354] __attribute__((aligned(1))) = {
    0x52, 0x4c, 0x00, 0x70, 0xf4, 0x38, 0x04, 0x00, 0x77, 0x24, 0x07, 0xdd, 0x44, 0x1d, 0x31, 0xd6, 0xfe, 0x16,
    0x83, 0xd2, 0x67, 0x0c, 0xf9, 0xce, 0x32, 0x03, 0x25, 0xcd, 0x72, 0xfb, 0x81, 0xcd, 0x68, 0xf4, 0x81, 0xce,
    0x66, 0xec, 0x39, 0xd1, 0x09, 0x22, 0x2e, 0xfb, 0xa1, 0x16, 0xbb, 0x3e, 0xf0, 0xca, 0xeb, 0x3c, 0x19, 0xcb,
    0xf1, 0x09, 0x6d, 0xfd, 0x27, 0xf1, 0x8f, 0x30, 0x61, 0x00, 0x60, 0x39, 0xcd, 0x37, 0x1b, 0xf4, 0x2b, 0x2d,
    0x70, 0xd7, 0x6f, 0xfb, 0xa4, 0xf1, 0x54, 0xde, 0xaf, 0x16, 0x18, 0xde, 0x24, 0x17, 0x6f, 0x05, 0x96, 0xfa,
    0xc1, 0x18, 0xfa, 0xdb, 0xb1, 0x1b, 0x7f, 0xd8, 0x1f, 0x0b, 0xc2, 0xe7, 0xe7, 0xe5, 0xc6, 0x1e, 0x5a, 0xf6,
    0x33, 0x1c, 0xe0, 0x3b, 0xf7, 0xcf, 0x4b, 0x41, 0x56, 0xcd, 0x92, 0x10, 0x22, 0xfc, 0x8c, 0xf1, 0xb1, 0x30,
    0x39, 0xfd, 0xc5, 0x3d, 0x83, 0x32, 0x59, 0xf8, 0xa8, 0x2d, 0xbe, 0xd7, 0xda, 0xfd, 0xad, 0xf0, 0xa3, 0xde,
    0x16, 0x16, 0x55, 0xdd, 0x7a, 0x17, 0x0f, 0x04, 0xb3, 0xfb, 0xfc, 0x18, 0x2c, 0xdf, 0x9b, 0x1f, 0xbc, 0xd9,
    0xf3, 0x0e, 0xd8, 0xe6, 0xe7, 0xe7, 0xfe, 0x1d, 0x21, 0xf3, 0xcf, 0x21, 0x10, 0x39, 0x70, 0xd7, 0xf5, 0x45,
    0xe6, 0xce, 0xfb, 0x14, 0xdd, 0xfa, 0x44, 0xf2, 0x45, 0x30, 0xab, 0xf9, 0xc2, 0x41, 0x48, 0x2c, 0xb3, 0xfc,
    0x8d, 0x2e, 0xe8, 0xd7, 0x8a, 0x02, 0x87, 0xec, 0xfb, 0xe0, 0x9c, 0x11, 0x07, 0xdb, 0x5f, 0x17, 0x34, 0x01,
    0x90, 0xfe, 0x50, 0x19, 0x05, 0xe3, 0x02, 0x23, 0x46, 0xdb, 0x73, 0x12, 0x32, 0xe4, 0x10, 0xeb, 0x6b, 0x1c,
    0x87, 0xf0, 0x9b, 0x26, 0xf2, 0x35, 0x2e, 0xdc, 0xf8, 0x48, 0x0e, 0xd1, 0x38, 0x19, 0xdf, 0xf8, 0x29, 0xf2,
    0x7e, 0x2f, 0xdf, 0xf3, 0x65, 0x46, 0xf8, 0x24, 0x2e, 0xff, 0x3b, 0x2e, 0x51, 0xd8, 0x63, 0x06, 0x71, 0xe9,
    0x18, 0xe3, 0x1b, 0x0f, 0xb2, 0xd9, 0xbe, 0x17, 0x5a, 0xff, 0x85, 0x00, 0x6b, 0x19, 0x59, 0xe4, 0x34, 0x24,
    0x34, 0xdd, 0xae, 0x15, 0xc3, 0xe2, 0xe4, 0xec, 0xed, 0x1a, 0xd8, 0xee, 0xd1, 0x29, 0x49, 0x33, 0xd4, 0xe3,
    0x2b, 0x4c, 0xb9, 0xd1, 0xdc, 0x1b, 0x84, 0xf8, 0x77, 0xf3, 0xc8, 0x2d, 0xb0, 0xef, 0x50, 0x4a, 0x0b, 0x1c,
    0xba, 0x05, 0x96, 0x2e, 0xe8, 0xd8, 0x0b, 0x0a, 0x1c, 0xe7, 0x4b, 0xe5, 0x0a, 0x00, 0x4b, 0x00, 0x3c, 0xfb,
    0x7e, 0xcd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0x00,
};

static struct primarydata *primarydata_instance;
static float angle_est_fun(const primarydata_stu *ctx);
static void data_pre_processing(const u8 *input, primarydata_stu *ctx);
static void mac_data_load(primarydata_stu *ctx, u8 *i_data, u8 *q_data, u8 *rssi, u8 *agc);
float cfo_est_fun(u8, u8, u8, primarydata_stu *);
s32 frequency_offset(u8, float, s16, u8, primarydata_stu *);
__attribute__((weak)) void put_float(double value) { (void)value; }

s32 atan_fun(s32 x, s32 y)
{
    if (x == 0 || y == 0) {
        if ((x | y) == 0) return 0;
        if (x == 0 && y > 0) return 512;
        return x == 0 && y < 0 ? -512 : 0;
    }
    int rneg = x < 0, ineg = y < 0;
    s32 r = rneg ? -x : x, i = ineg ? -y : y;
    s32 base = 0, numerator = i, denominator = r;
    if (i > r) { base = 256; numerator = i - r; denominator = i + r; }
    u16 index = (u16)((double)numerator * 256.0 / (double)denominator + 0.5);
    s32 angle = arctan_table[index < 257 ? index : 257] + base;
    int quadrant = (x > 0 && ineg) || (rneg && y > 0);
    s32 result = quadrant ? -angle : angle;
    if (rneg && y > 0) return result + 1024;
    return (y & x) < 0 ? result - 1024 : result;
}

s16 sin_func(s16 *phase)
{
    int x = *phase % 512;
    if (x > 255) x -= 512;
    else if (x < -255) x += 512;
    int magnitude = x < 0 ? -x : x;
    int index = magnitude & 255;
    if (index > 127) {
        int folded = 256 - index;
        index = (folded & 255) == 128 ? 127 : folded;
    }
    *phase = x;
    return x > 0 ? sin_table[index & 255] : -sin_table[index & 255];
}

void complex_multi(s32 ar, s32 ai, s32 br, s32 bi, s32 *real, s32 *imag)
{
    s32 r = br * ar - bi * ai;
    s32 i = bi * ar + br * ai;
    *real = r;
    *imag = i;
}

s32 frequency_offset(u8 len, float step, s16 initial, u8 reference, primarydata_stu *ctx)
{
    s16 phase = initial, cosine_phase = initial + 128;
    float phasef = (float)initial;
    int maximum = 0;
    /* Reference mode always processes eight entries, even when len is zero. */
    int count = reference == 1 ? 8 : len;
    for (int n = 0; n < count; ++n) {
        int sine = sin_func(&phase), cosine = sin_func(&cosine_phase);
        int r = reference == 1 ? ctx->reference_data_r[n] : ctx->sam_slot_data_r[n];
        int i = reference == 1 ? ctx->reference_data_i[n] : ctx->sam_slot_data_i[n];
        int nr = (s32)((u32)(r * cosine) + 256u - (u32)(i * sine)) / 512;
        int ni = (s32)((u32)(r * sine) + 256u + (u32)(i * cosine)) / 512;
        if (reference == 1) {
            ctx->reference_data_r[n] = nr;
            ctx->reference_data_i[n] = ni;
        } else {
            ctx->sam_slot_data_r[n] = nr;
            ctx->sam_slot_data_i[n] = ni;
        }
        int ar = (s16)nr, ai = (s16)ni;
        if (ar < 0) ar = -ar;
        if (ai < 0) ai = -ai;
        if ((u32)maximum < (u32)ar) maximum = ar;
        if ((u32)maximum < (u32)ai) maximum = ai;
        phasef += step;
        if (phasef > 512.0f) phasef -= 512.0f;
        else if (phasef < -512.0f) phasef += 512.0f;
        phase = (s16)phasef;
        cosine_phase = phase + 128;
    }
    return maximum;
}

float cfo_est_fun(u8 len, u8 gap, u8 reference, primarydata_stu *ctx)
{
    int real = 0, imag = 0;
    if (reference == 1) {
        for (int n = 1; n < 8; ++n) {
            int r = ctx->reference_data_r[n], i = ctx->reference_data_i[n];
            int pr = ctx->reference_data_r[n - 1], pi = ctx->reference_data_i[n - 1];
            real += (s32)((u32)(pr * r) + 64u + (u32)(pi * i)) / 128;
            imag += (s32)((u32)(pr * i) + 64u - (u32)(pi * r)) / 128;
        }
    } else {
        if (len <= gap) return 0.0f;
        for (int n = gap; n < len; ++n) {
            int r = ctx->sam_slot_data_r[n], i = ctx->sam_slot_data_i[n];
            int pr = ctx->sam_slot_data_r[n - gap], pi = ctx->sam_slot_data_i[n - gap];
            real += (s32)((u32)(pr * r) + 64u + (u32)(pi * i)) / 128;
            imag += (s32)((u32)(pr * i) + 64u - (u32)(pi * r)) / 128;
        }
    }
    return ((float)atan_fun(real, imag) * 0.25f) / (float)gap;
}

int cte_data_algorithm_module_init(void)
{
    primarydata_instance = malloc(sizeof(*primarydata_instance));
    return primarydata_instance == NULL;
}

void cte_data_algorithm_module_close(void)
{
    /* SDK deliberately leaves the pointer unchanged after free. */
    if (primarydata_instance) free(primarydata_instance);
}

float distance_est(primarydata_stu *ctx, float reference_power, float sensitivity)
{
    s8 ref = (s8)(0u - (u8)(ctx->RSSI + ctx->AGC_index));
    double scaled = (ctx->sampleslot_data_abs_sum / ctx->reference_data_abs_sum) * 2000.0;
    s8 row;
    /* Unordered comparisons enter the scan, whose strict intervals skip equality. */
    if (!(scaled <= 20.0)) {
        if (!(scaled >= 200000.0)) {
            row = 79;
            for (int j = 0; j < 79; ++j) {
                if (scaled > (double)log_table[j] && scaled < (double)log_table[j + 1]) {
                    row = j;
                    break;
                }
            }
        } else row = 80;
    } else row = 0;
    float correction = (float)((double)((int)row - 40) * 0.5);
    float power = (float)ref + correction;
    printf("ref_power:%d avg_pwr_final:", (int)ref);
    put_float((double)power);
    float exponentf = (reference_power - power) / (sensitivity * 10.0f);
    s8 exponent = (s8)exponentf;
    float fraction = exponentf - (float)exponent;
    if (fraction > 0.5f) {
        exponent = (s8)(exponent + 1);
        fraction = exponentf - (float)exponent;
    } else if (fraction < -0.5f) {
        exponent = (s8)(exponent - 1);
        fraction = exponentf - (float)exponent;
    }
    float mantissa = (float)((double)(fraction * 10.0f) * 2.0 + 0.5);
    s8 index = (s8)(mantissa + 10.0f);
    int clamped = index < 0 ? 0 : index < 20 ? index : 20;
    float result = m_table[clamped];
    while (exponent != 0) {
        if (exponent > 0) { result *= 10.0f; exponent = (s8)(exponent - 1); }
        else { result /= 10.0f; exponent = (s8)(exponent + 1); }
    }
    return result;
}

static void data_pre_processing(const u8 *data, primarydata_stu *ctx)
{
    ctx->antenna_max_amp = 0;
    __builtin_memset(&ctx->reference_data_abs_sum, 0, 16);
    for (int n = 0, off = 8; off < 40; off += 4, ++n) {
        int r = data[off + 2] | ((u32)data[off + 3] << 8);
        int i = data[off] | ((u32)data[off + 1] << 8);
        if (r > 32768) r |= -65536;
        if (i > 32768) i |= -65536;
        ctx->reference_data_r[n] = r;
        ctx->reference_data_i[n] = i;
        double sum = ctx->reference_data_abs_sum + (double)(r * r);
        ctx->reference_data_abs_sum = (double)(i * i) + sum;
    }
    ctx->reference_data_abs_sum *= 0.125;
    int off = 40, n = 1;
    for (;;) {
        /* The terminal record is read even when data_len is small. */
        int r = data[off + 2] | ((u32)data[off + 3] << 8);
        int i = data[off] | ((u32)data[off + 1] << 8);
        if (r > 32768) r |= -65536;
        if (i > 32768) i |= -65536;
        int rr = r / 8, ii = i / 8;
        if (off < (int)ctx->data_len - 16) {
            ctx->sam_slot_data_i[n] = i;
            ctx->sam_slot_data_r[n] = r;
            double sum = (double)(rr * rr) + ctx->sampleslot_data_abs_sum;
            ctx->sampleslot_data_abs_sum = (double)(ii * ii) + sum;
            off = (s16)(off + 4);
            n = (s16)(n + 1);
        } else {
            ctx->sam_slot_data_r[0] = r;
            ctx->sam_slot_data_i[0] = i;
            double sum = (double)(rr * rr) + ctx->sampleslot_data_abs_sum;
            sum = (double)(ii * ii) + sum;
            ctx->sampleslot_data_abs_sum = (sum * 64.0) / (double)n;
            ctx->freq_offset = data[off - 2] | ((u16)data[off - 1] << 8);
            ctx->RSSI = data[off - 4];
            ctx->AGC_index = data[off - 3];
            break;
        }
    }
}

static void mac_data_load(primarydata_stu *ctx, u8 *real, u8 *imag, u8 *rssi, u8 *agc)
{
    float reference_cfo = -0.0f - cfo_est_fun(8, 1, 1, ctx);
    (void)frequency_offset(8, reference_cfo, 0, 1, ctx);
    u8 count = ctx->sam_slot_num;
    float sample_cfo = -0.0f - cfo_est_fun(count, ctx->antenna_num, 0, ctx);
    int sample_max = frequency_offset(count, sample_cfo, 0, 0, ctx);
    int r = ctx->reference_data_r[7], i = ctx->reference_data_i[7];
    int sr = ctx->sam_slot_data_r[0], si = ctx->sam_slot_data_i[0];
    int pr = (s32)((u32)(sr * r) + (u32)(si * i));
    int pi = (s32)((u32)(si * r) - (u32)(sr * i));
    int phase = atan_fun(pr, pi);
    int ref_max = frequency_offset(8, 0.0f, (s16)((phase + 2) / 4), 1, ctx);
    u8 slots = ctx->sam_slot_num;
    ctx->antenna_max_amp = (u32)ref_max > (u32)sample_max ? ref_max : sample_max;
    for (int n = 0; n < 8; ++n) {
        real[n] = (ctx->reference_data_r[n] * 127 + 8001) / ctx->antenna_max_amp;
        imag[n] = (ctx->reference_data_i[n] * 127 + 8001) / ctx->antenna_max_amp;
    }
    int end = (u8)(slots + 7);
    for (int n = 8; n < end; ++n) {
        real[n] = (ctx->sam_slot_data_r[n - 7] * 127 + 8001) / ctx->antenna_max_amp;
        imag[n] = (ctx->sam_slot_data_i[n - 7] * 127 + 8001) / ctx->antenna_max_amp;
    }
    *rssi = ctx->RSSI;
    *agc = ctx->AGC_index;
}

int cte_data_conversion(void *data, u8 samples, u8 antennas, s8 *real, s8 *imag)
{
    u8 rssi, agc;
    struct primarydata *ctx = primarydata_instance;
    ctx->antenna_num = antennas;
    ctx->data_len = ((u16)samples << 2) + 26;
    ctx->sam_slot_num = samples - 7;
    data_pre_processing(data, ctx);
    mac_data_load(ctx, (u8 *)real, (u8 *)imag, &rssi, &agc);
    return 0;
}

int cte_data_angle_calc(void *data, u8 samples, u8 antennas, s8 *real, s8 *imag)
{
    u8 rssi, agc;
    struct primarydata *ctx = primarydata_instance;
    ctx->wave_len = 12.5f;
    ctx->antenna_dista = 4.35f;
    ctx->antenna_num = antennas;
    ctx->antenna_num_row = 4;
    ctx->antenna_num_col = 4;
    ctx->data_len = ((u16)samples << 2) + 26;
    ctx->sam_slot_num = samples - 7;
    data_pre_processing(data, ctx);
    mac_data_load(ctx, (u8 *)real, (u8 *)imag, &rssi, &agc);
    float angle = angle_est_fun(ctx);
    puts("angle:");
    put_float((double)angle);
    (void)IQ_data_assessment(primarydata_instance);
    float distance = distance_est(primarydata_instance, -50.0f, 2.5f);
    puts("distance:");
    put_float((double)distance);
    return (int)angle;
}

int IQ_data_assessment(primarydata_stu *ctx)
{
    s32 antennas=ctx->antenna_num, slots=ctx->sam_slot_num;
    s32 count=antennas, amp=0;
    float angle_sum=0.0f;
    if (antennas < slots) {
        s32 stop=antennas-1;
        do {
            if (stop>0) {
                for (s32 k=0;k<stop;++k) {
                    u8 a=(u8)(((count-antennas+k)&255));
                    u8 b=(u8)((k+count)&255);
                    /* SDK GEPs can reach neighboring fields in the final group. */
                    s16 ar16, ai16, br16, bi16;
                    __builtin_memcpy(&ar16, __builtin_assume_aligned((u8 *)ctx+offsetof(primarydata_stu, sam_slot_data_r)+2u*a, 2), 2);
                    __builtin_memcpy(&ai16, __builtin_assume_aligned((u8 *)ctx+offsetof(primarydata_stu, sam_slot_data_i)+2u*a, 2), 2);
                    __builtin_memcpy(&br16, __builtin_assume_aligned((u8 *)ctx+offsetof(primarydata_stu, sam_slot_data_r)+2u*b, 2), 2);
                    __builtin_memcpy(&bi16, __builtin_assume_aligned((u8 *)ctx+offsetof(primarydata_stu, sam_slot_data_i)+2u*b, 2), 2);
                    s32 ar=ar16, ai=ai16, br=br16, bi=bi16;
                    s32 phase=atan_fun((s32)((u32)(bi*ai)+(u32)(br*ar)), (s32)((u32)(bi*ar)-(u32)(br*ai)));
                    angle_sum += (float)(phase>0 ? phase : -phase);
                    amp += (br>0 ? br : -br) +
                           (bi>0 ? bi : -bi);
                }
            }
            count += antennas;
            if (count >= slots) break;
        } while (1);
    }
    float denom=(float)(u8)slots;
    printf("amp jitter:"); put_float((double)((float)amp/denom));
    printf("angle jitter:"); put_float((double)(angle_sum/denom));
    return 0;
}

void cte_data_test_case(void *priv)
{
    (void)priv;
    primarydata_stu *ctx=malloc(sizeof(*ctx));
    primarydata_instance=ctx;
    u8 *buffers=malloc(164);
    ctx->wave_len=12.5f; ctx->antenna_dista=4.35f;
    ctx->antenna_num=16; ctx->antenna_num_row=4; ctx->antenna_num_col=4;
    ctx->data_len=354; ctx->sam_slot_num=74;
    log_print(2, NULL, "[Info]: [CTE_AGRTHM]ant_cnt:%d slot_num:%d data_len:%d\n\r\n", ctx->antenna_num, 74, 354);
    ctx=primarydata_instance;
    data_pre_processing(cte_data,ctx);
    u8 test_rssi, test_agc;
    mac_data_load(ctx,buffers,buffers+82,&test_rssi,&test_agc);
    float angle=angle_est_fun(ctx);
    puts("angle:"); put_float(angle);
    float distance=distance_est(primarydata_instance,-50.0f,2.5f);
    puts("distance:"); put_float(distance);
    if (buffers) { printf("free i_data:%x\n",buffers); free(buffers); }
    cte_data_algorithm_module_close();
}
/* Recovered from BD19 angle_est_fun; rows/cols<=1 leave SDK undef angles. */
static float angle_est_fun(const struct primarydata *ctx)
{
    int rows = ctx->antenna_num_row;
    float row_angle, col_angle;
    if (rows > 1) {
        int real = 0, imag = 0;
        for (int group = rows; group < ctx->sam_slot_num; group += rows) {
            for (u8 j = 0; (int)j < rows - 1; ++j) {
                int index = group - rows + j;
                int a = ctx->sam_slot_data_r[(u8)index];
                int b = ctx->sam_slot_data_i[(u8)index];
                int c = ctx->sam_slot_data_r[(u8)(index + 1)];
                int d = ctx->sam_slot_data_i[(u8)(index + 1)];
                /* Individual s16 products fit i32; sums/differences wrap. */
                real += (int)((u32)(d * b) + (u32)(c * a)) / 64;
                imag += (int)((u32)(d * a) - (u32)(c * b)) / 64;
            }
        }
        float phase = (float)atan_fun(real, imag);
        float ratio = (phase * ctx->wave_len) / (ctx->antenna_dista * 2048.0f);
        int index = (int)((double)ratio * 256.0 + 0.5);
        index = index > 256 ? 256 : index > -256 ? index : -256;
        if (index < 0)
            row_angle = (float)(180.0 - ((double)acos_lut[-index] * 90.0) * 0x1p-11);
        else
            row_angle = (float)((double)((u32)acos_lut[index] * 90u) * 0x1p-11);
    }
    int antennas = ctx->antenna_num;
    int cols = ctx->antenna_num_col;
    if (cols > 1) {
        int real = 0, imag = 0;
        for (int group = antennas; group < ctx->sam_slot_num; group += antennas) {
            for (u8 j = 0; (int)j < rows - 1; ++j) {
                for (u8 k = 0; (int)k < cols - 1; ++k) {
                    int index = group - antennas + j + k * rows;
                    int a = ctx->sam_slot_data_r[(u8)index];
                    int b = ctx->sam_slot_data_i[(u8)index];
                    int c = ctx->sam_slot_data_r[(u8)(index + rows)];
                    int d = ctx->sam_slot_data_i[(u8)(index + rows)];
                    real += (int)((u32)(d * b) + (u32)(c * a)) / 64;
                    imag += (int)((u32)(d * a) - (u32)(c * b)) / 64;
                }
            }
        }
        float phase = (float)atan_fun(real, imag);
        float ratio = (phase * ctx->wave_len) / (ctx->antenna_dista * 2048.0f);
        int index = (int)((double)ratio * 256.0 + 0.5);
        index = index > 256 ? 256 : index > -256 ? index : -256;
        if (index < 0)
            col_angle = (float)(180.0 - ((double)acos_lut[-index] * 90.0) * 0x1p-11);
        else
            col_angle = (float)((double)((u32)acos_lut[index] * 90u) * 0x1p-11);
    }
    if (cols == 1)
        return (row_angle > 0.0f ? 180.0f : -180.0f) - row_angle;
    float angle;
    float row_delta = row_angle - 90.0f;
    float col_delta = col_angle - 90.0f;
    /* Ordered comparisons and negative zero match the SDK's fabs selects. */
    float abs_row = row_delta > 0.0f ? row_delta : -0.0f - row_delta;
    float abs_col = col_delta > 0.0f ? col_delta : -0.0f - col_delta;
    if (cols == 1) {
        angle = row_angle;
    } else if (row_angle >= 90.0f && col_angle >= 90.0f) {
        if (abs_row < 12.0f) angle = -0.0f - row_angle;
        else if (abs_col < 12.0f) angle = (col_angle + 90.0f) - 360.0f;
        else angle = (((col_angle + 90.0f) - 360.0f) - row_angle) * 0.5f;
    } else if (row_angle <= 90.0f && col_angle >= 90.0f) {
        if (abs_row < 12.0f) angle = -0.0f - row_angle;
        else if (abs_col < 12.0f) angle = 90.0f - col_angle;
        else angle = ((90.0f - col_angle) - row_angle) * 0.5f;
    } else if (row_angle >= 90.0f && col_angle <= 90.0f) {
        if (abs_row < 12.0f) angle = row_angle;
        else if (abs_col < 12.0f) angle = col_angle + 90.0f;
        else angle = ((row_angle + 90.0f) + col_angle) * 0.5f;
    } else {
        if (abs_row < 12.0f) angle = row_angle;
        else if (abs_col < 12.0f) angle = 90.0f - col_angle;
        else angle = ((90.0f - row_angle) + col_angle) * 0.5f;
    }
    return (angle > 0.0f ? 180.0f : -180.0f) - angle;
}

/* Host regression fixtures. */
#ifdef UNITTEST
#include "test.h"
#include <string.h>
#include <math.h>
void log_print(int level, const char *tag, const char *format, ...)
{ (void)level; (void)tag; (void)format; }

int main(void)
{
    CHECK(atan_fun(0, 0) == 0 && atan_fun(0, 1) == 512);
    CHECK(atan_fun(0, -1) == -512 && atan_fun(1, 1) == 256);
    CHECK(atan_fun(1, -1) == -256 && atan_fun(-1, 1) == 768);
    s16 phase = 128;
    CHECK(sin_func(&phase) == 512 && phase == 128);
    phase = -128; CHECK(sin_func(&phase) == -512 && phase == -128);
    phase = 512; CHECK(sin_func(&phase) == 0 && phase == 0);
    s32 real, imag;
    complex_multi(3, 4, 5, 6, &real, &imag);
    CHECK(real == -9 && imag == 38);
    /* Aliased outputs retain the original real-then-imag store order. */
    complex_multi(3, 4, 5, 6, &real, &real); CHECK(real == 38);

    /* Raw IR uses strict >32768 conversion for power, but truncates storage. */
    u8 raw[60] = { 0 };
    for (int off = 8; off < 48; off += 4) raw[off + 3] = 128;
    struct primarydata context = { .data_len = 60 };
    data_pre_processing(raw, &context);
    CHECK(context.reference_data_r[0] == -32768 && context.reference_data_i[0] == 0);
    CHECK(context.reference_data_abs_sum == 1073741824.0);
    CHECK(context.sampleslot_data_abs_sum == 1073741824.0);
    CHECK(context.sam_slot_data_r[0] == -32768 && context.sam_slot_data_r[1] == -32768);
    CHECK(context.freq_offset == -32768 && context.RSSI == 0 && context.AGC_index == 0);
    CHECK(frequency_offset(0, 0.0f, 0, 0, &context) == 0);
    CHECK(cfo_est_fun(1, 1, 0, &context) == 0.0f);

    /* Exact log endpoints and NaN fall through all 79 strict intervals. */
    context.RSSI = -50; context.reference_data_abs_sum = 2000;
    context.sampleslot_data_abs_sum = 22;
    CHECK(distance_est(&context, 69.5f, 2.5f) == 1.0f);
    context.sampleslot_data_abs_sum = NAN;
    CHECK(distance_est(&context, 69.5f, 2.5f) == 1.0f);
    context.sampleslot_data_abs_sum = 20;
    CHECK(distance_est(&context, 30.0f, 2.5f) == 1.0f);

    CHECK(cte_data_algorithm_module_init() == 0);
    s8 r[82] = { 0 }, i[82] = { 0 };
    /* Original SDK LLVM sample: sums9633/11085, angle49; final byte untouched. */
    CHECK(cte_data_conversion((void *)cte_data, 81, 16, r, i) == 0);
    unsigned rs = 0, is = 0;
    for (int j = 0; j < 82; ++j) { rs += (u8)r[j]; is += (u8)i[j]; }
    CHECK(rs == 9633 && is == 11085 && r[81] == 0 && i[81] == 0);
    CHECK(cte_data_angle_calc((void *)cte_data, 81, 16, r, i) == 49);
    struct primarydata *stale = primarydata_instance;
    cte_data_algorithm_module_close();
    CHECK(primarydata_instance == stale);
    primarydata_instance = NULL;
    cte_data_algorithm_module_close();
    /* Exercise the SDK's self-contained diagnostic vector and lifecycle. */
    cte_data_test_case(NULL);
    primarydata_instance = NULL;
    return TEST_DONE("btctrler_bd19_cte_data_algorithm");
}
#endif
