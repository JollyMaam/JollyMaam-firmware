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

#define SLOTS     8
#define SLOT_NONE 0xff
#define SLOT_LAST 0xfe

/* ---- input.c ------------------------------------------------------------ */

enum {
    PAD_A = 1 << 0, PAD_B = 1 << 1, PAD_SCREENSHOT = 1 << 2, PAD_X = 1 << 3,
    PAD_Y = 1 << 4, PAD_M = 1 << 5, PAD_L1 = 1 << 6, PAD_R1 = 1 << 7,
    PAD_L4 = 1 << 8, PAD_R4 = 1 << 9, PAD_SELECT = 1 << 10, PAD_START = 1 << 11,
    PAD_HOME = 1 << 12, PAD_L3 = 1 << 13, PAD_R3 = 1 << 14,
};
#define PAD_FACE (PAD_A | PAD_B | PAD_X | PAD_Y)

enum { HAT_UP, HAT_UP_RIGHT, HAT_RIGHT, HAT_DOWN_RIGHT, HAT_DOWN, HAT_DOWN_LEFT,
       HAT_LEFT, HAT_UP_LEFT, HAT_NEUTRAL };

#define STICK_MAX   32767
#define TRIGGER_MAX 65535u

struct in_raw {
    u16 buttons;
    u8 hat;
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
    u16 buttons;
    u8 hat;
    s16 lx, ly, rx, ry;
    u16 l2, r2;
    u8 imu_count;                /* samples new since the last report, oldest first */
    struct imu_sample imu[IMU_PER_REPORT];
};
#define PAD_CONTROLS_SIZE ((u32)(size_t)&((struct pad *)0)->imu_count)

void in_map(const struct in_raw *raw, struct pad *pad);
int in_moved(const struct pad *now, const struct pad *ref);

/* ---- input_imu.c -------------------------------------------------------- */

enum { IMU_OFF, IMU_MOTION, IMU_GYRO };

struct imu_pointer {
    s64 motion[2];               /* what is left under a count */
    u32 last_us;
    u8 have;
};

u32 imu_poll(u32 now, u8 mode);   /* advances the sensor; returns its next deadline */
int imu_take(u32 *seq, struct imu_sample *out, int max);
void imu_gyro_bias(s32 out[3]);  /* the learned zero-rate offset, 1/16 LSB */
void imu_pointer(struct imu_pointer *p, const struct imu_sample *s, int n, u16 per_degree,
                 u16 accel, s32 *x, s32 *y);

/* ---- input_calibration.c ------------------------------------------------ */

#define CAL_GATE_KNOTS 16

struct cal_stick {
    u16 minimum[2], center[2], maximum[2];
    u16 gate[CAL_GATE_KNOTS];
    u16 inner_deadzone, outer_deadzone;
};

struct cal_trigger { u16 rest, full, inner_deadzone, outer_deadzone; };

struct set_cal {
    struct cal_stick stick[2];
    struct cal_trigger trigger[2];
};

enum { CAL_IDLE, CAL_RUNNING, CAL_SAVE };

void cal_apply(const struct set_cal *cal, const struct in_raw *raw, struct pad *pad);
void cal_start(u32 now);
void cal_cancel(u32 now);
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
};

struct ctl_request {
    u8 kind;
    u8 slot;
    u8 generate_new_address;
};

void ctl_main(void);

/* ---- bt.c --------------------------------------------------------------- */

enum { BT_CLASSIC, BT_LE };

struct bt_bond {
    u8 peer[6];                  /* least significant byte first; all zero: none */
    u8 link_key[16];
    u8 key_type;
    u8 le_peer[6];
    u8 ltk[16];
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

#define BT_WRITE_MAX 96
enum { BT_WRITE_OUTPUT, BT_WRITE_FEATURE, BT_WRITE_ATT };
struct bt_write {
    u8 kind;
    u16 id;
    u16 len;
    u8 data[BT_WRITE_MAX];
};

enum { BT_EV_NONE, BT_EV_LINKED, BT_EV_UNLINKED, BT_EV_FAILED, BT_EV_STOPPED,
       BT_EV_PEER, BT_EV_BONDED, BT_EV_WRITE };

struct bt_ev {
    u8 kind;
    u8 lost;                     /* BT_EV_UNLINKED: the signal went, not the host */
    u8 reason;                   /* HCI disconnect reason */
    struct bt_bond bond;         /* BT_EV_PEER, BT_EV_BONDED */
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
void bt_disconnect(void);
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
};

struct profile_host {
    u8 rumble[2];
    u8 has_players, players, players_flash;
    u8 has_lightbar;
    u8 lightbar[3];
    u8 imu;
    u8 ended;                    /* host ended this session: stop locally */
};

struct profile {
    const char *name;
    const struct bt_device *device;
    u8 (*begin)(const struct profile_session *s);
    void (*end)(void);
    int (*report)(const struct pad *pad, u32 now, u8 *out, u8 *id);
    int (*write)(const struct bt_write *w, u32 now, struct bt_bond *bond);
    void (*host)(u32 now, struct profile_host *out);
    int (*read)(u8 kind, u16 id, u16 offset, u8 *out, u16 max);
    const struct profile_wake *wake;   /* before connecting a bonded slot */
};

/* Runs before bt_start() with the radio otherwise quiet. */
struct profile_wake {
    int (*start)(const struct profile_session *s);   /* 0 started, <0 nothing to wake */
    int (*step)(u32 now, u32 *next);  /* 1 finished; else call again at *next */
    void (*stop)(void);
};

extern const struct profile profile_switch, profile_switch2, profile_xbox, profile_xbox_ble;
extern const struct profile profile_dualsense, profile_dualshock4, profile_generic, profile_mouse,
    profile_generic_ble;
extern const struct profile_wake profile_switch_wake;

/* ---- settings.c --------------------------------------------------------- */

enum { PROFILE_SWITCH, PROFILE_SWITCH2, PROFILE_XBOX, PROFILE_XBOX_BLE, PROFILE_DUALSENSE,
       PROFILE_DUALSHOCK4, PROFILE_GENERIC, PROFILE_MOUSE, PROFILE_GENERIC_BLE, PROFILE_COUNT };
enum { GRIPS_OFF, GRIPS_HOST, GRIPS_FADE, GRIPS_RUMBLE, GRIPS_INPUT, GRIPS_BOTH,
       GRIPS_PRIMARY, GRIPS_SECONDARY };
enum { PLAYERS_OFF, PLAYERS_HOST, PLAYERS_FADE, PLAYERS_PRIMARY, PLAYERS_SECONDARY };

#define PALETTE 16
#define LEVELS  16
struct colour { u8 entry, level; };      /* a palette entry at a brightness level */

struct set_slot {
    u8 type;
    struct colour connect_colour;        /* connecting, pairing: which slot */
    struct colour primary_colour;        /* connected */
    struct colour secondary_colour;      /* the background */
    u8 grips;
    u8 players;
    u8 rumble;               /* percent */
    u8 variant;              /* mouse: 0 games, 1 desktop */
    u16 stick_speed;         /* mouse: pointer counts per second, stick all the way */
    u16 gyro_speed;          /* mouse, touchpad: counts per degree turned; 0 off */
    u16 gyro_accel;          /* percent of gyro_speed for fast turns */
    u8 address[6];           /* zero: derived */
    struct bt_bond bond;
    u16 report_hz;           /* the most reports a second; 0: REPORT_HZ */
    u16 pull_delay_us;       /* build a pulled report this long after bt.c asks */
    struct bt_link link;
};

/* What a change can be made to: a slot, the palette or calibration. */
#define TARGET_PALETTE SLOTS
#define TARGET_CAL     (SLOTS + 1)
#define TARGETS        (SLOTS + 2)

enum { SET_OK, SET_UNKNOWN, SET_BAD_LENGTH, SET_RANGE, SET_FULL };

void set_init(void);
void set_poll(u32 now, int radio_quiet);
void set_flush(void);                    /* write what is pending now: before sleep or reboot */
const struct set_slot *set_slot(u8 slot);
struct rgb set_colour(struct colour c);
const struct bt_bond *set_bond(u8 slot);
void set_bond_store(u8 slot, const struct bt_bond *bond);
void set_bond_forget(u8 slot);
void set_slot_address(u8 slot, u8 out[6]);
void set_slot_new_address(u8 slot);
const struct set_cal *set_cal(void);
void set_cal_store(const struct set_cal *cal);
/* Over USB: */
u32 set_describe(u32 n, u8 *out, u32 max);
u32 set_read(u8 target, u8 *out, u32 max);
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

struct led_scene {
    u8 colour;
    struct led_wave home;
    u8 bar;
    struct led_wave bar_wave;
    u8 grips;
    struct led_wave grips_wave;
};

struct led_state {
    u8 scene;              /* enum ctl_kind */
    u8 slot;               /* SLOT_NONE: none */
    u32 since_ms;
};

struct led_inputs {
    struct rgb connect_colour, primary_colour, secondary_colour, idle_colour, battery_colour;
    u8 grips, player_lights;   /* the slot's settings */
    u8 ephemeral;
    u8 rumble[2];
    u8 reach[2];
    u8 usb_powered, usb_plug_flash, low_battery;
    u8 has_players, players, players_flash;
    u8 has_lightbar;
    struct rgb lightbar;
    u8 cal_lit;
    struct rgb cal;
};

void led_frame(const struct led_scene *scene, const struct led_state *s, u32 now,
               const struct led_inputs *in, struct led_frame *out);
void led_reach(u8 reach[2], const struct pad *pad);

/* ---- settings_ux.c ------------------------------------------------------ */

struct ux_timeouts {
    u16 home_grace_ms;
    u16 home_tap_ms;
    u32 connect_ms;
    u32 pair_ms;
    u32 idle_sleep_ms;
    u32 offline_sleep_ms;
    u16 unplug_sleep_ms;
    u16 stop_max_ms;
    u16 boot_scene_ms;
    u16 uboot_window_ms;
    u16 led_frame_ms;
    u16 usb_plug_flash_ms;
};

struct ux_colours { struct colour idle, battery; };

extern const struct ux_timeouts ux_timeouts;
extern const struct ux_colours ux_colours;
extern const struct led_scene ux_scenes[CTL_KIND_COUNT];

struct ctl_request ux_step(struct pad *pad, u32 now, u8 state);
u32 ux_deadline(void);                      /* when ux_step may next fire a hold, 0: none */
void ux_ignore_held(void);
struct ctl_request ux_boot(u16 buttons, u8 hat, u8 state);

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
    u32 period_us;               /* between the central's anchors, as bt.c reckons it; 0 unknown */
};

struct bt_timing {
    struct stat_hist ack_wait;   /* report to the controller until it says sent */
    struct stat_hist slot_air;   /* report into the input slot until the anchor it went out at */
    struct stat_hist ack_gap;    /* between Number Of Completed Packets for the link */
    struct stat_hist want_send;  /* bt.c asked for a report until it was handed over */
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
 * padtool trace reads it (usb.md, TRACE).  Markers: */
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
    u8 hat_held;
    u16 buttons_held;
};

u32 plat_ms(void);
u32 plat_us(void);                          /* wraps */
/* Sampling profiler: a timer interrupt above everything else counts the
 * interrupted code address, in buckets over flash code, RAM code and the mask
 * ROM.  plat_prof_start(0) stops it; read returns the header and the counts
 * from a byte offset (architecture/usb.md, PROFILE). */
void plat_prof_start(u16 hz, u32 zoom, u8 zoom_shift);   /* zoom: flash window start, 0 none */
u32 plat_prof_read(u32 offset, u8 *out, u32 max);
u32 plat_peek(u32 addr, u8 *out, u32 max);  /* 32-bit words from addr, for registers */
void plat_i2c_read(u8 dev, u8 reg, u8 *buf, u8 len);   /* starts; EV_IMU when done */
void plat_i2c_write(u8 dev, u8 reg, u8 value);
int plat_i2c_done(void);                    /* 1 done, 0 busy, -1 failed */
u32 plat_wait(u32 deadline_ms, u32 at_us);   /* either 0: none; at_us to 250 µs */
void plat_wake(u32 events);                 /* from another thread or an interrupt */
void plat_delay_ms(u32 ms);                 /* blocking; set-up only */
void plat_bt_thread(void (*body)(void));
void plat_bt_wait(u32 ms);                  /* the bluetooth thread sleeps until woken */
void plat_bt_wake(void);
void plat_lock(void);
void plat_unlock(void);
void plat_boot_info(struct plat_boot *out);
void plat_input(struct in_raw *raw);
void plat_leds(const struct led_frame *f);
void plat_motors(u8 left, u8 right);
int plat_usb_present(void);
void plat_usb_service(void);
u16 plat_battery_mv(void);
void plat_stats(struct plat_stats *out);
const u8 *plat_unique_id(void);
int plat_store_read(u16 key, void *buf, u16 len);
int plat_store_write(u16 key, const void *buf, u16 len);
u8 plat_retained_slot(void);
void plat_retain_slot(u8 slot);
void plat_power_off(void);
void plat_reboot(void);
void plat_reboot_loader(void);

#endif
