#include "pad.h"

/* The Switch 2 uses a 5 ms link (four 1.25 ms units). The SDK's
 * connection-request validator rejects anything below six. Keep all its
 * other checks: substitute six only while validating, then restore four
 * before the SDK builds the actual connection and its timing.
 *
 * The linker aliases the original input section as sdk_ll_procConnectReq
 * and redirects blc_ll_procConnectReq here. --wrap alone cannot intercept
 * a reference defined in the caller's own archive object. SDK untouched. */
void sdk_ll_procConnectReq(u8 *packet, u8 address_type);
void __real_blt_connect(u8 *packet);

static u8 enabled;
static u8 *short_request;
static u16 requested;
static u8 pending;

void plat_ll_allow_short_interval(int allow) { enabled = (u8)allow; }

void plat_ll_connect_request(u8 *packet, u8 address_type)
{
    u16 interval = (u16)(packet[28] | packet[29] << 8);
    requested = interval;
    pending = 1;
    if (enabled && interval == 4 && packet[25] < 4) {
        packet[28] = 6;
        short_request = packet;
        sdk_ll_procConnectReq(packet, address_type);
        packet[28] = 4;
        short_request = NULL;
    } else sdk_ll_procConnectReq(packet, address_type);
}

void __wrap_blt_connect(u8 *packet)
{
    if (packet == short_request) packet[28] = 4;
    __real_blt_connect(packet);
}

int plat_ll_requested_interval(u16 *interval)
{
    if (!pending) return 0;
    *interval = requested;
    pending = 0;
    return 1;
}

#ifdef UNITTEST
static u8 seen_validation, seen_connection, reject;
void sdk_ll_procConnectReq(u8 *packet, u8 address_type)
{
    (void)address_type;
    seen_validation = packet[28];
    if (!reject && packet[28] >= 6) __wrap_blt_connect(packet);
}
void __real_blt_connect(u8 *packet) { seen_connection = packet[28]; }
int main(void)
{
    u8 packet[40] = {0};
    u16 interval;
    packet[25] = 1; packet[28] = 4;
    plat_ll_connect_request(packet, 0);
    CHECK(seen_validation == 4 && !seen_connection && packet[28] == 4);
    plat_ll_allow_short_interval(1);
    plat_ll_connect_request(packet, 1);
    CHECK(seen_validation == 6 && seen_connection == 4 && packet[28] == 4);
    CHECK(plat_ll_requested_interval(&interval) && interval == 4);
    CHECK(!plat_ll_requested_interval(&interval));
    reject = 1; seen_connection = 0;
    plat_ll_connect_request(packet, 0);
    CHECK(!seen_connection && packet[28] == 4);
    reject = 0; packet[25] = 4; seen_connection = 0;
    plat_ll_connect_request(packet, 0); CHECK(seen_validation == 4 && !seen_connection);
    packet[25] = 1; packet[28] = 12;
    plat_ll_connect_request(packet, 0);
    CHECK(seen_validation == 12 && seen_connection == 12 && packet[28] == 12);
    return TEST_DONE("Telink connection interval");
}
#endif
