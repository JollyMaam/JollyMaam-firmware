#include <string.h>
#include "pad.h"

/* Every setting is a row in a field table: a never-reused id, a name, a type,
 * its range, its default (per slot) and a line of help.  The tools read the
 * table over USB (set_describe), so a new row needs no tool update.
 *
 * Each field is either default or set.  Only set fields are stored; reset
 * clears them, so a better default in a new firmware reaches every field
 * that was never set. */

/* T_LIST: size numbers of a byte each, each at most max. */
enum { T_ENUM, T_NUMBER, T_RGB, T_BYTES, T_TEXT, T_ENTRY, T_LIST };
/* The rows the code itself uses. */
enum { ID_TYPE = 1, ID_TRANSPORT = 2, ID_ADDRESS = 8, ID_BOND = 9 };

struct set_field {
    u8 id;                   /* stored and sent; never reused */
    u8 type;
    u16 offset;              /* in the target's struct */
    u8 size;
    u16 max;                 /* T_NUMBER: largest value; T_ENUM: from names */
    const char *name;
    const char *const *names;   /* T_ENUM, NULL-terminated */
    u32 def;                 /* little-endian bytes of the default */
    const u32 *slot_def;     /* slot fields: a default per slot instead; NULL: def */
    const u16 *number_def;   /* T_NUMBER: the board's default instead of def */
    const u8 *list_def;      /* T_LIST: its default numbers */
    const char *help;
    u8 flags;                /* F_DEV: a developer setting (padtool --dev) */
};
#define F_DEV 1

#define RGB24(r, g, b)       ((r) | (g) << 8 | (u32)(b) << 16)
#define PER_SLOT(...)        .slot_def = (const u32[SLOTS]){ __VA_ARGS__ }
#define FIELD(f)             .offset = offsetof(struct set_slot, f), \
                             .size = sizeof(((struct set_slot *)0)->f)

static const char *const types[] = {
    "switch", "switch2", "xbox", "xboxble", "dualsense", "dualshock4", "generic", "mouse",
    "genericble", "none", "dualsenseble", NULL,
};
static const char *const transports[] = { "bluetooth", "usb", "bluetooth+usb", "dongle", "dongle+usb", NULL };
static const char *const grips[] = {
    "off", "host", "fade", "rumble", "input", "rumble+input", "primary", "secondary", "rainbow",
    "heartbeat", NULL,
};
static const char *const players[] = {
    "off", "host", "fade", "primary", "secondary", "rainbow", NULL,
};
static const char *const curve_enabled[] = { "disabled", "enabled", NULL };
static const char *const left_to[] = { "left stick", "right stick", "nothing", NULL };
static const char *const right_to[] = { "right stick", "left stick", "nothing", NULL };
static const char *const axis_way[] = { "normal", "inverted", NULL };
static const char *const remap_names[] = { "base", "a", "b", "x", "y", "up", "down", "left", "right", NULL };
static const u8 linear_points[STICK_CURVE_POINTS] = { 16, 32, 48, 64, 80, 96, 112, 128, 143, 159, 175, 191, 207, 223, 239 };

static const char *const layouts[] = { "games", "desktop", NULL };
static const char *const trigger_modes[] = { "default", "hair", "rapid", NULL };
static const char *const rumble_steps[] = { "off", "1", "2", "3", "4", "5", NULL };   /* RUMBLE_STEPS */
static const char *const phys[] = { "1m", "2m", NULL };
static const char *const onoff[] = { "on", "off", NULL };
static const char *const tx_powers[] = { "1", "2", "3", "4", "5", "6", NULL };

/* Slots A B X Y up down left right.  Colours are palette entries; how
 * bright they are is the palette's. */
static const struct set_field slot_fields[] = {
    { .id = ID_TYPE, .name = "type", .type = T_ENUM, .names = types, FIELD(type),
      .slot_def = board_slots.type,
      .help = "what the slot connects as; changing it renews address and clears bond" },
    { .id = ID_TRANSPORT, .name = "transport", .type = T_ENUM, .names = transports, FIELD(transport),
      .slot_def = board_slots.transport,
      .help = "how: bluetooth; usb, a wired controller; bluetooth+usb, usb while a usb host "
              "is there, else bluetooth (never back to it mid-session); dongle, through our dongle, which is the type on its usb port; "
              "dongle+usb, usb while a usb host is there, else the dongle" },
    { .id = 3, .name = "connect_colour", .type = T_ENTRY, FIELD(connect_colour),
      PER_SLOT(5, 11, 7, 1, 9, 13, 3, 15),
      .help = "Home and player lights while connecting and pairing: which slot" },
    { .id = 4, .name = "primary_colour", .type = T_ENTRY, FIELD(primary_colour),
      .def = 1,
      .help = "Home while connected; the player lights around the player number" },
    { .id = 5, .name = "secondary_colour", .type = T_ENTRY, FIELD(secondary_colour),
      .def = 7,
      .help = "the console's player number; resting grips; the other end of fade" },
    { .id = 6, .name = "grips", .type = T_ENUM, .names = grips, FIELD(grips),
      .def = GRIPS_OFF,
      .help = "grip lights while connected: host is the console's lightbar colour; fade, rumble, "
              "input, rainbow and heartbeat are animations, lit brighter than the steady colours" },
    { .id = 7, .name = "players", .type = T_ENUM, .names = players, FIELD(players),
      .def = PLAYERS_HOST,
      .help = "player lights while connected: host is the console's player number; fade and "
              "rainbow are animations, lit brighter than the steady colours" },
    { .id = ID_ADDRESS, .name = "address", .type = T_BYTES, FIELD(address),
      .help = "the slot's own address, least significant byte first; zero: derived" },
    { .id = ID_BOND, .name = "bond", .type = T_BYTES, FIELD(bond),
      .help = "the paired host: peer, link key, key type, LE peer, LTK" },
    /* The link, for measuring it; next connect. */
    { .id = 10, .flags = F_DEV, .name = "report_hz", .type = T_NUMBER, .max = 1000, FIELD(report_hz),
      .def = 0, .help = "most reports per second, Classic and LE, whole milliseconds apart; "
                             "reports are built when the radio has room; 0: 250" },
    { .id = 11, .flags = F_DEV, .name = "le_interval_min", .type = T_NUMBER, .max = 3200,
      FIELD(link.le_interval_min), .def = 0,
      .help = "LE: connection interval to ask for, 1.25 ms units (6 = 7.5 ms); 0: the profile's" },
    { .id = 12, .flags = F_DEV, .name = "le_interval_max", .type = T_NUMBER, .max = 3200,
      FIELD(link.le_interval_max), .def = 0,
      .help = "LE: the longest interval to accept; 0: le_interval_min" },
    { .id = 13, .flags = F_DEV, .name = "le_phy", .type = T_ENUM, .names = phys, FIELD(link.le_phy),
      .def = 0, .help = "LE: 2m asks the host for the 2M PHY" },
    { .id = 14, .flags = F_DEV, .name = "in_flight", .type = T_NUMBER, .max = 8, FIELD(link.in_flight),
      .def = 2, .help = "input reports in the controller at once; 0: 2" },
    { .id = 15, .flags = F_DEV, .name = "poll_us", .type = T_NUMBER, .max = 40000, FIELD(link.poll_us),
      .def = 3750, .help = "Classic: Best Effort poll request in us (3750 minimum, rounded down to 1250 steps); 0: none" },
    { .id = 16, .flags = F_DEV, .name = "pull_delay_us", .type = T_NUMBER, .max = 5000, FIELD(pull_delay_us),
      .def = 0, .help = "build a report this long after the radio asks for it (fresher, "
                             "until it misses the connection event)" },
    { .id = 17, .flags = F_DEV, .name = "jit_us", .type = T_NUMBER, .max = 10000, FIELD(link.jit_us),
      .def = 0, .help = "build a report this long before the connection event or sniff "
                             "anchor it goes out at; 0: adaptive; 1: as soon as the radio has room" },
    { .id = 18, .flags = F_DEV, .name = "edr3", .type = T_ENUM, .names = onoff, FIELD(link.edr3_off),
      .def = 1, .help = "Classic: 3 Mbit/s packets (an 83-byte PlayStation report fits "
                             "one, but a Linux host stalled more often); off: 2 Mbit/s only" },
    { .id = 19, .flags = F_DEV, .name = "sniff_max", .type = T_NUMBER, .max = 800, FIELD(link.sniff_max),
      .def = 0, .help = "Classic: ask the host for sniff, at most this many 0.625 ms "
                             "slots (even); 0: take what the host chooses" },
    { .id = 20, .flags = F_DEV, .name = "sniff_min", .type = T_NUMBER, .max = 800, FIELD(link.sniff_min),
      .def = 0, .help = "Classic: the shortest sniff interval asked for; 0: sniff_max" },
    { .id = 21, .name = "tx_power", .type = T_ENUM, .names = tx_powers, FIELD(link.tx_power),
      .def = 4, .help = "radio strength 1-6: 1 is about 20 dB below 5; 6 about 3 dB "
                             "above 5, using more battery" },
    { .id = 22, .name = "remap", .type = T_ENUM, .names = remap_names, FIELD(remap),
      .def = 0, .help = "the remap each connect starts with: base, or a remap slot (which takes what it "
                             "has not set from base); Home+M then a slot key picks another until a "
                             "disconnect, another slot or sleep" },
};

/* The palette target: the colours, and how bright each kind of light is. */
struct set_palette {
    struct rgb entry[PALETTE];
    struct set_brightness brightness;
};

#define ENTRY(i, r, g, b) { .id = (i) + 1, .name = "palette" #i, .type = T_RGB, \
  .offset = 3 * (i), .size = 3, .def = RGB24(r, g, b) }
#define BRIGHTNESS(f)     .offset = offsetof(struct set_palette, brightness.f), \
                          .size = sizeof(((struct set_palette *)0)->brightness.f)

/* Full brightness: white, then rainbow order from red through pink.
 * Idle uses white (0); battery uses orange (3), selected in settings_ux.c.
 * Brightness in tenths of the LEDs' smallest step: 10 is 1 of 255. */
static const struct set_field palette_fields[] = {
    ENTRY(0, 255, 255, 255), /* white */
    ENTRY(1, 255, 0, 0),    /* red */
    ENTRY(2, 255, 51, 0),   /* vermilion */
    ENTRY(3, 255, 102, 0),  /* orange */
    ENTRY(4, 255, 191, 0),  /* amber */
    ENTRY(5, 255, 255, 0),  /* yellow */
    ENTRY(6, 128, 255, 0),  /* lime */
    ENTRY(7, 0, 255, 0),    /* green */
    ENTRY(8, 0, 255, 128),  /* mint */
    ENTRY(9, 0, 255, 255),  /* cyan */
    ENTRY(10, 0, 128, 255), /* azure */
    ENTRY(11, 0, 0, 255),   /* blue */
    ENTRY(12, 77, 0, 255),  /* indigo */
    ENTRY(13, 153, 0, 255), /* violet */
    ENTRY(14, 255, 0, 255), /* magenta */
    ENTRY(15, 255, 0, 128), /* pink */
    { .id = 17, .name = "steady_brightness", .type = T_NUMBER, .max = 2550, BRIGHTNESS(steady),
      .number_def = &board_led_brightness.steady, .help = "Home and the player lights while connected, and steady grips: tenths of "
                         "the LEDs' smallest step (10 is 1 of 255); in-between values need dithering" },
    { .id = 18, .name = "animation_brightness", .type = T_NUMBER, .max = 2550, BRIGHTNESS(animation),
      .number_def = &board_led_brightness.animation, .help = "animated grips and player lights (fade, rumble, input, rainbow, heartbeat), "
                          "in tenths of the LEDs' smallest step" },
    { .id = 19, .name = "connect_brightness", .type = T_NUMBER, .max = 2550, BRIGHTNESS(connect),
      .number_def = &board_led_brightness.connect, .help = "the lights while connecting and pairing, in tenths of the LEDs' smallest step" },
    { .id = 20, .flags = F_DEV, .name = "dither_ms", .type = T_NUMBER, .max = 20, BRIGHTNESS(dither_ms),
      .def = 2, .help = "experiment: refresh dim lights this often (ms), alternating the steps on either "
                        "side, for brightness between them and smooth dim fades; 0 off" },
};

#define TEXT(x) #x
#define NUMBER_TEXT(x) TEXT(x)
#define RULES_TEXT NUMBER_TEXT(REMAP_RULES)
#define REMAP(f) .offset = offsetof(struct set_remap, f), .size = sizeof(((struct set_remap *)0)->f)

_Static_assert(sizeof(struct remap_rule) == 10, "stored rule layout");

/* Remap slots A-right; the tools read and write map and rules as text. */
static const struct set_field remap_fields[] = {
    { .id = 1, .name = "name", .type = T_TEXT, REMAP(name),
      .help = "what this remap is for; UTF-8, up to 24 bytes" },
    { .id = 2, .name = "map", .type = T_BYTES, REMAP(map),
      .help = "each key's base row: while held it sends itself (default), another key or "
              "nothing; its rules add to that" },
    { .id = 3, .name = "rules", .type = T_BYTES, REMAP(rule),
      .help = "up to " RULES_TEXT ": when a key is held, held for, pressed, released, tapped or double-tapped, "
              "hold, turbo, tap or toggle other keys, nothing, or make chosen keys turbo while it is on "
              "(turbo modifier)" },
    /* From here on the second record (REMAP_STICKS): a stick at a time, then
     * rumble, the mouse and the triggers. */
    { .id = 4, .name = "left_maps_to", .type = T_ENUM, .names = left_to, REMAP(maps_to[0]),
      .help = "the host stick the physical left stick drives; if both drive one, the one pushed further wins" },
    { .id = 5, .name = "left_x_axis", .type = T_ENUM, .names = axis_way, REMAP(invert[0][0]),
      .help = "inverted: the left stick's left and right are swapped" },
    { .id = 6, .name = "left_y_axis", .type = T_ENUM, .names = axis_way, REMAP(invert[0][1]),
      .help = "inverted: the left stick's up and down are swapped" },
    { .id = 7, .name = "left_inner_deadzone", .type = T_NUMBER, .max = 25, REMAP(deadzone[0].inner),
      .help = "left stick: percent around centre suppressed" },
    { .id = 8, .name = "left_outer_deadzone", .type = T_NUMBER, .max = 25, REMAP(deadzone[0].outer),
      .help = "left stick: percent of outer travel discarded" },
    { .id = 9, .name = "left_curve_enabled", .type = T_ENUM, .names = curve_enabled, REMAP(left_curve_enabled),
      .help = "enable left stick points; disabled passes calibrated input through exactly and retains the points" },
    { .id = 10, .name = "left_curve_points", .type = T_BYTES, REMAP(left_curve_points),
      .help = "15 nondecreasing output bytes (0-255) at 6.25% input intervals; endpoints fixed at zero and full" },
    { .id = 11, .name = "right_maps_to", .type = T_ENUM, .names = right_to, REMAP(maps_to[1]),
      .help = "the host stick the physical right stick drives; if both drive one, the one pushed further wins" },
    { .id = 12, .name = "right_x_axis", .type = T_ENUM, .names = axis_way, REMAP(invert[1][0]),
      .help = "inverted: the right stick's left and right are swapped" },
    { .id = 13, .name = "right_y_axis", .type = T_ENUM, .names = axis_way, REMAP(invert[1][1]),
      .help = "inverted: the right stick's up and down are swapped" },
    { .id = 14, .name = "right_inner_deadzone", .type = T_NUMBER, .max = 25, REMAP(deadzone[1].inner),
      .help = "right stick: percent around centre suppressed" },
    { .id = 15, .name = "right_outer_deadzone", .type = T_NUMBER, .max = 25, REMAP(deadzone[1].outer),
      .help = "right stick: percent of outer travel discarded" },
    { .id = 16, .name = "right_curve_enabled", .type = T_ENUM, .names = curve_enabled, REMAP(right_curve_enabled),
      .help = "enable right stick points; disabled passes calibrated input through exactly and retains the points" },
    { .id = 17, .name = "right_curve_points", .type = T_BYTES, REMAP(right_curve_points),
      .help = "15 nondecreasing output bytes (0-255) at 6.25% input intervals; endpoints fixed at zero and full" },
    { .id = 18, .name = "rumble", .type = T_ENUM, .names = rumble_steps, REMAP(rumble),
      .def = 3, .help = "how strong the motors are: each step has its own motor ranges and kick "
                        "(developer settings); Home+M then L1/R1 changes it until a disconnect" },
    { .id = 19, .flags = F_DEV, .name = "rumble_left_min", .type = T_LIST, .max = 100, REMAP(rumble_min[0]),
      .list_def = board_rumble.min[0], .help = "left motor, steps 1-5: percent power for the lightest rumble; "
                                            "rumble between this and the max spreads evenly" },
    { .id = 20, .flags = F_DEV, .name = "rumble_left_max", .type = T_LIST, .max = 100, REMAP(rumble_max[0]),
      .list_def = board_rumble.max[0], .help = "left motor, steps 1-5: percent power for full rumble" },
    { .id = 21, .flags = F_DEV, .name = "rumble_right_min", .type = T_LIST, .max = 100, REMAP(rumble_min[1]),
      .list_def = board_rumble.min[1], .help = "right motor, steps 1-5: percent power for the lightest rumble" },
    { .id = 22, .flags = F_DEV, .name = "rumble_right_max", .type = T_LIST, .max = 100, REMAP(rumble_max[1]),
      .list_def = board_rumble.max[1], .help = "right motor, steps 1-5: percent power for full rumble" },
    { .id = 23, .flags = F_DEV, .name = "rumble_kick_ms", .type = T_LIST, .max = 255, REMAP(rumble_kick_ms),
      .list_def = board_rumble.kick_ms, .help = "steps 1-5: a motor starting from stopped runs at full power "
                                         "this long first, so a low minimum still starts it; 0: no kick" },
    { .id = 24, .flags = F_DEV, .name = "mouse_layout", .type = T_ENUM, .names = layouts, REMAP(mouse_layout),
      .def = 0, .help = "mouse: what each control does; from the next connect" },
    { .id = 25, .flags = F_DEV, .name = "stick_speed", .type = T_NUMBER, .max = 20000, REMAP(stick_speed),
      .def = 1500, .help = "mouse: pointer counts per second, stick all the way; from the next connect" },
    { .id = 26, .flags = F_DEV, .name = "gyro_speed", .type = T_NUMBER, .max = 1000, REMAP(gyro_speed),
      .def = 32, .help = "mouse, touchpad: counts per degree turned; 0 off; from the next connect" },
    { .id = 27, .flags = F_DEV, .name = "gyro_accel", .type = T_NUMBER, .max = 1000, REMAP(gyro_accel),
      .def = 250, .help = "mouse, touchpad: percent of gyro_speed for fast turns; from the next connect" },
    { .id = 28, .name = "l2_press", .type = T_NUMBER, .max = 100, REMAP(trigger_press[0]),
      .def = 5, .help = "L2: percent of travel where it counts as pressed (a host's digital trigger, "
                         "hair and rapid); it lets go 3% short of it" },
    { .id = 29, .name = "l2_mode", .type = T_ENUM, .names = trigger_modes, REMAP(trigger_mode[0]),
      .help = "L2: default: the analog value as it is; hair: full from the press point, else nothing; rapid: as hair, "
              "but it lets go as soon as the trigger rises 5% and presses again 5% down, anywhere" },
    { .id = 30, .name = "r2_press", .type = T_NUMBER, .max = 100, REMAP(trigger_press[1]),
      .def = 5, .help = "R2: percent of travel where it counts as pressed (a host's digital trigger, "
                         "hair and rapid); it lets go 3% short of it" },
    { .id = 31, .name = "r2_mode", .type = T_ENUM, .names = trigger_modes, REMAP(trigger_mode[1]),
      .help = "R2: default: the analog value as it is; hair: full from the press point, else nothing; rapid: as hair, "
              "but it lets go as soon as the trigger rises 5% and presses again 5% down, anywhere" },
};
#define REMAP_STICKS 3                    /* remap fields from this index: the second record */
#define LEFT_POINTS  (&remap_fields[9])
#define RIGHT_POINTS (&remap_fields[16])
#define NAME_FIELD  (&remap_fields[0])
#define MAP_FIELD   (&remap_fields[1])
#define RULES_FIELD (&remap_fields[2])
_Static_assert(sizeof(remap_fields) / sizeof(remap_fields[0]) == 31, "LEFT_POINTS, RIGHT_POINTS");
_Static_assert(sizeof(remap_fields) / sizeof(remap_fields[0]) <= 32, "a bit per field in set_bits");

/* What remap slot A is until it is changed; the others start empty.  Map:
 * the key with bit n - 1. */
static const struct set_remap remap_a_default = {
    .name = "Swap A/B X/Y",
    .map = { [0] = 1 + 1, [1] = 0 + 1, [3] = 4 + 1, [4] = 3 + 1 },     /* A=B B=A X=Y Y=X */
};

_Static_assert(sizeof(struct set_cal) == 112, "stored calibration layout");

static const struct set_field cal_fields[] = {
    { .id = 1, .name = "calibration", .type = T_BYTES, .offset = 0,
      .size = sizeof(struct set_cal),
      .help = "sensor calibration for sticks and triggers; legacy stick deadzones ignored" },
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* ---- the values -------------------------------------------------------------
 * A target is a slot, the palette or calibration: a struct of values and a
 * bit per field that is set. */

#define MAGIC      0x4332
#define VERSION    1
#ifndef RECORD_MAX
#define RECORD_MAX 224             /* the largest record of a target kept */
#endif
#define SETTLE_MS  100
/* The tools always see SLOTS slots and SLOTS remap slots (the targets); a
 * board short of RAM keeps fewer of them (make/<product>.mk).  One it does
 * not keep reads as its defaults, takes no writes, and as a slot is of type
 * none. */
#ifndef SLOTS_STORED
#define SLOTS_STORED  SLOTS
#endif
#ifndef REMAPS_STORED
#define REMAPS_STORED SLOTS
#endif
/* A board without remaps keeps no base either; its remap targets read as
 * their defaults, and set_remap() is not there to call. */
#ifndef REMAP_BASE_STORED
#define REMAP_BASE_STORED 1
#endif
_Static_assert(REMAP_BASE_STORED || !REMAPS_STORED, "remap slots inherit from a kept base");
#ifndef STAGE_MAX
#define STAGE_MAX     1024
#endif
/* syscfg items: slots 1-8, palette 9, calibration 10, remap slots 11-18,
 * their sticks 19-26; the base remap 27 and its sticks 28. */
#define KEY_REMAP        11
#define KEY_REMAP_STICKS (KEY_REMAP + SLOTS)
#define KEY_BASE         (KEY_REMAP_STICKS + SLOTS)
#define KEY_BASE_STICKS  (KEY_BASE + 1)
#define KEYS             (KEY_BASE_STICKS + 1)

static struct set_slot slots[SLOTS_STORED];
static const struct set_slot none_slot = { .type = PROFILE_NONE };
static struct set_palette pal;
static struct set_cal cal;
#if REMAP_BASE_STORED
static struct set_remap remap;   /* the one remap held: the rest stay in flash */
static u8 remap_held;            /* its target; 0: none */
#endif
static u32 set_bits[TARGETS];
static u32 dirty;            /* a bit per target */
static u32 tried;            /* a bit per remap target changed in ephemeral mode */
static u32 edits;
static u32 changed_ms;
static u8 commit_now, ephemeral;
static u8 stage[STAGE_MAX];
static u16 staged, staged_items;

static int stored(u8 t)
{
    if (t < SLOTS) return t < SLOTS_STORED;
    if (t >= TARGET_REMAP && t < TARGET_BASE) return t - TARGET_REMAP < REMAPS_STORED;
    if (t == TARGET_BASE) return REMAP_BASE_STORED;
    return t < TARGETS;
}

static const struct set_field *fields_of(u8 t, u32 *count)
{
    if (t < SLOTS) { *count = COUNT(slot_fields); return slot_fields; }
    if (t == TARGET_PALETTE) { *count = COUNT(palette_fields); return palette_fields; }
    if (t >= TARGET_REMAP) { *count = COUNT(remap_fields); return remap_fields; }
    *count = COUNT(cal_fields);
    return cal_fields;
}

static u8 *hold(u8 t);

static u8 *remap_values(u8 t)
{
#if REMAP_BASE_STORED
    return hold(t);
#else
    (void)t;
    return NULL;                 /* none kept: no writes, read as defaults */
#endif
}

static u8 *values_of(u8 t)
{
    return t < SLOTS ? (u8 *)&slots[t] : t == TARGET_PALETTE ? (u8 *)&pal :
           t == TARGET_CAL ? (u8 *)&cal : remap_values(t);
}

/* A target is one record, a remap slot two: its rules, then its sticks. */
static u32 parts_of(u8 t)
{
    return t >= TARGET_REMAP ? 2 : 1;
}

static u32 part_of(u8 t, u32 field)
{
    return t >= TARGET_REMAP && field >= REMAP_STICKS;
}

static u16 key_of(u8 t, u32 part)
{
    if (t < TARGET_REMAP) return (u16)(1 + t);
    if (t == TARGET_BASE) return (u16)(part ? KEY_BASE_STICKS : KEY_BASE);
    return (u16)((part ? KEY_REMAP_STICKS : KEY_REMAP) + t - TARGET_REMAP);
}

static const struct set_field *find(u8 t, u8 id, u32 *bit)
{
    u32 n, i;
    const struct set_field *f = fields_of(t, &n);

    *bit = 0;
    for (i = 0; i < n; i++)
        if (f[i].id == id) {
            *bit = i;
            return &f[i];
        }
    return NULL;
}

/* A field's default in target t (a slot's own, if it has one). */
static u32 field_def(const struct set_field *f, u8 t)
{
    if (f->number_def) return *f->number_def;
    return f->slot_def && t < SLOTS ? f->slot_def[t] : f->def;
}

static u32 enum_count(const struct set_field *f)
{
    u32 n = 0;

    while (f->names[n]) n++;
    return n;
}

static u32 field_max(const struct set_field *f)
{
    return f->type == T_ENUM ? enum_count(f) - 1 : f->type == T_ENTRY ? PALETTE - 1 : f->max;
}

/* Each input a key, itself or none; bit 15 is no key. */
static int map_ok(const u8 *v)
{
    u32 i;

    for (i = 0; i < REMAP_INPUTS; i++)
        if (v[i] != REMAP_OFF && (v[i] > REMAP_INPUTS || v[i] == 15 + 1 || (i == 15 && v[i])))
            return 0;
    return 1;
}

static int rules_ok(const u8 *v)
{
    u32 i;

    for (i = 0; i < REMAP_RULES; i++) {
        const struct remap_rule *r = (const struct remap_rule *)(v + i * sizeof(*r));
        u32 o = r->outputs[0] | r->outputs[1] << 8 | (u32)r->outputs[2] << 16 | (u32)r->outputs[3] << 24;
        int level = r->trigger == RT_HELD || r->trigger == RT_HELD_FOR;
        int timed = r->trigger == RT_HELD_FOR || r->trigger == RT_TAPPED || r->trigger == RT_DOUBLE;

        if (!r->trigger) continue;
        if (r->trigger >= RT_COUNT || r->input >= REMAP_INPUTS || r->input == 15 ||
            r->effect >= RE_COUNT || level != remap_level_effect(r->effect) || (timed && !r->ms10) ||
            (o & ~PAD_DIGITAL) || r->rate > REMAP_MAX_HZ || r->reserved)
            return 0;
    }
    return 1;
}

static u8 check(const struct set_field *f, const u8 *v, u32 len)
{
    u32 x;

    if (!f) return SET_UNKNOWN;
    if (!len) return SET_OK;
    if (len != f->size) return SET_BAD_LENGTH;
    if (f == LEFT_POINTS || f == RIGHT_POINTS) {
        for (x = 1; x < len; x++) if (v[x] < v[x - 1]) return SET_RANGE;
    }
    if (f == MAP_FIELD && !map_ok(v)) return SET_RANGE;
    if (f == RULES_FIELD && !rules_ok(v)) return SET_RANGE;
    switch (f->type) {
    case T_ENUM:
    case T_NUMBER:
        x = f->size == 1 ? v[0] : v[0] | (u32)v[1] << 8;
        return x <= field_max(f) ? SET_OK : SET_RANGE;
    case T_ENTRY:
        return v[0] < PALETTE ? SET_OK : SET_RANGE;
    case T_LIST:
        for (x = 0; x < len; x++) if (v[x] > f->max) return SET_RANGE;
        return SET_OK;
    default:
        return SET_OK;
    }
}

static void touch(u8 t)
{
    if (ephemeral) {
        if (t >= TARGET_REMAP) tried |= 1u << t;
        return;
    }
    dirty |= 1u << t;
    changed_ms = plat_ms();
}

/* The fields remap slot t takes from the base while unset: all but its
 * name, and but A's map and rules (its swap). */
static u32 inherited(u8 t)
{
    if (t < TARGET_REMAP || t >= TARGET_BASE || !stored(TARGET_BASE)) return 0;
    return t == TARGET_REMAP ? ~0u << REMAP_STICKS : ~1u;
}

/* A field's default but for what a remap slot takes from the base. */
static void own_default(u8 t, const struct set_field *f, u8 *out)
{
    u32 def = field_def(f, t);

    memset(out, 0, f->size);
    memcpy(out, &def, f->size < 4 ? f->size : 4);
    if (f == LEFT_POINTS || f == RIGHT_POINTS)
        memcpy(out, linear_points, sizeof(linear_points));
    if (f->list_def)
        memcpy(out, f->list_def, f->size);
    if (t == TARGET_REMAP && f < &remap_fields[REMAP_STICKS])
        memcpy(out, (const u8 *)&remap_a_default + f->offset, f->size);
}

static void base_value(const struct set_field *f, u8 *out);

/* The reset path and read-only USB defaults query share these bytes. */
static void default_value(u8 t, const struct set_field *f, u8 *out)
{
    if (t >= TARGET_REMAP && (inherited(t) >> (f - remap_fields) & 1)) base_value(f, out);
    else own_default(t, f, out);
}

static int in_ram(u8 t);

/* Ephemeral changes live only in RAM: only the remap held (the one in use)
 * takes them. */
static int ephemeral_refuses(u8 t)
{
    return ephemeral && !in_ram(t);
}

/* The remap slot held follows the base in its unset fields. */
static void inherit(void)
{
    u32 i;
    u8 t;

    for (t = TARGET_REMAP; t < TARGET_BASE && stored(t); t++)
        for (i = 0; in_ram(t) && i < COUNT(remap_fields); i++)
            if (!(set_bits[t] & (1u << i)))
                default_value(t, &remap_fields[i], values_of(t) + remap_fields[i].offset);
}

/* A field back to its default (len 0) or set to v. */
static void put(u8 t, const struct set_field *f, u32 bit, const u8 *v, u32 len)
{
    u8 *p = values_of(t) + f->offset;

    if (len) {
        memset(p, 0, f->size);
        memcpy(p, v, len < f->size ? len : f->size);
        set_bits[t] |= 1u << bit;
    } else {
        default_value(t, f, p);
        set_bits[t] &= ~(1u << bit);
    }
}

static void reset(u8 t)
{
    u32 n, i;
    const struct set_field *f = fields_of(t, &n);

    for (i = 0; i < n; i++) put(t, &f[i], i, NULL, 0);
}

static void clear(u8 t, u8 id)
{
    u32 bit;
    const struct set_field *f = find(t, id, &bit);

    put(t, f, bit, NULL, 0);
}

static void set_slot_new_address(u8 slot, u8 out[6]);

static void renew_identity(u8 slot)
{
    u8 address[6];
    u32 bit;
    const struct set_field *f = find(slot, ID_ADDRESS, &bit);

    set_slot_new_address(slot, address);
    clear(slot, ID_BOND);
    put(slot, f, bit, address, sizeof(address));
}

/* Id 0 from outside keeps pairing unless it changes the host identity. */
static void reset_keeping_pairing(u8 t)
{
    u32 n, i;
    const struct set_field *f = fields_of(t, &n);
    u8 type, transport;

    if (t >= SLOTS) {
        reset(t);
        return;
    }
    type = slots[t].type;
    transport = slots[t].transport;
    for (i = 0; i < n; i++)
        if (f[i].id != ID_ADDRESS && f[i].id != ID_BOND) put(t, &f[i], i, NULL, 0);
    if ((slots[t].type != type && !set_via_dongle(transport)) ||
        set_via_dongle(transport) != set_via_dongle(slots[t].transport))
        renew_identity(t);
}

/* ---- records ------------------------------------------------------------------
 * The set fields of one target, {id, len, value} each, behind
 * {u16 magic, u8 version, u8 used}.  An unknown id or a value out of range is
 * dropped on load, so rows can come and go without invalidating the rest. */

static u32 encode(u8 t, u32 part, u8 *out)
{
    u32 n, i, used = 4;
    const struct set_field *f = fields_of(t, &n);

    for (i = 0; i < n; i++) {
        if (!(set_bits[t] & (1u << i)) || part_of(t, i) != part) continue;
        out[used] = f[i].id;
        out[used + 1] = f[i].size;
        memcpy(out + used + 2, values_of(t) + f[i].offset, f[i].size);
        used += 2u + f[i].size;
    }
    out[0] = (u8)MAGIC;
    out[1] = (u8)(MAGIC >> 8);
    out[2] = VERSION;
    out[3] = (u8)used;
    return used;
}

/* A record's last good value for id: its length, 0 if none. */
static u32 record_field(u8 t, const u8 *in, u32 len, u8 id, const u8 **value)
{
    u32 used, o, bit, found = 0;

    if (len < 4 || (in[0] | in[1] << 8) != MAGIC || in[2] != VERSION) return 0;
    used = in[3] < len ? in[3] : len;
    for (o = 4; o + 2 <= used && o + 2u + in[o + 1] <= used; o += 2u + in[o + 1])
        if (in[o] == id && in[o + 1] && check(find(t, id, &bit), in + o + 2, in[o + 1]) == SET_OK) {
            *value = in + o + 2;
            found = in[o + 1];
        }
    return found;
}

/* The fields in mask that a record of t has, into values v: which they were. */
static u32 unpack(u8 t, const u8 *in, u32 len, u8 *v, u32 mask)
{
    u32 n, i, l, found = 0;
    const struct set_field *f = fields_of(t, &n);
    const u8 *p;

    for (i = 0; i < n; i++)
        if ((mask >> i & 1) && (l = record_field(t, in, len, f[i].id, &p)) != 0) {
            memset(v + f[i].offset, 0, f[i].size);
            memcpy(v + f[i].offset, p, l < f[i].size ? l : f[i].size);
            found |= 1u << i;
        }
    return found;
}

static void write_target(u8 t)
{
    u8 buf[RECORD_MAX];
    u32 part;

    for (part = 0; part < parts_of(t); part++)
        plat_store_write(key_of(t, part), buf, (u16)encode(t, part, buf));
}

/* ---- the remap held -------------------------------------------------------
 * One remap is in RAM at a time; the others are read from flash. */

#if REMAP_BASE_STORED
static int in_ram(u8 t)
{
    return t < TARGET_REMAP || remap_held == t;
}

/* Field bit of remap target t as flash has it, into out: 0 if not set there. */
static __attribute__((noinline)) int stored_field(u8 t, const struct set_field *f, u32 bit, u8 *out)
{
    u8 buf[RECORD_MAX];
    const u8 *p;
    int n = plat_store_read(key_of(t, part_of(t, bit)), buf, sizeof(buf));
    u32 l = n > 0 ? record_field(t, buf, (u32)n, f->id, &p) : 0;

    if (!l) return 0;
    memset(out, 0, f->size);
    memcpy(out, p, l < f->size ? l : f->size);
    return 1;
}

static void base_value(const struct set_field *f, u8 *out)
{
    if (remap_held == TARGET_BASE) memcpy(out, (const u8 *)&remap + f->offset, f->size);
    else if (!stored_field(TARGET_BASE, f, (u32)(f - remap_fields), out)) own_default(TARGET_BASE, f, out);
}

/* The remap held put out: its changes written; ephemeral ones are lost. */
static __attribute__((noinline)) void release(void)
{
    u8 t = remap_held;

    if (!t) return;
    if (dirty >> t & 1) write_target(t);
    if (tried >> t & 1) util_log("settings: ephemeral changes to remap %u lost", t - TARGET_REMAP);
    dirty &= ~(1u << t);
    tried &= ~(1u << t);
    remap_held = 0;
}

/* Remap target t from flash into remap: its defaults, what it takes from
 * the base, then its own records. */
static __attribute__((noinline)) void load(u8 t)
{
    u8 buf[RECORD_MAX], *v = (u8 *)&remap;
    u32 mask = inherited(t), part, k;
    int n;

    for (k = 0; k < COUNT(remap_fields); k++)
        own_default(t, &remap_fields[k], v + remap_fields[k].offset);
    for (part = 0; mask && part < parts_of(t); part++) {
        n = plat_store_read(key_of(TARGET_BASE, part), buf, sizeof(buf));
        if (n > 0) unpack(TARGET_BASE, buf, (u32)n, v, mask);
    }
    set_bits[t] = 0;
    for (part = 0; part < parts_of(t); part++) {
        n = plat_store_read(key_of(t, part), buf, sizeof(buf));
        if (n > 0) set_bits[t] |= unpack(t, buf, (u32)n, v, ~0u);
    }
    remap_held = t;
}

/* Remap target t in RAM, read in in place of the one held if it is not. */
static u8 *hold(u8 t)
{
    if (remap_held != t) {
        release();
        load(t);
    }
    return (u8 *)&remap;
}
#else
static int in_ram(u8 t)
{
    return t < TARGET_REMAP;
}

static void base_value(const struct set_field *f, u8 *out)
{
    own_default(TARGET_BASE, f, out);
}
#endif

void set_init(void)
{
    u8 buf[RECORD_MAX];
    u8 t;
    u32 part;
    int n;

#if REMAP_BASE_STORED
    remap_held = 0;              /* read in when wanted */
#endif
    tried = 0;
    for (t = 0; t < TARGET_REMAP; t++) {
        if (!stored(t)) continue;
        reset(t);
        for (part = 0; part < parts_of(t); part++) {
            memset(buf, 0, sizeof(buf));
            n = plat_store_read(key_of(t, part), buf, sizeof(buf));
            if (n > 0) set_bits[t] |= unpack(t, buf, (u32)n, values_of(t), ~0u);
        }
    }
    if (!(set_bits[TARGET_CAL] & 1)) util_log("settings: calibration required");
}

static void write_dirty(void)
{
    u8 t;

    for (t = 0; t < TARGETS; t++)
        if (dirty & (1u << t)) write_target(t);
    if (dirty) util_log("settings: written");
    dirty = 0;
    commit_now = 0;
}

/* Written once nothing has changed for a moment and no pairing or link
 * set-up is under way (a flash write then has reset the chip). */
void set_poll(u32 now, int radio_quiet)
{
    if (dirty && radio_quiet && (commit_now || now - changed_ms >= SETTLE_MS)) write_dirty();
}

void set_flush(void)
{
    write_dirty();
}

/* ---- the controller's view ------------------------------------------------- */

const struct set_slot *set_slot(u8 slot)
{
    slot %= SLOTS;
    return slot < SLOTS_STORED ? &slots[slot] : &none_slot;
}

struct rgb set_colour(u8 entry)
{
    return pal.entry[entry % PALETTE];
}

const struct set_brightness *set_brightness(void)
{
    return &pal.brightness;
}

const struct bt_bond *set_bond(u8 slot)
{
    return &set_slot(slot)->bond;
}

void set_bond_store(u8 slot, const struct bt_bond *bond)
{
    struct set_slot *s;
    u32 bit;
    const struct set_field *f = find(slot % SLOTS, ID_BOND, &bit);
    u8 a[6];

    if (!stored(slot % SLOTS)) return;
    s = &slots[slot % SLOTS];
    if (!memcmp(&s->bond, bond, sizeof(*bond))) return;
    put(slot % SLOTS, f, bit, (const u8 *)bond, sizeof(*bond));
    if (util_is_zero(s->address, 6)) {
        set_slot_address(slot, a);
        f = find(slot % SLOTS, ID_ADDRESS, &bit);
        put(slot % SLOTS, f, bit, a, 6);
    }
    touch(slot % SLOTS);
}

static u32 fnv1a(u32 h, const u8 *d, u32 n)
{
    while (n--) h = (h ^ (d ? *d++ : 0)) * 16777619u;
    return h;
}

/* A slot's own address: pinned once it bonded, else derived from the flash ID,
 * the slot and its type, locally administered.  Least significant byte first. */
void set_slot_address(u8 slot, u8 out[6])
{
    const u8 *seed = plat_unique_id();
    /* Through the dongle the type can change under the same host. */
    u8 salt[3] = { (u8)(slot % SLOTS), set_via_dongle(set_slot(slot)->transport) ? 0xff :
                                       set_slot(slot)->type, 0 };
    u32 a, b;

    if (!util_is_zero(set_slot(slot)->address, 6)) {
        memcpy(out, set_slot(slot)->address, 6);
        if (board_slots.static_random && set_slot(slot)->type != PROFILE_SWITCH2) out[5] |= 0xc0;
        return;
    }
    a = fnv1a(fnv1a(2166136261u, seed, 16), salt, 3);
    b = fnv1a(fnv1a(0x811c9dc5u ^ 0x5a5a5a5au, salt, 3), seed, 16);
    memcpy(out, &a, 4);
    memcpy(out + 4, &b, 2);
    out[5] = (u8)((out[5] | 0x02) & 0xfe);
    if (board_slots.static_random && set_slot(slot)->type != PROFILE_SWITCH2) out[5] |= 0xc0;
}

/* Roll all address bytes from the complete previous identity and current tick,
 * with no fixed prefix or stored generation counter, keeping the local and
 * unicast bits.  This is only a candidate: the caller decides when to store it. */
static void set_slot_new_address(u8 slot, u8 out[6])
{
    u32 a, b, now = plat_ms();

    set_slot_address(slot, out);
    a = fnv1a(fnv1a(2166136261u, out, 6), (const u8 *)&now, sizeof(now));
    b = fnv1a(fnv1a(0x811c9dc5u ^ 0x5a5a5a5au, out, 6), (const u8 *)&now, sizeof(now));
    memcpy(out, &a, 4);
    memcpy(out + 4, &b, 2);
    out[5] = (u8)((out[5] | 0x02) & 0xfe);
    if (board_slots.static_random && set_slot(slot)->type != PROFILE_SWITCH2) out[5] |= 0xc0;
}

/* First pairing uses the inspectable identity; replacing a bond may renew it. */
void set_pairing_address(u8 slot, int renew, u8 out[6])
{
    if (renew && !util_is_zero(set_bond(slot), sizeof(struct bt_bond))) set_slot_new_address(slot, out);
    else set_slot_address(slot, out);
}

/* A pairing that succeeded: the address it ran at and its bond, together.
 * Until then the slot keeps its old ones. */
void set_pairing_store(u8 slot, const u8 address[6], const struct bt_bond *bond)
{
    u32 bit;
    const struct set_field *f = find(slot % SLOTS, ID_ADDRESS, &bit);

    if (!stored(slot % SLOTS)) return;
    put(slot % SLOTS, f, bit, address, 6);
    touch(slot % SLOTS);
    set_bond_store(slot, bond);
}

const struct set_cal *set_cal(void)
{
    return &cal;
}

#if REMAP_BASE_STORED
const struct set_remap *set_remap(u8 remap)
{
    return (const struct set_remap *)hold((u32)remap + 1 <= REMAPS_STORED ? (u8)(TARGET_REMAP + remap) : TARGET_BASE);
}
#endif

u32 set_edits(void)
{
    return edits;
}

void set_cal_store(const struct set_cal *c)
{
    put(TARGET_CAL, &cal_fields[0], 0, (const u8 *)c, cal_fields[0].size);
    touch(TARGET_CAL);
    commit_now = 1;
}

/* ---- changes from outside: collect, then commit -------------------------------
 * Items are {target, id, len, value}; len 0 puts the field back to its
 * default, and id 0 (len 0) the whole target, a slot's pairing kept.  Each is checked as it arrives;
 * SET_UNPAIR (len 0, slot only) clears the bond and generates a new address.
 * commit applies them in order. */

u32 set_write(const u8 *in, u32 len, u8 *results, u32 max)
{
    u32 o = 0, n = 0, bit;

    while (o + 3 <= len && n < max) {
        u8 t = in[o], id = in[o + 1], l = in[o + 2];
        const struct set_field *f = t < TARGETS ? find(t, id, &bit) : NULL;
        u8 r;

        if (o + 3u + l > len) { results[n++] = SET_BAD_LENGTH; break; }
        if (t >= TARGETS || !stored(t) || (id == SET_UNPAIR && t >= SLOTS)) r = SET_UNKNOWN;
        else if (ephemeral_refuses(t)) r = SET_NOT_HELD;
        else if (id == 0 || id == SET_UNPAIR) r = l ? SET_BAD_LENGTH : SET_OK;
        else r = check(f, in + o + 3, l);
        if (r == SET_OK && staged + 3u + l > STAGE_MAX) r = SET_FULL;
        if (r == SET_OK) {
            memcpy(stage + staged, in + o, 3u + l);
            staged = (u16)(staged + 3u + l);
            staged_items++;
        }
        results[n++] = r;
        o += 3u + l;
    }
    return n;
}

void set_apply(void)
{
    u32 o, bit;

    for (o = 0; o + 3 <= staged; o += 3u + stage[o + 2]) {
        u8 t = stage[o], id = stage[o + 1], l = stage[o + 2];
        const struct set_field *f;

        if (ephemeral_refuses(t)) {      /* put out since it was checked */
            util_log("settings: remap %u no longer held", t - TARGET_REMAP);
            continue;
        }
        if (id == SET_UNPAIR) {
            renew_identity(t);
        } else if (id == 0) {
            reset_keeping_pairing(t);
        } else {
            f = find(t, id, &bit);
            u8 v = (u8)(l ? stage[o + 3] : field_def(f, t));

            /* A new type is a new device to the console: the old address
             * and bond belong to the old one.  Through the dongle the host
             * stays the dongle, whatever the type; moving to or from the
             * dongle changes the host (dongle and dongle+usb share it). */
            if (t < SLOTS && ((id == ID_TYPE && v != slots[t].type &&
                               !set_via_dongle(slots[t].transport)) ||
                              (id == ID_TRANSPORT && set_via_dongle(v) !=
                               set_via_dongle(slots[t].transport)))) {
                renew_identity(t);
            }
            put(t, f, bit, stage + o + 3, l);
        }
        touch(t);
    }
    inherit();
    if (staged) {
        edits++;
        util_log("settings: %u changes%s", staged_items, ephemeral ? " (ephemeral)" : "");
    }
    commit_now = 1;
    set_drop();
}

void set_drop(void)
{
    staged = staged_items = 0;
}

u32 set_staged(void)
{
    return staged_items;
}

/* From here on nothing is written to flash, until the next boot.  What is
 * already waiting is written first, before it can pick up test values. */
void set_ephemeral(void)
{
    if (ephemeral) return;
    write_dirty();
    ephemeral = 1;
    util_log("settings: ephemeral");
}

int set_is_ephemeral(void)
{
    return ephemeral;
}

/* ---- describing and reading, for the tools ------------------------------- */

static u32 put_string(u8 *out, u32 o, u32 max, const char *s)
{
    u32 n = s ? (u32)strlen(s) + 1 : 1;

    if (o + n > max) return max + 1;
    if (s) memcpy(out + o, s, n);
    else out[o] = 0;
    return o + n;
}

/* Field number n over all targets: slots, palette, calibration, remap slots.
 * [0] version [1] target kind [2] id [3] type [4] size [6..7] max, then
 * name, help, the enum names (all NUL-terminated) and the defaults (one per
 * slot for slot fields).  0: past the last. */
u32 set_describe(u32 n, u8 *out, u32 max)
{
    u32 kinds[4] = { COUNT(slot_fields), COUNT(palette_fields), COUNT(cal_fields), COUNT(remap_fields) };
    static const struct set_field *const tables[4] = { slot_fields, palette_fields, cal_fields, remap_fields };
    const struct set_field *f;
    u32 kind = 0, o, i, defs;

    while (kind < 4 && n >= kinds[kind]) n -= kinds[kind++];
    if (kind == 4 || max < 8) return 0;
    f = &tables[kind][n];
    out[0] = VERSION;
    out[1] = (u8)kind;
    out[2] = f->id;
    out[3] = f->type;
    out[4] = f->size;
    out[5] = f->flags;
    util_put16(out + 6, (u16)field_max(f));
    o = put_string(out, 8, max, f->name);
    o = put_string(out, o, max, f->help);
    for (i = 0; f->type == T_ENUM && f->names[i]; i++) o = put_string(out, o, max, f->names[i]);
    defs = kind == 0 ? SLOTS : 1;
    for (i = 0; i < defs && o <= max; i++) {
        u32 size = f->size < 4 ? f->size : 0, def = field_def(f, (u8)i);

        if (o + size > max) return 0;
        memcpy(out + o, &def, size);
        o += size;
    }
    return o <= max ? o : 0;
}

/* [0] version [1] target [2] field count, then per field: id, set (0/1),
 * len, value. */
static u32 read_values(u8 t, u8 *out, u32 max, int defaults)
{
    u32 n, i, o = 3;
    const struct set_field *f;

    if (t >= TARGETS || max < 3) return 0;
    if (!stored(t)) defaults = 1;
    f = fields_of(t, &n);
    for (i = 0; i < n; i++) {
        if (o + 3u + f[i].size > max) return 0;
        out[o] = f[i].id;
        out[o + 1] = defaults ? 0 : (set_bits[t] >> i) & 1;
        out[o + 2] = f[i].size;
        if (defaults) default_value(t, &f[i], out + o + 3);
#if REMAP_BASE_STORED
        else if (!in_ram(t)) {   /* not held: from flash, as it would be read in */
            out[o + 1] = (u8)stored_field(t, &f[i], i, out + o + 3);
            if (!out[o + 1]) default_value(t, &f[i], out + o + 3);
        }
#endif
        else if (t < SLOTS && f[i].id == ID_ADDRESS) set_slot_address(t, out + o + 3);
        else memcpy(out + o + 3, values_of(t) + f[i].offset, f[i].size);
        o += 3u + f[i].size;
    }
    out[0] = VERSION;
    out[1] = t;
    out[2] = (u8)n;
    return o;
}

u32 set_read(u8 t, u8 *out, u32 max) { return read_values(t, out, max, 0); }
u32 set_read_defaults(u8 t, u8 *out, u32 max) { return read_values(t, out, max, 1); }

#ifdef UNITTEST
/* A field's bit in set_bits. */
static u32 bit_of(u8 t, u8 id)
{
    u32 bit;

    find(t, id, &bit);
    return 1u << bit;
}
/* As VM: an item is as long as written, and a read returns at most that. */
static u8 store[KEYS][RECORD_MAX];
static u16 store_len[KEYS];
static int store_writes;
static u32 ms;
static const u8 uid[16] = { 9, 8, 7 };

int plat_store_read(u16 key, void *buf, u16 len)
{
    if (len > store_len[key]) len = store_len[key];
    memcpy(buf, store[key], len);
    return len;
}
int plat_store_write(u16 key, const void *buf, u16 len)
{
    if (len > RECORD_MAX) len = RECORD_MAX;
    memcpy(store[key], buf, len);
    store_len[key] = len;
    store_writes++;
    return len;
}
u32 plat_ms(void) { return ms; }
const u8 *plat_unique_id(void) { return uid; }
const struct board_led_brightness board_led_brightness = {
    .steady = 40, .animation = 1600, .connect = 200,
    .idle = 200, .battery = 200,
};
const struct board_slots board_slots = {
    .type = { PROFILE_GENERIC_BLE, PROFILE_DUALSENSE, PROFILE_XBOX, PROFILE_SWITCH2,
              PROFILE_NONE, PROFILE_NONE, PROFILE_NONE, PROFILE_NONE },
    .transport = { TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_DONGLE, TRANSPORT_BT_USB,
                   TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_BT_USB },
    .static_random = SLOTS_STORED < SLOTS,
};
const struct board_rumble board_rumble = {
    .min = { { 15, 20, 25, 25, 25 }, { 15, 15, 15, 15, 15 } },
    .max = { { 35, 55, 75, 80, 90 }, { 50, 60, 70, 80, 90 } },
    .kick_ms = { 0, 0, 0, 25, 30 },
};
void util_log(const char *fmt, ...) { (void)fmt; }
void util_put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

static void decode(u8 t, const u8 *in, u32 len)
{
    set_bits[t] |= unpack(t, in, len, values_of(t), ~0u);
}

static u32 write1(u8 t, u8 id, u8 len, const void *v)
{
    u8 item[3 + 255], r = 0xee;

    item[0] = t;
    item[1] = id;
    item[2] = len;
    if (len) memcpy(item + 3, v, len);
    set_write(item, 3u + len, &r, 1);
    return r;
}

/* Every record of a kept target fits with every field set. */
static void records_fit(void)
{
    u32 t, worst = 4;

    for (t = 0; t < 5; t++) {
        u8 target = t == 0 ? 0 : t == 1 ? TARGET_PALETTE : t == 2 ? TARGET_CAL : TARGET_BASE;
        u32 n, i, used = 4;
        const struct set_field *f = fields_of(target, &n);

        if (!stored(target)) continue;
        for (i = 0; i < n; i++) if (part_of(target, i) == (t == 4)) used += 2u + f[i].size;
        if (used > worst) worst = used;
    }
    CHECK(worst <= RECORD_MAX && worst <= 255);
    CHECK(COUNT(slot_fields) <= 32 && sizeof(struct set_slot) <= 256);
}

#if REMAP_BASE_STORED && REMAPS_STORED >= 2
/* A remap not held reads as it will once held; one put out keeps what
 * changed there, but for ephemeral changes. */
static void held_remaps(void)
{
    u8 a[512], b[512], rumble = 4, kick[RUMBLE_STEPS] = { 1, 2, 3, 4, 5 };
    u32 n;

    ephemeral = 0;
    CHECK(write1(TARGET_BASE, 23, RUMBLE_STEPS, kick) == SET_OK);
    CHECK(write1(TARGET_REMAP + 1, 18, 1, &rumble) == SET_OK);
    set_apply();
    (void)set_remap(0);
    n = set_read(TARGET_REMAP + 1, a, sizeof(a));
    CHECK(n > 3 && set_remap(1)->rumble == 4 && set_remap(1)->rumble_kick_ms[4] == 5);
    CHECK(set_read(TARGET_REMAP + 1, b, sizeof(b)) == n && !memcmp(a, b, n));
    CHECK(set_remap(0)->rumble_kick_ms[2] == 3);              /* from the base */
    set_ephemeral();
    rumble = 2;
    CHECK(write1(TARGET_REMAP + 1, 18, 1, &rumble) == SET_NOT_HELD);
    (void)set_remap(1);
    CHECK(write1(TARGET_REMAP + 1, 18, 1, &rumble) == SET_OK);
    (void)set_remap(0);                                          /* put out before commit */
    set_apply();
    CHECK(set_remap(1)->rumble == 4);                           /* held again */
    CHECK(write1(TARGET_REMAP + 1, 18, 1, &rumble) == SET_OK);
    set_apply();
    CHECK(set_remap(1)->rumble == 2);
    (void)set_remap(0);                                          /* changes lost */
    CHECK(set_remap(1)->rumble == 4);
    ephemeral = 0;
    CHECK(write1(TARGET_BASE, 0, 0, NULL) == SET_OK && write1(TARGET_REMAP + 1, 0, 0, NULL) == SET_OK);
    set_apply();
    set_flush();
    CHECK(set_remap(1)->rumble == 3 && set_remap(0)->rumble_kick_ms[4] == board_rumble.kick_ms[4]);
}
#else
static void held_remaps(void) {}
#endif

#if SLOTS_STORED == SLOTS && REMAPS_STORED == SLOTS
/* Every slot and remap slot stored (the Cyclone 2's). */
static int full_storage(void)
{
    u8 buf[512], a[6], b[6], n[6];
    struct bt_bond bond;
    u8 x;
    u16 speed = 3000, huge = 60000;
    u8 col[2] = { 4, 5 };
    u32 t;

    { static const u8 zeros[16];
      CHECK(fnv1a(2166136261u, NULL, 16) == fnv1a(2166136261u, zeros, 16)); }
    records_fit();

    set_init();
    CHECK(set_slot(0)->type == PROFILE_GENERIC_BLE && set_slot(1)->type == PROFILE_DUALSENSE);
    CHECK(set_slot(2)->type == PROFILE_XBOX && set_slot(3)->type == PROFILE_SWITCH2);
    CHECK(set_slot(0)->transport == TRANSPORT_BT_USB && set_slot(1)->transport == TRANSPORT_BT_USB);
    CHECK(set_slot(2)->transport == TRANSPORT_DONGLE && set_slot(3)->transport == TRANSPORT_BT_USB);
    for (t = 4; t < SLOTS; t++) CHECK(set_slot(t)->type == PROFILE_NONE);
    CHECK(set_slot(1)->link.in_flight == 2 && set_slot(7)->link.poll_us == 3750);
    /* Inspecting an unused slot reveals its first pairing identity without
     * saving it. Classic and LE replacements can still request a new one. */
    {
        u8 address[6], pair[6], values[512];
        u32 size = set_read(7, values, sizeof(values)), at;
        int found = 0;

        set_slot_address(7, address);
        set_pairing_address(7, 1, pair);
        CHECK(!memcmp(address, pair, 6) && !set_bits[7]);
        for (at = 3; at + 3 <= size; at += 3u + values[at + 2]) {
            if (values[at] != ID_ADDRESS) continue;
            CHECK(values[at + 1] == 0 && values[at + 2] == 6);
            CHECK(!memcmp(values + at + 3, address, 6));
            found = 1;
        }
        CHECK(found && !set_bits[7] && !dirty);
        size = set_read_defaults(7, values, sizeof(values));
        for (at = 3; at + 3 <= size; at += 3u + values[at + 2])
            if (values[at] == ID_ADDRESS) CHECK(!memcmp(values + at + 3, (u8[6]){0}, 6));
        slots[7].bond.peer[0] = 1;
        set_pairing_address(7, 1, pair);
        CHECK(memcmp(address, pair, 6));
        set_pairing_address(7, 0, pair);
        CHECK(!memcmp(address, pair, 6));
        slots[7].bond.peer[0] = 0;
        slots[7].bond.le_peer[0] = 1;
        set_pairing_address(7, 1, pair);
        CHECK(memcmp(address, pair, 6));
        slots[7].bond.le_peer[0] = 0;
        set_slot_address(7, pair);
        CHECK(!memcmp(address, pair, 6) && !set_bits[7] && !dirty);
    }
    /* Remap A is the swap until changed; the others are empty. */
    CHECK(!strcmp(set_remap(0)->name, "Swap A/B X/Y") && set_remap(0)->map[0] == 2 && set_remap(0)->map[4] == 4);
    CHECK(!set_remap(1)->name[0] && !set_remap(1)->map[0] && !set_remap(0)->rule[0].trigger);
    /* Rumble and the mouse take their field defaults, remap A's too. */
    for (t = 0; t < 2; t++)
        CHECK(set_remap(t)->rumble == 3 && set_remap(t)->rumble_min[0][1] == 20 &&
              set_remap(t)->rumble_max[0][0] == 35 && set_remap(t)->rumble_max[1][4] == 90 &&
              set_remap(t)->rumble_kick_ms[0] == 0 && set_remap(t)->rumble_kick_ms[3] == 25 &&
              set_remap(t)->stick_speed == 1500 && set_remap(t)->gyro_accel == 250);
    CHECK(set_remap(REMAP_BASE)->rumble_max[0][1] == 55 && set_remap(REMAP_BASE)->gyro_speed == 32 &&
          !set_remap(REMAP_BASE)->name[0] && !set_remap(REMAP_BASE)->map[0]);
    CHECK(set_remap(SLOT_NONE) == set_remap(REMAP_BASE));
    /* A list: every number in range, the whole length. */
    CHECK(write1(TARGET_REMAP + 1, 19, 5, (const u8[]){ 10, 20, 30, 40, 101 }) == SET_RANGE);
    CHECK(write1(TARGET_REMAP + 1, 19, 4, (const u8[]){ 10, 20, 30, 40 }) == SET_BAD_LENGTH);
    CHECK(write1(TARGET_REMAP + 1, 19, 5, (const u8[]){ 10, 20, 30, 40, 100 }) == SET_OK);
    set_drop();
    /* The remap slots take what they have not set from the base, but their
     * names; A keeps its swap. */
    {
        u8 map[REMAP_INPUTS] = { [3] = REMAP_OFF }, d[512];
        char name[REMAP_NAME] = "Base";

        CHECK(write1(TARGET_BASE, 18, 1, &(u8){ 1 }) == SET_OK);
        CHECK(write1(TARGET_BASE, 1, sizeof(name), name) == SET_OK);
        CHECK(write1(TARGET_BASE, 2, sizeof(map), map) == SET_OK);
        set_apply();
        CHECK(set_remap(1)->rumble == 1 && set_remap(0)->rumble == 1 && set_remap(7)->map[3] == REMAP_OFF);
        CHECK(set_remap(0)->map[3] == 4 + 1 && !set_remap(1)->name[0] && !strcmp(set_remap(0)->name, "Swap A/B X/Y"));
        CHECK(set_read_defaults(TARGET_REMAP + 2, d, sizeof(d)) > 3);   /* a slot's default: the base's */
        CHECK(write1(TARGET_REMAP + 1, 18, 1, &(u8){ 4 }) == SET_OK);
        set_apply();
        CHECK(write1(TARGET_BASE, 18, 1, &(u8){ 2 }) == SET_OK);
        set_apply();
        CHECK(set_remap(1)->rumble == 4 && set_remap(2)->rumble == 2);
        CHECK(write1(TARGET_REMAP + 1, 18, 0, NULL) == SET_OK);
        set_apply();
        CHECK(set_remap(1)->rumble == 2);
        /* Stored under its own keys, and followed again after a restart. */
        set_flush();
        CHECK(store[KEY_BASE][2] == VERSION && store[KEY_BASE_STICKS][2] == VERSION);
        memset(&remap, 0, sizeof(remap));
        set_init();
        CHECK(set_remap(3)->rumble == 2 && set_remap(3)->map[3] == REMAP_OFF && set_remap(0)->map[3] == 4 + 1);
        CHECK(!strcmp(set_remap(REMAP_BASE)->name, "Base"));
        CHECK(write1(TARGET_BASE, 0, 0, NULL) == SET_OK);
        set_apply();
        set_flush();
        CHECK(set_remap(1)->rumble == 3 && !set_remap(1)->map[3] && !set_remap(REMAP_BASE)->name[0]);
    }
    {
        u8 d[512];
        u32 n = set_read_defaults(TARGET_REMAP, d, sizeof(d));

        CHECK(n > 3 && d[3] == 1 && !d[4] && !memcmp(d + 6, "Swap A/B X/Y", 13));
    }
    CHECK(set_slot(0)->connect_colour == 5);
    {
        struct rgb c = set_colour(set_slot(2)->connect_colour);    /* X: green */
        CHECK(c.r == 0 && c.g == 255 && c.b == 0);
        c = set_colour(set_slot(6)->connect_colour);               /* left: orange */
        CHECK(c.r == 255 && c.g == 102 && c.b == 0);
        CHECK(set_brightness()->steady == board_led_brightness.steady &&
              set_brightness()->animation == board_led_brightness.animation &&
              set_brightness()->connect == board_led_brightness.connect &&
              set_brightness()->dither_ms == 2);
    }

    store_writes = 0;

    /* Collected, not applied, until commit; then stored. */
    x = 6;
    CHECK(write1(1, 14, 1, &x) == SET_OK);
    CHECK(write1(1, 15, 2, &speed) == SET_OK);
    CHECK(write1(1, 15, 2, &huge) == SET_RANGE);
    CHECK(write1(1, 99, 1, &x) == SET_UNKNOWN);
    CHECK(write1(1, 14, 2, &speed) == SET_BAD_LENGTH);
    x = PROFILE_COUNT;
    CHECK(write1(1, 1, 1, &x) == SET_RANGE);
    CHECK(write1(TARGETS, 1, 1, &x) == SET_UNKNOWN);
    CHECK(set_staged() == 2 && set_slot(1)->link.in_flight == 2);
    set_apply();
    CHECK(set_slot(1)->link.in_flight == 6 && set_slot(1)->link.poll_us == 3000 && set_staged() == 0);
    set_poll(ms, 1);
    CHECK(store_writes == 1);

    /* Back from the store; a default with nothing set; reset. */
    memset(slots, 0, sizeof(slots));
    CHECK(store_len[key_of(1, 0)] == 4 + (2 + 1) + (2 + 2));   /* only what is set: in_flight, poll_us */
    set_init();
    CHECK(set_slot(1)->link.in_flight == 6 && set_slot(1)->link.poll_us == 3000);
    CHECK(set_slot(0)->link.in_flight == 2);
    {
        u8 defaults[512], current[512], again[512];
        u32 size = set_read(1, current, sizeof(current));
        CHECK(set_read_defaults(1, defaults, sizeof(defaults)) == size);
        CHECK(set_read(1, again, sizeof(again)) == size && !memcmp(current, again, size));
        u32 at = 3;
        for (u32 i = 0; i < defaults[2]; i++) {
            CHECK(defaults[at + 1] == 0);
            at += 3 + defaults[at + 2];
        }
        CHECK(set_read_defaults(TARGETS, defaults, sizeof(defaults)) == 0);
        CHECK(set_read_defaults(0, defaults, 3) == 0);
    }
    CHECK(write1(1, 14, 0, NULL) == SET_OK);
    set_apply();
    CHECK(set_slot(1)->link.in_flight == 2 && set_slot(1)->link.poll_us == 3000);
    CHECK(write1(1, 0, 0, NULL) == SET_OK);
    set_apply();
    CHECK(set_slot(1)->link.poll_us == 3750 && set_bits[1] == 0);
    CHECK(write1(1, 3, 1, &(u8){ PALETTE }) == SET_RANGE);
    set_poll(ms, 1);

    /* Curves, a remap slot's second record: defaults, validation, staging,
     * persistence and reset. */
    {
        u8 points[STICK_CURVE_POINTS] = { 0, 5, 10, 20, 30, 45, 60, 80, 100, 140, 180, 200, 220, 240, 255 };
        u8 mode = 1, r = TARGET_REMAP + 3;
        CHECK(set_remap(3)->left_curve_enabled == 0);
        CHECK(!memcmp(set_remap(3)->left_curve_points, linear_points, sizeof(points)));
        CHECK(!memcmp(set_remap(0)->right_curve_points, linear_points, sizeof(points)));
        CHECK(write1(r, 9, 1, &mode) == SET_OK);
        CHECK(write1(r, 10, sizeof(points), points) == SET_OK);
        CHECK(write1(r, 17, sizeof(points) - 1, points) == SET_BAD_LENGTH);
        points[3] = 1;
        CHECK(write1(r, 17, sizeof(points), points) == SET_RANGE);
        mode = 2;
        CHECK(write1(r, 16, 1, &mode) == SET_RANGE);
        CHECK(set_remap(3)->left_curve_enabled == 0);
        set_apply();
        set_poll(ms, 1);
        CHECK(store[KEY_REMAP_STICKS + 3][4] == 9 && store[KEY_REMAP + 3][3] == 4);   /* curves in the sticks record */
        set_init();
        CHECK(set_remap(3)->left_curve_enabled == 1);
        CHECK(set_remap(3)->left_curve_points[3] == 20);
        CHECK(set_remap(3)->right_curve_enabled == 0);
        mode = 0;
        CHECK(write1(r, 9, 1, &mode) == SET_OK);
        set_apply();
        CHECK(!set_remap(3)->left_curve_enabled && set_remap(3)->left_curve_points[3] == 20);
        CHECK(write1(r, 10, 0, NULL) == SET_OK);
        set_apply();
        CHECK(!memcmp(set_remap(3)->left_curve_points, linear_points, sizeof(points)));
    }

    /* An unknown id in a record is skipped. */
    store[key_of(2, 0)][3] = 4 + 3 + 3;
    store[key_of(2, 0)][0] = (u8)MAGIC;
    store[key_of(2, 0)][1] = MAGIC >> 8;
    store[key_of(2, 0)][2] = VERSION;
    store[key_of(2, 0)][4] = 200; store[key_of(2, 0)][5] = 1; store[key_of(2, 0)][6] = 1;
    store[key_of(2, 0)][7] = 14; store[key_of(2, 0)][8] = 1; store[key_of(2, 0)][9] = 4;
    store_len[key_of(2, 0)] = 10;
    set_init();
    CHECK(set_slot(2)->link.in_flight == 4);

    /* Pairing pins the address; a new type rolls the address and clears bond. */
    memset(&bond, 0x11, sizeof(bond));
    set_slot_address(0, a);
    set_bond_store(0, &bond);
    set_slot_address(0, b);
    CHECK(!memcmp(a, b, 6) && set_bits[0] == (bit_of(0, ID_ADDRESS) | bit_of(0, ID_BOND)));
    x = PROFILE_GENERIC_BLE;
    CHECK(write1(0, 1, 1, &x) == SET_OK);      /* the same type: kept */
    set_apply();
    CHECK(set_bond(0)->peer[0] == 0x11);
    CHECK(write1(0, 0, 0, NULL) == SET_OK);    /* reset, the same type: pairing kept */
    set_apply();
    CHECK(set_bond(0)->peer[0] == 0x11 && set_bits[0] == (bit_of(0, ID_ADDRESS) | bit_of(0, ID_BOND)));
    x = PROFILE_XBOX;
    CHECK(write1(0, 1, 1, &x) == SET_OK);
    set_apply();
    set_slot_address(0, b);
    CHECK(set_bond(0)->peer[0] == 0 && memcmp(a, b, 6) && set_slot(0)->type == PROFILE_XBOX);
    memcpy(n, b, 6);
    x = PROFILE_SWITCH;
    CHECK(write1(0, ID_TYPE, 1, &x) == SET_OK);
    set_apply();
    set_slot_address(0, b);
    CHECK(memcmp(a, b, 6) && memcmp(n, b, 6));  /* Switch -> Xbox -> Switch never restores a */
    memcpy(n, b, 6);
    x = PROFILE_XBOX;
    CHECK(write1(0, ID_TYPE, 1, &x) == SET_OK);
    set_apply();
    set_slot_address(0, b);
    CHECK(write1(0, 0, 0, NULL) == SET_OK);    /* resetting type also rolls the identity */
    set_apply();
    set_slot_address(0, a);
    CHECK(memcmp(a, b, 6) && memcmp(a, n, 6));
    /* ... and an address after the type stays. */
    memset(a, 0x22, 6);
    CHECK(write1(0, 1, 1, &x) == SET_OK && write1(0, ID_ADDRESS, 6, a) == SET_OK);
    set_apply();
    set_slot_address(0, b);
    CHECK(!memcmp(a, b, 6));
    /* A new address is only a candidate; a pairing stores it with its bond. */
    set_slot_new_address(0, b);
    CHECK(memcmp(a, b, 6));
    CHECK(memcmp(a + 3, b + 3, 3) && (b[5] & 3) == 2);
    {
        u8 changed_prefix[6], same[6];
        /* Both halves of the candidate depend on the old prefix too. */
        set_slot_new_address(0, same);
        CHECK(!memcmp(same, b, 6));
        slots[0].address[4] ^= 0x80;
        set_slot_new_address(0, changed_prefix);
        slots[0].address[4] ^= 0x80;
        CHECK(memcmp(b, changed_prefix, 3) && memcmp(b + 3, changed_prefix + 3, 3));
    }
    set_slot_address(0, n);
    CHECK(!memcmp(a, n, 6));
    set_pairing_store(0, b, &bond);
    set_slot_address(0, n);
    CHECK(!memcmp(b, n, 6) && set_bond(0)->peer[0] == 0x11);

    /* Unpair is an ordered, cancellable settings action. It renews the
     * address and clears the whole Classic/LE bond in the same commit. */
    CHECK(write1(TARGET_PALETTE, SET_UNPAIR, 0, NULL) == SET_UNKNOWN);
    CHECK(write1(0, SET_UNPAIR, 1, &x) == SET_BAD_LENGTH);
    CHECK(write1(0, SET_UNPAIR, 0, NULL) == SET_OK);
    set_slot_address(0, a);
    CHECK(!memcmp(a, n, 6) && set_bond(0)->peer[0] == 0x11);
    set_drop();
    CHECK(set_bond(0)->peer[0] == 0x11);
    CHECK(write1(0, SET_UNPAIR, 0, NULL) == SET_OK);
    CHECK(write1(0, 14, 1, &(u8){ 7 }) == SET_OK);
    set_apply();
    set_slot_address(0, b);
    CHECK(memcmp(a, b, 6) && memcmp(a + 3, b + 3, 3) && (b[5] & 3) == 2);
    memset(&bond, 0, sizeof(bond));
    CHECK(!memcmp(set_bond(0), &bond, sizeof(bond)) && set_slot(0)->link.in_flight == 7);
    CHECK(write1(0, SET_UNPAIR, 0, NULL) == SET_OK);
    set_apply();
    set_slot_address(0, n);
    CHECK(memcmp(b, n, 6));   /* even two commits within the same tick */
    set_flush();
    set_init();
    set_slot_address(0, a);
    CHECK(!memcmp(a, n, 6) && !memcmp(set_bond(0), &bond, sizeof(bond)));
    memset(&bond, 0x11, sizeof(bond));

    /* Through the dongle the host stays when the type changes; moving to or
     * from the dongle is a new host.  A dongle slot's address ignores its type. */
    x = TRANSPORT_DONGLE;
    CHECK(write1(1, ID_TRANSPORT, 1, &x) == SET_OK);
    set_apply();
    set_bond_store(1, &bond);
    set_slot_address(1, a);
    x = PROFILE_DUALSENSE;
    CHECK(write1(1, ID_TYPE, 1, &x) == SET_OK);
    set_apply();
    set_slot_address(1, b);
    CHECK(set_bond(1)->peer[0] == 0x11 && !memcmp(a, b, 6));
    x = TRANSPORT_USB;
    CHECK(write1(1, ID_TRANSPORT, 1, &x) == SET_OK);
    set_apply();
    CHECK(set_bond(1)->peer[0] == 0);
    reset_keeping_pairing(1);
    clear(1, ID_ADDRESS);
    {
        u8 d1[6], d2[6];

        slots[1].transport = TRANSPORT_DONGLE;
        slots[1].type = PROFILE_SWITCH;
        set_slot_address(1, d1);
        slots[1].type = PROFILE_XBOX;
        set_slot_address(1, d2);
        CHECK(!memcmp(d1, d2, 6));
        slots[1].transport = TRANSPORT_DONGLE_USB;          /* the same host as dongle */
        set_slot_address(1, d2);
        CHECK(!memcmp(d1, d2, 6));
        slots[1].transport = TRANSPORT_BT;
        set_slot_address(1, d2);
        CHECK(memcmp(d1, d2, 6));
    }
    /* dongle+usb: the dongle's host too.  To and from dongle, or a new type,
     * keeps the pairing; to bluetooth+usb is a new host. */
    x = TRANSPORT_DONGLE_USB;
    CHECK(write1(1, ID_TRANSPORT, 1, &x) == SET_OK);
    set_apply();
    CHECK(set_slot(1)->transport == TRANSPORT_DONGLE_USB && set_usb_first(TRANSPORT_DONGLE_USB));
    set_bond_store(1, &bond);
    set_slot_address(1, a);
    x = TRANSPORT_DONGLE;
    CHECK(write1(1, ID_TRANSPORT, 1, &x) == SET_OK);
    x = TRANSPORT_DONGLE_USB;
    CHECK(write1(1, ID_TRANSPORT, 1, &x) == SET_OK);
    x = PROFILE_SWITCH2;
    CHECK(write1(1, ID_TYPE, 1, &x) == SET_OK);
    set_apply();
    set_slot_address(1, b);
    CHECK(set_bond(1)->peer[0] == 0x11 && !memcmp(a, b, 6));
    x = TRANSPORT_BT_USB;
    CHECK(write1(1, ID_TRANSPORT, 1, &x) == SET_OK);
    set_apply();
    set_slot_address(1, b);
    CHECK(set_bond(1)->peer[0] == 0 && memcmp(a, b, 6));
    x = TRANSPORT_DONGLE_USB + 1;
    CHECK(write1(1, ID_TRANSPORT, 1, &x) == SET_RANGE);
    reset_keeping_pairing(1);
    clear(1, ID_ADDRESS);

    /* Palette and colours. */
    CHECK(write1(TARGET_PALETTE, 5, 3, "\x10\x20\x30") == SET_OK);
    CHECK(write1(0, 3, 1, col) == SET_OK);
    col[0] = PALETTE;
    CHECK(write1(0, 3, 1, col) == SET_RANGE);
    col[0] = 6;
    CHECK(write1(0, 4, 1, col) == SET_OK);
    CHECK(write1(0, 4, 2, col) == SET_BAD_LENGTH);
    {
        u16 b = 15, huge = 2551;

        CHECK(write1(TARGET_PALETTE, 17, 2, &b) == SET_OK);
        CHECK(write1(TARGET_PALETTE, 18, 2, &huge) == SET_RANGE);
    }
    set_apply();
    CHECK(pal.entry[4].r == 0x10 && set_slot(0)->connect_colour == 4 && set_slot(0)->primary_colour == 6);
    CHECK(set_brightness()->steady == 15);
    {
        u8 defaults[256];
        u32 n = set_read_defaults(TARGET_PALETTE, defaults, sizeof(defaults));
        int found = 0;

        for (u32 at = 3; at + 3 <= n; at += 3u + defaults[at + 2]) {
            if (defaults[at] != 17) continue;
            CHECK(!defaults[at + 1] && defaults[at + 2] == 2);
            CHECK((defaults[at + 3] | (u16)defaults[at + 4] << 8) == board_led_brightness.steady);
            found = 1;
        }
        CHECK(found && set_brightness()->steady == 15);
        CHECK(write1(TARGET_PALETTE, 17, 0, NULL) == SET_OK);
        set_apply();
        CHECK(set_brightness()->steady == board_led_brightness.steady);
    }

    /* Ephemeral: applied, never stored, pairing included. */
    set_poll(ms, 1);
    t = (u32)store_writes;
    set_ephemeral();
    x = 1;
    CHECK(write1(3, 14, 1, &x) == SET_OK);
    set_apply();
    set_bond_store(3, &bond);
    set_cal_store(&cal);
    set_poll(ms + 1000, 1);
    set_flush();
    CHECK(set_slot(3)->link.in_flight == 1 && store_writes == (int)t);


    /* Describe and read. */
    CHECK(set_describe(0, buf, sizeof(buf)) > 8 && buf[2] == 1 && !strcmp((char *)buf + 8, "type"));
    CHECK(set_describe(COUNT(slot_fields), buf, sizeof(buf)) && buf[1] == 1);
    CHECK(set_describe(COUNT(slot_fields) + COUNT(palette_fields), buf, sizeof(buf)) && buf[1] == 2);
    t = COUNT(slot_fields) + COUNT(palette_fields) + COUNT(cal_fields);
    CHECK(set_describe(t, buf, sizeof(buf)) && buf[1] == 3 && buf[3] == T_TEXT);
    CHECK(set_describe(t + COUNT(remap_fields), buf, sizeof(buf)) == 0);
    for (t = 0; t < COUNT(slot_fields) + COUNT(palette_fields) + COUNT(cal_fields) + COUNT(remap_fields); t++)
        CHECK(set_describe(t, buf, 512) > 0);
    CHECK(set_read(3, buf, sizeof(buf)) > 3 && buf[2] == COUNT(slot_fields));
    CHECK(set_read(TARGET_CAL, buf, sizeof(buf)) == 6u + cal_fields[0].size);
    {
        u32 sizes = 0, i;

        for (i = 0; i < COUNT(remap_fields); i++) sizes += remap_fields[i].size;
        CHECK(sizes <= sizeof(struct set_remap) && sizes + 2 > sizeof(struct set_remap));   /* at most padding */
        CHECK(set_read(TARGET_REMAP + 7, buf, sizeof(buf)) == 3 + 3 * COUNT(remap_fields) + sizes);
    }

    /* Remap slots: checked, stored under their own keys, read back. */
    {
        u8 map[REMAP_INPUTS] = { 0 }, rules[sizeof(((struct set_remap *)0)->rule)] = { 0 };
        struct remap_rule *r = (struct remap_rule *)rules;
        u32 used;

        set_drop();
        (void)set_remap(1);          /* held: ephemeral changes need that */
        map[3] = 4 + 1; map[4] = 3 + 1; map[2] = REMAP_OFF;
        CHECK(write1(TARGET_REMAP + 1, 2, sizeof(map), map) == SET_OK);
        map[0] = 15 + 1;
        CHECK(write1(TARGET_REMAP + 1, 2, sizeof(map), map) == SET_RANGE);
        map[0] = REMAP_INPUTS + 1;
        CHECK(write1(TARGET_REMAP + 1, 2, sizeof(map), map) == SET_RANGE);
        CHECK(write1(TARGET_REMAP + 1, 1, 10, "Mario Kart") == SET_BAD_LENGTH);
        CHECK(write1(TARGET_REMAP + 1, 1, REMAP_NAME, "Mario Kart\0\0\0\0\0\0\0\0\0\0\0\0\0\0") == SET_OK);
        r[0].trigger = RT_HELD; r[0].input = 9; r[0].effect = RE_TURBO; r[0].outputs[0] = PAD_A; r[0].rate = 0;
        r[1].trigger = RT_TAPPED; r[1].input = 5; r[1].effect = RE_TAP; r[1].outputs[1] = PAD_HOME >> 8;
        CHECK(write1(TARGET_REMAP + 1, 3, sizeof(rules), rules) == SET_RANGE);   /* tapped under 0 ms */
        r[1].ms10 = 20;
        CHECK(write1(TARGET_REMAP + 1, 3, sizeof(rules), rules) == SET_OK);
        r[1].effect = RE_HOLD;                                                   /* an event, a level effect */
        CHECK(write1(TARGET_REMAP + 1, 3, sizeof(rules), rules) == SET_RANGE);
        r[1].effect = RE_TAP; r[0].rate = REMAP_MAX_HZ + 1;
        CHECK(write1(TARGET_REMAP + 1, 3, sizeof(rules), rules) == SET_RANGE);
        r[0].rate = 0; r[0].outputs[1] = 0x80;                                   /* bit 15 */
        CHECK(write1(TARGET_REMAP + 1, 3, sizeof(rules), rules) == SET_RANGE);
        r[0].outputs[1] = 0; r[0].input = 15;
        CHECK(write1(TARGET_REMAP + 1, 3, sizeof(rules), rules) == SET_RANGE);
        r[0].input = 9; r[0].effect = RE_TURBO_MODIFIER;                         /* a level effect */
        CHECK(write1(TARGET_REMAP + 1, 3, sizeof(rules), rules) == SET_OK);
        r[0].trigger = RT_PRESSED;
        CHECK(write1(TARGET_REMAP + 1, 3, sizeof(rules), rules) == SET_RANGE);
        r[0].effect = RE_TOGGLE_TURBO_MODIFIER;
        CHECK(write1(TARGET_REMAP + 1, 3, sizeof(rules), rules) == SET_OK);
        r[0].effect = RE_COUNT;
        CHECK(write1(TARGET_REMAP + 1, 3, sizeof(rules), rules) == SET_RANGE);
        r[0].trigger = RT_HELD; r[0].effect = RE_TURBO;
        {
            u8 on = 1, two = 2;

            CHECK(write1(TARGET_REMAP + 1, 4, 1, &on) == SET_OK);               /* left sends to the right */
            CHECK(write1(TARGET_REMAP + 1, 13, 1, &two) == SET_RANGE);           /* right y axis */
        }
        t = set_edits();
        set_apply();
        CHECK(set_edits() == t + 1);
        CHECK(set_remap(1)->map[3] == 5 && set_remap(1)->map[2] == REMAP_OFF);
        CHECK(!strcmp(set_remap(1)->name, "Mario Kart") && set_remap(1)->rule[1].ms10 == 20);
        CHECK(set_remap(1)->maps_to[0] == STICK_OTHER && !set_remap(1)->invert[1][1]);
        CHECK(key_of(TARGET_REMAP + 1, 0) == KEY_REMAP + 1 && key_of(TARGET_REMAP + 1, 1) == KEY_REMAP_STICKS + 1);
        CHECK(key_of(TARGET_CAL, 0) == 10 && KEYS - 1 <= 49);
        used = encode(TARGET_REMAP + 1, 0, buf);
        CHECK(encode(TARGET_REMAP + 1, 1, buf + 256) == 4u + 3);                 /* left sends to: the sticks record */
        reset(TARGET_REMAP + 1);
        CHECK(set_remap(1)->map[3] == 0 && !set_remap(1)->name[0]);
        decode(TARGET_REMAP + 1, buf, used);
        CHECK(set_remap(1)->map[3] == 5 && set_remap(1)->rule[0].input == 9 && set_remap(1)->rule[1].trigger == RT_TAPPED);
        CHECK(write1(TARGET_REMAP + 1, 0, 0, NULL) == SET_OK);
        set_apply();
        CHECK(set_remap(1)->rule[0].trigger == RT_NONE && set_remap(1)->map[3] == 0);
        CHECK(set_remap(2)->map[3] == 0);
    }

    /* Deadzones: a remap slot's, beside its curves; calibration has none. */
    x = 26;
    CHECK(write1(TARGET_REMAP + 2, 7, 1, &x) == SET_RANGE);
    CHECK(write1(TARGET_CAL, 2, 1, &x) == SET_UNKNOWN);
    x = 5;
    CHECK(write1(TARGET_REMAP + 2, 7, 1, &x) == SET_OK);
    set_apply();
    CHECK(set_remap(2)->deadzone[0].inner == 5 && !set_remap(2)->deadzone[0].outer);
    {
        u32 used = encode(TARGET_REMAP + 2, 1, buf);
        reset(TARGET_REMAP + 2);
        CHECK(!set_remap(2)->deadzone[0].inner);
        decode(TARGET_REMAP + 2, buf, used);
        CHECK(set_remap(2)->deadzone[0].inner == 5);
    }

    memset(&bond, 0x22, sizeof(bond));
    CHECK(write1(6, ID_BOND, sizeof(bond) - 1, &bond) == SET_BAD_LENGTH);
    held_remaps();
    return TEST_DONE("settings");
}
#else

/* A board that keeps fewer slots and remap slots: the others read as their
 * defaults, take no writes, and a slot not kept is of type none. */
static int reduced_storage(void)
{
    u8 buf[512], type = PROFILE_XBOX_BLE, rumble = 4;
    struct bt_bond bond;
    u32 t;
    u8 addresses[SLOTS][6], pair[6];

    records_fit();
    set_init();
    for (t = 0; t < SLOTS_STORED; t++) {
        u32 j;
        set_slot_address(t, addresses[t]);
        if (set_slot(t)->type != PROFILE_SWITCH2) CHECK((addresses[t][5] & 0xc0) == 0xc0);
        for (j = 0; j < t; j++) CHECK(memcmp(addresses[t], addresses[j], 6));
        set_pairing_address(t, 0, pair);
        CHECK(!memcmp(pair, addresses[t], 6));
    }
    if (SLOTS_STORED > 3 && set_slot(3)->type == PROFILE_SWITCH2) {
        static const u8 public_address[6] = { 1, 2, 3, 4, 5, 0x12 };
        CHECK(write1(3, ID_ADDRESS, 6, public_address) == SET_OK);
        set_apply(); set_slot_address(3, pair);
        CHECK(!memcmp(pair, public_address, 6));
    }
    for (t = 0; t < SLOTS; t++)
        CHECK(set_slot(t)->type == (t < SLOTS_STORED ? board_slots.type[t] : PROFILE_NONE));
    /* Not kept: defaults to read, writes refused, nothing stored. */
    CHECK(set_read(SLOTS_STORED, buf, sizeof(buf)) > 3 && buf[1] == SLOTS_STORED);
    CHECK(buf[3] == ID_TYPE && !buf[4] && buf[6] == board_slots.type[SLOTS_STORED]);
    CHECK(write1(SLOTS_STORED, ID_TYPE, 1, &type) == SET_UNKNOWN);
    CHECK(write1(TARGET_REMAP + REMAPS_STORED, 18, 1, &rumble) == SET_UNKNOWN);
    memset(&bond, 0x11, sizeof(bond));
    set_bond_store(SLOTS_STORED, &bond);
    CHECK(!set_bond(SLOTS_STORED)->peer[0]);
#if REMAP_BASE_STORED
    CHECK(set_remap(REMAPS_STORED) == set_remap(REMAP_BASE));
#else
    /* No base either: it reads as its defaults, as the remap slots do. */
    CHECK(write1(TARGET_BASE, 18, 1, &rumble) == SET_UNKNOWN);
    {
        u8 d[512];
        u32 n = set_read(TARGET_BASE, buf, sizeof(buf));

        CHECK(n > 3 && n == set_read_defaults(TARGET_BASE, d, sizeof(d)) && !memcmp(buf, d, n));
        CHECK(set_read(TARGET_REMAP + 1, d, sizeof(d)) == n);
        CHECK(!memcmp(buf + 3, d + 3, n - 3));    /* a slot's: the base's */
    }
#endif
    /* Kept: as on every board, and back after a restart. */
    CHECK(write1(0, ID_TYPE, 1, &type) == SET_OK);
#if REMAP_BASE_STORED
    CHECK(write1(TARGET_BASE, 18, 1, &rumble) == SET_OK);
#endif
    set_apply();
    set_poll(ms, 1);
    CHECK(set_slot(0)->type == PROFILE_XBOX_BLE);
#if REMAP_BASE_STORED
    CHECK(set_remap(REMAPS_STORED)->rumble == 4);
#endif
    set_init();
    CHECK(set_slot(0)->type == PROFILE_XBOX_BLE);
#if REMAP_BASE_STORED
    CHECK(set_remap(REMAP_BASE)->rumble == 4);
#endif
    held_remaps();
    return TEST_DONE("settings");
}
#endif

int main(void)
{
#if SLOTS_STORED == SLOTS && REMAPS_STORED == SLOTS
    return full_storage();
#else
    return reduced_storage();
#endif
}
#endif
