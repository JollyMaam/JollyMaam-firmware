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
#define ENABLE_LOG_ERROR

/* Keep our own bonding request when the peer asks for "no bonding".  A Switch
 * console does exactly that and still expects the link key to survive to the
 * next connection, which only a bonding pairing guarantees.  Without this the
 * join pairing is negotiated as non-bonding and the console refuses the stored
 * key on every reconnect (docs/live-bond-capture.md). */
#define SSP_KEEP_BONDING_FLAG 1

/* ACL payload the host sends.  The controller's ACL TX pool is 5 * 512 bytes
 * (CONFIG_BT_TX_BUFF_SIZE in src/sdkconfig/app_config.h), so staying at the
 * Bluetooth 4.0/5.0 default keeps every packet inside one pool buffer. */
#define HCI_ACL_PAYLOAD_SIZE            251
/* RX queue entry size in the transport; must hold header + ACL payload. */
#define HCI_INCOMING_PACKET_BUFFER_SIZE (HCI_ACL_PAYLOAD_SIZE + 8)

/* Single HID link, plus the L2CAP channels of one HID device connection. */
#define MAX_NR_HCI_CONNECTIONS          1
#define MAX_NR_L2CAP_CHANNELS           8
/* SDP plus the HID control and interrupt services. */
#define MAX_NR_L2CAP_SERVICES           4
#define MAX_NR_SDP_SERVICE_RECORDS      3

#endif
