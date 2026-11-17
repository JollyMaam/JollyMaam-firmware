#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

#include "link_limits.h"

/* Port features.  The Jieli libc provides memcpy/strlen/..., but BTstack's
 * allocators must not rely on malloc: a pool configuration is required so
 * that every registered service/channel has its own storage. */
/* BTstack's own assert: btstack_assert_failed() gets the file's basename and
 * the line, without the path, function and expression strings of libc's
 * assert() - about 3.4 KB less flash. */
#define ENABLE_BTSTACK_ASSERT

#define ENABLE_CLASSIC
/* The pad never runs an inquiry, asks for a remote name or answers a pairing
 * request itself (SSP auto-accept).  Only honoured by a BTstack carrying the
 * patch (branch btstack-jl-port of the fork): 1.5 KB of hci.c. */
#define DISABLE_CLASSIC_GAP_CLIENT

#define ENABLE_BLE
/* LE Secure Connections.  P-256 through the controller's HCI commands, which
 * btstack_hci_jieli.c answers on a thread of its own (the controller's are
 * stubs). */
#define ENABLE_LE_SECURE_CONNECTIONS
#ifdef BOARD_DONGLE
#define ENABLE_LE_CENTRAL        /* the link to our pad (link.h) */
#else
#define ENABLE_LE_PERIPHERAL
#endif
/* Ask for 251-byte LE PDUs on every connection: at the default 27 bytes a
 * Switch 2 input report (70 bytes with headers) took three connection events. */
#define ENABLE_LE_DATA_LENGTH_EXTENSION
#define MAX_NR_LE_DEVICE_DB_ENTRIES     4
#define MAX_NR_SM_LOOKUP_ENTRIES        3
#ifdef BOARD_DONGLE
#define MAX_NR_WHITELIST_ENTRIES        HUB_SLOTS       /* HUB_SLOTS bonded pads */
#else
#define MAX_NR_WHITELIST_ENTRIES        1
#endif
#define MAX_NR_GATT_CLIENTS             0
#define MAX_ATT_DB_SIZE                 512
#define ENABLE_LOG_ERROR

#ifdef CONFIG_CPU_BD19
#define HCI_ACL_PAYLOAD_SIZE            256     /* 13 KB of heap: link packets are small */
#else
#define HCI_ACL_PAYLOAD_SIZE            516
#endif
/* RX queue entry size in the transport; must hold header + ACL payload. */
#define HCI_INCOMING_PACKET_BUFFER_SIZE (HCI_ACL_PAYLOAD_SIZE + 8)

#ifdef CONFIG_CPU_BD19
/* A full queue drops the controller's packet, events too; the slot being
 * handled counts.  At 3, the events after a connection came in a burst and
 * Read Remote Features Complete was lost. */
#define RX_QUEUE_DEPTH                  6
#else
#define RX_QUEUE_DEPTH                  16
#endif

#ifdef BOARD_DONGLE
#define MAX_NR_HCI_CONNECTIONS          HUB_SLOTS       /* HUB_SLOTS pads */
#else
#define MAX_NR_HCI_CONNECTIONS          1
#endif
#define MAX_NR_L2CAP_CHANNELS           8

#define MAX_NR_L2CAP_SERVICES           4
#define MAX_NR_SERVICE_RECORD_ITEMS     3

#endif
