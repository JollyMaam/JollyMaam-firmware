#include "../sdk.h"

static const s16 G711_seg_end[8] = {255, 511, 1023, 2047, 4095, 8191, 16383, 32767};

static u32 btalaw_need_buf(void) { return 1; }
static u32 btalaw_init(void *ptr) { *(u8 *)ptr = 0; return 0; }
static u32 btulaw_need_buf(void) { return 1; }
static u32 btulaw_init(void *ptr) { *(u8 *)ptr = 0; return 0; }

static u32 btalaw_encode(void *ptr, s16 *in, u8 *out, s16 len)
{
    (void)ptr;
    for (s32 i = 0; i < (s32)len; ++i) {
        s16 sample = in[i];
        u8 mask = sample >= 0 ? 213 : 85;
        if (sample < 0) sample = (s16)(0 - sample);
        s32 seg = 0;
        while (seg < 8 && G711_seg_end[seg] < sample) ++seg;
        if (seg >= 8) out[i] = mask ^ 127;
        else {
            s32 shifted = seg < 2 ? ((s32)sample >> 4) : ((s32)sample >> (seg + 3));
            out[i] = (u8)((((seg << 4) | (shifted & 15)) ^ mask));
        }
    }
    return 0;
}

static u32 btalaw_decode(void *ptr, u8 *in, s16 *out, s16 len)
{
    (void)ptr;
    for (s32 i = 0; i < (s32)len; ++i) {
        u8 code = in[i];
        u8 a = code ^ 85;
        s16 t = (s16)((a & 15) << 4);
        u32 seg = (a >> 4) & 7;
        if (seg == 0) t |= 8;
        else if (seg == 1) t |= 264;
        else t = (s16)((u16)(t | 264) << (seg - 1));
        out[i] = (code & 0x80) ? t : (s16)(0 - t);
    }
    return 0;
}

static u32 btulaw_encode(void *ptr, s16 *in, u8 *out, s16 len)
{
    (void)ptr;
    for (s32 i = 0; i < (s32)len; ++i) {
        s32 sample = in[i];
        s32 mask;
        if (sample < 0) { sample = 132 - sample; mask = 127; }
        else { sample += 132; mask = 255; }
        s32 seg = 0;
        while (seg < 8 && G711_seg_end[seg] < sample) ++seg;
        if (seg >= 8) out[i] = (u8)(mask & 128);
        else out[i] = (u8)((((sample >> (seg + 3)) & 15) | (seg << 4)) ^ mask);
    }
    return 0;
}

static u32 btulaw_decode(void *ptr, u8 *in, s16 *out, s16 len)
{
    (void)ptr;
    for (s32 i = 0; i < (s32)len; ++i) {
        u8 code = in[i], u = code ^ 255;
        s32 t = (((u << 3) & 120) | 132) << ((u >> 4) & 7);
        out[i] = (s16)(code >= 128 ? 132 - t : t - 132);
    }
    return 0;
}

const struct esco_codec_ops esco_alaw_codec = {
    btalaw_need_buf, btalaw_init, btalaw_encode, btalaw_decode
};
const struct esco_codec_ops esco_ulaw_codec = {
    btulaw_need_buf, btulaw_init, btulaw_encode, btulaw_decode
};

#ifdef UNITTEST
#include <assert.h>
#include <string.h>
int main(void)
{
    s16 samples[] = {0, 1, -1, 32767, -32768};
    u8 codes[5]; s16 decoded[5]; u8 state[2] = {0xff, 0xff};
    assert(esco_alaw_codec.need_buf() == 1 && esco_ulaw_codec.need_buf() == 1);
    assert(esco_alaw_codec.init(state) == 0 && state[0] == 0 && state[1] == 0xff);
    assert(esco_ulaw_codec.init(state + 1) == 0 && state[1] == 0);
    assert(esco_alaw_codec.encode(NULL, samples, codes, 5) == 0);
    assert(codes[0] == 0xd5 && codes[1] == 0xd5 && codes[2] == 0x55);
    assert(codes[3] == 0xaa && codes[4] == 0x55);
    assert(esco_ulaw_codec.encode(NULL, samples, codes, 5) == 0);
    assert(codes[0] == 0xff && codes[1] == 0xff && codes[2] == 0x7f);
    assert(codes[3] == 0x80 && codes[4] == 0x00);
    assert(esco_alaw_codec.decode(NULL, codes, decoded, 5) == 0);
    assert(esco_alaw_codec.decode(NULL, (u8[]){0xd5, 0x55}, decoded, 2) == 0);
    assert(decoded[0] == 8 && decoded[1] == -8);
    assert(esco_ulaw_codec.decode(NULL, (u8[]){0xff, 0x7f}, decoded, 2) == 0);
    assert(decoded[0] == 0 && decoded[1] == 0);
    for (int i = 0; i < 256; ++i) {
        u8 c = (u8)i; s16 pair[3] = {0x1234, 0, 0x2345};
        esco_alaw_codec.decode(NULL, &c, pair + 1, 1);
        esco_ulaw_codec.decode(NULL, &c, pair + 1, 1);
        assert(pair[0] == 0x1234 && pair[2] == 0x2345);
    }
    assert(esco_alaw_codec.encode(NULL, NULL, NULL, 0) == 0);
    assert(esco_ulaw_codec.decode(NULL, NULL, NULL, -1) == 0);
    memset(codes, 0x6b, sizeof(codes));
    esco_ulaw_codec.encode(NULL, samples, codes, 0);
    assert(codes[0] == 0x6b && codes[4] == 0x6b);
    esco_ulaw_codec.encode(NULL, samples, codes, -1);
    assert(codes[0] == 0x6b && codes[4] == 0x6b);
    return 0;
}
#endif
