#ifndef PAD_H
#define PAD_H

#ifdef UNITTEST
#include "test.h"
#endif

/* The SDK's own definitions, spelled out so that files using BTstack (which
 * has its own bool) need no SDK header.  Identical typedefs may repeat. */
typedef unsigned char u8;
typedef char s8;
typedef unsigned short u16;
typedef signed short s16;
typedef unsigned int u32;
typedef signed int s32;
typedef long long s64;
typedef unsigned long long u64;

#include <stddef.h>

#ifdef UNITTEST
#define RETAINED
#else
#define RETAINED __attribute__((section(".retained"), used, aligned(4)))
#endif

#define RGB(r,g,b) { r, g, b }
struct rgb { u8 r, g, b; };
/* A colour at full strength and how bright it is lit: tenths of the LEDs'
 * smallest step, 0-2550 (10: 1 of 255). */
struct shade { struct rgb rgb; u16 brightness; };

#define SLOTS     8
#define SLOT_NONE 0xff
#define SLOT_LAST 0xfe

/* ---- input.c ------------------------------------------------------------ */

enum {
    PAD_A = 1 << 0, PAD_B = 1 << 1, PAD_SCREENSHOT = 1 << 2, PAD_X = 1 << 3,
    PAD_Y = 1 << 4, PAD_M = 1 << 5, PAD_L1 = 1 << 6, PAD_R1 = 1 << 7,
    PAD_L4 = 1 << 8, PAD_R4 = 1 << 9, PAD_SELECT = 1 << 10, PAD_START = 1 << 11,
    PAD_HOME = 1 << 12, PAD_L3 = 1 << 13, PAD_R3 = 1 << 14,
    PAD_UP = 1 << 16, PAD_RIGHT = 1 << 17, PAD_DOWN = 1 << 18, PAD_LEFT = 1 << 19,
};
#define PAD_FACE (PAD_A | PAD_B | PAD_X | PAD_Y)
#define PAD_DPAD (PAD_UP | PAD_RIGHT | PAD_DOWN | PAD_LEFT)
#define PAD_DIGITAL (0x7fffu | PAD_DPAD)  /* every button and direction */

/* The d-pad as a host hat value, for formats that carry one. */
enum { HAT_UP, HAT_UP_RIGHT, HAT_RIGHT, HAT_DOWN_RIGHT, HAT_DOWN, HAT_DOWN_LEFT,
       HAT_LEFT, HAT_UP_LEFT, HAT_NEUTRAL };

static inline u8 in_hat(u32 b)
{
    u32 right = b & PAD_RIGHT, left = b & PAD_LEFT;
    if (b & PAD_UP) return right ? HAT_UP_RIGHT : left ? HAT_UP_LEFT : HAT_UP;
    if (b & PAD_DOWN) return right ? HAT_DOWN_RIGHT : left ? HAT_DOWN_LEFT : HAT_DOWN;
    return right ? HAT_RIGHT : left ? HAT_LEFT : HAT_NEUTRAL;
}

#define STICK_MAX   32767
#define TRIGGER_MAX 65535u

struct in_raw {
    u32 buttons;
    u8 sticks_valid;
    u16 stick[4];
    u16 trigger[2];
    u8 trigger_stop[2];
    u32 changed_us;              /* when a button or the d-pad last changed (plat_us) */
};

struct imu_sample {
    s16 accel[3];
    s16 gyro[3];
    u32 us;                      /* when it was read */
};

#define IMU_PER_REPORT 4

struct pad {
    u32 buttons;                 /* PAD_*: opposite d-pad directions cancel */
    s16 lx, ly, rx, ry;
    u16 l2, r2;
    u8 imu_count;                /* samples new since the last report, oldest first */
    struct imu_sample imu[IMU_PER_REPORT];
    u8 jack;                     /* a plug in the headphone jack */
    /* The gyro's zero-rate offset as learned so far (motion.c, or the pad
     * through the link), 1/16 LSB; known 0: none yet. */
    s32 gyro_bias[3];
    u8 gyro_bias_known;
};
#define PAD_CONTROLS_SIZE ((u32)(size_t)&((struct pad *)0)->imu_count)

/* Opposite d-pad directions cancel. */
static inline u32 in_dpad(u32 b)
{
    if ((b & PAD_UP) && (b & PAD_DOWN)) b &= ~(u32)(PAD_UP | PAD_DOWN);
    if ((b & PAD_LEFT) && (b & PAD_RIGHT)) b &= ~(u32)(PAD_LEFT | PAD_RIGHT);
    return b;
}

void in_map(const struct in_raw *raw, struct pad *pad);
int in_moved(const struct pad *now, const struct pad *ref);

/* ---- motion.c, and the board's sensor -------------------------------------
 * Samples are in the Cyclone 2's sensor units: gyro 16 LSB per degree per
 * second, accelerometer 8192 per g. */

enum { IMU_OFF, IMU_MOTION, IMU_GYRO };

struct imu_pointer {
    s64 motion[2];               /* what is left under a count */
    u32 last_us;
    u8 have;
};

/* The board's: advances its motion sensor (a board without one returns 0);
 * returns its next deadline, 0 none. */
u32 imu_poll(u32 now, u8 mode);
void imu_store(const s16 accel[3], const s16 gyro[3]);   /* a sample from the sensor */
int imu_take(u32 *seq, struct imu_sample *out, int max);
int imu_gyro_bias(s32 out[3]);   /* the learned zero-rate offset, 1/16 LSB; 0: none yet */
/* The gyro samples in a report as pointer counts, with the pad's gyro_bias;
 * p carries what is left between calls. */
void imu_pointer(struct imu_pointer *p, const struct pad *pad, u16 per_degree, u16 accel,
                 s32 *x, s32 *y);

/* ---- input_calibration.c ------------------------------------------------
 * Sensor measurements only; deadzones (radial percentages) belong to the
 * remap slot in use.  The measurement blob keeps its layout; legacy stick
 * deadzones are ignored.  Trigger calibration is unchanged. */

#define CAL_GATE_KNOTS 16

struct cal_stick {
    u16 minimum[2], center[2], maximum[2];
    u16 gate[CAL_GATE_KNOTS];
    u16 legacy_inner_deadzone, legacy_outer_deadzone;
};

struct cal_trigger { u16 rest, full, inner_deadzone, outer_deadzone; };

struct cal_deadzone { u8 inner, outer; }; /* percent, each 0..25 */

struct set_cal {
    struct cal_stick stick[2];
    struct cal_trigger trigger[2];
};

enum { CAL_IDLE, CAL_RUNNING, CAL_SAVE };

/* deadzone: per stick, NULL none. */
void cal_apply(const struct set_cal *cal, const struct cal_deadzone *deadzone,
               const struct in_raw *raw, struct pad *pad);
void cal_start(u32 now);
int cal_step(const struct in_raw *raw, u32 now, struct set_cal *cal);
int cal_led(u32 now, struct rgb *out);

/* ---- controller.c ------------------------------------------------------- */

enum ctl_kind {
    CTL_BOOT, CTL_IDLE, CTL_CONNECTING, CTL_PAIRING, CTL_CONNECTED,
    CTL_STOPPING, CTL_SLEEPING, CTL_REBOOTING, CTL_KIND_COUNT,
};

enum req_kind {
    REQ_NONE, REQ_CONNECT, REQ_PAIR, REQ_DISCONNECT, REQ_SLEEP, REQ_CALIBRATE,
    REQ_REBOOT, REQ_REBOOT_LOADER, REQ_HOME_TAP, REQ_STATS_RESET,
    REQ_RELINK,              /* connected: take the session down and connect it again */
    REQ_REMAP,               /* connected: remap slot .slot, SLOT_NONE the base; kept until a disconnect, another slot or sleep */
    REQ_LED_DEMO,                /* the lights show LED demo .slot (ux_led_demos), SLOT_NONE: stop */
    REQ_RUMBLE,              /* the motors run at .motor (0-255 duty) for .ms, over the host's; 0 ms: stop */
    REQ_RUMBLE_STEP,         /* config mode L1/R1: .slot 0 lower, 1 raise the rumble step (RAM only) */
};

struct ctl_request {
    u8 kind;
    u8 slot;
    u8 generate_new_address;
    u8 quiet;                /* a restart the pad made itself: it never wakes a host by USB */
    u8 motor[2];             /* REQ_RUMBLE: left, right */
    u16 ms;                  /* REQ_RUMBLE */
};

#define RUMBLE_TEST_MAX_MS 10000

void ctl_main(void);

/* ---- bt_pad.c (the board's Bluetooth on BTstack) ------------------------ */

enum { BT_CLASSIC, BT_LE };

struct bt_bond {
    u8 peer[6];                  /* least significant byte first; all zero: none */
    u8 link_key[16];
    u8 key_type;
    u8 le_peer[6];
    u8 ltk[16];
    /* LE Secure Connections: the host's identity from the pairing, which
     * with ltk restores the bond to the security manager.  Zero after a
     * legacy pairing, whose key the security manager derives again. */
    u8 le_irk[16];
    u8 le_id[6];                 /* least significant byte first */
    u8 le_id_type;
};

struct bt_device {
    u8 transport;
    const char *name;
    /* Classic */
    u32 class_of_device;
    u16 vendor_id, product_id, version;
    const u8 *hid_descriptor;
    u16 hid_descriptor_len;
    u16 sniff_slots, sniff_attempt;
    /* LE */
    const u8 *att_db;
    u16 input_handle;
    u8 (*advertising)(const struct bt_bond *bond, int pairing, int wake, u8 out[31]);
    const u8 *scan_response;
    u8 scan_response_len;
    const u8 *sm_er, *sm_ir;     /* security manager keys: NULL, no SMP */
    u16 interval_min, interval_max;
};

/* A host write, from Bluetooth or USB: an output or feature report (id: its
 * report id, data without it), an ATT write (id: the handle), or a USB bulk
 * transfer (Switch 2's command pipe).  The kinds also name what a profile's
 * read() answers; BT_WRITE_VENDOR only there: a USB vendor request on EP0
 * (id: bRequest, offset: wValue; max 0 for one without data to the host,
 * accepted when read() returns 0). */
#define BT_WRITE_MAX 96
enum { BT_WRITE_OUTPUT, BT_WRITE_FEATURE, BT_WRITE_ATT, BT_WRITE_BULK, BT_WRITE_VENDOR };
struct bt_write {
    u8 kind;
    u16 id;
    u16 len;
    u8 data[BT_WRITE_MAX];
};

enum { BT_EV_NONE, BT_EV_LINKED, BT_EV_UNLINKED, BT_EV_FAILED, BT_EV_STOPPED,
       BT_EV_PEER, BT_EV_BONDED, BT_EV_PAIRED, BT_EV_WRITE };

struct bt_ev {
    u8 kind;
    u8 lost;                     /* BT_EV_UNLINKED: the signal went, not the host */
    u8 reason;                   /* HCI disconnect reason */
    struct bt_bond bond;         /* BT_EV_PEER, BT_EV_BONDED, BT_EV_PAIRED */
    struct bt_write write;       /* BT_EV_WRITE */
};

/* How the link is asked to run, from the slot's settings; 0 in a field: the
 * profile's or the stack's own. */
struct bt_link {
    u16 le_interval_min, le_interval_max;    /* 1.25 ms units, requested after encryption */
    u8 le_phy;               /* 0 leave it; 1 ask for 2M */
    u8 in_flight;            /* input reports in the controller at once */
    u16 poll_us;             /* Classic: Best Effort poll interval requested (us) */
    u8 tx_power;             /* 0-5 shown as 1-6: the controller's steps 0-4, then
                                step 4 from its pro table */
    u16 sniff_max, sniff_min; /* Classic: ask the host for sniff (slots); 0: don't */
    u8 edr3_off;             /* Classic: no 3 Mbit/s EDR packets */
    u16 jit_us;              /* lead before the anchor a report is for: 0 adaptive, 1 none (when asked) */
};

struct profile;
void bt_start(const struct profile *p, const u8 address[6], const struct bt_bond *bond,
              int pairing, const struct bt_link *link);
void bt_stop(void);
void bt_abort(void);
void bt_connect(void);
int bt_send_input(u8 id, const u8 *data, u16 len);
int bt_send_message(u16 id, const u8 *data, u16 len);
int bt_poll(struct bt_ev *ev);
int bt_radio_quiet(void);
int bt_input_pending(void);
int bt_input_wanted(void);
u32 bt_input_wanted_us(void);            /* when it became wanted (plat_us), 0: not wanted */
u32 bt_input_due_us(void);               /* when to build it (plat_us, jit_us), 0: not wanted */
void bt_update_bond(const struct bt_bond *bond);
struct bt_stats;
void bt_stats(struct bt_stats *out);
struct bt_timing;
void bt_timing(struct bt_timing *out, int reset);

/* ---- profile_*.c -------------------------------------------------------- */

struct set_slot;
struct profile_session {
    u8 slot;
    u8 pairing;
    u8 address[6];
    const struct bt_bond *bond;
    const struct set_slot *settings;
    u8 usb;                      /* the session is a wired one: USB report formats */
};

struct profile_host {
    u8 rumble[2];
    u8 has_players, players, players_flash;
    u8 has_lightbar;
    u8 lightbar[3];
    u8 imu;
    u8 ended;                    /* host ended this session: stop locally */
};

/* A profile that can be a wired controller too describes its USB device; the
 * platform builds the descriptors from it.  kind: the interfaces it has. */
enum { USB_HID, USB_XINPUT, USB_HID_BULK };
/* audio: the stock pad's speaker, only while a plug is in the jack; or the
 * Switch 2 Pro Controller's headphones and microphone, always there, each
 * function behind an interface association. */
enum { USB_AUDIO_NONE, USB_AUDIO_SPEAKER, USB_AUDIO_HEADSET };
struct usb_identity {
    u16 vendor_id, product_id, bcd_device;
    const char *manufacturer, *product, *serial;   /* ASCII; NULL: none */
    u8 kind;
    u8 report_ids;               /* HID: the first byte each way is the report id */
    u8 interval;                 /* interrupt endpoints, ms */
    u8 in_size, out_size;        /* their packet sizes; out_size 0: no OUT endpoint */
    u8 max_power;                /* 2 mA units */
    u8 audio;                    /* USB_AUDIO_*: a sound card beside it, on a board with one */
    const u8 *hid_descriptor;
    u16 hid_descriptor_len;
};

struct profile {
    const char *name;
    const struct bt_device *device;
    const struct usb_identity *usb;    /* NULL: Bluetooth only */
    u8 (*begin)(const struct profile_session *s);
    void (*end)(void);
    int (*report)(const struct pad *pad, u32 now, u8 *out, u8 *id);
    int (*write)(const struct bt_write *w, u32 now, struct bt_bond *bond);
    void (*host)(u32 now, struct profile_host *out);
    int (*read)(u8 kind, u16 id, u16 offset, u8 *out, u16 max);
    const struct profile_wake *wake;   /* before connecting a bonded slot */
};

/* A profile's message to the host (a reply, a separate report), over the
 * session's transport, never dropped while there is room.  HOST_BULK in id:
 * USB bulk IN (Switch 2's command pipe).  The pad's controller.c and the
 * dongle's dongle.c each have one. */
#define HOST_BULK 0x8000
int host_send(u16 id, const u8 *data, u16 len);

/* Runs before bt_start() with the radio otherwise quiet. */
struct profile_wake {
    int (*start)(const struct profile_session *s);   /* 0 started, <0 nothing to wake */
    int (*step)(u32 now, u32 *next);  /* 1 finished; else call again at *next */
    void (*stop)(void);
};

extern const struct profile profile_switch, profile_switch2, profile_xbox, profile_xbox_ble;
extern const struct profile profile_dualsense, profile_dualshock4, profile_generic, profile_mouse,
    profile_generic_ble, profile_link;
extern const struct profile_wake profile_switch_wake;
/* The profile for a slot type (PROFILE_*, settings.c); NULL for none.  In
 * profile_link.c, which both boards build. */
const struct profile *profile_for(u8 type);

/* ---- settings.c --------------------------------------------------------- */

enum { PROFILE_SWITCH, PROFILE_SWITCH2, PROFILE_XBOX, PROFILE_XBOX_BLE, PROFILE_DUALSENSE,
       PROFILE_DUALSHOCK4, PROFILE_GENERIC, PROFILE_MOUSE, PROFILE_GENERIC_BLE, PROFILE_NONE, PROFILE_COUNT };
/* How a slot reaches its host: Bluetooth; USB (a wired controller); USB while
 * a USB host is there, else Bluetooth (a USB session that ends does not move
 * back); through our dongle, which is the slot's type on its own USB port; or
 * USB while a USB host is there, else the dongle. */
enum { TRANSPORT_BT, TRANSPORT_USB, TRANSPORT_BT_USB, TRANSPORT_DONGLE, TRANSPORT_DONGLE_USB };

/* bluetooth+usb, dongle+usb: USB while a USB host is there, else the radio. */
static inline int set_usb_first(u8 transport)
{
    return transport == TRANSPORT_BT_USB || transport == TRANSPORT_DONGLE_USB;
}

/* dongle, dongle+usb: the radio's host is our dongle, whatever the type. */
static inline int set_via_dongle(u8 transport)
{
    return transport == TRANSPORT_DONGLE || transport == TRANSPORT_DONGLE_USB;
}
enum { GRIPS_OFF, GRIPS_HOST, GRIPS_FADE, GRIPS_RUMBLE, GRIPS_INPUT, GRIPS_BOTH,
       GRIPS_PRIMARY, GRIPS_SECONDARY, GRIPS_RAINBOW, GRIPS_HEARTBEAT };
enum { PLAYERS_OFF, PLAYERS_HOST, PLAYERS_FADE, PLAYERS_PRIMARY, PLAYERS_SECONDARY, PLAYERS_RAINBOW };

#define PALETTE 16

/* How bright each kind of light is, for the whole pad (the palette's
 * settings): tenths of the LEDs' smallest step (10: 1 of 255). */
struct set_brightness {
    u16 steady;              /* Home and the player lights connected, steady grips */
    u16 animation;           /* animated grips and player lights */
    u16 connect;             /* connecting and pairing */
    u8 dither_ms;            /* dim lights between steps: refresh this often; 0 off */
};

/* Curves are tool-defined point arrays; disabling retains the saved points
 * and passes calibrated input through exactly. */
#define STICK_CURVE_POINTS 15

struct set_slot {
    u8 type;
    u8 transport;
    u8 connect_colour;                   /* palette entries: connecting, pairing; which slot */
    u8 primary_colour;                   /* connected */
    u8 secondary_colour;                 /* the background */
    u8 grips;
    u8 players;
    /* Not stored with the slot: a session's copy takes them from its remap slot. */
    u8 variant;              /* mouse: 0 games, 1 desktop */
    u16 stick_speed;         /* mouse: pointer counts per second, stick all the way */
    u16 gyro_speed;          /* mouse, touchpad: counts per degree turned; 0 off */
    u16 gyro_accel;          /* percent of gyro_speed for fast turns */
    u8 address[6];           /* zero: derived */
    struct bt_bond bond;
    u16 report_hz;           /* the most reports a second; 0: REPORT_HZ */
    u16 pull_delay_us;       /* build a pulled report this long after bt_pad.c asks */
    struct bt_link link;
    u8 remap;                /* the remap slot each connect starts with: 0 none, 1-8 slots A-right */
};


#define RUMBLE_STEPS 5

/* A remap slot: the simple map, then rules for the keys that need more.
 * Inputs and outputs are PAD_* bit numbers (0-19, not 15). */
#define REMAP_NAME   24
#define REMAP_INPUTS 20
#define REMAP_RULES  16
#define REMAP_OFF    0xff        /* map: the key sends nothing */
#define REMAP_MAX_HZ 30          /* turbo rate 0: flips every report */

/* Level triggers (held, held for) go with level effects (hold, turbo,
 * nothing, turbo modifier); the others are events, with tap, toggle hold,
 * toggle turbo, toggle turbo modifier.  A turbo modifier's outputs are input
 * keys: while it is on, whatever they send while held pulses at its rate. */
enum { RT_NONE, RT_HELD, RT_HELD_FOR, RT_PRESSED, RT_RELEASED, RT_TAPPED, RT_DOUBLE, RT_COUNT };
enum { RE_HOLD, RE_TURBO, RE_NOTHING, RE_TAP, RE_TOGGLE_HOLD, RE_TOGGLE_TURBO,
       RE_TURBO_MODIFIER, RE_TOGGLE_TURBO_MODIFIER, RE_COUNT };

static inline int remap_level_effect(u8 e)
{
    return e < RE_TAP || e == RE_TURBO_MODIFIER;
}

struct remap_rule {
    u8 trigger, input;
    u8 ms10;                 /* held for, tapped under, double-tapped within: 10 ms units */
    u8 effect;
    u8 outputs[4];           /* PAD_* mask, little-endian */
    u8 rate;                 /* turbo or turbo modifier Hz; 0: every report */
    u8 reserved;             /* zero: room for a condition key */
};

enum { STICK_SAME, STICK_OTHER, STICK_NOTHING };   /* two on one host stick: the further out wins */

struct set_remap {
    char name[REMAP_NAME];   /* NUL-padded UTF-8 */
    u8 map[REMAP_INPUTS];    /* each key's base row, while held: 0 itself, n the key with bit
                                n - 1, REMAP_OFF nothing; its rules add to it */
    struct remap_rule rule[REMAP_RULES];
    /* The sticks (a second record): curves, disabled = calibrated input exactly. */
    u8 left_curve_enabled, right_curve_enabled;
    u8 left_curve_points[STICK_CURVE_POINTS];
    u8 right_curve_points[STICK_CURVE_POINTS];
    struct cal_deadzone deadzone[2];
    /* Each physical stick: its curve, inversion, then where it goes. */
    u8 invert[2][2];         /* [stick][x, y] 1: inverted */
    u8 maps_to[2];           /* STICK_*: the host stick it drives */
    u8 rumble;               /* step: 0 off, 1-RUMBLE_STEPS */
    /* Per step: [left, right] percent duty for the lightest and the full
     * rumble, and full power this long when a motor starts. */
    u8 rumble_min[2][RUMBLE_STEPS], rumble_max[2][RUMBLE_STEPS];
    u8 rumble_kick_ms[RUMBLE_STEPS];
    u8 mouse_layout;         /* mouse: 0 games, 1 desktop */
    u16 stick_speed;         /* mouse: pointer counts per second, stick all the way */
    u16 gyro_speed;          /* mouse, touchpad: counts per degree turned; 0 off */
    u16 gyro_accel;          /* percent of gyro_speed for fast turns */
};

/* The remap slot's sticks: curves, inversion, which host stick each drives;
 * NULL: as calibrated. */
void in_response(const struct set_remap *m, struct pad *pad);

/* What a change can be made to: a slot, the palette, calibration or a remap slot. */
#define TARGET_PALETTE SLOTS
#define TARGET_CAL     (SLOTS + 1)
#define TARGET_REMAP   (SLOTS + 2)
#define TARGET_BASE    (TARGET_REMAP + SLOTS)   /* the base remap: what remap slots inherit */
#define TARGETS        (TARGET_BASE + 1)

enum { SET_OK, SET_UNKNOWN, SET_BAD_LENGTH, SET_RANGE, SET_FULL };
enum { SET_UNPAIR = 0xfe };       /* WRITE action: slot only, zero-length; applied on COMMIT */

void set_init(void);
void set_poll(u32 now, int radio_quiet);
void set_flush(void);                    /* write what is pending now: before sleep or reboot */
const struct set_slot *set_slot(u8 slot);
struct rgb set_colour(u8 entry);         /* a palette entry, at full strength */
const struct set_brightness *set_brightness(void);
const struct bt_bond *set_bond(u8 slot);
void set_bond_store(u8 slot, const struct bt_bond *bond);
void set_slot_address(u8 slot, u8 out[6]);
/* Use the current identity for first pairing; renew only to replace a bond.
 * A candidate is saved with the bond after successful pairing. */
void set_pairing_address(u8 slot, int renew, u8 out[6]);
void set_pairing_store(u8 slot, const u8 address[6], const struct bt_bond *bond);
const struct set_cal *set_cal(void);
/* Remap slot 0-7, or REMAP_BASE: the base remap, which the slots take every
 * field they have not set from (but their names; A keeps its own swap). */
#define REMAP_BASE SLOTS
const struct set_remap *set_remap(u8 remap);
u32 set_edits(void);                     /* counts commits from the tools */
void set_cal_store(const struct set_cal *cal);
/* Over USB: */
u32 set_describe(u32 n, u8 *out, u32 max);
/* Current values include the effective slot address, even when still derived;
 * its set flag stays clear until saved. Defaults retain zero for derived. */
u32 set_read(u8 target, u8 *out, u32 max);
u32 set_read_defaults(u8 target, u8 *out, u32 max); /* same format, no state changes */
/* WRITE items: {target, id, length, bytes}; SET_UNPAIR clears the bond and
 * stores a new address at COMMIT. DROP cancels it like any other setting edit. */
u32 set_write(const u8 *items, u32 len, u8 *results, u32 max);
void set_apply(void);
void set_drop(void);
u32 set_staged(void);
void set_ephemeral(void);
int set_is_ephemeral(void);

/* ---- leds.c ------------------------------------------------------------- */

enum { LIGHT_HOME, LIGHT_P0, LIGHT_P1, LIGHT_P2, LIGHT_P3, LIGHT_GRIP_L,
       LIGHT_GRIP_R, LIGHT_UNUSED, LIGHT_COUNT };

struct led_frame { struct rgb light[LIGHT_COUNT]; };
/* A frame finer than the LEDs: 1/256 of their steps, 0-65280. */
struct led_fine { u16 light[LIGHT_COUNT][3]; };

/* Blinks have soft edges, breathing eases in and out, a sweep is a light
 * gliding to and fro (a step: the time from one light to the next). */
enum { WAVE_OFF, WAVE_SOLID, WAVE_BLINK, WAVE_BREATHE, WAVE_SWEEP };
struct led_wave { u8 kind; u16 a, b; };    /* blink: on, off; breathe: period; sweep: step */
#define LED_OFF            { WAVE_OFF, 0, 0 }
#define LED_SOLID          { WAVE_SOLID, 0, 0 }
#define LED_BLINK(on, off) { WAVE_BLINK, (on), (off) }
#define LED_BREATHE(ms)    { WAVE_BREATHE, (ms), 0 }
#define LED_SWEEP(ms)      { WAVE_SWEEP, (ms), 0 }

enum { BAR_ALL, BAR_SLOT, BAR_PLAYER };       /* BAR_PLAYER: the slot's players setting */
enum { GRIPS_WAVE, GRIPS_SLOT_SETTING };
enum { COLOUR_IDLE, COLOUR_CONNECT, COLOUR_PRIMARY };
/* The whole pad as one, instead of Home, the bar and the grips: remap config
 * mode (colours flowing down the pad); restarting into the ROM loader (a
 * green light runs down and fills the pad, a flash, then dim green, which
 * the LEDs hold while the loader runs; LED_LOADER_MS in all). */
enum { EFFECT_NONE, EFFECT_CONFIG, EFFECT_LOADER };
#define LED_LOADER_MS 900

struct led_scene {
    u8 colour;
    struct led_wave home;
    u8 bar;
    struct led_wave bar_wave;
    u8 grips;
    struct led_wave grips_wave;
    u8 effect;               /* EFFECT_*: then the rest is unused */
};

/* A new scene fades in over the frame shown when it began (from). */
struct led_state {
    u8 scene;              /* enum ctl_kind */
    u8 slot;               /* SLOT_NONE: none */
    u32 since_ms;
    struct led_fine from;
};

struct led_inputs {
    struct shade connect_colour, primary_colour, secondary_colour, idle_colour, battery_colour;
    u16 animation;         /* animated grips and player lights: their colours at this brightness */
    u8 grips, player_lights;   /* the slot's settings */
    u8 ephemeral;
    u8 rumble[2];
    u8 reach[2];
    u8 usb_powered, usb_plug_lit, low_battery;
    u8 has_players, players, players_flash;
    u8 has_lightbar;
    struct rgb lightbar;
    u8 cal_lit;
    struct rgb cal;
};

void led_frame(const struct led_scene *scene, const struct led_state *s, u32 now,
               const struct led_inputs *in, struct led_fine *out);
/* The fine frame in the LEDs' steps.  dither: a dim light between two steps
 * shows each in turn, carrying what is left (acc, the caller's) to the next
 * refresh.  Returns whether refreshing again soon shows more than this. */
int led_dither(const struct led_fine *f, u16 acc[LIGHT_COUNT][3], int dither, struct led_frame *out);
void led_reach(u8 reach[2], const struct pad *pad);

/* ---- remap.c ------------------------------------------------------------ */

/* The remap slot's buttons from the ones ux_step leaves.  every_report: keys
 * at turbo rate 0, for the report builder to flip.  Reset when the remap
 * slot or its settings change, and outside a connection. */
u32 remap_step(const struct set_remap *m, u32 in, u32 now, u32 *every_report);
void remap_reset(void);
u32 remap_deadline(void);                   /* the next turbo edge, pulse end or hold time; 0: none */

/* ---- settings_ux.c ------------------------------------------------------ */

struct ux_timeouts {
    u16 home_grace_ms;
    u16 tap_ms;
    u32 connect_ms;
    u32 pair_ms;
    u32 idle_sleep_ms;
    u32 offline_sleep_ms;
    u16 unplug_sleep_ms;
    u16 stop_max_ms;
    u16 boot_scene_ms;
    u16 uboot_window_ms;
    u16 led_frame_ms;
    u16 usb_plug_ms;
    u32 config_ms;           /* remap config mode with no key pressed */
    u16 rumble_feedback_ms;  /* config mode L1/R1: the new step at full strength */
};

struct ux_colours { u8 idle, battery; };

/* The board's LED defaults, in tenths of the LEDs' smallest step.
 * Palette settings override steady, animation and connect; idle and battery
 * use the board levels directly. */
struct board_led {
    u16 steady, animation, connect;
    u16 idle, battery;
};
extern const struct board_led board_led;

/* The board's defaults for the remap settings rumble_*, steps 1-RUMBLE_STEPS:
 * [left, right] percent duty for the lightest and the full rumble, and full
 * power this long when a motor starts.  Its motors set them. */
struct board_rumble {
    u8 min[2][RUMBLE_STEPS], max[2][RUMBLE_STEPS];
    u8 kick_ms[RUMBLE_STEPS];
};
extern const struct board_rumble board_rumble;

extern const struct ux_timeouts ux_timeouts;
extern const u8 ux_usb_autoconnect;
extern const struct ux_colours ux_colours;

/* LED demos (padtool led-demo): a scene in slot A's colours, with these instead
 * of the slot's settings; 0 in a brightness: the palette's.  A demo lasts
 * UX_LED_DEMO_MS after the last request for it. */
#define UX_LED_DEMO_MS 30000
enum { LED_DEMO_DITHER_PALETTE, LED_DEMO_DITHER_OFF, LED_DEMO_DITHER_ON };
enum { LED_DEMO_SCENE, LED_DEMO_CONFIG, LED_DEMO_LOADER };   /* the scene, or one of the others */
struct ux_led_demo {
    const char *name;
    u8 scene;                /* enum ctl_kind: its LED scene */
    u8 kind;                 /* LED_DEMO_*; the loader's runs again every couple of seconds */
    u8 grips, players;       /* connected: GRIPS_*, PLAYERS_* */
    u16 steady, animation;
    u8 dither;               /* LED_DEMO_DITHER_* */
};
extern const struct ux_led_demo ux_led_demos[];
extern const u8 ux_led_demo_count;
extern const struct led_scene ux_scenes[CTL_KIND_COUNT];
extern const struct led_scene ux_config_scene, ux_loader_scene;   /* remap config mode, to the loader */

/* Home gestures, table in settings_ux.c.  ux_reported: the controller saw a
 * held-back Home tap's down report leave. */
struct ctl_request ux_step(struct pad *pad, u32 now, u8 state);
void ux_reported(u32 now);
/* Earliest permitted idle power-off: 0 while USB powered, otherwise the
 * later of offline/unplug waits (or the longer ephemeral inactivity wait).
 * The controller uses this for both its sleep decision and wake deadline. */
u32 ux_idle_deadline(u32 idle_since, u32 activity_ms, u32 usb_seen_ms,
                     int usb_powered, int ephemeral);
u32 ux_deadline(void);                      /* when ux_step may next fire a hold, 0: none */
u8 ux_armed(void);                          /* slot a release would connect, or SLOT_NONE */
void ux_ignore_held(void);
int ux_config(u32 *since);                  /* in remap config mode; since: when it began */
struct ctl_request ux_boot(u32 buttons, int home_wake, u32 now);

/* ---- usb.c -------------------------------------------------------------- */

/* Timing and counters, kept by the loop, read over USB (padtool stats); times
 * in µs.  Laid out without padding (checked below), so the bytes padtool reads
 * are the struct as it is in memory. */
#define STAT_BUCKETS 16          /* bucket i: below 32 << i µs; the last: the rest */
struct stat_hist {
    u64 sum;
    u32 count, max;
    u16 bucket[STAT_BUCKETS];
};

enum { STEP_INPUT, STEP_BT, STEP_USB, STEP_POWER, STEP_TIMEOUTS, STEP_REPORT, STEP_OUTPUTS,
       STEP_SETTINGS, STEP_COUNT };

struct bt_stats {
    u32 sent;                    /* input reports handed to the controller */
    u32 replaced;                /* overwritten in the newest-wins slot before that */
    u32 acked;                   /* packets the controller reports as sent */
    u32 loop_runs;               /* passes of the bluetooth thread's run loop */
    u32 loop_us;                 /* time in them, wake to sleep (higher priorities included) */
    u32 anchors;                 /* connection events / sniff anchors the central's packets marked */
    u32 late;                    /* reports that went out an anchor or more after the one after hand-over */
};

/* The link as the controller reports it, and when it said packets went. */
struct bt_link_info {
    u8 transport;                /* 0 none, 1 Classic, 2 LE */
    u8 phy_tx, phy_rx;           /* LE: 1 1M, 2 2M, 3 coded */
    u8 mode;                     /* Classic: 0 active, 2 sniff */
    u16 interval;                /* LE: 1.25 ms units; Classic sniff: slots */
    u16 latency, timeout;        /* LE: events; 10 ms units */
    u16 tx_octets, tx_time;      /* LE: data length the controller sends with */
    u16 rx_octets, rx_time;      /* LE: and takes */
    u16 acl_buffers;             /* the controller's ACL packets (LE ones on LE) */
    u32 poll_us;                 /* Classic: accepted poll interval (us), 0 unknown */
    u32 lead_us;                 /* reports are built this long before their anchor; 0: when asked */
    u32 period_us;               /* between the central's anchors, as bt_pad.c reckons it; 0 unknown */
};

struct bt_timing {
    struct stat_hist ack_wait;   /* report to the controller until it says sent */
    struct stat_hist slot_air;   /* report into the input slot until the anchor it went out at */
    struct stat_hist ack_gap;    /* between Number Of Completed Packets for the link */
    struct stat_hist want_send;  /* bt_pad.c asked for a report until it was handed over */
    struct bt_link_info link;
};

struct plat_stats {
    u32 button_changes;          /* debounced changes seen by the tick */
    u32 stick_frames;            /* good frames from the stick MCU */
    u32 stick_resyncs;           /* bytes skipped to find a frame */
};

struct ctl_stats {
    u32 since_ms, now_ms;
    u32 passes, reserved;
    struct stat_hist pass;       /* one pass of the loop, wake to sleep */
    struct stat_hist interval;   /* between reports sent */
    struct stat_hist age;        /* button change to the report carrying it */
    u32 step_max[STEP_COUNT];
    u32 step_sum[STEP_COUNT];
    u32 reports, imu_samples;
    struct bt_stats bt;
    struct plat_stats plat;
    struct bt_timing bt_timing;
};
_Static_assert(sizeof(struct stat_hist) == 48 && sizeof(struct ctl_stats) == 272 + 4 * 48 + 32,
               "padtool reads struct ctl_stats byte for byte");

/* Tool protocol 3 adds shared sensor calibration, queued by usb_poll().
 * Protocol 4 adds read-only setting defaults; 5 uses 15-point stick curves
 * and independent enable flags. These settings are pad-only. */
struct usb_status {
    u8 state;
    u8 slot;
    u8 usb_powered;
    u8 low_battery;
    u16 battery_mv;
    u32 uptime_ms;
    u8 ephemeral;
    u8 staged;               /* settings items waiting for commit */
    const struct ctl_stats *stats;
    const struct pad *input;     /* the controls, calibrated, before Home gestures and the remap; NULL: none */
    const struct in_raw *raw;    /* as read */
};

struct ctl_request usb_poll(const struct usb_status *status);
u32 usb_command(const u8 cdb[16], u8 *data, u32 len, int from_host);   /* the platform's SCSI hook */

/* ---- utils.c ------------------------------------------------------------ */

void util_log_init(void);
void util_log(const char *fmt, ...);
u32 util_log_read(u32 *seq, char *out, u32 max);
void util_put16(u8 *p, u16 v);
void util_put32(u8 *p, u32 v);
u32 util_get32(const u8 *p);
void util_aes128(const u8 key[16], const u8 in[16], u8 out[16]);
void util_stat_add(struct stat_hist *h, u32 us);
/* Timing trace: a ring of {µs, marker, argument}, any thread or interrupt;
 * padtool trace reads it.  Markers: */
enum { TR_WANT = 1, TR_BT_WAKE, TR_BT_SLEEP, TR_CTL_WAKE, TR_BUILT, TR_SENT, TR_ACK,
       TR_CTL_SLEEP, TR_STICK, TR_HANDED, TR_ANCHOR };
void util_trace(u8 marker, u16 arg);
u32 util_trace_read(u32 offset, u8 *out, u32 max);
void util_trace_enable(int on);

/* ---- platform_jieli.c --------------------------------------------------- */

enum { EV_BUTTONS = 1, EV_USB = 2, EV_BT = 4, EV_IMU = 8, EV_DEADLINE = 0x80 };

enum { BOOT_POWER_ON, BOOT_HOME, BOOT_USB, BOOT_RESET };
struct plat_boot {
    u8 cause;
    u32 buttons_held;
};

u32 plat_ms(void);
u32 plat_us(void);                          /* wraps */
/* Sampling profiler: a timer interrupt above everything else counts the
 * interrupted code address, in buckets over flash code, RAM code and the mask
 * ROM.  plat_prof_start(0) stops it; read returns the header and the counts
 * from a byte offset (padtool profile). */
void plat_prof_start(u16 hz, u32 zoom, u8 zoom_shift);   /* zoom: flash window start, 0 none */
u32 plat_prof_read(u32 offset, u8 *out, u32 max);
u32 plat_peek(u32 addr, u8 *out, u32 max);  /* 32-bit words from addr, for registers */
u32 plat_wait(u32 deadline_ms, u32 at_us);   /* either 0: none; at_us to 250 µs */
void plat_wake(u32 events);                 /* from another thread or an interrupt */
void plat_delay_ms(u32 ms);                 /* blocking; set-up only */
void plat_bt_thread(void (*body)(void));
void plat_bt_wait(u32 ms);                  /* the bluetooth thread sleeps until woken */
void plat_bt_wake(void);
void plat_p256_thread(void (*body)(void));  /* the P-256 thread, below the main loop */
void plat_p256_wait(void);
void plat_p256_wake(void);
void plat_lock(void);
void plat_unlock(void);
void plat_boot_info(struct plat_boot *out);
void plat_input(struct in_raw *raw);
void plat_leds(const struct led_frame *f);
void plat_motors(u8 left, u8 right);       /* duty of 255 */
int plat_usb_present(void);
int plat_usb_suspended(void);               /* the host suspended the bus (3 ms quiet) */
void plat_usb_service(void);
void plat_usb_probe_request(u32 extra);   /* USB_PROBE test builds: tools/usbprobe.py */
/* USB as a game controller.  plat_usb_mode(NULL) is the tool device (mass
 * storage, the vendor commands); a personality re-enumerates as that device.
 * read answers the host's GET_REPORT in the USB interrupt (constant data, as
 * on Bluetooth).  Input is newest-wins: one waits, a newer one replaces it;
 * messages queue behind each other and go first.  EV_USB when the host took
 * a report or wrote one. */
void plat_usb_mode(const struct usb_identity *id,
                   int (*read)(u8 kind, u16 id, u16 offset, u8 *out, u16 max));
int plat_usb_ready(void);                   /* the host configured the personality */
int plat_usb_jack_in(void);                 /* a plug in the headphone jack, debounced */
int plat_usb_input_wanted(void);            /* nothing waits to go to the host */
int plat_usb_send_input(u8 id, const u8 *data, u16 len);
int plat_usb_send_message(u16 id, const u8 *data, u16 len);   /* HOST_BULK: bulk IN */
int plat_usb_take(struct bt_write *w);      /* the host's next write, 0: none */
/* Feature report 0xf5 "tool", whatever the personality (hidraw can send it):
 * the session ends and USB is the tool device again. */
#define USB_TOOL_REPORT 0xf5
u16 plat_battery_mv(void);
void plat_stats(struct plat_stats *out);
const u8 *plat_unique_id(void);
extern const char board_name[];   /* model, e.g. "Cyclone 2" or "Cyclone 2 Dongle":
                                    * tool USB/SCSI product, IDENTIFY, plain HID names */
int plat_store_read(u16 key, void *buf, u16 len);
int plat_store_write(u16 key, const void *buf, u16 len);
u8 plat_retained_slot(void);
void plat_retain_slot(u8 slot);
void plat_power_off(void);
void plat_reboot(void);
void plat_reboot_loader(void);
void plat_watch_stop(void);                 /* the hang watch, before a long wait on purpose */

#endif
