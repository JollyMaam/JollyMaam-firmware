#include <string.h>
#include "pad.h"
#include "yc3121.h"
#include "board.h"

/* 8BitDo Ultimate 2C Bluetooth: a Yichip YC3121 (Cortex-M0).  Pins as stock
 * v1.01 sets them (its GPIO set-up, 0x0101c9e0), each proven on the pad
 * with our firmware unless said otherwise (the analysis notes):
 *   power hold  0.7 high keeps the pad on; on battery it starts only while
 *               Home is down
 *   lights      1.11, 1.6, 0.15, 2.3, high = lit (the four player lights)
 *   matrix      rows 1.5, 0.6, 0.1, 2.4 driven low in turn; columns
 *               0.9..0.13 pulled up, low = down
 *   own pins    Home 1.10, L2 2.11, R2 2.12, pulled up, low = down
 *   sticks      ADC channels 4 (left X), 5 (left Y), 0 (right X), 1 (right
 *               Y): rest near 1390 of 0..2046, about +-270 travel
 *   battery     ADC channel 3 (scale not established: see BATTERY_*)
 *   USB power   0.14, high = present; 1.12 low while charging (likely)
 *   motors      1.2 PWM 1, 1.1 PWM 2 (stock's; which side each is, and its
 *               PWM's polarity, not established)
 * The ADC is the radio core's: no sticks until it is up. */

const char board_name[] = "Ultimate 2C Bluetooth";

#include "settings_defaults.inc"

struct pin { u8 group, pin; };

#define POWER_HOLD  ((struct pin){ 0, 7 })
#define USB_POWER   ((struct pin){ 0, 14 })
#define CHARGING    ((struct pin){ 1, 12 })

static void out(struct pin p, int high)
{
    yc_pin(p.group, p.pin, high ? YC_PIN_HIGH : YC_PIN_LOW);
}

static int in(struct pin p)
{
    return yc_pin_in(p.group, p.pin);
}

/* ---- the key matrix and the buttons on their own pins -------------------- */

#define ROWS 4
#define COLUMNS 5
#define COLUMNS_MASK 0x3e00          /* 0.9..0.13 */
#define ROW_SETTLE_US 5
#define SCAN_GAP_US 1000             /* from one full scan to the next */
#define DEBOUNCE_SCANS 2             /* a release: up this many scans in a row */

static const struct pin rows[ROWS] = { { 1, 5 }, { 0, 6 }, { 0, 1 }, { 2, 4 } };
/* Each position's PAD_*, as the U2CW's matrix has them (Switch names on the
 * buttons: B south is PAD_A); Star is PAD_M. */
static const u32 keys[ROWS][COLUMNS] = {
    { PAD_UP, PAD_Y, PAD_START, PAD_R4, PAD_L4 },
    { PAD_LEFT, PAD_X, PAD_M, PAD_R1, PAD_L1 },
    { PAD_DOWN, PAD_B, PAD_SELECT, PAD_R3, PAD_L3 },
    { PAD_RIGHT, PAD_A, PAD_SCREENSHOT, 0, 0 },
};
static const struct pin home = { 1, 10 }, l2 = { 2, 11 }, r2 = { 2, 12 };

static struct {
    u32 scanned_us;
    u32 stable;                      /* debounced: PAD_*, with L2 and R2 here */
    u32 changed_us;
    u8 same[32];                     /* scans a bit has read other than stable */
} mx;

#define L2_DOWN (1u << 30)
#define R2_DOWN (1u << 31)

/* Everything down now, one scan. */
static u32 scan(void)
{
    u32 down = 0, r, c, columns;

    for (r = 0; r < ROWS; r++) {
        out(rows[r], 0);
        yc_delay_us(ROW_SETTLE_US);
        columns = (u32)(~YC_GPIO_IN(0) & COLUMNS_MASK) >> 9;
        out(rows[r], 1);
        for (c = 0; c < COLUMNS; c++)
            if (columns >> c & 1) down |= keys[r][c];
    }
    if (!in(home)) down |= PAD_HOME;
    if (!in(l2)) down |= L2_DOWN;
    if (!in(r2)) down |= R2_DOWN;
    return down;
}

u32 board_held(void)
{
    return scan() & ~(L2_DOWN | R2_DOWN);
}

/* A press counts at once; a release after DEBOUNCE_SCANS scans up. */
static u32 matrix_poll(void)
{
    u32 now = plat_us(), down, changed = 0, i;

    if (now - mx.scanned_us < SCAN_GAP_US) return 0;
    mx.scanned_us = now;
    down = scan();
    for (i = 0; i < 32; i++) {
        u32 bit = 1u << i;

        if ((down ^ mx.stable) & bit) {
            if ((down & bit) || ++mx.same[i] >= DEBOUNCE_SCANS) {
                mx.stable ^= bit;
                mx.same[i] = 0;
                changed = 1;
            }
        } else {
            mx.same[i] = 0;
        }
    }
    if (!changed) return 0;
    mx.changed_us = now;
    return EV_BUTTONS;
}

/* ---- the sticks and the battery, on the radio core's ADC ------------------- */

#define STICK_EVERY_US 4000
#define BATTERY_EVERY_MS 1000
#define STICK_REST 1395              /* a rest outside REST_SPREAD of it is not taken */
#define REST_SPREAD 120
#define STICK_SCALE 36               /* +-270 codes to about the core's +-0x2600 */
#define MOVED 3                      /* codes an axis moves to wake the loop */
/* Battery: stock scales its calibrated channel-3 reading by 3.7; with the
 * ADC's code near 0.61 mV, about 2.26 mV a code (not established). */
#define BATTERY_UV_PER_CODE 2265
#define BATTERY_EMPTY_MV 3500
#define BATTERY_FULL_MV 4100
#define BATTERY_LOW_MV 3500
#define BATTERY_OKAY_MV 3600

static const u8 stick_channel[4] = { 4, 5, 0, 1 };   /* LX LY RX RY */
/* The core's raw directions: the left stick growing right and down, the
 * right one left and up.  Codes here fall to the right on both X, fall
 * downward on left Y and rise downward on right Y. */
static const s8 stick_sign[4] = { -1, -1, 1, -1 };

static struct {
    u32 stick_us, battery_ms;
    u16 code[4], rest[4];
    u8 valid;
    u16 battery;
} ad;

static u32 sticks_poll(void)
{
    u32 now = plat_us(), moved = 0, i;

    if (!plat_adc_ready() || now - ad.stick_us < STICK_EVERY_US) return 0;
    ad.stick_us = now;
    for (i = 0; i < 4; i++) {
        u16 c = plat_adc_read(stick_channel[i]);

        if (!ad.valid)                   /* the first reading: the rest */
            ad.rest[i] = c > STICK_REST - REST_SPREAD && c < STICK_REST + REST_SPREAD ? c : STICK_REST;
        if (c > ad.code[i] + MOVED || c + MOVED < ad.code[i]) moved = 1;
        ad.code[i] = c;
    }
    ad.valid = 1;
    if ((u32)plat_ms() - ad.battery_ms >= BATTERY_EVERY_MS || !ad.battery) {
        ad.battery_ms = plat_ms();
        ad.battery = plat_adc_read(3);
    }
    return moved ? EV_STICKS : 0;
}

u32 board_poll(void)
{
    return matrix_poll() | sticks_poll();
}

void board_input(struct in_raw *raw)
{
    u32 i;

    memset(raw, 0, sizeof(*raw));
    raw->buttons = mx.stable & ~(L2_DOWN | R2_DOWN);
    raw->changed_us = mx.changed_us;
    raw->sticks_valid = ad.valid;
    for (i = 0; i < 4; i++) {
        s32 v = 0x4000 + stick_sign[i] * ((s32)ad.code[i] - ad.rest[i]) * STICK_SCALE;

        raw->stick[i] = (u16)(v < 0 ? 0 : v > 0x7fff ? 0x7fff : v);
    }
    /* Digital triggers as the core's uncalibrated ones: 10 bits, falling
     * as they are pressed. */
    raw->trigger[0] = mx.stable & L2_DOWN ? 0 : 0x3ff;
    raw->trigger[1] = mx.stable & R2_DOWN ? 0 : 0x3ff;
}

void board_battery(struct battery_status *out_status)
{
    u16 mv = (u16)((u32)ad.battery * BATTERY_UV_PER_CODE / 1000);

    out_status->flags = 0;
    battery_voltage(out_status, mv, BATTERY_EMPTY_MV, BATTERY_FULL_MV);
    battery_low(out_status, BATTERY_LOW_MV, BATTERY_OKAY_MV);
    battery_charge(out_status, board_usb_present(), !in(CHARGING));
}

int board_usb_present(void)
{
    return in(USB_POWER);
}

/* ---- the lights: on or off, from the core's player lights ----------------- */

static const struct pin lights[4] = { { 1, 11 }, { 1, 6 }, { 0, 15 }, { 2, 3 } };

void plat_leds(const struct led_frame *f)
{
    u32 i;

    for (i = 0; i < 4; i++) {
        const struct rgb *c = &f->light[LIGHT_P0 + i];

        out(lights[i], c->r || c->g || c->b);
    }
}

/* ---- the motors: PWM timers 1 and 2 ----------------------------------------
 * Yichip's timers (0xf0c00): timer n's high count at 8n, low count at 8n + 4,
 * the control word at 0x48 with 4 bits a timer (bit 0 on).  Stock runs them
 * at 100 Hz on the CPU clock. */

#define TIM_HIGH(n) (*(volatile u32 *)(0xf0c00 + 8 * (n)))
#define TIM_LOW(n)  (*(volatile u32 *)(0xf0c04 + 8 * (n)))
#define TIM_CTRL    (*(volatile u32 *)0xf0c48)
#define MOTOR_HZ    100
#define GPCFG_PWM(n) (16 + (n))

static void motor_set(u32 timer, u8 duty)
{
    u32 unit = YC_CPU_MHZ * 1000000u / MOTOR_HZ / 255;

    TIM_CTRL &= ~(0xfu << 4 * timer);
    if (!duty) return;
    TIM_HIGH(timer) = unit * duty;
    TIM_LOW(timer) = unit * (255u - duty);
    TIM_CTRL |= 1u << 4 * timer;
}

void plat_motors(u8 left, u8 right)
{
    motor_set(1, left);
    motor_set(2, right);
}

/* ---- power ------------------------------------------------------------------ */

void board_init(void)
{
    u32 i;

    out(POWER_HOLD, 1);
    for (i = 0; i < 4; i++) out(lights[i], 0), out(rows[i], 1);
    for (i = 9; i <= 13; i++) yc_pin(0, (int)i, YC_PIN_PULLUP);
    yc_pin(home.group, home.pin, YC_PIN_PULLUP);
    yc_pin(l2.group, l2.pin, YC_PIN_PULLUP);
    yc_pin(r2.group, r2.pin, YC_PIN_PULLUP);
    yc_pin(USB_POWER.group, USB_POWER.pin, 0);
    yc_pin(CHARGING.group, CHARGING.pin, 0);
    plat_motors(0, 0);
    yc_pin(1, 2, GPCFG_PWM(1));
    yc_pin(1, 1, GPCFG_PWM(2));
}

/* Lights and motors off, Home let go, then the power hold released: on
 * battery the pad is off.  USB power keeps it on: then a restart. */
void board_power_off(void)
{
    u32 i;

    plat_motors(0, 0);
    for (i = 0; i < 4; i++) out(lights[i], 0);
    while (!in(home))
        if (board_usb_present()) plat_reboot();
    out(POWER_HOLD, 0);
    plat_delay_ms(500);
    plat_reboot();
}

/* No motion sensor yet (stock's is on I2C: not established which). */
u32 imu_poll(u32 now, u8 mode)
{
    (void)now;
    (void)mode;
    return 0;
}
