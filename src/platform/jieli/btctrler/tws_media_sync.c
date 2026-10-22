/* The one live piece of the SDK's TWS media sync (tws_media_sync.c):
 * get_bt_slot_time, the Classic slot-clock to time converter that the
 * (unused, TWS-only) tws_sync_call code still names.  The rest of the
 * member is earbud media sync and goes with it.  Kept as the SDK has it:
 * the 16-bit slot masks, unsigned wrapping arithmetic, TWS adjustments only
 * when CONFIG_BTCTLER_TWS_ENABLE is set and a TWS link exists, and the
 * local-time callback running after the clock read but before the
 * "too far ahead" (> 10000 slots) test.  Types 0..4 select the conversion;
 * any other type returns 0. */
#include "btctrler/glue.h"

extern BTCTRLER_CONFIG int CONFIG_BTCTLER_TWS_ENABLE;

#define TWS_CODE __attribute__((section(".classic_tws_code")))

/* Slot clock and fine bit count; re-read once if the count is below 56. */
static void read_slot_info(u32 *slot, u32 *delta) TWS_CODE;
static void read_slot_info(u32 *slot, u32 *delta)
{
    *slot = bredr_read_slot_clk();
    *delta = bredr_finetimecnt();
    if (*delta < 56)
        *slot = bredr_read_slot_clk();
}

u32 get_bt_slot_time(u8 type, u32 time, int *ret_time, int (*local_us_time)(void)) TWS_CODE;
u32 get_bt_slot_time(u8 type, u32 time, int *ret_time, int (*local_us_time)(void))
{
    u32 slot = 0, delta = 0, ret = 0;
    u32 remote_clkoffset, bitoff;
    int tws_on;

    switch (type) {
    case 0:
        ret = bredr_read_slot_clk();
        break;
    case 1:
        read_slot_info(&slot, &delta);
        ret = slot * 625 + delta;
        break;
    case 3:
        /* bt_future_time: time moved onto the TWS peer's clock, masked to 16 bits. */
        remote_clkoffset = 0;
        tws_on = CONFIG_BTCTLER_TWS_ENABLE != 0;
        if (tws_on && tws_link.link) {
            remote_clkoffset = bredr_link_clk_offset(tws_link.link);
            if (lc_local_slot_bitoff(tws_link.link) < 4)
                remote_clkoffset = bredr_link_clk_offset(tws_link.link);
        }
        read_slot_info(&slot, &delta);
        ret = (remote_clkoffset + time + slot) & 65535;
        break;
    case 2:
        /* bt_local_future_us_diff: microseconds from now to the (peer-clock)
         * slot "time"; a slot more than 10000 ahead is treated as behind. */
        remote_clkoffset = 0;
        bitoff = 0;
        tws_on = CONFIG_BTCTLER_TWS_ENABLE != 0;
        if (tws_on && tws_link.link) {
            remote_clkoffset = bredr_link_clk_offset(tws_link.link);
            bitoff = lc_local_slot_bitoff(tws_link.link);
            if (bitoff < 4)
                remote_clkoffset = bredr_link_clk_offset(tws_link.link);
            time = (time - remote_clkoffset) & 65535;
        }
        read_slot_info(&slot, &delta);
        {
            u32 ahead = (time - slot) & 65535;
            *ret_time = local_us_time();
            if (ahead > 10000) {
                u32 behind = (slot - time) & 65535;
                ret = 0 - delta + bitoff + behind * (u32)-625;
            } else {
                ret = ahead * 625 + bitoff - delta;
            }
        }
        break;
    case 4:
        /* slot_unit_to_us_time: slots to microseconds, less the fine count. */
        bitoff = 0;
        tws_on = CONFIG_BTCTLER_TWS_ENABLE != 0;
        if (tws_on && tws_link.link)
            bitoff = lc_local_slot_bitoff(tws_link.link);
        ret = bitoff + time * 625 - bredr_finetimecnt();
        break;
    default:
        break;
    }
    return ret;
}

#ifdef UNITTEST
#include "test.h"

int CONFIG_BTCTLER_TWS_ENABLE;
struct tws_link_connection tws_link;

static u32 clk_values[4], clk_reads, fine_value;
static u32 clk_offsets[4], offset_calls;
static u32 bitoff_value, bitoff_calls, us_calls, us_value;
static char order[16];
static int order_len;

u32 bredr_read_slot_clk(void)
{
    u32 v = clk_values[clk_reads < 3 ? clk_reads : 3];
    clk_reads++;
    order[order_len++] = 'c';
    return v;
}
u32 bredr_finetimecnt(void) { order[order_len++] = 'f'; return fine_value; }
int bredr_link_clk_offset(struct link_connection *lc)
{
    (void)lc;
    order[order_len++] = 'o';
    return (int)clk_offsets[offset_calls++];
}
u32 lc_local_slot_bitoff(struct link_connection *lc)
{
    (void)lc;
    bitoff_calls++;
    order[order_len++] = 'b';
    return bitoff_value;
}
static int local_us(void) { us_calls++; order[order_len++] = 'u'; return (int)us_value; }

static void reset(void)
{
    memset(clk_values, 0, sizeof(clk_values)); clk_reads = 0; fine_value = 100;
    memset(clk_offsets, 0, sizeof(clk_offsets)); offset_calls = 0;
    bitoff_value = 0; bitoff_calls = 0; us_calls = 0; us_value = 0;
    memset(order, 0, sizeof(order)); order_len = 0;
    CONFIG_BTCTLER_TWS_ENABLE = 0; memset(&tws_link, 0, sizeof(tws_link));
}

int main(void)
{
    static struct link_connection peer;
    int ret_time = 0x55;

    /* Unknown types return 0 and read nothing. */
    reset();
    CHECK(get_bt_slot_time(5, 7, &ret_time, local_us) == 0);
    CHECK(get_bt_slot_time(255, 7, &ret_time, local_us) == 0);
    CHECK(clk_reads == 0 && order_len == 0 && ret_time == 0x55);

    /* Type 0: the raw slot clock. */
    reset(); clk_values[0] = 0x1234567;
    CHECK(get_bt_slot_time(0, 9, &ret_time, local_us) == 0x1234567 && clk_reads == 1);

    /* Type 1: slot*625 + fine; fine >= 56 reads the clock once. */
    reset(); clk_values[0] = 1000; fine_value = 56;
    CHECK(get_bt_slot_time(1, 0, &ret_time, local_us) == 1000 * 625 + 56);
    CHECK(!strcmp(order, "cf"));
    /* fine < 56 re-reads the clock and uses the second value. */
    reset(); clk_values[0] = 1000; clk_values[1] = 1001; fine_value = 55;
    CHECK(get_bt_slot_time(1, 0, &ret_time, local_us) == 1001 * 625 + 55);
    CHECK(!strcmp(order, "cfc"));
    /* the product wraps in 32 bits */
    reset(); clk_values[0] = clk_values[1] = 0x10000000; fine_value = 3;
    CHECK(get_bt_slot_time(1, 0, &ret_time, local_us) == (u32)(0x10000000u * 625u) + 3u);

    /* Type 3 without TWS: (time + slot) & 0xffff; nothing about TWS is read. */
    reset(); clk_values[0] = 0x20010; fine_value = 60; clk_offsets[0] = 999;
    CHECK(get_bt_slot_time(3, 0xfff0, &ret_time, local_us) == ((0xfff0u + 0x20010u) & 65535));
    CHECK(offset_calls == 0 && bitoff_calls == 0);
    /* TWS enabled but no link, then a link but TWS disabled: no adjustment. */
    reset(); CONFIG_BTCTLER_TWS_ENABLE = 1; clk_values[0] = 5; clk_offsets[0] = 999;
    CHECK(get_bt_slot_time(3, 10, &ret_time, local_us) == 15 && offset_calls == 0);
    reset(); tws_link.link = &peer; clk_values[0] = 5; clk_offsets[0] = 999;
    CHECK(get_bt_slot_time(3, 10, &ret_time, local_us) == 15 && offset_calls == 0);
    /* TWS and link, bit offset >= 4: the first offset only. */
    reset(); CONFIG_BTCTLER_TWS_ENABLE = 1; tws_link.link = &peer;
    clk_values[0] = 5; clk_offsets[0] = 100; clk_offsets[1] = 200; bitoff_value = 4;
    CHECK(get_bt_slot_time(3, 10, &ret_time, local_us) == 115);
    CHECK(!strcmp(order, "obcf"));
    /* bit offset < 4: the offset is read again and the second value is used. */
    reset(); CONFIG_BTCTLER_TWS_ENABLE = 1; tws_link.link = &peer;
    clk_values[0] = 5; clk_offsets[0] = 100; clk_offsets[1] = 200; bitoff_value = 3;
    CHECK(get_bt_slot_time(3, 10, &ret_time, local_us) == 215);
    CHECK(!strcmp(order, "obocf"));
    /* the 16-bit mask applies after the sum (negative offsets wrap) */
    reset(); CONFIG_BTCTLER_TWS_ENABLE = 1; tws_link.link = &peer;
    clk_values[0] = 2; clk_offsets[0] = (u32)-20; clk_offsets[1] = (u32)-20; bitoff_value = 0;
    CHECK(get_bt_slot_time(3, 10, &ret_time, local_us) == ((u32)(-20 + 10 + 2) & 65535));

    /* Type 2 without TWS: ahead = (time - slot) & 0xffff; result ahead*625 - delta. */
    reset(); clk_values[0] = 100; fine_value = 70; us_value = 4242;
    CHECK(get_bt_slot_time(2, 150, &ret_time, local_us) == 50 * 625 - 70);
    CHECK(ret_time == 4242 && us_calls == 1 && !strcmp(order, "cfu"));
    /* ahead exactly 10000 is still "ahead". */
    reset(); clk_values[0] = 100; fine_value = 70;
    CHECK(get_bt_slot_time(2, 10100, &ret_time, local_us) == 10000u * 625u - 70u);
    /* ahead 10001: treated as behind by (slot - time) & 0xffff slots. */
    reset(); clk_values[0] = 100; fine_value = 70;
    CHECK(get_bt_slot_time(2, 10101, &ret_time, local_us) == (u32)(-70 - 625 * (int)((100u - 10101u) & 65535)));
    /* time behind slot by 5: ahead wraps to 65531 (> 10000), behind is 5. */
    reset(); clk_values[0] = 100; fine_value = 70;
    CHECK(get_bt_slot_time(2, 95, &ret_time, local_us) == (u32)(-70 - 625 * 5));
    /* without TWS the time is not masked first; the difference is. */
    reset(); clk_values[0] = clk_values[1] = 100; fine_value = 0;
    CHECK(get_bt_slot_time(2, 0x10000 + 150, &ret_time, local_us) == 50u * 625u);
    /* with TWS: time = (time - offset) & 0xffff, bit offset added. */
    reset(); CONFIG_BTCTLER_TWS_ENABLE = 1; tws_link.link = &peer;
    clk_values[0] = clk_values[1] = 100; fine_value = 10; clk_offsets[0] = 30; clk_offsets[1] = 40; bitoff_value = 7;
    CHECK(get_bt_slot_time(2, 190, &ret_time, local_us) == (190u - 30u - 100u) * 625u + 7u - 10u);
    CHECK(!strcmp(order, "obcfcu"));
    /* bit offset < 4 re-reads the offset: 40 is used. */
    reset(); CONFIG_BTCTLER_TWS_ENABLE = 1; tws_link.link = &peer;
    clk_values[0] = clk_values[1] = 100; fine_value = 10; clk_offsets[0] = 30; clk_offsets[1] = 40; bitoff_value = 2;
    CHECK(get_bt_slot_time(2, 190, &ret_time, local_us) == (190u - 40u - 100u) * 625u + 2u - 10u);
    CHECK(!strcmp(order, "obocfcu"));

    /* Type 4: bitoff + time*625 - fine; bit offset only with TWS and a link. */
    reset(); fine_value = 33;
    CHECK(get_bt_slot_time(4, 10, &ret_time, local_us) == 6250 - 33 && !strcmp(order, "f"));
    reset(); CONFIG_BTCTLER_TWS_ENABLE = 1; tws_link.link = &peer; bitoff_value = 9; fine_value = 33;
    CHECK(get_bt_slot_time(4, 10, &ret_time, local_us) == 6250 + 9 - 33 && !strcmp(order, "bf"));
    reset(); CONFIG_BTCTLER_TWS_ENABLE = 1; bitoff_value = 9; fine_value = 33;
    CHECK(get_bt_slot_time(4, 10, &ret_time, local_us) == 6250 - 33);
    return TEST_DONE("btctrler_tws_media_sync");
}
#endif
