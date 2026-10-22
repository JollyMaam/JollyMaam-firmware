#include <system/includes.h>
#include <asm/adc_api.h>
#include <asm/power/power_port.h>
#include "app_config.h"
#include "board.h"

/* Pin probe, until the Nova 2 Lite's pins are known.  It logs (padtool log)
 * what each pin of ports A-C does while the user presses the buttons:
 *
 *   - every tick, all probed pins are read pulled down, then pulled up:
 *     "low" (held low from outside), "high", or "open" (follows the pull).
 *     A button to ground turns an open pin low; one to 3.3 V, high.
 *   - one pin per tick is also driven, low with the others pulled up, then
 *     high with them pulled down: a pin that follows it is linked to it.
 *     A key matrix shows as links that come and go with a key.
 *   - the six ADC pins of the stock image's scan table are left alone and
 *     their values logged when they move.
 *
 * A pin held low from outside is never driven high, one held high never
 * driven low.  The SPI flash pins and port D are never touched.  Between
 * samples the probed pins are inputs pulled down. */

#define NPORT 3                          /* A, B, C */
#define SETTLE 64                        /* register reads between a change and the sample */
#define STABLE_ROUNDS 2                  /* rounds a new state must last to be logged */
#define ADC_STEP 150                     /* of 1023: PA1 jumps ~100 at rest */
#define ADC_EVERY_MS 50

typedef unsigned long long u64;

static JL_PORT_FLASH_TypeDef *const port[NPORT] = { JL_PORTA, JL_PORTB, JL_PORTC };

static const u8 adc_gpio[] = { IO_PORTA_01, IO_PORTB_08, IO_PORTC_04, IO_PORTC_05, IO_PORTB_03, IO_PORTA_10 };
static const u32 adc_ch[] = { AD_CH_PA1, AD_CH_PB8, AD_CH_PC4, AD_CH_PC5, AD_CH_PB3, AD_CH_PA10 };
#define NADC (sizeof(adc_ch) / sizeof(adc_ch[0]))

struct state {
    u64 lo, hi;                          /* read pulled down, pulled up */
    u64 link[48];                        /* pins that followed pin i */
};

static u16 cand[NPORT];                  /* the probed pins */
static u8 order[48], count;
static u8 next;

static struct state work, last, shown;
static u8 same_rounds;
static volatile u32 published;           /* rounds published to the control thread */
static u32 seen;
static struct state pub;
static u32 adc_ms;
static u16 adc_logged[NADC];
static u8 adc_far[NADC];

static HOT u64 read_all(void)
{
    return (u64)(JL_PORTA->IN & cand[0]) | (u64)(JL_PORTB->IN & cand[1]) << 16 |
           (u64)(JL_PORTC->IN & cand[2]) << 32;
}

static HOT void settle(void)
{
    u32 i;

    for (i = 0; i < SETTLE; i++) (void)JL_PORTA->IN;
}

static HOT void pulls(int up)
{
    u32 i;

    for (i = 0; i < NPORT; i++) {
        if (up) {
            port[i]->PD &= ~cand[i];
            port[i]->PU |= cand[i];
        } else {
            port[i]->PU &= ~cand[i];
            port[i]->PD |= cand[i];
        }
    }
}

void probe_start(void)
{
    u32 i, g;

    for (i = 0; i < NPORT; i++) cand[i] = 0xffff;
    for (i = 0; i < NADC; i++) cand[adc_gpio[i] / 16] &= ~BIT(adc_gpio[i] % 16);
    /* SPI flash: port D always, and its group's pins on A or B; WP and
     * HOLD only in 4-bit mode */
    if (spi_get_port() == 0) {
        if (get_sfc_bit_mode() == 4) cand[1] &= ~BIT(7);
    } else {
        cand[0] &= ~(BIT(13) | BIT(14));
        if (get_sfc_bit_mode() == 4) cand[0] &= ~BIT(15);
    }

    for (i = 0; i < NPORT; i++) {
        port[i]->OUT &= ~cand[i];
        port[i]->DIR |= cand[i];
        port[i]->DIE |= cand[i];
    }
    pulls(0);
    for (g = 0; g < 48; g++)
        if (cand[g / 16] & BIT(g % 16)) order[count++] = (u8)g;

    for (i = 0; i < NADC; i++) {
        gpio_set_pull_up(adc_gpio[i], 0);
        gpio_set_pull_down(adc_gpio[i], 0);
        gpio_set_die(adc_gpio[i], 0);
        gpio_direction_input(adc_gpio[i]);
        adc_add_sample_ch(adc_ch[i]);
    }
    util_log("probe: %u pins, A %04x B %04x C %04x, flash port %u, %u-bit", count, cand[0],
             cand[1], cand[2], spi_get_port(), get_sfc_bit_mode());
}

HOT void probe_tick(void)
{
    u32 g = order[next];
    JL_PORT_FLASH_TypeDef *p = port[g / 16];
    u32 b = BIT(g % 16);
    u64 me = (u64)1 << g;
    u64 lo, hi, drove_lo, drove_hi;

    if (!count) return;
    lo = read_all();
    pulls(1);
    settle();
    hi = read_all();
    /* Drive it low against the pull-ups, unless something holds it high,
     * then high against the pull-downs, unless something holds it low. */
    drove_lo = hi;
    if (!(lo & me)) {
        p->OUT &= ~b;
        p->DIR &= ~b;
        settle();
        drove_lo = read_all();
        p->DIR |= b;
    }
    pulls(0);
    drove_hi = lo;
    if (hi & me) {
        p->OUT |= b;
        p->DIR &= ~b;
        settle();
        drove_hi = read_all();
        p->OUT &= ~b;
        p->DIR |= b;
    }

    work.lo = lo;
    work.hi = hi;
    work.link[g] = ((hi & ~drove_lo) | (drove_hi & ~lo)) & ~me;

    if (++next < count) return;
    next = 0;
    if (memcmp(&work, &last, sizeof(work))) {
        last = work;
        same_rounds = 0;
    } else if (same_rounds < STABLE_ROUNDS && ++same_rounds == STABLE_ROUNDS &&
               memcmp(&last, &pub, sizeof(last))) {
        pub = last;
        published++;
        plat_raise(EV_BUTTONS);
    }
    if ((s32)(plat_now_ms - adc_ms) >= ADC_EVERY_MS) {
        adc_ms = plat_now_ms;
        plat_raise(EV_BUTTONS);
    }
}

static const char *pin_name(u32 g, char *buf)
{
    buf[0] = 'P';
    buf[1] = (char)('A' + g / 16);
    buf[2] = (char)('0' + (g % 16) / 10);
    buf[3] = (char)('0' + g % 16 % 10);
    buf[4] = 0;
    return buf;
}

static const char *pin_class(const struct state *s, u32 g)
{
    u64 m = (u64)1 << g;
    int lo = !!(s->lo & m), hi = !!(s->hi & m);

    return lo ? (hi ? "high" : "odd") : (hi ? "open" : "low");
}

static int linked(const struct state *s, u32 a, u32 b)
{
    return !!(s->link[a] & ((u64)1 << b)) || !!(s->link[b] & ((u64)1 << a));
}

static void report_pins(const struct state *now, int first)
{
    char na[5], nb[5];
    u32 i, j;

    for (i = 0; i < count; i++) {
        u32 g = order[i];
        const char *c = pin_class(now, g);

        if (first ? strcmp(c, "open") : strcmp(c, pin_class(&shown, g)))
            util_log("%s %s%s%s", pin_name(g, na), first ? "" : pin_class(&shown, g),
                     first ? "" : " -> ", c);
    }
    for (i = 0; i < count; i++)
        for (j = i + 1; j < count; j++) {
            u32 a = order[i], b = order[j];
            int l = linked(now, a, b);

            if (first ? l : l != linked(&shown, a, b))
                util_log("link %s-%s %s", pin_name(a, na), pin_name(b, nb), l ? "on" : "off");
        }
}

void probe_report(void)
{
    u32 n = published;
    u32 i;

    if (n != seen) {
        static struct state now;

        plat_lock();
        now = pub;
        plat_unlock();
        report_pins(&now, seen == 0);
        shown = now;
        seen = n;
    }
    for (i = 0; i < NADC; i++) {
        u16 v = (u16)adc_get_value(adc_ch[i]);
        char na[5];

        int far = v > adc_logged[i] + ADC_STEP || v + ADC_STEP < adc_logged[i];

        /* a move must hold for two reads (ADC_EVERY_MS apart) */
        adc_far[i] = far ? adc_far[i] + 1 : 0;
        if (adc_far[i] >= 2 || !adc_logged[i]) {
            adc_far[i] = 0;
            adc_logged[i] = v ? v : 1;
            util_log("adc %s %u", pin_name(adc_gpio[i], na), v);
        }
    }
}
