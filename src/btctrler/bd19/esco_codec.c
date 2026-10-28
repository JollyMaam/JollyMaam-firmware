#include "../sdk.h"

static const struct esco_codec_ops *const switch_table[4] = {
    &esco_ulaw_codec, &esco_alaw_codec, 0, 0
};

const struct esco_codec_ops *esco_codec_for_type(int type)
{
    if ((unsigned)type < 4) return switch_table[type];
    return 0;
}

#ifdef UNITTEST
#ifndef BTCTRLER_CODEC_LIBRARY_TEST
#include "test.h"
static u32 selector_ulaw_need(void) { return 81; }
static u32 selector_alaw_need(void) { return 82; }
/* Distinct stand-ins make this file's standalone fixture meaningful. The
 * independent combined check links both actual codec tables instead. */
const struct esco_codec_ops esco_ulaw_codec = { .need_buf = selector_ulaw_need };
const struct esco_codec_ops esco_alaw_codec = { .need_buf = selector_alaw_need };
int main(void)
{
    CHECK(esco_codec_for_type(0) == &esco_ulaw_codec);
    CHECK(esco_codec_for_type(1) == &esco_alaw_codec);
    CHECK(esco_codec_for_type(0)->need_buf() == 81);
    CHECK(esco_codec_for_type(1)->need_buf() == 82);
    CHECK(esco_codec_for_type(2) == NULL && esco_codec_for_type(3) == NULL);
    CHECK(esco_codec_for_type(-1) == NULL && esco_codec_for_type(4) == NULL);
    CHECK(esco_codec_for_type(-2147483647 - 1) == NULL && esco_codec_for_type(2147483647) == NULL);
    return TEST_DONE("bd19_esco_codec");
}
#endif
#endif
