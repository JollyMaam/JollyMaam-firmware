#ifndef CYCLONE2_RUNTIME_H
#define CYCLONE2_RUNTIME_H

#include "generic/typedef.h"

struct cyclone2_report_statistics {
    u32 poll_count;
    u32 send_attempts;
    u32 send_accepted;
    u32 send_busy;
    u32 send_deferred;
    u32 send_errors;
    /* Reports per second the slot is set to send, and what it sent in the
     * last whole second. */
    u32 target_hz;
    u32 hz;
};

struct cyclone2_connection_status {
    u8 slot;
    u8 profile_type;
    u8 connection_state;
    u8 usb_present;
    u8 connect_attempts;
    u8 local_address[6];
    /* The running slot's type or address differs from its settings: the
     * change waits for the next connect. */
    u8 stale;
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

u8 get_cur_bt_idx(void);

int cyclone2_radio_quiet(void);
void cyclone2_report_statistics_get(struct cyclone2_report_statistics *stats);
int cyclone2_connect_request(u8 slot);

int cyclone2_wake_request(u8 slot, u8 channel, u8 seconds);

void cyclone2_wake_cancel(void);
int cyclone2_pair_request(u8 slot);
int cyclone2_pair_fresh_request(u8 slot);
int cyclone2_clear_keys_request(void);
int cyclone2_reboot_request(void);
int cyclone2_disconnect_request(void);
/* Power off, or on USB power only disconnect. */
void cyclone2_sleep_request(void);
/* The slot last started since power was lost, or CYCLONE2_UX_NO_SLOT. */
u8 cyclone2_last_slot(void);
int cyclone2_radio_stopped(void);
void cyclone2_connection_status_get(struct cyclone2_connection_status *status);
int cyclone2_usb_present(void);
int cyclone2_inject_start(u32 period_ms, u32 duration_ms);
void cyclone2_inject_stop(void);
void cyclone2_injection_status_get(struct cyclone2_injection_status *status);

#endif
