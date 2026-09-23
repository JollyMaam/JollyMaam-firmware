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

/* Last Bluetooth bring-up milestone.  Lives in the same preserved SRAM
 * section as the boot request so it survives the reset that follows a
 * controller crash, and is printed by the console `status` command. */
extern volatile u32 cyclone2_bringup_trace;
extern volatile u32 cyclone2_boot_count;

#define CYCLONE2_BRINGUP_LOG_ENTRIES 900
extern volatile u32 cyclone2_bringup_log[CYCLONE2_BRINGUP_LOG_ENTRIES];
extern volatile u32 cyclone2_bringup_log_head;

/* Append one milestone to the reset-surviving bring-up log. */
void cyclone2_bringup_record(u32 code);

/* Reset cause, sampled once at the top of app_main - as early as the
 * application can look, since the bootloader may already have consumed the
 * register.  Printed by the console `status` command; see
 * docs/reboot-hunting.md. */
extern u8 cyclone2_reset_source;  /* power_reset_source_dump() */
extern u8 cyclone2_reset_p33;     /* P3_RST_SRC, raw */
extern u8 cyclone2_reset_rtc;     /* R3_RST_SRC, raw */
#endif
