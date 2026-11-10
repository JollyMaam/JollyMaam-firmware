#include <string.h>
#include "btstack_config.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "gap.h"
#include "hci.h"
#include "l2cap.h"
#include "ble/att_db.h"
#include "ble/att_server.h"
#include "ble/le_device_db.h"
#include "updater.h"
#include "updater_gatt.h"

/* The updater over Bluetooth LE: the tool device's commands (updater.c's
 * updater_frame()) in one GATT service (updater_gatt.h), no pairing.  The host writes a frame to
 * REQUEST: u8 sub-command, u8 tag, u32 argument (little-endian), then the
 * data a host-to-pad command carries (UPDATE_DATA's image bytes).  A command
 * with an answer (IDENTIFY, STATUS, LOG, UPDATE_INFO) answers in one
 * notification of REPLY: u8 tag << 2, then the answer, at most the ATT MTU
 * - 4 bytes.
 * The host writes with response, one at a time; the response goes once the
 * command ran (a sector erased, the data programmed).  A flood of writes
 * without response filled the HCI receive queue (btstack_hci_jieli.c) while
 * a sector erased: it drops what does not fit, and BTstack joined the
 * fragments left of two writes into one.
 *
 * Advertised as "<board> Update" with the service UUID; the address is the
 * flash UID's, apart from the pad's slots. */

const hci_transport_t *hci_jieli_transport(void);
const btstack_run_loop_t *hci_jieli_run_loop(void);
void hci_jieli_set_address(const u8 a[6]);

static hci_con_handle_t con = HCI_CON_HANDLE_INVALID;
static btstack_packet_callback_registration_t hci_registration;
static u8 reply[512];
static u16 reply_len;            /* waiting for room to send */

static void send_reply(void)
{
    if (!reply_len || con == HCI_CON_HANDLE_INVALID) return;
    if (!att_server_can_send_packet_now(con)) {
        att_server_request_can_send_now_event(con);
        return;
    }
    att_server_notify(con, UPD_REPLY, reply, reply_len);
    reply_len = 0;
}

static int att_write(hci_con_handle_t h, u16 handle, u16 mode, u16 offset, u8 *buf, u16 size)
{
    u32 max;

    (void)mode;
    if (handle != UPD_REQUEST || offset) return 0;
    con = h;
    max = att_server_get_mtu(h) - 3u;
    if (max > sizeof(reply)) max = sizeof(reply);
    reply_len = (u16)updater_frame(buf, size, reply, max);
    send_reply();
    return 0;
}

static u8 address[6];

static void advertise(void)
{
    static u8 scan[31];
    bd_addr_t none = { 0 };
    u8 n = updater_scan_response(scan);

    gap_advertisements_set_params(0x0030, 0x0030, 0, 0, none, 0x07, 0);
    gap_advertisements_set_data(sizeof(updater_adv), (u8 *)updater_adv);
    gap_scan_response_set_data(n, scan);
    gap_advertisements_enable(1);
}

static void on_event(u8 type, u16 channel, u8 *packet, u16 size)
{
    (void)channel;
    (void)size;
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
        /* Set before power-on, the LE address does not take: set it again,
         * then advertise, as bt_pad.c does. */
        hci_jieli_set_address(address);
        advertise();
        break;
    case HCI_EVENT_META_GAP:
        if (hci_event_gap_meta_get_subevent_code(packet) != GAP_SUBEVENT_LE_CONNECTION_COMPLETE) break;
        con = gap_subevent_le_connection_complete_get_connection_handle(packet);
        util_log("updater: le connected, interval %u", gap_subevent_le_connection_complete_get_conn_interval(packet));
        /* 7.5-15 ms; 4 s outlasts the longest sector erase. */
        gap_request_connection_parameter_update(con, 6, 12, 0, 400);
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        util_log("updater: le disconnected, reason %02x",
                 hci_event_disconnection_complete_get_reason(packet));
        con = HCI_CON_HANDLE_INVALID;
        reply_len = 0;
        break;
    case ATT_EVENT_MTU_EXCHANGE_COMPLETE:
        util_log("updater: le mtu %u", att_event_mtu_exchange_complete_get_MTU(packet));
        break;
    case ATT_EVENT_CAN_SEND_NOW:
        send_reply();
        break;
    }
}

static void body(void)
{
    updater_address(address);
    hci_jieli_set_address(address);
    btstack_memory_init();
    btstack_run_loop_init(hci_jieli_run_loop());
    hci_init(hci_jieli_transport(), NULL);
    l2cap_init();
    le_device_db_init();
    att_server_init(updater_att_db, NULL, att_write);   /* no SM: sm_none.c */
    hci_registration.callback = on_event;
    hci_add_event_handler(&hci_registration);
    att_server_register_packet_handler(on_event);
    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
}

int updater_ble_connected(void)
{
    return con != HCI_CON_HANDLE_INVALID;
}

void updater_ble_start(void)
{
    plat_bt_thread(body);
}
