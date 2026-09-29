#ifndef CYCLONE2_LED_H
#define CYCLONE2_LED_H

#include "generic/typedef.h"

void cyclone2_led_initialize(void);
void cyclone2_led_set_all(u8 red, u8 green, u8 blue);
void cyclone2_led_set_home(u8 red, u8 green, u8 blue);

/* Only the LEDs in mask, in one colour, until a zero mask releases them. */
void cyclone2_led_debug(u32 mask, u8 red, u8 green, u8 blue);
/* Each LED its own colour: red green blue yellow cyan magenta white orange. */
void cyclone2_led_debug_legend(void);

#endif
