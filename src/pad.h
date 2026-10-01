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

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifdef UNITTEST
#define RETAINED
#else
#define RETAINED __attribute__((section(".retained"), used, aligned(4)))
#endif

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
};

struct pad {
    u16 buttons;
    u8 hat;
    s16 lx, ly, rx, ry;
    u16 l2, r2;
};

void in_map(const struct in_raw *raw, struct pad *pad);
int in_moved(const struct pad *now, const struct pad *ref);

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
    REQ_REBOOT, REQ_REBOOT_LOADER, REQ_HOME_TAP,
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
    u32 class_of_device;
    u16 vendor_id, product_id, version;
    const u8 *hid_descriptor;
    u16 hid_descriptor_len;
    u16 sniff_slots, sniff_attempt;
};

#define BT_WRITE_MAX 64
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

struct profile;
void bt_start(const struct profile *p, const u8 address[6], const struct bt_bond *bond,
              int pairing);
void bt_stop(void);
void bt_abort(void);
void bt_connect(void);
void bt_disconnect(void);
int bt_send_input(u8 id, const u8 *data, u16 len);
int bt_send_message(u8 id, const u8 *data, u16 len);
int bt_poll(struct bt_ev *ev);
int bt_radio_quiet(void);

/* ---- profile_*.c -------------------------------------------------------- */

enum { IMU_OFF, IMU_MOTION, IMU_GYRO };

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
};

struct profile {
    const char *name;
    u16 report_hz;
    const struct bt_device *device;
    u8 (*begin)(const struct profile_session *s);
    void (*end)(void);
    int (*report)(const struct pad *pad, u32 now, u8 *out, u8 *id);
    int (*write)(const struct bt_write *w, u32 now, struct bt_bond *bond);
    void (*host)(u32 now, struct profile_host *out);
    int (*read)(u8 kind, u16 id, u8 *out, u16 max);
};

extern const struct profile profile_switch;

/* ---- settings.c --------------------------------------------------------- */

enum { PROFILE_SWITCH, PROFILE_SWITCH2, PROFILE_XBOX, PROFILE_XBOX_BLE, PROFILE_DUALSENSE,
       PROFILE_DUALSHOCK4, PROFILE_GENERIC, PROFILE_MOUSE, PROFILE_COUNT };
enum { GRIPS_NONE, GRIPS_RUMBLE, GRIPS_INPUT, GRIPS_BOTH, GRIPS_LIGHTBAR };

struct set_slot {
    u8 type;
    u8 rumble;
    u8 grips;
    u8 colour[3];
};

struct set_prefs {
    u8 idle_colour[3];
    u8 battery_colour[3];
};

const struct set_slot *set_slot(u8 slot);
const struct set_prefs *set_prefs(void);
void set_init(void);
void set_poll(u32 now, int radio_quiet);
void set_commit(void);
const struct bt_bond *set_bond(u8 slot);
void set_bond_store(u8 slot, const struct bt_bond *bond);
void set_bond_forget(u8 slot);
void set_slot_address(u8 slot, u8 out[6]);
void set_slot_new_address(u8 slot);
const struct set_cal *set_cal(void);
void set_cal_store(const struct set_cal *cal);

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

enum { BAR_ALL, BAR_SLOT, BAR_PLAYER };
enum { GRIPS_WAVE, GRIPS_SLOT_SETTING };
enum { COLOUR_IDLE, COLOUR_SLOT };

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
    struct rgb slot_colour, idle_colour, battery_colour;
    u8 grips;              /* the slot's grips setting */
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

extern const struct ux_timeouts ux_timeouts;
extern const struct led_scene ux_scenes[CTL_KIND_COUNT];

struct ctl_request ux_step(struct pad *pad, u32 now, u8 state);
u32 ux_deadline(void);                      /* when ux_step may next fire a hold, 0: none */
void ux_ignore_held(void);
struct ctl_request ux_boot(u16 buttons, u8 hat, u8 state);

/* ---- usb.c -------------------------------------------------------------- */

struct usb_status {
    u8 state;
    u8 slot;
    u8 usb_powered;
    u8 low_battery;
    u16 battery_mv;
    u32 uptime_ms;
};

struct ctl_request usb_poll(const struct usb_status *status);
u32 usb_command(const u8 cdb[16], u8 *data, u32 max);   /* from the platform's SCSI hook */

/* ---- utils.c ------------------------------------------------------------ */

void util_log_init(void);
void util_log(const char *fmt, ...);
u32 util_log_read(u32 *seq, char *out, u32 max);
void util_put16(u8 *p, u16 v);
void util_put32(u8 *p, u32 v);
u32 util_get32(const u8 *p);

/* ---- platform_jieli.c --------------------------------------------------- */

enum { EV_BUTTONS = 1, EV_USB = 2, EV_BT = 4, EV_IMU = 8, EV_DEADLINE = 0x80 };

enum { BOOT_POWER_ON, BOOT_HOME, BOOT_USB, BOOT_RESET };
struct plat_boot {
    u8 cause;
    u8 hat_held;
    u16 buttons_held;
};

u32 plat_ms(void);
u32 plat_wait(u32 deadline_ms);
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
const u8 *plat_unique_id(void);
int plat_store_read(u16 key, void *buf, u16 len);
int plat_store_write(u16 key, const void *buf, u16 len);
u8 plat_retained_slot(void);
void plat_retain_slot(u8 slot);
void plat_power_off(void);
void plat_reboot(void);
void plat_reboot_loader(void);

#endif
