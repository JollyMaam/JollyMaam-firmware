#include <string.h>
#include "pad.h"
#include "yc3121.h"
#include "board.h"
#include "i2c.h"

/* 8BitDo Ultimate 2C Bluetooth: a Yichip YC3121 (Cortex-M0).  Pins as stock
 * v1.01 sets them (its GPIO set-up, 0x0101c9e0), each proven on the pad
 * with our firmware unless said otherwise (the analysis notes):
 *   power hold  0.7 high keeps the pad on; on battery it starts only while
 *               Home is down
 *   lights      1.11, 1.6, 0.15, 2.3, high = lit (the four player lights);
 *               Home 0.5, below Home 0.4, Sync (red) 2.2, high = lit
 *   Sync        the matrix's row 3, column 3
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

/* ---- the pad: its controls and defaults ------------------------------------
 * How this pad is set up, in one place; the rest of the file wires the
 * hardware to these inputs. */

const char board_name[] = "Ultimate 2C Bluetooth";
const char board_short_name[] = "U2C BT";

/* Nintendo order: B south, A east, Y west, X north.  The others as the
 * U2CW's (L4 and R4 on top); M is the Star button; L2 and R2 are digital.
 * Only the face buttons' labels are established.  Not placed yet. */
const struct board_control board_layout[] = {
    /* input         kind                 label      x  y */
    { IN_SOUTH,      LAYOUT_FACE,         "B",       0, 0 },
    { IN_EAST,       LAYOUT_FACE,         "A",       0, 0 },
    { IN_WEST,       LAYOUT_FACE,         "Y",       0, 0 },
    { IN_NORTH,      LAYOUT_FACE,         "X",       0, 0 },
    { IN_UP,         LAYOUT_DPAD,         "",        0, 0 },
    { IN_DOWN,       LAYOUT_DPAD,         "",        0, 0 },
    { IN_LEFT,       LAYOUT_DPAD,         "",        0, 0 },
    { IN_RIGHT,      LAYOUT_DPAD,         "",        0, 0 },
    { IN_LSTICK,     LAYOUT_STICK,        "",        0, 0 },
    { IN_L3,         LAYOUT_STICK_CLICK,  "",        0, 0 },
    { IN_RSTICK,     LAYOUT_STICK,        "",        0, 0 },
    { IN_R3,         LAYOUT_STICK_CLICK,  "",        0, 0 },
    { IN_L2,         LAYOUT_TRIGGER_DIGITAL, "",      0, 0 },
    { IN_R2,         LAYOUT_TRIGGER_DIGITAL, "",      0, 0 },
    { IN_L1,         LAYOUT_SHOULDER,     "",        0, 0 },
    { IN_R1,         LAYOUT_SHOULDER,     "",        0, 0 },
    { IN_L4,         LAYOUT_TOP,          "",        0, 0 },
    { IN_R4,         LAYOUT_TOP,          "",        0, 0 },
    { IN_SELECT,     LAYOUT_MENU,         "",        0, 0 },
    { IN_START,      LAYOUT_MENU,         "",        0, 0 },
    { IN_HOME,       LAYOUT_MENU,         "",        0, 0 },
    { IN_SCREENSHOT, LAYOUT_MENU,         "",        0, 0 },
    { IN_M,          LAYOUT_MENU,         "",        0, 0 },
};
const u8 board_layout_count = LAYOUT_COUNT(board_layout);

/* The base remap the pad starts with: every key sends itself. */
const struct set_remap board_remap_base = { .name = "" };

/* Home is dimmed (PWM, plat_leds), the others are on or off: the four
 * player lights white, so the host's player number shows alone, and dark
 * while connecting; the cable and pairing signs on the lights below Home
 * and beside it (red), as the U2CW's. */
const struct board_led_brightness board_led_brightness = {
    .steady = 240, .animation = 400, .connect = 240,
    .idle = 240, .battery = 240, .players_mono = 1, .home_signs = 1,
    .players_dark_connecting = 1,
};

/* As the U2CW's motors until these are measured. */
const struct board_rumble board_rumble = {
    .min = { { 32, 32, 32, 45, 55 }, { 32, 32, 32, 45, 55 } },
    .max = { { 65, 85, 100, 100, 100 }, { 65, 85, 100, 100, 100 } },
    .kick_ms = { 0, 0, 0, 0, 0 },
};

/* Classic Bluetooth (the radio core's HID device) and USB (HID and Xbox 360
 * personalities; no Switch 2 one): Switch Pro, generic HID, DualShock 4,
 * DualSense, Xbox; each slot USB while a host is there, else Bluetooth. */
const struct board_slots board_slots = {
    .type = { PROFILE_SWITCH, PROFILE_GENERIC, PROFILE_DS4, PROFILE_DS5,   /* A B X Y */
              PROFILE_NONE, PROFILE_NONE, PROFILE_NONE, PROFILE_NONE },
    .transport = { TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_BT_USB,
                   TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_BT_USB },
    .types = 1u << PROFILE_SWITCH | 1u << PROFILE_GENERIC | 1u << PROFILE_DS4 |
             1u << PROFILE_DS5 | 1u << PROFILE_MOUSE | 1u << PROFILE_XBOX | 1u << PROFILE_SWITCH2 |
             1u << PROFILE_NONE,
    .transports = 1u << TRANSPORT_BT | 1u << TRANSPORT_USB | 1u << TRANSPORT_BT_USB,
};

/* ---- the wiring ------------------------------------------------------------ */

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
/* Each position's PAD_*, as the U2CW's matrix has them; the buttons are in
 * Nintendo order (board_layout above: B south).  Star is PAD_M. */
static const u32 keys[ROWS][COLUMNS] = {
    { PAD_UP, PAD_NORTH, PAD_START, PAD_R4, PAD_L4 },
    { PAD_LEFT, PAD_WEST, PAD_M, PAD_R1, PAD_L1 },
    { PAD_DOWN, PAD_EAST, PAD_SELECT, PAD_R3, PAD_L3 },
    { PAD_RIGHT, PAD_SOUTH, PAD_SCREENSHOT, 0, 0 },     /* 3, 3: Sync (SYNC_DOWN) */
};
static const struct pin home = { 1, 10 }, l2 = { 2, 11 }, r2 = { 2, 12 };

static struct {
    u32 scanned_us;
    u32 stable;                      /* debounced: PAD_*, with L2 and R2 here */
    u32 changed_us;
    u8 same[32];                     /* scans a bit has read other than stable */
} mx;

#define SYNC_DOWN (1u << 29)
#define L2_DOWN (1u << 30)
#define R2_DOWN (1u << 31)
#define OWN (SYNC_DOWN | L2_DOWN | R2_DOWN)   /* not PAD_* */

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
            if (columns >> c & 1) down |= r == 3 && c == 3 ? SYNC_DOWN : keys[r][c];
    }
    if (!in(home)) down |= PAD_HOME;
    if (!in(l2)) down |= L2_DOWN;
    if (!in(r2)) down |= R2_DOWN;
    return down;
}

u32 board_held(void)
{
    return scan() & ~OWN;
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

static u32 i2c_poll(void);


u32 board_poll(void)
{
    return matrix_poll() | sticks_poll() | i2c_poll();
}

void board_input(struct in_raw *raw)
{
    u32 i;

    memset(raw, 0, sizeof(*raw));
    raw->buttons = mx.stable & ~OWN;
    raw->pair_button = (mx.stable & SYNC_DOWN) != 0;
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

/* ---- PWM: Yichip's timers -------------------------------------------------
 * Timers at 0xf0c00: timer n's high count at 8n, low count at 8n + 4, the
 * control word at 0x48 with 4 bits a timer (bit 0 on), counting the CPU
 * clock; a pin set to GPCFG_PWM(n) follows timer n.  Stock runs the motors
 * on timers 1 and 2 at 100 Hz; the Home light takes timer 3. */

#define TIM_HIGH(n) (*(volatile u32 *)(0xf0c00 + 8 * (n)))
#define TIM_LOW(n)  (*(volatile u32 *)(0xf0c04 + 8 * (n)))
#define TIM_CTRL    (*(volatile u32 *)0xf0c48)
#define GPCFG_PWM(n) (16 + (n))

/* Duty in 255ths at hz; 0 stops the timer. */
static void pwm_set(u32 timer, u32 hz, u8 duty)
{
    u32 unit = YC_CPU_MHZ * 1000000u / hz / 255;

    TIM_CTRL &= ~(0xfu << 4 * timer);
    if (!duty) return;
    TIM_HIGH(timer) = unit * duty;
    TIM_LOW(timer) = unit * (255u - duty);
    TIM_CTRL |= 1u << 4 * timer;
}

/* ---- the lights ---------------------------------------------------------------
 * Home dimmed by PWM to the core's Home light (its brightest colour), off
 * and fully on as plain pin levels.  The others on or off: the player lights
 * from the core's, those at least half as bright as the brightest lit: a
 * gliding light (the pairing sweep) with soft edges shows as one, a blink
 * or the host's number as they are.  The below-Home and red Sync lights
 * from the core's cable and pairing lights (home_signs), as the U2CW's
 * controller_lite.c drives its three. */

static const struct pin lights[4] = { { 1, 11 }, { 1, 6 }, { 0, 15 }, { 2, 3 } };
static const struct pin home_light = { 0, 5 }, below_home = { 0, 4 }, sync_light = { 2, 2 };
#define HOME_TIMER  3
#define HOME_HZ     1000
static u8 home_duty;

static u8 brightest(const struct rgb *c)
{
    return c->r > c->g ? (c->r > c->b ? c->r : c->b) : (c->g > c->b ? c->g : c->b);
}

/* Timer writes only on a change, so a steady level keeps its cycle whole. */
static void home_set(u8 duty)
{
    if (duty == home_duty) return;
    home_duty = duty;
    if (!duty || duty == 255) {
        pwm_set(HOME_TIMER, HOME_HZ, 0);
        out(home_light, duty);
        return;
    }
    pwm_set(HOME_TIMER, HOME_HZ, duty);
    yc_pin(home_light.group, home_light.pin, GPCFG_PWM(HOME_TIMER));
}

void plat_leds(const struct led_frame *f)
{
    u8 level[4], top = 0;
    u32 i;

    for (i = 0; i < 4; i++) {
        const struct rgb *c = &f->light[LIGHT_P0 + i];

        level[i] = brightest(c);
        if (level[i] > top) top = level[i];
    }
    for (i = 0; i < 4; i++) out(lights[i], level[i] && level[i] * 2 >= top);
    home_set(brightest(&f->light[LIGHT_HOME]));
#define LIT(n) (f->light[n].r || f->light[n].g || f->light[n].b)
    out(below_home, LIT(LIGHT_CABLE));
    out(sync_light, LIT(LIGHT_PAIRING));
#undef LIT
}

/* ---- the motors: PWM timers 1 and 2, as stock ------------------------------- */

#define MOTOR_HZ    100

void plat_motors(u8 left, u8 right)
{
    pwm_set(1, MOTOR_HZ, left);
    pwm_set(2, MOTOR_HZ, right);
}

/* ---- power ------------------------------------------------------------------ */

void board_init(void)
{
    u32 i;

    out(POWER_HOLD, 1);
    for (i = 0; i < 4; i++) out(lights[i], 0), out(rows[i], 1);
    out(home_light, 0), out(below_home, 0), out(sync_light, 0);
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

/* Lights, motors and the radio core off, Home let go, then the power hold
 * released, and nothing more but waiting for USB, as stock's power-off (its
 * 0x01024194) does: the supply takes a while to die, and a restart before
 * it has would hold it again (a restart 500 ms after the release kept the
 * pad on, RAM and all).  A cable meanwhile restarts the pad. */
void board_power_off(void)
{
    u32 i;

    plat_watch_stop();
    plat_motors(0, 0);
    for (i = 0; i < 4; i++) out(lights[i], 0);
    home_set(0);
    out(below_home, 0), out(sync_light, 0);
    *(volatile u8 *)0xf853c = 0x94;      /* WAKEUP_BT: the radio core halted */
    while (!in(home))
        if (board_usb_present()) plat_reboot();
    out(POWER_HOLD, 0);
    for (;;)
        if (board_usb_present()) plat_reboot();
}

/* ---- the motion sensor, on I2C --------------------------------------------------
 * Ours is an ICM-42670 at 0xd0 (WHO_AM_I 0x75 reads 0x63; stock's 0x0102079a
 * and 0x01020420 for its IDs 0x67 and 0x63).  Stock also looks for a
 * BMI160 and another InvenSense part at 0xd0, and a QMI8658 at 0xd6 or 0xd4
 * (register 0 reads 0x05, its 0x010284a4): the QMI8658 is looked for too
 * (imu_i2c.c has it), the others not.  The pins work only once LPM
 * register 1's bit 17 is clear, as stock clears it at boot
 * (plat_lpm_release_pins): until then SDA reads 0 whatever drives it.
 * Stock's I2C (0xf8d00,
 * its 0x0101ff60 and 0x010207f8): SDA 2.0, SCL 1.15; a DMA-like engine as
 * the QSPI's: the bytes out at +0, the bytes in at +4, their lengths at +8
 * (out << 16 | in), bit 24 of +0x20 set for a read; +0xf bit 5 pulsed then
 * 0x80 starts it, +0x10 bit 0 is done.  It reports no missing
 * acknowledge.  Timing at +0x1c and +0x20 (low 24 bits): stock identifies
 * at 0x7676ecec / 0x7676 (its 0x01020036(1)) and runs 8 times faster,
 * 0x0e0e1d1d / 0x0e0e.
 *
 * Axes: stock puts the sensor's y, -x, z into its Pro Controller report,
 * accelerometer and gyro alike.  Ours: x <- -y, y <- x, z <- z, the turn
 * that gives the same gyro through profile_switch.c's mapping (stock's
 * accelerometer y then comes out opposite to ours; which is right is not
 * established). */

#define I2C(off)     (*(volatile u32 *)(0xf8d00 + (off)))
#define I2C_B(off)   (*(volatile u8 *)(0xf8d00 + (off)))
#define GPCFG_SCL    58
#define GPCFG_SDA    59
#define I2C_TIMEOUT_US 2000

const s8 imu_axis[3] = { -2, 1, 3 };

static struct {
    u8 found, probed, busy, failed;
    u8 *in;
    u32 n_in, t0;
    u32 out;                     /* the engine's buffers: word aligned, as stock's */
    u32 buf[4];
} i2c;

static void i2c_timing(u32 lo, u32 hi)
{
    I2C(0x1c) = lo;
    I2C(0x20) = hi;
}

static void i2c_start(u32 n_out, u32 n_in, int read)
{
    if (read) I2C(0x20) |= 1u << 24;
    else I2C(0x20) &= ~(1u << 24);
    I2C_B(0xf) |= 0x20;
    I2C_B(0xf) &= (u8)~0x20;
    I2C(0) = (u32)&i2c.out;
    I2C(4) = (u32)i2c.buf;
    I2C(8) = n_out << 16 | n_in;
    I2C_B(0xf) = 0x80;
    i2c.t0 = plat_us();
}

static int i2c_wait(void)
{
    while (!(I2C(0x10) & 1))
        if (plat_us() - i2c.t0 > I2C_TIMEOUT_US) return 0;
    return 1;
}

/* Each sensor stock knows that imu_i2c.c drives, by its ID register, at
 * stock's slow timing, up to 5 times each as stock does: a few ms when
 * nothing answers.  Then the fast timing. */
static const struct {
    const struct imu_chip *chip;
    const char *name;
    u8 dev, reg, id[2];
} sensors[] = {
    { &imu_icm42670, "ICM-42670", 0xd0, 0x75, { 0x67, 0x63 } },
    { &imu_qmi8658a, "QMI8658A", 0xd6, 0x00, { 0x05, 0x05 } },
    { &imu_qmi8658a, "QMI8658A", 0xd4, 0x00, { 0x05, 0x05 } },
};

static int i2c_probe(void)
{
    u32 k, tries;

    plat_lpm_release_pins();
    yc_pin(2, 0, GPCFG_SDA);
    yc_pin(1, 15, GPCFG_SCL);
    i2c_timing(0x7676ecec, 0x7676);
    for (k = 0; k < sizeof(sensors) / sizeof(sensors[0]); k++)
        for (tries = 0; tries < 5; tries++) {
            u8 dev = sensors[k].dev, id;

            i2c.out = dev | (u32)sensors[k].reg << 8 | (u32)(dev | 1) << 16;
            i2c.buf[0] = 0;
            i2c_start(3, 1, 1);
            if (!i2c_wait()) continue;
            id = (u8)i2c.buf[0];
            if (id == sensors[k].id[0] || id == sensors[k].id[1]) {
                imu_chip = sensors[k].chip;
                imu_dev = dev;
                i2c_timing(0x0e0e1d1d, 0x0e0e);
                util_log("imu: %s at %02x (id %02x)", sensors[k].name, dev, id);
                return 1;
            }
        }
    return 0;
}

/* Fails (done -1) when no sensor answered: looked for once per boot, so a
 * pad without one doesn't stall the loop each second the driver retries. */
static int i2c_begin(void)
{
    if (!i2c.found && !i2c.probed) {
        i2c.probed = 1;
        i2c.found = (u8)i2c_probe();
        if (!i2c.found) util_log("imu: no sensor answers");
    }
    i2c.failed = !i2c.found;
    i2c.busy = i2c.found;
    return i2c.found;
}

void plat_i2c_read(u8 dev, u8 reg, u8 *buf, u8 len)
{
    if (len > sizeof(i2c.buf) || !i2c_begin()) {
        i2c.failed = 1;
        i2c.busy = 0;
        return;
    }
    i2c.in = buf;
    i2c.n_in = len;
    i2c.out = dev | (u32)reg << 8 | (u32)(dev | 1) << 16;
    i2c_start(3, len, 1);
}

void plat_i2c_write(u8 dev, u8 reg, u8 value)
{
    if (!i2c_begin()) return;
    i2c.in = NULL;
    i2c.out = dev | (u32)reg << 8 | (u32)value << 16;
    i2c_start(3, 0, 0);
}

static int i2c_finished(void)
{
    if (!i2c.busy) return 1;
    if (I2C(0x10) & 1) {
        if (i2c.in) memcpy(i2c.in, i2c.buf, i2c.n_in);
    } else if (plat_us() - i2c.t0 > I2C_TIMEOUT_US) {
        i2c.failed = 1;
    } else {
        return 0;
    }
    i2c.busy = 0;
    return 1;
}

int plat_i2c_done(void)
{
    if (!i2c_finished()) return 0;
    if (i2c.failed) {
        i2c.failed = 0;
        return -1;
    }
    return 1;
}



/* For board_poll: EV_IMU once a transfer has finished. */
static u32 i2c_poll(void)
{
    return i2c.busy && i2c_finished() ? EV_IMU : 0;
}

