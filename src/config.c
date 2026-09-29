#include <string.h>

#ifdef CONFIG_HOST_TEST
#include "config_host.h"
#else
#include <system/includes.h>
#include <system/user_cfg.h>
#include "usb.h"
#endif

#include "config.h"

#define CONFIG_FORMAT           0xC1U
#define CONFIG_SETTLE_MS        100U
#define CONFIG_ITEM_MAX         256U
#define CONFIG_ITEM_PREFS       48U
#define CONFIG_ITEM_CAL         49U
#define CONFIG_ITEM_BOND        34U
#define CONFIG_INSTANCES        (2U + CONFIG_PROFILES)
#define LEAF_INDEX_SHIFT        3U

#define log(...) cyclone2_usb_debug("config: " __VA_ARGS__)

struct config config;
static struct config committed;

static u32 changed_ms;
static u8 commit_requested;
static u8 buffer[CONFIG_ITEM_MAX];

u16 config_dropped_records;
u16 config_schema_errors;

static int same_run(const struct cfg_item *first, const struct cfg_item *second)
{
    const char *hash = strchr(first->path, '#');

    return first->count && first->count == second->count && hash &&
           !strncmp(first->path, second->path, (u32)(hash - first->path) + 1U);
}

int cfg_leaf_next(const struct cfg_item *items, u16 count, u32 *position,
                  struct cfg_leaf *leaf)
{
    u32 item = *position >> LEAF_INDEX_SHIFT;
    u32 index = *position & ((1U << LEAF_INDEX_SHIFT) - 1U);
    u32 start;
    u32 end;

    if (item >= count) return 0;
    leaf->item = &items[item];
    leaf->index = (u8)index;
    if (!items[item].count) {
        *position = (item + 1U) << LEAF_INDEX_SHIFT;
        return 1;
    }
    for (start = item; start > 0U && same_run(&items[start - 1U], &items[item]); start--) {
    }
    for (end = item; end + 1U < count && same_run(&items[item], &items[end + 1U]); end++) {
    }
    if (item < end) *position = ((item + 1U) << LEAF_INDEX_SHIFT) | index;
    else if (index + 1U < items[item].count)
        *position = (start << LEAF_INDEX_SHIFT) | (index + 1U);
    else *position = (end + 1U) << LEAF_INDEX_SHIFT;
    return 1;
}

const char *const cfg_slot_names[] = { "A", "B", "X", "Y" };
const char *const cfg_stick_names[] = { "left", "right" };
const char *const cfg_trigger_names[] = { "l2", "r2" };

static void leaf_path(const struct cfg_leaf *leaf, int numeric, char *out,
                      u32 size)
{
    const char *path = leaf->item->path;
    u32 length = 0U;

    while (*path && length + 2U < size) {
        if (*path == '#' && leaf->item->names && !numeric) {
            const char *name = leaf->item->names[leaf->index];

            while (*name && length + 2U < size) out[length++] = *name++;
        } else if (*path == '#') {
            if (leaf->index >= 10U) out[length++] = (char)('0' + leaf->index / 10U);
            out[length++] = (char)('0' + leaf->index % 10U);
        } else {
            out[length++] = *path;
        }
        path++;
    }
    out[length] = '\0';
}

void cfg_leaf_path(const struct cfg_leaf *leaf, char *out, u32 size)
{
    leaf_path(leaf, 0, out, size);
}

static char lower(char letter)
{
    return letter >= 'A' && letter <= 'Z' ? (char)(letter - 'A' + 'a') : letter;
}

static int path_under(const char *path, const char *prefix)
{
    while (*prefix) {
        if (lower(*prefix++) != lower(*path++)) return 0;
    }
    return *path == '\0' || *path == '.' || path[-1] == '.';
}

int cfg_leaf_under(const struct cfg_leaf *leaf, const char *prefix)
{
    char name[48];

    if (!*prefix) return 1;
    leaf_path(leaf, 0, name, sizeof(name));
    if (path_under(name, prefix)) return 1;
    if (!leaf->item->names) return 0;
    leaf_path(leaf, 1, name, sizeof(name));
    return path_under(name, prefix);
}

int cfg_leaf_find(const struct cfg_item *items, u16 count, const char *path,
                  struct cfg_leaf *leaf)
{
    char name[48];
    u32 position = 0U;

    while (cfg_leaf_next(items, count, &position, leaf)) {
        leaf_path(leaf, 0, name, sizeof(name));
        if (path_under(name, path) && strlen(name) == strlen(path)) return 0;
        if (!leaf->item->names) continue;
        leaf_path(leaf, 1, name, sizeof(name));
        if (path_under(name, path) && strlen(name) == strlen(path)) return 0;
    }
    return -1;
}

u8 *cfg_leaf_data(struct config *base, const struct cfg_leaf *leaf)
{
    return (u8 *)base + leaf->item->offset +
           (u32)leaf->index * leaf->item->stride;
}

static void put_uint(u8 *out, u32 size, u32 value)
{
    memcpy(out, &value, size > 4U ? 4U : size);
}

static u32 get_uint(const u8 *value, u32 size)
{
    u32 result = 0U;

    memcpy(&result, value, size > 4U ? 4U : size);
    return result;
}

void cfg_leaf_default(const struct cfg_leaf *leaf, u8 *out)
{
    const struct cfg_item *item = leaf->item;
    u32 index;

    if (item->def_ptr) {
        memcpy(out, (const u8 *)item->def_ptr + (u32)leaf->index * item->size,
               item->size);
    } else if (item->type == CFG_LIST) {
        for (index = 0U; index < item->elements; index++)
            put_uint(out + index * 2U, 2U, item->def);
    } else if (item->type == CFG_MAC || item->type == CFG_HEX) {
        memset(out, 0, item->size);
    } else {
        memset(out, 0, item->size);
        put_uint(out, item->size, item->def);
    }
}

int cfg_leaf_is_default(const struct config *base, const struct cfg_leaf *leaf)
{
    u8 value[CONFIG_ITEM_MAX / 4U];

    cfg_leaf_default(leaf, value);
    return !memcmp(cfg_leaf_data((struct config *)base, leaf), value,
                   leaf->item->size);
}

int cfg_leaf_is_pending(const struct cfg_leaf *leaf)
{
    return memcmp(cfg_leaf_data(&config, leaf), cfg_leaf_data(&committed, leaf),
                  leaf->item->size) != 0;
}

static const char hex_digits[] = "0123456789abcdef";

const char *cfg_choice_name(const struct cfg_item *item, u32 value)
{
    const struct cfg_choice *choice;

    for (choice = item->choices; choice && choice->name; choice++)
        if (choice->value == value) return choice->name;
    return NULL;
}

static int all_zero(const u8 *value, u32 size)
{
    while (size--) if (*value++) return 0;
    return 1;
}

static u32 percent_of(u32 fraction)
{
    return (fraction * 100U + CAL_STICK_MAX / 2U) / CAL_STICK_MAX;
}

static u32 append(char *out, u32 size, u32 length, const char *text)
{
    while (*text && length + 1U < size) out[length++] = *text++;
    out[length] = '\0';
    return length;
}

static u32 append_uint(char *out, u32 size, u32 length, u32 value)
{
    char digits[11];
    u32 count = 0U;

    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value);
    while (count && length + 1U < size) out[length++] = digits[--count];
    out[length] = '\0';
    return length;
}

void cfg_format(const struct cfg_item *item, const u8 *value, char *out,
                u32 size)
{
    u32 length = 0U;
    u32 index;
    const char *name;

    out[0] = '\0';
    switch (item->type) {
    case CFG_BOOL:
        append(out, size, 0U, value[0] ? "true" : "false");
        break;
    case CFG_UINT:
        append_uint(out, size, 0U, get_uint(value, item->size));
        break;
    case CFG_PERCENT:
        append_uint(out, size, 0U, percent_of(get_uint(value, item->size)));
        break;
    case CFG_ENUM:
        name = cfg_choice_name(item, get_uint(value, item->size));
        if (name) append(out, size, 0U, name);
        else append_uint(out, size, 0U, get_uint(value, item->size));
        break;
    case CFG_MAC:
        if (all_zero(value, item->size)) {
            append(out, size, 0U, "none");
            break;
        }
        for (index = 0U; index < 6U && length + 3U < size; index++) {
            u8 byte = value[5U - index];
            if (index) out[length++] = ':';
            out[length++] = hex_digits[byte >> 4];
            out[length++] = hex_digits[byte & 15U];
        }
        out[length] = '\0';
        break;
    case CFG_HEX:
        if (all_zero(value, item->size)) {
            append(out, size, 0U, "none");
            break;
        }
        for (index = 0U; index < item->size && length + 2U < size; index++) {
            out[length++] = hex_digits[value[index] >> 4];
            out[length++] = hex_digits[value[index] & 15U];
        }
        out[length] = '\0';
        break;
    case CFG_LIST:
        for (index = 0U; index < item->elements; index++) {
            if (index) length = append(out, size, length, ",");
            length = append_uint(out, size, length,
                                 get_uint(value + index * 2U, 2U));
        }
        break;
    }
}

static int hex_value(char letter)
{
    if (letter >= '0' && letter <= '9') return letter - '0';
    if (letter >= 'a' && letter <= 'f') return letter - 'a' + 10;
    if (letter >= 'A' && letter <= 'F') return letter - 'A' + 10;
    return -1;
}

static int parse_number(const char **text, u32 *value)
{
    const char *p = *text;
    u32 result = 0U;
    u32 digits = 0U;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        for (p += 2; hex_value(*p) >= 0; p++, digits++) {
            if (result >> 28) return -1;
            result = (result << 4) | (u32)hex_value(*p);
        }
    } else {
        for (; *p >= '0' && *p <= '9'; p++, digits++) {
            u32 digit = (u32)(*p - '0');
            if (result > (0xffffffffU - digit) / 10U) return -1;
            result = result * 10U + digit;
        }
    }
    if (!digits) return -1;
    *text = p;
    *value = result;
    return 0;
}

static u32 uint_limit(const struct cfg_item *item)
{
    if (item->max) return item->max;
    return item->size >= 4U ? 0xffffffffU : (1U << (item->size * 8U)) - 1U;
}

/* Is a stored value one this item can hold?  NULL, or why not. */
static const char *validate(const struct cfg_item *item, const u8 *value)
{
    u32 number = get_uint(value, item->size);

    switch (item->type) {
    case CFG_BOOL:
        return number > 1U ? "not a boolean" : NULL;
    case CFG_UINT:
        return number < item->min || number > uint_limit(item) ?
               "out of range" : NULL;
    case CFG_PERCENT:
        return percent_of(number) > item->max ? "out of range" : NULL;
    case CFG_ENUM:
        return cfg_choice_name(item, number) ? NULL : "not one of the choices";
    default:
        return NULL;
    }
}

static int word_is(const char *text, const char *word)
{
    return !strcmp(text, word);
}

const char *cfg_parse(const struct cfg_item *item, const char *text, u8 *out)
{
    const struct cfg_choice *choice;
    u32 value;
    u32 index;

    memset(out, 0, item->size);
    switch (item->type) {
    case CFG_BOOL:
        if (word_is(text, "true") || word_is(text, "on") ||
            word_is(text, "yes") || word_is(text, "1")) out[0] = 1U;
        else if (!(word_is(text, "false") || word_is(text, "off") ||
                   word_is(text, "no") || word_is(text, "0")))
            return "expected true or false";
        return NULL;
    case CFG_UINT:
    case CFG_PERCENT:
        if (parse_number(&text, &value) != 0 ||
            (*text && !(item->type == CFG_PERCENT && word_is(text, "%"))))
            return "expected a number";
        if (item->type == CFG_PERCENT) {
            if (value > item->max) return "out of range";
            value = (value * CAL_STICK_MAX + 50U) / 100U;
        } else if (value < item->min || value > uint_limit(item)) {
            return "out of range";
        }
        put_uint(out, item->size, value);
        return NULL;
    case CFG_ENUM:
        for (choice = item->choices; choice && choice->name; choice++) {
            if (!strcmp(choice->name, text)) {
                put_uint(out, item->size, choice->value);
                return NULL;
            }
        }
        return "not one of the choices";
    case CFG_MAC:
        if (word_is(text, "none")) return NULL;
        for (index = 0U; index < 6U; index++) {
            int high = hex_value(text[0]);
            int low = high < 0 ? -1 : hex_value(text[1]);

            if (low < 0) return "expected aa:bb:cc:dd:ee:ff";
            out[5U - index] = (u8)(high << 4 | low);
            text += 2;
            if (index < 5U && *text++ != ':') return "expected aa:bb:cc:dd:ee:ff";
        }
        return *text ? "expected aa:bb:cc:dd:ee:ff" : NULL;
    case CFG_HEX:
        if (word_is(text, "none")) return NULL;
        for (index = 0U; index < item->size; index++) {
            int high = hex_value(text[0]);
            int low = high < 0 ? -1 : hex_value(text[1]);

            if (low < 0) return "expected hex bytes";
            out[index] = (u8)(high << 4 | low);
            text += 2;
        }
        return *text ? "too many hex digits" : NULL;
    case CFG_LIST:
        for (index = 0U; index < item->elements; index++) {
            if (index) {
                if (*text++ != ',') return "too few values";
                while (*text == ' ') text++;
            }
            if (parse_number(&text, &value) != 0 || value > 0xffffU)
                return "expected numbers up to 65535, comma separated";
            put_uint(out + index * 2U, 2U, value);
        }
        return *text ? "too many values" : NULL;
    }
    return "unknown type";
}

static const char *set_note;
static void config_changed(void);

const char *cfg_set(struct config *target, const struct cfg_leaf *leaf,
                    const char *text)
{
    u8 value[CONFIG_ITEM_MAX / 4U];
    u8 *data = cfg_leaf_data(target, leaf);
    const char *error = cfg_parse(leaf->item, text, value);

    set_note = NULL;
    if (error) return error;
    if (memcmp(data, value, leaf->item->size)) {
        memcpy(data, value, leaf->item->size);
        config_changed();
        if (leaf->item->changed)
            set_note = leaf->item->changed(target, leaf->index);
    }
    return NULL;
}

const char *cfg_set_note(void)
{
    return set_note;
}

const char *cfg_check(const struct config *target, const struct cfg_leaf *leaf)
{
    return leaf->item->check ? leaf->item->check(target, leaf->index) : NULL;
}

void cfg_leaf_format(const struct config *base, const struct cfg_leaf *leaf,
                     char *out, u32 size, int *derived)
{
    const u8 *value = cfg_leaf_data((struct config *)base, leaf);
    u8 address[6];

    *derived = leaf->item->derived && all_zero(value, leaf->item->size);
    if (*derived) {
        leaf->item->derived(leaf->index, address);
        value = address;
    }
    cfg_format(leaf->item, value, out, size);
}

void cfg_status_format(const struct cfg_leaf *leaf, char *out, u32 size)
{

    static u32 scratch[64];

    memset(scratch, 0, sizeof(scratch));
    leaf->item->get(scratch);
    cfg_format(leaf->item, (const u8 *)scratch + leaf->item->offset +
               (u32)leaf->index * leaf->item->stride, out, size);
}

const char *cfg_type_name(const struct cfg_item *item)
{
    static const char *const names[] = {
        "true/false", "number", "one of", "address", "hex", "list", "percent",
    };

    return item->type < sizeof(names) / sizeof(names[0]) ?
           names[item->type] : "?";
}

const char *cfg_apply_name(u8 apply)
{
    return apply == CFG_NEXT_CONNECT ? "takes effect on the next connect or pair" :
           apply == CFG_REBOOT ? "takes effect after a reboot" :
           "takes effect now";
}

static u8 instance_group(u8 instance)
{
    return instance == 0U ? CFG_GROUP_PREFS :
           instance == 1U ? CFG_GROUP_CAL : CFG_GROUP_BOND;
}

static u8 instance_slot(u8 instance)
{
    return instance >= 2U ? (u8)(instance - 2U) : 0U;
}

static u16 instance_item(u8 instance)
{
    return instance == 0U ? CONFIG_ITEM_PREFS :
           instance == 1U ? CONFIG_ITEM_CAL :
           (u16)(CONFIG_ITEM_BOND + instance - 2U);
}

static int leaf_in_instance(const struct cfg_leaf *leaf, u8 instance)
{
    u8 group = instance_group(instance);

    if (leaf->item->group != group) return 0;
    return group != CFG_GROUP_BOND || leaf->index == instance_slot(instance);
}

static int instance_pending(u8 instance)
{
    struct cfg_leaf leaf;
    u32 position = 0U;

    while (cfg_leaf_next(config_items, config_item_count, &position, &leaf))
        if (leaf_in_instance(&leaf, instance) && cfg_leaf_is_pending(&leaf))
            return 1;
    return 0;
}

int config_pending(void)
{
    return memcmp(&config, &committed, sizeof(config)) != 0;
}

static u32 encode(u8 instance, u8 *out, u32 capacity)
{
    struct cfg_leaf leaf;
    u32 position = 0U;
    u32 length = 0U;

    out[length++] = CONFIG_FORMAT;
    while (cfg_leaf_next(config_items, config_item_count, &position, &leaf)) {
        u32 key;

        if (!leaf_in_instance(&leaf, instance) ||
            cfg_leaf_is_default(&config, &leaf)) continue;
        key = ((u32)leaf.item->uid << LEAF_INDEX_SHIFT) | leaf.index;
        if (length + 5U + leaf.item->size > capacity) {
            config_schema_errors++;
            break;
        }
        while (key >= 0x80U) {
            out[length++] = (u8)(key | 0x80U);
            key >>= 7;
        }
        out[length++] = (u8)key;
        out[length++] = (u8)leaf.item->size;
        memcpy(out + length, cfg_leaf_data(&config, &leaf), leaf.item->size);
        length += leaf.item->size;
    }
    if (length == 1U) {
        out[length++] = 0U;
        out[length++] = 1U;
        out[length++] = 0U;
    }
    return length;
}

static const struct cfg_item *item_by_uid(u32 uid)
{
    u16 index;

    for (index = 0U; index < config_item_count; index++)
        if (config_items[index].uid == uid) return &config_items[index];
    return NULL;
}

static void decode(u8 instance, const u8 *data, u32 length)
{
    u32 offset = 1U;

    if (length == 0U || data[0] != CONFIG_FORMAT) return;
    while (offset < length) {
        struct cfg_leaf leaf;
        u32 key = 0U;
        u32 shift = 0U;
        u32 size;

        do {
            if (offset >= length || shift > 21U) return;
            key |= (u32)(data[offset] & 0x7fU) << shift;
            shift += 7U;
        } while (data[offset++] & 0x80U);
        if (offset >= length) return;
        size = data[offset++];
        if (offset + size > length) return;

        if (!(key >> LEAF_INDEX_SHIFT)) {
            offset += size;
            continue;
        }
        leaf.item = item_by_uid(key >> LEAF_INDEX_SHIFT);
        leaf.index = (u8)(key & ((1U << LEAF_INDEX_SHIFT) - 1U));
        if (!leaf.item || leaf.item->size != size ||
            leaf.index >= (leaf.item->count ? leaf.item->count : 1U) ||
            !leaf_in_instance(&leaf, instance) ||
            validate(leaf.item, data + offset)) {
            config_dropped_records++;
            log("dropped a stored record (key %u, %u bytes)", key, size);
        } else {
            memcpy(cfg_leaf_data(&config, &leaf), data + offset, size);
        }
        offset += size;
    }
}

static void flush(void)
{
    u8 instance;
    u8 written = 0U;

    for (instance = 0U; instance < CONFIG_INSTANCES; instance++) {
        struct cfg_leaf leaf;
        u32 position = 0U;
        u32 length;
        int result;

        if (!instance_pending(instance)) continue;
        /* Encode, write and mark committed as one step: the btstack task
         * writes bonds, and must not change one half-way through. */
        local_irq_disable();
        length = encode(instance, buffer, sizeof(buffer));
        result = syscfg_write(instance_item(instance), buffer, length);
        if (result == (int)length) {
            while (cfg_leaf_next(config_items, config_item_count, &position,
                                 &leaf)) {
                if (leaf_in_instance(&leaf, instance))
                    memcpy(cfg_leaf_data(&committed, &leaf),
                           cfg_leaf_data(&config, &leaf), leaf.item->size);
            }
        }
        local_irq_enable();
        if (result != (int)length) {
            log("writing item %u failed (%d)", instance_item(instance), result);
            continue;
        }
        written++;
    }
    if (written) log("wrote %u group%s to flash", written, written == 1U ? "" : "s");
}

/* Bumped by every change (config_changed()), from any task; the poll only
 * compares it, so an unchanged configuration costs nothing to watch. */
static volatile u32 change_count;
static u32 change_seen;
/* Something may differ from flash: a change since the last write, or the
 * identity seed drawn at boot. */
static u8 check_needed = 1U;

static void config_changed(void)
{
    change_count++;
}

void config_poll(u32 now_ms, int radio_quiet)
{
    u32 count = change_count;

    if (count != change_seen) {
        change_seen = count;
        changed_ms = now_ms;
        check_needed = 1U;
    }
    if (!check_needed && !commit_requested) return;
    if (!(config.autocommit || commit_requested) || !radio_quiet ||
        now_ms - changed_ms < CONFIG_SETTLE_MS) return;
    commit_requested = 0U;
    check_needed = 0U;
    if (config_pending()) flush();
}

int config_commit(int radio_quiet)
{
    if (!radio_quiet) {
        commit_requested = 1U;
        return 0;
    }
    commit_requested = 0U;
    flush();
    return 1;
}

int config_commit_queued(void)
{
    return commit_requested;
}

void config_defaults(struct config *out)
{
    struct cfg_leaf leaf;
    u32 position = 0U;

    memset(out, 0, sizeof(*out));
    while (cfg_leaf_next(config_items, config_item_count, &position, &leaf))
        cfg_leaf_default(&leaf, cfg_leaf_data(out, &leaf));
}

void config_reset_all(int keep_autocommit)
{
    struct cfg_leaf leaf;
    u32 position = 0U;
    u8 autocommit = config.autocommit;

    while (cfg_leaf_next(config_items, config_item_count, &position, &leaf))
        cfg_leaf_default(&leaf, cfg_leaf_data(&config, &leaf));
    if (keep_autocommit) config.autocommit = autocommit;
    config_changed();
    if (!config_flash_uid() && all_zero(config.identity_seed,
                                        sizeof(config.identity_seed)))
        get_random_number(config.identity_seed, sizeof(config.identity_seed));
}

static void check_schema(void)
{
    u16 first;
    u16 second;

    for (first = 0U; first < config_item_count; first++) {
        const struct cfg_item *item = &config_items[first];
        struct cfg_leaf leaf = { item, 0U };
        u8 value[CONFIG_ITEM_MAX / 4U];

        for (second = first + 1U; second < config_item_count; second++) {
            if (config_items[second].uid == item->uid) {
                log("uid %u used twice", item->uid);
                config_schema_errors++;
            }
        }
        if (item->size > sizeof(value) || item->count > 8U ||
            (item->type == CFG_LIST && item->size != item->elements * 2U) ||
            (item->type == CFG_MAC && item->size != 6U) ||
            (item->type == CFG_PERCENT && item->size != 2U) ||
            ((item->type == CFG_UINT || item->type == CFG_ENUM ||
              item->type == CFG_BOOL) && item->size > 4U)) {
            log("%s has the wrong size for its type", item->path);
            config_schema_errors++;
            continue;
        }
        for (leaf.index = 0U; leaf.index < (item->count ? item->count : 1U);
             leaf.index++) {
            cfg_leaf_default(&leaf, value);
            if (validate(item, value)) {
                log("%s: the default is not a valid value", item->path);
                config_schema_errors++;
            }
        }
    }
}

void config_init(void)
{
    u8 instance;

    check_schema();
    config_reset_all(0);
    memset(config.identity_seed, 0, sizeof(config.identity_seed));
    for (instance = 0U; instance < CONFIG_INSTANCES; instance++) {
        int length = syscfg_read(instance_item(instance), buffer, sizeof(buffer));

        if (length > 0) decode(instance, buffer, (u32)length);
    }
    committed = config;
    /* No flash ID and no stored seed: draw one, and keep it (it is then an
     * ordinary pending change). */
    if (!config_flash_uid() &&
        all_zero(config.identity_seed, sizeof(config.identity_seed)))
        get_random_number(config.identity_seed, sizeof(config.identity_seed));
    change_seen = change_count;
    check_needed = 1U;
}

#ifndef CONFIG_HOST_TEST
extern u8 *get_norflash_uuid(void) __attribute__((weak));
#endif

/* Read once (at boot, from config_init()): later calls must not go to the
 * flash while the radio runs. */
static u8 flash_uid_checked;

const u8 *config_flash_uid(void)
{
    static const u8 *uid;

    if (!flash_uid_checked) {
        flash_uid_checked = 1U;
#ifdef CONFIG_HOST_TEST
        uid = get_norflash_uuid();
#else
        uid = get_norflash_uuid ? get_norflash_uuid() : NULL;
#endif
        if (uid && (all_zero(uid, 16U) ||
                    (uid[0] == 0xffU && uid[1] == 0xffU && uid[2] == 0xffU &&
                     uid[3] == 0xffU && uid[4] == 0xffU && uid[5] == 0xffU)))
            uid = NULL;
    }
    return uid;
}

#ifdef CONFIG_HOST_TEST
void config_flash_uid_forget(void)
{
    flash_uid_checked = 0U;
}
#endif

static u32 fnv1a(u32 hash, const u8 *data, u32 length)
{
    while (length--) hash = (hash ^ *data++) * 16777619U;
    return hash;
}

void config_derived_address(u8 slot, u8 address[6])
{
    const struct config_profile *profile = &config.profile[slot % CONFIG_PROFILES];
    const u8 *seed = config.identity_seed;
    u8 salt[3];
    u32 first;
    u32 second;

    if (all_zero(seed, 16U) && config_flash_uid()) seed = config_flash_uid();
    salt[0] = slot;
    salt[1] = profile->type;
    salt[2] = 0U;
    first = fnv1a(fnv1a(2166136261U, seed, 16U), salt, sizeof(salt));
    second = fnv1a(fnv1a(0x811c9dc5U ^ 0x5a5a5a5aU, salt, sizeof(salt)), seed, 16U);
    memcpy(address, &first, 4U);
    memcpy(address + 4U, &second, 2U);
    /* Locally administered, individual (not group).  Least significant byte
     * first, so the octet printed first is address[5]. */
    address[5] = (u8)((address[5] | 0x02U) & 0xfeU);
}

void config_slot_address(u8 slot, u8 address[6])
{
    const u8 *pinned = config.profile[slot % CONFIG_PROFILES].address;

    if (!all_zero(pinned, 6U)) memcpy(address, pinned, 6U);
    else config_derived_address(slot, address);
}

void config_slot_address_fresh(u8 slot)
{
    u8 *address = config.profile[slot % CONFIG_PROFILES].address;
    u32 hash;

    config_slot_address(slot, address);
    config_changed();
    /* Least significant byte first: address[0..2] are the three octets
     * printed last. */
    hash = fnv1a(fnv1a(2166136261U, address, 6U), address, 6U);
    if ((hash & 0xffffffU) == ((u32)address[0] | (u32)address[1] << 8 |
                               (u32)address[2] << 16)) hash ^= 1U;
    address[0] = (u8)hash;
    address[1] = (u8)(hash >> 8);
    address[2] = (u8)(hash >> 16);
}

/* Writers for what the stack and profiles learn at run time.  Every change
 * to `config` goes through this file, so autocommit sees all of them. */

static struct config_bond *bond_of(u8 slot)
{
    return &config.profile[slot % CONFIG_PROFILES].bond;
}

void config_bond_forget_keys(u8 slot)
{
    struct config_bond *bond = bond_of(slot);

    memset(bond->link_key, 0, sizeof(bond->link_key));
    bond->key_type = 0U;
    memset(bond->le_peer, 0, sizeof(bond->le_peer));
    memset(bond->ltk, 0, sizeof(bond->ltk));
    config_changed();
}

void config_bond_set_peer(u8 slot, const u8 peer[6])
{
    if (peer) memcpy(bond_of(slot)->peer, peer, 6U);
    else memset(bond_of(slot)->peer, 0, 6U);
    config_changed();
}

void config_bond_set_link_key(u8 slot, const u8 peer[6], const u8 key[16],
                              u8 type)
{
    struct config_bond *bond = bond_of(slot);

    memcpy(bond->peer, peer, 6U);
    memcpy(bond->link_key, key, 16U);
    bond->key_type = type;
    config_changed();
}

void config_bond_clear_link_key(u8 slot)
{
    struct config_bond *bond = bond_of(slot);

    memset(bond->link_key, 0, sizeof(bond->link_key));
    bond->key_type = 0U;
    config_changed();
}

void config_bond_set_le(u8 slot, const u8 peer[6], const u8 ltk[16])
{
    struct config_bond *bond = bond_of(slot);

    memcpy(bond->le_peer, peer, 6U);
    memcpy(bond->ltk, ltk, 16U);
    config_changed();
}

void config_pin_slot_address(u8 slot, const u8 address[6])
{
    memcpy(config.profile[slot % CONFIG_PROFILES].address, address, 6U);
    config_changed();
}

void config_set_stick_cal(u8 index, const struct cal_stick *stick)
{
    config.cal.stick[index % 2U] = *stick;
    config_changed();
}

void config_set_trigger_cal(u8 index, const struct cal_trigger *trigger)
{
    config.cal.trigger[index % 2U] = *trigger;
    config_changed();
}

void config_set_autocommit(u8 on)
{
    config.autocommit = on ? 1U : 0U;
    config_changed();
}
