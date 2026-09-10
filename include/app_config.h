#ifndef CYCLONE2_APP_CONFIG_H
#define CYCLONE2_APP_CONFIG_H

/* Import the SDK's common configuration vocabulary, then specialize the
 * unmodified board/platform sources for the Cyclone 2. */
#include_next "app_config.h"

#undef CONFIG_DEBUG_ENABLE

#undef TCFG_UART0_ENABLE
#define TCFG_UART0_ENABLE DISABLE_THIS_MOUDLE /* PA0 is button A */

#undef TCFG_ADKEY_ENABLE
#define TCFG_ADKEY_ENABLE DISABLE_THIS_MOUDLE

#undef TCFG_IOKEY_ENABLE
#define TCFG_IOKEY_ENABLE ENABLE_THIS_MOUDLE
#undef TCFG_IOKEY_POWER_ONE_PORT
#ifdef __LD__
/* app_config.h also preprocesses sdk_ld.c; keep C declarations out of that
 * pass while preserving the SDK encoding (port B = group 1, pin 11). */
#define TCFG_IOKEY_POWER_ONE_PORT (16 * 1 + 11)
#else
#include "cyclone2_board.h"
#define TCFG_IOKEY_POWER_ONE_PORT CYCLONE2_BUTTON_HOME
#endif

#undef TCFG_CODE_SWITCH_ENABLE
#define TCFG_CODE_SWITCH_ENABLE DISABLE_THIS_MOUDLE /* PB7 is M */

#endif
