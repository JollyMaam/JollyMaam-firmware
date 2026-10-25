#include "pad.h"
#include "att_telink.h"

/* Complete PDUs stay in the controller's full-size RX FIFO. Fragmented
 * PDUs go through the SDK's smaller L2CAP buffer only if they fit. The SDK
 * bounds continuation copies but does not bound its first-fragment copy. */
int blc_l2cap_packet_receive(u16 handle, u8 *packet);
static u8 skip;
static u16 get16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

void plat_l2cap_reset(void) { skip = 0; }

int plat_l2cap_receive(u16 handle, u8 *packet)
{
    u8 kind = packet[0] & 3, len = packet[1];
    u16 total;
    if (kind == 2) {
        skip = 0;
        if (len < 4) return 0;
        total = get16(packet + 2);
        if (len != total + 4 && (total > TELINK_ATT_BUFFER || len + 2 >= TELINK_ATT_BUFFER + 7)) {
            skip = 1;
            if (get16(packet + 4) == 4 && len >= 7)
                plat_att_reject_large(packet[6], get16(packet + 7));
            return 0;
        }
    } else if (kind == 1 && skip) return 0;
    return blc_l2cap_packet_receive(handle, packet);
}

#ifdef UNITTEST
static u32 accepted, rejected;
int blc_l2cap_packet_receive(u16 handle, u8 *packet)
{ CHECK(handle == 64 && packet); accepted++; return 0; }
void plat_att_reject_large(u8 opcode, u16 handle)
{ CHECK(opcode == 0x12 && handle == 0x14); rejected++; }
int main(void)
{
    u8 packet[253] = {2, 251, 0x2c,1, 4,0, 0x12,0x14,0};
    plat_l2cap_reset(); plat_l2cap_receive(64, packet);
    CHECK(!accepted && rejected == 1);
    packet[0] = 1; packet[1] = 53; plat_l2cap_receive(64, packet); CHECK(!accepted);
    packet[0] = 2; packet[1] = 204; packet[2] = 200; packet[3] = 0;
    plat_l2cap_receive(64, packet); CHECK(accepted == 1); /* complete, no copy */
    packet[1] = 27; packet[2] = 100;
    plat_l2cap_receive(64, packet); CHECK(accepted == 2);
    packet[0] = 1; packet[1] = 27; plat_l2cap_receive(64, packet); CHECK(accepted == 3);
    packet[0] = 2; packet[1] = 3; plat_l2cap_receive(64, packet); CHECK(accepted == 3);
    return TEST_DONE("Telink RX bounds");
}
#endif
