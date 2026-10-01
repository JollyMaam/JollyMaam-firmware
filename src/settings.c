#include <string.h>
#include "pad.h"

/* Preferences, for now constants; bonds and calibration are stored. */

static const struct set_prefs prefs = {
    .idle_colour = { 5, 5, 5 },
    .battery_colour = { 5, 2, 0 },
};

#define SLOT(t, r, g, b) { .type = t, .rumble = 1, .grips = GRIPS_LIGHTBAR, .colour = { r, g, b }, \
                         .stick_speed = 1500, .gyro_speed = 32, .gyro_accel = 250 }

static const struct set_slot slots[SLOTS] = {
    SLOT(PROFILE_SWITCH, 0, 0, 5),      /* A: blue */
    SLOT(PROFILE_SWITCH, 5, 0, 0),      /* B: red */
    SLOT(PROFILE_GENERIC, 3, 0, 5),     /* X: purple */
    SLOT(PROFILE_SWITCH2, 0, 5, 0),     /* Y: green */
    SLOT(PROFILE_MOUSE, 0, 4, 4),       /* up: cyan */
    SLOT(PROFILE_MOUSE, 4, 4, 0),       /* down: yellow */
    SLOT(PROFILE_MOUSE, 4, 0, 3),       /* left: pink */
    SLOT(PROFILE_MOUSE, 2, 4, 0),       /* right: lime */
};

const struct set_slot *set_slot(u8 slot)
{
    return &slots[slot % SLOTS];
}

const struct set_prefs *set_prefs(void)
{
    return &prefs;
}

/* ---- stored records --------------------------------------------------------
 * Each record is a header and a whole struct.  A record that does not match
 * the struct (another version or size) is ignored: defaults instead. */

#define MAGIC        0x5032
#define BOND_VERSION 1
#define CAL_VERSION  1
#define KEY_CAL      49
#define SETTLE_MS    100

struct head { u16 magic; u8 version; u8 size; };

struct bond_record {
    struct head h;
    struct bt_bond bond;
    u8 address[6];           /* pinned when the slot bonded; zero: derived */
};

struct cal_record {
    struct head h;
    struct set_cal cal;
};

static struct bond_record bonds[SLOTS];
static struct cal_record cal;
static u16 dirty;            /* a bit per slot, and bit 15 for calibration */
static u32 changed_ms;
static u8 commit_now;

static u16 bond_key(u8 slot)
{
    return slot < 4 ? 34 + slot : 52 + slot - 4;
}

static int load(u16 key, void *rec, u16 size, u8 version)
{
    struct head *h = rec;

    if (plat_store_read(key, rec, size) == size && h->magic == MAGIC &&
        h->version == version && h->size == (u8)size)
        return 1;
    memset(rec, 0, size);
    h->magic = MAGIC;
    h->version = version;
    h->size = (u8)size;
    return 0;
}

void set_init(void)
{
    u8 i;

    for (i = 0; i < SLOTS; i++) load(bond_key(i), &bonds[i], sizeof(bonds[i]), BOND_VERSION);
    if (!load(KEY_CAL, &cal, sizeof(cal), CAL_VERSION)) util_log("settings: calibration required");
}

static void touch(u16 bit)
{
    dirty |= bit;
    changed_ms = plat_ms();
}

static void write_dirty(void)
{
    u8 i;

    for (i = 0; i < SLOTS; i++)
        if (dirty & (1u << i)) plat_store_write(bond_key(i), &bonds[i], sizeof(bonds[i]));
    if (dirty & 0x8000) plat_store_write(KEY_CAL, &cal, sizeof(cal));
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

void set_commit(void)
{
    write_dirty();
}

const struct bt_bond *set_bond(u8 slot)
{
    return &bonds[slot % SLOTS].bond;
}

void set_bond_store(u8 slot, const struct bt_bond *bond)
{
    struct bond_record *r = &bonds[slot % SLOTS];
    static const u8 zero[6];

    if (!memcmp(&r->bond, bond, sizeof(*bond))) return;
    r->bond = *bond;
    if (!memcmp(r->address, zero, 6)) set_slot_address(slot, r->address);
    touch(1u << (slot % SLOTS));
}

void set_bond_forget(u8 slot)
{
    memset(&bonds[slot % SLOTS].bond, 0, sizeof(struct bt_bond));
    touch(1u << (slot % SLOTS));
}

static u32 fnv1a(u32 h, const u8 *d, u32 n)
{
    while (n--) h = (h ^ *d++) * 16777619u;
    return h;
}

/* A slot's own address: pinned once it bonded, else derived from the flash ID
 * and the slot, locally administered.  Least significant byte first. */
void set_slot_address(u8 slot, u8 out[6])
{
    static const u8 zero[16];
    const u8 *seed = plat_unique_id();
    u8 salt[3] = { (u8)(slot % SLOTS), set_slot(slot)->type, 0 };
    u32 a, b;

    if (memcmp(bonds[slot % SLOTS].address, zero, 6)) {
        memcpy(out, bonds[slot % SLOTS].address, 6);
        return;
    }
    if (!seed) seed = zero;
    a = fnv1a(fnv1a(2166136261u, seed, 16), salt, 3);
    b = fnv1a(fnv1a(0x811c9dc5u ^ 0x5a5a5a5au, salt, 3), seed, 16);
    memcpy(out, &a, 4);
    memcpy(out + 4, &b, 2);
    out[5] = (u8)((out[5] | 0x02) & 0xfe);
}

/* A new address for the slot: a host that still holds a key for the old one
 * sees a pad it has never met. */
void set_slot_new_address(u8 slot)
{
    struct bond_record *r = &bonds[slot % SLOTS];
    u8 a[6];
    u32 h;

    set_slot_address(slot, a);
    h = fnv1a(fnv1a(2166136261u, a, 6), a, 6) ^ plat_ms();
    a[0] = (u8)h;
    a[1] = (u8)(h >> 8);
    a[2] = (u8)(h >> 16);
    memcpy(r->address, a, 6);
    touch(1u << (slot % SLOTS));
}

const struct set_cal *set_cal(void)
{
    return &cal.cal;
}

void set_cal_store(const struct set_cal *c)
{
    cal.cal = *c;
    touch(0x8000);
    commit_now = 1;
}
