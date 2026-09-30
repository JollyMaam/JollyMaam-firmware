#include "config.h"
#include "profile_types.h"

/* 66 Hz is a Pro Controller's own rate on a Switch (15 ms). */
static const struct cfg_choice tx_powers[] = {
    { CONFIG_TX_POWER_NORMAL, "normal" }, { CONFIG_TX_POWER_HIGH, "high" },
    { 0, NULL },
};

static const struct cfg_choice report_rates[] = {
    { 0, "auto" }, { 66, "66" }, { 100, "100" }, { 125, "125" },
    { 250, "250" }, { 500, "500" }, { 1000, "1000" }, { 0, NULL },
};

/* A new type is a new host relationship: its rate starts from the type's
 * default again. */
static const char *type_changed(struct config *c, u8 index)
{
    struct config_profile *profile = &c->profile[index];
    int variant = !config_variant_fits(profile->type, profile->variant);

    if (variant) profile->variant = CONFIG_VARIANT_DEFAULT;
    if (!profile->report_hz)
        return variant ? "variant back to default" : NULL;
    profile->report_hz = 0U;
    return variant ? "report_hz back to auto, variant back to default" :
                     "report_hz back to auto";
}

static const struct cfg_choice variants[] = {
    { CONFIG_VARIANT_DEFAULT, "default" },
    { CONFIG_VARIANT_DESKTOP, "desktop" },
    { 0, NULL },
};

/* Every type has its default layout; the mouse types also a desktop one. */
int config_variant_fits(u8 type, u8 variant)
{
    if (variant == CONFIG_VARIANT_DEFAULT) return 1;
    return variant == CONFIG_VARIANT_DESKTOP &&
           (type == CYCLONE2_PROFILE_STICKMOUSE ||
            type == CYCLONE2_PROFILE_GYROMOUSE);
}

static const char *check_variant(const struct config *c, u8 index)
{
    return config_variant_fits(c->profile[index].type,
                               c->profile[index].variant) ?
           NULL : "the slot's type has no such variant";
}

static const struct cfg_choice profile_types[] = {
    { CYCLONE2_PROFILE_SWITCH, "switch" },
    { CYCLONE2_PROFILE_SWITCH2, "switch2" },
    { CYCLONE2_PROFILE_XBOX, "xbox" },
    { CYCLONE2_PROFILE_XBOX_BLE, "xboxble" },
    { CYCLONE2_PROFILE_DUALSENSE, "dualsense" },
    { CYCLONE2_PROFILE_DUALSHOCK4, "dualshock4" },
    { CYCLONE2_PROFILE_GENERIC, "generic" },
    { CYCLONE2_PROFILE_STICKMOUSE, "stickmouse" },
    { CYCLONE2_PROFILE_GYROMOUSE, "gyromouse" },
    { 0, NULL },
};

static const u8 default_profile_types[CONFIG_PROFILES] = {
    CYCLONE2_PROFILE_SWITCH, CYCLONE2_PROFILE_SWITCH,
    CYCLONE2_PROFILE_GENERIC, CYCLONE2_PROFILE_SWITCH2,
    CYCLONE2_PROFILE_STICKMOUSE, CYCLONE2_PROFILE_GYROMOUSE,     /* up, down */
    CYCLONE2_PROFILE_STICKMOUSE, CYCLONE2_PROFILE_GYROMOUSE,     /* left, right */
};

static const u8 default_profile_variants[CONFIG_PROFILES] = {
    CONFIG_VARIANT_DEFAULT, CONFIG_VARIANT_DEFAULT,
    CONFIG_VARIANT_DEFAULT, CONFIG_VARIANT_DEFAULT,
    CONFIG_VARIANT_DEFAULT, CONFIG_VARIANT_DEFAULT,
    CONFIG_VARIANT_DESKTOP, CONFIG_VARIANT_DESKTOP,
};

/* Direct LED channel values, close to the old 2% default output. */
static const u8 default_profile_colours[CONFIG_PROFILES][3] = {
    { 0x00, 0x00, 0x05 },       /* A blue */
    { 0x05, 0x00, 0x00 },       /* B red */
    { 0x03, 0x00, 0x05 },       /* X purple */
    { 0x00, 0x05, 0x00 },       /* Y green */
    { 0x00, 0x04, 0x04 },       /* up cyan */
    { 0x04, 0x04, 0x00 },       /* down yellow */
    { 0x04, 0x00, 0x03 },       /* left pink */
    { 0x02, 0x04, 0x00 },       /* right lime */
};
static const u8 default_idle_colour[3] = { 0x05, 0x05, 0x05 };
static const u8 default_battery_colour[3] = { 0x05, 0x02, 0x00 };

static const struct cfg_choice grip_modes[] = {
    { CONFIG_GRIPS_NONE, "none" },
    { CONFIG_GRIPS_RUMBLE, "reactive-rumble" },
    { CONFIG_GRIPS_INPUT, "reactive-input" },
    { CONFIG_GRIPS_BOTH, "reactive" },
    { CONFIG_GRIPS_LIGHTBAR, "lightbar" },
    { 0, NULL },
};

/* Uncalibrated sticks: live sensor endpoints with a nominal 0x4000 centre,
 * measured on the first unit.  The vertical low endpoints differ slightly. */
static const u16 default_stick_minimum[2][2] = {{0x1600, 0x1850},
                                                {0x1600, 0x1a40}};

/* Dead zones are fractions of the stick's reach; the two of them together
 * must leave half of it. */
static const char *check_stick_deadzones(const struct config *c, u8 index)
{
    const struct cal_stick *stick = &c->cal.stick[index];

    return (u32)stick->inner_deadzone + stick->outer_deadzone > 16383U ?
           "inner and outer dead zones together must be at most 50%" : NULL;
}

static const char *check_trigger_deadzones(const struct config *c, u8 index)
{
    const struct cal_trigger *trigger = &c->cal.trigger[index];

    return (u32)trigger->inner_deadzone + trigger->outer_deadzone > 16383U ?
           "inner and outer dead zones together must be at most 50%" : NULL;
}

/* Retired uids, never to be reused: 2 (global.report_ms),
 * 14 (profile.#.lightbar), 70 (led.brightness), 71 (led.grips). */
const struct cfg_item config_items[] = {

    { .uid = 1, .path = "global.autocommit", .type = CFG_BOOL, CFG_FIELD(autocommit),
      .group = CFG_GROUP_PREFS, .def = 1,
      .help = "write changes to flash by themselves; when off nothing is "
              "written until `config commit`, and a reboot drops the changes" },
    { .uid = 3, .path = "identity.seed", .type = CFG_HEX,
      CFG_FIELD(identity_seed), .group = CFG_GROUP_PREFS,
      .apply = CFG_NEXT_CONNECT,
      .help = "what the pad's Bluetooth addresses are derived from; none = "
              "the flash chip's unique ID" },

    { .uid = 60, .path = "log.console", .type = CFG_BOOL, CFG_FIELD(log.console),
      .group = CFG_GROUP_PREFS, .def = 1,
      .help = "print log lines on the console" },
    { .uid = 61, .path = "log.btstack", .type = CFG_BOOL, CFG_FIELD(log.btstack),
      .group = CFG_GROUP_PREFS, .def = 1,
      .help = "BTstack's own messages (errors; the build has no info level) "
              "and the port's BTSTACK: lines" },
    { .uid = 62, .path = "log.hcidump", .type = CFG_BOOL, CFG_FIELD(log.hcidump),
      .group = CFG_GROUP_PREFS,
      .help = "trace every HCI packet - a lot of output; stays on across a "
              "reboot, which is how to trace one" },

    { .uid = 72, .path = "led.idle_colour", .type = CFG_COLOUR,
      CFG_FIELD(led.idle_colour), .group = CFG_GROUP_PREFS,
      .def_ptr = default_idle_colour,
      .help = "Home colour during boot and idle; rrggbb sets its brightness" },
    { .uid = 73, .path = "led.battery_colour", .type = CFG_COLOUR,
      CFG_FIELD(led.battery_colour), .group = CFG_GROUP_PREFS,
      .def_ptr = default_battery_colour,
      .help = "Home colour for low battery and the USB-host charging cue; "
              "rrggbb sets brightness" },

    { .uid = 10, .path = "profile.#.type", .type = CFG_ENUM,
      CFG_PROFILE_FIELD(type), .group = CFG_GROUP_PREFS,
      .apply = CFG_NEXT_CONNECT, .choices = profile_types,
      .def_ptr = default_profile_types, .changed = type_changed,
      .help = "what the slot pretends to be; stickmouse and gyromouse are a "
              "Bluetooth keyboard and mouse aimed with a stick, or with the "
              "gyro and a stick" },
    { .uid = 18, .path = "profile.#.variant", .type = CFG_ENUM,
      CFG_PROFILE_FIELD(variant), .group = CFG_GROUP_PREFS,
      .choices = variants, .def_ptr = default_profile_variants,
      .check = check_variant,
      .help = "which layout of its type the slot uses.  Mouse types: default "
              "is for games (left stick WASD, right stick aims, M scrolls); "
              "desktop has the left stick point, the right stick scroll, A "
              "and B click, M slow the pointer.  Other types: default only.  "
              "The tables are in src/profile_mouse.c" },
    { .uid = 12, .path = "profile.#.report_hz", .type = CFG_ENUM,
      CFG_PROFILE_FIELD(report_hz), .group = CFG_GROUP_PREFS,
      .choices = report_rates,
      .help = "input reports per second; auto is what the slot's type uses "
              "with its real console.  `status report.hz` is what is sent" },
    { .uid = 26, .path = "profile.#.in_flight", .type = CFG_UINT,
      CFG_PROFILE_FIELD(in_flight), .group = CFG_GROUP_PREFS,
      .apply = CFG_NEXT_CONNECT, .min = 1, .max = 10, .def = 2,
      .help = "input reports that may wait in the Bluetooth controller at "
              "once (it has 10 Classic and 4 LE buffers).  Few keep reports "
              "fresh; more keep the link busy through retransmissions" },
    { .uid = 28, .path = "profile.#.tx_power", .type = CFG_ENUM,
      CFG_PROFILE_FIELD(tx_power), .group = CFG_GROUP_PREFS,
      .apply = CFG_NEXT_CONNECT, .choices = tx_powers,
      .def = CONFIG_TX_POWER_NORMAL,
      .help = "transmit power: normal is the controller's highest normal "
              "step; high is its production-test mode (radio supply 1.40 V "
              "instead of 1.30 V, pro power table), for range at some "
              "battery cost" },
    { .uid = 29, .path = "profile.#.edr3", .type = CFG_BOOL,
      CFG_PROFILE_FIELD(edr3), .group = CFG_GROUP_PREFS,
      .apply = CFG_NEXT_CONNECT, .def = 1,
      .help = "Classic: allow 3 Mbit/s packets, as the stock pad does; a "
              "DualShock 4 or DualSense report then fits one packet instead "
              "of two.  They need a stronger signal than 2 Mbit/s ones" },
    { .uid = 13, .path = "profile.#.colour", .type = CFG_COLOUR,
      CFG_PROFILE_FIELD(colour), .group = CFG_GROUP_PREFS,
      .def_ptr = default_profile_colours,
      .help = "the slot's Home and player colour, and grip effect colour; "
              "rrggbb sets brightness" },
    { .uid = 11, .path = "profile.#.rumble", .type = CFG_BOOL,
      CFG_PROFILE_FIELD(rumble), .group = CFG_GROUP_PREFS, .def = 1,
      .help = "let the host drive the rumble motors" },
    { .uid = 15, .path = "profile.#.grips", .type = CFG_ENUM,
      CFG_PROFILE_FIELD(grips), .group = CFG_GROUP_PREFS,
      .choices = grip_modes, .def = CONFIG_GRIPS_LIGHTBAR,
      .help = "what drives the grip lights while connected: lightbar shows "
              "host colour if supported; reactive-rumble follows rumble, "
              "reactive-input follows controls, reactive combines both; "
              "none keeps them dark" },

    { .uid = 16, .path = "profile.#.stick_speed", .type = CFG_UINT,
      CFG_PROFILE_FIELD(stick_speed), .group = CFG_GROUP_PREFS,
      .min = 100, .max = 10000, .def = 1500,
      .help = "mouse types: pointer counts per second with the right stick "
              "all the way over (slower near the centre)" },
    { .uid = 17, .path = "profile.#.gyro_speed", .type = CFG_UINT,
      CFG_PROFILE_FIELD(gyro_speed), .group = CFG_GROUP_PREFS,
      .min = 1, .max = 200, .def = 32,
      .help = "gyromouse: pointer counts per degree turned; dualsense and "
              "dualshock4: touchpad points per degree while M holds a finger "
              "on it (1920 across)" },
    { .uid = 19, .path = "profile.#.gyro_accel", .type = CFG_UINT,
      CFG_PROFILE_FIELD(gyro_accel), .group = CFG_GROUP_PREFS,
      .min = 100, .max = 800, .def = 250,
      .help = "gyro acceleration: percent of gyro_speed when turning fast "
              "(150 degrees a second and up; full speed below 20); 100 is "
              "none.  Lets a flick cross the screen while slow aim stays fine" },

    { .uid = 20, .path = "profile.#.address", .type = CFG_MAC,
      CFG_PROFILE_FIELD(address), .group = CFG_GROUP_BOND,
      .apply = CFG_NEXT_CONNECT, .derived = config_derived_address,
      .help = "the pad's Bluetooth address in this slot.  Unset, it is derived "
              "from the flash ID (identity.seed), the slot and its type; "
              "a pairing pins it, and `pair SLOT --fresh` moves it on" },

    { .uid = 21, .path = "profile.#.bond.peer", .type = CFG_MAC,
      CFG_PROFILE_FIELD(bond.peer), .group = CFG_GROUP_BOND,
      .apply = CFG_NEXT_CONNECT,
      .help = "the Classic host the slot reconnects to" },
    { .uid = 22, .path = "profile.#.bond.link_key", .type = CFG_HEX,
      CFG_PROFILE_FIELD(bond.link_key), .group = CFG_GROUP_BOND,
      .apply = CFG_NEXT_CONNECT,
      .help = "the Classic link key, as the stack keeps it" },
    { .uid = 23, .path = "profile.#.bond.key_type", .type = CFG_UINT,
      CFG_PROFILE_FIELD(bond.key_type), .group = CFG_GROUP_BOND,
      .apply = CFG_NEXT_CONNECT, .max = 8,
      .help = "the link key's type (4 = unauthenticated P-192, what a "
              "console pairing gives)" },
    { .uid = 24, .path = "profile.#.bond.le_peer", .type = CFG_MAC,
      CFG_PROFILE_FIELD(bond.le_peer), .group = CFG_GROUP_BOND,
      .apply = CFG_NEXT_CONNECT,
      .help = "the Switch 2 console the slot reconnects to" },
    { .uid = 25, .path = "profile.#.bond.ltk", .type = CFG_HEX,
      CFG_PROFILE_FIELD(bond.ltk), .group = CFG_GROUP_BOND,
      .apply = CFG_NEXT_CONNECT,
      .help = "the Switch 2 LE long-term key" },

    { .uid = 40, .path = "cal.stick.#.minimum", .type = CFG_LIST,
      CFG_STICK_FIELD(minimum), .elements = 2, .group = CFG_GROUP_CAL,
      .def_ptr = default_stick_minimum,
      .help = "raw sensor value at the low end, x,y (0 = left stick)" },
    { .uid = 41, .path = "cal.stick.#.center", .type = CFG_LIST,
      CFG_STICK_FIELD(center), .elements = 2, .group = CFG_GROUP_CAL,
      .def = 0x4000, .help = "raw sensor value at rest, x,y" },
    { .uid = 42, .path = "cal.stick.#.maximum", .type = CFG_LIST,
      CFG_STICK_FIELD(maximum), .elements = 2, .group = CFG_GROUP_CAL,
      .def = 0x6600, .help = "raw sensor value at the high end, x,y" },
    { .uid = 43, .path = "cal.stick.#.gate", .type = CFG_LIST,
      CFG_STICK_FIELD(gate), .elements = CAL_GATE_KNOTS,
      .group = CFG_GROUP_CAL, .def = CAL_STICK_MAX,
      .help = "how far the stick reaches, every 22.5 degrees from +x "
              "(32767 = full scale)" },
    { .uid = 44, .path = "cal.stick.#.deadzone_inner", .type = CFG_PERCENT,
      CFG_STICK_FIELD(inner_deadzone), .group = CFG_GROUP_CAL, .max = 30,
      .check = check_stick_deadzones,
      .help = "centre dead zone, % of the stick's reach" },
    { .uid = 45, .path = "cal.stick.#.deadzone_outer", .type = CFG_PERCENT,
      CFG_STICK_FIELD(outer_deadzone), .group = CFG_GROUP_CAL, .max = 30,
      .check = check_stick_deadzones,
      .help = "outer band that already reads full scale, %" },
    { .uid = 50, .path = "cal.trigger.#.rest", .type = CFG_UINT,
      CFG_TRIGGER_FIELD(rest), .group = CFG_GROUP_CAL, .max = 0x3ff,
      .help = "raw ADC value released (0 = not calibrated: the trigger "
              "tracks its own rest position; 0 = L2)" },
    { .uid = 51, .path = "cal.trigger.#.full", .type = CFG_UINT,
      CFG_TRIGGER_FIELD(full), .group = CFG_GROUP_CAL, .max = 0x3ff,
      .help = "raw ADC value pressed all the way" },
    { .uid = 52, .path = "cal.trigger.#.deadzone_inner", .type = CFG_PERCENT,
      CFG_TRIGGER_FIELD(inner_deadzone), .group = CFG_GROUP_CAL, .max = 30,
      .check = check_trigger_deadzones,
      .help = "travel ignored at the start, %" },
    { .uid = 53, .path = "cal.trigger.#.deadzone_outer", .type = CFG_PERCENT,
      CFG_TRIGGER_FIELD(outer_deadzone), .group = CFG_GROUP_CAL, .max = 30,
      .check = check_trigger_deadzones,
      .help = "travel at the end that already reads full, %" },
};

const u16 config_item_count = sizeof(config_items) / sizeof(config_items[0]);
