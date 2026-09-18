#include "system/includes.h"

const int vm_max_size_config = VM_MAX_SIZE_CONFIG; // from cflags

/* required by app */
u8 get_vbat_percent(void)
{
  return 0xff;
}

/* required by ??? */
u8 is_pwm_led_on(void)
{
  return 0;
}

/* required by cpu/br23/liba/cpu.a(startup.S.o) */
u16 update_result_get(void)
{
  return 0; /* UPDATA_NON from include_lib/update/update.h */
};
