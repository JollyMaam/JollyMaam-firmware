#include <system/includes.h>

#include "btstack_jl.h"
#include "calibration.h"
#include "config.h"
#include "console.h"
#include "controller.h"
#include "imu.h"
#include "input.h"
#include "main.h"
#include "profile.h"
#include "profile_classic.h"
#include "rumble.h"
#include "usb.h"
#include "profile_switch_wake.h"

#define MASKROM_NVRAM_ADDRESS ((const volatile u8 *)0x00010800U)

extern void cyclone2_usb_uboot_request(void);

static int number(const char *text, u32 *value)
{
    u32 result = 0U;
    u32 base = 10U;

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16U;
        text += 2;
    }
    if (!*text) return -1;
    for (; *text; text++) {
        u32 digit;

        if (*text >= '0' && *text <= '9') digit = (u32)(*text - '0');
        else if (base == 16U && *text >= 'a' && *text <= 'f') digit = (u32)(*text - 'a' + 10);
        else if (base == 16U && *text >= 'A' && *text <= 'F') digit = (u32)(*text - 'A' + 10);
        else return -1;
        if (result > (0xffffffffU - digit) / base) return -1;
        result = result * base + digit;
    }
    *value = result;
    return 0;
}

static int slot_argument(const char *text, u8 *slot)
{
    u8 index;
    u32 value;

    for (index = 0U; index < CONFIG_PROFILES; index++) {
        char name = cfg_slot_names[index][0];

        if (text[0] && !text[1] &&
            (text[0] == name || text[0] == (char)(name - 'A' + 'a'))) {
            *slot = index;
            return 0;
        }
    }
    if (number(text, &value) != 0 || value > 3U) return -1;
    *slot = (u8)value;
    return 0;
}

static int busy(void)
{
    return console_fail("a profile transition is already in progress");
}

static int cmd_connect(int argc, char **argv)
{
    u8 slot;
    int result;

    if (argc != 2 || slot_argument(argv[1], &slot) != 0) return CONSOLE_USAGE;
    result = cyclone2_connect_request(slot);
    if (result < 0) return busy();
    console_ok(result ? "reconnecting slot %s" : "already connected to slot %s",
               config_slot_name(slot));
    return 0;
}

static int cmd_disconnect(int argc, char **argv)
{
    int result;

    (void)argv;
    if (argc != 1) return CONSOLE_USAGE;
    result = cyclone2_disconnect_request();
    if (result < 0) return busy();
    console_ok(result ? "disconnecting" : "already disconnected");
    return 0;
}

static int cmd_pair(int argc, char **argv)
{
    u8 slot;
    int fresh = argc == 3 && !strcmp(argv[2], "--fresh");

    if ((argc != 2 && !fresh) || slot_argument(argv[1], &slot) != 0)
        return CONSOLE_USAGE;
    if ((fresh ? cyclone2_pair_fresh_request(slot) :
                 cyclone2_pair_request(slot)) <= 0) return busy();
    console_ok("slot %s forgot its bond%s and is waiting to pair",
               config_slot_name(slot), fresh ? ", has a new address," : "");
    return 0;
}

static int cmd_wake(int argc, char **argv)
{
    u32 channel = WAKE_CHANNEL_DEFAULT;
    u32 seconds = WAKE_SECONDS_DEFAULT;
    u8 slot;
    int result;

    if (argc == 2 && !strcmp(argv[1], "stop")) {
        cyclone2_wake_cancel();
        console_ok("wake stopped");
        return 0;
    }
    if (argc < 2 || argc > 4 || slot_argument(argv[1], &slot) != 0 ||
        (argc > 2 && (number(argv[2], &channel) != 0 || channel > WAKE_CHANNEL_MAX)) ||
        (argc > 3 && (number(argv[3], &seconds) != 0 ||
                      seconds < WAKE_SECONDS_MIN || seconds > WAKE_SECONDS_MAX)))
        return CONSOLE_USAGE;
    result = cyclone2_wake_request(slot, (u8)channel, (u8)seconds);
    if (result > 0) console_ok("waking slot %s's console", config_slot_name(slot));
    else if (result == -3) return console_fail("slot %s has no console address yet; connect it once first", config_slot_name(slot));
    else if (result == -2) return CONSOLE_USAGE;
    else if (result == -4) return console_fail("profile has no wake action");
    else return busy();
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return CONSOLE_USAGE;
    if (cyclone2_reboot_request() <= 0) return busy();
    console_ok("rebooting");
    return 0;
}

static int cmd_uboot(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return CONSOLE_USAGE;
    cyclone2_usb_uboot_request();
    return 0;
}

static int cmd_bleadv(int argc, char **argv)
{
    extern const u8 *cyclone2_switch2_adv_data(u8 *length);
    u8 length;

    if (argc == 2 && !strcmp(argv[1], "on")) {
        const u8 *data = cyclone2_switch2_adv_data(&length);

        btstack_jl_core_le_advertise(data, length);
        console_ok("LE advertising the Switch 2 payload");
    } else if (argc == 2 && !strcmp(argv[1], "off")) {
        btstack_jl_core_le_advertise(NULL, 0U);
        console_ok("LE advertising off");
    } else {
        return CONSOLE_USAGE;
    }
    return 0;
}

static int cmd_cal(int argc, char **argv)
{
    if (argc != 2) return CONSOLE_USAGE;
    if (!strcmp(argv[1], "start")) {
        if (cyclone2_calibration_start() != 0)
            return console_fail("calibration already running");
        console_ok("hands off the sticks and triggers");
    } else if (!strcmp(argv[1], "done")) {
        if (cyclone2_calibration_phase() != CYCLONE2_CAL_SWEEP)
            return console_fail("not in the sweep phase");
        cyclone2_calibration_finish();
        console_ok("finishing");
    } else if (!strcmp(argv[1], "cancel")) {
        cyclone2_calibration_cancel();
        console_ok("cancelled");
    } else {
        return CONSOLE_USAGE;
    }
    return 0;
}

static int cmd_sensors(int argc, char **argv)
{
    struct cyclone2_raw_input raw;
    struct cyclone2_input_report report;

    (void)argv;
    if (argc != 1) return CONSOLE_USAGE;

    cyclone2_input_get_raw(&raw);
    cyclone2_calibration_apply(&raw, &report);
    console_printf("raw L %04x %04x R %04x %04x L2 %u R2 %u stop %u%u valid %u\r\n"
                   "out L %d %d R %d %d L2 %u R2 %u\r\n",
                   raw.stick[0], raw.stick[1], raw.stick[2], raw.stick[3],
                   raw.trigger[0], raw.trigger[1], raw.trigger_stop[0],
                   raw.trigger_stop[1], raw.sticks_valid,
                   report.left_x, report.left_y, report.right_x,
                   report.right_y, report.left_trigger, report.right_trigger);
    return 0;
}

static int cmd_inject(int argc, char **argv)
{
    u32 period;
    u32 duration;

    if (argc == 2 && !strcmp(argv[1], "stop")) {
        cyclone2_inject_stop();
        console_ok("injection stopped");
        return 0;
    }
    if (argc != 4 || strcmp(argv[1], "a") || number(argv[2], &period) != 0 ||
        number(argv[3], &duration) != 0) return CONSOLE_USAGE;
    if (cyclone2_inject_start(period, duration) != 0)
        return console_fail("needs a connection, an even period of 20..1000 ms "
                            "and 1..60000 ms");
    console_ok("injection started");
    return 0;
}

static int cmd_rumble(int argc, char **argv)
{
    u32 left;
    u32 right;
    u32 duration;

    if (argc == 2 && !strcmp(argv[1], "stop")) {
        cyclone2_rumble_stop();
        console_ok("rumble stopped");
        return 0;
    }
    if (argc != 4 || number(argv[1], &left) != 0 || number(argv[2], &right) != 0 ||
        number(argv[3], &duration) != 0) return CONSOLE_USAGE;
    if (left > 255U || right > 255U ||
        duration > CYCLONE2_RUMBLE_MAX_DURATION_MS ||
        cyclone2_rumble_set((u8)left, (u8)right, (u16)duration) != 0)
        return console_fail("strength 0..255, duration 1..%u ms",
                            CYCLONE2_RUMBLE_MAX_DURATION_MS);
    console_ok("rumbling");
    return 0;
}

static int cmd_imu(int argc, char **argv)
{
    if (argc != 2) return CONSOLE_USAGE;
    if (!strcmp(argv[1], "init")) {
        cyclone2_imu_enable();
        console_ok("IMU initialization requested");
    } else if (!strcmp(argv[1], "stop")) {
        cyclone2_imu_disable();
        console_ok("IMU sampling stopped");
    } else if (!strcmp(argv[1], "sample")) {
        struct cyclone2_imu_sample sample;

        if (cyclone2_imu_latest(&sample) != 0)
            return console_fail("no IMU sample yet; imu init");
        console_printf("imu sample_ms=%u accel=%d,%d,%d gyro=%d,%d,%d\r\n",
                       sample.timestamp_ms, sample.accel_x, sample.accel_y,
                       sample.accel_z, sample.gyro_x, sample.gyro_y,
                       sample.gyro_z);
    } else {
        return CONSOLE_USAGE;
    }
    return 0;
}

static int cmd_input(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "reset")) return CONSOLE_USAGE;
    cyclone2_input_reset_stats();
    console_ok("input counters reset");
    return 0;
}

/* One line for tools that poll: uptime and boot counter (a reset shows as
 * either moving), what the radio is doing, and the reset causes.  The fields
 * are key=value and only ever appended to. */
static int cmd_stat(int argc, char **argv)
{
    static const char *const states[] = {
        "off", "connecting", "pairing", "connected", "stopping", "waking",
    };
    struct cyclone2_connection_status status;

    (void)argv;
    if (argc != 1) return CONSOLE_USAGE;
    cyclone2_connection_status_get(&status);
    console_printf("stat up=%u boots=%08x slot=%u profile=%s state=%s attempt=%u "
                   "addr=%02x%02x%02x%02x%02x%02x usb=%u reset=%u p33=%u rtc=%u "
                   "btidle=%u pending=%u\r\n",
                   (unsigned)sys_timer_get_ms(), (unsigned)cyclone2_boot_count,
                   status.slot, cyclone2_profile_name(status.profile_type),
                   status.connection_state < 6U ?
                   states[status.connection_state] : "unknown",
                   status.connect_attempts,
                   status.local_address[5], status.local_address[4],
                   status.local_address[3], status.local_address[2],
                   status.local_address[1], status.local_address[0],
                   status.usb_present, (unsigned)cyclone2_reset_source,
                   (unsigned)cyclone2_reset_p33, (unsigned)cyclone2_reset_rtc,
                   (unsigned)btstack_jl_core_idle(), (unsigned)config_pending());
    return 0;
}

/* The reset mailbox, the newest bring-up markers and the MaskROM NVRAM. */
static int cmd_bringup(int argc, char **argv)
{
    u32 head = cyclone2_bringup_log_head;
    unsigned count = head < 96U ? (unsigned)head : 96U;
    unsigned first = (unsigned)(head - count);
    unsigned i;

    (void)argv;
    if (argc != 1) return CONSOLE_USAGE;
    console_write("reset-mailbox=");
    console_hex((const u8 *)&reset_mailbox, sizeof(reset_mailbox));
    console_printf(" boots=%08x bt-trace=%08x bt-log%08x/%08x:",
                   (unsigned)cyclone2_boot_count,
                   (unsigned)cyclone2_bringup_trace,
                   (unsigned)cyclone2_bringup_log_head,
                   (unsigned)cyclone2_boot_count);
    for (i = 0U; i < count; i++)
        console_printf("%08x ", (unsigned)cyclone2_bringup_log[
                       (first + i) % CYCLONE2_BRINGUP_LOG_ENTRIES]);
    console_write(" nvram=");
    console_hex((const u8 *)MASKROM_NVRAM_ADDRESS, 16U);
    console_printf("\r\nreset-source=%u p33=%u rtc=%u uptime=%u ms\r\n",
                   (unsigned)cyclone2_reset_source, (unsigned)cyclone2_reset_p33,
                   (unsigned)cyclone2_reset_rtc, (unsigned)sys_timer_get_ms());
    return 0;
}

static void dump(u32 address, u32 length, int skip_blank)
{
    const volatile u8 *base = (const volatile u8 *)address;
    u32 offset;

    for (offset = 0U; offset < length; offset += 16U) {
        u32 index;

        if (skip_blank) {
            for (index = 0U; index < 16U; index++)
                if (base[offset + index] != 0x00U && base[offset + index] != 0xffU)
                    break;
            if (index == 16U) continue;
        }
        console_printf("%05x:", (unsigned)(address + offset));
        for (index = 0U; index < 16U && offset + index < length; index++)
            console_printf(" %02x", base[offset + index]);
        console_write("\r\n");
    }
}

static int cmd_bondstore(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return CONSOLE_USAGE;
    dump(0x10700U, 0x100U, 0);
    return 0;
}

static int cmd_nvscan(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return CONSOLE_USAGE;
    dump(0x10000U, 0x800U, 1);
    return 0;
}

static int cmd_clearkeys(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return CONSOLE_USAGE;
    if (cyclone2_clear_keys_request() <= 0) return busy();
    console_ok("clearing the controller's pairing records and restarting");
    return 0;
}

static int cmd_delall(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return CONSOLE_USAGE;
    cyclone2_classic_forget_all();
    console_ok("asked the stack to forget every device");
    return 0;
}

static int ram_address(const char *text, u32 *address)
{
    char prefixed[12];

    if (!(text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))) {
        if (strlen(text) > 8U) return -1;
        snprintf(prefixed, sizeof(prefixed), "0x%s", text);
        text = prefixed;
    }
    return number(text, address) != 0 || *address > 0x3ffffU ? -1 : 0;
}

static int cmd_peek(int argc, char **argv)
{
    u32 address;
    u32 length = 16U;

    if (argc < 2 || argc > 3 || ram_address(argv[1], &address) != 0 ||
        (argc == 3 && (number(argv[2], &length) != 0 || !length || length > 64U)))
        return CONSOLE_USAGE;
    console_hex((const u8 *)address, length);
    console_write("\r\n");
    return 0;
}

static int cmd_poke(int argc, char **argv)
{
    u8 bytes[32];
    u32 address;
    u32 count = 0U;
    const char *hex;

    if (argc != 3 || ram_address(argv[1], &address) != 0) return CONSOLE_USAGE;
    for (hex = argv[2]; hex[0] && hex[1] && count < sizeof(bytes); hex += 2) {
        u32 value;
        char prefixed[5] = { '0', 'x', hex[0], hex[1], '\0' };

        if (number(prefixed, &value) != 0) return CONSOLE_USAGE;
        bytes[count++] = (u8)value;
    }
    if (!count || *hex) return CONSOLE_USAGE;
    memcpy((void *)address, bytes, count);
    __asm__ volatile("csync");
    console_write("OK wrote ");
    console_hex(bytes, count);
    console_write("\r\n");
    return 0;
}

static const char *const on_off[] = { "on", "off", NULL };
static const char *const slots[] = { "A", "B", "X", "Y", NULL };
static const char *const wake_words[] = { "A", "B", "X", "Y", "stop", NULL };
static const char *const cal_words[] = { "start", "done", "cancel", NULL };
static const char *const inject_words[] = { "a", "stop", NULL };
static const char *const imu_words[] = { "init", "stop", "sample", NULL };
static const char *const input_words[] = { "reset", NULL };
static const char *const rumble_words[] = { "stop", NULL };
static const char *const reset_words[] = { "--all", NULL };
static const char *const get_words[] = { "--changed", NULL };

static const struct console_command config_commands[] = {
    { "get", "[PATH] [--changed]", "the settings under PATH; --changed: only "
      "those not at their default, as lines that restore them", config_cmd_get,
      get_words, CONSOLE_PATHS_CONFIG },
    { "reset", "PATH | --all [--no-commit]", "back to the default: PATH, or "
      "every setting (written at once)", config_cmd_reset, reset_words,
      CONSOLE_PATHS_CONFIG },
    { "commit", NULL, "write changed settings to flash now", config_cmd_commit },
    { "pending", NULL, "settings changed and not written yet", config_cmd_pending },
    { NULL },
};

static const struct console_command status_commands[] = {
    { "get", "[PATH]", "the values under PATH", status_cmd_get, NULL,
      CONSOLE_PATHS_STATUS },
    { NULL },
};

static const struct console_command debug_commands[] = {
    { "stat", NULL, "one key=value line for tools: uptime, boots, state, ...",
      cmd_stat },
    { "sensors", NULL, "raw stick and trigger values and what they map to",
      cmd_sensors },
    { "wake", "SLOT [CHANNEL] [SECONDS] | stop", "wake a slot's console, "
      "then reconnect", cmd_wake, wake_words },
    { "inject", "a PERIOD_MS DURATION_MS | stop", "press A over and over "
      "(latency tests)", cmd_inject, inject_words },
    { "rumble", "LEFT RIGHT MS | stop", "run the motors (0..255)", cmd_rumble,
      rumble_words },
    { "imu", "init|stop|sample", "the motion sensor", cmd_imu, imu_words },
    { "input", "reset", "zero the input counters (status input)", cmd_input,
      input_words },
    { "bleadv", "on|off", "LE-advertise the Switch 2 payload", cmd_bleadv, on_off },
    { "bringup", NULL, "reset mailbox, bring-up markers and MaskROM NVRAM",
      cmd_bringup },
    { "bondstore", NULL, "the controller's own pairing records", cmd_bondstore },
    { "clearkeys", NULL, "zero the controller's pairing records and restart",
      cmd_clearkeys },
    { "nvscan", NULL, "non-blank lines of the controller's retention memory",
      cmd_nvscan },
    { "delall", NULL, "ask the stack to forget every device", cmd_delall },
    { "peek", "ADDR [LEN]", "read RAM (hex address)", cmd_peek },
    { "poke", "ADDR HEX", "write RAM: contiguous hex, deadbeef", cmd_poke },
    { "complete", "LINE", "what Tab would offer for LINE", console_cmd_complete },
    { NULL },
};

const struct console_command console_commands[] = {
    { "config", NULL, "settings: show, change, save, restore (config help)",
      .subcommands = config_commands,
      .overview =
      "The pad's settings.  A change is written to flash by itself a moment\r\n"
      "after it is made (global.autocommit), never in the middle of a pairing.\r\n"
      "After config, a verb (below) or else a path: config profile.A.type.\r\n"
      "\r\n"
      "  config PATH                      a setting, or everything under it\r\n"
      "  config PATH=VALUE                change it; says when it takes effect\r\n"
      "  config PATH help                 what it is, its choices and default\r\n" },
    { "status", NULL, "what the pad is doing, read-only (status help)",
      .subcommands = status_commands,
      .overview =
      "What the pad is doing, read-only: status get, or a path: status radio.\r\n"
      "\r\n"
      "  status PATH                      a value, or everything under it\r\n"
      "  status PATH help                 what a value means\r\n" },
    { "connect", "SLOT", "reconnect a slot to its bonded host", cmd_connect, slots },
    { "disconnect", NULL, "stop Bluetooth without sleeping", cmd_disconnect },
    { "pair", "SLOT [--fresh]", "forget a slot's bond and wait to pair; --fresh: "
      "new address", cmd_pair, slots },
    { "cal", "start|done|cancel", "calibrate the sticks and triggers (Home+M 3 s)",
      cmd_cal, cal_words },
    { "reboot", NULL, "restart into this build", cmd_reboot },
    { "uboot", NULL, "restart into the UBOOT1.00 loader, for flashing", cmd_uboot },
    { "debug", NULL, "diagnostics and tests (debug help)",
      .subcommands = debug_commands,
      .overview = "Diagnostics and tests.  `debug NAME help` explains one.\r\n\r\n" },
    { "help", "[TOPIC]", "this, or help on a command or setting", console_cmd_help,
      NULL, CONSOLE_PATHS_ANY },
    { NULL },
};
