#include <system/includes.h>
#include <asm/adc_api.h>
#include <asm/charge.h>
#include <asm/power/p33.h>
#include <usb/otg.h>
#include <usb/usb_phy.h>

#include "btstack_jl.h"
#include "calibration.h"
#include "config.h"
#include "controller_state.h"
#include "controller.h"
#include "imu.h"
#include "input.h"
#include "main.h"
#include "profile.h"
#include "rumble.h"
#include "lights.h"
#include "profile_switch_wake.h"

extern u16 config_dropped_records;
extern u16 config_schema_errors;

static const struct cfg_choice radio_states[] = {
    { CYCLONE2_CONTROLLER_IDLE, "idle" },
    { CYCLONE2_CONTROLLER_CONNECTING, "connecting" },
    { CYCLONE2_CONTROLLER_PAIRING, "pairing" },
    { CYCLONE2_CONTROLLER_CONNECTED, "connected" },
    { CYCLONE2_CONTROLLER_DISCONNECTED, "disconnected" },
    { CYCLONE2_CONTROLLER_WAKING, "waking" },
    { CYCLONE2_CONTROLLER_STOPPING, "stopping" },
    { 0, NULL },
};

static const struct cfg_choice profile_types[] = {
    { CYCLONE2_PROFILE_SWITCH, "switch" },
    { CYCLONE2_PROFILE_SWITCH2, "switch2" },
    { CYCLONE2_PROFILE_XBOX, "xbox" },
    { CYCLONE2_PROFILE_XBOX_BLE, "xboxble" },
    { CYCLONE2_PROFILE_DUALSENSE, "dualsense" },
    { CYCLONE2_PROFILE_GENERIC, "generic" },
    { 0, NULL },
};

static const struct cfg_choice slots[] = {
    { 0, "A" }, { 1, "B" }, { 2, "X" }, { 3, "Y" }, { 0, NULL },
};

static const struct cfg_choice cal_phases[] = {
    { CYCLONE2_CAL_IDLE, "idle" }, { CYCLONE2_CAL_SETTLE, "settle" },
    { CYCLONE2_CAL_REST, "rest" }, { CYCLONE2_CAL_SWEEP, "sweep" },
    { CYCLONE2_CAL_RESULT, "result" }, { 0, NULL },
};

static const struct cfg_choice imu_states[] = {
    { 0, "stopped" }, { 1, "starting" }, { 2, "running" }, { 0, NULL },
};

struct status_system {
    u32 uptime_ms;
    u32 boots;
    u8 reset_source;
    u8 reset_p33;
    u8 reset_rtc;
    u8 usb;
    u8 usb_sense;
    u8 usb_otg;
    u8 ldo5v_detect;
    u8 ldo5v_enabled;
    u16 usb_sof_frame;
    u32 vbat_mv;
    u8 charge;
    u8 cal_phase;
    u8 config_pending;
    u8 commit_queued;
    u16 dropped_records;
    u16 schema_errors;
    u8 flash_uid[16];
    u32 sys_hz;
    u32 lsb_hz;
};

static void get_system(void *out)
{
    struct status_system *s = out;
    const u8 *uid = config_flash_uid();

    s->uptime_ms = sys_timer_get_ms();
    s->sys_hz = clk_get("sys");
    s->lsb_hz = clk_get("lsb");
    s->boots = cyclone2_boot_count;
    s->reset_source = cyclone2_reset_source;
    s->reset_p33 = cyclone2_reset_p33;
    s->reset_rtc = cyclone2_reset_rtc;
    s->usb = (u8)cyclone2_usb_present();
    s->usb_sense = (u8)gpio_read(CYCLONE2_USB_SENSE);
    s->usb_otg = (u8)usb_otg_online(USB0);
    s->usb_sof_frame = usb_read_sofframe(USB0);
    {
        u8 ldo5v = p33_rx_1byte(P3_LDO5V_CON);

        s->ldo5v_detect = (ldo5v >> 5) & 1U;
        s->ldo5v_enabled = ldo5v & 1U;
    }

    s->vbat_mv = adc_get_voltage(AD_CH_VBAT) * 4U;
    s->charge = (u8)get_charge_online_flag();
    s->cal_phase = (u8)cyclone2_calibration_phase();
    s->config_pending = (u8)config_pending();
    s->commit_queued = (u8)config_commit_queued();
    s->dropped_records = config_dropped_records;
    s->schema_errors = config_schema_errors;
    if (uid) memcpy(s->flash_uid, uid, sizeof(s->flash_uid));
}

struct status_radio {
    struct cyclone2_connection_status connection;
    u8 idle;
};

static void get_radio(void *out)
{
    struct status_radio *s = out;

    cyclone2_connection_status_get(&s->connection);
    s->idle = btstack_jl_core_idle();
}

static void get_report(void *out)
{
    cyclone2_report_statistics_get(out);
}

static void get_air(void *out)
{
    btstack_jl_core_input_stats(out);
}

static void get_input(void *out)
{
    cyclone2_input_get_stats(out);
}

static void get_inject(void *out)
{
    cyclone2_injection_status_get(out);
}

static void get_rumble(void *out)
{
    cyclone2_rumble_get_status(out);
}

struct status_imu {
    struct cyclone2_imu_status imu;
    u8 state;
};

static void get_imu(void *out)
{
    struct status_imu *s = out;

    cyclone2_imu_get_status(&s->imu);
    s->state = s->imu.initialized ? 2U : s->imu.enabled ? 1U : 0U;
}

static void get_wake(void *out)
{
    wake_status_get(out);
}

#define SYSTEM(member)  CFG_STATUS_FIELD(struct status_system, member, get_system)
#define RADIO(member)   CFG_STATUS_FIELD(struct status_radio, member, get_radio)
#define REPORT(member)  CFG_STATUS_FIELD(struct cyclone2_report_statistics, member, get_report)
#define AIR(member)     CFG_STATUS_FIELD(btstack_jl_input_stats_t, member, get_air)
#define INPUT(member)   CFG_STATUS_FIELD(struct cyclone2_input_stats, member, get_input)
#define INJECT(member)  CFG_STATUS_FIELD(struct cyclone2_injection_status, member, get_inject)
static const struct cfg_choice light_scenes[] = {
    { CYCLONE2_SCENE_BOOT, "boot" }, { CYCLONE2_SCENE_IDLE, "idle" },
    { CYCLONE2_SCENE_CONNECTING, "connecting" },
    { CYCLONE2_SCENE_PAIRING, "pairing" },
    { CYCLONE2_SCENE_CONNECTED, "connected" },
    { CYCLONE2_SCENE_DISCONNECTED, "disconnected" },
    { CYCLONE2_SCENE_WAKING, "waking" },
    { CYCLONE2_SCENE_SWITCHING, "switching" }, { CYCLONE2_SCENE_OFF, "off" },
    { 0, NULL },
};

static void get_lights(void *out)
{
    cyclone2_lights_status_get(out);
}

#define LIGHTS(member)  CFG_STATUS_FIELD(struct cyclone2_lights_status, member, get_lights)
#define RUMBLE(member)  CFG_STATUS_FIELD(struct cyclone2_rumble_status, member, get_rumble)
#define IMU(member)     CFG_STATUS_FIELD(struct status_imu, member, get_imu)
#define WAKE(member)    CFG_STATUS_FIELD(struct wake_status, member, get_wake)

const struct cfg_item status_items[] = {
    { .path = "system.uptime_ms", .type = CFG_UINT, SYSTEM(uptime_ms) },
    { .path = "system.sys_hz", .type = CFG_UINT, SYSTEM(sys_hz),
      .help = "CPU clock" },
    { .path = "system.lsb_hz", .type = CFG_UINT, SYSTEM(lsb_hz),
      .help = "peripheral bus clock: UART and I2C divisors count it" },
    { .path = "system.boots", .type = CFG_UINT, SYSTEM(boots),
      .help = "boot counter (survives resets, not power loss)" },
    { .path = "reset.source", .type = CFG_UINT, SYSTEM(reset_source) },
    { .path = "reset.p33", .type = CFG_UINT, SYSTEM(reset_p33) },
    { .path = "reset.rtc", .type = CFG_UINT, SYSTEM(reset_rtc) },
    { .path = "system.usb", .type = CFG_BOOL, SYSTEM(usb),
      .help = "active USB host frames seen recently" },
    { .path = "system.usb_sense", .type = CFG_BOOL, SYSTEM(usb_sense),
      .help = "PC3 input, shared with LED SPI" },
    { .path = "system.usb_otg", .type = CFG_UINT, SYSTEM(usb_otg),
      .help = "SDK USB OTG mode" },
    { .path = "system.usb_sof_frame", .type = CFG_UINT, SYSTEM(usb_sof_frame),
      .help = "host USB frame counter; freezes on disconnect or suspend" },
    { .path = "system.ldo5v_detect", .type = CFG_BOOL, SYSTEM(ldo5v_detect),
      .help = "PMU 5 V detect bit" },
    { .path = "system.ldo5v_enabled", .type = CFG_BOOL, SYSTEM(ldo5v_enabled),
      .help = "PMU 5 V detector enabled" },
    { .path = "battery.vbat_mv", .type = CFG_UINT, SYSTEM(vbat_mv),
      .help = "not a validated cell measurement; see docs/power-reset.md" },
    { .path = "battery.charging", .type = CFG_BOOL, SYSTEM(charge),
      .help = "SDK charger flag (uninitialized on this board; not reliable)" },
    { .path = "cal.phase", .type = CFG_ENUM, SYSTEM(cal_phase),
      .choices = cal_phases },
    { .path = "config.pending", .type = CFG_BOOL, SYSTEM(config_pending),
      .help = "settings changed and not written to flash yet" },
    { .path = "config.commit_queued", .type = CFG_BOOL, SYSTEM(commit_queued),
      .help = "a commit is waiting for the radio to settle" },
    { .path = "config.dropped_records", .type = CFG_UINT,
      SYSTEM(dropped_records),
      .help = "stored values that could not be read back at boot" },
    { .path = "config.schema_errors", .type = CFG_UINT, SYSTEM(schema_errors),
      .help = "mistakes in the settings table; 0 unless a build is broken" },
    { .path = "config.flash_id", .type = CFG_HEX, SYSTEM(flash_uid),
      .help = "the flash chip's unique ID, the default identity seed" },

    { .path = "radio.state", .type = CFG_ENUM,
      RADIO(connection.connection_state), .choices = radio_states },
    { .path = "radio.slot", .type = CFG_ENUM, RADIO(connection.slot),
      .choices = slots, .help = "named after its Home+button chord" },
    { .path = "radio.type", .type = CFG_ENUM, RADIO(connection.profile_type),
      .choices = profile_types },
    { .path = "radio.address", .type = CFG_MAC, RADIO(connection.local_address) },
    { .path = "radio.attempts", .type = CFG_UINT,
      RADIO(connection.connect_attempts) },
    { .path = "radio.stale", .type = CFG_BOOL, RADIO(connection.stale),
      .help = "the running slot's type or address changed; reconnect to use it" },
    { .path = "radio.idle", .type = CFG_BOOL, RADIO(idle),
      .help = "the Bluetooth task has powered the controller off" },

    { .path = "report.target_hz", .type = CFG_UINT, REPORT(target_hz),
      .help = "reports per second the slot is set to send (profile.#.report_hz)" },
    { .path = "report.hz", .type = CFG_UINT, REPORT(hz),
      .help = "reports the profile took in the last whole second" },
    { .path = "report.polls", .type = CFG_UINT, REPORT(poll_count) },
    { .path = "report.attempts", .type = CFG_UINT, REPORT(send_attempts) },
    { .path = "report.accepted", .type = CFG_UINT, REPORT(send_accepted) },
    { .path = "report.busy", .type = CFG_UINT, REPORT(send_busy) },
    { .path = "report.deferred", .type = CFG_UINT, REPORT(send_deferred) },
    { .path = "report.errors", .type = CFG_UINT, REPORT(send_errors) },
    { .path = "report.air.sent", .type = CFG_UINT, AIR(sent),
      .help = "Classic input reports handed to the link since the HID channel opened" },
    { .path = "report.air.replaced", .type = CFG_UINT, AIR(replaced),
      .help = "input reports replaced by a newer one before the link took them" },
    { .path = "report.air.messages", .type = CFG_UINT, AIR(messages),
      .help = "queued reports (replies, events) sent" },
    { .path = "report.air.wait_avg_us", .type = CFG_UINT, AIR(wait_avg_us),
      .help = "input report posted -> handed to the link, average" },
    { .path = "report.air.wait_max_us", .type = CFG_UINT, AIR(wait_max_us) },
    { .path = "report.air.sniff_entries", .type = CFG_UINT, AIR(sniff_entries),
      .help = "times the host put the link into sniff" },

    { .path = "input.valid", .type = CFG_UINT, INPUT(valid_frames) },
    { .path = "input.crc", .type = CFG_UINT, INPUT(crc_failures) },
    { .path = "input.dropped", .type = CFG_UINT, INPUT(dropped_bytes) },
    { .path = "input.overflow", .type = CFG_UINT, INPUT(overflow_events) },
    { .path = "input.last_axes_ms", .type = CFG_UINT,
      INPUT(last_axes_timestamp_ms) },
    { .path = "input.dma_irq", .type = CFG_UINT, INPUT(dma_interrupts) },
    { .path = "input.timeout_irq", .type = CFG_UINT, INPUT(timeout_interrupts) },
    { .path = "input.max_backlog", .type = CFG_UINT, INPUT(max_backlog) },

    { .path = "inject.active", .type = CFG_BOOL, INJECT(active) },
    { .path = "inject.period_ms", .type = CFG_UINT, INJECT(period_ms) },
    { .path = "inject.duration_ms", .type = CFG_UINT, INJECT(duration_ms) },
    { .path = "inject.edges", .type = CFG_UINT, INJECT(emitted_edges) },
    { .path = "inject.overruns", .type = CFG_UINT, INJECT(overruns) },

    { .path = "led.scene", .type = CFG_ENUM, LIGHTS(scene),
      .choices = light_scenes, .help = "what the lights show (lights_render.c)" },
    { .path = "led.debug", .type = CFG_BOOL, LIGHTS(debug),
      .help = "debug led holds the lights; debug led off gives them back" },
    { .path = "led.low_battery", .type = CFG_BOOL, LIGHTS(low_battery) },
    { .path = "led.host_players", .type = CFG_BOOL, LIGHTS(has_players),
      .help = "the host set a player number" },
    { .path = "led.players_on", .type = CFG_HEX, LIGHTS(players_on),
      .help = "bar lights the host lit, bit 0 = top" },
    { .path = "led.players_flash", .type = CFG_HEX, LIGHTS(players_flash) },
    { .path = "led.host_lightbar", .type = CFG_BOOL, LIGHTS(has_lightbar),
      .help = "the host sent a lightbar setting (black turns grips off)" },
    { .path = "led.lightbar", .type = CFG_COLOUR, LIGHTS(lightbar),
      .help = "the colour received from the host for the grip lights" },
    { .path = "led.lit", .type = CFG_HEX, LIGHTS(lit),
      .help = "rrggbb per light as sent: Home, bar top to bottom, left grip, "
              "right grip, unused" },
    { .path = "rumble.active", .type = CFG_BOOL, RUMBLE(active) },
    { .path = "rumble.left", .type = CFG_UINT, RUMBLE(left) },
    { .path = "rumble.right", .type = CFG_UINT, RUMBLE(right) },
    { .path = "rumble.updates", .type = CFG_UINT, RUMBLE(updates) },
    { .path = "rumble.max_latency_ms", .type = CFG_UINT,
      RUMBLE(max_apply_latency_ms) },

    { .path = "imu.state", .type = CFG_ENUM, IMU(state), .choices = imu_states },
    { .path = "imu.id", .type = CFG_UINT, IMU(imu.device_id) },
    { .path = "imu.samples", .type = CFG_UINT, IMU(imu.samples) },
    { .path = "imu.errors", .type = CFG_UINT, IMU(imu.errors) },
    { .path = "imu.asleep", .type = CFG_BOOL, IMU(imu.asleep),
      .help = "accelerometer and gyro switched off: the profile does not use them" },
    { .path = "imu.read_us", .type = CFG_UINT, IMU(imu.read_us),
      .help = "time the last sample read took on the I2C bus" },

    { .path = "wake.active", .type = CFG_BOOL, WAKE(active) },
    { .path = "wake.slot", .type = CFG_UINT, WAKE(slot) },
    { .path = "profile_switch_wake.channel", .type = CFG_UINT, WAKE(channel) },
    { .path = "wake.starts", .type = CFG_UINT, WAKE(starts) },
    { .path = "wake.elapsed_ms", .type = CFG_UINT, WAKE(elapsed_ms) },
};

const u16 status_item_count = sizeof(status_items) / sizeof(status_items[0]);
