/* Owned Classic link-control command dispatch, recovered from BR23 SDK IR.
 * Parameters start after the three-byte HCI header. All address commands
 * have BD_ADDR at offset zero; no framed-buffer guessing is needed here.
 * The six-word SDK message is consumed synchronously by its role machine.
 * Debug printfs are omitted; valid-command event order is preserved. */
#include <string.h>
#include "btctrler_sdk_abi.h"

extern const int CONFIG_BREDR_INQUIRY;
extern const int CONFIG_LMP_MASTER_ESCO_ENABLE;

static unsigned short read16(const unsigned char *p)
{ return (unsigned short)(p[0] | ((unsigned short)p[1] << 8)); }
static unsigned int read32(const unsigned char *p)
{ return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
         ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24); }
static void status(unsigned short opcode, int error)
{ BTCTRLER_BIN_hci_send_event(0x0f, "112", error, 1, (int)opcode); }
static void complete(unsigned short opcode)
{ BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1", 0); }
static void address_complete(unsigned short opcode, unsigned char *address)
{ BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1A", 0, address); }

int hci_lmp_link_control_cmd_handler(unsigned short opcode, unsigned char *cmd, int len)
{
    int required;
    switch (opcode) {
    case 0x0401: /* Inquiry */
        if (!CONFIG_BREDR_INQUIRY) return 1;
        required = 5; break;
    case 0x0402: /* Inquiry Cancel */
        if (!CONFIG_BREDR_INQUIRY) return 1;
        required = 0; break;
    case 0x0405: required = 13; break; /* Create Connection */
    case 0x0409: required = 7; break; /* Accept Connection Request */
    case 0x040b: required = 22; break; /* Link Key Request Reply */
    case 0x040c: required = 6; break; /* Link Key Request Negative Reply */
    case 0x040d: required = 23; break; /* PIN Code Request Reply */
    case 0x040e: required = 6; break; /* PIN Code Request Negative Reply */
    case 0x0411: required = 2; break; /* Authentication Requested */
    case 0x0413: required = 3; break; /* Set Connection Encryption */
    case 0x0419: required = 10; break; /* Remote Name Request */
    case 0x041a: required = 6; break; /* Remote Name Request Cancel */
    case 0x041b: required = 2; break; /* Read Remote Supported Features */
    case 0x041c: required = 3; break; /* Read Remote Extended Features */
    case 0x041f: required = 2; break; /* Read Clock Offset */
    case 0x0428: /* Setup Synchronous Connection */
    case 0x0429: /* Accept Synchronous Connection Request */
    case 0x042a: /* Reject Synchronous Connection Request */
        if (!CONFIG_LMP_MASTER_ESCO_ENABLE) return 1;
        required = opcode == 0x0428 ? 17 : opcode == 0x0429 ? 21 : 7;
        break;
    case 0x042b: required = 9; break; /* IO Capability Request Reply */
    case 0x042c: required = 6; break; /* User Confirmation Request Reply */
    case 0x042d: required = 6; break; /* User Confirmation Negative Reply */
    case 0x042e: required = 10; break; /* User Passkey Request Reply */
    case 0x042f: required = 6; break; /* User Passkey Request Negative Reply */
    case 0x0c60: required = 7; break; /* Send Keypress Notification */
    default: return 0;
    }
    if (len < required || (required && !cmd)) {
        /* Reply commands use Command Complete, asynchronous procedures use
         * Command Status. Include a zero BD_ADDR when too short to read it. */
        static const unsigned char no_address[6];
        if ((opcode >= 0x040b && opcode <= 0x040e) ||
            (opcode >= 0x042b && opcode <= 0x042f) || opcode == 0x0c60)
            BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1A", 0x12,
                cmd && len >= 6 ? cmd : no_address);
        else if (opcode == 0x0402) /* No parameters, only a negative length. */
            BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1", 0x12);
        else if (opcode == 0x041a)
            BTCTRLER_BIN_hci_send_event_cmd_complete(opcode, "1", 0x12);
        else status(opcode, 0x12);
        return 1;
    }
    /* SDK initializes only words used by each procedure. Initialize the
     * remainder too so unused fields cannot contain old stack contents. */
    int msg[6] = { opcode, 0, 0, 0, 0, 0 };
    switch (opcode) {
    case 0x0401: {
        unsigned int lap = (unsigned int)cmd[0] | ((unsigned int)cmd[1] << 8) |
                           ((unsigned int)cmd[2] << 16);
        status(opcode, 0);
        BTCTRLER_BIN_link_inquiry_enable((unsigned char *)&lap, cmd[3] * 1280,
                                       cmd[4], 0);
        break;
    }
    case 0x0402:
        complete(opcode);
        BTCTRLER_BIN_link_inquiry_disable();
        BTCTRLER_BIN_hci_send_event(1, "1", 0);
        break;
    case 0x0405:
        status(opcode, 0);
        BTCTRLER_BIN_link_page_try_start_enable(0, cmd, cmd[8], 0);
        break;
    case 0x0409:
        status(opcode, 0);
        msg[1] = cmd[6];
        lmp_hci_cmd_addr_to_conn(cmd, msg);
        break;
    case 0x0411:
    case 0x0413:
        status(opcode, 0);
        if (opcode == 0x0413) msg[1] = cmd[2];
        BTCTRLER_BIN_lmp_hci_cmd_handler_to_conn(cmd, msg);
        break;
    case 0x040b:
        memcpy(&msg[1], cmd + 6, 16);
        address_complete(opcode, cmd);
        lmp_hci_cmd_addr_to_conn(cmd, msg);
        break;
    case 0x040d:
        msg[1] = cmd[6];
        memcpy(&msg[2], cmd + 7, 16);
        address_complete(opcode, cmd);
        lmp_hci_cmd_addr_to_conn(cmd, msg);
        break;
    case 0x042b:
        msg[1] = cmd[6]; msg[2] = cmd[7]; msg[3] = cmd[8];
        address_complete(opcode, cmd);
        lmp_hci_cmd_addr_to_conn(cmd, msg);
        break;
    case 0x040c: case 0x040e: case 0x042c:
        address_complete(opcode, cmd);
        lmp_hci_cmd_addr_to_conn(cmd, msg);
        break;
    case 0x042d: case 0x042e: case 0x042f: case 0x0c60:
        /* Preserve the SDK's route-before-complete order and opcode-only
         * messages, including its unused passkey/keypress parameters. */
        lmp_hci_cmd_addr_to_conn(cmd, msg);
        address_complete(opcode, cmd);
        break;
    case 0x0419:
        status(opcode, 0);
        BTCTRLER_BIN_lmp_hci_remote_name_request(cmd, cmd[6], read16(cmd + 8));
        break;
    case 0x041a:
        complete(opcode);
        BTCTRLER_BIN_link_page_disable(4);
        break;
    case 0x041b: case 0x041c: {
        unsigned short handle = read16(cmd) & 0x0fff;
        if (!BTCTRLER_BIN_lmp_conn_for_handle(handle)) { status(opcode, 2); break; }
        status(opcode, 0);
        if (opcode == 0x041b) BTCTRLER_BIN_lmp_hci_read_remote_supported_features(handle);
        else BTCTRLER_BIN_lmp_hci_read_remote_extended_features(handle);
        /* The SDK helper has no page argument. Preserve that limitation. */
        break;
    }
    case 0x041f:
        status(opcode, 0);
        BTCTRLER_BIN_lmp_hci_read_clock_offset(read16(cmd) & 0x0fff);
        break;
    case 0x0428:
        status(opcode, 0);
        BTCTRLER_BIN_lmp_hci_setup_sync_connection(read16(cmd), read32(cmd + 2),
            read32(cmd + 6), read16(cmd + 10), read16(cmd + 12), cmd[14], read16(cmd + 15));
        break;
    case 0x0429: case 0x042a:
        /* Retain the SDK's handle-based routing of these voice commands.
         * The pad disables this feature; this is not a corrected voice ABI. */
        msg[1] = cmd[2];
        BTCTRLER_BIN_lmp_hci_cmd_handler_to_conn(cmd, msg);
        if (opcode == 0x0429) status(opcode, 0);
        break;
    }
    return 1;
}

#ifdef UNITTEST
#include <stdarg.h>
#include "test.h"
/* Exercise branches normally disabled by the pad's platform configuration. */
const int CONFIG_BREDR_INQUIRY = 1;
const int CONFIG_LMP_MASTER_ESCO_ENABLE = 1;
static int sequence, event_at, route_at, event_kind, event_error, event_opcode;
static int helper, words[6], found = 1;
static unsigned char event_address[6], routed_address[6];
static unsigned int args[7];
static void clear(void)
{
    sequence = event_at = route_at = event_kind = helper = 0;
    memset(words, 0xaa, sizeof(words)); memset(args, 0, sizeof(args));
}
int BTCTRLER_BIN_hci_send_event(int event, const char *format, ...)
{
    va_list ap; va_start(ap, format);
    event_at = ++sequence; event_kind = event; event_error = va_arg(ap, int);
    if (!strcmp(format, "112")) {
        CHECK(va_arg(ap, int) == 1); event_opcode = va_arg(ap, int);
    } else CHECK(event == 1 && !strcmp(format, "1"));
    va_end(ap); return 0;
}
int BTCTRLER_BIN_hci_send_event_cmd_complete(int opcode, const char *format, ...)
{
    va_list ap; va_start(ap, format);
    event_at = ++sequence; event_kind = 0x0e; event_opcode = opcode;
    event_error = va_arg(ap, int);
    if (!strcmp(format, "1A")) memcpy(event_address, va_arg(ap, const unsigned char *), 6);
    else CHECK(!strcmp(format, "1"));
    va_end(ap); return 0;
}
void lmp_hci_cmd_addr_to_conn(unsigned char *cmd, int *msg)
{
    route_at = ++sequence; memcpy(routed_address, cmd, 6); memcpy(words, msg, sizeof(words));
}
void BTCTRLER_BIN_lmp_hci_cmd_handler_to_conn(unsigned char *cmd, int *msg)
{
    route_at = ++sequence; args[0] = read16(cmd); memcpy(words, msg, sizeof(words));
}
struct lmp_connection *BTCTRLER_BIN_lmp_conn_for_handle(int handle)
{ args[0] = handle; return found ? (struct lmp_connection *)args : 0; }
void BTCTRLER_BIN_link_page_try_start_enable(unsigned char type, unsigned char *address,
    unsigned char repetition, int timeout)
{
    helper = 1; route_at = ++sequence; CHECK(type == 0 && timeout == 0);
    memcpy(routed_address, address, 6); args[0] = repetition;
}
int BTCTRLER_BIN_link_inquiry_enable(unsigned char *lap, int duration,
    unsigned char count, int (*result)(struct link_inquiry_result *))
{
    helper = 2; route_at = ++sequence; args[0] = read32(lap); args[1] = duration;
    args[2] = count; CHECK(!result); return 0;
}
void BTCTRLER_BIN_link_inquiry_disable(void) { helper = 3; route_at = ++sequence; }
void BTCTRLER_BIN_link_page_disable(int reason)
{ helper = 4; route_at = ++sequence; args[0] = reason; }
void BTCTRLER_BIN_lmp_hci_read_remote_supported_features(unsigned short handle)
{ helper = 5; route_at = ++sequence; args[0] = handle; }
void BTCTRLER_BIN_lmp_hci_read_remote_extended_features(unsigned short handle)
{ helper = 6; route_at = ++sequence; args[0] = handle; }
void BTCTRLER_BIN_lmp_hci_read_clock_offset(unsigned short handle)
{ helper = 7; route_at = ++sequence; args[0] = handle; }
void BTCTRLER_BIN_lmp_hci_remote_name_request(unsigned char *address,
    unsigned char repetition, unsigned short clock)
{
    helper = 8; route_at = ++sequence; memcpy(routed_address, address, 6);
    args[0] = repetition; args[1] = clock;
}
void BTCTRLER_BIN_lmp_hci_setup_sync_connection(unsigned short handle,
    unsigned int tx, unsigned int rx, unsigned short latency, unsigned short voice,
    unsigned char retransmission, unsigned short packets)
{
    helper = 9; route_at = ++sequence; args[0] = handle; args[1] = tx; args[2] = rx;
    args[3] = latency; args[4] = voice; args[5] = retransmission; args[6] = packets;
}
static void run(unsigned short opcode, unsigned char *cmd, int len)
{ clear(); CHECK(hci_lmp_link_control_cmd_handler(opcode, cmd, len) == 1); }
int main(void)
{
    const struct { unsigned short opcode; int len, complete; } commands[] = {
        {0x0401,5,0},{0x0402,0,1},{0x0405,13,0},{0x0409,7,0},
        {0x040b,22,1},{0x040c,6,1},{0x040d,23,1},{0x040e,6,1},
        {0x0411,2,0},{0x0413,3,0},{0x0419,10,0},{0x041a,6,1},
        {0x041b,2,0},{0x041c,3,0},{0x041f,2,0},
        {0x0428,17,0},{0x0429,21,0},{0x042a,7,0},
        {0x042b,9,1},{0x042c,6,1},{0x042d,6,1},{0x042e,10,1},
        {0x042f,6,1},{0x0c60,7,1}
    };
    /* Null storage proves each rejection precedes any parameter access or
     * call to the connection machine, even for every partial length. */
    for (unsigned int i = 0; i < sizeof(commands)/sizeof(commands[0]); i++) {
        for (int len = -1; len < commands[i].len; len++) {
            run(commands[i].opcode, 0, len);
            CHECK(event_kind == (commands[i].complete ? 0x0e : 0x0f));
            CHECK(event_error == 0x12 && event_opcode == commands[i].opcode);
            CHECK(!route_at && !helper);
        }
    }
    unsigned char cmd[23];
    for (int i = 0; i < 23; i++) cmd[i] = (unsigned char)(i + 1);
    clear(); CHECK(hci_lmp_link_control_cmd_handler(0x0406, 0, 0) == 0);
    CHECK(!sequence); /* Another SDK dispatcher owns Disconnect. */
    run(0x0405, cmd, 13);
    CHECK(helper == 1 && args[0] == 9 && event_at < route_at);
    CHECK(!memcmp(cmd, routed_address, 6));
    run(0x0409, cmd, 7);
    CHECK(words[0] == 0x0409 && words[1] == 7 && event_at < route_at);
    run(0x040b, cmd, 22);
    CHECK(words[0] == 0x040b && !memcmp(&words[1], cmd + 6, 16));
    CHECK(words[5] == 0 && event_at < route_at && !memcmp(event_address, cmd, 6));
    run(0x040d, cmd, 23);
    CHECK(words[1] == 7 && !memcmp(&words[2], cmd + 7, 16) && event_at < route_at);
    run(0x042b, cmd, 9);
    CHECK(words[1] == 7 && words[2] == 8 && words[3] == 9 && event_at < route_at);
    CHECK(words[4] == 0 && words[5] == 0);
    const unsigned short before[] = {0x040c,0x040e,0x042c};
    const unsigned short after[] = {0x042d,0x042e,0x042f,0x0c60};
    for (unsigned int i = 0; i < sizeof(before)/sizeof(before[0]); i++) {
        run(before[i], cmd, 23); CHECK(event_at < route_at && words[0] == before[i]);
    }
    for (unsigned int i = 0; i < sizeof(after)/sizeof(after[0]); i++) {
        run(after[i], cmd, 23); CHECK(route_at < event_at && words[0] == after[i]);
    }
    run(0x0411, cmd, 2); CHECK(args[0] == 0x0201 && words[1] == 0 && event_at < route_at);
    run(0x0413, cmd, 3); CHECK(words[1] == 3 && event_at < route_at);
    cmd[1] = 0xf2;
    run(0x041b, cmd, 2); CHECK(helper == 5 && args[0] == 0x0201 && event_at < route_at);
    run(0x041c, cmd, 3); CHECK(helper == 6 && args[0] == 0x0201 && event_at < route_at);
    found = 0; run(0x041b, cmd, 2); CHECK(event_error == 2 && !helper && !route_at);
    run(0x041c, cmd, 3); CHECK(event_error == 2 && !helper && !route_at); found = 1;
    run(0x041f, cmd, 2); CHECK(helper == 7 && args[0] == 0x0201 && event_at < route_at);
    cmd[1] = 2;
    run(0x0419, cmd, 10); CHECK(helper == 8 && args[0] == 7 && args[1] == 0x0a09);
    run(0x041a, cmd, 6); CHECK(helper == 4 && args[0] == 4 && event_at < route_at);
    run(0x0401, cmd, 5); CHECK(helper == 2 && args[0] == 0x030201 && args[1] == 5120 && args[2] == 5);
    run(0x0402, 0, 0); CHECK(helper == 3 && event_kind == 1 && event_at > route_at);
    run(0x0428, cmd, 17);
    CHECK(helper == 9 && args[0] == 0x0201 && args[1] == 0x06050403 && args[2] == 0x0a090807);
    CHECK(args[3] == 0x0c0b && args[4] == 0x0e0d && args[5] == 15 && args[6] == 0x1110);
    run(0x0429, cmd, 21); CHECK(words[1] == 3 && route_at < event_at);
    run(0x042a, cmd, 7); CHECK(words[1] == 3 && route_at && !event_at);
    return TEST_DONE("btctrler_link_control");
}
#endif
