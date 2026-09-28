#include <system/includes.h>

#include "btstack_jl.h"
#include "calibration.h"
#include "console.h"
#include "controller.h"
#include "hci_dump_console.h"
#include "imu.h"
#include "input.h"
#include "main.h"
#include "profile_classic.h"
#include "rumble.h"
#include "usb.h"
#include "wake/wake.h"

/*
 * The console's commands: things that do something.  Settings are not
 * commands - they are `config.PATH = VALUE` (console.c, config_schema.c) -
 * and state is `status.PATH` (status.c).
 */

#define MASKROM_NVRAM_ADDRESS ((const volatile u8 *)0x00010800U)

extern void cyclone2_usb_uboot_request(void);

/* ---------------------------------------------------------------- parsing */

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
    u32 value;

    if (number(text, &value) != 0 || value > 3U) return -1;
    *slot = (u8)value;
    return 0;
}

static int busy(void)
{
    return console_fail("a profile transition is already in progress");
}

/* ------------------------------------------------------------------ radio */

static int cmd_connect(int argc, char **argv)
{
    u8 slot;
    int result;

    if (argc != 2 || slot_argument(argv[1], &slot) != 0) return CONSOLE_USAGE;
    result = cyclone2_connect_request(slot);
    if (result < 0) return busy();
    console_ok(result ? "reconnecting slot %u" : "already connected to slot %u",
               slot);
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
    console_ok("slot %u forgot its bond%s and is waiting to pair", slot,
               fresh ? ", has a new address," : "");
    return 0;
}

static int cmd_wake(int argc, char **argv)
{
    u32 channel = WAKE_CHANNEL_DEFAULT;
    u32 seconds = WAKE_SECONDS_DEFAULT;
    u8 slot;
    int result;

    if (argc == 2 && !strcmp(argv[1], "stop")) {
        wake_stop();
        console_ok("wake stopped");
        return 0;
    }
    if (argc < 2 || argc > 4 || slot_argument(argv[1], &slot) != 0 ||
        (argc > 2 && (number(argv[2], &channel) != 0 || channel > WAKE_CHANNEL_MAX)) ||
        (argc > 3 && (number(argv[3], &seconds) != 0 ||
                      seconds < WAKE_SECONDS_MIN || seconds > WAKE_SECONDS_MAX)))
        return CONSOLE_USAGE;
    result = cyclone2_wake_request(slot, (u8)channel, (u8)seconds);
    if (result > 0) console_ok("waking slot %u's console", slot);
    else if (result == -3) return console_fail("slot %u has no console address yet; connect it once first", slot);
    else if (result == -2) return CONSOLE_USAGE;
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

static int cmd_hcidump(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "on")) cyclone2_hci_dump_set_packets(1U);
    else if (argc == 2 && !strcmp(argv[1], "off")) cyclone2_hci_dump_set_packets(0U);
    else if (argc != 1) return CONSOLE_USAGE;
    console_ok("HCI packet trace %s",
               cyclone2_hci_dump_packets_enabled() ? "on" : "off");
    return 0;
}

/* --------------------------------------------------------------- controls */

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
    } else if (!strcmp(argv[1], "raw")) {
        struct cyclone2_raw_input raw;
        struct cyclone2_input_report report;

        /* The poll read the sensors at most one control period ago. */
        cyclone2_input_get_raw(&raw);
        cyclone2_calibration_apply(&raw, &report);
        console_printf("raw L %04x %04x R %04x %04x L2 %u R2 %u stop %u%u valid %u\r\n"
                       "out L %d %d R %d %d L2 %u R2 %u\r\n",
                       raw.stick[0], raw.stick[1], raw.stick[2], raw.stick[3],
                       raw.trigger[0], raw.trigger[1], raw.trigger_stop[0],
                       raw.trigger_stop[1], raw.sticks_valid,
                       report.left_x, report.left_y, report.right_x,
                       report.right_y, report.left_trigger, report.right_trigger);
    } else {
        return CONSOLE_USAGE;
    }
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

/* ------------------------------------------------------------ diagnostics */

/* One short line for tools that poll. */
static int cmd_stat(int argc, char **argv)
{
    struct cyclone2_connection_status status;

    (void)argv;
    if (argc != 1) return CONSOLE_USAGE;
    cyclone2_connection_status_get(&status);
    console_printf("stat up=%u boots=%08x state=%u addr=%02x%02x%02x%02x%02x%02x reset=%u p33=%u btidle=%u\r\n",
                   (unsigned)sys_timer_get_ms(), (unsigned)cyclone2_boot_count,
                   (unsigned)status.connection_state,
                   status.local_address[5], status.local_address[4],
                   status.local_address[3], status.local_address[2],
                   status.local_address[1], status.local_address[0],
                   (unsigned)cyclone2_reset_source, (unsigned)cyclone2_reset_p33,
                   (unsigned)btstack_jl_core_idle());
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

/* The controller's pairing records in retention memory below the MaskROM
 * boot word: does a clear take effect? */
static int cmd_bondstore(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return CONSOLE_USAGE;
    dump(0x10700U, 0x100U, 0);
    return 0;
}

/* Every non-blank line of the controller's retention memory. */
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

/* Raw RAM access, internal RAM only, for finding where the controller
 * library keeps state.  Addresses are hex, with or without 0x. */
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

/* ------------------------------------------------------------------ table */

static const char *const on_off[] = { "on", "off", NULL };
static const char *const slots[] = { "0", "1", "2", "3", NULL };
static const char *const wake_words[] = { "0", "1", "2", "3", "stop", NULL };
static const char *const cal_words[] = { "start", "done", "cancel", "raw", NULL };
static const char *const inject_words[] = { "a", "stop", NULL };
static const char *const imu_words[] = { "init", "stop", "sample", NULL };
static const char *const input_words[] = { "reset", NULL };
static const char *const rumble_words[] = { "stop", NULL };
static const char *const reset_words[] = { "--no-commit", NULL };

const struct console_command console_commands[] = {
    { "config", "[.PATH] [= VALUE]", "show or change settings (Tab completes paths)",
      NULL, NULL, 1 },
    { "status", "[.PATH]", "show what the pad is doing (read-only)", NULL, NULL, 1 },
    { "help", "[COMMAND|PATH]", "this list, or help on one command or setting",
      console_cmd_help, NULL, 1 },
    { "unset", "PATH", "put settings back to their defaults (`unset config`: all "
      "but autocommit)", console_cmd_unset, NULL, 1 },
    { "export", "[PATH]", "the settings that differ from the defaults, as lines "
      "import takes (keys included)", console_cmd_export, NULL, 1 },
    { "import", NULL, "read settings lines until `end`; applied together or not "
      "at all", console_cmd_import, NULL, 0 },
    { "commit", NULL, "write changed settings to flash now", console_cmd_commit,
      NULL, 0 },
    { "pending", NULL, "settings changed and not written yet", console_cmd_pending,
      NULL, 0 },
    { "factory-reset", "[--no-commit]", "every setting back to its default, "
      "written at once unless --no-commit", console_cmd_factory_reset,
      reset_words, 0 },
    { "connect", "N", "reconnect slot N to its bonded host", cmd_connect, slots, 0 },
    { "disconnect", NULL, "stop Bluetooth without sleeping", cmd_disconnect, NULL, 0 },
    { "pair", "N [--fresh]", "forget slot N's bond and wait to pair (--fresh: "
      "with a new address)", cmd_pair, slots, 0 },
    { "wake", "N [CHANNEL] [SECONDS] | stop", "wake slot N's sleeping Switch, "
      "then reconnect", cmd_wake, wake_words, 0 },
    { "reboot", NULL, "reset the chip into this build", cmd_reboot, NULL, 0 },
    { "uboot", NULL, "reboot into the BR23 UBOOT1.00 loader (flashing)",
      cmd_uboot, NULL, 0 },
    { "cal", "start|done|cancel|raw", "the stick and trigger calibration (as "
      "Home+M 3 s); raw: sensor values", cmd_cal, cal_words, 0 },
    { "inject", "a PERIOD_MS DURATION_MS | stop", "press A over and over "
      "(latency tests)", cmd_inject, inject_words, 0 },
    { "rumble", "LEFT RIGHT MS | stop", "run the motors (0..255)", cmd_rumble,
      rumble_words, 0 },
    { "imu", "init|stop|sample", "the motion sensor", cmd_imu, imu_words, 0 },
    { "input", "reset", "zero the input counters (status.input)", cmd_input,
      input_words, 0 },
    { "debug", "[on|off]", "log lines on the console", console_cmd_debug, on_off, 0 },
    { "hcidump", "[on|off]", "trace HCI packets to the log", cmd_hcidump, on_off, 0 },
    { "bleadv", "on|off", "LE-advertise the Switch 2 payload", cmd_bleadv, on_off, 0 },
    { "stat", NULL, "one line of numbers for tools", cmd_stat, NULL, 0 },
    { "bringup", NULL, "reset mailbox, bring-up markers and MaskROM NVRAM",
      cmd_bringup, NULL, 0 },
    { "bondstore", NULL, "dump the controller's pairing records", cmd_bondstore,
      NULL, 0 },
    { "clearkeys", NULL, "zero the controller's pairing records and restart",
      cmd_clearkeys, NULL, 0 },
    { "nvscan", NULL, "non-blank lines of the controller's retention memory",
      cmd_nvscan, NULL, 0 },
    { "delall", NULL, "ask the stack to forget every device", cmd_delall, NULL, 0 },
    { "peek", "ADDR [LEN]", "read RAM", cmd_peek, NULL, 0 },
    { "poke", "ADDR HEX", "write RAM", cmd_poke, NULL, 0 },
    { "complete", "LINE", "what Tab would offer for LINE (for tools)",
      console_cmd_complete, NULL, 0 },
};

const u16 console_command_count =
    sizeof(console_commands) / sizeof(console_commands[0]);
