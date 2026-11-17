#include "board.h"
/* This dongle has no analog audio hardware. */
void plat_audio_on(void) {}
void plat_audio_off(void) {}
void plat_audio_write(const u8 *p, u32 n) { (void)p; (void)n; }
void plat_audio_volume(s16 left, s16 right, u8 mute) { (void)left; (void)right; (void)mute; }
void plat_audio_stream(int on) { (void)on; }
