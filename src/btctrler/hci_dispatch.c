/* Owned hci_controller.c host packet entry, command framing and group
 * dispatch.  hci_command_process is called from h4_hci_push_data (direct)
 * and the HCI thread (queued).  Packets have their HCI header and no H4
 * type byte. */
#include <stdio.h>
#include "btctrler/glue.h"

void hci_command_process(u8 *packet, int size)
{
    /* With fewer than two bytes there is no complete opcode to acknowledge.
     * Do not read it or call any SDK group handler. */
    if (!packet || size < 2) return;
    u16 opcode = (u16)(packet[0] | (packet[1] << 8));
    if (size < 3 || size != 3 + (int)packet[2]) {
        hci_send_event_cmd_complete(opcode, "1", 0x12);
        return;
    }
    u8 *parameters = packet + 3;
    int length = packet[2], result = 0;
    _Bool bad_group = 0;
    /* Preserve common-handler-first routing and call the Classic handler
     * only for an exactly zero result. Negative results are not re-routed. */
    switch (opcode >> 10) {
    case 1:
        result = hci_link_control_cmd_handler(opcode, parameters, length);
        if (!result) result = hci_lmp_link_control_cmd_handler(opcode, parameters, length);
        break;
    case 2:
        result = hci_link_policy_cmd_handler(opcode, parameters, length);
        if (!result) result = hci_lmp_link_policy_cmd_handler(opcode, parameters, length);
        break;
    case 3:
        result = hci_controller_baseband_cmd_handler(opcode, parameters, length);
        if (!result) result = hci_lmp_controller_baseband_cmd_handler(opcode, parameters, length);
        break;
    case 4:
        result = hci_informational_parameters_cmd_handler(opcode, parameters, length);
        if (!result) result = hci_lmp_informational_parameters_cmd_handler(opcode, parameters, length);
        break;
    case 5:
        result = hci_status_parameters_cmd_handler(opcode, parameters, length);
        if (!result) result = hci_lmp_status_parameters_cmd_handler(opcode, parameters, length);
        break;
    case 6:
        result = hci_testing_cmd_handler(opcode, parameters, length);
        if (!result) result = hci_lmp_testing_cmd_handler(opcode, parameters, length);
        break;
    case 8:
        if (config_btctler_modules & 2)
            result = hci_le_controller_cmd_handler(opcode, parameters, length);
        break;
    case 63:
        result = hci_vendor_cmd_handler(opcode, parameters, length);
        if (!result) result = hci_lmp_vendor_cmd_handler(opcode, parameters, length);
        break;
    case 62:
        /* The SDK does not gate its vendor LE group on module enable. */
        result = ogf_vendor_le_controller(opcode, parameters, length);
        break;
    default: bad_group = 1; break;
    }
    if (bad_group) btctrler_assert_failed("Controller unknown commands", opcode);
    else if (result > 1) { btctrler_assert_failed("Match more than once", opcode); return; }
    if (!result) hci_send_event_cmd_complete(opcode, "1", 0x01);    /* Unknown HCI Command */
}

/* Host to controller (the transport's send).  ACL data goes to whichever
 * link owns the handle; the SDK checks LE first and then Classic. */
void h4_hci_push_data(int type, const u8 *packet, int size)
{
    switch (type) {
    case 1: hci_command_process((u8 *)packet, size); break;
    case 2: {
        /* Fix: the SDK read the handle from, and copied, packets shorter
         * than the ACL header. */
        if (size < 4) break;
        int handle = (packet[0] | packet[1] << 8) & 0x0fff;
        if ((config_btctler_modules & 2) && (config_btctler_le_roles & 3) && ll_link_for_handle(handle))
            ll_push_acl_data(packet, size);
        if ((config_btctler_modules & 1) && lmp_conn_for_handle(handle))
            lmp_hci_send_packet_standard(packet, size);
        break;
    }
    case 5: hci_push_iso_sdu((u8 *)packet, (u16)size); break;
    }
}

#ifndef UNITTEST
void btctrler_assert_failed(const char *what, int value)
{
    if (config_asser) {
        printf("ASSERT-FAILD: %s 0x%x\n", what, value);
        cpu_assert_debug();
    } else {
        p33_soft_reset();
    }
}
#endif

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
int config_btctler_modules = 3;
int config_btctler_mode, CONFIG_BREDR_INQUIRY, CONFIG_LMP_MASTER_ESCO_ENABLE, config_asser;
int config_btctler_le_roles = 6;
static int le_handle = -1, classic_handle = -1, le_pushed, classic_sent, iso_pushed;
static struct le_link *le_link_stub = (struct le_link *)1;
static struct lmp_connection classic_conn;
struct le_link *ll_link_for_handle(int handle) { return handle == le_handle ? le_link_stub : 0; }
struct lmp_connection *lmp_conn_for_handle(int handle) { return handle == classic_handle ? &classic_conn : 0; }
int ll_push_acl_data(const u8 *packet, int len) { (void)packet; le_pushed = len; return 0; }
int lmp_hci_send_packet_standard(const u8 *packet, int len) { (void)packet; classic_sent = len; return 0; }
void hci_push_iso_sdu(uint8_t *packet, uint16_t size) { (void)packet; iso_pushed = size; }
static int returns[17], seen[2], calls, completions, error, asserted, event_opcode, length_seen;
static u8 *parameters_seen;
static int group(int id, u16 opcode, u8 *parameters, int length)
{
    CHECK(calls < 2); if (calls < 2) seen[calls++] = id;
    event_opcode = opcode; parameters_seen = parameters; length_seen = length;
    return returns[id];
}
#define HANDLER(name, id, q, n) int name(u16 opcode, q u8 *parameters, n length) \
    { return group(id, opcode, (u8 *)parameters, (int)length); }
HANDLER(hci_link_control_cmd_handler, 1, const, u32)
HANDLER(hci_lmp_link_control_cmd_handler, 2, , u32)
HANDLER(hci_link_policy_cmd_handler, 3, const, u32)
HANDLER(hci_lmp_link_policy_cmd_handler, 4, , u32)
HANDLER(hci_controller_baseband_cmd_handler, 5, const, u32)
HANDLER(hci_lmp_controller_baseband_cmd_handler, 6, const, u32)
HANDLER(hci_informational_parameters_cmd_handler, 7, const, u32)
HANDLER(hci_lmp_informational_parameters_cmd_handler, 8, const, u32)
HANDLER(hci_status_parameters_cmd_handler, 9, const, u32)
HANDLER(hci_lmp_status_parameters_cmd_handler, 10, const, u32)
HANDLER(hci_testing_cmd_handler, 11, const, u32)
HANDLER(hci_lmp_testing_cmd_handler, 12, const, u32)
HANDLER(hci_vendor_cmd_handler, 13, const, u32)
HANDLER(hci_lmp_vendor_cmd_handler, 14, , u32)
HANDLER(hci_le_controller_cmd_handler, 15, const, u32)
HANDLER(ogf_vendor_le_controller, 16, const, int)
void btctrler_assert_failed(const char *what, int value) { (void)what; asserted++; event_opcode = value; }
int hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format); CHECK(!strcmp(format, "1"));
    error = va_arg(ap, int); event_opcode = opcode; completions++; va_end(ap); return 0;
}
static void run(u8 *packet, int size)
{
    calls = completions = error = asserted = 0; memset(seen, 0, sizeof(seen));
    hci_command_process(packet, size);
}
int main(void)
{
    /* Exact-size arrays make premature header reads detectable under ASan. */
    u8 one[] = {0x05}, two[] = {0x05,0x04}, header[] = {0x05,0x04,13};
    run(0, 258); CHECK(!calls && !completions);
    run(one, -1); CHECK(!calls && !completions);
    run(one, 0); CHECK(!calls && !completions);
    run(one, 1); CHECK(!calls && !completions);
    run(two, 2); CHECK(!calls && error == 0x12 && event_opcode == 0x0405);
    run(header, 3); CHECK(!calls && error == 0x12);
    u8 packet[258] = {0x05,0x04,255};
    returns[1] = 1;
    run(packet, 258); CHECK(calls == 1 && !completions && length_seen == 255);
    run(packet, 257); CHECK(!calls && error == 0x12);
    packet[2] = 0;
    run(packet, 4); CHECK(!calls && error == 0x12); /* Trailing bytes. */
    const int groups[] = {1,2,3,4,5,6,63};
    const int values[] = {0,1,2,-1};
    for (unsigned int i = 0; i < sizeof(groups)/sizeof(groups[0]); i++) {
        u16 opcode = (u16)((groups[i] << 10) | 0x03ff);
        packet[0] = (u8)opcode; packet[1] = opcode >> 8; packet[2] = 2;
        for (unsigned int j = 0; j < sizeof(values)/sizeof(values[0]); j++) {
            memset(returns, 0, sizeof(returns));
            returns[2*i+1] = values[j]; returns[2*i+2] = 1;
            run(packet, 5);
            CHECK(seen[0] == (int)(2*i+1));
            CHECK(calls == (values[j] ? 1 : 2));
            if (!values[j]) CHECK(seen[1] == (int)(2*i+2));
            CHECK(parameters_seen == packet + 3 && length_seen == 2);
            /* More than one match is the SDK's assertion; otherwise handled. */
            CHECK(asserted == (values[j] > 1) && !completions);
        }
        memset(returns, 0, sizeof(returns));
        run(packet, 5); CHECK(completions == 1 && error == 0x01 && !asserted); /* Unknown HCI Command */
    }
    packet[0] = 0x01; packet[1] = 0x20; packet[2] = 0;
    run(packet, 3); CHECK(calls == 1 && seen[0] == 15 && completions == 1 && error == 0x01);
    packet[1] = 0xf8;
    run(packet, 3); CHECK(calls == 1 && seen[0] == 16 && completions == 1);
    for (int ogf = 0; ogf < 64; ogf++) {
        if ((ogf >= 1 && ogf <= 6) || ogf == 8 || ogf >= 62) continue;
        packet[0] = 1; packet[1] = (u8)(ogf << 2);
        run(packet, 3); CHECK(!calls && asserted == 1 && completions == 1 && error == 0x01);
    }

    /* Host packets: commands, ACL by handle owner, ISO. */
    u8 acl[] = {0x34, 0x22, 1, 0, 9};
    le_handle = 0x234; classic_handle = 0x30;
    h4_hci_push_data(2, acl, 5); CHECK(le_pushed == 5 && !classic_sent);
    le_pushed = 0; le_handle = -1; classic_handle = 0x234;
    h4_hci_push_data(2, acl, 5); CHECK(!le_pushed && classic_sent == 5);
    classic_sent = 0; config_btctler_modules = 2;
    h4_hci_push_data(2, acl, 5); CHECK(!le_pushed && !classic_sent);
    config_btctler_modules = 3;
    h4_hci_push_data(2, acl, 3); CHECK(!classic_sent);           /* shorter than its header */
    h4_hci_push_data(5, acl, 5); CHECK(iso_pushed == 5);
    packet[0] = 0x03; packet[1] = 0x0c; packet[2] = 0; memset(returns, 0, sizeof(returns)); returns[5] = 1;
    calls = 0; h4_hci_push_data(1, packet, 3); CHECK(calls == 1 && seen[0] == 5);
    return TEST_DONE("btctrler_hci_dispatch");
}
#endif
