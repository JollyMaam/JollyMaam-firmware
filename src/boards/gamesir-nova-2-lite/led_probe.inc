/* Only included by a NOVA_LED_PROBE diagnostic build. */
static u8 led_probe_active;
#define H "[%0+0] = %1\n\t"
#define L "[%0+0] = %2\n\t"
#define WAVE(NAME, ZERO, ONE) \
static HOT void NAME(const u8 *p, u32 n) \
{ \
    volatile u32 *out = &JL_PORTB->OUT; \
    u32 lo, hi, v, mask; \
    local_irq_disable(); \
    lo = *out & ~BIT(0); \
    hi = lo | BIT(0); \
    while (n--) { \
        v = *p++; \
        for (mask = 0x80; mask; mask >>= 1) { \
            if (v & mask) { \
                __asm__ volatile(ONE :: "r"(out), "r"(hi), "r"(lo) : "memory"); \
            } else { \
                __asm__ volatile(ZERO :: "r"(out), "r"(hi), "r"(lo) : "memory"); \
            } \
        } \
    } \
    local_irq_enable(); \
}
/* Complete pulses: the compiler cannot insert instructions among writes. */
WAVE(led_probe_1, H H L L L L L L L L L L, H H H H H H H L L L L L)
WAVE(led_probe_2, H H L L L L L L L L, H H H H H H L L L L)
WAVE(led_probe_3, H H L L L L L L L L L L L L, H H H H H H H H L L L L L L)
WAVE(led_probe_4, H H H L L L L L L L L L L L, H H H H H H H H L L L L L L)
WAVE(led_probe_5, H H H L L L L L L L L L L L L L, H H H H H H H H H L L L L L L L)
WAVE(led_probe_6, H H L L L L L L L L L L L L L L, H H H H H H H H H L L L L L L L)
WAVE(led_probe_7, H H H L L L L L L L L L L L L L L L, H H H H H H H H H H L L L L L L L L)
WAVE(led_probe_8, H H H H L L L L L L L L L L L L L L L L, H H H H H H H H H H H L L L L L L L L L)
WAVE(led_probe_9, H H H H L L L L L L L L L L L L L L L L L L, H H H H H H H H H H H H L L L L L L L L L L)
WAVE(led_probe_10, H H L L L L L L L L L L L L L L L L, H H H H H H H H H H L L L L L L L L)

#undef WAVE
#undef H
#undef L

static void (*const led_probe_waves[])(const u8 *, u32) = {
    ws_send, led_probe_1, led_probe_2, led_probe_3, led_probe_4, led_probe_5, led_probe_6, led_probe_7, led_probe_8, led_probe_9, led_probe_10
};

u32 board_led_probe(u32 arg)
{
    u8 bytes[LED_CHAIN * 3] = { 0 };
    u32 wave = arg & 255, colour = arg >> 8 & 255;

    if (arg == 0xffffffff) return 0x31504c4e; /* NLP1 */
    if (arg == 0xfffffffd) return 0x31525750; /* PWR1: read-only power snapshot */
    if (arg == 0xfffffff0 || arg == 0xfffffff1) {
        u32 i, value = 0, first = (arg & 1) * 4;

        for (i = 0; i < 4; i++) value |= (u32)P33_CON_GET(P3_ANA_CON0 + first + i) << (8 * i);
        return value;
    }
    if (arg == 0xfffffff2)
        return (u32)P33_CON_GET(P3_ANA_CON8) | (u32)P33_CON_GET(P3_ANA_CON9) << 8 |
               (u32)P33_CON_GET(P3_ANA_KEEP) << 16 | (u32)P33_CON_GET(P3_VLD_KEEP) << 24;
    if (wave == 255) {
        led_probe_active = 0;
        pin_low(PIN_LED);
        ws_send(bytes, sizeof(bytes));
        led_ready = 0;
        return 1;
    }
    if (wave >= sizeof(led_probe_waves) / sizeof(led_probe_waves[0]) || colour > 4) return 0;
    led_probe_active = 1;
    pin_low(PIN_LED);
    plat_mdelay(1);                 /* a reset interval before this frame */
    if (colour == 0 || colour == 1) bytes[0] = 255;
    if (colour == 0 || colour == 2) bytes[1] = 255;
    if (colour == 0 || colour == 3) bytes[2] = 255;
    led_probe_waves[wave](bytes, sizeof(bytes));
    return 1;
}
