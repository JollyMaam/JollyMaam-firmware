#ifndef CYCLONE2_RUNTIME_H
#define CYCLONE2_RUNTIME_H

#include "generic/typedef.h"

struct cyclone2_report_statistics {
    u32 poll_count;
    u32 send_attempts;
    u32 send_accepted;
    u32 send_busy;
};

u8 cyclone2_report_period_get(void);
int cyclone2_report_period_set(u8 period_ms);
void cyclone2_report_statistics_get(struct cyclone2_report_statistics *stats);

#endif
