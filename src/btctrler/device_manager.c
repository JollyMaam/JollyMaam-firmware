/* Owned common controller/baseband and local-information commands
 * (device_manager.c): event masks, reset, version, features, address. */
#include <string.h>
#include "btctrler/glue.h"

int hci_controller_baseband_cmd_handler(u16 opcode, const u8 *data, u32 len)
{
    u32 required;
    switch (opcode) {
    case 0x0c01: case 0x0c63: required = 8; break; /* Event Mask / Page 2 */
    case 0x0c03: required = 0; break; /* Reset */
    default: return 0;
    }
    if (len < required || (required && !data)) {
        hci_send_event_cmd_complete(opcode, "1", 0x12);
        return 1;
    }
    switch (opcode) {
    case 0x0c01:
        memcpy(hci_param.event_mask, data, 8);
        break;
    case 0x0c63:
        /* The SDK acknowledges Page 2 without storing or applying it. */
        break;
    case 0x0c03:
        if (config_btctler_modules & 1) lmp_reset();
        if (config_btctler_modules & 2) {
            hci_param.features[4] |= 0x40;  /* LE supported (controller) */
            hci_param.features[6] |= 0x02;  /* simultaneous LE and BR/EDR */
            if (config_btctler_mode & 1) ll_reset();
            else if (config_btctler_mode & 4) le_dut_hci_reset_handle();
        }
        break;
    }
    hci_send_event_cmd_complete(opcode, "1", 0);
    return 1;
}

int hci_informational_parameters_cmd_handler(u16 opcode, const u8 *data, u32 len)
{
    u32 required;
    switch (opcode) {
    case 0x1001: case 0x1002: case 0x1003: case 0x1009: required = 0; break;
    case 0x1004: required = 1; break;
    default: return 0;
    }
    if (len < required || (required && !data)) {
        hci_send_event_cmd_complete(opcode, "1", 0x12);
        return 1;
    }
    switch (opcode) {
    case 0x1001: /* Read Local Version Information */
        hci_send_event_cmd_complete(opcode, "112122", 0, hci_param.hci_version,
            hci_param.hci_revision, hci_param.lmp_pal_version,
            hci_param.manufacturer_name, hci_param.lmp_pal_subversion);
        break;
    case 0x1002: /* Read Local Supported Commands */
        hci_send_event_cmd_complete(opcode, "1c64", 0, hci_param.local_supported_commands);
        break;
    case 0x1003: /* Read Local Supported Features */
        hci_send_event_cmd_complete(opcode, "1c08", 0, hci_param.features);
        break;
    case 0x1004: { /* Read Local Extended Features */
        /* Page 0 is the standard features, 1-2 ext_features.  The SDK
         * indexed past ext_features for larger pages; refuse those. */
        static const u8 empty[8];
        u32 page = data[0];
        const u8 *features = page == 0 ? hci_param.features :
            page <= 2 ? hci_param.ext_features + (page - 1) * 8 : 0;
        hci_send_event_cmd_complete(opcode, "111c08", features ? 0 : 0x12,
            page, 2, features ? features : empty);
        break;
    }
    case 0x1009: /* Read BD_ADDR: Classic's in a dual-mode build. */
        if (config_btctler_modules & 1)
            hci_send_event_cmd_complete(opcode, "1A", 0, hci_param.addr);
        else if (config_btctler_modules & 2)
            hci_send_event_cmd_complete(opcode, "1A", 0, le_param.public_address);
        break;
    }
    return 1;
}

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
struct hci_parameter hci_param = { .hci_version = 13, .hci_revision = 12576, .lmp_pal_version = 13,
    .manufacturer_name = 1494 /* Jieli, 0x05d6 */, .lmp_pal_subversion = 8891 };
struct le_parameter le_param;
int config_btctler_modules = 3, config_btctler_mode = 1, CONFIG_BREDR_INQUIRY, CONFIG_LMP_MASTER_ESCO_ENABLE;
static u8 *const mask = hci_param.event_mask, *const features = hci_param.features,
    *const extended = hci_param.ext_features, *const commands = hci_param.local_supported_commands,
    *const classic = hci_param.addr, *const le = le_param.public_address;
static int sequence, classic_at, le_at, dut_at, event_at, error, seen_opcode;
static int values[5], page_seen, max_seen;
static unsigned char payload[64];
static const char *format_seen;
void lmp_reset(void) { classic_at = ++sequence; }
void ll_reset(void)
{ le_at = ++sequence; CHECK(features[4] & 0x40); CHECK(features[6] & 2); }
void le_dut_hci_reset_handle(void) { dut_at = ++sequence; }
int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format); event_at = ++sequence; seen_opcode = opcode;
    format_seen = format; error = va_arg(ap, int);
    if (!strcmp(format, "112122")) for (int i = 0; i < 5; i++) values[i] = va_arg(ap, int);
    else if (!strcmp(format, "1A")) memcpy(payload, va_arg(ap, unsigned char *), 6);
    else if (!strcmp(format, "1c08")) memcpy(payload, va_arg(ap, unsigned char *), 8);
    else if (!strcmp(format, "1c64")) memcpy(payload, va_arg(ap, unsigned char *), 64);
    else if (!strcmp(format, "111c08")) {
        page_seen = va_arg(ap, int); max_seen = va_arg(ap, int);
        memcpy(payload, va_arg(ap, const unsigned char *), 8);
    } else CHECK(!strcmp(format, "1"));
    va_end(ap); return 0;
}
static void clear(void)
{ sequence = classic_at = le_at = dut_at = event_at = error = 0; }
int main(void)
{
    unsigned char data[8] = {1,2,3,4,5,6,7,8};
    for (int n = 0; n < 8; n++) {
        clear(); CHECK(hci_controller_baseband_cmd_handler(0x0c01, 0, n) == 1);
        CHECK(error == 0x12 && event_at && !classic_at && !le_at);
        clear(); CHECK(hci_controller_baseband_cmd_handler(0x0c63, 0, n) == 1);
        CHECK(error == 0x12 && event_at);
    }
    clear(); CHECK(hci_controller_baseband_cmd_handler(0x0c01, data, 8) == 1);
    CHECK(!memcmp(mask, data, 8) && !error);
    memset(data, 0xa5, 8); clear(); hci_controller_baseband_cmd_handler(0x0c63, data, 8);
    CHECK(mask[0] == 1 && mask[7] == 8 && !error); /* Page 2 remains ignored. */
    memset(features, 0xa5, 8); clear(); hci_controller_baseband_cmd_handler(0x0c03, 0, 0);
    CHECK(classic_at == 1 && le_at == 2 && event_at == 3 && !dut_at);
    CHECK(features[4] == 0xe5 && features[6] == 0xa7 && features[5] == 0xa5);
    config_btctler_mode = 5; clear(); hci_controller_baseband_cmd_handler(0x0c03, 0, 0);
    CHECK(le_at && !dut_at); /* Normal mode wins over DUT. */
    config_btctler_mode = 4; clear(); hci_controller_baseband_cmd_handler(0x0c03, 0, 0);
    CHECK(dut_at == 2 && event_at == 3 && !le_at);
    config_btctler_modules = 1; clear(); hci_controller_baseband_cmd_handler(0x0c03, 0, 0);
    CHECK(classic_at == 1 && event_at == 2 && !dut_at && !le_at);
    config_btctler_modules = 2; config_btctler_mode = 0;
    clear(); hci_controller_baseband_cmd_handler(0x0c03, 0, 0);
    CHECK(!classic_at && !le_at && !dut_at && event_at == 1);
    clear(); CHECK(!hci_controller_baseband_cmd_handler(0x0c02, 0, 0) && !event_at);
    config_btctler_modules = 3;
    hci_informational_parameters_cmd_handler(0x1001, 0, 0);
    CHECK(!strcmp(format_seen, "112122") && values[0] == 13 && values[1] == 12576);
    CHECK(values[2] == 13 && values[3] == 1494 && values[4] == 8891);
    for (int i = 0; i < 64; i++) commands[i] = (unsigned char)i;
    hci_informational_parameters_cmd_handler(0x1002, 0, 0); CHECK(!memcmp(payload, commands, 64));
    hci_informational_parameters_cmd_handler(0x1003, 0, 0); CHECK(!memcmp(payload, features, 8));
    for (int i = 0; i < 16; i++) extended[i] = (unsigned char)(i + 10);
    for (int i = 0; i < 256; i++) {
        unsigned char page = (unsigned char)i; clear();
        CHECK(hci_informational_parameters_cmd_handler(0x1004, &page, 1) == 1);
        CHECK(page_seen == i && max_seen == 2 && seen_opcode == 0x1004);
        CHECK(error == (i > 2 ? 0x12 : 0));
        const unsigned char zero[8] = {0};
        CHECK(!memcmp(payload, i == 0 ? features : i <= 2 ? extended + (i - 1) * 8 : zero, 8));
    }
    clear(); hci_informational_parameters_cmd_handler(0x1004, 0, 0); CHECK(error == 0x12);
    classic[0] = 1; le[0] = 2;
    hci_informational_parameters_cmd_handler(0x1009, 0, 0); CHECK(payload[0] == 1);
    config_btctler_modules = 2; hci_informational_parameters_cmd_handler(0x1009, 0, 0); CHECK(payload[0] == 2);
    config_btctler_modules = 0; clear(); hci_informational_parameters_cmd_handler(0x1009, 0, 0); CHECK(!event_at);
    CHECK(!hci_informational_parameters_cmd_handler(0x1005, 0, 0));
    return TEST_DONE("btctrler_device_manager");
}
#endif
