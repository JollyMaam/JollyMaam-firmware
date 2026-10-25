#ifndef TELINK_SDK_H
#define TELINK_SDK_H

/* The Telink SDK's headers, for the app's files that call the SDK (the
 * platform and the board).  The SDK builds with -fpack-struct and the app
 * does not, so its structures are packed here the same way.  Its types
 * (u8 ... u64, size_t) stand in for pad.h's, which then come after it. */

#pragma pack(push, 1)
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"
#pragma pack(pop)

#define PAD_SDK_TYPES
#include "pad.h"

#endif
