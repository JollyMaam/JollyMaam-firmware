/*********************************************************************************************
    *   Filename        : ble_hogp.c

    *   Description     :

    *   Author          :

    *   Email           : zh-jieli.com

    *   Last modifiled  : 2020-09-01 11:14

    *   Copyright:(c)JIELI  2011-2020  @ , All Rights Reserved.
*********************************************************************************************/

// *****************************************************************************
/* EXAMPLE_START
 *
 * @text All newer operating systems provide GATT Client functionality.
 * The LE Counter examples demonstrates how to specify a minimal GATT Database
 * with a custom GATT Service and a custom Characteristic that sends periodic
 * notifications.
 */
// *****************************************************************************
#include "system/app_core.h"
#include "system/includes.h"

#include "app_config.h"
#include "app_action.h"

#include "btstack/btstack_task.h"
#include "btstack/bluetooth.h"
#include "user_cfg.h"
#include "vm.h"
#include "btcontroller_modules.h"
#include "bt_common.h"

#include "le_common.h"
#include "standard_hid.h"

#include "rcsp_bluetooth.h"
#include "JL_rcsp_api.h"
#include "update_loader_download.h"
#include "custom_cfg.h"
#include "ble_hogp_profile.h"
#include "gatt_common/le_gatt_common.h"
#include "app_comm_bt.h"

/* #if RCSP_BTMATE_EN */
/* #include "btstack/JL_rcsp_api.h" */
/* #include "rcsp_bluetooth.h" */
/* #endif */

#if LE_DEBUG_PRINT_EN
//#define log_info            y_printf
#define log_info(x, ...)  printf("[BLE_HOGP]" x " ", ## __VA_ARGS__)
#define log_info_hexdump  put_buf

#else
#define log_info(...)
#define log_info_hexdump(...)
#endif

static u8 remote_ble_addr[7] = {0};
static void hid_timer_mouse_handler(void);
extern get_cur_bt_idx();
extern u8 get_key_timeout_flag();

#define EIR_TAG_STRING   0xd6, 0x05, 0x08, 0x00, 'J', 'L', 'A', 'I', 'S', 'D','K'
static const char user_tag_string[] = {EIR_TAG_STRING};
// TRANSLATED: User-configurable pairing key for the prototype and customer's app.
//const u8 link_key_data[16] = {0x06, 0x77, 0x5f, 0x87, 0x91, 0x8d, 0xd4, 0x23, 0x00, 0x5d, 0xf1, 0xd8, 0xcf, 0x0c, 0x14, 0x2b};

#define ATT_LOCAL_MTU_SIZE        (64)

// TRANSLATED: Number of packets the ATT buffer can hold.
#define ATT_PACKET_NUMS_MAX       (2)

// TRANSLATED: ATT buffer size; configurable, but at least 23 bytes.
#define ATT_SEND_CBUF_SIZE        (ATT_PACKET_NUMS_MAX * (ATT_PACKET_HEAD_SIZE + ATT_LOCAL_MTU_SIZE))

static volatile hci_con_handle_t hogp_con_handle;
int ble_hid_timer_handle = 0;

// TRANSLATED: Request connection-parameter updates: 0=disabled, 1=enabled.
static uint8_t hogp_connection_update_enable = 1;

// TRANSLATED: Request connection parameters in table order; stop when the host accepts.
static const struct conn_update_param_t Peripheral_Preferred_Connection_Parameters[] = {
    /* Prefer one 7.5-ms event per report opportunity. Hosts may reject the
     * exact interval, so retain progressively looser zero-latency fallbacks. */
    {6,  6, 0, 600},
    {6,  9, 0, 600},
    {12, 12, 0, 600},
};
// TRANSLATED: Number of available parameter groups.
#define CONN_PARAM_TABLE_CNT      (sizeof(Peripheral_Preferred_Connection_Parameters)/sizeof(struct conn_update_param_t))

static u8 hogp_adv_data[ADV_RSP_PACKET_MAX];//max is 31
static u8 hogp_scan_rsp_data[ADV_RSP_PACKET_MAX];//max is 31

static u8 first_pair_flag;     // TRANSLATED: First-pairing flag.
static u8 is_hogp_active = 0;  // TRANSLATED: Prevents entering sleep.
static adv_cfg_t hogp_server_adv_config;

/*--------------------------------------------*/
// TRANSLATED: Normal disconnected advertising interval, in 0.625 ms units.
#define ADV_INTERVAL_MIN                (160 * 5)

/*------------------------------
TRANSLATED: With a bond, first advertise for fast reconnect, then normally; without a bond, advertise normally only.
TRANSLATED: Directed advertising includes only the peer address, without advertising or scan-response data.
*/
#define PAIR_RECONNECT_ADV_EN               1 /* TRANSLATED: Use directed advertising at startup to prioritize reconnecting to a bonded peer. */

// TRANSLATED: Two directed-advertising types are available:
// TRANSLATED: ADV_DIRECT_IND: 1.28 s timeout, fixed 2 ms interval.
// TRANSLATED: ADV_DIRECT_IND_LOW: interval and channels match normal advertising.
// TRANSLATED: ADV_IND: undirected, with normal advertising interval and channels.
static u8 pair_reconnect_adv_type = ADV_DIRECT_IND;

// TRANSLATED: Advertising interval while waiting to reconnect, in 0.625 ms units.
#define PAIR_RECONNECT_ADV_INTERVAL     (32) /*>=32,  ADV_IND,ADV_DIRECT_IND_LOW,interval*/
static  u32 pair_reconnect_adv_timeout = 5000;/* TRANSLATED: Reconnect advertising duration, in milliseconds. */

// TRANSLATED: Advertising channels.
static  u8 le_adv_channel_sel = ADV_CHANNEL_ALL;

//--------------------------------------------
#define BLE_VM_HEAD_TAG           (0xB35C)
#define BLE_VM_TAIL_TAG           (0x5CB3)
struct pair_info_t {
    u16 head_tag;             // TRANSLATED: Header marker.
    u8  pair_flag: 2;         // TRANSLATED: Whether pairing information is valid.
    u8  direct_adv_cnt: 6;    // TRANSLATED: Directed-advertising attempt count.
    u8 direct_adv_flag: 1;
    u8  peer_address_info[7]; // TRANSLATED: Bonded peer address information.
    u16 tail_tag;// TRANSLATED: Trailer marker.
};

// TRANSLATED: Pairing information table.
static struct pair_info_t  hogp_pair_info;
static u8 cur_peer_addr_info[7];// TRANSLATED: Current peer address information.

#define REPEAT_DIRECT_ADV_COUNT  (2)// *1.28s

// TRANSLATED: Some procedures bypass peripheral latency to respond promptly to phone packets.
// TRANSLATED: Number of intervals for which latency is bypassed.
#define LATENCY_SKIP_INTERVAL_MIN     (3)
#define LATENCY_SKIP_INTERVAL_KEEP    (20)
#define LATENCY_SKIP_INTERVAL_LONG    (0xffff)

//hid device infomation
static u8 Appearance[] = {BLE_APPEARANCE_GENERIC_HID & 0xff, BLE_APPEARANCE_GENERIC_HID >> 8}; //
static const char Manufacturer_Name_String[] = "zhuhai_jieli";
static const char Model_Number_String[] = "hid_mouse";
static const char Serial_Number_String[] = "000000";
static const char Hardware_Revision_String[] = "0.0.1";
static const char Firmware_Revision_String[] = "0.0.1";
static const char Software_Revision_String[] = "0.0.1";
static const u8 System_ID[] = {0, 0, 0, 0, 0, 0, 0, 0};

// TRANSLATED: Product information for testing.
#define  PNP_VID_SOURCE   0x02
#define  PNP_VID          0x05ac //0x05d6
#define  PNP_PID          0x022C //
#define  PNP_PID_VERSION  0x011b //1.1.11

static u8 PnP_ID[] = {PNP_VID_SOURCE, PNP_VID & 0xFF, PNP_VID >> 8, PNP_PID & 0xFF, PNP_PID >> 8, PNP_PID_VERSION & 0xFF, PNP_PID_VERSION >> 8};
/* static const u8 PnP_ID[] = {0x02, 0x17, 0x27, 0x40, 0x00, 0x23, 0x00}; */
/* static const u8 PnP_ID[] = {0x02, 0xac, 0x05, 0x2c, 0x02, 0x1b, 0x01}; */

/* static const u8 hid_information[] = {0x11, 0x01, 0x00, 0x01}; */
/* static const u8 hid_information[] = {0x01, 0x01, 0x00, 0x03}; */
static const u8 hid_information[] = {0x11, 0x01, 0x00, 0x03};
static const u8 hid_information1[] = {0x00, 0x00, 0x00, 0x00};

static  u8 *report_map; // TRANSLATED: Report descriptor.
static  u16 report_map_size;// TRANSLATED: Report descriptor size.
static u16 hogp_adv_timeout_number = 0;
#define HID_REPORT_MAP_DATA    report_map
#define HID_REPORT_MAP_SIZE    report_map_size

// TRANSLATED: When the report changes, send Service Changed so the host fetches it again.
static u8 hid_report_change = 0;
static u8 hid_battery_level = 88;
static u32 hid_battery_level_add_sum;/* TRANSLATED: Sum of battery samples. */
static u32 hid_battery_level_add_cnt;/* TRANSLATED: Battery sample count. */

#define HID_BATTERY_TIMER_SET     (60000) /* TRANSLATED: Battery-change check period, typically 30-60 seconds. */
static u16 hid_battery_notify_timer_id;

static u8(*get_vbat_percent_call)(void) = NULL;
static void (*le_hogp_output_callback)(u8 *buffer, u16 size) = NULL;
//------------------------------------------------------
static void __hogp_resume_all_ccc_enable(u8 update_request);
static void __check_report_map_change(void);
void ble_hid_transfer_channel_recieve(u8 *packet, u16 size);
void ble_hid_transfer_channel_recieve1(u8 *packet, u16 size);
static uint16_t hogp_att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset, uint8_t *buffer, uint16_t buffer_size);
static int hogp_att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size);
static int hogp_event_packet_handler(int event, u8 *packet, u16 size, u8 *ext_param);
static void hogp_adv_config_set(void);
static u8 hid_get_vbat_handle(void);
//------------------------------------------------------

static u8 multi_dev_adv_flag = 0; /* TRANSLATED: Switch advertising between multiple device addresses. */
void set_multi_devices_adv_flag(u8 adv_flag)
{
    multi_dev_adv_flag = adv_flag;
}

// TRANSLATED: Passkey-entry encryption.
#define PASSKEY_ENABLE                     0

static const sm_cfg_t hogp_sm_init_config = {
    .slave_security_auto_req = 0,
    .slave_set_wait_security = 0,

#if PASSKEY_ENABLE
    .io_capabilities = IO_CAPABILITY_DISPLAY_ONLY,
#else
    .io_capabilities = IO_CAPABILITY_NO_INPUT_NO_OUTPUT,
#endif

    .authentication_req_flags = SM_AUTHREQ_BONDING | SM_AUTHREQ_MITM_PROTECTION,
    .min_key_size = 7,
    .max_key_size = 16,
    .sm_cb_packet_handler = NULL,
};

const gatt_server_cfg_t hogp_server_init_cfg = {
    .att_read_cb = &hogp_att_read_callback,
    .att_write_cb = &hogp_att_write_callback,
    .event_packet_handler = &hogp_event_packet_handler,
};

static gatt_ctrl_t hogp_gatt_control_block = {
    //public
    .mtu_size = ATT_LOCAL_MTU_SIZE,
    .cbuffer_size = ATT_SEND_CBUF_SIZE,
    .multi_dev_flag	= 0,

    //config
    .server_config = &hogp_server_init_cfg,

    .client_config = NULL,

    .sm_config = &hogp_sm_init_config,
    //cbk,event handle
    .hci_cb_packet_handler = NULL,
};

/*************************************************************************************************/
/*!
 *  TRANSLATED: Post a message to the app.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static void __ble_bt_evnet_post(u32 arg_type, u8 priv_event, u8 *args, u32 value)
{
    struct sys_event e;
    e.type = SYS_BT_EVENT;
    e.arg  = (void *)arg_type;
    e.u.bt.event = priv_event;
    if (args) {
        memcpy(e.u.bt.args, args, 7);
    }
    e.u.bt.value = value;
    sys_event_notify(&e);
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Publish BLE status.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static void __ble_state_to_user(u8 state, u8 reason)
{
    static u8 ble_state = 0xff;
    if (state != ble_state) {
        log_info("ble_state:%02x===>%02x\n", ble_state, state);
        ble_state = state;
        __ble_bt_evnet_post(SYS_BT_EVENT_BLE_STATUS, state, NULL, reason);
    }
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Request a connection-parameter update.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static void __hogp_send_connetion_update_deal(u16 conn_handle)
{
    if (hogp_connection_update_enable) {
        if (0 == ble_gatt_server_connetion_update_request(conn_handle, Peripheral_Preferred_Connection_Parameters, CONN_PARAM_TABLE_CNT)) {
            hogp_connection_update_enable = 0;
        }
    }
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Read/write bonded-peer information in VM.
 *
 *  \param      [in] rw_flag： 0--read， 1--write
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static void __hogp_conn_pair_vm_do(struct pair_info_t *info, u8 rw_flag)
{
    int ret;
    int vm_len = sizeof(struct pair_info_t);

    log_info("-hogp_pair_info vm_do:%d\n", rw_flag);
    if (rw_flag == 0) {
        if (multi_dev_adv_flag) {
            ret = syscfg_read(CFG_BLE_BONDING_REMOTE_INFO + get_cur_bt_idx(), (u8 *)info, vm_len);
        } else {
            ret = syscfg_read(CFG_BLE_BONDING_REMOTE_INFO, (u8 *)info, vm_len);
        }
        if (vm_len != ret) {
            log_info("-null--\n");
            memset(info, 0, vm_len);
        }

        if ((BLE_VM_HEAD_TAG == info->head_tag) && (BLE_VM_TAIL_TAG == info->tail_tag)) {
            log_info("-exist--\n");
        } else {
            log_info("-reset vm--\n");
            memset(info, 0, vm_len);
            info->head_tag = BLE_VM_HEAD_TAG;
            info->tail_tag = BLE_VM_TAIL_TAG;
        }
    } else {
        if (multi_dev_adv_flag) {
            syscfg_write(CFG_BLE_BONDING_REMOTE_INFO + get_cur_bt_idx(), (u8 *)info, vm_len);
        } else {
            syscfg_write(CFG_BLE_BONDING_REMOTE_INFO, (u8 *)info, vm_len);
        }
    }
    log_info_hexdump(info, vm_len);
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Configure the HID descriptor.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static const u16 change_handle_table[2] = {ATT_CHARACTERISTIC_2a4b_01_VALUE_HANDLE, ATT_CHARACTERISTIC_2a4b_01_VALUE_HANDLE};
static void __check_report_map_change(void)
{
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Proactively enable notifications when reconnecting.

 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static void __hogp_resume_all_ccc_enable(u8 update_request)
{
    log_info("__hogp_resume_all_ccc_enable\n");
    ble_gatt_server_characteristic_ccc_set(hogp_con_handle, ATT_CHARACTERISTIC_2a4d_01_CLIENT_CONFIGURATION_HANDLE, ATT_OP_NOTIFY);
    ble_gatt_server_characteristic_ccc_set(hogp_con_handle, ATT_CHARACTERISTIC_2a4d_02_CLIENT_CONFIGURATION_HANDLE, ATT_OP_NOTIFY);
    ble_gatt_server_characteristic_ccc_set(hogp_con_handle, ATT_CHARACTERISTIC_2a4d_04_CLIENT_CONFIGURATION_HANDLE, ATT_OP_NOTIFY);
    ble_gatt_server_characteristic_ccc_set(hogp_con_handle, ATT_CHARACTERISTIC_2a4d_05_CLIENT_CONFIGURATION_HANDLE, ATT_OP_NOTIFY);
    ble_gatt_server_characteristic_ccc_set(hogp_con_handle, ATT_CHARACTERISTIC_2a4d_06_CLIENT_CONFIGURATION_HANDLE, ATT_OP_NOTIFY);
    ble_gatt_server_characteristic_ccc_set(hogp_con_handle, ATT_CHARACTERISTIC_2a4d_07_CLIENT_CONFIGURATION_HANDLE, ATT_OP_NOTIFY);
    ble_gatt_server_characteristic_ccc_set(hogp_con_handle, ATT_CHARACTERISTIC_ae42_01_CLIENT_CONFIGURATION_HANDLE, ATT_OP_NOTIFY);
    ble_gatt_server_characteristic_ccc_set(hogp_con_handle, ATT_CHARACTERISTIC_2a19_01_CLIENT_CONFIGURATION_HANDLE, ATT_OP_NOTIFY);

    if (update_request) {
        __hogp_send_connetion_update_deal(hogp_con_handle);
    }
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Report the detected peer operating system.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  TRANSLATED: Reference implementation for identifying the phone OS.
 */
/*************************************************************************************************/
static void __att_check_remote_result(u16 con_handle, remote_type_e remote_type)
{
    log_info("le_hogp %02x:remote_type= %02x\n", con_handle, remote_type);
    __ble_bt_evnet_post(SYS_BT_EVENT_FORM_COMMON, COMMON_EVENT_BLE_REMOTE_TYPE, NULL, remote_type);
    //to do
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Handle common GATT events, including HCI and GATT.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static int hogp_event_packet_handler(int event, u8 *packet, u16 size, u8 *ext_param)
{
    /* log_info("event: %02x,size= %d\n",event,size); */

    switch (event) {

    case GATT_COMM_EVENT_CAN_SEND_NOW:
        break;

    case GATT_COMM_EVENT_SERVER_INDICATION_COMPLETE:
        log_info("INDICATION_COMPLETE:con_handle= %04x,att_handle= %04x\n", \
                 little_endian_read_16(packet, 0), little_endian_read_16(packet, 2));
        break;

    case GATT_COMM_EVENT_CONNECTION_COMPLETE:
        hogp_con_handle = little_endian_read_16(packet, 0);
        first_pair_flag = 0;
        memcpy(cur_peer_addr_info, ext_param + 7, 7);

        log_info("connection_handle:%04x, rssi= %d\n", hogp_con_handle, ble_vendor_get_peer_rssi(hogp_con_handle));
        log_info("peer_address_info:");
        put_buf(cur_peer_addr_info, 7);
        log_info("con_interval = %d\n", little_endian_read_16(ext_param, 14 + 0));
        log_info("con_latency = %d\n", little_endian_read_16(ext_param, 14 + 2));
        log_info("cnn_timeout = %d\n", little_endian_read_16(ext_param, 14 + 4));

        if (little_endian_read_16(ext_param, 14 + 2)) {
            // TRANSLATED: Nonzero latency.
            hogp_connection_update_enable = 0;
            /* __hogp_resume_all_ccc_enable(0); */
        } else {
            hogp_connection_update_enable = 1;
        }

        if (ble_hid_timer_handle) {
            log_info("mdy_timer= %d\n", (u32)(little_endian_read_16(ext_param, 14 + 0) * 1.25));
            sys_s_hi_timer_modify(ble_hid_timer_handle, (u32)(little_endian_read_16(ext_param, 14 + 0) * 1.25));
        }

        if (little_endian_read_16(ext_param, 14 + 2)) {
            ble_op_latency_skip(hogp_con_handle, LATENCY_SKIP_INTERVAL_KEEP); //
        }
        break;

    case GATT_COMM_EVENT_DISCONNECT_COMPLETE:
        log_info("disconnect_handle:%04x,reason= %02x\n", little_endian_read_16(packet, 0), packet[2]);
        hogp_con_handle = 0;
        if (8 == packet[2] && hogp_pair_info.pair_flag) {
            // TRANSLATED: After a timeout disconnect, use directed advertising if bonded.
            hogp_pair_info.direct_adv_cnt = REPEAT_DIRECT_ADV_COUNT;
            hogp_adv_config_set();
        }
        break;

    case GATT_COMM_EVENT_ENCRYPTION_CHANGE:
        log_info("ENCRYPTION_CHANGE:handle=%04x,state=%d,process =%02x", little_endian_read_16(packet, 0), packet[2], packet[3]);
        if (packet[3] == LINK_ENCRYPTION_RECONNECT) {
            log_info("reconnect...\n");
            if ((!hogp_pair_info.pair_flag) || memcmp(cur_peer_addr_info, hogp_pair_info.peer_address_info, 7)) {
                log_info("update reconnect peer\n");
                put_buf(cur_peer_addr_info, 7);
                memcpy(hogp_pair_info.peer_address_info, cur_peer_addr_info, 7);
                hogp_pair_info.pair_flag = 1;
                __hogp_conn_pair_vm_do(&hogp_pair_info, 1);
            }

            __hogp_resume_all_ccc_enable(1);
            // TRANSLATED: On reconnect, retrieve this from the pairing table.
            u8 tmp_buf[6];
            u8 remote_type = 0;
            swapX(&cur_peer_addr_info[1], tmp_buf, 6);
            ble_list_get_remote_type(tmp_buf, cur_peer_addr_info[0], &remote_type);
            log_info("list's remote_type:%d\n", remote_type);
            __att_check_remote_result(hogp_con_handle, remote_type);
        } else {
            // TRANSLATED: Start this check only during pairing.
            log_info("first pair...\n");
            memcpy(hogp_pair_info.peer_address_info, cur_peer_addr_info, 7);
            att_server_set_check_remote(hogp_con_handle, __att_check_remote_result);
            hogp_pair_info.pair_flag = 1;
            __hogp_conn_pair_vm_do(&hogp_pair_info, 1);
        }
        __ble_state_to_user(BLE_PRIV_PAIR_ENCRYPTION_CHANGE, first_pair_flag);
        break;

    case GATT_COMM_EVENT_CONNECTION_UPDATE_COMPLETE:
        log_info("conn_param update_complete:%04x\n", little_endian_read_16(packet, 0));
        log_info("update_interval = %d\n", little_endian_read_16(ext_param, 6 + 0));
        log_info("update_latency = %d\n", little_endian_read_16(ext_param, 6 + 2));
        log_info("update_timeout = %d\n", little_endian_read_16(ext_param, 6 + 4));
        if (ble_hid_timer_handle) {
            log_info("mdy_timer= %d\n", (u32)(little_endian_read_16(ext_param, 6 + 0) * 1.25));
            sys_s_hi_timer_modify(ble_hid_timer_handle, (u32)(little_endian_read_16(ext_param, 6 + 0) * 1.25));
        }
        break;

    case GATT_COMM_EVENT_DIRECT_ADV_TIMEOUT:
        log_info("DIRECT_ADV_TIMEOUT\n");
        if (hogp_pair_info.direct_adv_cnt) {
            hogp_pair_info.direct_adv_cnt--;
        }
        hogp_adv_config_set();
        break;

    case GATT_COMM_EVENT_ENCRYPTION_REQUEST:
        first_pair_flag = 1;
        memcpy(hogp_pair_info.peer_address_info, &packet[4], 7);
        hogp_pair_info.pair_flag = 0;
        __ble_state_to_user(BLE_PRIV_MSG_PAIR_CONFIRM, 0);
        break;

    case GATT_COMM_EVENT_SERVER_STATE:
        log_info("server_state: %02x,%02x\n", little_endian_read_16(packet, 1), packet[0]);
        __ble_state_to_user(packet[0], hogp_con_handle);
        break;

    case GATT_COMM_EVENT_CONNECTION_UPDATE_REQUEST_RESULT:
    case GATT_COMM_EVENT_MTU_EXCHANGE_COMPLETE:
        break;

    default:
        break;
    }
    return 0;
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Handle client reads.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  TRANSLATED: A readable profile attribute marked DYNAMIC invokes read_callback.
 */
/*************************************************************************************************/

// ATT Client Read Callback for Dynamic Data
// - if buffer == NULL, don't copy data, just return size of value
// - if buffer != NULL, copy data and return number bytes copied
// @param con_handle of hci le connection
// @param attribute_handle to be read
// @param offset defines start of attribute value
// @param buffer
// @param buffer_size

// TRANSLATED: Stack callback for host ATT reads.
static uint16_t hogp_att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset, uint8_t *buffer, uint16_t buffer_size)
{

    uint16_t  att_value_len = 0;
    uint16_t handle = att_handle;

    log_info("read_callback, handle= 0x%04x,buffer= %08x,offset= %04x\n", handle, (u32)buffer, offset);

    switch (handle) {
    case ATT_CHARACTERISTIC_2a00_01_VALUE_HANDLE: {
        char *gap_name = ble_comm_get_gap_name();
        att_value_len = strlen(gap_name);

        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            break;
        }

        if (buffer) {
            memcpy(buffer, &gap_name[offset], buffer_size);
            att_value_len = buffer_size;
            log_info("\n------read gap_name: %s\n", gap_name);
        }
    }
    break;

    case ATT_CHARACTERISTIC_2a01_01_VALUE_HANDLE:
        att_value_len = sizeof(Appearance);
        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            break;
        }
        if (buffer) {
            memcpy(buffer, &Appearance[offset], buffer_size);
            att_value_len = buffer_size;
        }
        break;

    case ATT_CHARACTERISTIC_2a04_01_VALUE_HANDLE:
        att_value_len = 8;//fixed
        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            break;
        }
        if (buffer) {
            log_info("\n------get Peripheral_Preferred_Connection_Parameters\n");
            memcpy(buffer, &Peripheral_Preferred_Connection_Parameters[0], buffer_size);
            att_value_len = buffer_size;
        }
        break;

    case ATT_CHARACTERISTIC_2a29_01_VALUE_HANDLE:
        att_value_len = strlen(Manufacturer_Name_String);
        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            break;
        }
        if (buffer) {
            memcpy(buffer, &Manufacturer_Name_String[offset], buffer_size);
            att_value_len = buffer_size;
        }
        break;

    case ATT_CHARACTERISTIC_2a24_01_VALUE_HANDLE:
        att_value_len = strlen(Model_Number_String);
        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            break;
        }
        if (buffer) {
            memcpy(buffer, &Model_Number_String[offset], buffer_size);
            att_value_len = buffer_size;
        }
        break;

    case ATT_CHARACTERISTIC_2a25_01_VALUE_HANDLE:
        att_value_len = strlen(Serial_Number_String);
        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            break;
        }
        if (buffer) {
            memcpy(buffer, &Serial_Number_String[offset], buffer_size);
            att_value_len = buffer_size;
        }
        break;

    case ATT_CHARACTERISTIC_2a27_01_VALUE_HANDLE:
        att_value_len = strlen(Hardware_Revision_String);
        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            break;
        }
        if (buffer) {
            memcpy(buffer, &Hardware_Revision_String[offset], buffer_size);
            att_value_len = buffer_size;
        }
        break;

    case ATT_CHARACTERISTIC_2a26_01_VALUE_HANDLE:
        att_value_len = strlen(Firmware_Revision_String);
        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            break;
        }
        if (buffer) {
            memcpy(buffer, &Firmware_Revision_String[offset], buffer_size);
            att_value_len = buffer_size;
        }
        break;

    case ATT_CHARACTERISTIC_2a28_01_VALUE_HANDLE:
        att_value_len = strlen(Software_Revision_String);
        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            break;
        }
        if (buffer) {
            memcpy(buffer, &Software_Revision_String[offset], buffer_size);
            att_value_len = buffer_size;
        }
        break;

    case ATT_CHARACTERISTIC_2a23_01_VALUE_HANDLE:
        att_value_len = sizeof(System_ID);
        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            break;
        }
        if (buffer) {
            memcpy(buffer, &System_ID[offset], buffer_size);
            att_value_len = buffer_size;
        }
        break;

    case ATT_CHARACTERISTIC_2a50_01_VALUE_HANDLE:
        log_info("read PnP_ID\n");
        att_value_len = sizeof(PnP_ID);
        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            break;
        }
        if (buffer) {
            memcpy(buffer, &PnP_ID[offset], buffer_size);
            att_value_len = buffer_size;
        }
        break;

    case ATT_CHARACTERISTIC_2a19_01_VALUE_HANDLE:
        att_value_len = 1;
        if (buffer) {
            if (get_vbat_percent_call) {
                hid_battery_level = hid_get_vbat_handle();
                log_info("read vbat:%d\n", hid_battery_level);
            }
            buffer[0] = hid_battery_level;
        }
        break;

    case ATT_CHARACTERISTIC_2a4b_01_VALUE_HANDLE:
        att_value_len = HID_REPORT_MAP_SIZE;
        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            break;
        }
        if (buffer) {
            memcpy(buffer, &HID_REPORT_MAP_DATA[offset], buffer_size);
            att_value_len = buffer_size;
        }
        break;

    case ATT_CHARACTERISTIC_2a4a_01_VALUE_HANDLE:
        att_value_len = sizeof(hid_information);
        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            break;
        }
        if (buffer) {
            memcpy(buffer, &hid_information[offset], buffer_size);
            att_value_len = buffer_size;
        }
        break;

    case ATT_CHARACTERISTIC_ae10_01_VALUE_HANDLE:
        att_value_len = sizeof(hid_information1);
        if ((offset >= att_value_len) || (offset + buffer_size) > att_value_len) {
            log_info("len is err");
            break;
        }
        if (buffer) {
            memcpy(buffer, &hid_information1[offset], buffer_size);
            att_value_len = buffer_size;
            put_buf(buffer, att_value_len);
        }
        /* __hogp_send_connetion_update_deal(connection_handle); */
        break;

    case ATT_CHARACTERISTIC_2a05_01_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_2a19_01_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_2a4d_01_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_2a4d_02_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_2a4d_04_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_2a4d_05_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_2a4d_06_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_2a4d_07_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_ae42_01_CLIENT_CONFIGURATION_HANDLE:
        if (buffer) {
            buffer[0] = ble_gatt_server_characteristic_ccc_get(hogp_con_handle, handle);
            buffer[1] = 0;
        }
        att_value_len = 2;
        break;

    default:
        break;
    }

    log_info("att_value_len= %d\n", att_value_len);
    return att_value_len;

}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Handle client reads (original SDK description on the write handler).
 *
 *  \param      [in]
 *
 *  \return
 *
 *  TRANSLATED: A readable profile attribute marked DYNAMIC invokes read_callback (original SDK note on the write handler).
 */
/*************************************************************************************************/
// ATT Client Write Callback for Dynamic Data
// @param con_handle of hci le connection
// @param attribute_handle to be written
// @param transaction - ATT_TRANSACTION_MODE_NONE for regular writes, ATT_TRANSACTION_MODE_ACTIVE for prepared writes and ATT_TRANSACTION_MODE_EXECUTE
// @param offset into the value - used for queued writes and long attributes
// @param buffer
// @param buffer_size
// @param signature used for signed write commmands
// @returns 0 if write was ok, ATT_ERROR_PREPARE_QUEUE_FULL if no space in queue, ATT_ERROR_INVALID_OFFSET if offset is larger than max buffer

// TRANSLATED: Stack callback for host ATT writes.
static int hogp_att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size)
{
    int result = 0;
    u16 tmp16;

    u16 handle = att_handle;

    log_info("write_callback, handle= 0x%04x,size = %d\n", handle, buffer_size);

    switch (handle) {

    case ATT_CHARACTERISTIC_2a00_01_VALUE_HANDLE:
        break;

    case ATT_CHARACTERISTIC_2a4d_03_VALUE_HANDLE:
        put_buf(buffer, buffer_size);           // TRANSLATED: Keyboard LED state.
        if (le_hogp_output_callback) {
            le_hogp_output_callback(buffer, buffer_size);
        }
        break;

    case ATT_CHARACTERISTIC_2a05_01_CLIENT_CONFIGURATION_HANDLE:
        ble_gatt_server_characteristic_ccc_set(connection_handle, handle, buffer[0]);
        if (buffer[0]) {
            __check_report_map_change();
        }
        break;

    case ATT_CHARACTERISTIC_ae42_01_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_2a4d_01_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_2a4d_02_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_2a4d_04_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_2a4d_05_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_2a4d_06_CLIENT_CONFIGURATION_HANDLE:
    case ATT_CHARACTERISTIC_2a4d_07_CLIENT_CONFIGURATION_HANDLE:
        ble_op_latency_skip(hogp_con_handle, LATENCY_SKIP_INTERVAL_MIN); //
    case ATT_CHARACTERISTIC_2a19_01_CLIENT_CONFIGURATION_HANDLE:
        __hogp_send_connetion_update_deal(hogp_con_handle);
        log_info("\n------write ccc:%04x,%02x\n", handle, buffer[0]);
        ble_gatt_server_characteristic_ccc_set(hogp_con_handle, handle, buffer[0]);
        break;

    case ATT_CHARACTERISTIC_ae41_01_VALUE_HANDLE:
        ble_hid_transfer_channel_recieve(buffer, buffer_size);
        break;

    case ATT_CHARACTERISTIC_ae10_01_VALUE_HANDLE:
        tmp16 = sizeof(hid_information1);
        if ((offset >= tmp16) || (offset + buffer_size) > tmp16) {
            break;
        }

        log_info("cur_state:");
        put_buf(hid_information1, tmp16);

        memcpy(&hid_information1[offset], buffer, buffer_size);

        log_info("new_state:");
        put_buf(hid_information1, tmp16);
        ble_hid_transfer_channel_recieve1(hid_information1, tmp16);
        break;

    default:
        break;
    }
    return 0;
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Generate advertising data in the buffer.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static u8  adv_name_ok;   // TRANSLATED: Prefer placing the name in advertising data.
static int hogp_make_set_adv_data(u8 adv_reconnect)
{
    u8 offset = 0;
    u8 *buf = hogp_adv_data;

    offset += make_eir_packet_val(&buf[offset], offset, HCI_EIR_DATATYPE_FLAGS, FLAGS_GENERAL_DISCOVERABLE_MODE | FLAGS_EDR_NOT_SUPPORTED, 1);
    offset += make_eir_packet_val(&buf[offset], offset, HCI_EIR_DATATYPE_COMPLETE_16BIT_SERVICE_UUIDS, HID_UUID_16, 2);
    offset += make_eir_packet_data(&buf[offset], offset, HCI_EIR_DATATYPE_APPEARANCE_DATA, Appearance, 2);

    if (!adv_reconnect) {/* TRANSLATED: Reconnect advertising omits the name by default. */
        char *gap_name = ble_comm_get_gap_name();
        u8 name_len = strlen(gap_name);
        u8 vaild_len = ADV_RSP_PACKET_MAX - (offset + 2);
        if (name_len < vaild_len) {
            offset += make_eir_packet_data(&buf[offset], offset, HCI_EIR_DATATYPE_COMPLETE_LOCAL_NAME, (void *)gap_name, name_len);
            adv_name_ok = 1;
        } else {
            adv_name_ok = 0;
        }
    }

    if (offset > ADV_RSP_PACKET_MAX) {
        puts("***hogp_adv_data overflow!!!!!!\n");
        return -1;
    }
    log_info("hogp_adv_data(%d):", offset);
    log_info_hexdump(buf, offset);
    hogp_server_adv_config.adv_data_len = offset;
    hogp_server_adv_config.adv_data = hogp_adv_data;
    return 0;
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Generate scan-response data in the buffer.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static int hogp_make_set_rsp_data(u8 adv_reconnect)
{
    u8 offset = 0;
    u8 *buf = hogp_scan_rsp_data;

    if (!adv_reconnect) {/* TRANSLATED: Reconnect advertising omits the name by default. */
        if (!adv_name_ok) {
            char *gap_name = ble_comm_get_gap_name();
            u8 name_len = strlen(gap_name);
            u8 vaild_len = ADV_RSP_PACKET_MAX - (offset + 2);
            if (name_len > vaild_len) {
                name_len = vaild_len;
            }
            offset += make_eir_packet_data(&buf[offset], offset, HCI_EIR_DATATYPE_COMPLETE_LOCAL_NAME, (void *)gap_name, name_len);
        }
    }

    if (offset > ADV_RSP_PACKET_MAX) {
        puts("***rsp_data overflow!!!!!!\n");
        return -1;
    }

    log_info("rsp_data(%d):", offset);
    log_info_hexdump(buf, offset);
    hogp_server_adv_config.rsp_data_len = offset;
    hogp_server_adv_config.rsp_data = hogp_scan_rsp_data;
    return 0;
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Change the GATT name.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void modify_ble_name(const char *name)
{
    log_info(">>>modify_ble_name:%s\n", name);
    ble_comm_set_config_name(bt_get_local_name(), 1);
}

static void wait_to_open_adv(void)
{
    if (!hogp_con_handle) {
        log_info("allow open adv");
        ble_gatt_server_adv_enable(1);
    } else {
        log_info("no allow open adv");
    }
}

static void __hogp_reconnect_low_timeout_handle(void)
{
    if (0 == hogp_con_handle && ble_gatt_server_get_work_state() == BLE_ST_ADV) {
        log_info("ADV_RECONNECT timeout!!!\n");
        ble_gatt_server_adv_enable(0);
        hogp_pair_info.direct_adv_cnt = 0;
        hogp_adv_config_set();
        ble_gatt_server_adv_enable(1);
    } else {
        /* TRANSLATED: Cancel directed advertising in other cases. */
        log_info("Set Switch to ADV_IND Config\n");
        hogp_pair_info.direct_adv_cnt = 0;
        hogp_adv_config_set();
    }
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Configure advertising parameters.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  TRANSLATED: Undirected reconnect advertising improves compatibility with hosts that do not support directed connections.
 */
/*************************************************************************************************/
static void hogp_adv_config_set(void)
{
    int ret = 0;
    int adv_reconnect_flag = 0;

    hogp_server_adv_config.adv_interval = ADV_INTERVAL_MIN;
    hogp_server_adv_config.adv_auto_do = 1;
    hogp_server_adv_config.adv_channel = le_adv_channel_sel;

    if (PAIR_RECONNECT_ADV_EN && hogp_pair_info.pair_flag && hogp_pair_info.direct_adv_cnt) {
        hogp_server_adv_config.adv_type = pair_reconnect_adv_type;
        memcpy(hogp_server_adv_config.direct_address_info, hogp_pair_info.peer_address_info, 7);
        if (pair_reconnect_adv_type != ADV_DIRECT_IND) {
            hogp_server_adv_config.adv_interval = PAIR_RECONNECT_ADV_INTERVAL;
            if (pair_reconnect_adv_timeout) {
                if (hogp_adv_timeout_number) {
                    sys_timeout_del(hogp_adv_timeout_number);
                    hogp_adv_timeout_number = 0;
                }
                hogp_adv_timeout_number = sys_timeout_add(0, __hogp_reconnect_low_timeout_handle, pair_reconnect_adv_timeout);
            }
        }
        log_info("RECONNECT_ADV1= %02x, address:", pair_reconnect_adv_type);
        put_buf(hogp_server_adv_config.direct_address_info, 7);
        adv_reconnect_flag = 1;
    } else {
        hogp_server_adv_config.adv_type = ADV_IND;
        memset(hogp_server_adv_config.direct_address_info, 0, 7);
    }

    log_info("adv_type:%d,channel=%02x\n", hogp_server_adv_config.adv_type, hogp_server_adv_config.adv_channel);

    ret |= hogp_make_set_adv_data(adv_reconnect_flag);
    ret |= hogp_make_set_rsp_data(adv_reconnect_flag);

    if (ret) {
        log_info("adv_setup_init fail!!!\n");
        return;
    }
    ble_gatt_server_set_adv_config(&hogp_server_adv_config);

}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Configure directed-advertising parameters.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  TRANSLATED: Undirected reconnect advertising improves compatibility with hosts that do not support directed connections.
 */
/*************************************************************************************************/
void hogp_reconnect_adv_config_set(u8 adv_type, u32 adv_timeout)
{
    int ret = 0;
    int adv_reconnect_flag = 0;

    hogp_server_adv_config.adv_interval = PAIR_RECONNECT_ADV_INTERVAL;
    hogp_server_adv_config.adv_auto_do = 1;
    hogp_server_adv_config.adv_channel = ADV_CHANNEL_ALL;

    __hogp_conn_pair_vm_do(&hogp_pair_info, 0);

    if (hogp_pair_info.pair_flag) {
        adv_reconnect_flag = 1; /* TRANSLATED: Already paired. */
        hogp_server_adv_config.adv_type = adv_type;
    } else {
        hogp_server_adv_config.adv_type = ADV_IND;/* TRANSLATED: Force discoverable advertising. */
    }

    if (PAIR_RECONNECT_ADV_EN && hogp_pair_info.pair_flag) {
        memcpy(hogp_server_adv_config.direct_address_info, &hogp_pair_info.peer_address_info, 7);
        log_info("RECONNECT_ADV2= %02x, address:", adv_type);
        put_buf(hogp_server_adv_config.direct_address_info, 7);

        if (adv_type == ADV_DIRECT_IND) {
            hogp_pair_info.direct_adv_cnt = (adv_timeout + 1279) / 1280; /* TRANSLATED: Number of directed-advertising attempts. */
        } else {
            if (adv_timeout) {
                /* TRANSLATED: Switch to undirected advertising after the timeout. */
                if (hogp_adv_timeout_number) {
                    sys_timeout_del(hogp_adv_timeout_number);
                    hogp_adv_timeout_number = 0;
                }
                hogp_adv_timeout_number = sys_timeout_add(0, __hogp_reconnect_low_timeout_handle, adv_timeout);
            }
        }
    } else {
        memset(hogp_server_adv_config.direct_address_info, 0, 7);
        hogp_server_adv_config.adv_interval = ADV_INTERVAL_MIN;
    }

    log_info("adv_type:%d,channel=%02x\n", hogp_server_adv_config.adv_type, hogp_server_adv_config.adv_channel);

    ret |= hogp_make_set_adv_data(adv_reconnect_flag);
    ret |= hogp_make_set_rsp_data(adv_reconnect_flag);

    if (ret) {
        log_info("adv_setup_init fail!!!\n");
        return;
    }
    ble_gatt_server_set_adv_config(&hogp_server_adv_config);
}

/*************************************************************************************************/
/*!
 *  \brief      init
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static void hogp_server_init(void)
{
    log_info("%s", __FUNCTION__);
    ble_gatt_server_set_profile(hogp_profile_data, sizeof(hogp_profile_data));
    hogp_adv_config_set();
}

/*************************************************************************************************/
/*!
 *  \brief      bt_ble_before_start_init
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void bt_ble_before_start_init(void)
{
    log_info("%s", __FUNCTION__);
    ble_comm_init(&hogp_gatt_control_block);
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Compute battery level by accumulating samples and averaging.
 *
 *  \param      [in]
 *
 *  \return vbat percent
 *
 *  \note
 */
/*************************************************************************************************/
static u8 hid_get_vbat_handle(void)
{
    if (!get_vbat_percent_call) {
        return 0;
    }

    u8 cur_val, avg_val, val;

    if (hid_battery_level_add_cnt > 10) {
        /* TRANSLATED: Average after more than ten samples. */
        hid_battery_level_add_sum = hid_battery_level_add_sum / hid_battery_level_add_cnt;
        hid_battery_level_add_cnt = 1;
    }

    cur_val = get_vbat_percent_call();

    if (hid_battery_level_add_cnt) {
        avg_val = hid_battery_level_add_sum / hid_battery_level_add_cnt;
        if (cur_val > (avg_val + 2) || (cur_val + 2) < avg_val) {
            /* TRANSLATED: Large change. */
            hid_battery_level_add_sum = 0;
            hid_battery_level_add_cnt = 0;
        }
    }

    hid_battery_level_add_sum += cur_val;
    hid_battery_level_add_cnt++;

    /* TRANSLATED: Simple accumulation and averaging. */
    val = (u8)(hid_battery_level_add_sum / hid_battery_level_add_cnt);
    log_info("vbat: avg=%d,cur=%d,val=%d\n", avg_val, cur_val, val);
    return val;
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Periodically check battery changes and notify the host.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static void hid_battery_timer_handler(void *priev)
{
    if (hogp_con_handle && get_vbat_percent_call) {
        u8 tmp_val = hid_get_vbat_handle();
        /* tmp_val = (tmp_val +10)%101; */
        if (hid_battery_level != tmp_val) {
            hid_battery_level = tmp_val;
            if (ble_gatt_server_characteristic_ccc_get(hogp_con_handle, ATT_CHARACTERISTIC_2a19_01_CLIENT_CONFIGURATION_HANDLE)) {
                log_info("notify battery: %d\n", hid_battery_level);
                ble_comm_att_send_data(hogp_con_handle, ATT_CHARACTERISTIC_2a19_01_VALUE_HANDLE, &hid_battery_level, 1, ATT_OP_AUTO_READ_CCC);
            }
        }
    }
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Initialize the module.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void bt_ble_init(void)
{
    log_info("%s\n", __FUNCTION__);
    log_info("ble_file: %s", __FILE__);

    hogp_con_handle = 0;
    __hogp_conn_pair_vm_do(&hogp_pair_info, 0);
    if (hogp_pair_info.pair_flag) {
        hogp_pair_info.direct_adv_cnt = REPEAT_DIRECT_ADV_COUNT;
    }

    if (!hid_battery_notify_timer_id && HID_BATTERY_TIMER_SET) {
        log_info("add hid_battery_notify_timer_id\n");
        hid_battery_notify_timer_id = sys_timer_add((void *)0, hid_battery_timer_handler, HID_BATTERY_TIMER_SET);
    }

#if DOUBLE_BT_SAME_NAME
    ble_comm_set_config_name(bt_get_local_name(), 0);
#else
    ble_comm_set_config_name(bt_get_local_name(), 1);
#endif

    hogp_server_init();
    ble_module_enable(1);
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Shut down the module.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void bt_ble_exit(void)
{
    log_info("***** ble_exit******\n");
    if (ble_hid_timer_handle) {
        sys_s_hi_timer_del(ble_hid_timer_handle);
        ble_hid_timer_handle = 0;
    }

    if (hid_battery_notify_timer_id) {
        sys_timer_del(hid_battery_notify_timer_id);
        hid_battery_notify_timer_id = 0;
    }

    log_info("%s\n", __FUNCTION__);
    ble_module_enable(0);
    ble_comm_exit();
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Enable/disable the module.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void ble_module_enable(u8 en)
{
    log_info("mode_en:%d\n", en);
    ble_comm_module_enable(en);
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Register the sleep-entry control.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
static u8 hogp_idle_query(void)
{
    return !is_hogp_active;
}

REGISTER_LP_TARGET(le_hogp_target) = {
    .name = "ble_hogp",
    .is_idle = hogp_idle_query,
};

/*************************************************************************************************/
/*!
 *  TRANSLATED: Set the device icon.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void le_hogp_set_icon(u16 class_type)
{
    memcpy(Appearance, &class_type, 2);
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Set the HID descriptor.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void le_hogp_set_ReportMap(u8 *map, u16 size)
{
    report_map = map;
    report_map_size = size;
    hid_report_change = 1;
}

/*************************************************************************************************/
/*!
 *  \brief      set callback
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void le_hogp_set_output_callback(void *cb)
{
    le_hogp_output_callback = cb;
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Configure bonding in the pairing table.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void le_hogp_set_pair_config(u8 pair_max, u8 is_allow_cover)
{
    ble_list_config_reset(pair_max, is_allow_cover); // TRANSLATED: Enable one-to-one bonding.
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Allow pairing and bonding.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void le_hogp_set_pair_allow(void)
{
    hogp_pair_info.pair_flag = 0;
    __hogp_conn_pair_vm_do(&hogp_pair_info, 1);
    ble_list_clear_all();
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Get battery level.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void le_hogp_regiest_get_battery(u8(*get_battery_cbk)(void))
{
    get_vbat_percent_call = get_battery_cbk;
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Configure reconnect advertising parameters.
 *
 *  \param     type [in] ADV_DIRECT_IND, ADV_DIRECT_IND_LOW,ADV_IND
 *
 *  TRANSLATED: ADV_DIRECT_IND: high-duty directed advertising, 1.28 s timeout, fixed 2 ms interval.
 *  TRANSLATED: ADV_DIRECT_IND_LOW: directed, with normal advertising interval and channels.
 *  TRANSLATED: ADV_IND: undirected, with normal advertising interval and channels.
 *  \param     adv_timeout [in]

 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void le_hogp_set_reconnect_adv_cfg(u8 adv_type, u32 adv_timeout)
{
    pair_reconnect_adv_type = adv_type;
    pair_reconnect_adv_timeout = adv_timeout;
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Configure advertising channels.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void le_hogp_set_adv_channel(int channel)
{
    le_adv_channel_sel = channel;
}

/*************************************************************************************************/
/*!
 *  \brief      set PNP_PID
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void le_hogp_set_PNP_info(const u8 *info)
{
    memcpy(PnP_ID, info, sizeof(PnP_ID));
}

/*************************************************************************************************/
/*!
 *  \brief      is connect
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
int ble_hid_is_connected(void)
{
    return hogp_con_handle;
}

/*************************************************************************************************/
/*!
 *  \brief      disconnect
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void le_hogp_disconnect(void)
{
    if (hogp_con_handle) {
        ble_comm_disconnect(hogp_con_handle);
    }
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Get the current peer address information.
 *
 *  \param      [in]
 *
 *  \return     addr_type + address
 *
 *  \note
 */
/*************************************************************************************************/
u8 *ble_cur_connect_addrinfo(void)
{
    return cur_peer_addr_info;
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Set the bonded address.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void ble_set_pair_addrinfo(u8 *addr_info)
{
    log_info("ble_set_pair_addrinfo\n");

    if (addr_info) {
        put_buf(addr_info, 7);
        memcpy(hogp_pair_info.peer_address_info, addr_info, 7);
        hogp_pair_info.head_tag = BLE_VM_HEAD_TAG;
        hogp_pair_info.tail_tag = BLE_VM_TAIL_TAG;
        hogp_pair_info.pair_flag = 1;
        hogp_pair_info.direct_adv_flag = 1;
        hogp_pair_info.direct_adv_cnt = REPEAT_DIRECT_ADV_COUNT;
        __hogp_conn_pair_vm_do(&hogp_pair_info, 1);
    } else {
        memset(&hogp_pair_info, 0, sizeof(struct pair_info_t));
    }
}

// TRANSLATED: Report IDs supported by the profile.
static const u16 report_id_handle_table[] = {
    0,
    HID_REPORT_ID_01_SEND_HANDLE,
    HID_REPORT_ID_02_SEND_HANDLE,
    HID_REPORT_ID_03_SEND_HANDLE,
    HID_REPORT_ID_04_SEND_HANDLE,
    HID_REPORT_ID_05_SEND_HANDLE,
    HID_REPORT_ID_06_SEND_HANDLE,
};

/*************************************************************************************************/
/*!
 *  TRANSLATED: Send HID data.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
int ble_hid_data_send(u8 report_id, u8 *data, u16 len)
{
    if (report_id == 0 || report_id > 6) {
        log_info("report_id %d,err!!!\n", report_id);
        return -1;
    }

    putchar('@');
    return ble_comm_att_send_data(hogp_con_handle, report_id_handle_table[report_id], data, len, ATT_OP_AUTO_READ_CCC);
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Send HID data.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void ble_hid_key_deal_test(u16 key_msg)
{
    if (key_msg) {
        ble_hid_data_send(1, &key_msg, 2);
        key_msg = 0;//key release
        ble_hid_data_send(1, &key_msg, 2);
    }
}

/*************************************************************************************************/
/*!
 *  \brief
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
int ble_hid_transfer_channel_send(u8 *packet, u16 size)
{
    /* log_info("transfer_tx(%d):", size); */
    /* log_info_hexdump(packet, size); */
    return ble_comm_att_send_data(hogp_con_handle, ATT_CHARACTERISTIC_ae42_01_VALUE_HANDLE, packet, size, ATT_OP_AUTO_READ_CCC);
}

/*************************************************************************************************/
/*!
 *  \brief
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
int ble_hid_transfer_channel_send1(u8 *packet, u16 size)
{
    /* log_info("transfer_tx(%d):", size); */
    /* log_info_hexdump(packet, size); */
    return ble_comm_att_send_data(hogp_con_handle, ATT_CHARACTERISTIC_ae10_01_VALUE_HANDLE, packet, size, ATT_OP_AUTO_READ_CCC);
}

/*************************************************************************************************/
/*!
 *  \brief
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void __attribute__((weak)) ble_hid_transfer_channel_recieve(u8 *packet, u16 size)
{
    log_info("transfer_rx(%d):", size);
    log_info_hexdump(packet, size);
    //ble_hid_transfer_channel_send(packet,size);//for test
}
/*************************************************************************************************/
/*!
 *  \brief
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void __attribute__((weak)) ble_hid_transfer_channel_recieve1(u8 *packet, u16 size)
{
    log_info("transfer_rx1(%d):", size);
    log_info_hexdump(packet, size);
}

/*************************************************************************************************/
/*!
 *  TRANSLATED: Control pairing mode.
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void ble_set_pair_list_control(u8 mode)
{
    bool ret = 0;
    u8 connect_address[6];
    switch (mode) {
    case 0:
        // TRANSLATED: Disable pairing.
        ret = ble_list_pair_accept(0);
        break;

    case 1:
        // TRANSLATED: Enable pairing.
        ret = ble_list_pair_accept(1);
        break;

    case 2:
        // TRANSLATED: Keep the existing bond and reject new pairing requests.
        swapX(&cur_peer_addr_info[1], connect_address, 6);
        //bond current's device
        ret = ble_list_bonding_remote(connect_address, cur_peer_addr_info[0]);
        //close pair
        ble_list_pair_accept(0);
        break;

    default:
        break;
    }
    log_info("%s: %02x,ret=%02x\n", __FUNCTION__, mode, ret);
}

/*************************************************************************************************/
/*!
 *  \brief      testbox,key test
 *
 *  \param      [in]
 *
 *  \return
 *
 *  \note
 */
/*************************************************************************************************/
void ble_server_send_test_key_num(u8 key_num)
{
    if (hogp_con_handle) {
        if (get_remote_test_flag()) {
            ble_op_test_key_num(hogp_con_handle, key_num);
        } else {
            log_info("-not conn testbox\n");
        }
    }
}

void call_hogp_adv_config_set()
{
    return hogp_adv_config_set();
}
