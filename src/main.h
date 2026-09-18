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
#endif
