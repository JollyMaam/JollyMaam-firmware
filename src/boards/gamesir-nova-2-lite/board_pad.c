#include <system/includes.h>
#include <asm/power_interface.h>
#include <asm/power/p33.h>
#include <asm/adc_api.h>
#include <asm/mcpwm.h>
#include "app_config.h"
#include "board.h"

/* GameSir Nova 2 Lite (PID AC695X_TRANS): a BR23 pad with Hall sticks and
 * triggers on the ADC, a key matrix, two motors, no IMU.  Pins from
 * docs/gamesir-nova-2-lite/.  Soft power-off wakes on Home or USB insertion.
 * Not known yet: the charger and a power latch. */

#define PIN_HOME        IO_PORTB_01
#define PIN_M           IO_PORTB_02
#define PIN_R1          IO_PORTB_05
#define PIN_L1          IO_PORTB_07
#define PIN_USB_SENSE   IO_PORTA_00

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
    /* PC2 */ { PAD_NORTH,   PAD_WEST,   PAD_SOUTH, PAD_EAST,   PAD_L3 },
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

static void leds_first(void);

/* The sticks' 10-bit readings, times 32, land where the Cyclone 2's stick
 * MCU puts its own (center near 0x4000, input_calibration.c's defaults),
 * and each axis already turns the way the Cyclone's does: the core needs
 * no board-specific signs.  Triggers read lower when pulled, as there. */
void plat_input(struct in_raw *raw)
{
    u32 changed_us, i;
    u32 lines = plat_lines(&changed_us);

    raw->buttons = buttons_of(lines) | matrix_state;
    leds_first();
    if ((s32)(matrix_changed_us - changed_us) > 0) changed_us = matrix_changed_us;
    raw->changed_us = changed_us;
    for (i = 0; i < 4; i++) raw->stick[i] = (u16)(adc_get_value(adc_ch[i]) << 5);
    raw->sticks_valid = 1;
    raw->trigger[0] = (u16)adc_get_value(adc_ch[4]);
    raw->trigger[1] = (u16)adc_get_value(adc_ch[5]);
    raw->trigger_stop[0] = raw->trigger_stop[1] = 0;
}

/* ---- motors: MCPWM ------------------------------------------------------
 * Each motor runs while either of two pins is high.  Stock drives PC3 (left)
 * from MCPWM channel 2 through an output channel and PB10 (right) from
 * channel 3 on its own L pin (mcpwm_init inlined near 0x01e26254 in the
 * stock image).  PB9 and PA12 are held low.  An output channel picks its pin
 * by the pin's own PU, PD, DIE and OUT bits (gpio_output_channle), so PC3
 * must not be reconfigured while it runs.
 *
 * Both run at 24 kHz, above hearing; at 6.5 kHz (stock's) they whine.  Stock
 * sets the left one's strength by frequency (0x01e418a4: 22 kHz with duty
 * held high for low levels, down to 130 Hz near full), which hums.
 *
 * At 24 kHz either motor keeps turning down to 65% duty: the rumble
 * defaults start there.  No kick. */

#define MOTOR_HZ        24000

static const struct {
    u32 pin; pwm_ch_num_type ch; pwm_timer_num_type timer; u8 low_pin;
} motor[2] = {
    { IO_PORTC_03, pwm_ch2, pwm_timer2, 0 }, { IO_PORTB_10, pwm_ch3, pwm_timer3, 1 },
};
static const u32 motor_other[2] = { IO_PORTB_09, IO_PORTA_12 };
static u8 motor_level[2] = { 1, 1 };

static void pin_low(u32 pin)
{
    gpio_set_pull_up(pin, 0);
    gpio_set_pull_down(pin, 0);
    gpio_set_die(pin, 1);
    gpio_direction_output(pin, 0);
}

static void motor_set(u32 i, u8 level)
{
    struct pwm_platform_data cfg;

    if (level == motor_level[i]) return;
    motor_level[i] = level;
    if (!level) {
        mcpwm_ch_open_or_close(motor[i].ch, 0);
        JL_MCPWM->MCPWM_CON0 &= ~BIT((u32)motor[i].timer + 8);
        pin_low(motor[i].pin);
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.frequency = MOTOR_HZ;
    cfg.pwm_ch_num = motor[i].ch;
    cfg.pwm_timer_num = motor[i].timer;
    cfg.duty = (u32)level * 10000 / 255;
    cfg.pwm_aligned_mode = pwm_edge_aligned;
    cfg.h_pin = motor[i].low_pin ? (u8)-1 : (u8)motor[i].pin;
    cfg.l_pin = motor[i].low_pin ? (u8)motor[i].pin : (u8)-1;
    cfg.h_pin_output_ch_num = 0;
    mcpwm_init(&cfg);
}

static void motors_init(void)
{
    pin_low(motor_other[0]);
    pin_low(motor_other[1]);
    motor_set(0, 0);
    motor_set(1, 0);
}

void plat_motors(u8 left, u8 right)
{
    motor_set(0, left);
    motor_set(1, right);
}

#include "settings_defaults.inc"

/* ---- LEDs: a chain on PB0, the Home light first -----------------------------
 * The stock image clocks 18 bytes, MSB first, out of PB0 from RAM code with
 * interrupts off, each bit timed by timer 3: a WS2812-type chain of six.
 * Here the writes themselves time the bits: a write of JL_PORTB->OUT takes
 * some 0.15-0.2 us and the CPU waits for it, while a timer read (0.26 us)
 * or a read-modify-write of OUT (0.28 us) is too coarse to time edges with.
 * A bit is twelve writes, near stock's 1.7 us: a 0 two high and ten low, a
 * 1 seven high and five low.  Four high for a 1 lit nothing; six high in a
 * bit of eight lit the wrong lights.  The other port B outputs hold still with
 * interrupts off, so OUT is written whole.  About 250 us with interrupts off, so a frame goes out only when it
 * changed.  (Taking over timer 2 the way stock takes timer 3 hung the pad,
 * cause not established.)  A light's bytes go out R, G, B. */

#define PIN_LED         IO_PORTB_00
#define LED_CHAIN       6

static u8 led_sent[LED_CHAIN * 3];
static u8 led_ready;

static HOT void ws_send(const u8 *p, u32 n)
{
    volatile u32 *out = &JL_PORTB->OUT;
    const u8 *end = p + n;
    u32 hi, lo, v, mask;

    local_irq_disable();
    lo = *out & ~BIT(0);
    hi = lo | BIT(0);
    for (; p < end; p++) {
        v = *p;
        for (mask = 0x80; mask; mask >>= 1) {
            if (v & mask) {
                *out = hi; *out = hi; *out = hi; *out = hi; *out = hi; *out = hi; *out = hi;
                *out = lo; *out = lo; *out = lo; *out = lo; *out = lo;
            } else {
                *out = hi; *out = hi;
                *out = lo; *out = lo; *out = lo; *out = lo; *out = lo;
                *out = lo; *out = lo; *out = lo; *out = lo; *out = lo;
            }
        }
    }
    local_irq_enable();
}

static void leds_put(const u8 *bytes)
{
    if (!led_ready) {
        pin_low(PIN_LED);
        led_ready = 1;
    } else if (!memcmp(bytes, led_sent, sizeof(led_sent))) {
        return;
    }
    memcpy(led_sent, bytes, sizeof(led_sent));
    ws_send(led_sent, sizeof(led_sent));
}

#ifdef NOVA_LED_PROBE
#include "led_probe.inc"
#endif

/* The core sends a frame only when it changes, and none while it stays dark
 * from the start; the chain keeps its colours while it has power, across
 * resets.  So the first input poll sends a dark frame. */
static void leds_first(void)
{
    u8 dark[LED_CHAIN * 3] = { 0 };

    if (!led_ready) leds_put(dark);
}

/* Position 0 is the Home light; positions 1-5, each lit alone, showed
 * nothing.  The rest of the core's lights go there anyway, as stock sends
 * six. */
void plat_leds(const struct led_frame *f)
{
    u8 bytes[LED_CHAIN * 3];
    u32 i;

#ifdef NOVA_LED_PROBE
    if (led_probe_active) return;
#endif
    for (i = 0; i < LED_CHAIN; i++) {
        bytes[i * 3] = f->light[i].r;
        bytes[i * 3 + 1] = f->light[i].g;
        bytes[i * 3 + 2] = f->light[i].b;
    }
    leds_put(bytes);
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
static u8 boot_wakeup_source, boot_wakeup_pending;

static u32 held_now(void)
{
    return buttons_of(plat_read_lines()) | matrix_read_now();
}

void board_latch(void)
{
    u32 i;

    /* Home can already be released by the time the app starts. P33 keeps
     * the GPIO wake source (bit 1) and the pending wake-port mask; Home
     * uses port index 1. Read them before power_init() reconfigures wake. */
    boot_wakeup_source = P33_CON_GET(P3_WKUP_SRC);
    boot_wakeup_pending = P33_CON_GET(P3_WKUP_PND);
    for (i = 0; i < board_line_count; i++) plat_pull_up_input(board_lines[i].gpio);
    gpio_set_pull_up(PIN_USB_SENSE, 0);
    gpio_set_pull_down(PIN_USB_SENSE, 0);
    gpio_direction_input(PIN_USB_SENSE);
    gpio_set_die(PIN_USB_SENSE, 1);
    matrix_init();
    boot.buttons_held = held_now();
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
    boot.cause = (boot.buttons_held & PAD_HOME) ||
                 ((boot_wakeup_source & BIT(1)) && (boot_wakeup_pending & BIT(1)))
                 ? BOOT_HOME : gpio_read(PIN_USB_SENSE) ? BOOT_USB : BOOT_POWER_ON;
}

void plat_boot_info(struct plat_boot *out)
{
    boot.buttons_held = held_now();
    util_log("board: wake source %02x pending %02x", boot_wakeup_source, boot_wakeup_pending);
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

/* Active-high on PA0 (also connected to PC7), with no pull-up/down.
 * Runtime USB detection still uses bus frames in the platform. */
int board_usb_sense(void)
{
    return gpio_read(PIN_USB_SENSE);
}

int board_jack(void)
{
    return 0;
}

void plat_power_off(void)
{
    struct led_frame dark = { 0 };

    plat_watch_stop();
    plat_motors(0, 0);
    plat_leds(&dark);
    plat_mdelay(1);              /* the LED chain latches after its data stays low */
    power_set_soft_poweroff();
}

/* No motion sensor. */
u32 imu_poll(u32 now, u8 mode)
{
    (void)now; (void)mode;
    return 0;
}

const char board_name[] = "Nova 2 Lite";
const u8 board_audio = 0;
const u32 board_amp_enable = 0;
/* Home and Menu's matrix row/column must read correctly in board_latch(),
 * before power_init() restores the pins parked during soft power-off.
 * Otherwise a Home wake could appear to hold the Home+Menu loader chord. */
const u32 board_keep_pins[] = { PIN_HOME, IO_PORTA_06, IO_PORTC_02, PIN_USB_SENSE };
const u8 board_keep_count = sizeof(board_keep_pins) / sizeof(board_keep_pins[0]);

static struct port_wakeup home_wakeup = {
    .pullup_down_enable = ENABLE, .edge = FALLING_EDGE,
    .attribute = BLUETOOTH_RESUME, .iomap = PIN_HOME, .filter_enable = ENABLE,
};
static struct port_wakeup usb_wakeup_port = {
    .pullup_down_enable = DISABLE, .edge = RISING_EDGE,
    .attribute = BLUETOOTH_RESUME, .iomap = PIN_USB_SENSE, .filter_enable = ENABLE,
};
/* The chip's internal charger has no input; USB wakes through PA0. */
static const struct charge_wakeup charge_wkup = { .attribute = 0 };
static const struct sub_wakeup sub_wkup = { .attribute = BLUETOOTH_RESUME };
static const struct wakeup_param wakeups = {
    .port[1] = &home_wakeup, .port[2] = &usb_wakeup_port,
    .sub = &sub_wkup, .charge = &charge_wkup,
};
const struct wakeup_param *const board_wakeups = &wakeups;

/* Preserve the legacy VBAT reading; no board-specific level or charger
 * status has been established here. Warning thresholds retain their policy. */
void board_battery(struct battery_status *out)
{
    static struct battery_status b;

    battery_voltage(&b, adc_get_voltage(AD_CH_VBAT) * 4, 0, 0);
    battery_low(&b, 3500, 3600);
    *out = b;
}
