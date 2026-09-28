#include "app_config.h"
#include "system/includes.h"
#include "update_loader_download.h"

const int config_update_mode = UPDATE_BT_LMP_EN | UPDATE_BLE_TEST_EN | UPDATE_APP_EN | UPDATE_UART_EN;

const int support_dual_bank_update_en = 0;

const int support_norflash_update_en  = 0;

const int support_norflash_ufw_update_en = 0;

#if OTA_TWS_SAME_TIME_NEW
const int support_ota_tws_same_time_new =  1;
#else
const int support_ota_tws_same_time_new =  0;
#endif

const int support_vm_data_keep = 1;

const char log_tag_const_v_UPDATE AT(.LOG_TAG_CONST) = LIB_DEBUG &  FALSE;
const char log_tag_const_i_UPDATE AT(.LOG_TAG_CONST) = LIB_DEBUG &  FALSE;
const char log_tag_const_d_UPDATE AT(.LOG_TAG_CONST) = LIB_DEBUG &  FALSE;
const char log_tag_const_w_UPDATE AT(.LOG_TAG_CONST) = LIB_DEBUG &  TRUE;
const char log_tag_const_e_UPDATE AT(.LOG_TAG_CONST) = LIB_DEBUG &  TRUE;
