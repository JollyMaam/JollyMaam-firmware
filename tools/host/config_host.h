/* What src/config.c needs from the SDK, for the host test
 * (tools/config_test.c): an in-memory syscfg and a few no-ops. */
#ifndef CONFIG_HOST_H
#define CONFIG_HOST_H
#include "generic/typedef.h"

int syscfg_read(u16 item_id, void *buf, u16 len);
int syscfg_write(u16 item_id, void *buf, u16 len);
void get_random_number(u8 *ptr, u8 len);
void cyclone2_usb_debug(const char *format, ...);
u8 *get_norflash_uuid(void);
#define local_irq_disable() ((void)0)
#define local_irq_enable() ((void)0)
#endif
