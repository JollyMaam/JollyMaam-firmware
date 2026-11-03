#ifndef STORE_H
#define STORE_H

/* core/store.c's record log, shared with the host settings-capacity check
 * (tools/jollymaam/settings_schema.c).  Of its two banks one is live; the
 * other is the compaction destination, not more room. */
#define STORE_SECTOR         0x1000u   /* the erase unit; a bank is whole sectors */
#define STORE_HEADER_SIZE    8u
#define STORE_KEY_MAX        64u       /* keys are 1..STORE_KEY_MAX-1 */
#define STORE_DATA_MAX       248u      /* the most bytes a record holds */
#define STORE_RECORD_SIZE(n) (((n) + 9u) & ~3u)
/* Keys the platforms keep beside the settings' (1..28). */
#define STORE_RECEIVER_KEY   62u
#define STORE_RECEIVER_SIZE  8u
#define STORE_LAST_SLOT_KEY  63u
#define STORE_LAST_SLOT_SIZE 4u

#endif
