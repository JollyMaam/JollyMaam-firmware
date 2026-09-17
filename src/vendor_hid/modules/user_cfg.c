#include "app_config.h"
#include "user_cfg.h"
#include "fs.h"
#include "string.h"
#include "system/includes.h"
#include "vm.h"
#include "btcontroller_config.h"
#include "app_main.h"
#include "app_power_manage.h"
#include "bt_common.h"

#include "audio_config.h"

#define LOG_TAG_CONST       USER_CFG
#define LOG_TAG             "[USER_CFG]"
#define LOG_ERROR_ENABLE
#define LOG_DEBUG_ENABLE
#define LOG_INFO_ENABLE
#define LOG_DUMP_ENABLE
#define LOG_CLI_ENABLE
#include "debug.h"

/* TRANSLATED: Bluetooth TX power level; the SDK default is approximately 0 dBm. */
#define  SET_BLE_TX_POWER_LEVEL        (6)

void lp_winsize_init(struct lp_ws_t *lp);
/* void bt_max_pwr_set(u8 pwr, u8 pg_pwr, u8 iq_pwr, u8 ble_pwr); */

extern APP_VAR app_var;

BT_CONFIG bt_cfg = {
    .edr_name        = "JL_HID_DEBUG",
    .mac_addr        = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
    .tws_local_addr  = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
    .rf_power        = 10,
    .dac_analog_gain = 25,
    .mic_analog_gain = 7,
    .tws_device_indicate = 0x6688,
};

u8 get_max_sys_vol(void)
{
    //return (audio_cfg.max_sys_vol);
    return 15;
}

u8 get_tone_vol(void)
{
    return 15;
}

//======================================================================================//
//                                 		TRANSLATED: BTIF configuration-item table.
//	TRANSLATED: Parameter 1: configuration-item ID.
//	TRANSLATED: Parameter 2: storage size in bytes.
//	TRANSLATED: IDs registered here are read/written in the BTIF region;
//		  TRANSLATED: unregistered IDs use the VM region by default.
//======================================================================================//
const struct btif_item btif_table[] = {
// 	 	item id 		   	   len   	//
    {CFG_BT_MAC_ADDR, 			6 },
    {CFG_BT_FRE_OFFSET,   		6 },   // TRANSLATED: Frequency-offset calibration from the test box.
    //{CFG_DAC_DTB,   			2 },
    //{CFG_MC_BIAS,   			1 },
    {0, 						0 },   //reserved cfg
};

// TRANSLATED: Maximum VM region size.
const int vm_max_size_config = VM_MAX_SIZE_CONFIG; // TRANSLATED: Configured by the macro in app_cfg.
//======================================================================================//

struct lp_ws_t lp_winsize = {
    .lrc_ws_inc = 480,      //260
    .lrc_ws_init = 160,
    .bt_osc_ws_inc = 100,
    .bt_osc_ws_init = 140,
    .osc_change_mode = 0,
};

u16 bt_get_tws_device_indicate(u8 *tws_device_indicate)
{
    return bt_cfg.tws_device_indicate;
}

const u8 *bt_get_mac_addr()
{
    return bt_cfg.mac_addr;
}

void bt_set_mac_addr(u8 *addr)
{
    memcpy(bt_cfg.mac_addr, addr, 6);
}

static u8 bt_mac_addr_for_testbox[6] = {0};
void bt_get_vm_mac_addr(u8 *addr)
{

    memcpy(addr, bt_mac_addr_for_testbox, 6);
}

void bt_get_tws_local_addr(u8 *addr)
{
    memcpy(addr, bt_cfg.tws_local_addr, 6);
}

const char *bt_get_local_name()
{
    return (const char *)(bt_cfg.edr_name);
}

void bt_set_local_name(char *name, u8 len)
{
    memcpy(bt_cfg.edr_name, name, len);
    bt_cfg.edr_name[len] = 0;
}

const char *bt_get_pin_code()
{
    return "0000";
}

void cfg_file_parse(u8 idx)
{
    u8 tmp[128] = {0};
    int ret = 0;

    memset(tmp, 0x00, sizeof(tmp));

    /*************************************************************************/
    /*                      CFG READ IN cfg_tools.bin                        */
    /*************************************************************************/
    //-----------------------------CFG_BT_NAME--------------------------------------//
    ret = syscfg_read(CFG_BT_NAME, tmp, 32);
    if (ret < 0) {
        log_info("read bt name err\n");
    } else if (ret >= LOCAL_NAME_LEN) {
        memset(bt_cfg.edr_name, 0x00, LOCAL_NAME_LEN);
        memcpy(bt_cfg.edr_name, tmp, LOCAL_NAME_LEN);
        bt_cfg.edr_name[LOCAL_NAME_LEN - 1] = 0;
    } else {
        memset(bt_cfg.edr_name, 0x00, LOCAL_NAME_LEN);
        memcpy(bt_cfg.edr_name, tmp, ret);
    }
    /* g_printf("bt name config:%s\n", bt_cfg.edr_name); */
    log_info("bt name config:%s\n", bt_cfg.edr_name);

    //-----------------------------CFG_BT_RF_POWER_ID----------------------------//
    ret = syscfg_read(CFG_BT_RF_POWER_ID, &app_var.rf_power, 1);
    if (ret < 0) {
        log_debug("read rf err\n");
        app_var.rf_power = 10;
    }

    bt_max_pwr_set(app_var.rf_power, 5, 8, SET_BLE_TX_POWER_LEVEL);
    /* g_printf("rf config:%d\n", app_var.rf_power); */
    log_info("rf config:%d,%d\n", app_var.rf_power, SET_BLE_TX_POWER_LEVEL);

    app_var.music_volume = 14;
    app_var.wtone_volume = 14;

    app_var.warning_tone_v = LOW_POWER_WARN_VAL;
    app_var.poweroff_tone_v = LOW_POWER_OFF_VAL;
    log_info("warning_tone_v:%d poweroff_tone_v:%d\n", app_var.warning_tone_v, app_var.poweroff_tone_v);
    app_var.auto_off_time =  TCFG_AUTO_SHUT_DOWN_TIME;
    log_info("auto_off_time:%d\n", app_var.auto_off_time);

    /*************************************************************************/
    /*                      CFG READ IN VM                                   */
    /*************************************************************************/
    u8 mac_buf[6];
    u8 mac_buf_tmp[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    u8 mac_buf_tmp2[6] = {0, 0, 0, 0, 0, 0};
        do {
            ret = syscfg_read(CFG_BT_MAC_ADDR, mac_buf, 6);
            if ((ret != 6) || !memcmp(mac_buf, mac_buf_tmp, 6) || !memcmp(mac_buf, mac_buf_tmp2, 6)) {
                get_random_number(mac_buf, 6);
                syscfg_write(CFG_BT_MAC_ADDR, mac_buf, 6);
            }
        } while (0);

    syscfg_read(CFG_BT_MAC_ADDR, bt_mac_addr_for_testbox, 6);
    if (!memcmp(bt_mac_addr_for_testbox, mac_buf_tmp, 6)) {
        get_random_number(bt_mac_addr_for_testbox, 6);
        syscfg_write(CFG_BT_MAC_ADDR, bt_mac_addr_for_testbox, 6);
        log_info(">>>init mac addr!!!\n");
    }

    log_info("mac:");
    log_info_hexdump(mac_buf, sizeof(mac_buf));
    memcpy(bt_cfg.mac_addr, mac_buf, 6);

    /*************************************************************************/
    /*                      CFG READ IN isd_config.ini                       */
    /*************************************************************************/
    LRC_CONFIG lrc_cfg;
    ret = syscfg_read(CFG_LRC_ID, &lrc_cfg, sizeof(LRC_CONFIG));
    if (ret > 0) {
        log_info("lrc cfg:");
        log_info_hexdump(&lrc_cfg, sizeof(LRC_CONFIG));
        lp_winsize.lrc_ws_inc      = lrc_cfg.lrc_ws_inc;
        lp_winsize.lrc_ws_init     = lrc_cfg.lrc_ws_init;
        lp_winsize.bt_osc_ws_inc   = lrc_cfg.btosc_ws_inc;
        lp_winsize.bt_osc_ws_init  = lrc_cfg.btosc_ws_init;
        lp_winsize.osc_change_mode = lrc_cfg.lrc_change_mode;
    }
    /* printf("%d %d %d \n",lp_winsize.lrc_ws_inc,lp_winsize.lrc_ws_init,lp_winsize.osc_change_mode); */
    lp_winsize_init(&lp_winsize);
}
