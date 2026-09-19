#ifndef CYCLONE2_RUNTIME_H
#define CYCLONE2_RUNTIME_H

#include "generic/typedef.h"

struct cyclone2_report_statistics {
    u32 poll_count;
    u32 send_attempts;
    u32 send_accepted;
    u32 send_busy;
};

struct cyclone2_connection_status {
    u8 slot;
    u8 profile_type;
    u8 connection_state;
    u8 usb_present;
    u8 local_address[6];
};

struct cyclone2_injection_status {
    u8 active;
    u8 synthetic_a;
    u16 reserved;
    u32 period_ms;
    u32 duration_ms;
    u32 emitted_edges;
    u32 overruns;
};

u8 cyclone2_report_period_get(void);
int cyclone2_report_period_set(u8 period_ms);
void cyclone2_report_statistics_get(struct cyclone2_report_statistics *stats);
int cyclone2_connect_request(u8 slot);
void cyclone2_connection_status_get(struct cyclone2_connection_status *status);
int cyclone2_usb_present(void);
int cyclone2_inject_start(u32 period_ms, u32 duration_ms);
void cyclone2_inject_stop(void);
void cyclone2_injection_status_get(struct cyclone2_injection_status *status);

#endif
