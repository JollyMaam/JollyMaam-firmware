#ifndef _APP_CHARGESTORE_H_
#define _APP_CHARGESTORE_H_

#include "typedef.h"
#include "system/event.h"

enum {
    TWS_CHANNEL_LEFT = 1, // TRANSLATED: Left earbud.
    TWS_CHANNEL_RIGHT, // TRANSLATED: Right earbud.
};

enum {
    TWS_DEL_TWS_ADDR = 1, // TRANSLATED: Delete the peer earbud address.
    TWS_DEL_PHONE_ADDR,// TRANSLATED: Delete the phone address.
    TWS_DEL_ALL_ADDR,// TRANSLATED: Delete both phone and peer earbud addresses.
};

struct _CHARGE_STORE_INFO {
    u8 tws_local_addr[6];
    u8 tws_remote_addr[6];
    u8 tws_mac_addr[6];
    u32 search_aa;
    u32 pair_aa;
} _GNU_PACKED_;
typedef struct _CHARGE_STORE_INFO   CHARGE_STORE_INFO;

extern u8 chargestore_get_power_level(void);
extern u8 chargestore_get_power_status(void);
extern u8 chargestore_get_cover_status(void);
extern u8 chargestore_get_sibling_power_level(void);
extern void chargestore_set_bt_init_ok(u8 flag);
extern u8 chargestore_get_testbox_status(void);
extern void chargestore_clear_testbox_status(void);
extern int app_chargestore_event_handler(struct chargestore_event *chargestore_dev);
extern u8 chargestore_get_connect_status(void);
extern void chargestore_clear_connect_status(void);
extern u8 chargestore_get_earphone_online(void);
extern u8 chargestore_get_earphone_pos(void);
extern int chargestore_sync_chg_level(void);
extern void chargestore_set_power_level(u8 power);
extern void chargestore_set_sibling_chg_lev(u8 chg_lev);
extern void chargestore_set_phone_disconnect(void);
extern void chargestore_set_phone_connect(void);
extern u8 chargestore_check_going_to_poweroff(void);
extern void chargestore_shutdown_reset(void);

#endif    //_APP_CHARGESTORE_H_
