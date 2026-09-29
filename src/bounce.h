#ifndef CYCLONE2_BOUNCE_H
#define CYCLONE2_BOUNCE_H

#include "generic/typedef.h"

/* Watch the button lines for up to 3 s and print, per line, how many
 * changes (presses and releases) it saw and how long they bounced.
 * Blocks the calling task (the console) meanwhile.  -1: bad duration. */
int cyclone2_bounce_capture(u32 seconds, int raw_output);

#endif
