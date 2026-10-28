#include "../sdk.h"

static const struct esco_codec_ops *const switch_table[4] = {
    &esco_ulaw_codec, &esco_alaw_codec, 0, 0
};

const struct esco_codec_ops *esco_codec_for_type(int type)
{
    if ((unsigned)type < 4) return switch_table[type];
    return 0;
}

#if defined(UNITTEST) && !defined(ESCO_SELECTOR_ONLY)
#include <assert.h>
int main(void)
{
    assert(esco_codec_for_type(0) == &esco_ulaw_codec);
    assert(esco_codec_for_type(1) == &esco_alaw_codec);
    assert(esco_codec_for_type(2) == 0 && esco_codec_for_type(-1) == 0);
    return 0;
}
#endif
