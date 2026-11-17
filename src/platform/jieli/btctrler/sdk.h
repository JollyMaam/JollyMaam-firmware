/* Controller layouts are recovered independently for each SDK target. */
#ifndef BTCTRLER_SDK_SELECTOR_H
#define BTCTRLER_SDK_SELECTOR_H
#ifdef CONFIG_CPU_BD19
#include "sdk_bd19.h"
#else
#include "sdk_br23.h"
#endif
#endif
