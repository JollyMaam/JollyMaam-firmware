#ifndef CYCLONE2_LED_H
#define CYCLONE2_LED_H

#include "generic/typedef.h"

void cyclone2_led_initialize(void);
void cyclone2_led_set_all(u8 red, u8 green, u8 blue);
void cyclone2_led_set_home(u8 red, u8 green, u8 blue);

#endif
