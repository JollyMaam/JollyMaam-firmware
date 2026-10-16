#include <string.h>
#include "pad.h"

/* Every setting is a row in a field table: a never-reused id, a name, a type,
 * its range, its default (per slot) and a line of help.  The tools read the
 * table over USB (set_describe), so a new row needs no tool update.
 *
 * Each field is either default or set.  Only set fields are stored; reset
 * clears them, so a better default in a new firmware reaches every field
 * that was never set. */

enum { T_ENUM, T_NUMBER, T_COLOUR, T_RGB, T_BYTES };
/* The rows the code itself uses. */
enum { ID_TYPE = 1, ID_ADDRESS = 12, ID_BOND = 13, ID_TRANSPORT = 35,
       ID_LEFT_POINTS = 33, ID_RIGHT_POINTS = 34 };

struct set_field {
    u8 id;                   /* stored and sent; never reused */
    u8 type;
    u8 offset;               /* in the target's struct */
    u8 size;
    u16 max;                 /* T_NUMBER: largest value; T_ENUM: from names */
    const char *name;
    const char *const *names;   /* T_ENUM, NULL-terminated */
    u32 def[SLOTS];          /* little-endian bytes of the default, per slot */
    const char *help;
    u8 flags;                /* F_DEV: a developer setting (padtool --dev) */
};
#define F_DEV 1

#define COLOUR(entry, level) ((entry) | (level) << 8)
#define RGB24(r, g, b)       ((r) | (g) << 8 | (u32)(b) << 16)
#define ALL(v)               { [0 ... SLOTS - 1] = (v) }
#define FIELD(f)             .offset = offsetof(struct set_slot, f), \
                             .size = sizeof(((struct set_slot *)0)->f)

static const char *const types[] = {
    "switch", "switch2", "xbox", "xboxble", "dualsense", "dualshock4", "generic", "mouse",
    "genericble", NULL,
};
static const char *const transports[] = { "bluetooth", "usb", "bluetooth+usb", "dongle", NULL };
static const char *const grips[] = {
    "off", "host", "fade", "rumble", "input", "rumble+input", "primary", "secondary", NULL,
};
static const char *const players[] = {
    "off", "host", "fade", "primary", "secondary", NULL,
};
static const char *const curves[] = { "linear", "concave", "s-curve", "custom", NULL };
static const u8 linear_points[STICK_CURVE_POINTS] = { 32, 64, 96, 128, 159, 191, 223 };

static const char *const layouts[] = { "games", "desktop", NULL };
static const char *const phys[] = { "1m", "2m", NULL };
static const char *const onoff[] = { "on", "off", NULL };
static const char *const tx_powers[] = { "1", "2", "3", "4", "5", "6", NULL };

/* Slots A B X Y up down left right.  Colours are {palette entry, brightness}. */
static const struct set_field slot_fields[] = {
    { .id = ID_TYPE, .name = "type", .type = T_ENUM, .names = types, FIELD(type),
      .def = { PROFILE_SWITCH, PROFILE_SWITCH, PROFILE_GENERIC, PROFILE_SWITCH2,
               [4 ... 7] = PROFILE_MOUSE },
      .help = "what the slot connects as; changing it clears address and bond" },
    { .id = ID_TRANSPORT, .name = "transport", .type = T_ENUM, .names = transports, FIELD(transport),
      .def = ALL(TRANSPORT_BT),
      .help = "how: bluetooth; usb, a wired controller; bluetooth+usb, usb while a usb host "
              "is there, else bluetooth (never back to it mid-session); dongle, through our dongle, which is the type on its usb port" },
    { .id = 2, .name = "connect_colour", .type = T_COLOUR, FIELD(connect_colour),
      .def = { COLOUR(3, 5), COLOUR(2, 5), COLOUR(4, 5), COLOUR(5, 5),
               COLOUR(6, 4), COLOUR(7, 4), COLOUR(8, 4), COLOUR(9, 4) },
      .help = "Home and player lights while connecting and pairing: which slot" },
    { .id = 3, .name = "primary_colour", .type = T_COLOUR, FIELD(primary_colour),
      .def = ALL(COLOUR(2, 1)),
      .help = "Home while connected; the player lights around the player number" },
    { .id = 4, .name = "secondary_colour", .type = T_COLOUR, FIELD(secondary_colour),
      .def = ALL(COLOUR(5, 1)),
      .help = "the console's player number; resting grips; the other end of fade" },
    { .id = 5, .name = "grips", .type = T_ENUM, .names = grips, FIELD(grips),
      .def = ALL(GRIPS_OFF),
      .help = "grip lights while connected: host is the console's lightbar colour" },
    { .id = 6, .name = "players", .type = T_ENUM, .names = players, FIELD(players),
      .def = ALL(PLAYERS_HOST),
      .help = "player lights while connected: host is the console's player number" },
    { .id = 7, .name = "rumble", .type = T_NUMBER, .max = 100, FIELD(rumble),
      .def = ALL(100), .help = "motor strength in percent; 0 turns it off" },
    { .id = 8, .name = "mouse_layout", .type = T_ENUM, .names = layouts, FIELD(variant),
      .def = ALL(0), .help = "mouse: what each control does" },
    { .id = 9, .name = "stick_speed", .type = T_NUMBER, .max = 20000, FIELD(stick_speed),
      .def = ALL(1500), .help = "mouse: pointer counts per second, stick all the way" },
    { .id = 10, .name = "gyro_speed", .type = T_NUMBER, .max = 1000, FIELD(gyro_speed),
      .def = ALL(32), .help = "mouse, touchpad: counts per degree turned; 0 off" },
    { .id = 11, .name = "gyro_accel", .type = T_NUMBER, .max = 1000, FIELD(gyro_accel),
      .def = ALL(250), .help = "mouse, touchpad: percent of gyro_speed for fast turns" },
    { .id = ID_ADDRESS, .name = "address", .type = T_BYTES, FIELD(address),
      .help = "the slot's own address, least significant byte first; zero: derived" },
    { .id = ID_BOND, .name = "bond", .type = T_BYTES, FIELD(bond),
      .help = "the paired host: peer, link key, key type, LE peer, LTK" },
    /* The link, for measuring it; next connect. */
    { .id = 14, .flags = F_DEV, .name = "report_hz", .type = T_NUMBER, .max = 1000, FIELD(report_hz),
      .def = ALL(0), .help = "most reports per second, Classic and LE, whole milliseconds apart; "
                             "reports are built when the radio has room; 0: 250" },
    { .id = 15, .flags = F_DEV, .name = "le_interval_min", .type = T_NUMBER, .max = 3200,
      FIELD(link.le_interval_min), .def = ALL(0),
      .help = "LE: connection interval to ask for, 1.25 ms units (6 = 7.5 ms); 0: the profile's" },
    { .id = 16, .flags = F_DEV, .name = "le_interval_max", .type = T_NUMBER, .max = 3200,
      FIELD(link.le_interval_max), .def = ALL(0),
      .help = "LE: the longest interval to accept; 0: le_interval_min" },
    { .id = 17, .flags = F_DEV, .name = "le_phy", .type = T_ENUM, .names = phys, FIELD(link.le_phy),
      .def = ALL(0), .help = "LE: 2m asks the host for the 2M PHY" },
    { .id = 18, .flags = F_DEV, .name = "in_flight", .type = T_NUMBER, .max = 8, FIELD(link.in_flight),
      .def = ALL(2), .help = "input reports in the controller at once; 0: 2" },
    { .id = 19, .flags = F_DEV, .name = "poll_us", .type = T_NUMBER, .max = 40000, FIELD(link.poll_us),
      .def = ALL(3750), .help = "Classic: Best Effort poll request in us (3750 minimum, rounded down to 1250 steps); 0: none" },
    { .id = 23, .flags = F_DEV, .name = "pull_delay_us", .type = T_NUMBER, .max = 5000, FIELD(pull_delay_us),
      .def = ALL(0), .help = "build a report this long after the radio asks for it (fresher, "
                             "until it misses the connection event)" },
    { .id = 29, .flags = F_DEV, .name = "jit_us", .type = T_NUMBER, .max = 10000, FIELD(link.jit_us),
      .def = ALL(0), .help = "build a report this long before the connection event or sniff "
                             "anchor it goes out at; 0: adaptive; 1: as soon as the radio has room" },
    { .id = 28, .flags = F_DEV, .name = "edr3", .type = T_ENUM, .names = onoff, FIELD(link.edr3_off),
      .def = ALL(1), .help = "Classic: 3 Mbit/s packets (an 83-byte PlayStation report fits "
                             "one, but a Linux host stalled more often); off: 2 Mbit/s only" },
    { .id = 26, .flags = F_DEV, .name = "sniff_max", .type = T_NUMBER, .max = 800, FIELD(link.sniff_max),
      .def = ALL(0), .help = "Classic: ask the host for sniff, at most this many 0.625 ms "
                             "slots (even); 0: take what the host chooses" },
    { .id = 27, .flags = F_DEV, .name = "sniff_min", .type = T_NUMBER, .max = 800, FIELD(link.sniff_min),
      .def = ALL(0), .help = "Classic: the shortest sniff interval asked for; 0: sniff_max" },
    /* ids 20, 21: tx_step and tx_table, replaced by tx_power before release;
     * id 24: sniff_exit, now a rule in bt.c (sniff_too_slow); id 25: sniff_req,
     * replaced by sniff_max and sniff_min */
    { .id = 22, .name = "tx_power", .type = T_ENUM, .names = tx_powers, FIELD(link.tx_power),
      .def = ALL(4), .help = "radio strength 1-6: 1 is about 20 dB below 5; 6 about 3 dB "
                             "above 5, using more battery" },
    { .id = 31, .name = "left_curve", .type = T_ENUM, .names = curves, FIELD(left_curve),
      .help = "left stick radial response; applies immediately after calibration" },
    { .id = 32, .name = "right_curve", .type = T_ENUM, .names = curves, FIELD(right_curve),
      .help = "right stick radial response; applies immediately after calibration" },
    { .id = ID_LEFT_POINTS, .name = "left_curve_points", .type = T_BYTES, FIELD(left_curve_points),
      .help = "custom: 7 ascending output bytes (0-255) at 12.5,25,37.5,50,62.5,75,87.5% input" },
    { .id = ID_RIGHT_POINTS, .name = "right_curve_points", .type = T_BYTES, FIELD(right_curve_points),
      .help = "custom: 7 ascending output bytes (0-255) at 12.5,25,37.5,50,62.5,75,87.5% input" },
};

#define ENTRY(i, r, g, b) { .id = (i) + 1, .name = "palette" #i, .type = T_RGB, \
  .offset = 3 * (i), .size = 3, .def = { RGB24(r, g, b) } }

/* Full brightness; entries 0 and 1 are the idle and battery roles
 * (settings_ux.c).  The defaults are today's colours. */
static const struct set_field palette_fields[PALETTE] = {
    ENTRY(0, 255, 255, 255), ENTRY(1, 255, 102, 0), ENTRY(2, 255, 0, 0),
    ENTRY(3, 0, 0, 255), ENTRY(4, 153, 0, 255), ENTRY(5, 0, 255, 0),
    ENTRY(6, 0, 255, 255), ENTRY(7, 255, 255, 0), ENTRY(8, 255, 0, 191),
    ENTRY(9, 128, 255, 0), ENTRY(10, 0, 0, 0), ENTRY(11, 0, 0, 0),
    ENTRY(12, 0, 0, 0), ENTRY(13, 0, 0, 0), ENTRY(14, 0, 0, 0), ENTRY(15, 0, 0, 0),
};

_Static_assert(offsetof(struct set_cal, deadzone) == 112, "stored calibration layout");

static const struct set_field cal_fields[] = {
    { .id = 1, .name = "calibration", .type = T_BYTES, .offset = 0,
      .size = offsetof(struct set_cal, deadzone),
      .help = "sensor calibration for sticks and triggers; legacy stick deadzones ignored" },
    { .id = 2, .name = "left_inner_deadzone", .type = T_NUMBER, .max = 25,
      .offset = offsetof(struct set_cal, deadzone[0].inner), .size = 1,
      .help = "left stick: percent around centre suppressed; immediate, preserved by calibration" },
    { .id = 3, .name = "left_outer_deadzone", .type = T_NUMBER, .max = 25,
      .offset = offsetof(struct set_cal, deadzone[0].outer), .size = 1,
      .help = "left stick: percent of outer travel discarded; immediate, preserved by calibration" },
    { .id = 4, .name = "right_inner_deadzone", .type = T_NUMBER, .max = 25,
      .offset = offsetof(struct set_cal, deadzone[1].inner), .size = 1,
      .help = "right stick: percent around centre suppressed; immediate, preserved by calibration" },
    { .id = 5, .name = "right_outer_deadzone", .type = T_NUMBER, .max = 25,
      .offset = offsetof(struct set_cal, deadzone[1].outer), .size = 1,
      .help = "right stick: percent of outer travel discarded; immediate, preserved by calibration" },
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* ---- the values -------------------------------------------------------------
 * A target is a slot, the palette or calibration: a struct of values and a
 * bit per field that is set. */

#define MAGIC      0x4332
#define VERSION    1
#define RECORD_MAX 192
#define SETTLE_MS  100
#define STAGE_MAX  1024
/* 11 and 12 belonged to the removed shortcut/mapping prototype, 13 to the
 * last slot (now plat_retain_slot); do not reuse. */
#define KEYS 14

static struct set_slot slots[SLOTS];
static struct rgb palette[PALETTE];
static struct set_cal cal;
static u32 set_bits[TARGETS];
static u16 dirty;            /* a bit per target */
static u32 changed_ms;
static u8 commit_now, ephemeral;
static u8 stage[STAGE_MAX];
static u16 staged, staged_items;

static const struct set_field *fields_of(u8 t, u32 *count)
{
    if (t < SLOTS) { *count = COUNT(slot_fields); return slot_fields; }
    if (t == TARGET_PALETTE) { *count = COUNT(palette_fields); return palette_fields; }
    *count = COUNT(cal_fields);
    return cal_fields;
}

static u8 *values_of(u8 t)
{
    return t < SLOTS ? (u8 *)&slots[t] : t == TARGET_PALETTE ? (u8 *)palette : (u8 *)&cal;
}

static u16 key_of(u8 t)
{
    return 1 + t;            /* syscfg user items: slots 1-8, palette 9, calibration 10 */
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

static u32 enum_count(const struct set_field *f)
{
    u32 n = 0;

    while (f->names[n]) n++;
    return n;
}

static u32 field_max(const struct set_field *f)
{
    return f->type == T_ENUM ? enum_count(f) - 1 : f->type == T_COLOUR ? PALETTE - 1 : f->max;
}

static u8 check(const struct set_field *f, const u8 *v, u32 len)
{
    u32 x;

    if (!f) return SET_UNKNOWN;
    if (!len) return SET_OK;
    if (len != f->size && !(f->id == ID_BOND && len == BT_BOND_V1_SIZE)) return SET_BAD_LENGTH;
    if (f->type == T_BYTES && (f->id == ID_LEFT_POINTS || f->id == ID_RIGHT_POINTS)) {
        for (x = 1; x < len; x++) if (v[x] < v[x - 1]) return SET_RANGE;
    }
    switch (f->type) {
    case T_ENUM:
    case T_NUMBER:
        x = f->size == 1 ? v[0] : v[0] | (u32)v[1] << 8;
        return x <= field_max(f) ? SET_OK : SET_RANGE;
    case T_COLOUR:
        return v[0] < PALETTE && v[1] < LEVELS ? SET_OK : SET_RANGE;
    default:
        return SET_OK;
    }
}

static void touch(u8 t)
{
    if (ephemeral) return;
    dirty |= (u16)(1u << t);
    changed_ms = plat_ms();
}

/* A field back to its default (len 0) or set to v. */
static void put(u8 t, const struct set_field *f, u32 bit, const u8 *v, u32 len)
{
    u8 *p = values_of(t) + f->offset;
    u32 def = f->def[t < SLOTS ? t : 0];

    if (len) {
        memset(p, 0, f->size);
        memcpy(p, v, len < f->size ? len : f->size);
        set_bits[t] |= 1u << bit;
    } else {
        memset(p, 0, f->size);
        memcpy(p, &def, f->size < 4 ? f->size : 4);
        if (t < SLOTS && (f->id == ID_LEFT_POINTS || f->id == ID_RIGHT_POINTS))
            memcpy(p, linear_points, sizeof(linear_points));
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

/* Id 0 from outside: a slot's pairing (address, bond) is kept, unless the
 * type changes with the reset. */
static void reset_keeping_pairing(u8 t)
{
    u32 n, i;
    const struct set_field *f = fields_of(t, &n);
    u8 type = slots[t % SLOTS].type;

    if (t >= SLOTS) {
        reset(t);
        return;
    }
    for (i = 0; i < n; i++)
        if (f[i].id != ID_ADDRESS && f[i].id != ID_BOND) put(t, &f[i], i, NULL, 0);
    if (slots[t].type != type) {
        clear(t, ID_ADDRESS);
        clear(t, ID_BOND);
    }
}

/* ---- records ------------------------------------------------------------------
 * The set fields of one target, {id, len, value} each, behind
 * {u16 magic, u8 version, u8 used}.  An unknown id or a value out of range is
 * dropped on load, so rows can come and go without invalidating the rest. */

static u32 encode(u8 t, u8 *out)
{
    u32 n, i, used = 4;
    const struct set_field *f = fields_of(t, &n);

    for (i = 0; i < n; i++) {
        if (!(set_bits[t] & (1u << i))) continue;
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

static void decode(u8 t, const u8 *in, u32 len)
{
    u32 used, o, bit;

    if (len < 4 || (in[0] | in[1] << 8) != MAGIC || in[2] != VERSION) return;
    used = in[3] < len ? in[3] : len;
    for (o = 4; o + 2 <= used && o + 2u + in[o + 1] <= used; o += 2u + in[o + 1]) {
        const struct set_field *f = find(t, in[o], &bit);

        if (in[o + 1] && check(f, in + o + 2, in[o + 1]) == SET_OK)
            put(t, f, bit, in + o + 2, in[o + 1]);
    }
}

void set_init(void)
{
    u8 buf[RECORD_MAX];
    u8 t;
    int n;

    for (t = 0; t < TARGETS; t++) {
        reset(t);
        memset(buf, 0, sizeof(buf));
        n = plat_store_read(key_of(t), buf, sizeof(buf));
        if (n > 0) decode(t, buf, (u32)n);
    }
    if (!(set_bits[TARGET_CAL] & 1)) util_log("settings: calibration required");
}

static void write_dirty(void)
{
    u8 buf[RECORD_MAX];
    u8 t;

    for (t = 0; t < TARGETS; t++)
        if (dirty & (1u << t)) {
            memset(buf, 0, sizeof(buf));
            encode(t, buf);
            plat_store_write(key_of(t), buf, sizeof(buf));
        }
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
    return &slots[slot % SLOTS];
}

/* Brightness levels: the LEDs are bright, so the low end goes in single
 * steps and the rest about 1.4 times apart. */
static const u8 levels[LEVELS] = { 0, 1, 2, 3, 4, 5, 7, 10, 14, 20, 28, 40, 56, 80, 128, 255 };

struct rgb set_colour(struct colour c)
{
    struct rgb p = palette[c.entry % PALETTE], o;
    u32 level = levels[c.level % LEVELS];

    o.r = (u8)((p.r * level + 127) / 255);
    o.g = (u8)((p.g * level + 127) / 255);
    o.b = (u8)((p.b * level + 127) / 255);
    return o;
}

const struct bt_bond *set_bond(u8 slot)
{
    return &slots[slot % SLOTS].bond;
}

void set_bond_store(u8 slot, const struct bt_bond *bond)
{
    struct set_slot *s = &slots[slot % SLOTS];
    u32 bit;
    const struct set_field *f = find(slot % SLOTS, ID_BOND, &bit);
    static const u8 zero[6];
    u8 a[6];

    if (!memcmp(&s->bond, bond, sizeof(*bond))) return;
    put(slot % SLOTS, f, bit, (const u8 *)bond, sizeof(*bond));
    if (!memcmp(s->address, zero, 6)) {
        set_slot_address(slot, a);
        f = find(slot % SLOTS, ID_ADDRESS, &bit);
        put(slot % SLOTS, f, bit, a, 6);
    }
    touch(slot % SLOTS);
}

static u32 fnv1a(u32 h, const u8 *d, u32 n)
{
    while (n--) h = (h ^ *d++) * 16777619u;
    return h;
}

/* A slot's own address: pinned once it bonded, else derived from the flash ID,
 * the slot and its type, locally administered.  Least significant byte first. */
void set_slot_address(u8 slot, u8 out[6])
{
    static const u8 zero[16];
    const u8 *seed = plat_unique_id();
    /* Through the dongle the type can change under the same host. */
    u8 salt[3] = { (u8)(slot % SLOTS), set_slot(slot)->transport == TRANSPORT_DONGLE ? 0xff :
                                       set_slot(slot)->type, 0 };
    u32 a, b;

    if (memcmp(slots[slot % SLOTS].address, zero, 6)) {
        memcpy(out, slots[slot % SLOTS].address, 6);
        return;
    }
    if (!seed) seed = zero;
    a = fnv1a(fnv1a(2166136261u, seed, 16), salt, 3);
    b = fnv1a(fnv1a(0x811c9dc5u ^ 0x5a5a5a5au, salt, 3), seed, 16);
    memcpy(out, &a, 4);
    memcpy(out + 4, &b, 2);
    out[5] = (u8)((out[5] | 0x02) & 0xfe);
}

/* A new address for the slot, not stored: a host that still holds a key for
 * the old one sees a pad it has never met. */
void set_slot_new_address(u8 slot, u8 out[6])
{
    u32 h;

    set_slot_address(slot, out);
    h = fnv1a(fnv1a(2166136261u, out, 6), out, 6) ^ plat_ms();
    out[0] = (u8)h;
    out[1] = (u8)(h >> 8);
    out[2] = (u8)(h >> 16);
}

/* A pairing that succeeded: the address it ran at and its bond, together.
 * Until then the slot keeps its old ones. */
void set_pairing_store(u8 slot, const u8 address[6], const struct bt_bond *bond)
{
    u32 bit;
    const struct set_field *f = find(slot % SLOTS, ID_ADDRESS, &bit);

    put(slot % SLOTS, f, bit, address, 6);
    touch(slot % SLOTS);
    set_bond_store(slot, bond);
}

const struct set_cal *set_cal(void)
{
    return &cal;
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
 * commit applies them in order. */

u32 set_write(const u8 *in, u32 len, u8 *results, u32 max)
{
    u32 o = 0, n = 0, bit;

    while (o + 3 <= len && n < max) {
        u8 t = in[o], id = in[o + 1], l = in[o + 2];
        const struct set_field *f = t < TARGETS ? find(t, id, &bit) : NULL;
        u8 r;

        if (o + 3u + l > len) { results[n++] = SET_BAD_LENGTH; break; }
        r = t >= TARGETS ? SET_UNKNOWN : id == 0 ? (l ? SET_BAD_LENGTH : SET_OK)
                                                 : check(f, in + o + 3, l);
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

        if (id == 0) {
            reset_keeping_pairing(t);
        } else {
            f = find(t, id, &bit);
            u8 v = (u8)(l ? stage[o + 3] : f->def[t]);

            /* A new type is a new device to the console: the old address
             * and bond belong to the old one.  Through the dongle the host
             * stays the dongle, whatever the type; moving to or from the
             * dongle changes the host. */
            if (t < SLOTS && ((id == ID_TYPE && v != slots[t].type &&
                               slots[t].transport != TRANSPORT_DONGLE) ||
                              (id == ID_TRANSPORT && (v == TRANSPORT_DONGLE) !=
                               (slots[t].transport == TRANSPORT_DONGLE)))) {
                clear(t, ID_ADDRESS);
                clear(t, ID_BOND);
            }
            put(t, f, bit, stage + o + 3, l);
        }
        touch(t);
    }
    if (staged) util_log("settings: %u changes%s", staged_items, ephemeral ? " (ephemeral)" : "");
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

/* Field number n over all targets: slots, then palette, then calibration.
 * [0] version [1] target kind [2] id [3] type [4] size [6..7] max, then
 * name, help, the enum names (all NUL-terminated) and the defaults (one per
 * slot for slot fields).  0: past the last. */
u32 set_describe(u32 n, u8 *out, u32 max)
{
    u32 kinds[3] = { COUNT(slot_fields), COUNT(palette_fields), COUNT(cal_fields) };
    static const struct set_field *const tables[3] = { slot_fields, palette_fields, cal_fields };
    const struct set_field *f;
    u32 kind = 0, o, i, defs;

    while (kind < 3 && n >= kinds[kind]) n -= kinds[kind++];
    if (kind == 3 || max < 8) return 0;
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
        u32 size = f->size < 4 ? f->size : 0;

        if (o + size > max) return 0;
        memcpy(out + o, &f->def[i], size);
        o += size;
    }
    return o <= max ? o : 0;
}

/* [0] version [1] target [2] field count, then per field: id, set (0/1),
 * len, value. */
u32 set_read(u8 t, u8 *out, u32 max)
{
    u32 n, i, o = 3;
    const struct set_field *f;

    if (t >= TARGETS || max < 3) return 0;
    f = fields_of(t, &n);
    for (i = 0; i < n; i++) {
        if (o + 3u + f[i].size > max) return 0;
        out[o] = f[i].id;
        out[o + 1] = (set_bits[t] >> i) & 1;
        out[o + 2] = f[i].size;
        memcpy(out + o + 3, values_of(t) + f[i].offset, f[i].size);
        o += 3u + f[i].size;
    }
    out[0] = VERSION;
    out[1] = t;
    out[2] = (u8)n;
    return o;
}

#ifdef UNITTEST
static u8 store[KEYS][RECORD_MAX];
static int store_writes;
static u16 read_size = RECORD_MAX;
static u32 ms;
static const u8 uid[16] = { 9, 8, 7 };

int plat_store_read(u16 key, void *buf, u16 len)
{
    if (len > read_size) len = read_size;
    memcpy(buf, store[key], len);
    return len;
}
int plat_store_write(u16 key, const void *buf, u16 len)
{
    memcpy(store[key], buf, len < RECORD_MAX ? len : RECORD_MAX);
    store_writes++;
    return len;
}
u32 plat_ms(void) { return ms; }
const u8 *plat_unique_id(void) { return uid; }
void util_log(const char *fmt, ...) { (void)fmt; }
void util_put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

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

int main(void)
{
    u8 buf[512], a[6], b[6], n[6];
    struct bt_bond bond;
    u8 x;
    u16 speed = 3000, huge = 60000;
    struct colour col = { 4, 5 };
    u32 t, worst = 4;

    /* Every target fits a record with every field set. */
    for (t = 0; t < 3; t++) {
        u32 n, i, used = 4;
        const struct set_field *f = fields_of(t == 0 ? 0 : t == 1 ? TARGET_PALETTE : TARGET_CAL, &n);

        for (i = 0; i < n; i++) used += 2u + f[i].size;
        if (used > worst) worst = used;
    }
    CHECK(worst <= RECORD_MAX && worst <= 255);
    CHECK(COUNT(slot_fields) <= 32 && sizeof(struct set_slot) <= 256);

    set_init();
    CHECK(set_slot(0)->type == PROFILE_SWITCH && set_slot(4)->type == PROFILE_MOUSE);
    CHECK(set_slot(1)->rumble == 100 && set_slot(7)->stick_speed == 1500);
    CHECK(set_slot(0)->connect_colour.entry == 3 && set_slot(0)->connect_colour.level == 5);
    {
        struct rgb c = set_colour(set_slot(2)->connect_colour);   /* today's X: 3,0,5 */
        CHECK(c.r == 3 && c.g == 0 && c.b == 5);
        c = set_colour(set_slot(6)->connect_colour);              /* left: 4,0,3 */
        CHECK(c.r == 4 && c.g == 0 && c.b == 3);
    }

    store_writes = 0;

    /* Collected, not applied, until commit; then stored. */
    x = 60;
    CHECK(write1(1, 7, 1, &x) == SET_OK);
    CHECK(write1(1, 9, 2, &speed) == SET_OK);
    CHECK(write1(1, 9, 2, &huge) == SET_RANGE);
    CHECK(write1(1, 99, 1, &x) == SET_UNKNOWN);
    CHECK(write1(1, 7, 2, &speed) == SET_BAD_LENGTH);
    x = PROFILE_COUNT;
    CHECK(write1(1, 1, 1, &x) == SET_RANGE);
    CHECK(write1(TARGETS, 1, 1, &x) == SET_UNKNOWN);
    CHECK(set_staged() == 2 && set_slot(1)->rumble == 100);
    set_apply();
    CHECK(set_slot(1)->rumble == 60 && set_slot(1)->stick_speed == 3000 && set_staged() == 0);
    set_poll(ms, 1);
    CHECK(store_writes == 1);

    /* Back from the store; a default with nothing set; reset. */
    memset(slots, 0, sizeof(slots));
    read_size = 160;   /* existing firmware records return their shorter length */
    set_init();
    read_size = RECORD_MAX;
    CHECK(set_slot(1)->rumble == 60 && set_slot(1)->stick_speed == 3000);
    CHECK(set_slot(0)->rumble == 100);
    CHECK(write1(1, 7, 0, NULL) == SET_OK);
    set_apply();
    CHECK(set_slot(1)->rumble == 100 && set_slot(1)->stick_speed == 3000);
    CHECK(write1(1, 0, 0, NULL) == SET_OK);
    set_apply();
    CHECK(set_slot(1)->stick_speed == 1500 && set_bits[1] == 0);
    col.level = LEVELS;
    CHECK(write1(1, 2, 2, &col) == SET_RANGE);
    col.level = 5;
    set_poll(ms, 1);

    /* Curves: defaults, validation, staging, persistence and reset. */
    {
        u8 points[STICK_CURVE_POINTS] = { 0, 10, 30, 60, 100, 180, 255 };
        u8 mode = STICK_CUSTOM;
        CHECK(set_slot(1)->left_curve == STICK_LINEAR);
        CHECK(!memcmp(set_slot(1)->left_curve_points, linear_points, sizeof(points)));
        CHECK(write1(1, 31, 1, &mode) == SET_OK);
        CHECK(write1(1, ID_LEFT_POINTS, sizeof(points), points) == SET_OK);
        CHECK(write1(1, ID_RIGHT_POINTS, sizeof(points) - 1, points) == SET_BAD_LENGTH);
        points[3] = 1;
        CHECK(write1(1, ID_RIGHT_POINTS, sizeof(points), points) == SET_RANGE);
        mode = STICK_CUSTOM + 1;
        CHECK(write1(1, 32, 1, &mode) == SET_RANGE);
        CHECK(set_slot(1)->left_curve == STICK_LINEAR);
        set_apply();
        set_poll(ms, 1);
        set_init();
        CHECK(set_slot(1)->left_curve == STICK_CUSTOM);
        CHECK(set_slot(1)->left_curve_points[3] == 60);
        CHECK(set_slot(1)->right_curve == STICK_LINEAR);
        CHECK(write1(1, ID_LEFT_POINTS, 0, NULL) == SET_OK);
        CHECK(write1(1, 31, 0, NULL) == SET_OK);
        set_apply();
        CHECK(set_slot(1)->left_curve == STICK_LINEAR);
        CHECK(!memcmp(set_slot(1)->left_curve_points, linear_points, sizeof(points)));
    }

    /* An unknown id in a record is skipped. */
    store[key_of(2)][3] = 4 + 3 + 3;
    store[key_of(2)][0] = (u8)MAGIC;
    store[key_of(2)][1] = MAGIC >> 8;
    store[key_of(2)][2] = VERSION;
    store[key_of(2)][4] = 200; store[key_of(2)][5] = 1; store[key_of(2)][6] = 1;
    store[key_of(2)][7] = 7; store[key_of(2)][8] = 1; store[key_of(2)][9] = 42;
    set_init();
    CHECK(set_slot(2)->rumble == 42);

    /* Pairing pins the address; a new type clears address and bond. */
    memset(&bond, 0x11, sizeof(bond));
    set_slot_address(0, a);
    set_bond_store(0, &bond);
    set_slot_address(0, b);
    CHECK(!memcmp(a, b, 6) && set_bits[0] == (1u << 12 | 1u << 13));
    x = PROFILE_SWITCH;
    CHECK(write1(0, 1, 1, &x) == SET_OK);      /* the same type: kept */
    set_apply();
    CHECK(set_bond(0)->peer[0] == 0x11);
    CHECK(write1(0, 0, 0, NULL) == SET_OK);    /* reset, the same type: pairing kept */
    set_apply();
    CHECK(set_bond(0)->peer[0] == 0x11 && set_bits[0] == (1u << 12 | 1u << 13));
    x = PROFILE_XBOX;
    CHECK(write1(0, 1, 1, &x) == SET_OK);
    set_apply();
    set_slot_address(0, b);
    CHECK(set_bond(0)->peer[0] == 0 && memcmp(a, b, 6) && set_slot(0)->type == PROFILE_XBOX);
    /* ... and an address after the type stays. */
    memset(a, 0x22, 6);
    CHECK(write1(0, 1, 1, &x) == SET_OK && write1(0, 12, 6, a) == SET_OK);
    set_apply();
    set_slot_address(0, b);
    CHECK(!memcmp(a, b, 6));
    /* A new address is only a candidate; a pairing stores it with its bond. */
    set_slot_new_address(0, b);
    CHECK(memcmp(a, b, 6));
    set_slot_address(0, n);
    CHECK(!memcmp(a, n, 6));
    set_pairing_store(0, b, &bond);
    set_slot_address(0, n);
    CHECK(!memcmp(b, n, 6) && set_bond(0)->peer[0] == 0x11);

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
        slots[1].transport = TRANSPORT_BT;
        set_slot_address(1, d2);
        CHECK(memcmp(d1, d2, 6));
    }

    /* Palette and colours. */
    CHECK(write1(TARGET_PALETTE, 5, 3, "\x10\x20\x30") == SET_OK);
    CHECK(write1(0, 2, 2, &col) == SET_OK);
    col.entry = PALETTE;
    CHECK(write1(0, 2, 2, &col) == SET_RANGE);
    set_apply();
    CHECK(palette[4].r == 0x10 && set_slot(0)->connect_colour.entry == 4);

    /* Ephemeral: applied, never stored, pairing included. */
    set_poll(ms, 1);
    t = (u32)store_writes;
    set_ephemeral();
    x = 10;
    CHECK(write1(3, 7, 1, &x) == SET_OK);
    set_apply();
    set_bond_store(3, &bond);
    set_cal_store(&cal);
    set_poll(ms + 1000, 1);
    set_flush();
    CHECK(set_slot(3)->rumble == 10 && store_writes == (int)t);


    /* Describe and read. */
    CHECK(set_describe(0, buf, sizeof(buf)) > 8 && buf[2] == 1 && !strcmp((char *)buf + 8, "type"));
    CHECK(set_describe(COUNT(slot_fields), buf, sizeof(buf)) && buf[1] == 1);
    CHECK(set_describe(COUNT(slot_fields) + PALETTE, buf, sizeof(buf)) && buf[1] == 2);
    CHECK(set_describe(COUNT(slot_fields) + PALETTE + COUNT(cal_fields), buf, sizeof(buf)) == 0);
    for (t = 0; t < COUNT(slot_fields) + PALETTE + COUNT(cal_fields); t++) CHECK(set_describe(t, buf, 512) > 0);
    CHECK(set_read(3, buf, sizeof(buf)) > 3 && buf[2] == COUNT(slot_fields));
    CHECK(set_read(TARGET_CAL, buf, sizeof(buf)) == 6 + cal_fields[0].size + 4 * 4);

    /* Deadzones are independent fields, never overwritten by calibration. */
    x = 26;
    CHECK(write1(TARGET_CAL, 2, 1, &x) == SET_RANGE);
    x = 5;
    CHECK(write1(TARGET_CAL, 2, 1, &x) == SET_OK);
    set_apply();
    CHECK(set_cal()->deadzone[0].inner == 5 && !set_cal()->deadzone[0].outer);
    {
        struct set_cal measured = cal;
        measured.deadzone[0].inner = 0;
        set_cal_store(&measured);
        CHECK(set_cal()->deadzone[0].inner == 5);
        u32 used = encode(TARGET_CAL, buf);
        reset(TARGET_CAL);
        CHECK(!set_cal()->deadzone[0].inner);
        decode(TARGET_CAL, buf, used);
        CHECK(set_cal()->deadzone[0].inner == 5);
    }

    /* A bond stored before the identity fields still loads, zero-extended. */
    memset(&bond, 0x22, sizeof(bond));
    CHECK(write1(6, ID_BOND, BT_BOND_V1_SIZE, &bond) == SET_OK);
    CHECK(write1(6, ID_BOND, BT_BOND_V1_SIZE + 1, &bond) == SET_BAD_LENGTH);
    set_apply();
    CHECK(set_bond(6)->ltk[15] == 0x22 && set_bond(6)->le_irk[0] == 0 && set_bond(6)->le_id_type == 0);
    return TEST_DONE("settings");
}
#endif
