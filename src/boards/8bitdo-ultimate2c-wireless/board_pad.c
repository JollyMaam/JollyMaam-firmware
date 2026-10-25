#include "sdk.h"
#include "board.h"

/* 8BitDo Ultimate 2C Wireless: a Telink TLSR8208 (B80).  Pins are in
 * docs/8bitdo-ultimate2c-wireless/README.md.  The scanner runs a step at a
 * time from the platform's wait loop and never waits: a matrix row settles
 * while other work runs, as does each ADC channel after the mux moves. */

const char board_name[] = "Ultimate 2C Wireless";

/* ---- the button matrix: a row driven low, the columns pulled up ---------- */

#define ROWS 4
#define COLUMNS 5
#define ROW_SETTLE_US 20
#define SCAN_GAP_US 1000             /* from one full scan to the next */
#define DEBOUNCE_SCANS 3             /* the same reading this many scans in a row */

static const GPIO_PinTypeDef rows[ROWS] = { GPIO_PD6, GPIO_PA0, GPIO_PC0, GPIO_PA3 };
static const GPIO_PinTypeDef columns[COLUMNS] = { GPIO_PA5, GPIO_PC1, GPIO_PA6, GPIO_PC3, GPIO_PA7 };
#define HOME GPIO_PA4                /* on its own, active low */

/* Each position's PAD_* bit, row by row; 0: not a button (Sync and the
 * rear mode switch, which are the board's own). */
static const u32 keys[ROWS][COLUMNS] = {
    { PAD_UP, PAD_Y, PAD_START, PAD_R4, PAD_L4 },
    { PAD_LEFT, PAD_X, PAD_M, PAD_R1, PAD_L1 },             /* M: the Turbo button */
    { PAD_DOWN, PAD_B, PAD_SELECT, PAD_R3, PAD_L3 },
    { PAD_RIGHT, PAD_A, PAD_SCREENSHOT, 0, 0 },
};

static struct {
    u8 row, driving;
    u32 tick;                        /* clock_time(): the row went low */
    u32 scanned;                     /* clock_time(): the last full scan ended */
    u32 reading;                     /* PAD_* this scan */
    u32 stable;                      /* debounced */
    u32 changed_us;
    u8 same[32];                     /* scans a bit has differed from stable */
} mx;

static void matrix_init(void)
{
    u32 i;

    for (i = 0; i < ROWS; i++) {
        gpio_write(rows[i], 1);      /* high before it drives */
        gpio_set_func(rows[i], AS_GPIO);
        gpio_set_output_en(rows[i], 1);
        gpio_set_input_en(rows[i], 0);
    }
    for (i = 0; i <= COLUMNS; i++) {
        GPIO_PinTypeDef pin = i < COLUMNS ? columns[i] : HOME;

        gpio_set_output_en(pin, 0);
        gpio_set_func(pin, AS_GPIO);
        gpio_set_input_en(pin, 1);
        gpio_setup_up_down_resistor(pin, PM_PIN_PULLUP_10K);
    }
    mx.scanned = clock_time();
}

static u32 matrix_poll(void)
{
    u32 i, b, changed = 0;

    if (!mx.driving) {
        if (!mx.row && !clock_time_exceed(mx.scanned, SCAN_GAP_US)) return 0;
        gpio_write(rows[mx.row], 0);
        mx.tick = clock_time();
        mx.driving = 1;
        return 0;
    }
    if (!clock_time_exceed(mx.tick, ROW_SETTLE_US)) return 0;
    for (i = 0; i < COLUMNS; i++)
        if (!gpio_read(columns[i])) mx.reading |= keys[mx.row][i];
    gpio_write(rows[mx.row], 1);
    mx.driving = 0;
    if (++mx.row < ROWS) return 0;
    mx.row = 0;
    mx.scanned = clock_time();
    if (!gpio_read(HOME)) mx.reading |= PAD_HOME;
    for (i = 0; i < 32; i++) {
        b = 1u << i;
        if (!((mx.reading ^ mx.stable) & b)) {
            mx.same[i] = 0;
        } else if (++mx.same[i] >= DEBOUNCE_SCANS) {
            mx.stable ^= b;
            mx.same[i] = 0;
            changed = 1;
        }
    }
    mx.reading = 0;
    if (changed) mx.changed_us = plat_us();
    return changed ? EV_BUTTONS : 0;
}

/* ---- sticks and triggers: six ADC channels in turn ----------------------- */

#define AXES 6
#define MUX_SETTLE_US 50
#define SAMPLE_US 200                /* DMA fills the four samples */
#define SAMPLE_TIMEOUT_US 1000
#define AXIS_GAP_US 100
#define DMA_EMPTY 0xa5a5a5a5u

/* LX LY RX RY L2 R2; channel = pin number + 1. */
static const adc_input_pin_def_e axis_pins[AXES] = {
    ADC_GPIO_PB0, ADC_GPIO_PB1, ADC_GPIO_PB4, ADC_GPIO_PB5, ADC_GPIO_PB2, ADC_GPIO_PB3,
};
static const u8 axis_channels[AXES] = { 1, 2, 5, 6, 3, 4 };

static volatile u32 dma[4] __attribute__((aligned(16)));

static struct {
    u8 axis, phase, valid;
    u32 tick;
    u16 code[AXES];                  /* 13-bit, the middle two of four samples */
} ad;

static void adc_start(void)
{
    u32 i;

    adc_init();
    for (i = 0; i < AXES; i++) {
        adc_base_pin_init(axis_pins[i]);
        gpio_setup_up_down_resistor(axis_pins[i] & 0xfff, PM_PIN_UP_DOWN_FLOAT);
    }
    adc_set_vref_vbat_divider(ADC_VBAT_DIVIDER_OFF);
    adc_set_ain_pre_scaler(ADC_PRESCALER_1);   /* as stock: analog 0xfa = 0x3d */
    adc_power_on_sar_adc(1);
    ad.tick = clock_time();
}

static void adc_poll(void)
{
    u16 sorted[4];
    u32 i, j, complete = 1;

    if (ad.phase == 0) {
        if (!clock_time_exceed(ad.tick, AXIS_GAP_US)) return;
        adc_set_ain_chn_misc(axis_channels[ad.axis], GND);
        ad.tick = clock_time();
        ad.phase = 1;
        return;
    }
    if (ad.phase == 1) {
        if (!clock_time_exceed(ad.tick, MUX_SETTLE_US)) return;
        adc_reset_adc_module();
        for (i = 0; i < 4; i++) dma[i] = DMA_EMPTY;
        adc_config_misc_channel_buf((u16 *)dma, sizeof(dma));
        dfifo_enable_dfifo2();
        ad.tick = clock_time();
        ad.phase = 2;
        return;
    }
    if (!clock_time_exceed(ad.tick, SAMPLE_US)) return;
    for (i = 0; i < 4; i++)
        if (dma[i] == DMA_EMPTY) complete = 0;
    if (!complete && !clock_time_exceed(ad.tick, SAMPLE_TIMEOUT_US)) return;
    dfifo_disable_dfifo2();
    if (complete) {
        for (i = 0; i < 4; i++) {
            u16 c = dma[i] & (1u << 13) ? 0 : (u16)(dma[i] & 0x1fff);   /* negative: 0 */

            for (j = i; j > 0 && sorted[j - 1] > c; j--) sorted[j] = sorted[j - 1];
            sorted[j] = c;
        }
        ad.code[ad.axis] = (u16)((sorted[1] + sorted[2]) / 2);
        ad.valid |= (u8)(1u << ad.axis);
    } else {
        ad.valid &= (u8)~(1u << ad.axis);
    }
    if (++ad.axis == AXES) ad.axis = 0;
    ad.phase = 0;
    ad.tick = clock_time();
}

/* ---- the board's interface to the platform ------------------------------- */

void board_init(void)
{
    matrix_init();
    adc_start();
}

u32 board_poll(void)
{
    adc_poll();
    return matrix_poll();
}

static u16 clamp10(s32 v)
{
    return (u16)(v < 0 ? 0 : v > 1023 ? 1023 : v);
}

/* The Cyclone 2's raw scales and directions (input_calibration.c): sticks
 * near 0x4000 at rest, the left one growing right and down, the right one
 * left and up; triggers in 10 bits, falling as they are pressed.  Codes
 * here: sticks 1300-6600 about 4000 at rest, right X growing right; triggers
 * about 3650 at rest and 2100 pressed in. */
void board_input(struct in_raw *raw)
{
    raw->buttons = mx.stable;
    raw->changed_us = mx.changed_us;
    raw->sticks_valid = (ad.valid & 0x0f) == 0x0f;
    raw->stick[0] = (u16)(ad.code[0] << 2);
    raw->stick[1] = (u16)(ad.code[1] << 2);
    raw->stick[2] = (u16)((8191 - ad.code[2]) << 2);
    raw->stick[3] = (u16)(ad.code[3] << 2);
    raw->trigger[0] = clamp10(((s32)ad.code[4] - 1900) / 2);
    raw->trigger[1] = clamp10(((s32)ad.code[5] - 1900) / 2);
    raw->trigger_stop[0] = raw->trigger_stop[1] = 0;
}

/* ---- this pad's OTP --------------------------------------------------------
 * Its OTP word at 0x3fb4 reads 0x10000000, which the stock boot loader runs
 * flash firmware with, but the SDK's cpu_wakeup_init() wants 0xffffffff
 * there and resets the chip otherwise.  Linked with --wrap=otp_read: only
 * that word, with that value, reads as the SDK wants. */

void __real_otp_read(unsigned int address, unsigned int words, unsigned int *buffer);

void __wrap_otp_read(unsigned int address, unsigned int words, unsigned int *buffer)
{
    __real_otp_read(address, words, buffer);
    if (address == 0x3fb4 && words == 1 && buffer[0] == 0x10000000) buffer[0] = 0xffffffff;
}
