#include "asm/gpio.h"

#include "board.h"
#include "cyclone2/input.h"

struct digital_button {
    u32 gpio;
    u16 report_mask;
};

static const struct digital_button buttons[] = {
    {CYCLONE2_BUTTON_A,          CYCLONE2_REPORT_BUTTON_A},
    {CYCLONE2_BUTTON_B,          CYCLONE2_REPORT_BUTTON_B},
    {CYCLONE2_BUTTON_X,          CYCLONE2_REPORT_BUTTON_X},
    {CYCLONE2_BUTTON_Y,          CYCLONE2_REPORT_BUTTON_Y},
    {CYCLONE2_BUTTON_L1,         CYCLONE2_REPORT_BUTTON_L1},
    {CYCLONE2_BUTTON_R1,         CYCLONE2_REPORT_BUTTON_R1},
    {CYCLONE2_BUTTON_L3,         CYCLONE2_REPORT_BUTTON_L3},
    {CYCLONE2_BUTTON_R3,         CYCLONE2_REPORT_BUTTON_R3},
    {CYCLONE2_BUTTON_SELECT,     CYCLONE2_REPORT_BUTTON_SELECT},
    {CYCLONE2_BUTTON_START,      CYCLONE2_REPORT_BUTTON_START},
    {CYCLONE2_BUTTON_HOME,       CYCLONE2_REPORT_BUTTON_HOME},
    {CYCLONE2_BUTTON_SCREENSHOT, CYCLONE2_REPORT_BUTTON_SCREENSHOT},
    {CYCLONE2_BUTTON_L4,         CYCLONE2_REPORT_BUTTON_L4},
    {CYCLONE2_BUTTON_R4,         CYCLONE2_REPORT_BUTTON_R4},
    {CYCLONE2_BUTTON_M,          CYCLONE2_REPORT_BUTTON_M},
};

static const u32 dpad_gpios[] = {
    CYCLONE2_DPAD_UP,
    CYCLONE2_DPAD_RIGHT,
    CYCLONE2_DPAD_DOWN,
    CYCLONE2_DPAD_LEFT,
};

static void configure_active_low_input(u32 gpio)
{
    gpio_set_pull_down(gpio, 0);
    gpio_set_pull_up(gpio, 1);
    gpio_set_die(gpio, 1);
    gpio_direction_input(gpio);
}

static u8 read_hat(void)
{
    u8 up = !gpio_read(CYCLONE2_DPAD_UP);
    u8 right = !gpio_read(CYCLONE2_DPAD_RIGHT);
    u8 down = !gpio_read(CYCLONE2_DPAD_DOWN);
    u8 left = !gpio_read(CYCLONE2_DPAD_LEFT);

    if (up == down) {
        up = 0;
        down = 0;
    }
    if (left == right) {
        left = 0;
        right = 0;
    }

    if (up) {
        if (right) {
            return CYCLONE2_HAT_UP_RIGHT;
        }
        if (left) {
            return CYCLONE2_HAT_UP_LEFT;
        }
        return CYCLONE2_HAT_UP;
    }
    if (down) {
        if (right) {
            return CYCLONE2_HAT_DOWN_RIGHT;
        }
        if (left) {
            return CYCLONE2_HAT_DOWN_LEFT;
        }
        return CYCLONE2_HAT_DOWN;
    }
    if (right) {
        return CYCLONE2_HAT_RIGHT;
    }
    if (left) {
        return CYCLONE2_HAT_LEFT;
    }
    return CYCLONE2_HAT_NEUTRAL;
}

void cyclone2_input_initialize(void)
{
    unsigned index;

    for (index = 0; index < sizeof(buttons) / sizeof(buttons[0]); index++) {
        configure_active_low_input(buttons[index].gpio);
    }
    for (index = 0;
         index < sizeof(dpad_gpios) / sizeof(dpad_gpios[0]);
         index++) {
        configure_active_low_input(dpad_gpios[index]);
    }
}

void cyclone2_input_read(struct cyclone2_input_report *report)
{
    unsigned index;

    report->buttons = 0;
    for (index = 0; index < sizeof(buttons) / sizeof(buttons[0]); index++) {
        if (!gpio_read(buttons[index].gpio)) {
            report->buttons |= buttons[index].report_mask;
        }
    }

    report->hat = read_hat();
    report->left_x = 0x80;
    report->left_y = 0x80;
    report->right_x = 0x80;
    report->right_y = 0x80;
    report->left_trigger = 0;
    report->right_trigger = 0;
}
