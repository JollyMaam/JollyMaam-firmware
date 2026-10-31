#include "yc3121.h"

/* 8BitDo Ultimate 2C Bluetooth (YC3121) bring-up: the first own code on the
 * pad.  The four player lights run a chase; while a button is down they all
 * light (Home: the outer two).  Any button held about 3 s lets go of the
 * power hold, then resets the chip (on USB power the pad stays on, and the
 * boot loader starts this again unless L1+R1 are held).
 *
 * Pins as stock v1.01 drives them, each proven here on 2026-10-31:
 *   power hold  0.7 high keeps the pad on (stock drops it to switch off)
 *   lights      1.11, 1.6, 0.15, 2.3, high = lit (its player-light chase)
 *   matrix      rows 1.5, 0.6, 0.1, 2.4 driven low in turn; columns
 *               0.9..0.13 with pull-ups, low = down
 *   Home        1.10 with pull-up, low = down; 2.11 and 2.12 likewise
 * The loader leaves the CPU at 48 MHz (HCLK_CON divider 2, seen on the
 * lights); a busy loop runs about five times slower than its cycle count
 * from flash, so delays count SysTick, which runs on the CPU clock. */

struct pin { uint8_t group, pin; };

static const struct pin lights[4] = { { 1, 11 }, { 1, 6 }, { 0, 15 }, { 2, 3 } };
static const struct pin rows[4] = { { 1, 5 }, { 0, 6 }, { 0, 1 }, { 2, 4 } };
static const struct pin keys[3] = { { 1, 10 }, { 2, 11 }, { 2, 12 } };   /* Home first */
#define COLUMNS_MASK 0x3e00             /* 0.9..0.13 */
#define POWER_GROUP 0
#define POWER_PIN 7

static void delay_us(uint32_t us)
{
    while (us) {
        uint32_t step = us < 100000 ? us : 100000;      /* within SysTick's 24 bits */
        uint32_t start = YC_SYST_CVR, cycles = step * YC_CPU_MHZ;
        while (((start - YC_SYST_CVR) & 0xffffff) < cycles)
            ;
        us -= step;
    }
}

void yc_reset(void)
{
    YC_RST_EN |= 1;
    YC_RESET = 0x55;
    for (;;)
        ;
}

static void show(unsigned mask)
{
    for (int i = 0; i < 4; i++)
        yc_pin(lights[i].group, lights[i].pin, mask >> i & 1 ? YC_PIN_HIGH : YC_PIN_LOW);
}

/* 0 none, 1 Home, 2 anything else. */
static int scan(void)
{
    int down = 0;
    for (int r = 0; r < 4; r++) {
        yc_pin(rows[r].group, rows[r].pin, YC_PIN_LOW);
        delay_us(5);
        if (~YC_GPIO_IN(0) & COLUMNS_MASK)
            down = 2;
        yc_pin(rows[r].group, rows[r].pin, YC_PIN_HIGH);
    }
    for (int k = 2; k > 0; k--)
        if (!yc_pin_in(keys[k].group, keys[k].pin))
            down = 2;
    if (!down && !yc_pin_in(keys[0].group, keys[0].pin))
        down = 1;
    return down;
}

int main(void)
{
    YC_SYST_RVR = 0xffffff;
    YC_SYST_CSR = 5;                    /* on, CPU clock, no interrupt */
    yc_pin(POWER_GROUP, POWER_PIN, YC_PIN_HIGH);
    show(0);
    for (int r = 0; r < 4; r++)
        yc_pin(rows[r].group, rows[r].pin, YC_PIN_HIGH);
    for (int c = 9; c <= 13; c++)
        yc_pin(0, c, YC_PIN_PULLUP);
    for (int k = 0; k < 3; k++)
        yc_pin(keys[k].group, keys[k].pin, YC_PIN_PULLUP);

    unsigned tick = 0, held = 0;
    for (;; tick++) {                   /* 10 ms a pass */
        int down = scan();
        held = down ? held + 1 : 0;
        if (held >= 300)
            break;
        if (down)
            show(down == 1 ? 0x9 : 0xf);
        else
            show(1u << (tick / 25 % 4));
        delay_us(10000);
    }
    show(0);
    yc_pin(POWER_GROUP, POWER_PIN, YC_PIN_LOW);
    delay_us(1000000);
    yc_reset();
}
