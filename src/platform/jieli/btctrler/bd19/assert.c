/* Shared assertion boundary for owned BD19 controller members.
 * Match the existing owned controller's assertion policy: debug stop when
 * enabled, otherwise the SDK's BD19 p33 soft reset. */
#include <stdio.h>
#include "btctrler/glue.h"

void btctrler_assert_failed(const char *what, int value)
{
    if (config_asser) {
        printf("ASSERT-FAILD: %s 0x%x\n", what, value);
        cpu_assert_debug();
    } else {
        p33_soft_reset();
    }
}
