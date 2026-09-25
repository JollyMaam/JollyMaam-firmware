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
};

struct cyclone2_connection_status {
    u8 slot;
    u8 profile_type;
    u8 connection_state;
    u8 usb_present;
    u8 connect_attempts;
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
/* Wake the console this slot last connected to, then reconnect it.  Returns 1
 * when the sequence is on its way, -1 while a transition or another wake is
 * already running, -2 for out-of-range arguments, -3 when the slot has no
 * peer address to aim at, and -4 before the controller library has ever been
 * started (the wake takes the baseband scheduler, so it needs the controller
 * that owns it to exist). */
int cyclone2_wake_request(u8 slot, u8 channel, u8 seconds);
/* The wake session calls this when it finishes; declared here because the
 * controller owns the state machine it returns the radio to. */
void cyclone2_wake_finished(void);
int cyclone2_pair_request(u8 slot);
int cyclone2_pair_fresh_request(u8 slot);
int cyclone2_new_address_request(void);
int cyclone2_set_address_request(const u8 address[6]);
int cyclone2_connect_with_address_request(u8 slot, const u8 address[6]);
int cyclone2_forget_peer_request(const u8 address[6]);
int cyclone2_set_peer_request(u8 slot, const u8 address[6]);
int cyclone2_clear_keys_request(void);
int cyclone2_reboot_request(void);
int cyclone2_disconnect_request(void);
void cyclone2_connection_status_get(struct cyclone2_connection_status *status);
int cyclone2_usb_present(void);
int cyclone2_inject_start(u32 period_ms, u32 duration_ms);
void cyclone2_inject_stop(void);
void cyclone2_injection_status_get(struct cyclone2_injection_status *status);

#endif
