#ifndef JOLLYMAAM_WIRE_H
#define JOLLYMAAM_WIRE_H

/* Shared wire contract. Constants only: no runtime tables or native structs.
 * Multi-byte fields are little-endian; transports frame the logical CDB. */
#define JM_OP 0xf5
#define JM_PROTOCOL 2
#define JM_MAIN_MAGIC 0x4e49414du
#define JM_MAIN_HEAD_SIZE 0x120u
#define JM_MAIN_VERSION 1
#define JM_MAIN_CHECKED_SIZE 44
#define JM_UPDATE_INFO_VERSION 1
#define JM_UPDATE_INFO_SIZE 32

/* Byte offsets, independent of C struct padding and target alignment. */
#define JM_ID_SIZE 56
#define JM_ID_EXT_SIZE 88
#define JM_ID_BUILD_SIZE 16
#define JM_ID_UID_SIZE 16
#define JM_PRODUCT_SIZE 16
#define JM_ID_BOARD_NAME_SIZE 32
#define JM_STATUS_SIZE 16
#define JM_MAIN_CRC_START 4
#define JM_MAIN_CRC_SIZE 36
enum {
  JM_ID_PROTOCOL_OFF = 4,
  JM_ID_BOARD_OFF = 6,
  JM_ID_UPDATER_OFF = 7,
  JM_ID_BUILD_OFF = 8,
  JM_ID_UID_OFF = 24,
  JM_ID_PRODUCT_OFF = 40,
  JM_ID_BOARD_NAME_OFF = 56
};
enum {
  JM_STATUS_STATE_OFF = 0,
  JM_STATUS_SLOT_OFF = 1,
  JM_STATUS_USB_POWERED_OFF = 2,
  JM_STATUS_LOW_BATTERY_OFF = 3,
  JM_STATUS_BATTERY_MV_OFF = 4,
  JM_STATUS_BATTERY_FLAGS_OFF = 6,
  JM_STATUS_BATTERY_PERCENT_OFF = 7,
  JM_STATUS_UPTIME_OFF = 8,
  JM_STATUS_EPHEMERAL_OFF = 12,
  JM_STATUS_STAGED_OFF = 13,
  JM_STATUS_CHANGES_OFF = 14
};
enum {
  JM_UPDATE_INFO_VERSION_OFF = 0,
  JM_UPDATE_INFO_STATE_OFF = 1,
  JM_UPDATE_INFO_ERROR_OFF = 2,
  JM_UPDATE_INFO_VALID_OFF = 3,
  JM_UPDATE_INFO_AT_OFF = 4,
  JM_UPDATE_INFO_ROOM_OFF = 8,
  JM_UPDATE_INFO_WRITTEN_OFF = 12,
  JM_UPDATE_INFO_BEGUN_OFF = 16,
  JM_UPDATE_INFO_EXTRA_OFF = 20,
  JM_UPDATE_INFO_DATA_MAX_OFF = 24
};
enum {
  JM_MAIN_MAGIC_OFF = 0,
  JM_MAIN_SIZE_OFF = 4,
  JM_MAIN_CRC_OFF = 8,
  JM_MAIN_ENTRY_OFF = 12,
  JM_MAIN_FORMAT_OFF = 16,
  JM_MAIN_AT_OFF = 20,
  JM_MAIN_PRODUCT_OFF = 24,
  JM_MAIN_HEADER_CRC_OFF = 40
};

/* LAYOUT: u8 version, entry size, the board's count, entries here, then
 * the entries from the first asked for: u8 input (IN_*), u8 kind, label
 * (8 bytes UTF-8, NUL-padded), u8 x, u8 y. */
#define JM_LAYOUT_VERSION 1
#define JM_LAYOUT_HEAD_SIZE 4
#define JM_LAYOUT_ENTRY_SIZE 12

enum jm_command_id {
  JM_CMD_IDENTIFY = 0x00,
  JM_CMD_STATUS = 0x01,
  JM_CMD_LOG = 0x02,
  JM_CMD_STATS = 0x03,
  JM_CMD_INPUT = 0x04,
  JM_CMD_CLOCK = 0x05,
  JM_CMD_LAYOUT = 0x06,
  JM_CMD_REBOOT = 0x10,
  JM_CMD_REBOOT_LOADER = 0x11,
  JM_CMD_RELINK = 0x12,
  JM_CMD_CONNECT = 0x13,
  JM_CMD_PROFILE = 0x14,
  JM_CMD_PEEK = 0x15,
  JM_CMD_TRACE = 0x16,
  JM_CMD_PAIR = 0x17,
  JM_CMD_CALIBRATE = 0x18,
  JM_CMD_LED_DEMO = 0x19,
  JM_CMD_RUMBLE = 0x1a,
  JM_CMD_DISCONNECT = 0x1b,
  JM_CMD_PROBE = 0x1c,
  JM_CMD_CONFIG = 0x1d,
  JM_CMD_PRESS = 0x1e,
  JM_CMD_FIELD = 0x20,
  JM_CMD_READ = 0x21,
  JM_CMD_WRITE = 0x22,
  JM_CMD_COMMIT = 0x23,
  JM_CMD_DROP = 0x24,
  JM_CMD_EPHEMERAL = 0x25,
  JM_CMD_RESULT = 0x26,
  JM_CMD_DEFAULTS = 0x27,
  JM_CMD_FIELDS = 0x28,
  JM_CMD_TARGET = 0x30,
  JM_CMD_TOOL_ONLY = 0x31,
  JM_CMD_HUB = 0x32,
  JM_CMD_USB_PROBE = 0x3f,
  JM_CMD_UPDATER = 0x40,
  JM_CMD_UPDATE_INFO = 0x41,
  JM_CMD_UPDATE_BEGIN = 0x42,
  JM_CMD_UPDATE_DATA = 0x43,
  JM_CMD_UPDATE_COMMIT = 0x44
};

/* HID request/reply headers exclude the API's report ID byte. */
#define JM_HID_HEAD_SIZE 20            /* the command less its client: the B80's report is this + 336 */
#define JM_HID_CLIENT_HEAD_SIZE 24     /* a command's head: CDB, length, flags, tag, client */
#define JM_HID_INPUT_HEAD_SIZE 8       /* an answer's input report: client, tag, state, offset */
#define JM_HID_REPORT_JIELI 512
#define JM_HID_REPORT_TELINK 356
/* A device whose report is one 64-byte packet (the YC3121's) gathers a
 * host-to-device command's data from several reports, each with the
 * command's head (the length the whole's), up to this. */
#define JM_HID_REPORT_SMALL 64
#define JM_HID_GATHER 512
enum jm_hid_state { JM_HID_IDLE, JM_HID_BUSY, JM_HID_MORE, JM_HID_LAST, JM_HID_REFUSED };
enum jm_hid_flags { JM_HID_OUT = 1, JM_HID_INPUT = 2 };

enum jm_update_state { JM_UPDATE_IDLE, JM_UPDATE_WRITING, JM_UPDATE_COMMITTED };
enum jm_update_error {
  JM_UPDATE_ERR_NONE, JM_UPDATE_ERR_SIZE, JM_UPDATE_ERR_ORDER,
  JM_UPDATE_ERR_NOT_BEGUN, JM_UPDATE_ERR_VERIFY, JM_UPDATE_ERR_INCOMPLETE,
  JM_UPDATE_ERR_HEADER, JM_UPDATE_ERR_CRC, JM_UPDATE_ERR_KEY,
  JM_UPDATE_ERR_BOARD, JM_UPDATE_ERR_LAYOUT
};

enum jm_battery_flags {
  JM_BATTERY_MV_VALID = 1,
  JM_BATTERY_PERCENT_VALID = 2,
  JM_BATTERY_CHARGE_VALID = 4,
  JM_BATTERY_EXTERNAL_POWER = 8,
  JM_BATTERY_CHARGING = 16,
  JM_BATTERY_LOW_VALID = 32,
  JM_BATTERY_LOW = 64,
  JM_BATTERY_FULL = 128
};
#endif
