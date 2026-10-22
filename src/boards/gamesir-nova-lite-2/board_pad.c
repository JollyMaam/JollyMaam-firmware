#include <system/includes.h>
#include <asm/power_interface.h>
#include <asm/adc_api.h>
#include <asm/mcpwm.h>
#include "app_config.h"
#include "board.h"

/* GameSir Nova 2 Lite (PID AC695X_TRANS): a BR23 pad with Hall sticks and
 * triggers on the ADC, a key matrix, two motors, no IMU.  Pins from
 * docs/gamesir-nova-lite-2/.  Not known yet: the LEDs, USB presence, the
 * charger, a power latch; so no lights, USB taken as present, and power-off
 * restarts. */

#define PIN_HOME        IO_PORTB_01
#define PIN_M           IO_PORTB_02
#define PIN_R1          IO_PORTB_05
#define PIN_L1          IO_PORTB_07

/* ---- buttons on their own pin: debounced by the platform's tick ----------- */

const struct line board_lines[] = {
    {PIN_HOME, PAD_HOME}, {PIN_M, PAD_M}, {PIN_R1, PAD_R1}, {PIN_L1, PAD_L1},
};
const u8 board_line_count = sizeof(board_lines) / sizeof(board_lines[0]);

static u32 buttons_of(u32 down)
{
    u32 i, buttons = 0;

    for (i = 0; i < board_line_count; i++)
        if (down & BIT(i)) buttons |= board_lines[i].bit;
    return buttons;
}

/* ---- the key matrix ---------------------------------------------------------
 * Three columns, PC0..PC2, five rows on port A, pulled up.  A key joins its
 * row and column.  One column at a time is driven low; the rows are read a
 * tick (250 us) later, then the next column is driven: a scan every 750 us.
 * PA5 is joined to PA4 on the board and left alone.  Which way the scan has
 * to go (columns driven or rows) is not established: columns are driven. */

#define MATRIX_COLS     3
#define MATRIX_ROWS     5
#define COL_MASK        (BIT(0) | BIT(1) | BIT(2))                       /* PC0..PC2 */
#define ROW_MASK        (BIT(3) | BIT(4) | BIT(6) | BIT(7) | BIT(8))     /* PA3, PA4, PA6..PA8 */
#define MATRIX_LOCK     8                /* scans a key ignores after it changed: 6 ms */

static const u8 row_bit[MATRIX_ROWS] = { 3, 4, 6, 7, 8 };
static const u32 matrix_button[MATRIX_COLS][MATRIX_ROWS] = {
    /*          PA3          PA4         PA6        PA7         PA8 */
    /* PC0 */ { PAD_UP,      PAD_RIGHT,  PAD_LEFT,  PAD_DOWN,   PAD_R3 },
    /* PC1 */ { PAD_SELECT,  PAD_SCREENSHOT, PAD_START, PAD_R4, PAD_L4 },
    /* PC2 */ { PAD_Y,       PAD_X,      PAD_A,     PAD_B,      PAD_L3 },
};

static u8 matrix_col;
static volatile u32 matrix_state;        /* PAD_* bits of the matrix keys down */
static volatile u32 matrix_changed_us;
static u8 matrix_lock[MATRIX_COLS][MATRIX_ROWS];

static HOT void column_drive(u32 col)
{
    JL_PORTC->OUT &= ~COL_MASK;
    JL_PORTC->DIR |= COL_MASK;           /* the others float */
    JL_PORTC->DIR &= ~BIT(col);
}

static void matrix_init(void)
{
    JL_PORTC->PU &= ~COL_MASK;
    JL_PORTC->PD &= ~COL_MASK;
    JL_PORTC->DIE |= COL_MASK;
    JL_PORTC->OUT &= ~COL_MASK;
    JL_PORTC->DIR |= COL_MASK;
    JL_PORTA->PD &= ~ROW_MASK;
    JL_PORTA->PU |= ROW_MASK;
    JL_PORTA->DIE |= ROW_MASK;
    JL_PORTA->DIR |= ROW_MASK;
}

/* Every key at once, for boot: each column for a few microseconds. */
static u32 matrix_read_now(void)
{
    u32 col, row, i, down = 0;

    for (col = 0; col < MATRIX_COLS; col++) {
        u32 rows;

        column_drive(col);
        for (i = 0; i < 200; i++) (void)JL_PORTA->IN;
        rows = JL_PORTA->IN;
        for (row = 0; row < MATRIX_ROWS; row++)
            if (!(rows & BIT(row_bit[row]))) down |= matrix_button[col][row];
    }
    JL_PORTC->DIR |= COL_MASK;
    return down;
}

static HOT void matrix_tick(void)
{
    u32 rows = JL_PORTA->IN;
    u32 col = matrix_col;
    u32 row, changed = 0;

    for (row = 0; row < MATRIX_ROWS; row++) {
        u32 bit = matrix_button[col][row];
        u32 down = !(rows & BIT(row_bit[row]));

        if (matrix_lock[col][row]) {
            matrix_lock[col][row]--;
        } else if (down != !!(matrix_state & bit)) {
            matrix_state ^= bit;
            matrix_lock[col][row] = MATRIX_LOCK;
            changed = 1;
        }
    }
    matrix_col = (u8)(col + 1 == MATRIX_COLS ? 0 : col + 1);
    column_drive(matrix_col);
    if (changed) {
        matrix_changed_us = plat_now_ms * 1000;
        plat_counters.button_changes++;
        plat_raise(EV_BUTTONS);
    }
}

/* ---- sticks and triggers: Hall sensors on the ADC ------------------------ */

static const u32 adc_pin[6] = { IO_PORTA_01, IO_PORTB_08, IO_PORTC_04, IO_PORTC_05,
                                IO_PORTA_10, IO_PORTB_03 };
static const u32 adc_ch[6] = { AD_CH_PA1, AD_CH_PB8, AD_CH_PC4, AD_CH_PC5,   /* LX LY RX RY */
                               AD_CH_PA10, AD_CH_PB3 };                      /* LT RT */

#define ADC_FIRST 16

static void analog_init(void)
{
    u32 i;

    for (i = 0; i < 6; i++) {
        gpio_set_pull_down(adc_pin[i], 0);
        gpio_set_pull_up(adc_pin[i], 0);
        gpio_set_die(adc_pin[i], 0);
        gpio_direction_input(adc_pin[i]);
        adc_add_sample_ch(adc_ch[i]);
    }
    /* A channel reads 0 or 1 until its first sample, which an uncalibrated
     * trigger would take as its rest: wait for them (a few ms).  A trigger
     * held down reads near 100. */
    for (i = 0; i < 50 && (adc_get_value(adc_ch[4]) < ADC_FIRST ||
                           adc_get_value(adc_ch[5]) < ADC_FIRST); i++)
        plat_mdelay(1);
    util_log("board: first ADC samples after %u ms", i);
}

/* The sticks' 10-bit readings, times 32, land where the Cyclone 2's stick
 * MCU puts its own (center near 0x4000, input_calibration.c's defaults),
 * and each axis already turns the way the Cyclone's does: the core needs
 * no board-specific signs.  Triggers read lower when pulled, as there. */
void plat_input(struct in_raw *raw)
{
    u32 changed_us, i;
    u32 lines = plat_lines(&changed_us);

    raw->buttons = buttons_of(lines) | matrix_state;
    if ((s32)(matrix_changed_us - changed_us) > 0) changed_us = matrix_changed_us;
    raw->changed_us = changed_us;
    for (i = 0; i < 4; i++) raw->stick[i] = (u16)(adc_get_value(adc_ch[i]) << 5);
    raw->sticks_valid = 1;
    raw->trigger[0] = (u16)adc_get_value(adc_ch[4]);
    raw->trigger[1] = (u16)adc_get_value(adc_ch[5]);
    raw->trigger_stop[0] = raw->trigger_stop[1] = 0;
}

/* ---- motors: PWM at 25 kHz ------------------------------------------------
 * Each motor runs while either of two pins is high.  The stock image's motor
 * table names PC3 (left) and PB10 (right); neither is an MCPWM channel's own
 * pin, so they go through output channels 0 and 1.  PB9 (left) and PA12
 * (right) are held low. */

static const struct {
    u32 pin; pwm_ch_num_type ch; pwm_timer_num_type timer; u8 out;
} motor[2] = {
    { IO_PORTC_03, pwm_ch2, pwm_timer2, 0 }, { IO_PORTB_10, pwm_ch1, pwm_timer1, 1 },
};
static const u32 motor_other[2] = { IO_PORTB_09, IO_PORTA_12 };
static u8 motor_level[2];

extern void mcpwm_set_duty(pwm_ch_num_type pwm_ch, pwm_timer_num_type timer_ch, u16 duty);

static void pin_low(u32 pin)
{
    gpio_set_pull_up(pin, 0);
    gpio_set_pull_down(pin, 0);
    gpio_set_die(pin, 1);
    gpio_direction_output(pin, 0);
}

/* Both channels stay open; duty 0 holds the pin low. */
static void motors_init(void)
{
    struct pwm_platform_data cfg;
    u32 i;

    pin_low(motor_other[0]);
    pin_low(motor_other[1]);
    for (i = 0; i < 2; i++) {
        pin_low(motor[i].pin);
        memset(&cfg, 0, sizeof(cfg));
        cfg.frequency = 25000;
        cfg.pwm_ch_num = motor[i].ch;
        cfg.pwm_timer_num = motor[i].timer;
        cfg.duty = 0;
        cfg.pwm_aligned_mode = pwm_edge_aligned;
        cfg.h_pin = (u8)motor[i].pin;
        cfg.h_pin_output_ch_num = motor[i].out;
        cfg.l_pin = (u8)-1;
        mcpwm_init(&cfg);
    }
}

static void motor_set(u32 i, u8 level)
{
    if (level == motor_level[i]) return;
    motor_level[i] = level;
    mcpwm_set_duty(motor[i].ch, motor[i].timer, (u16)((u32)level * 10000 / 255));
}

void plat_motors(u8 left, u8 right)
{
    motor_set(0, left);
    motor_set(1, right);
}

/* ---- boot: what was held, the way back from a bad build -------------------
 * Home + Menu with USB at the very start goes to the ROM loader before any
 * of this firmware can hang.  Without it, two boots in a row that do not run
 * BOOT_HEALTHY_MS (a hang-watch or watchdog reset, a crash) go there on the
 * third.  The count is in retained RAM, so a power cut starts it over. */

#define BOOT_MAGIC      0x4e4c3242      /* "NL2B" */
#define BOOT_FAILS_MAX  2
#define BOOT_HEALTHY_MS 10000

static struct { u32 magic, fails; } guard RETAINED;
static struct plat_boot boot;

static u32 held_now(void)
{
    return buttons_of(plat_read_lines()) | matrix_read_now();
}

void board_latch(void)
{
    u32 i;

    for (i = 0; i < board_line_count; i++) plat_pull_up_input(board_lines[i].gpio);
    matrix_init();
    boot.buttons_held = held_now();
    if ((boot.buttons_held & (PAD_HOME | PAD_START)) == (PAD_HOME | PAD_START))
        plat_enter_loader();
    if (guard.magic != BOOT_MAGIC) {
        guard.magic = BOOT_MAGIC;
        guard.fails = 0;
    }
    if (guard.fails >= BOOT_FAILS_MAX) {
        guard.fails = 0;
        plat_flush_dcache(&guard, sizeof(guard));
        plat_enter_loader();
    }
    guard.fails++;
    plat_flush_dcache(&guard, sizeof(guard));
    boot.cause = boot.buttons_held & PAD_HOME ? BOOT_HOME : BOOT_USB;
}

void plat_boot_info(struct plat_boot *out)
{
    boot.buttons_held = held_now();
    *out = boot;
}

/* ---- the rest of the board interface --------------------------------------- */

void board_start(void)
{
    analog_init();
    motors_init();
    column_drive(matrix_col);
}

HOT void board_tick(void)
{
    matrix_tick();
    if (guard.fails && plat_now_ms >= BOOT_HEALTHY_MS) {
        guard.fails = 0;
        plat_flush_dcache(&guard, sizeof(guard));
    }
}

void board_to_loader(void)
{
}

/* No sense pin known: USB is taken as there. */
int board_usb_sense(void)
{
    return 1;
}

int board_jack(void)
{
    return 0;
}

void plat_leds(const struct led_frame *f)
{
    (void)f;
}

/* No wake-up source is set up yet, so soft power-off could not be left.
 * With USB taken as present the controller does not get here; restart if
 * it does. */
void plat_power_off(void)
{
    plat_reboot();
}

/* No IMU: input_imu.c links for the profiles' motion maths, and its I2C
 * reads fail. */
void plat_i2c_read(u8 dev, u8 reg, u8 *buf, u8 len)
{
    (void)dev; (void)reg; (void)buf; (void)len;
}

void plat_i2c_write(u8 dev, u8 reg, u8 value)
{
    (void)dev; (void)reg; (void)value;
}

int plat_i2c_done(void)
{
    return -1;
}

const char board_name[] = "GameSir Nova 2 Lite pad";
const u8 board_audio = 0;
const u32 board_amp_enable = 0;
const u32 board_keep_pins[] = { PIN_HOME };
const u8 board_keep_count = sizeof(board_keep_pins) / sizeof(board_keep_pins[0]);
const struct wakeup_param *const board_wakeups = NULL;
