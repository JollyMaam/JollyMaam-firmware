#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

/* The updater's BTstack: an LE peripheral with one GATT service and no
 * pairing (src/updater/updater_ble.c).  The rest as the pad's
 * (src/boards/gamesir-cyclone-2/btstack_config.h). */
#define ENABLE_BTSTACK_ASSERT
#define ENABLE_BLE
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_DATA_LENGTH_EXTENSION
#define MAX_NR_LE_DEVICE_DB_ENTRIES     1
#define MAX_NR_SM_LOOKUP_ENTRIES        1
#define MAX_NR_WHITELIST_ENTRIES        1
#define MAX_NR_GATT_CLIENTS             0
#define MAX_ATT_DB_SIZE                 128
#define ENABLE_LOG_ERROR

#define HCI_ACL_PAYLOAD_SIZE            516
#define HCI_INCOMING_PACKET_BUFFER_SIZE (HCI_ACL_PAYLOAD_SIZE + 8)
#define RX_QUEUE_DEPTH                  16

#define MAX_NR_HCI_CONNECTIONS          1
#define MAX_NR_L2CAP_CHANNELS           2
#define MAX_NR_L2CAP_SERVICES           1

#endif
