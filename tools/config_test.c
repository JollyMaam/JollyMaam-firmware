/*
 * Host check for the settings engine (src/config.c, src/config_schema.c).
 *
 *     make config-test
 *
 * Runs the engine against an in-memory syscfg: defaults, text in and out,
 * the sparse flash format and what a reader does with records it does not
 * like, the commit rules, and the derived addresses.  It never touches the
 * pad.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

/* ------------------------------------------------------------ the stubs */

#define ITEMS 256
static u8 flash_data[ITEMS][256];
static int flash_length[ITEMS];
static int flash_writes[ITEMS];
static u8 flash_uid[16] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                            0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xf0, 0x01 };
static int have_uid = 1;
static int verbose;

int syscfg_read(u16 item, void *buf, u16 len)
{
    if (!flash_length[item]) return -1;
    if (flash_length[item] > len) return -2;
    memcpy(buf, flash_data[item], flash_length[item]);
    return flash_length[item];
}

int syscfg_write(u16 item, void *buf, u16 len)
{
    memcpy(flash_data[item], buf, len);
    flash_length[item] = len;
    flash_writes[item]++;
    return len;
}

void get_random_number(u8 *ptr, u8 len)
{
    while (len--) *ptr++ = (u8)rand();
}

void cyclone2_usb_debug(const char *format, ...)
{
    va_list arguments;

    if (!verbose) return;
    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    putchar('\n');
}

u8 *get_norflash_uuid(void)
{
    return have_uid ? flash_uid : NULL;
}

/* -------------------------------------------------------------- helpers */

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static struct cfg_leaf leaf(const char *path)
{
    struct cfg_leaf found;

    if (cfg_leaf_find(config_items, config_item_count, path, &found) != 0) {
        printf("FAIL no leaf %s\n", path);
        exit(1);
    }
    return found;
}

static const char *set(const char *path, const char *value)
{
    struct cfg_leaf found = leaf(path);
    const char *error = cfg_set(&config, &found, value);

    return error ? error : cfg_check(&config, &found);
}

static const char *get(const char *path)
{
    static char text[128];
    struct cfg_leaf found = leaf(path);

    cfg_format(found.item, cfg_leaf_data(&config, &found), text, sizeof(text));
    return text;
}

static u32 now;

/* Let the engine see the change, then time pass. */
static void settle(void)
{
    config_poll(now, 1);
    now += 150;
    config_poll(now, 1);
}

void config_flash_uid_forget(void);

static void reboot(void)
{
    config_flash_uid_forget();
    memset(&config, 0xa5, sizeof(config));
    config_init();
}

static void writes_reset(void)
{
    memset(flash_writes, 0, sizeof(flash_writes));
}

static int total_writes(void)
{
    int sum = 0;

    for (int item = 0; item < ITEMS; item++) sum += flash_writes[item];
    return sum;
}

/* ---------------------------------------------------------------- tests */

extern u16 config_dropped_records;
extern u16 config_schema_errors;

static void test_defaults(void)
{
    struct cfg_leaf found;

    reboot();
    CHECK(config_schema_errors == 0);
    CHECK(config.autocommit == 1);
    CHECK(!strcmp(get("profile.0.report_hz"), "auto"));
    CHECK(!strcmp(get("log.hcidump"), "false"));
    CHECK(!strcmp(get("profile.0.type"), "switch"));
    CHECK(!strcmp(get("profile.Y.grips"), "lightbar"));
    CHECK(!strcmp(get("profile.2.type"), "generic"));
    CHECK(!strcmp(get("profile.3.type"), "switch2"));
    CHECK(!strcmp(get("profile.1.bond.peer"), "none"));
    CHECK(!strcmp(get("cal.stick.1.minimum"), "5632,6720"));
    CHECK(!strcmp(get("cal.stick.0.center"), "16384,16384"));
    CHECK(!strcmp(get("profile.A.colour"), "000005"));
    CHECK(!strcmp(get("profile.B.colour"), "050000"));
    CHECK(!strcmp(get("profile.X.colour"), "030005"));
    CHECK(!strcmp(get("profile.Y.colour"), "000500"));
    CHECK(!strcmp(get("led.idle_colour"), "050505"));
    CHECK(!strcmp(get("led.battery_colour"), "050200"));
    CHECK(cfg_leaf_find(config_items, config_item_count,
                        "led.brightness", &found) == -1);
    CHECK(cfg_leaf_find(config_items, config_item_count,
                        "led.grips", &found) == -1);
    CHECK(cfg_leaf_find(config_items, config_item_count,
                        "profile.Y.lightbar", &found) == -1);
    CHECK(!config_pending());
}

static void test_text(void)
{
    reboot();
    CHECK(set("profile.1.bond.peer", "D0:56:80:f0:ab:ec") == NULL);
    CHECK(!strcmp(get("profile.1.bond.peer"), "d0:56:80:f0:ab:ec"));
    CHECK(config.profile[1].bond.peer[0] == 0xec && config.profile[1].bond.peer[5] == 0xd0);
    CHECK(set("profile.1.bond.peer", "d0:56:80:f0:ab") != NULL);
    CHECK(set("profile.1.bond.peer", "none") == NULL);
    CHECK(!strcmp(get("profile.1.bond.peer"), "none"));
    CHECK(set("profile.0.bond.link_key", "00112233445566778899aabbccddeeff") == NULL);
    CHECK(!strcmp(get("profile.0.bond.link_key"), "00112233445566778899aabbccddeeff"));
    CHECK(set("profile.0.bond.link_key", "0011") != NULL);
    CHECK(set("profile.0.report_hz", "250") == NULL && config.profile[0].report_hz == 250);
    CHECK(set("profile.0.report_hz", "251") != NULL && config.profile[0].report_hz == 250);
    CHECK(set("profile.0.type", "switch") == NULL && config.profile[0].report_hz == 250);
    CHECK(cfg_set_note() == NULL);              /* same type: kept */
    CHECK(set("profile.0.type", "dualsense") == NULL && config.profile[0].report_hz == 0);
    CHECK(cfg_set_note() != NULL);              /* new type: back to auto */
    CHECK(!strcmp(get("profile.0.report_hz"), "auto"));
    CHECK(set("profile.0.type", "switch") == NULL);
    CHECK(set("profile.2.type", "xboxble") == NULL && !strcmp(get("profile.2.type"), "xboxble"));
    CHECK(set("profile.2.type", "dualshock4") == NULL && !strcmp(get("profile.2.type"), "dualshock4"));
    CHECK(set("profile.2.type", "ps5") != NULL);
    CHECK(set("profile.0.rumble", "off") == NULL && config.profile[0].rumble == 0);
    CHECK(set("profile.0.rumble", "maybe") != NULL);
    CHECK(set("profile.Y.grips", "none") == NULL &&
          config.profile[3].grips == CONFIG_GRIPS_NONE);
    CHECK(set("profile.Y.grips", "reactive") == NULL &&
          config.profile[3].grips == CONFIG_GRIPS_BOTH);
    CHECK(set("profile.Y.grips", "reactive-input") == NULL &&
          config.profile[3].grips == CONFIG_GRIPS_INPUT);
    CHECK(set("profile.Y.grips", "reactive-rumble") == NULL &&
          config.profile[3].grips == CONFIG_GRIPS_RUMBLE);
    CHECK(set("profile.Y.grips", "lightbar") == NULL &&
          config.profile[3].grips == CONFIG_GRIPS_LIGHTBAR);
    CHECK(set("profile.Y.grips", "rumble") != NULL);
    CHECK(set("profile.Y.grips", "input") != NULL);
    CHECK(set("profile.Y.grips", "both") != NULL);
    CHECK(set("profile.Y.grips", "maybe") != NULL);
    CHECK(set("profile.0.bond.key_type", "9") != NULL);
    CHECK(set("profile.0.bond.key_type", "0x4") == NULL && config.profile[0].bond.key_type == 4);
    CHECK(set("cal.stick.0.deadzone_inner", "10") == NULL);
    CHECK(!strcmp(get("cal.stick.0.deadzone_inner"), "10"));
    CHECK(set("cal.stick.0.deadzone_inner", "31") != NULL);
    CHECK(set("cal.stick.0.deadzone_outer", "30") == NULL);
    CHECK(set("cal.stick.0.deadzone_inner", "30") != NULL);   /* 60% together */
    CHECK(set("cal.stick.1.minimum", "100, 200") == NULL);
    CHECK(!strcmp(get("cal.stick.1.minimum"), "100,200"));
    CHECK(set("cal.stick.1.minimum", "100") != NULL);
    CHECK(set("cal.stick.1.minimum", "100,200,300") != NULL);
    CHECK(set("profile.B.colour", "#FF8000") == NULL &&
          config.profile[1].colour[0] == 0xff && config.profile[1].colour[1] == 0x80 &&
          config.profile[1].colour[2] == 0x00);
    CHECK(!strcmp(get("profile.B.colour"), "ff8000"));
    CHECK(set("profile.B.colour", "cyan") == NULL && !strcmp(get("profile.B.colour"), "00ffff"));
    CHECK(set("profile.B.colour", "ff80") != NULL && !strcmp(get("profile.B.colour"), "00ffff"));
    CHECK(set("profile.B.colour", "mauve") != NULL);
    CHECK(set("led.idle_colour", "010203") == NULL &&
          !strcmp(get("led.idle_colour"), "010203"));
    CHECK(set("led.battery_colour", "050300") == NULL &&
          !strcmp(get("led.battery_colour"), "050300"));
}

static void test_store(void)
{
    struct cfg_leaf found;

    memset(flash_length, 0, sizeof(flash_length));
    reboot();
    writes_reset();
    CHECK(set("log.hcidump", "true") == NULL);
    config_poll(now, 1);
    CHECK(total_writes() == 0);                  /* not settled yet */
    now += 50;
    config_poll(now, 1);
    CHECK(total_writes() == 0);
    now += 60;
    config_poll(now, 0);                         /* radio busy */
    CHECK(total_writes() == 0);
    config_poll(now, 1);
    CHECK(flash_writes[48] == 1 && total_writes() == 1);
    CHECK(!config_pending());
    /* 0xC1, key (uid 62 << 3 = 496: f0 03), length 1, value 1 */
    CHECK(flash_length[48] == 5 && flash_data[48][0] == 0xC1 &&
          flash_data[48][1] == 0xF0 && flash_data[48][2] == 0x03 &&
          flash_data[48][3] == 1 && flash_data[48][4] == 1);
    reboot();
    CHECK(config.log.hcidump == 1 && !config_pending());

    /* Back to the default: the record goes. */
    CHECK(set("log.hcidump", "false") == NULL);
    settle();
    CHECK(flash_length[48] == 4 && flash_data[48][1] == 0);

    /* A bond only rewrites its own slot's item. */
    writes_reset();
    CHECK(set("profile.2.bond.peer", "aa:bb:cc:dd:ee:ff") == NULL);
    found = leaf("profile.2.bond.peer");
    CHECK(cfg_leaf_is_pending(&found));
    settle();
    CHECK(flash_writes[36] == 1 && total_writes() == 1);
    reboot();
    CHECK(!strcmp(get("profile.2.bond.peer"), "aa:bb:cc:dd:ee:ff"));

    /* Calibration fits. */
    CHECK(set("cal.stick.0.gate", "30000,30001,30002,30003,30004,30005,30006,30007,"
              "30008,30009,30010,30011,30012,30013,30014,30015") == NULL);
    CHECK(set("cal.stick.1.gate", "30000,30001,30002,30003,30004,30005,30006,30007,"
              "30008,30009,30010,30011,30012,30013,30014,30015") == NULL);
    CHECK(set("cal.stick.0.minimum", "1,2") == NULL);
    CHECK(set("cal.stick.1.maximum", "60000,60001") == NULL);
    CHECK(set("cal.trigger.0.rest", "900") == NULL);
    CHECK(set("cal.trigger.1.deadzone_outer", "5") == NULL);
    settle();
    CHECK(flash_length[49] > 70 && flash_length[49] < 256);
    reboot();
    CHECK(!strcmp(get("cal.stick.1.maximum"), "60000,60001"));
    CHECK(!strcmp(get("cal.trigger.1.deadzone_outer"), "5"));
    CHECK(config.cal.stick[0].gate[15] == 30015);

    CHECK(set("led.idle_colour", "010203") == NULL);
    CHECK(set("led.battery_colour", "050300") == NULL);
    settle();
    reboot();
    CHECK(!strcmp(get("led.idle_colour"), "010203"));
    CHECK(!strcmp(get("led.battery_colour"), "050300"));

    CHECK(set("profile.Y.grips", "reactive-rumble") == NULL);
    settle();
    reboot();
    CHECK(!strcmp(get("profile.Y.grips"), "reactive-rumble"));
}

static void test_autocommit(void)
{
    memset(flash_length, 0, sizeof(flash_length));
    reboot();
    writes_reset();
    CHECK(set("global.autocommit", "false") == NULL);
    CHECK(set("log.hcidump", "true") == NULL);
    settle();
    CHECK(total_writes() == 0);                  /* not even autocommit */
    reboot();
    CHECK(config.autocommit == 1 && config.log.hcidump == 0);

    CHECK(set("global.autocommit", "false") == NULL);
    CHECK(set("log.hcidump", "true") == NULL);
    CHECK(config_commit(0) == 0 && config_commit_queued());
    config_poll(now, 0);
    CHECK(total_writes() == 0);                  /* waits for the radio */
    now += 150;
    config_poll(now, 1);
    CHECK(total_writes() == 1 && !config_commit_queued());
    reboot();
    CHECK(config.autocommit == 0 && config.log.hcidump == 1);

    /* Turning it back on writes what is pending. */
    CHECK(set("profile.1.type", "xbox") == NULL);
    settle();
    CHECK(total_writes() == 1);
    CHECK(set("global.autocommit", "true") == NULL);
    settle();
    reboot();
    CHECK(config.autocommit == 1 && config.profile[1].type == 3);

    /* unset config keeps autocommit; factory reset does not. */
    CHECK(set("global.autocommit", "off") == NULL);
    config_reset_all(1);
    CHECK(config.autocommit == 0 && config.log.hcidump == 0);
    config_reset_all(0);
    CHECK(config.autocommit == 1);
    CHECK(config_commit(1) == 1);
    CHECK(flash_length[48] == 4 && flash_data[48][1] == 0);
}

static void test_damage(void)
{
    static const u8 prefs[] = {
        0xC1,
        0xF8, 0x7F, 2, 1, 2,        /* unknown uid 2047: skipped */
        0xF0, 0x03, 2, 1, 0,        /* log.hcidump, wrong length */
        0xB0, 0x04, 1, 4,           /* retired led.brightness: ignored */
        (10 << 3) | 1, 1, 99,       /* profile.1.type = 99: not a type */
        (10 << 3) | 7, 1, 2,        /* profile.right.type = switch2: fine */
        0xA8, 0x01, 6, 1, 2, 3, 4, 5, 6, /* profile.0.bond.peer, in PREFS */
        (11 << 3) | 2, 1, 0,        /* profile.2.rumble = false: fine */
        0xF0, 0x03, 1, 1,           /* log.hcidump = true: fine */
        (10 << 3) | 0, 9,           /* truncated */
    };

    memset(flash_length, 0, sizeof(flash_length));
    memcpy(flash_data[48], prefs, sizeof(prefs));
    flash_length[48] = sizeof(prefs);
    memcpy(flash_data[49], "\x00\x01\x02", 3);  /* old format: ignored */
    flash_length[49] = 3;
    config_dropped_records = 0;
    reboot();
    CHECK(config_dropped_records == 5);
    CHECK(config.profile[7].type == 2);
    CHECK(config.profile[2].rumble == 0);
    CHECK(config.log.hcidump == 1);
    CHECK(config.profile[1].type == 0);
    CHECK(!strcmp(get("cal.stick.0.center"), "16384,16384"));
    CHECK(!strcmp(get("profile.A.colour"), "000005"));
    CHECK(!strcmp(get("profile.X.colour"), "030005"));
    CHECK(!strcmp(get("led.idle_colour"), "050505"));
    CHECK(!strcmp(get("led.battery_colour"), "050200"));
    CHECK(!strcmp(get("profile.Y.grips"), "lightbar"));
    CHECK(!config_pending());
}

static void test_addresses(void)
{
    u8 first[6], second[6], third[6], again[6];

    memset(flash_length, 0, sizeof(flash_length));
    reboot();
    config_slot_address(0, first);
    config_slot_address(1, second);
    CHECK(memcmp(first, second, 6));
    CHECK((first[5] & 0x03) == 0x02);            /* local, individual */
    config.profile[0].type = 3;
    config_slot_address(0, third);
    CHECK(memcmp(first, third, 6));
    config.profile[0].type = 0;
    config_slot_address(0, again);
    CHECK(!memcmp(first, again, 6));
    CHECK(set("profile.0.address", "d0:56:80:f0:ab:ec") == NULL);
    config_slot_address(0, third);
    CHECK(third[0] == 0xec && third[5] == 0xd0);

    /* --fresh: same upper half, new lower half, stored; a chain. */
    config_slot_address_fresh(0);
    config_slot_address(0, again);
    CHECK(!memcmp(again + 3, third + 3, 3) && memcmp(again, third, 3));
    CHECK(!memcmp(again, config.profile[0].address, 6));
    config_slot_address_fresh(0);
    config_slot_address(0, second);
    CHECK(!memcmp(second + 3, third + 3, 3) && memcmp(second, again, 3) &&
          memcmp(second, third, 3));
    /* Stateless: the same address always moves on to the same next one. */
    CHECK(set("profile.0.address", "d0:56:80:f0:ab:ec") == NULL);
    config_slot_address_fresh(0);
    CHECK(!memcmp(config.profile[0].address, again, 6));
    /* An unset address starts from the derived one, and is stored. */
    CHECK(set("profile.1.address", "none") == NULL);
    config_slot_address(1, third);
    config_slot_address_fresh(1);
    CHECK(!memcmp(config.profile[1].address + 3, third + 3, 3) &&
          memcmp(config.profile[1].address, third, 3));
    CHECK(set("profile.1.address", "none") == NULL);

    /* Another seed, other addresses. */
    CHECK(set("profile.0.address", "none") == NULL);
    CHECK(set("identity.seed", "0102030405060708090a0b0c0d0e0f10") == NULL);
    config_slot_address(0, third);
    CHECK(memcmp(first, third, 6));

    /* No flash ID: a seed is drawn and kept. */
    have_uid = 0;
    memset(flash_length, 0, sizeof(flash_length));
    reboot();
    CHECK(config_pending());
    settle();
    CHECK(!config_pending());
    memcpy(third, config.identity_seed, 6);
    reboot();
    CHECK(!memcmp(third, config.identity_seed, 6) && !config_pending());
    have_uid = 1;
}

static void test_paths(void)
{
    struct cfg_leaf found;
    char path[48];
    u32 position = 0;
    int count = 0;

    CHECK(cfg_leaf_find(config_items, config_item_count, "profile.3.bond.ltk", &found) == 0);
    CHECK(found.index == 3);
    CHECK(cfg_leaf_find(config_items, config_item_count, "profile.8.type", &found) != 0);
    CHECK(cfg_leaf_find(config_items, config_item_count, "profile.Up.type", &found) == 0 &&
          found.index == 4);
    CHECK(cfg_leaf_find(config_items, config_item_count, "profile.right.variant", &found) == 0 &&
          found.index == 7);
    /* Slots by chord button, sticks and triggers by side; any case. */
    CHECK(cfg_leaf_find(config_items, config_item_count, "profile.X.type", &found) == 0 &&
          found.index == 2);
    CHECK(cfg_leaf_find(config_items, config_item_count, "profile.y.bond.peer", &found) == 0 &&
          found.index == 3);
    cfg_leaf_path(&found, path, sizeof(path));
    CHECK(!strcmp(path, "profile.Y.bond.peer"));
    CHECK(cfg_leaf_under(&found, "profile.Y") && cfg_leaf_under(&found, "profile.3") &&
          cfg_leaf_under(&found, "PROFILE.y.bond") && !cfg_leaf_under(&found, "profile.X") &&
          !cfg_leaf_under(&found, "profile.Y.bo"));
    CHECK(cfg_leaf_find(config_items, config_item_count, "cal.stick.right.center", &found) == 0 &&
          found.index == 1);
    CHECK(cfg_leaf_find(config_items, config_item_count, "cal.trigger.L2.rest", &found) == 0 &&
          found.index == 0);
    CHECK(cfg_leaf_find(config_items, config_item_count, "profile.Z.type", &found) != 0);
    CHECK(cfg_leaf_find(config_items, config_item_count, "profile", &found) != 0);
    while (cfg_leaf_next(config_items, config_item_count, &position, &found)) {
        cfg_leaf_path(&found, path, sizeof(path));
        CHECK(strchr(path, '#') == NULL);
        count++;
    }
    CHECK(count == 7 + 8 * 17 + 2 * 6 + 2 * 4);
}

/* Variants belong to types; the d-pad slots store their bonds apart. */
static void test_slots(void)
{
    config_reset_all(0);
    CHECK(!strcmp(get("profile.up.type"), "stickmouse"));
    CHECK(!strcmp(get("profile.down.type"), "gyromouse"));
    CHECK(!strcmp(get("profile.left.variant"), "desktop"));
    CHECK(!strcmp(get("profile.right.variant"), "desktop"));
    CHECK(!strcmp(get("profile.A.variant"), "default"));
    CHECK(set("profile.A.variant", "desktop") != NULL);
    CHECK(set("profile.up.variant", "desktop") == NULL);
    CHECK(set("profile.up.type", "switch") == NULL);
    CHECK(!strcmp(get("profile.up.variant"), "default"));

    memset(flash_length, 0, sizeof(flash_length));
    CHECK(set("profile.right.bond.peer", "11:22:33:44:55:66") == NULL);
    CHECK(config_commit(1) == 1);
    CHECK(flash_length[55] > 0 && flash_length[41] == 0);
    reboot();
    CHECK(!strcmp(get("profile.right.bond.peer"), "11:22:33:44:55:66"));
    CHECK(!strcmp(get("profile.up.type"), "switch"));
    config_reset_all(0);
}

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    test_defaults();
    test_text();
    test_store();
    test_autocommit();
    test_damage();
    test_addresses();
    test_paths();
    test_slots();
    if (failures) {
        printf("%d check%s failed\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("config: all checks passed\n");
    return 0;
}
