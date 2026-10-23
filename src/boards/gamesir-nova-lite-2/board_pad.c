#include <system/includes.h>
#include <asm/power_interface.h>
#include <asm/adc_api.h>
#include "app_config.h"
#include "board.h"

/* GameSir Nova 2 Lite (PID AC695X_TRANS): a BR23 pad with Hall sticks and
 * triggers on the ADC, a key matrix, two motors, no IMU.  Pins from
 * docs/gamesir-nova-lite-2/.  Not known yet: USB presence, the charger, a
 * power latch; so USB is taken as present, and power-off restarts. */

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

/* ---- motors: software PWM from the tick ------------------------------------
 * Each motor runs while either of two pins is high.  The stock image's motor
 * table names PC3 (left) and PB10 (right); those are driven, PB9 and PA12
 * held low.  Neither is an MCPWM channel's own pin, and through the SDK's
 * output channels (mcpwm_init with h_pin_output_ch_num) the motors did not
 * run.  So the tick switches them: a 5 ms period of 20 ticks, 200 Hz. */

#define MOTOR_STEPS     20

static const u32 motor_pin[2] = { IO_PORTC_03, IO_PORTB_10 };
static const u32 motor_other[2] = { IO_PORTB_09, IO_PORTA_12 };
static JL_PORT_FLASH_TypeDef *motor_port[2];
static u32 motor_bit[2];
static volatile u8 motor_on_ticks[2];    /* of MOTOR_STEPS */
static u8 motor_phase;

static JL_PORT_FLASH_TypeDef *port_of(u32 gpio)
{
    static JL_PORT_FLASH_TypeDef *const ports[] = { JL_PORTA, JL_PORTB, JL_PORTC, JL_PORTD };

    return ports[gpio / 16];
}

static void pin_low(u32 pin)
{
    gpio_set_pull_up(pin, 0);
    gpio_set_pull_down(pin, 0);
    gpio_set_die(pin, 1);
    gpio_direction_output(pin, 0);
}

static void motors_init(void)
{
    u32 i;

    for (i = 0; i < 2; i++) {
        pin_low(motor_other[i]);
        pin_low(motor_pin[i]);
        motor_port[i] = port_of(motor_pin[i]);
        motor_bit[i] = BIT(motor_pin[i] % 16);
    }
}

static HOT void motors_tick(void)
{
    u32 i;

    for (i = 0; i < 2; i++) {
        if (!motor_port[i]) continue;
        if (motor_phase < motor_on_ticks[i]) motor_port[i]->OUT |= motor_bit[i];
        else motor_port[i]->OUT &= ~motor_bit[i];
    }
    if (++motor_phase == MOTOR_STEPS) motor_phase = 0;
}

void plat_motors(u8 left, u8 right)
{
    motor_on_ticks[0] = (u8)(((u32)left * MOTOR_STEPS + 254) / 255);
    motor_on_ticks[1] = (u8)(((u32)right * MOTOR_STEPS + 254) / 255);
}

/* ---- LEDs: a chain on PB0, the Home light first -----------------------------
 * The stock image clocks 18 bytes, MSB first, out of PB0 from RAM code with
 * interrupts off, each bit timed by timer 3: a WS2812-type chain of six.
 * Here the writes themselves time the bits: a write of JL_PORTB->OUT takes
 * some 0.15-0.2 us and the CPU waits for it, while a timer read (0.26 us)
 * or a read-modify-write of OUT (0.28 us) is too coarse to time edges with.
 * A bit is twelve writes, near stock's 1.7 us: a 0 two high and ten low, a
 * 1 seven high and five low.  Four high for a 1 lit nothing; six high in a
 * bit of eight lit the wrong lights.  The other port B outputs (the motors,
 * switched from the tick) hold still with interrupts off, so OUT is written
 * whole.  About 250 us with interrupts off, so a frame goes out only when it
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
    motors_tick();
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

/* No wake-up source is set up yet, so soft power-off could not be left.
 * With USB taken as present the controller does not get here; restart if
 * it does. */
void plat_power_off(void)
{
    plat_reboot();
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
const u32 board_keep_pins[] = { PIN_HOME };
const u8 board_keep_count = sizeof(board_keep_pins) / sizeof(board_keep_pins[0]);
const struct wakeup_param *const board_wakeups = NULL;
