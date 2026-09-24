#include <system/includes.h>
#include <stdarg.h>
#include <asm/adc_api.h>
#include <asm/charge.h>
#include <asm/irq.h>
#include <asm/power_interface.h>
#include <asm/wdt.h>

#include "controller.h"
#include "btstack_jl.h"
#include "hci_dump_console.h"
#include "input.h"
#include "imu.h"
#include "led.h"
#include "main.h"
#include "profile.h"
#include "rom.h"
#include "rumble.h"
#include "settings.h"
#include "usb.h"
#include "usb/device/cdc.h"
#include "usb/device/usb_stack.h"

#define USB_CON0              (*(volatile u32 *)0x001e1800U)
#define USB_CON1              (*(volatile u32 *)0x001e1804U)
#define USB_IO_CON0           (*(volatile u32 *)0x001e5100U)
#define USB_FORCE_SE0         BIT(11)
#define MASKROM_NVRAM_ADDRESS ((volatile u8 *)0x00010800U)
#define COMMAND_LINE_SIZE     80U
#define RESPONSE_DELAY_TICKS  30U
#define DEBUG_LINE_COUNT       16U
#define DEBUG_LINE_SIZE        96U

struct debug_line {
    u32 timestamp_ms;
    u8 length;
    char text[DEBUG_LINE_SIZE];
};

static char command_line[COMMAND_LINE_SIZE];
static u8 command_length;
static u8 last_was_cr;
static volatile u8 receive_pending;
static u8 greeting_pending;
static u8 uboot_pending;
static u8 response_delay;
static u8 debug_enabled = 1U;
static u8 debug_read_index;
static u8 debug_write_index;
static u8 terminal_was_open;
static struct debug_line debug_lines[DEBUG_LINE_COUNT];

extern void usb_start(void);
extern void nvram_set_boot_state(u32 state);
extern void flushinv_dcache(const void *address, u32 size);
extern u8 cyclone2_cdc_terminal_open(void);

static void console_write_bytes(const void *data, u32 length)
{
    cdc_write_data(USB0, (u8 *)data, length);
}

static void console_write(const char *text)
{
    console_write_bytes(text, strlen(text));
}

static void console_write_hex(const u8 *data, u32 length)
{
    static const char digits[] = "0123456789abcdef";
    char encoded[32];
    u32 count;
    u32 index;

    while (length != 0U) {
        count = length > sizeof(encoded) / 2U ? sizeof(encoded) / 2U : length;
        for (index = 0; index < count; index++) {
            encoded[index * 2U] = digits[data[index] >> 4];
            encoded[index * 2U + 1U] = digits[data[index] & 0x0fU];
        }
        console_write_bytes(encoded, count * 2U);
        data += count;
        length -= count;
    }
}

static void console_prompt(void)
{
    console_write("c2> ");
}

void cyclone2_usb_debug(const char *format, ...)
{
    struct debug_line *line;
    u8 next;
    int length;
    va_list arguments;

    if (!debug_enabled) return;
    next = (u8)((debug_write_index + 1U) % DEBUG_LINE_COUNT);
    if (next == debug_read_index) {
        debug_read_index = (u8)((debug_read_index + 1U) % DEBUG_LINE_COUNT);
    }

    line = &debug_lines[debug_write_index];
    line->timestamp_ms = sys_timer_get_ms();
    va_start(arguments, format);
    length = vsnprintf(line->text, sizeof(line->text), format, arguments);
    va_end(arguments);
    if (length < 0) return;
    if ((u32)length >= sizeof(line->text)) length = sizeof(line->text) - 1U;
    while (length > 0 &&
           (line->text[length - 1] == '\r' || line->text[length - 1] == '\n'))
        length--;
    if (length == 0) return;
    line->length = (u8)length;
    debug_write_index = next;
}

static void flush_debug_line(void)
{
    struct debug_line *line;
    char prefix[32];
    int prefix_length;

    if (!debug_enabled || !cyclone2_cdc_terminal_open() ||
        debug_read_index == debug_write_index) return;
    line = &debug_lines[debug_read_index];
    prefix_length = snprintf(prefix, sizeof(prefix), "\r\n[%u.%03u] ",
                             line->timestamp_ms / 1000U,
                             line->timestamp_ms % 1000U);
    if (prefix_length <= 0 ||
        cdc_write_data(USB0, (u8 *)prefix, (u32)prefix_length) !=
        (u32)prefix_length) return;
    console_write_bytes(line->text, line->length);
    console_write("\r\n");
    debug_read_index = (u8)((debug_read_index + 1U) % DEBUG_LINE_COUNT);
}

static void arm_maskrom_usb_token(void)
{
    nvram_set_boot_state(2U);
    __asm__ volatile("csync");
    flushinv_dcache((const void *)MASKROM_NVRAM_ADDRESS, 16U);
}

static void print_status(void)
{
    const u8 *mailbox = (const u8 *)&reset_mailbox;

    console_write("reset-mailbox=");
    console_write_hex(mailbox, sizeof(reset_mailbox));
    console_write(" boots=");
    console_write_hex((const u8 *)&cyclone2_boot_count, 4U);
    console_write(" bt-trace=");
    console_write_hex((const u8 *)&cyclone2_bringup_trace, 4U);
    console_write(" bt-log");
    console_write_hex((const u8 *)&cyclone2_bringup_log_head, 4U);
    console_write("/");
    console_write_hex((const u8 *)&cyclone2_boot_count, 4U);
    console_write(":");
    /* Oldest entry first, so the end of the line is the most recent step. */
    for (unsigned i = 0U; i < CYCLONE2_BRINGUP_LOG_ENTRIES; i++) {
        unsigned index = (cyclone2_bringup_log_head + i) %
                         CYCLONE2_BRINGUP_LOG_ENTRIES;
        {
            const u8 *entry = (const u8 *)&cyclone2_bringup_log[index];
            u32 value = cyclone2_bringup_log[index];
            (void) value;
            for (unsigned j = 4U; j > 0U; j--) {
                console_write_hex(&entry[j - 1U], 1U);
            }
        }
        console_write_bytes((const u8 *)" ", 1U);
    }
    console_write(" nvram=");
    console_write_hex((const u8 *)MASKROM_NVRAM_ADDRESS, 16U);
    console_write("\r\n");
    {
        char line[96];
        int length = snprintf(line, sizeof(line),
                              "reset-source=%u p33=%u rtc=%u uptime=%u ms\r\n",
                              (unsigned)cyclone2_reset_source,
                              (unsigned)cyclone2_reset_p33,
                              (unsigned)cyclone2_reset_rtc,
                              (unsigned)sys_timer_get_ms());
        if (length > 0) console_write_bytes(line, (u32)length);
    }
}

__attribute__((noinline)) static void print_report_status(void)
{
    struct cyclone2_report_statistics stats;
    char line[160];
    int length;

    cyclone2_report_statistics_get(&stats);
    length = snprintf(line, sizeof(line),
                      "report period=%u ms polls=%u attempts=%u accepted=%u busy=%u deferred=%u errors=%u\r\n",
                      cyclone2_report_period_get(), stats.poll_count,
                      stats.send_attempts, stats.send_accepted,
                      stats.send_busy, stats.send_deferred,
                      stats.send_errors);
    if (length > 0) console_write_bytes(line, (u32)length);
}

static const char *connection_state_name(u8 state)
{
    static const char *const names[] = {
        "off", "connecting", "pairing", "connected", "stopping"
    };
    return state < sizeof(names) / sizeof(names[0]) ? names[state] : "unknown";
}

/* One short line with the numbers a host wants to poll, without the 8 KB
 * bring-up ring that `status` prints.  A session tool asks for this several
 * times a second; the full status line drowns everything else. */
static void print_compact_status(void)
{
    struct cyclone2_connection_status status;
    char line[128];
    int length;

    cyclone2_connection_status_get(&status);
    length = snprintf(line, sizeof(line),
                      "stat up=%u boots=%08x state=%u addr=%02x%02x%02x%02x%02x%02x reset=%u p33=%u\r\n",
                      (unsigned)sys_timer_get_ms(),
                      (unsigned)cyclone2_boot_count,
                      (unsigned)status.connection_state,
                      status.local_address[5], status.local_address[4],
                      status.local_address[3], status.local_address[2],
                      status.local_address[1], status.local_address[0],
                      (unsigned)cyclone2_reset_source,
                      (unsigned)cyclone2_reset_p33);
    if (length > 0) console_write_bytes(line, (u32)length);
}

__attribute__((noinline)) static void print_connection_status(void)
{
    struct cyclone2_connection_status status;
    char line[128];
    int length;

    /* Compact status has its own command; see print_compact_status(). */

    cyclone2_connection_status_get(&status);
    length = snprintf(line, sizeof(line),
                      "slot=%u profile=%s state=%s attempt=%u address=%02x:%02x:%02x:%02x:%02x:%02x usb=%s\r\n",
                      status.slot, status.profile_type == CYCLONE2_PROFILE_GENERIC ?
                      "generic" : "switch", connection_state_name(status.connection_state),
                      status.connect_attempts,
                      status.local_address[5], status.local_address[4],
                      status.local_address[3], status.local_address[2],
                      status.local_address[1], status.local_address[0],
                      status.usb_present ? "present" : "absent");
    if (length > 0) console_write_bytes(line, (u32)length);
}

__attribute__((noinline)) static void print_injection_status(void)
{
    struct cyclone2_injection_status status;
    char line[128];
    int length;

    cyclone2_injection_status_get(&status);
    length = snprintf(line, sizeof(line),
                      "inject=%s a=%u period=%u duration=%u edges=%u overruns=%u\r\n",
                      status.active ? "active" : "stopped", status.synthetic_a,
                      status.period_ms, status.duration_ms, status.emitted_edges,
                      status.overruns);
    if (length > 0) console_write_bytes(line, (u32)length);
}

__attribute__((noinline)) static void print_input_status(void)
{
    struct cyclone2_input_stats stats;
    char line[192];
    int length;

    cyclone2_input_get_stats(&stats);
    length = snprintf(line, sizeof(line),
                      "input dropped=%u crc=%u overflow=%u valid=%u last_axes_ms=%u dma_irq=%u timeout_irq=%u max_backlog=%u\r\n",
                      stats.dropped_bytes, stats.crc_failures,
                      stats.overflow_events, stats.valid_frames,
                      stats.last_axes_timestamp_ms, stats.dma_interrupts,
                      stats.timeout_interrupts, stats.max_backlog);
    if (length > 0) console_write_bytes(line, (u32)length);
}

/* The Bluetooth controller keeps its pairing records in retention memory just
 * below the MaskROM boot word; dump them so the console can show whether a
 * clear took effect. */
static void print_bond_store(void)
{
    const volatile u8 *store = (const volatile u8 *)0x00010700U;
    char line[80];
    u32 base;
    u32 index;

    for (base = 0U; base < 0x100U; base += 16U) {
        int length = snprintf(line, sizeof(line), "%04x:", 0x10700U + base);
        for (index = 0U; index < 16U && length > 0; index++) {
            length += snprintf(line + length, sizeof(line) - (size_t)length,
                               " %02x", store[base + index]);
        }
        if (length > 0) {
            console_write_bytes(line, (u32)length);
            console_write("\r\n");
        }
    }
}

/* The host's own link-key store, one line per record.
 *
 * This is the database that answers the controller's LINK_KEY_REQUEST, so it
 * is the thing that has to hold the key a pairing produced.  Printing it by
 * raw slot (rather than through the stack's iterator) shows an empty store
 * plainly: count=0 means the key never reached the host. */
static void print_link_keys(void)
{
    u8 address[6];
    u8 key[16];
    u8 type;
    unsigned count = 0U;
    unsigned slot;
    char line[80];

    for (slot = 0U; slot < BTSTACK_JL_LINK_KEY_SLOTS; slot++) {
        int length;

        if (!btstack_jl_link_key_store_read((u8)slot, address, key, &type))
            continue;
        count++;
        length = snprintf(line, sizeof(line),
                          "key slot=%u peer=%02x:%02x:%02x:%02x:%02x:%02x type=%u key=",
                          slot, address[5], address[4], address[3],
                          address[2], address[1], address[0], (unsigned)type);
        if (length > 0) {
            console_write_bytes(line, (u32)length);
            console_write_hex(key, sizeof(key));
            console_write("\r\n");
        }
    }
    {
        int length = snprintf(line, sizeof(line), "keys count=%u\r\n", count);
        if (length > 0) console_write_bytes(line, (u32)length);
    }
}

/* Print every 16-byte line of the controller's retention memory that is not
 * blank, so a pairing record (or a half-erased one) shows up wherever it
 * lives. */
static void print_nvram_scan(void)
{
    const volatile u8 *base = (const volatile u8 *)0x00010000U;
    char line[96];
    u32 offset;

    for (offset = 0U; offset < 0x800U; offset += 16U) {
        u32 index;
        u32 interesting = 0U;
        int length;

        for (index = 0U; index < 16U; index++) {
            u8 value = base[offset + index];
            if (value != 0x00U && value != 0xffU) {
                interesting = 1U;
                break;
            }
        }
        if (!interesting) continue;

        length = snprintf(line, sizeof(line), "%05x:", 0x10000U + offset);
        for (index = 0U; index < 16U && length > 0; index++) {
            length += snprintf(line + length, sizeof(line) - (size_t)length,
                               " %02x", base[offset + index]);
        }
        if (length > 0) {
            console_write_bytes(line, (u32)length);
            console_write("\r\n");
        }
    }
}

static int parse_decimal(const char **cursor, u32 *value)
{
    const char *text = *cursor;
    u32 result = 0U;
    u8 digits = 0U;

    while (*text >= '0' && *text <= '9') {
        u32 digit = (u32)(*text - '0');
        if (result > (0xffffffffU - digit) / 10U) return -1;
        result = result * 10U + digit;
        digits = 1U;
        text++;
    }
    if (!digits) return -1;
    *cursor = text;
    *value = result;
    return 0;
}

static int parse_injection_args(const char *text, u32 *period, u32 *duration)
{
    if (parse_decimal(&text, period) != 0 || *text++ != ' ' ||
        parse_decimal(&text, duration) != 0 || *text != '\0') return -1;
    return 0;
}

static int parse_rumble_args(const char *text, u32 *left, u32 *right,
                             u32 *duration)
{
    if (parse_decimal(&text, left) != 0 || *text++ != ' ' ||
        parse_decimal(&text, right) != 0 || *text++ != ' ' ||
        parse_decimal(&text, duration) != 0 || *text != '\0') return -1;
    return 0;
}

static int parse_hex_byte(const char **cursor, u32 *value)
{
    const char *text = *cursor;
    u32 result = 0U;
    unsigned digit;

    for (digit = 0U; digit < 2U; digit++) {
        char letter = text[digit];
        u32 nibble;

        if (letter >= '0' && letter <= '9') nibble = (u32)(letter - '0');
        else if (letter >= 'a' && letter <= 'f') nibble = (u32)(letter - 'a' + 10);
        else if (letter >= 'A' && letter <= 'F') nibble = (u32)(letter - 'A' + 10);
        else return -1;
        result = (result << 4) | nibble;
    }
    *cursor = text + 2;
    *value = result;
    return 0;
}

/* "aa:bb:cc:dd:ee:ff" as printed -> the least-significant-byte-first form the
 * stack stores. */
static int parse_address(const char *text, u8 address[6])
{
    unsigned index;

    for (index = 0U; index < 6U; index++) {
        u32 value = 0U;
        if (parse_hex_byte(&text, &value) != 0) return -1;
        address[5U - index] = (u8)value;
        if (index < 5U && *text++ != ':') return -1;
    }
    return *text == '\0' ? 0 : -1;
}

/* Raw RAM access, for locating where the controller library keeps state the
 * application is expected to seed.  Deliberately narrow: internal RAM only,
 * bounded length, and hex in / hex out. */
static int parse_hex_bytes(const char *text, u8 *output, unsigned capacity,
                           unsigned *length)
{
    unsigned count = 0U;

    while (*text != '\0') {
        u32 value = 0U;

        if (count >= capacity) return -1;
        if (parse_hex_byte(&text, &value) != 0) return -1;
        output[count++] = (u8)value;
    }
    *length = count;
    return count != 0U ? 0 : -1;
}

static int parse_ram_address(const char *text, u32 *address)
{
    u32 value = 0U;
    unsigned digits = 0U;

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    while (*text != '\0') {
        u32 digit;

        if (*text >= '0' && *text <= '9') digit = (u32)(*text - '0');
        else if (*text >= 'a' && *text <= 'f') digit = (u32)(*text - 'a' + 10);
        else if (*text >= 'A' && *text <= 'F') digit = (u32)(*text - 'A' + 10);
        else return -1;
        value = (value << 4) | digit;
        if (++digits > 6U) return -1;
        text++;
    }
    if (digits == 0U || value > 0x3ffffU) return -1;
    *address = value;
    return 0;
}

/* "aa:bb:cc:dd:ee:ff <32 hex> [type]" - one bond record as the `keys`
 * command prints it.  The text is edited in place, so it has to be the
 * writable command line.  Type defaults to 4 (unauthenticated combination
 * key, what a console pairing produces). */
static int parse_key_record(char *text, u8 address[6], u8 key[16], u8 *type)
{
    char *space = strchr(text, ' ');
    unsigned length = 0U;
    u32 value = 0U;

    *type = 4U;
    if (space == NULL) return -1;
    *space = '\0';
    if (parse_address(text, address) != 0) return -1;

    text = space + 1;
    space = strchr(text, ' ');
    if (space != NULL) {
        const char *cursor = space + 1;

        *space = '\0';
        if (parse_decimal(&cursor, &value) != 0 || *cursor != '\0' ||
            value > 255U) return -1;
        *type = (u8)value;
    }
    if (parse_hex_bytes(text, key, 16U, &length) != 0 || length != 16U)
        return -1;
    return 0;
}

__attribute__((noinline)) static void print_rumble_status(void)
{
    struct cyclone2_rumble_status status;
    char line[96];
    int length;

    cyclone2_rumble_get_status(&status);
    length = snprintf(line, sizeof(line),
                      "rumble=%s left=%u right=%u duration=%u ms%s updates=%u request_ms=%u apply_ms=%u latency=%u max_latency=%u\r\n",
                      status.active ? "active" : "stopped", status.left,
                      status.right, status.duration_ms,
                      status.active && status.duration_ms == 0U ? " (latched)" : "",
                      status.updates, status.last_request_ms,
                      status.last_apply_ms, status.last_apply_latency_ms,
                      status.max_apply_latency_ms);
    if (length > 0) console_write_bytes(line, (u32)length);
}

__attribute__((noinline)) static void print_imu_status(void)
{
    struct cyclone2_imu_status status;
    const char *state;
    char line[128];
    int length;

    cyclone2_imu_get_status(&status);
    state = status.initialized ? "running" :
            (status.enabled ? "starting" : "stopped");
    length = snprintf(line, sizeof(line),
                      "imu=%s id=0x%02x samples=%u errors=%u last_error=%u last_sample_ms=%u enable=%u disable=%u\r\n",
                      state, status.device_id, status.samples, status.errors,
                      status.last_error, status.last_sample_ms,
                      status.enable_requests, status.disable_requests);
    if (length > 0) console_write_bytes(line, (u32)length);
}

__attribute__((noinline)) static void print_imu_sample(void)
{
    struct cyclone2_imu_sample sample;
    char line[160];
    int length;

    if (cyclone2_imu_latest(&sample) != 0) {
        console_write("ERR no IMU sample available; use imu init\r\n");
        return;
    }
    length = snprintf(line, sizeof(line),
                      "imu sample_ms=%u accel=%d,%d,%d gyro=%d,%d,%d\r\n",
                      sample.timestamp_ms, sample.accel_x, sample.accel_y,
                      sample.accel_z, sample.gyro_x, sample.gyro_y,
                      sample.gyro_z);
    if (length > 0) console_write_bytes(line, (u32)length);
}

/* Charge-driver state and the VBAT reading.  adc_get_voltage(AD_CH_VBAT) is
 * the dedicated VBAT path (vbat / 4 in mV), so no sample channel has to be
 * registered.
 *
 * Neither number is evidence about the battery: this build never initialises
 * the charge driver (see board_init() in src/init.c), so the flags report an
 * uninitialised module, and the VBAT reading is out of range for the cell -
 * the SoC's sense does not appear to be connected to it.  See
 * docs/charging.md. */
static void print_battery_status(void)
{
    char line[96];
    int length;

    /* Deliberately only these two reads.  get_ldo5v_online_hw(),
     * adc_check_vbat_lowpower() and get_charge_full_value() block once the
     * charge module has not been initialised, which is the case in this build
     * (see board_init() in src/init.c) - reading them hangs the console
     * mid-command. */
    length = snprintf(line, sizeof(line),
                      "battery vbat=%u mV charge=%u poweron=%u\r\n",
                      (unsigned)(adc_get_voltage(AD_CH_VBAT) * 4U),
                      (unsigned)get_charge_online_flag(),
                      (unsigned)get_charge_poweron_en());
    if (length > 0) console_write_bytes(line, (u32)length);
}

/* Keep the command parser's formatting buffers out of the frequently-called
 * USB poll frame. LTO otherwise inlines this through consume_input(). */
__attribute__((noinline)) static void execute_command(void)
{
    command_line[command_length] = '\0';

    if (!strcmp(command_line, "help") || !strcmp(command_line, "?")) {
        console_write("help     show this command list\r\n"
                      "debug on|off|status  control runtime diagnostics\r\n"
                      "report 5|8|10|16|status  set/report HID period in ms\r\n"
                      "connect 0|1|2|3  reconnect a saved profile slot\r\n"
                      "pair 0|1|2|3  forget a slot's bond and wait to pair\r\n"
                      "pair N fresh  as above but with a new Bluetooth address\r\n"
                      "newaddr  give the pad a new random Bluetooth address\r\n"
                      "setaddr AA:BB:..  set the pad's address to a saved one\r\n"
                      "unkey AA:BB:..  drop a stored link key and re-pair\r\n"
                      "peer N AA:BB:..  make slot N page that peer\r\n"
                      "clearkeys  zero the controller's pairing records\r\n"
                      "bondstore  dump the controller's pairing records\r\n"
                      "keys       dump the host link-key store\r\n"
                      "setkey AA:BB:.. KEY32 [type]  store one link key\r\n"
                      "restore N AA:BB:.. KEY32 [type]  set slot N's peer and key\r\n"
                      "hcidump on|off  trace HCI packets to the console\r\n"
                      "nvscan     list non-blank controller memory\r\n"
                      "delall     forget every device the stack remembers\r\n"
                      "connect status  show slot, address, state, and USB\r\n"
                      "stat       one-line uptime/boot/state summary\r\n"
                      "disconnect  stop Bluetooth without sleeping\r\n"
                      "reboot     reset the chip and come back as this build\r\n"
                      "battery  show charge-driver state and VBAT (driver is\r\n"
                      "         not initialised in this build; see docs/charging.md)\r\n"
                      "inject a PERIOD_MS DURATION_MS | stop | status\r\n"
                      "input status|reset  show/reset stick-DMA counters\r\n"
                      "rumble LEFT RIGHT MS | stop | status\r\n"
                      "imu init|stop|sample|status\r\n"
                      "status   show reset mailbox and MaskROM NVRAM\r\n"
                      "uboot    reboot into BR23 UBOOT1.00\r\n");
    } else if (!strcmp(command_line, "debug on")) {
        debug_enabled = 1U;
        console_write("debug output enabled\r\n");
    } else if (!strcmp(command_line, "debug off")) {
        debug_enabled = 0U;
        debug_read_index = debug_write_index;
        console_write("debug output disabled\r\n");
    } else if (!strcmp(command_line, "debug status")) {
        console_write(debug_enabled ? "debug output enabled\r\n" :
                                      "debug output disabled\r\n");
    } else if (!strcmp(command_line, "status")) {
        print_status();
    } else if (!strcmp(command_line, "connect status")) {
        print_connection_status();
    } else if (!strcmp(command_line, "stat")) {
        print_compact_status();
    } else if (!strcmp(command_line, "connect 0") ||
               !strcmp(command_line, "connect 1") ||
               !strcmp(command_line, "connect 2") ||
               !strcmp(command_line, "connect 3")) {
        u8 slot = (u8)(command_line[8] - '0');
        int result = cyclone2_connect_request(slot);
        if (result > 0) {
            console_write("OK reconnecting slot ");
            console_write_bytes(&command_line[8], 1U);
            console_write("\r\n");
        } else if (result == 0) {
            console_write("OK already connected to slot ");
            console_write_bytes(&command_line[8], 1U);
            console_write("\r\n");
        } else {
            console_write("ERR profile transition already in progress\r\n");
        }
    } else if (!strcmp(command_line, "disconnect")) {
        int result = cyclone2_disconnect_request();
        if (result >= 0) {
            console_write(result ? "OK disconnecting\r\n" :
                                   "OK already disconnected\r\n");
        } else {
            console_write("ERR profile transition already in progress\r\n");
        }
    } else if (!strcmp(command_line, "reboot")) {
        if (cyclone2_reboot_request() > 0) {
            console_write("OK rebooting\r\n");
        } else {
            console_write("ERR profile transition already in progress\r\n");
        }
    } else if (!strncmp(command_line, "pair ", 5U) && command_line[5] >= '0' &&
               command_line[5] <= '3' &&
               (command_line[6] == '\0' ||
                !strcmp(command_line + 6, " fresh"))) {
        u8 slot = (u8)(command_line[5] - '0');
        int result = command_line[6] == '\0' ?
                     cyclone2_pair_request(slot) :
                     cyclone2_pair_fresh_request(slot);
        if (result > 0) {
            console_write("OK forgetting slot ");
            console_write_bytes(&command_line[5], 1U);
            console_write(command_line[6] == '\0' ?
                          " and becoming pairable\r\n" :
                          " with a new address and becoming pairable\r\n");
        } else {
            console_write("ERR profile transition already in progress\r\n");
        }
    } else if (!strcmp(command_line, "inject stop")) {
        cyclone2_inject_stop();
        console_write("OK injection stopped\r\n");
    } else if (!strncmp(command_line, "unkey ", 6U)) {
        u8 address[6];
        if (parse_address(command_line + 6U, address) != 0) {
            console_write("ERR use unkey aa:bb:cc:dd:ee:ff\r\n");
        } else if (cyclone2_forget_peer_request(address) > 0) {
            console_write("OK forgetting that link key\r\n");
        } else {
            /* A well-formed address that the policy layer refused: the radio
             * is mid-transition, not the command malformed. */
            console_write("ERR profile transition already in progress\r\n");
        }
    } else if (!strncmp(command_line, "peer ", 5U) && command_line[5] >= '0' &&
               command_line[5] <= '3' && command_line[6] == ' ') {
        u8 address[6];
        if (parse_address(command_line + 7U, address) != 0) {
            console_write("ERR use peer N aa:bb:cc:dd:ee:ff\r\n");
        } else if (cyclone2_set_peer_request((u8)(command_line[5] - '0'),
                                             address) > 0) {
            console_write("OK that slot now pages that peer\r\n");
        } else {
            console_write("ERR profile transition already in progress\r\n");
        }
    } else if (!strcmp(command_line, "newaddr")) {
        if (cyclone2_new_address_request() > 0) {
            struct cyclone2_connection_status status;
            char line[64];
            int length;

            cyclone2_connection_status_get(&status);
            length = snprintf(line, sizeof(line),
                              "OK new address %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                              status.local_address[5], status.local_address[4],
                              status.local_address[3], status.local_address[2],
                              status.local_address[1], status.local_address[0]);
            if (length > 0) console_write_bytes(line, (u32)length);
        } else {
            console_write("ERR profile transition already in progress\r\n");
        }
    } else if (!strcmp(command_line, "battery")) {
        print_battery_status();
    } else if (!strncmp(command_line, "setaddr ", 8U)) {
        u8 address[6];

        if (parse_address(command_line + 8U, address) != 0) {
            console_write("ERR use setaddr aa:bb:cc:dd:ee:ff\r\n");
        } else if (cyclone2_set_address_request(address) > 0) {
            console_write("OK address set\r\n");
        } else {
            console_write("ERR profile transition already in progress\r\n");
        }
    } else if (!strcmp(command_line, "clearkeys")) {
        if (cyclone2_clear_keys_request() > 0) {
            console_write("OK clearing stored link keys and restarting\r\n");
        } else {
            console_write("ERR profile transition already in progress\r\n");
        }
    } else if (!strcmp(command_line, "bondstore")) {
        print_bond_store();
    } else if (!strcmp(command_line, "keys")) {
        print_link_keys();
    } else if (!strcmp(command_line, "hcidump")) {
        console_write(cyclone2_hci_dump_packets_enabled() ?
                      "hcidump packet trace on\r\n" :
                      "hcidump packet trace off\r\n");
    } else if (!strcmp(command_line, "hcidump on")) {
        cyclone2_hci_dump_set_packets(1U);
        console_write("OK HCI packet trace on\r\n");
    } else if (!strcmp(command_line, "hcidump off")) {
        cyclone2_hci_dump_set_packets(0U);
        console_write("OK HCI packet trace off\r\n");
    } else if (!strncmp(command_line, "setkey ", 7U)) {
        u8 address[6];
        u8 key[16];
        u8 type;

        if (parse_key_record(command_line + 7U, address, key, &type) != 0) {
            console_write("ERR use setkey aa:bb:cc:dd:ee:ff KEY32 [type]\r\n");
        } else {
            btstack_jl_link_key_store_write(address, key, type);
            console_write("OK link key stored\r\n");
        }
    } else if (!strncmp(command_line, "restore ", 8U) && command_line[8] >= '0' &&
               command_line[8] <= '3' && command_line[9] == ' ') {
        u8 slot = (u8)(command_line[8] - '0');
        u8 address[6];
        u8 key[16];
        u8 type;

        if (parse_key_record(command_line + 10U, address, key, &type) != 0) {
            console_write("ERR use restore N aa:bb:cc:dd:ee:ff KEY32 [type]\r\n");
        } else {
            cyclone2_peer_address_write(slot, address);
            btstack_jl_link_key_store_write(address, key, type);
            cyclone2_connect_request(slot);
            console_write("OK bond restored; paging that peer\r\n");
        }
    } else if (!strncmp(command_line, "peek ", 5U)) {
        u32 address;
        u32 length = 16U;
        char *space;

        space = strchr(command_line + 5U, ' ');
        if (space != NULL) {
            u32 value = 0U;
            const char *p = space + 1U;
            if (parse_decimal(&p, &value) != 0 || value == 0U ||
                value > 64U) {
                console_write("ERR use peek ADDR [LEN<=64]\r\n");
                command_length = 0U;
                console_prompt();
                return;
            }
            length = value;
            *space = '\0';
        }
        if (parse_ram_address(command_line + 5U, &address) == 0) {
            console_write_hex((const u8 *)address, length);
            console_write("\r\n");
        } else {
            console_write("ERR use peek ADDR [LEN]\r\n");
        }
    } else if (!strncmp(command_line, "poke ", 5U)) {
        u32 address;
        u8 bytes[32];
        unsigned length;
        const char *hex;
        char *space;

        space = strchr(command_line + 5U, ' ');
        hex = space != NULL ? space + 1U : NULL;
        if (space != NULL) *space = '\0';
        if (hex != NULL && parse_ram_address(command_line + 5U, &address) == 0 &&
            parse_hex_bytes(hex, bytes, sizeof(bytes), &length) == 0) {
            memcpy((void *)address, bytes, length);
            __asm__ volatile("csync");
            console_write("OK wrote ");
            console_write_hex(bytes, length);
            console_write("\r\n");
        } else {
            console_write("ERR use poke ADDR HEXBYTES\r\n");
        }
    } else if (!strcmp(command_line, "nvscan")) {
        print_nvram_scan();
    } else if (!strcmp(command_line, "delall")) {
        cyclone2_switch_forget_all();
        console_write("OK asked the stack to forget every device\r\n");
    } else if (!strcmp(command_line, "inject status")) {
        print_injection_status();
    } else if (!strcmp(command_line, "input status")) {
        print_input_status();
    } else if (!strcmp(command_line, "input reset")) {
        cyclone2_input_reset_stats();
        console_write("OK input counters reset\r\n");
    } else if (!strncmp(command_line, "inject a ", 9U)) {
        u32 period;
        u32 duration;
        if (parse_injection_args(command_line + 9, &period, &duration) == 0 &&
            cyclone2_inject_start(period, duration) == 0) {
            console_write("OK injection started\r\n");
        } else {
            console_write("ERR inject syntax/range; use inject a PERIOD_MS DURATION_MS\r\n");
        }
    } else if (!strcmp(command_line, "rumble stop")) {
        cyclone2_rumble_stop();
        print_rumble_status();
    } else if (!strcmp(command_line, "rumble status")) {
        print_rumble_status();
    } else if (!strncmp(command_line, "rumble ", 7U)) {
        u32 left;
        u32 right;
        u32 duration;
        if (parse_rumble_args(command_line + 7U, &left, &right, &duration) == 0 &&
            left <= 255U && right <= 255U &&
            duration <= CYCLONE2_RUMBLE_MAX_DURATION_MS &&
            cyclone2_rumble_set((u8)left, (u8)right, (u16)duration) == 0) {
            print_rumble_status();
        } else {
            console_write("ERR rumble range: strength 0..255, duration 1..2000 ms\r\n");
        }
    } else if (!strcmp(command_line, "imu init")) {
        cyclone2_imu_enable();
        console_write("OK IMU initialization requested\r\n");
    } else if (!strcmp(command_line, "imu stop")) {
        cyclone2_imu_disable();
        console_write("OK IMU sampling stopped\r\n");
    } else if (!strcmp(command_line, "imu sample")) {
        print_imu_sample();
    } else if (!strcmp(command_line, "imu status")) {
        print_imu_status();
    } else if (!strcmp(command_line, "report status")) {
        print_report_status();
    } else if (!strcmp(command_line, "report 5")) {
        cyclone2_report_period_set(5U);
        print_report_status();
    } else if (!strcmp(command_line, "report 8")) {
        cyclone2_report_period_set(8U);
        print_report_status();
    } else if (!strcmp(command_line, "report 10")) {
        cyclone2_report_period_set(10U);
        print_report_status();
    } else if (!strcmp(command_line, "report 16")) {
        cyclone2_report_period_set(16U);
        print_report_status();
    } else if (!strcmp(command_line, "uboot")) {
        arm_maskrom_usb_token();
        console_write("OK entering UBOOT1.00 token=");
        console_write_hex((const u8 *)MASKROM_NVRAM_ADDRESS, 16U);
        console_write("\r\n");
        response_delay = RESPONSE_DELAY_TICKS;
        uboot_pending = 1U;
    } else if (command_length != 0U) {
        console_write("ERR unknown command; type help\r\n");
    }

    command_length = 0U;
    if (!uboot_pending) console_prompt();
}

static void consume_input(const u8 *data, u32 length)
{
    u32 index;

    for (index = 0; index < length; index++) {
        u8 byte = data[index];

        if (byte == '\r' || byte == '\n') {
            if (byte == '\n' && last_was_cr) {
                last_was_cr = 0U;
                continue;
            }
            last_was_cr = byte == '\r';
            console_write("\r\n");
            execute_command();
            if (uboot_pending) return;
        } else if (byte == 0x08U || byte == 0x7fU) {
            last_was_cr = 0U;
            if (command_length != 0U) {
                command_length--;
                console_write("\b \b");
            }
        } else if (byte == 0x05U) {
            /* ENQ: the host's liveness ping, answered with a single ACK.
             * Nothing is echoed and nothing is added to the command line, so
             * a host can ask several times a second without leaving a mark on
             * the console.  This runs from the application poll, which is the
             * point: an answer means the firmware is running, not merely that
             * the USB device is still enumerated. */
            static const u8 ack = 0x06U;

            last_was_cr = 0U;
            console_write_bytes(&ack, 1U);
        } else if (byte >= 0x20U && byte <= 0x7eU) {
            last_was_cr = 0U;
            if (command_length + 1U < sizeof(command_line)) {
                command_line[command_length++] = (char)byte;
                console_write_bytes(&byte, 1U);
            }
        }
    }
}

static void cdc_receive_wakeup(struct usb_device_t *usb_device)
{
    (void)usb_device;
    receive_pending = 1U;
}

__attribute__((noreturn))
static void enter_maskrom_uboot(void)
{
    cyclone2_led_set_all(0U, 16U, 0U);
    rom_mdelay(100U);

    local_irq_disable();
    USB_CON0 = 0U;
    USB_CON1 = 0U;
    USB_IO_CON0 = USB_FORCE_SE0;
    rom_mdelay(1000U);
    USB_IO_CON0 = 0U;
    rom_mdelay(250U);

    while (!gpio_read(CYCLONE2_USB_SENSE)) rom_mdelay(100U);

    arm_maskrom_usb_token();
    wdt_init(WDT_256MS);
    local_irq_disable();
    for (;;) {
    }
}

void cyclone2_usb_control_initialize(void)
{
    usb_start();
    cdc_set_wakeup_handler(cdc_receive_wakeup);
    greeting_pending = 1U;
}

void cyclone2_usb_control_poll(void)
{
    u8 receive_buffer[64];
    u32 length;
    u8 terminal_open = cyclone2_cdc_terminal_open();

    if (!terminal_open) {
        /* A closed terminal must never let diagnostics obstruct controller
         * work. Keep only messages produced after the next open. */
        debug_read_index = debug_write_index;
        terminal_was_open = 0U;
        if (uboot_pending && --response_delay == 0U) enter_maskrom_uboot();
        return;
    }

    if (!terminal_was_open) {
        terminal_was_open = 1U;
        greeting_pending = 1U;
    }

    if (greeting_pending) {
        static const char greeting[] =
            "\r\nCyclone 2 configuration console\r\nType help for commands.\r\nc2> ";
        if (cdc_write_data(USB0, (u8 *)greeting, sizeof(greeting) - 1U) != 0U)
            greeting_pending = 0U;
    }

    if (receive_pending && !uboot_pending) {
        receive_pending = 0U;
        do {
            length = cdc_read_data(USB0, receive_buffer,
                                   sizeof(receive_buffer));
            if (length != 0U) consume_input(receive_buffer, length);
        } while (length != 0U && !uboot_pending);
    }

    if (!uboot_pending) flush_debug_line();

    if (uboot_pending && --response_delay == 0U) enter_maskrom_uboot();
}
