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

#define CYCLONE2_BRINGUP_LOG_ENTRIES 124
extern volatile u32 cyclone2_bringup_log[CYCLONE2_BRINGUP_LOG_ENTRIES];
extern volatile u32 cyclone2_bringup_log_head;

/* Append one milestone to the reset-surviving bring-up log. */
void cyclone2_bringup_record(u32 code);
#endif
