#ifndef CYCLONE2_CONFIG_H
#define CYCLONE2_CONFIG_H

#include <stddef.h>
#include "generic/typedef.h"
#include "calibration_math.h"

/*
 * The pad's settings: one typed struct the application reads and writes
 * directly, and a table (config_schema.c) that describes every leaf of it -
 * its name, default, range, when a change takes effect and a line of help.
 * The console, the flash store, export and import all work from that table.
 * plan/08-config.md has the design; docs/config.md the user's view.
 */

#define CONFIG_PROFILES 4U

/* One bond per profile slot.  Addresses are least significant byte first,
 * as the stack keeps them.  All zeroes means "none". */
struct config_bond {
    u8 local[6];        /* the pad's address when the bond was made */
    u8 peer[6];         /* the Classic host this slot pages */
    u8 link_key[16];
    u8 key_type;
    u8 le_peer[6];      /* the Switch 2 console */
    u8 ltk[16];
};

struct config_profile {
    u8 type;            /* enum cyclone2_profile_type */
    u8 rumble;
    u8 generation;      /* bumped by `pair N --fresh`: a new identity */
    struct config_bond bond;
};

struct config_cal {
    struct cal_stick stick[2];
    struct cal_trigger trigger[2];
};

struct config {
    u8 autocommit;
    u8 report_ms;
    u8 identity_seed[16];
    struct config_profile profile[CONFIG_PROFILES];
    struct config_cal cal;
};

extern struct config config;

/* ------------------------------------------------------------ the schema */

enum cfg_type {
    CFG_BOOL,       /* true/false (also on/off, 1/0) */
    CFG_UINT,       /* 1, 2 or 4 bytes, decimal or 0x.. */
    CFG_ENUM,       /* one of .choices */
    CFG_MAC,        /* aa:bb:cc:dd:ee:ff, most significant first; "none" */
    CFG_HEX,        /* bytes as hex, in memory order; "none" */
    CFG_LIST,       /* .elements u16 values, comma separated */
    CFG_PERCENT,    /* u16 fraction of 32767, shown as a whole percent */
};

enum cfg_apply {
    CFG_LIVE,           /* at once */
    CFG_NEXT_CONNECT,   /* the next time the slot connects or pairs */
    CFG_REBOOT,         /* after a reboot */
};

enum cfg_group {
    CFG_GROUP_PREFS,
    CFG_GROUP_CAL,
    CFG_GROUP_BOND,     /* one per profile slot */
    CFG_GROUP_NONE,     /* status items: not stored */
};

struct cfg_choice {
    u32 value;
    const char *name;
};

struct cfg_item {
    u16 uid;                /* never reused; a leaf that changes type gets a new one */
    const char *path;       /* dotted, '#' is the array index */
    u8 type;                /* enum cfg_type */
    u8 group;               /* enum cfg_group */
    u8 apply;               /* enum cfg_apply */
    u8 count;               /* array length; 0 = not an array */
    u8 elements;            /* CFG_LIST: values in the list */
    u16 offset;             /* into struct config (status: into the getter's struct) */
    u16 size;               /* bytes of one value */
    u16 stride;             /* bytes between array elements */
    u32 min, max;           /* CFG_UINT, CFG_PERCENT (percent) */
    const struct cfg_choice *choices;   /* CFG_ENUM, ends with a NULL name */
    u32 def;                /* default: the value, or every element of a list */
    const void *def_ptr;    /* or raw bytes: one value per array index */
    /* Cross-leaf rule, run after a set: an error message, or NULL. */
    const char *(*check)(const struct config *config, u8 index);
    /* Status items: fill the struct the item's offset points into. */
    void (*get)(void *out);
    const char *help;
};

#define CFG_ITEM_SIZE(type, member) sizeof(((type *)0)->member)

/* Where a leaf lives in struct config. */
#define CFG_FIELD(member) \
    .offset = offsetof(struct config, member), \
    .size = CFG_ITEM_SIZE(struct config, member)

/* A leaf of every profile slot: profile.#.member. */
#define CFG_PROFILE_FIELD(member) \
    .offset = offsetof(struct config, profile[0].member), \
    .size = CFG_ITEM_SIZE(struct config, profile[0].member), \
    .stride = sizeof(struct config_profile), .count = CONFIG_PROFILES

#define CFG_STICK_FIELD(member) \
    .offset = offsetof(struct config, cal.stick[0].member), \
    .size = CFG_ITEM_SIZE(struct config, cal.stick[0].member), \
    .stride = sizeof(struct cal_stick), .count = 2

#define CFG_TRIGGER_FIELD(member) \
    .offset = offsetof(struct config, cal.trigger[0].member), \
    .size = CFG_ITEM_SIZE(struct config, cal.trigger[0].member), \
    .stride = sizeof(struct cal_trigger), .count = 2

/* A status leaf: a member of the struct its getter fills. */
#define CFG_STATUS_FIELD(type, member, getter) \
    .offset = offsetof(type, member), .size = CFG_ITEM_SIZE(type, member), \
    .get = (getter), .group = CFG_GROUP_NONE

extern const struct cfg_item config_items[];
extern const u16 config_item_count;
extern const struct cfg_item status_items[];
extern const u16 status_item_count;

/* ------------------------------------------------------------ the engine */

/* Load flash over the defaults.  Once, at boot, before the radio. */
void config_init(void);

/* Write what is pending when that is allowed: autocommit (or a requested
 * commit), nothing changed for CONFIG_SETTLE_MS, and `radio_quiet` - the
 * caller's word that no pairing or radio transition is under way.  From the
 * application task's poll. */
void config_poll(u32 now_ms, int radio_quiet);

/* Ask for everything pending to be written at the next chance, whatever
 * autocommit says.  Returns 1 if it was written at once (radio_quiet), 0 if
 * it is queued. */
int config_commit(int radio_quiet);
int config_commit_queued(void);

/* Anything that differs from flash. */
int config_pending(void);

/* A struct holding only defaults. */
void config_defaults(struct config *out);

/* Everything back to its default.  keep_autocommit leaves config.autocommit
 * alone (`unset config`); a factory reset does not. */
void config_reset_all(int keep_autocommit);

/* The flash chip's unique ID (16 bytes), or NULL when it has none. */
const u8 *config_flash_uid(void);

/* The address a slot uses: its bond's, or the one derived from the seed. */
void config_slot_address(u8 slot, u8 address[6]);

/* The Classic host a slot pages (all zeroes: none). */
#define config_peer(slot) (config.profile[(slot) % CONFIG_PROFILES].bond.peer)

/* Drop a slot's bond (pairing). */
void config_bond_clear(u8 slot);

/* ------------------------------------------------ leaves, for the console */

/* A concrete leaf: an item and, for arrays, an index. */
struct cfg_leaf {
    const struct cfg_item *item;
    u8 index;
};

/* Build "profile.2.type" for a leaf (no namespace prefix). */
void cfg_leaf_path(const struct cfg_leaf *leaf, char *out, u32 size);

/* Look a path up ("profile.2.type"); 0 when found. */
int cfg_leaf_find(const struct cfg_item *items, u16 count, const char *path,
                  struct cfg_leaf *leaf);

/* Iterate every leaf of a table in order: start with *position = 0; returns
 * 0 when there are no more. */
int cfg_leaf_next(const struct cfg_item *items, u16 count, u32 *position,
                  struct cfg_leaf *leaf);

/* Raw access to a leaf's bytes in a struct config. */
u8 *cfg_leaf_data(struct config *base, const struct cfg_leaf *leaf);
void cfg_leaf_default(const struct cfg_leaf *leaf, u8 *out);
int cfg_leaf_is_default(const struct config *base, const struct cfg_leaf *leaf);
int cfg_leaf_is_pending(const struct cfg_leaf *leaf);

/* Text <-> value.  format writes at most size bytes; parse returns NULL on
 * success or an error message. */
void cfg_format(const struct cfg_item *item, const u8 *value, char *out,
                u32 size);
const char *cfg_parse(const struct cfg_item *item, const char *text, u8 *out);

/* Set a leaf of `target` from text; NULL or an error message.  Cross-leaf
 * rules are separate: cfg_check() after one set, or after a whole import. */
const char *cfg_set(struct config *target, const struct cfg_leaf *leaf,
                    const char *text);
const char *cfg_check(const struct config *target, const struct cfg_leaf *leaf);

/* Value of a status leaf, formatted. */
void cfg_status_format(const struct cfg_leaf *leaf, char *out, u32 size);

/* Name of a choice, or NULL. */
const char *cfg_choice_name(const struct cfg_item *item, u32 value);

const char *cfg_type_name(const struct cfg_item *item);
const char *cfg_apply_name(u8 apply);

#endif
