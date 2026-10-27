#include <string.h>
#include "dongle.h"

int dongle_address_valid(const u8 addr[6])
{
    static const u8 zero[6], erased[6] = {255,255,255,255,255,255};
    return memcmp(addr, zero, 6) && memcmp(addr, erased, 6);
}

void dongle_identity(u8 own[6])
{
    /* Stock VM item 2 is the dongle address in controller order.  Read
     * without modifying it. */
    if (plat_store_read(2, own, 6) != 6 || !dongle_address_valid(own)) {
        const u8 *uid = plat_unique_id();
        static const u8 fallback[6] = { 0, 0, 0, 0, 0, 0xc2 };
        memcpy(own, uid ? uid : fallback, 6);
        own[5] = (u8)((own[5] | 2) & 0xfe); /* locally administered unicast */
    }
}

/* Reserve radio time as the hub fills. Two links retain their fast interval;
 * scanning needs at least HUB_INTERVAL and one unused connection's share. */
u16 dongle_interval(u8 linked, int waiting, int hub)
{
    u16 want, scan;

    if (!hub) return LINK_INTERVAL;
    want = linked <= 2 ? LINK_INTERVAL : (u16)(2 * linked);
    scan = (u16)(2 * (linked + 1));
    if (scan < HUB_INTERVAL) scan = HUB_INTERVAL;
    if (waiting && want < scan) want = scan;
    return want;
}

#ifdef UNITTEST
static const u8 uid[16] = { 1, 2, 3, 4, 5, 0xff };
static const u8 stock_own[6] = { 0xa0, 0xfc, 0x2b, 0x81, 0x56, 0xd0 };
static int stored = 1, have_uid = 1;
const u8 *plat_unique_id(void) { return have_uid ? uid : 0; }
int plat_store_read(u16 key, void *buf, u16 len)
{
    CHECK(key == 2 && len == 6);
    if (!stored) return -1;
    if (stored == 2) memset(buf, 255, len);
    else memcpy(buf, stock_own, len);
    return len;
}
int main(void)
{
    u8 own[6];

    CHECK(dongle_interval(1, 0, 0) == LINK_INTERVAL);
    CHECK(dongle_interval(1, 0, 1) == LINK_INTERVAL);
    CHECK(dongle_interval(2, 0, 1) == LINK_INTERVAL);
    CHECK(dongle_interval(1, 1, 1) >= HUB_INTERVAL);
    CHECK(dongle_interval(2, 1, 1) >= HUB_INTERVAL);
    CHECK(dongle_interval(5, 1, 1) >= 12);
    CHECK(dongle_interval(6, 0, 1) >= 12);
    dongle_identity(own);
    CHECK(!memcmp(own, stock_own, 6));
    for (stored = 0; stored <= 2; stored += 2) {
        dongle_identity(own);
        CHECK(!memcmp(own, uid, 5) && own[5] == 0xfe);
    }
    stored = have_uid = 0;
    dongle_identity(own);
    CHECK(dongle_address_valid(own) && own[5] == 0xc2);
    return TEST_DONE("dongle_protocol");
}
#endif
