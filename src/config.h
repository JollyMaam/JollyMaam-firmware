#ifndef CYCLONE2_CONFIG_H
#define CYCLONE2_CONFIG_H

#include <stddef.h>
#include "generic/typedef.h"
#include "calibration_math.h"

#define CONFIG_PROFILES 4U

/* One bond per profile slot.  Addresses are least significant byte first,
 * as the stack keeps them.  All zeroes means "none". */
struct config_bond {
    u8 peer[6];
    u8 link_key[16];
    u8 key_type;
    u8 le_peer[6];
    u8 ltk[16];
};

struct config_profile {
    u8 type;
    u8 rumble;
    u8 address[6];
    struct config_bond bond;
};

struct config_cal {
    struct cal_stick stick[2];
    struct cal_trigger trigger[2];
};

struct config_log {
    u8 console;
    u8 btstack;
    u8 hcidump;
};

struct config {
    u8 autocommit;
    u8 report_ms;
    struct config_log log;
    u8 identity_seed[16];
    struct config_profile profile[CONFIG_PROFILES];
    struct config_cal cal;
};

extern struct config config;

enum cfg_type {
    CFG_BOOL,
    CFG_UINT,
    CFG_ENUM,
    CFG_MAC,        /* aa:bb:cc:dd:ee:ff, most significant first; "none" */
    CFG_HEX,
    CFG_LIST,
    CFG_PERCENT,
};

enum cfg_apply {
    CFG_LIVE,
    CFG_NEXT_CONNECT,
    CFG_REBOOT,
};

enum cfg_group {
    CFG_GROUP_PREFS,
    CFG_GROUP_CAL,
    CFG_GROUP_BOND,
    CFG_GROUP_NONE,
};

struct cfg_choice {
    u32 value;
    const char *name;
};

struct cfg_item {
    u16 uid;                /* never reused; a leaf that changes type gets a new one */
    const char *path;
    u8 type;
    u8 group;
    u8 apply;
    u8 count;

    const char *const *names;
    u8 elements;
    u16 offset;
    u16 size;
    u16 stride;
    u32 min, max;
    const struct cfg_choice *choices;
    u32 def;
    const void *def_ptr;

    const char *(*check)(const struct config *config, u8 index);

    void (*derived)(u8 index, u8 *out);

    void (*get)(void *out);
    const char *help;
};

#define CFG_ITEM_SIZE(type, member) sizeof(((type *)0)->member)

extern const char *const cfg_slot_names[];
extern const char *const cfg_stick_names[];
extern const char *const cfg_trigger_names[];
#define config_slot_name(slot) \
    ((slot) < CONFIG_PROFILES ? cfg_slot_names[(slot)] : "?")

#define CFG_FIELD(member) \
    .offset = offsetof(struct config, member), \
    .size = CFG_ITEM_SIZE(struct config, member)

#define CFG_PROFILE_FIELD(member) \
    .offset = offsetof(struct config, profile[0].member), \
    .size = CFG_ITEM_SIZE(struct config, profile[0].member), \
    .stride = sizeof(struct config_profile), .count = CONFIG_PROFILES, \
    .names = cfg_slot_names

#define CFG_STICK_FIELD(member) \
    .offset = offsetof(struct config, cal.stick[0].member), \
    .size = CFG_ITEM_SIZE(struct config, cal.stick[0].member), \
    .stride = sizeof(struct cal_stick), .count = 2, .names = cfg_stick_names

#define CFG_TRIGGER_FIELD(member) \
    .offset = offsetof(struct config, cal.trigger[0].member), \
    .size = CFG_ITEM_SIZE(struct config, cal.trigger[0].member), \
    .stride = sizeof(struct cal_trigger), .count = 2, \
    .names = cfg_trigger_names

#define CFG_STATUS_FIELD(type, member, getter) \
    .offset = offsetof(type, member), .size = CFG_ITEM_SIZE(type, member), \
    .get = (getter), .group = CFG_GROUP_NONE

extern const struct cfg_item config_items[];
extern const u16 config_item_count;
extern const struct cfg_item status_items[];
extern const u16 status_item_count;

void config_init(void);

/* Write what is pending when that is allowed: autocommit (or a requested
 * commit), nothing changed for CONFIG_SETTLE_MS, and `radio_quiet` - the
 * caller's word that no pairing or radio transition is under way.  From the
 * application task's poll. */
void config_poll(u32 now_ms, int radio_quiet);

int config_commit(int radio_quiet);
int config_commit_queued(void);

int config_pending(void);

void config_defaults(struct config *out);

/* Everything back to its default.  keep_autocommit leaves config.autocommit
 * alone (`unset config`); a factory reset does not. */
void config_reset_all(int keep_autocommit);

const u8 *config_flash_uid(void);

void config_slot_address(u8 slot, u8 address[6]);
void config_derived_address(u8 slot, u8 address[6]);

void config_slot_address_fresh(u8 slot);

#define config_peer(slot) (config.profile[(slot) % CONFIG_PROFILES].bond.peer)

void config_bond_clear(u8 slot);

struct cfg_leaf {
    const struct cfg_item *item;
    u8 index;
};

void cfg_leaf_path(const struct cfg_leaf *leaf, char *out, u32 size);

int cfg_leaf_under(const struct cfg_leaf *leaf, const char *prefix);

int cfg_leaf_find(const struct cfg_item *items, u16 count, const char *path,
                  struct cfg_leaf *leaf);

int cfg_leaf_next(const struct cfg_item *items, u16 count, u32 *position,
                  struct cfg_leaf *leaf);

u8 *cfg_leaf_data(struct config *base, const struct cfg_leaf *leaf);
void cfg_leaf_default(const struct cfg_leaf *leaf, u8 *out);
int cfg_leaf_is_default(const struct config *base, const struct cfg_leaf *leaf);
int cfg_leaf_is_pending(const struct cfg_leaf *leaf);

void cfg_leaf_format(const struct config *base, const struct cfg_leaf *leaf,
                     char *out, u32 size, int *derived);

void cfg_format(const struct cfg_item *item, const u8 *value, char *out,
                u32 size);
const char *cfg_parse(const struct cfg_item *item, const char *text, u8 *out);

const char *cfg_set(struct config *target, const struct cfg_leaf *leaf,
                    const char *text);
const char *cfg_check(const struct config *target, const struct cfg_leaf *leaf);

void cfg_status_format(const struct cfg_leaf *leaf, char *out, u32 size);

const char *cfg_choice_name(const struct cfg_item *item, u32 value);

const char *cfg_type_name(const struct cfg_item *item);
const char *cfg_apply_name(u8 apply);

#endif
