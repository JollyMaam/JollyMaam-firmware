/* The SDK's Classic clock arithmetic (bredr_utils.c).  Clocks are signed
 * ints that wrap at 2^27 (the SDK's constant 134217728, not 2^28), and
 * "later than" is decided by a window of 69120000, which is larger than half
 * the wrap (67108864); both quirks are kept as the SDK has them.  Sums are
 * done unsigned so overflow wraps instead of being undefined. */
#include "btctrler/glue.h"

#define CLKN_WRAP   134217728
#define CLKN_WINDOW 69120000

/* Forward distance from b to a, within the wrap. */
static int distance(int a, int b)
{
    return (int)((a > b ? (u32)a : (u32)a + CLKN_WRAP) - (u32)b);
}

/* 1 if a is less than a window after b. */
int bredr_clkn_after(int clkn_a, int clkn_b)
{
    return distance(clkn_a, clkn_b) < CLKN_WINDOW;
}

/* 1 if a is less than a window before b (b after a, as above). */
int bredr_clkn_before(int clkn_a, int clkn_b)
{
    return distance(clkn_b, clkn_a) < CLKN_WINDOW;
}

/* end - begin; when begin is more than a window past end, end is taken to
 * have wrapped and the wrap is added. */
int bredr_clkn2offset(int clkn_begin, int clkn_end)
{
    u32 offset = (u32)clkn_end - (u32)clkn_begin;
    if (clkn_begin > clkn_end && clkn_begin - clkn_end > CLKN_WINDOW)
        offset += CLKN_WRAP;
    return (int)offset;
}

/* begin + offset, with one wrap subtracted if that reaches 2^27 (no mask:
 * negative or very large offsets are not brought into range). */
int bredr_offset2clkn(int clkn_begin, int offset)
{
    u32 sum = (u32)offset + (u32)clkn_begin;
    return (int)((int)sum > CLKN_WRAP - 1 ? sum - CLKN_WRAP : sum);
}

#ifdef UNITTEST
#include "test.h"
int main(void)
{
    const int W = 134217728, T = 69120000;
    /* after: a > b is the plain difference a-b; else a+W-b.  Both must be
     * under T.  Equal clocks take the wrapped branch: W >= T, so 0. */
    CHECK(bredr_clkn_after(10, 5));
    CHECK(!bredr_clkn_after(5, 5));
    CHECK(bredr_clkn_after(T + 4, 5));              /* difference T-1 */
    CHECK(!bredr_clkn_after(T + 5, 5));             /* difference T */
    CHECK(bredr_clkn_after(3, W - 10));             /* wrapped: 3+W-(W-10)=13 */
    CHECK(!bredr_clkn_after(5, 10));                /* 5+W-10 is huge */
    /* before: the same with the operands swapped. */
    CHECK(bredr_clkn_before(5, 10));
    CHECK(!bredr_clkn_before(5, 5));
    CHECK(bredr_clkn_before(5, T + 4));
    CHECK(!bredr_clkn_before(5, T + 5));
    CHECK(bredr_clkn_before(W - 10, 3));
    CHECK(!bredr_clkn_before(10, 5));
    /* clkn2offset: plain end-begin unless begin>end by more than T. */
    CHECK(bredr_clkn2offset(5, 10) == 5);
    CHECK(bredr_clkn2offset(10, 5) == -5);
    CHECK(bredr_clkn2offset(7, 7) == 0);
    CHECK(bredr_clkn2offset(T + 5, 5) == -T);       /* difference T: no wrap */
    CHECK(bredr_clkn2offset(T + 6, 5) == 5 - (T + 6) + W);
    CHECK(bredr_clkn2offset(W - 10, 3) == 13);
    CHECK(bredr_clkn2offset(0, W - 1) == W - 1);    /* begin<end: never wraps */
    /* offset2clkn: sum, minus W when it exceeds W-1 only once. */
    CHECK(bredr_offset2clkn(100, 20) == 120);
    CHECK(bredr_offset2clkn(W - 1, 0) == W - 1);
    CHECK(bredr_offset2clkn(W - 1, 1) == 0);
    CHECK(bredr_offset2clkn(W - 10, 13) == 3);
    CHECK(bredr_offset2clkn(5, -10) == -5);         /* negative is not wrapped */
    CHECK(bredr_offset2clkn(W - 1, W - 1) == W - 2);
    CHECK(bredr_offset2clkn(W - 1, 2 * W) == 2 * W - 1); /* one wrap only */
    return TEST_DONE("btctrler_bredr_utils");
}
#endif
