/* Recovered common controller/baseband and local-information dispatch.
 * Shared local-device storage remains SDK-owned behind typed accessors.
 * Controller task context, reset ordering and event formats follow SDK IR. */
#include <string.h>
#include "btctrler_sdk_abi.h"

#ifndef UNITTEST
extern const int config_btctler_modules;
extern const int config_btctler_mode;
#else
static int config_btctler_modules = 3, config_btctler_mode = 1;
#endif

int hci_controller_baseband_cmd_handler(unsigned short opcode, unsigned char *data, int len)
{
    int required;
    switch (opcode) {
    case 0x0c01: case 0x0c63: required = 8; break; /* Event Mask / Page 2 */
    case 0x0c03: required = 0; break; /* Reset */
    default: return 0;
    }
    if (len < required || (required && !data)) {
        BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1", 0x12);
        return 1;
    }
    switch (opcode) {
    case 0x0c01:
        memcpy(BTCTRLER_ABI_event_mask(), data, 8);
        break;
    case 0x0c63:
        /* The SDK acknowledges Page 2 without storing or applying it. */
        break;
    case 0x0c03:
        if (config_btctler_modules & 1) BTCTRLER_BIN_lmp_reset();
        if (config_btctler_modules & 2) {
            unsigned char *features = BTCTRLER_ABI_local_features();
            features[4] |= 0x40;
            features[6] |= 0x02;
            if (config_btctler_mode & 1) BTCTRLER_BIN_ll_reset();
            else if (config_btctler_mode & 4) BTCTRLER_BIN_le_dut_hci_reset_handle();
        }
        break;
    }
    BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1", 0);
    return 1;
}

int hci_informational_parameters_cmd_handler(unsigned short opcode, unsigned char *data, int len)
{
    int required;
    switch (opcode) {
    case 0x1001: case 0x1002: case 0x1003: case 0x1009: required = 0; break;
    case 0x1004: required = 1; break;
    default: return 0;
    }
    if (len < required || (required && !data)) {
        BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1", 0x12);
        return 1;
    }
    switch (opcode) {
    case 0x1001: /* Read Local Version Information */
        BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "112122", 0,
            (int)BTCTRLER_ABI_hci_version(), (int)BTCTRLER_ABI_hci_revision(),
            (int)BTCTRLER_ABI_lmp_version(), (int)BTCTRLER_ABI_lmp_subversion(),
            (int)BTCTRLER_ABI_manufacturer());
        /* Preserve SDK ordering: subversion precedes manufacturer here. */
        break;
    case 0x1002: /* Read Local Supported Commands */
        BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1c64", 0, BTCTRLER_ABI_local_commands());
        break;
    case 0x1003: /* Read Local Supported Features */
        BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1c08", 0, BTCTRLER_ABI_local_features());
        break;
    case 0x1004: { /* Read Local Extended Features */
        unsigned int page = data[0];
        static const unsigned char empty[8];
        /* SDK page zero points back to its standard-feature field. Name
         * that storage explicitly and reject pages beyond the two-page
         * extension array instead of reading adjacent device state. */
        unsigned char *features = page == 0 ? BTCTRLER_ABI_local_features() :
            page <= 2 ? BTCTRLER_ABI_local_extended_features() + (page - 1) * 8 : 0;
        BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "111c08", features ? 0 : 0x12,
            (int)page, 2, features ? features : empty);
        break;
    }
    case 0x1009: /* Read BD_ADDR: Classic takes precedence in a dual build. */
        if (config_btctler_modules & 1)
            BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1A", 0, BTCTRLER_ABI_classic_address());
        else if (config_btctler_modules & 2)
            BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1A", 0, BTCTRLER_ABI_le_address());
        break;
    }
    return 1;
}

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
static unsigned char mask[8], features[8], extended[16], commands[64], classic[6], le[6];
static int sequence, classic_at, le_at, dut_at, event_at, error, seen_opcode;
static int values[5], page_seen, max_seen;
static unsigned char payload[64];
static const char *format_seen;
unsigned char *BTCTRLER_ABI_event_mask(void) { return mask; }
unsigned char *BTCTRLER_ABI_local_features(void) { return features; }
unsigned char *BTCTRLER_ABI_local_extended_features(void) { return extended; }
unsigned char *BTCTRLER_ABI_local_commands(void) { return commands; }
unsigned char *BTCTRLER_ABI_classic_address(void) { return classic; }
unsigned char *BTCTRLER_ABI_le_address(void) { return le; }
unsigned char BTCTRLER_ABI_hci_version(void) { return 13; }
unsigned short BTCTRLER_ABI_hci_revision(void) { return 12576; }
unsigned char BTCTRLER_ABI_lmp_version(void) { return 13; }
unsigned short BTCTRLER_ABI_manufacturer(void) { return 8891; }
unsigned short BTCTRLER_ABI_lmp_subversion(void) { return 1494; }
void BTCTRLER_BIN_lmp_reset(void) { classic_at = ++sequence; }
void BTCTRLER_BIN_ll_reset(void)
{ le_at = ++sequence; CHECK(features[4] & 0x40); CHECK(features[6] & 2); }
void BTCTRLER_BIN_le_dut_hci_reset_handle(void) { dut_at = ++sequence; }
int BTCTRLER_BIN_hci_send_event_cmd_complete(int opcode, const char *format, ...)
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
    for (int n = -1; n < 8; n++) {
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
