/* Owned outer HCI command framing and group dispatch, recovered from BR23
 * SDK IR. Called synchronously in the original direct/queued task contexts.
 * A single packet excludes the H4 type byte and includes its HCI header.
 * Queue storage, connection procedures and event transport remain SDK code. */
#include "btctrler_sdk_abi.h"

extern const int config_btctler_modules;

void hci_command_process(unsigned char *packet, int size)
{
    /* With fewer than two bytes there is no complete opcode to acknowledge.
     * Do not read it or call any SDK group handler. */
    if (!packet || size < 2) return;
    unsigned short opcode = (unsigned short)(packet[0] | ((unsigned short)packet[1] << 8));
    if (size < 3 || size != 3 + (int)packet[2]) {
        BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1", 0x12);
        return;
    }
    unsigned char *parameters = packet + 3;
    int length = packet[2], result = 0;
    _Bool bad_group = 0;
    /* Preserve common-handler-first routing and call the Classic handler
     * only for an exactly zero result. Negative results are not re-routed. */
    switch (opcode >> 10) {
    case 1:
        result = BTCTRLER_BIN_hci_link_control_cmd_handler(opcode, parameters, length);
        if (!result) result = hci_lmp_link_control_cmd_handler(opcode, parameters, length);
        break;
    case 2:
        result = BTCTRLER_BIN_hci_link_policy_cmd_handler(opcode, parameters, length);
        if (!result) result = hci_lmp_link_policy_cmd_handler(opcode, parameters, length);
        break;
    case 3:
        result = hci_controller_baseband_cmd_handler(opcode, parameters, length);
        if (!result) result = hci_lmp_controller_baseband_cmd_handler(opcode, parameters, length);
        break;
    case 4:
        result = hci_informational_parameters_cmd_handler(opcode, parameters, length);
        if (!result) result = BTCTRLER_BIN_hci_lmp_informational_parameters_cmd_handler(opcode, parameters, length);
        break;
    case 5:
        result = BTCTRLER_BIN_hci_status_parameters_cmd_handler(opcode, parameters, length);
        if (!result) result = BTCTRLER_BIN_hci_lmp_status_parameters_cmd_handler(opcode, parameters, length);
        break;
    case 6:
        result = BTCTRLER_BIN_hci_testing_cmd_handler(opcode, parameters, length);
        if (!result) result = BTCTRLER_BIN_hci_lmp_testing_cmd_handler(opcode, parameters, length);
        break;
    case 8:
        if (config_btctler_modules & 2)
            result = BTCTRLER_BIN_hci_le_controller_cmd_handler(opcode, parameters, length);
        break;
    case 63:
        result = BTCTRLER_BIN_hci_vendor_cmd_handler(opcode, parameters, length);
        if (!result) result = BTCTRLER_BIN_hci_lmp_vendor_cmd_handler(opcode, parameters, length);
        break;
    case 62:
        /* The SDK does not gate its vendor LE group on module enable. */
        result = BTCTRLER_BIN_ogf_vendor_le_controller(opcode, parameters, length);
        break;
    default: bad_group = 1; break;
    }
    /* SDK asserts/resets on unknown groups and results >1, and completes
     * an unhandled known-group command with Unknown HCI Command. Preserve
     * that contract in the extracted binary fragment, including its ABI. */
    BTCTRLER_BIN_finish_command(opcode, result, bad_group, packet, size);
}

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
const int config_btctler_modules = 3;
static int returns[17], seen[2], calls, finished, error, event_opcode;
static int result_seen, bad_seen, size_seen, length_seen;
static unsigned char *packet_seen, *parameters_seen;
static int group(int id, unsigned short opcode, unsigned char *parameters, int length)
{
    CHECK(calls < 2); if (calls < 2) seen[calls++] = id;
    event_opcode = opcode; parameters_seen = parameters; length_seen = length;
    return returns[id];
}
#define HANDLER(name, id) int name(unsigned short opcode, unsigned char *parameters, int length) \
    { return group(id, opcode, parameters, length); }
HANDLER(BTCTRLER_BIN_hci_link_control_cmd_handler, 1)
HANDLER(hci_lmp_link_control_cmd_handler, 2)
HANDLER(BTCTRLER_BIN_hci_link_policy_cmd_handler, 3)
HANDLER(hci_lmp_link_policy_cmd_handler, 4)
HANDLER(hci_controller_baseband_cmd_handler, 5)
HANDLER(hci_lmp_controller_baseband_cmd_handler, 6)
HANDLER(hci_informational_parameters_cmd_handler, 7)
HANDLER(BTCTRLER_BIN_hci_lmp_informational_parameters_cmd_handler, 8)
HANDLER(BTCTRLER_BIN_hci_status_parameters_cmd_handler, 9)
HANDLER(BTCTRLER_BIN_hci_lmp_status_parameters_cmd_handler, 10)
HANDLER(BTCTRLER_BIN_hci_testing_cmd_handler, 11)
HANDLER(BTCTRLER_BIN_hci_lmp_testing_cmd_handler, 12)
HANDLER(BTCTRLER_BIN_hci_vendor_cmd_handler, 13)
HANDLER(BTCTRLER_BIN_hci_lmp_vendor_cmd_handler, 14)
HANDLER(BTCTRLER_BIN_hci_le_controller_cmd_handler, 15)
HANDLER(BTCTRLER_BIN_ogf_vendor_le_controller, 16)
void BTCTRLER_BIN_finish_command(int opcode, int result, _Bool bad_group,
                                unsigned char *packet, int size)
{
    finished++; event_opcode = opcode; result_seen = result; bad_seen = bad_group;
    packet_seen = packet; size_seen = size;
}
int BTCTRLER_BIN_hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format); CHECK(!strcmp(format, "1"));
    error = va_arg(ap, int); event_opcode = opcode; va_end(ap); return 0;
}
static void clear(void)
{
    calls = finished = error = 0; memset(seen, 0, sizeof(seen));
    memset(returns, 0, sizeof(returns));
}
static void run(unsigned char *packet, int size)
{ clear(); hci_command_process(packet, size); }
int main(void)
{
    /* Exact-size arrays make premature header reads detectable under ASan. */
    unsigned char one[] = {0x05}, two[] = {0x05,0x04}, header[] = {0x05,0x04,13};
    run(0, 258); CHECK(!calls && !finished && !error);
    run(one, -1); CHECK(!calls && !finished && !error);
    run(one, 0); CHECK(!calls && !finished && !error);
    run(one, 1); CHECK(!calls && !finished && !error);
    run(two, 2); CHECK(!calls && !finished && error == 0x12 && event_opcode == 0x0405);
    run(header, 3); CHECK(!calls && !finished && error == 0x12);
    unsigned char packet[258] = {0x05,0x04,255};
    run(packet, 258); CHECK(calls == 2 && finished == 1 && !error && length_seen == 255);
    run(packet, 257); CHECK(!calls && !finished && error == 0x12);
    packet[2] = 0;
    run(packet, 4); CHECK(!calls && !finished && error == 0x12); /* Trailing bytes. */
    const int groups[] = {1,2,3,4,5,6,63};
    const int values[] = {0,1,2,-1};
    for (unsigned int i = 0; i < sizeof(groups)/sizeof(groups[0]); i++) {
        unsigned short opcode = (unsigned short)((groups[i] << 10) | 0x03ff);
        packet[0] = opcode; packet[1] = opcode >> 8; packet[2] = 2;
        for (unsigned int j = 0; j < sizeof(values)/sizeof(values[0]); j++) {
            clear(); returns[2*i+1] = values[j]; returns[2*i+2] = 1;
            hci_command_process(packet, 5);
            CHECK(seen[0] == (int)(2*i+1));
            CHECK(calls == (values[j] ? 1 : 2));
            if (!values[j]) CHECK(seen[1] == (int)(2*i+2));
            CHECK(finished == 1 && !bad_seen && !error);
            CHECK(result_seen == (values[j] ? values[j] : 1));
            CHECK(event_opcode == opcode && packet_seen == packet && size_seen == 5);
            CHECK(parameters_seen == packet + 3 && length_seen == 2);
        }
        run(packet, 5); CHECK(result_seen == 0 && !bad_seen); /* Unhandled command. */
    }
    packet[0] = 0x01; packet[1] = 0x20; packet[2] = 0;
    run(packet, 3); CHECK(calls == 1 && seen[0] == 15 && finished == 1 && !bad_seen);
    packet[1] = 0xf8;
    run(packet, 3); CHECK(calls == 1 && seen[0] == 16 && finished == 1 && !bad_seen);
    for (int ogf = 0; ogf < 64; ogf++) {
        if ((ogf >= 1 && ogf <= 6) || ogf == 8 || ogf >= 62) continue;
        packet[0] = 1; packet[1] = (unsigned char)(ogf << 2);
        run(packet, 3); CHECK(!calls && finished == 1 && bad_seen && result_seen == 0);
    }
    return TEST_DONE("btctrler_hci_dispatch");
}
#endif
