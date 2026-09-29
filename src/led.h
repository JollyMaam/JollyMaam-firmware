#ifndef CYCLONE2_LED_H
#define CYCLONE2_LED_H

#include "generic/typedef.h"
#include "lights.h"

/* The LED hardware only.  What to show is decided in lights.c; the one
 * other writer is the jump to the ROM loader (usb.c), which never returns. */
void cyclone2_led_initialize(void);
void cyclone2_led_write(const struct cyclone2_light_frame *frame);

#endif
