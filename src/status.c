#include <system/includes.h>
#include <asm/adc_api.h>
#include <asm/charge.h>

#include "btstack_jl.h"
#include "calibration.h"
#include "config.h"
#include "controller.h"
#include "imu.h"
#include "input.h"
#include "main.h"
#include "profile.h"
#include "rumble.h"
#include "wake/wake.h"

/*
 * The status namespace: read-only leaves, described like the settings.  Each
 * leaf is a member of a struct its getter fills, so a group of related values
 * costs one small function.
 */

extern u16 config_dropped_records;
extern u16 config_schema_errors;

static const struct cfg_choice radio_states[] = {
    { 0, "off" }, { 1, "connecting" }, { 2, "pairing" }, { 3, "connected" },
    { 4, "stopping" }, { 5, "waking" }, { 0, NULL },
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

/* ------------------------------------------------------------- getters */

struct status_system {
    u32 uptime_ms;
    u32 boots;
    u8 reset_source;
    u8 reset_p33;
    u8 reset_rtc;
    u8 usb;
    u32 vbat_mv;
    u8 charge;
    u8 cal_phase;
    u8 config_pending;
    u8 commit_queued;
    u16 dropped_records;
    u16 schema_errors;
    u8 flash_uid[16];
};

static void get_system(void *out)
{
    struct status_system *s = out;
    const u8 *uid = config_flash_uid();

    s->uptime_ms = sys_timer_get_ms();
    s->boots = cyclone2_boot_count;
    s->reset_source = cyclone2_reset_source;
    s->reset_p33 = cyclone2_reset_p33;
    s->reset_rtc = cyclone2_reset_rtc;
    s->usb = (u8)cyclone2_usb_present();
    /* Only these two reads: the other charge-driver calls block when the
     * driver is not initialised, as in this build (docs/charging.md). */
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
    u8 slot_address[CONFIG_PROFILES][6];
};

static void get_radio(void *out)
{
    struct status_radio *s = out;
    u8 slot;

    cyclone2_connection_status_get(&s->connection);
    s->idle = btstack_jl_core_idle();
    for (slot = 0U; slot < CONFIG_PROFILES; slot++)
        config_slot_address(slot, s->slot_address[slot]);
}

static void get_report(void *out)
{
    cyclone2_report_statistics_get(out);
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

/* --------------------------------------------------------------- table */

#define SYSTEM(member)  CFG_STATUS_FIELD(struct status_system, member, get_system)
#define RADIO(member)   CFG_STATUS_FIELD(struct status_radio, member, get_radio)
#define REPORT(member)  CFG_STATUS_FIELD(struct cyclone2_report_statistics, member, get_report)
#define INPUT(member)   CFG_STATUS_FIELD(struct cyclone2_input_stats, member, get_input)
#define INJECT(member)  CFG_STATUS_FIELD(struct cyclone2_injection_status, member, get_inject)
#define RUMBLE(member)  CFG_STATUS_FIELD(struct cyclone2_rumble_status, member, get_rumble)
#define IMU(member)     CFG_STATUS_FIELD(struct status_imu, member, get_imu)
#define WAKE(member)    CFG_STATUS_FIELD(struct wake_status, member, get_wake)

const struct cfg_item status_items[] = {
    { .path = "uptime_ms", .type = CFG_UINT, SYSTEM(uptime_ms) },
    { .path = "boots", .type = CFG_UINT, SYSTEM(boots),
      .help = "boot counter (survives resets, not power loss)" },
    { .path = "reset.source", .type = CFG_UINT, SYSTEM(reset_source) },
    { .path = "reset.p33", .type = CFG_UINT, SYSTEM(reset_p33) },
    { .path = "reset.rtc", .type = CFG_UINT, SYSTEM(reset_rtc) },
    { .path = "usb", .type = CFG_BOOL, SYSTEM(usb), .help = "USB power seen" },
    { .path = "battery.vbat_mv", .type = CFG_UINT, SYSTEM(vbat_mv),
      .help = "not wired to the cell on this board; see docs/charging.md" },
    { .path = "battery.charging", .type = CFG_BOOL, SYSTEM(charge) },
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
    { .path = "profile.#.address", .type = CFG_MAC, RADIO(slot_address[0]),
      .stride = 6, .count = CONFIG_PROFILES, .names = cfg_slot_names,
      .help = "the address the slot uses: its bond's, or the derived one" },

    { .path = "report.polls", .type = CFG_UINT, REPORT(poll_count) },
    { .path = "report.attempts", .type = CFG_UINT, REPORT(send_attempts) },
    { .path = "report.accepted", .type = CFG_UINT, REPORT(send_accepted) },
    { .path = "report.busy", .type = CFG_UINT, REPORT(send_busy) },
    { .path = "report.deferred", .type = CFG_UINT, REPORT(send_deferred) },
    { .path = "report.errors", .type = CFG_UINT, REPORT(send_errors) },

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

    { .path = "wake.active", .type = CFG_BOOL, WAKE(active) },
    { .path = "wake.slot", .type = CFG_UINT, WAKE(slot) },
    { .path = "wake.channel", .type = CFG_UINT, WAKE(channel) },
    { .path = "wake.starts", .type = CFG_UINT, WAKE(starts) },
    { .path = "wake.elapsed_ms", .type = CFG_UINT, WAKE(elapsed_ms) },
};

const u16 status_item_count = sizeof(status_items) / sizeof(status_items[0]);
