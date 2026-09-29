#include <system/includes.h>
#include <btstack/avctp_user.h>
#include <btstack/btstack_task.h>

#include "app_comm_bt.h"
#include "btstack_jl_hid_ext.h"
#include "edr_hid_user.h"
#include "main.h"
#include "profile.h"
#include "profile_classic.h"
#include "config.h"
#include "usb.h"

#define CLASSIC_HID_CONNECT_DELAY_MS 100U

#define CLASSIC_RECONNECT_DELAY_MS 300U

static const struct cyclone2_classic_profile *classic_profile;
static u8 classic_slot;
static u8 classic_pairing;
static u16 classic_connects;
static u8 bond_delete_pending;
static u8 peer_debug_remaining;
static u8 bond_to_delete[6];
static u8 reconnect_address[6];
static u8 reconnect_armed;

/* Log the far end of a connection event.  Addresses arrive least significant
 * byte first, like our own, so print them back to front. */
static void log_peer_address(const char *what, const u8 *address)
{
    cyclone2_usb_debug("%s peer %02x:%02x:%02x:%02x:%02x:%02x", what,
                       address[5], address[4], address[3], address[2],
                       address[1], address[0]);
}

extern void bt_set_local_name(char *name, u8 len);
extern void delete_link_key(bd_addr_t *address, u8 index);
/* Vendor controller-library hook: setting its internal key_special_bind flag
 * makes a "key missing" rejection during authentication fall back to SSP on
 * the same link instead of reporting the failure and dropping it.  A console
 * that has no usable record for this controller answers exactly that way, so
 * without this every reconnect ends in reason 5. */
extern void edr_vendor_keep_connect_for_keymiss(u8 enable);

/* Drop the stored link key for a peer.
 *
 * Bonding records live in the Bluetooth controller's own memory (a chip dump
 * shows one at 0x10760: peer address followed by the key), not in flash, so
 * neither reflashing nor erasing VM can clear them.  delete_link_key() is the
 * SDK's way in: the slot index selects a pairing entry and the record is
 * matched by peer address.  Try every index, because the index the SDK tracks
 * belongs to the current connection and a connection that fails
 * authentication never establishes one. */
void cyclone2_classic_forget_peer(const u8 address[6])
{
    u8 index;

    for (index = 0U; index < 4U; index++) {
        delete_link_key((bd_addr_t *)address, index);
    }
}

void cyclone2_classic_forget_all(void)
{
    user_send_cmd_prepare(USER_CTRL_DEL_ALL_REMOTE_INFO, 0, NULL);
}

/* Reconnect to the console whose address is stored in syscfg for this slot.
 * Paging has to wait until the stack has finished initialising, which is why
 * this runs from a timer rather than directly. */
static void reconnect_saved(void *private_data)
{
    (void)private_data;
    if (edr_hid_is_connected()) return;

    cyclone2_usb_debug("reconnect by page to %02x:%02x:%02x:%02x:%02x:%02x",
                       reconnect_address[5], reconnect_address[4],
                       reconnect_address[3], reconnect_address[2],
                       reconnect_address[1], reconnect_address[0]);
    bt_wait_phone_connect_control(0);
    user_send_cmd_prepare(USER_CTRL_START_CONNEC_VIA_ADDR,
                          sizeof(reconnect_address), reconnect_address);
}

/* Retry an outgoing connection that ended with the ACL up but no HID session.
 *
 * A console that accepts our page and then never opens the HID channels leaves
 * that ACL in place, and the stack will not start a second page while a
 * connection exists.  Drop whatever is left and page the saved peer again; the
 * caller bounds how many times this can happen. */
int cyclone2_classic_reconnect(void)
{
    if (edr_hid_is_connected()) return 0;
    if (!reconnect_armed) return 0;
    user_send_cmd_prepare(USER_CTRL_DISCONNECTION_HCI, 0, NULL);
    sys_timeout_add(NULL, reconnect_saved, CLASSIC_RECONNECT_DELAY_MS);
    return 1;
}

static void connect_hid_after_encryption(void *private_data)
{
    int result;

    (void)private_data;
    if (edr_hid_is_connected()) return;
    result = user_send_cmd_prepare(USER_CTRL_HID_CONN, 0, NULL);
    cyclone2_usb_debug("request HID channels after encryption result=%d",
                       result);
}

/* Bring up a Classic HID profile for one slot.
 *
 * `pairing` is set when the user asked to forget the old bond and pair again:
 * the stored remote address for the slot is cleared and the controller waits
 * to be paired (page and inquiry scan enabled once the stack is up).  Bonded
 * starts instead page the stored address, or fall back to the stack's own
 * "connect last device" memory if the slot has no stored address yet. */
void cyclone2_classic_start(const struct cyclone2_classic_profile *profile,
                            u8 slot, u8 pairing)
{
    u8 remote[6];

    /* A reconnect to a console that no longer holds a usable link key must
     * re-pair rather than drop the link; see the declaration above. */
    edr_vendor_keep_connect_for_keymiss(1U);
    classic_profile = profile;
    classic_slot = slot;
    classic_pairing = pairing;
    bond_delete_pending = 0U;
    reconnect_armed = 0U;
    peer_debug_remaining = 8U;
    if (pairing) {
        memcpy(bond_to_delete, config_peer(slot), sizeof(bond_to_delete));
        bond_delete_pending = 1U;
        config_bond_set_peer(slot, NULL);
    }

    bt_set_local_name((char *)profile->local_name,
                      (u8)strlen(profile->local_name));

    btstack_edr_start_before_init(profile->edr, 0);
    btstack_jl_hid_set_device_id(profile->vendor_id, profile->product_id,
                                 profile->version);
    btstack_jl_hid_set_get_report_handler(profile->get_report);
    user_hid_init(profile->output);
    if (profile->session_start) profile->session_start();
}

void cyclone2_classic_disconnect(void)
{
    user_hid_disconnect();
}

int cyclone2_classic_exit(void)
{
    if (classic_profile && classic_profile->session_exit)
        classic_profile->session_exit();
    return btstack_edr_exit(0);
}

int cyclone2_classic_is_connected(void)
{
    return edr_hid_is_connected() != 0;
}

static enum cyclone2_profile_event stay_pairable(void)
{
    if (!classic_pairing) return CYCLONE2_PROFILE_EVENT_DISCONNECTED;
    bt_wait_phone_connect_control(1);
    return CYCLONE2_PROFILE_EVENT_NONE;
}

enum cyclone2_profile_event cyclone2_classic_handle_event(
    const struct sys_event *event)
{
    static const u8 zero_address[6];
    u8 remote[6];

    if ((u32)event->arg == SYS_BT_EVENT_TYPE_CON_STATUS) {
        bt_comm_edr_status_event_handler((struct bt_event *)&event->u.bt);
        if (event->u.bt.event == BT_STATUS_INIT_OK) {
            /* Link milestones go in the bring-up ring: a reset that happens
             * while a console is around needs to say which step of the link it
             * landed in (see docs/power-reset.md). */
            cyclone2_bringup_record(0x6001u);
            if (classic_pairing) {
                if (bond_delete_pending) {

                    cyclone2_classic_forget_peer(bond_to_delete);
                    bond_delete_pending = 0U;
                }
                /* The SDK's own way into pairing mode (see
                 * mouse_single's mouse_bt_pair_start()) also forgets every
                 * device the Bluetooth controller remembers.  Without it a
                 * half-remembered record survives, and the console answers a
                 * connection with an authentication challenge instead of
                 * starting a fresh pairing. */
                user_send_cmd_prepare(USER_CTRL_DEL_ALL_REMOTE_INFO, 0, NULL);
                bt_wait_phone_connect_control(1);
            } else if (memcpy(remote, config_peer(classic_slot), sizeof(remote)),
                       memcmp(remote, zero_address, sizeof(remote))) {
                memcpy(reconnect_address, remote, sizeof(remote));
                reconnect_armed = 1U;
                sys_timeout_add(NULL, reconnect_saved, 100U);
            } else if (!bt_connect_phone_back_start()) {
                bt_wait_phone_connect_control(1);
            }
        } else if (event->u.bt.event == BT_STATUS_FIRST_CONNECTED ||
                   event->u.bt.event == BT_STATUS_SECOND_CONNECTED) {
            /* 0x6900|count: how many times this session has seen a link come
             * up.  A reset that always lands on the same count is a resource
             * leak; one that lands after a fixed time is a timer. */
            classic_connects++;
            cyclone2_bringup_record(0x6002u);
            cyclone2_bringup_record(0x6900u | (u32)(classic_connects & 0xffu));
            if (classic_profile && classic_profile->link_up)
                classic_profile->link_up();
            log_peer_address("connected", event->u.bt.args);
            config_bond_set_peer(classic_slot, event->u.bt.args);
        } else if (event->u.bt.event == BT_STATUS_ENCRY_COMPLETE) {
            cyclone2_bringup_record(0x6003u);
            log_peer_address("encrypted", event->u.bt.args);
            /* Record the console's address again here, now that the link is
             * authenticated rather than merely up.  This is the record the
             * wake sequence aims at (see docs/radio.md), so it is worth
             * writing at the point where the console is known to have
             * completed the exchange, not only when the ACL came up. */
            config_bond_set_peer(classic_slot, event->u.bt.args);
            sys_timeout_add(NULL, connect_hid_after_encryption,
                            CLASSIC_HID_CONNECT_DELAY_MS);
        } else if (event->u.bt.event == BT_STATUS_FIRST_DISCONNECT ||
                   event->u.bt.event == BT_STATUS_SECOND_DISCONNECT) {
            cyclone2_bringup_record(0x6004u);
            cyclone2_bringup_record(0x6a00u | (u32)(classic_connects & 0xffu));
            log_peer_address("disconnected", event->u.bt.args);
            return stay_pairable();
        }
    } else if ((u32)event->arg == SYS_BT_EVENT_TYPE_HCI_STATUS) {
        bt_comm_edr_hci_event_handler((struct bt_event *)&event->u.bt);
        if (event->u.bt.event == HCI_EVENT_CONNECTION_COMPLETE ||
            event->u.bt.event == HCI_EVENT_DISCONNECTION_COMPLETE ||
            event->u.bt.event == BTSTACK_EVENT_HCI_CONNECTIONS_DELETE) {

            cyclone2_bringup_record(
                (event->u.bt.event == HCI_EVENT_CONNECTION_COMPLETE ?
                 0x6500u : 0x6600u) | (u32)(event->u.bt.value & 0xffU));
            if (peer_debug_remaining != 0U) {
                peer_debug_remaining--;
                log_peer_address("hci", event->u.bt.args);
            }
        }
        /* The console owns the other half of a bond, so a rejected link key
         * cannot be repaired from here.  Say so instead of leaving the user
         * with a silent disconnect. */
        if (event->u.bt.event == HCI_EVENT_DISCONNECTION_COMPLETE &&
            (event->u.bt.value == ERROR_CODE_AUTHENTICATION_FAILURE ||
             event->u.bt.value == ERROR_CODE_PIN_OR_KEY_MISSING)) {
            cyclone2_usb_debug(
                "link key rejected (reason=%u); pair again from the host "
                "(console menu, or 'pair %u fresh' here)",
                event->u.bt.value, classic_slot);
        }
        if (event->u.bt.event == HCI_EVENT_DISCONNECTION_COMPLETE)
            return stay_pairable();
    }
    return CYCLONE2_PROFILE_EVENT_NONE;
}
