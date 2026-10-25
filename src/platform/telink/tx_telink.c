#include "pad.h"

/* A negotiated LL maximum is an upper bound, not a required packet size.
 * Fragment app traffic more finely so four TX slots can stay small while
 * receiving full 251-byte packets. Guard every SDK FIFO insertion before
 * its copy, including SMP and LL control traffic. */
#define TX_FRAGMENT 70
u8 __real_blc_ll_get_connEffectiveMaxTxOctets(void);
int sdk_ll_push_tx_fifo(u16 handle, u8 *packet);

u8 __wrap_blc_ll_get_connEffectiveMaxTxOctets(void)
{
    u8 maximum = __real_blc_ll_get_connEffectiveMaxTxOctets();
    return maximum < TX_FRAGMENT ? maximum : TX_FRAGMENT;
}

int plat_ll_push_tx_fifo(u16 handle, u8 *packet)
{
    if (packet[1] > TX_FRAGMENT) return 0;
    return sdk_ll_push_tx_fifo(handle, packet);
}

#ifdef UNITTEST
static u8 maximum, pushed;
u8 __real_blc_ll_get_connEffectiveMaxTxOctets(void) { return maximum; }
int sdk_ll_push_tx_fifo(u16 handle, u8 *packet)
{ CHECK(handle == 64 && packet[1] <= TX_FRAGMENT); pushed++; return 1; }
int main(void)
{
    u8 packet[2] = {3, 70};
    maximum = 251; CHECK(__wrap_blc_ll_get_connEffectiveMaxTxOctets() == 70);
    maximum = 27; CHECK(__wrap_blc_ll_get_connEffectiveMaxTxOctets() == 27);
    CHECK(plat_ll_push_tx_fifo(64, packet) && pushed == 1);
    packet[1] = 71; CHECK(!plat_ll_push_tx_fifo(64, packet) && pushed == 1);
    packet[1] = 255; CHECK(!plat_ll_push_tx_fifo(64, packet) && pushed == 1);
    return TEST_DONE("Telink TX bounds");
}
#endif
