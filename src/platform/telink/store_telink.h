#ifndef STORE_TELINK_H
#define STORE_TELINK_H

/* Shared with the host settings-capacity check. One sector is live; the
 * other is the compaction destination, not additional live capacity. */
#define TELINK_STORE_SECTOR_SIZE 0x1000u
#define TELINK_STORE_HEADER_SIZE 8u
#define TELINK_STORE_KEY_MAX 64u
#define TELINK_STORE_DATA_MAX 248u
#define TELINK_STORE_RECORD_SIZE(n) (((n) + 9u) & ~3u)
#define TELINK_STORE_RECEIVER_KEY 62u
#define TELINK_STORE_RECEIVER_SIZE 8u
#define TELINK_STORE_LAST_SLOT_KEY 63u
#define TELINK_STORE_LAST_SLOT_SIZE 4u

#endif
