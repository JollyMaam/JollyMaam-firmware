#include <string.h>
#include "dongle.h"

static u32 crc32(const u8 *data, u16 n)
{
    static const u32 t[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    u32 crc = ~0u;

    while (n--) {
        crc ^= *data++;
        crc = (crc >> 4) ^ t[crc & 15];
        crc = (crc >> 4) ^ t[crc & 15];
    }
    return ~crc;
}

/* Stock dongle 1.21's builder at 0x01e0c26a: A2 11, three reserved bytes,
 * then 25 control bytes. The final two bytes are the low 16 bits of IEEE
 * CRC-32 over the first 23 control bytes, little-endian. This minimal output
 * keeps the stock pad connected; motors, control flags and USB mode are zero. */
void dongle_output(u8 out[DONGLE_OUTPUT_SIZE])
{
    memset(out, 0, DONGLE_OUTPUT_SIZE);
    out[0] = 0xa2;
    out[1] = 0x11;
    u32 checksum = crc32(out + 5, 23);
    out[28] = (u8)checksum;
    out[29] = (u8)(checksum >> 8);
}

int dongle_address_valid(const u8 addr[6])
{
    static const u8 zero[6], erased[6] = {255,255,255,255,255,255};
    return memcmp(addr, zero, 6) && memcmp(addr, erased, 6);
}

void dongle_identity(u8 own[6], u8 peer[6])
{
    /* Stock VM item 2 is the dongle address in controller order; item 3 is
     * the pad's base address in display order. Read without modifying either. */
    if (plat_store_read(2, own, 6) != 6 || !dongle_address_valid(own)) {
        const u8 *uid = plat_unique_id();
        static const u8 fallback[6] = { 0, 0, 0, 0, 0, 0xc2 };
        memcpy(own, uid ? uid : fallback, 6);
        own[5] = (u8)((own[5] | 2) & 0xfe); /* locally administered unicast */
    }
    if (plat_store_read(3, peer, 6) != 6 || !dongle_address_valid(peer))
        memset(peer, 0, 6);       /* listen until a target is supplied or a pad connects */
}

#ifdef UNITTEST
static const u8 uid[16] = { 1, 2, 3, 4, 5, 0xff };
static const u8 stock_own[6] = { 0xa0, 0xfc, 0x2b, 0x81, 0x56, 0xd0 };
static const u8 stock_peer[6] = { 0xd0, 0x56, 0x80, 0xb1, 0x65, 0x1b };
static int stored = 1, have_uid = 1;
const u8 *plat_unique_id(void) { return have_uid ? uid : 0; }
int plat_store_read(u16 key, void *buf, u16 len)
{
    CHECK((key == 2 || key == 3) && len == 6);
    if (!stored) return -1;
    if (stored == 2) memset(buf, 255, len);
    else memcpy(buf, key == 2 ? stock_own : stock_peer, len);
    return len;
}
int main(void)
{
    u8 out[DONGLE_OUTPUT_SIZE], own[6], peer[6], zero[6] = {0};

    /* Independent IEEE check vector, and fixed output checked with zlib. */
    CHECK(crc32((const u8 *)"123456789", 9) == 0xcbf43926);
    memset(out, 0xff, sizeof(out));
    dongle_output(out);
    CHECK(out[0] == 0xa2 && out[1] == 0x11);
    for (int i = 2; i < 28; i++) CHECK(out[i] == 0);
    CHECK(out[28] == 0x44 && out[29] == 0x24);
    dongle_identity(own, peer);
    CHECK(!memcmp(own, stock_own, 6) && !memcmp(peer, stock_peer, 6));
    for (stored = 0; stored <= 2; stored += 2) {
        dongle_identity(own, peer);
        CHECK(!memcmp(own, uid, 5) && own[5] == 0xfe && !memcmp(peer, zero, 6));
    }
    stored = have_uid = 0;
    dongle_identity(own, peer);
    CHECK(dongle_address_valid(own) && own[5] == 0xc2 && !memcmp(peer, zero, 6));
    return TEST_DONE("dongle_protocol");
}
#endif
