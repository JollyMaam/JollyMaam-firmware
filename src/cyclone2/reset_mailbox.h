#ifndef CYCLONE2_RESET_MAILBOX_H
#define CYCLONE2_RESET_MAILBOX_H

#include "typedef.h"

#define CYCLONE2_BOOT_REQUEST_MAGIC  0x43324254U
struct cyclone2_boot_request {
    u32 magic;
    u8 profile;
    u8 pairing;
    u16 check;
};

extern volatile struct cyclone2_boot_request cyclone2_boot_request;

#endif
