#ifndef CYCLONE2_MAIN_H
#define CYCLONE2_MAIN_H

#include "typedef.h"

#define ACTION_CYCLONE2             0x000b

#define CYCLONE2_BOOT_REQUEST_MAGIC  0x43324254U
struct cyclone2_boot_request {
    u32 magic;
    u8 profile;
    u8 pairing;
    u16 check;
};

extern volatile struct cyclone2_boot_request reset_mailbox;

/* The slot last started, for Home to connect again.  Beside the mailbox, so
 * it survives a soft reset; never written to flash. */
#define CYCLONE2_LAST_SLOT_MAGIC  0x43324c53U
struct cyclone2_last_slot {
    u32 magic;
    u8 slot;
    u8 check;
    u16 reserved;
};

extern volatile struct cyclone2_last_slot last_slot_record;

/* Last Bluetooth bring-up milestone.  Lives in the same preserved SRAM
 * section as the boot request so it survives the reset that follows a
 * controller crash, and is printed by the console `status` command. */
extern volatile u32 cyclone2_bringup_trace;
extern volatile u32 cyclone2_boot_count;

/* Sized to hold a whole console session: at the steady-state report rate the
 * old 900 entries covered barely a minute, which is exactly when the failures
 * happen, so the evidence was overwritten by the time a reset could be read
 * back.  The section sits in the gap above the heap; see app.ld. */
#define CYCLONE2_BRINGUP_LOG_ENTRIES 2000
extern volatile u32 cyclone2_bringup_log[CYCLONE2_BRINGUP_LOG_ENTRIES];
extern volatile u32 cyclone2_bringup_log_head;

/* Append one milestone to the reset-surviving bring-up log. */
void cyclone2_bringup_record(u32 code);

/* Reset cause, sampled once at the top of app_main - as early as the
 * application can look, since the bootloader may already have consumed the
 * register.  Printed by the console `status` command; see
 * docs/power-reset.md. */
extern u8 cyclone2_reset_source;  /* power_reset_source_dump() */
extern u8 cyclone2_reset_p33;
extern u8 cyclone2_reset_rtc;
#endif
