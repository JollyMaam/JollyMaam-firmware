/*
 * BTstack configuration for the Cyclone 2 firmware.
 *
 * This build runs the upstream BTstack host (from /work/btstack-jl-port)
 * directly on top of the Jieli BR23 controller library, btctrler.a.  It is a
 * Classic-only HID device configuration: the Switch/console profile talks HID
 * over L2CAP and needs SDP, SSP and the HID device profile, but no RFCOMM
 * profiles, no LE and no audio.
 */
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

/* Port features.  The Jieli libc provides memcpy/strlen/..., but BTstack's
 * allocators must not rely on malloc: a pool configuration is required so
 * that every registered service/channel has its own storage. */
#define HAVE_ASSERT

/* Host features that are actually compiled into the image. */
#define ENABLE_CLASSIC
/* LE, for the Switch 2 profile (GATT peripheral).  The controller already
 * runs its LE link layer: config_btctler_modules includes BT_MODULE_LE with
 * the advertiser and slave roles (src/sdkconfig/lib_btctrler_config.c). */
#define ENABLE_BLE
#define ENABLE_LE_PERIPHERAL
#define MAX_NR_LE_DEVICE_DB_ENTRIES     4
#define MAX_NR_SM_LOOKUP_ENTRIES        3
#define MAX_NR_WHITELIST_ENTRIES        1
#define MAX_NR_GATT_CLIENTS             0
#define MAX_ATT_DB_SIZE                 512
#define ENABLE_LOG_ERROR

/* Tried and dropped (2026-09-25): an hci.c patch, SSP_KEEP_BONDING_FLAG,
 * that kept our own bonding request when the peer asks for "no bonding".
 * A genuine Pro Controller answers the pairing with auth_req 0x00, No
 * Bonding (/shared/nintendo-switch-pro-controller/hcidump), and with the
 * patch on the console refused every pad-initiated reconnect anyway.  The
 * link key is stored regardless: the controller reports it as a combination
 * key, which BTstack keeps whatever the bonding mode.  The patch has been
 * removed from /work/btstack-jl-port. */

/* ACL payload the host handles.  516 = the 512-byte LE ATT MTU a Pro
 * Controller 2 answers the Switch 2 with, plus the L2CAP header: BTstack's
 * ATT server offers min(ATT_REQUEST_BUFFER_SIZE, l2cap_max_le_mtu()), both
 * derived from this.  Outgoing packets are still fragmented to the
 * controller's own buffer size, and the controller delivers LE PDUs of at
 * most 251 bytes that BTstack reassembles. */
#define HCI_ACL_PAYLOAD_SIZE            516
/* RX queue entry size in the transport; must hold header + ACL payload. */
#define HCI_INCOMING_PACKET_BUFFER_SIZE (HCI_ACL_PAYLOAD_SIZE + 8)
/* Half the default depth, so the larger entries cost the same RAM. */
#define BR23_HCI_RX_QUEUE_DEPTH         16

/* Single HID link, plus the L2CAP channels of one HID device connection. */
#define MAX_NR_HCI_CONNECTIONS          1
#define MAX_NR_L2CAP_CHANNELS           8
/* SDP plus the HID control and interrupt services. */
#define MAX_NR_L2CAP_SERVICES           4
#define MAX_NR_SERVICE_RECORD_ITEMS     3

#endif
