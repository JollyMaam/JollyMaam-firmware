#include "config.h"
#include "profile_types.h"

/* 66 Hz is a Pro Controller's own rate on a Switch (15 ms). */
static const struct cfg_choice report_rates[] = {
    { 0, "auto" }, { 66, "66" }, { 100, "100" }, { 125, "125" },
    { 250, "250" }, { 500, "500" }, { 1000, "1000" }, { 0, NULL },
};

/* A new type is a new host relationship: its rate starts from the type's
 * default again. */
static const char *type_changed(struct config *c, u8 index)
{
    if (!c->profile[index].report_hz) return NULL;
    c->profile[index].report_hz = 0U;
    return "report_hz back to auto";
}

static const struct cfg_choice profile_types[] = {
    { CYCLONE2_PROFILE_SWITCH, "switch" },
    { CYCLONE2_PROFILE_SWITCH2, "switch2" },
    { CYCLONE2_PROFILE_XBOX, "xbox" },
    { CYCLONE2_PROFILE_XBOX_BLE, "xboxble" },
    { CYCLONE2_PROFILE_DUALSENSE, "dualsense" },
    { CYCLONE2_PROFILE_GENERIC, "generic" },
    { 0, NULL },
};

static const u8 default_profile_types[CONFIG_PROFILES] = {
    CYCLONE2_PROFILE_SWITCH, CYCLONE2_PROFILE_SWITCH,
    CYCLONE2_PROFILE_GENERIC, CYCLONE2_PROFILE_SWITCH2,
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

/* Retired uids, never to be reused: 2 (global.report_ms). */
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

    { .uid = 10, .path = "profile.#.type", .type = CFG_ENUM,
      CFG_PROFILE_FIELD(type), .group = CFG_GROUP_PREFS,
      .apply = CFG_NEXT_CONNECT, .choices = profile_types,
      .def_ptr = default_profile_types, .changed = type_changed,
      .help = "what the slot pretends to be" },
    { .uid = 12, .path = "profile.#.report_hz", .type = CFG_ENUM,
      CFG_PROFILE_FIELD(report_hz), .group = CFG_GROUP_PREFS,
      .choices = report_rates,
      .help = "input reports per second; auto is what the slot's type uses "
              "with its real console.  `status report.hz` is what is sent" },
    { .uid = 11, .path = "profile.#.rumble", .type = CFG_BOOL,
      CFG_PROFILE_FIELD(rumble), .group = CFG_GROUP_PREFS, .def = 1,
      .help = "let the host drive the rumble motors" },

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
