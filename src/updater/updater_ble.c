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
#include "pad.h"

/* The updater over Bluetooth LE: the tool device's commands (updater.c's
 * usb_command) in one GATT service, no pairing.  The host writes a frame to
 * REQUEST: u8 sub-command, u32 argument (little-endian), then the data a
 * host-to-pad command carries (UPDATE_DATA's image bytes).  A command with
 * an answer (IDENTIFY, STATUS, LOG, UPDATE_INFO) answers in a notification
 * of REPLY: u8 sub-command, then the answer, at most the ATT MTU - 4 bytes.
 * The host writes with response, one at a time; the response goes once the
 * command ran (a sector erased, the data programmed).  A flood of writes
 * without response filled the HCI receive queue (btstack_hci_jieli.c) while
 * a sector erased: it drops what does not fit, and BTstack joined the
 * fragments left of two writes into one.
 *
 * Advertised as "<board> Update" with the service UUID; the address is the
 * flash UID's, apart from the pad's slots. */

#define LE16(v)  (u8)(v), (u8)((v) >> 8)
/* c255xxxx-3e2d-66b1-8a4f-d5037c2b419e, least significant byte first. */
#define UUID(n)  0x9e, 0x41, 0x2b, 0x7c, 0x03, 0xd5, 0x4f, 0x8a, 0xb1, 0x66, 0x2d, 0x3e, n, 0x00, 0x55, 0xc2
#define SERVICE(h)                   LE16(24), LE16(0x0002), LE16(h), LE16(0x2800), UUID(0)
#define CHARACTERISTIC(h, props, n)  LE16(27), LE16(0x0002), LE16(h), LE16(0x2803), props, LE16((h) + 1), UUID(n)
#define VALUE(h, flags, n)           LE16(22), LE16(0x0300 | (flags)), LE16(h), UUID(n)
#define CCC(h)                       LE16(8), LE16(0x010e), LE16(h), LE16(0x2902)
enum { WRITE_COMMAND = 0x04, WRITE = 0x08, NOTIFY = 0x10 };
enum { REQUEST = 0x0003, REPLY = 0x0005 };

static const u8 att_db[] = {
    1,
    SERVICE(0x0001),
    CHARACTERISTIC(REQUEST - 1, WRITE_COMMAND | WRITE, 1),
    VALUE(REQUEST, WRITE_COMMAND | WRITE, 1),
    CHARACTERISTIC(REPLY - 1, NOTIFY, 2),
    VALUE(REPLY, 0, 2),
    CCC(REPLY + 1),
    0, 0,
};

const hci_transport_t *hci_jieli_transport(void);
const btstack_run_loop_t *hci_jieli_run_loop(void);
void hci_jieli_set_address(const u8 a[6]);
u32 usb_command(const u8 cdb[16], u8 *data, u32 max, int from_host);

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
    att_server_notify(con, REPLY, reply, reply_len);
    reply_len = 0;
}

static int att_write(hci_con_handle_t h, u16 handle, u16 mode, u16 offset, u8 *buf, u16 size)
{
    u8 cdb[16] = { 0xf5 };
    u32 n, max;

    (void)mode;
    if (handle != REQUEST || offset || size < 5) return 0;
    con = h;
    cdb[1] = buf[0];
    memcpy(cdb + 2, buf + 1, 4);
    if (size > 5) {
        usb_command(cdb, buf + 5, size - 5, 1);
        return 0;
    }
    max = att_server_get_mtu(h) - 4u;
    if (max > sizeof(reply) - 1) max = sizeof(reply) - 1;
    memset(reply, 0, sizeof(reply));
    n = usb_command(cdb, reply + 1, max, 0);
    if (n) {
        reply[0] = buf[0];
        reply_len = (u16)(n + 1);
        send_reply();
    }
    return 0;
}

static void on_event(u8 type, u16 channel, u8 *packet, u16 size)
{
    (void)channel;
    (void)size;
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
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

static void advertise(void)
{
    static u8 adv[] = { 2, 0x01, 0x06, 17, 0x07, UUID(0) };
    static u8 scan[31];
    bd_addr_t none = { 0 };
    u32 n = strlen(board_name);

    if (n > 22) n = 22;
    scan[0] = (u8)(n + 8);
    scan[1] = 0x09;              /* complete local name */
    memcpy(scan + 2, board_name, n);
    memcpy(scan + 2 + n, " Update", 7);
    gap_advertisements_set_params(0x0030, 0x0030, 0, 0, none, 0x07, 0);
    gap_advertisements_set_data(sizeof(adv), adv);
    gap_scan_response_set_data((u8)(scan[0] + 1), scan);
    gap_advertisements_enable(1);
}

static void body(void)
{
    const u8 *uid = plat_unique_id();
    u8 a[6] = { 0x55, 0x50, 0x44, 0x54, 0x00, 0xc2 };   /* without a UID */
    u32 i;

    if (uid) for (i = 0; i < 6; i++) a[i] = uid[i] ^ uid[i + 6] ^ uid[i + 10] ^ 0x5a;
    hci_jieli_set_address(a);
    btstack_memory_init();
    btstack_run_loop_init(hci_jieli_run_loop());
    hci_init(hci_jieli_transport(), NULL);
    l2cap_init();
    le_device_db_init();
    att_server_init(att_db, NULL, att_write);   /* no SM: sm_none.c */
    hci_registration.callback = on_event;
    hci_add_event_handler(&hci_registration);
    att_server_register_packet_handler(on_event);
    advertise();
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
