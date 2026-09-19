#include <asm/br23.h>
#include <asm/gpio.h>
#include <asm/mcpwm.h>
#include <system/timer.h>

#include "rumble.h"

#define RUMBLE_FREQUENCY_HZ 25000U

struct motor_config {
    u32 pin;
    pwm_ch_num_type channel;
    pwm_timer_num_type timer;
};

static const struct motor_config left_motor = {
    IO_PORTA_09, pwm_ch4, pwm_timer4
};
static const struct motor_config right_motor = {
    IO_PORTB_00, pwm_ch1, pwm_timer1
};

static struct cyclone2_rumble_status rumble_status;
static u16 stop_timeout;

static void motor_stop(const struct motor_config *motor)
{
    mcpwm_ch_open_or_close(motor->channel, 0U);
    JL_MCPWM->MCPWM_CON0 &= ~BIT((u32)motor->timer + 8U);
    gpio_set_pull_up(motor->pin, 0U);
    gpio_set_pull_down(motor->pin, 0U);
    gpio_set_die(motor->pin, 1U);
    gpio_direction_output(motor->pin, 0U);
}

static void motor_set(const struct motor_config *motor, u8 strength)
{
    struct pwm_platform_data config;

    if (strength == 0U) {
        motor_stop(motor);
        return;
    }

    memset(&config, 0, sizeof(config));
    config.frequency = RUMBLE_FREQUENCY_HZ;
    config.pwm_ch_num = motor->channel;
    config.pwm_timer_num = motor->timer;
    config.duty = ((u32)strength * 10000U) / 255U;
    config.pwm_aligned_mode = pwm_edge_aligned;
    config.h_pin = (u8)motor->pin;
    config.l_pin = (u8)-1;
    config.complementary_en = 0U;
    mcpwm_init(&config);
}

static void rumble_timeout(void *private_data)
{
    (void)private_data;
    stop_timeout = 0U;
    motor_stop(&left_motor);
    motor_stop(&right_motor);
    rumble_status.left = 0U;
    rumble_status.right = 0U;
    rumble_status.active = 0U;
    rumble_status.duration_ms = 0U;
}

void cyclone2_rumble_initialize(void)
{
    memset(&rumble_status, 0, sizeof(rumble_status));
    stop_timeout = 0U;
    motor_stop(&left_motor);
    motor_stop(&right_motor);
}

int cyclone2_rumble_set(u8 left, u8 right, u16 duration_ms)
{
    if (duration_ms > CYCLONE2_RUMBLE_MAX_DURATION_MS ||
        ((left != 0U || right != 0U) && duration_ms == 0U)) return -1;

    if (stop_timeout != 0U) {
        sys_timeout_del(stop_timeout);
        stop_timeout = 0U;
    }
    motor_set(&left_motor, left);
    motor_set(&right_motor, right);
    rumble_status.left = left;
    rumble_status.right = right;
    rumble_status.duration_ms = duration_ms;
    rumble_status.active = left != 0U || right != 0U;
    if (rumble_status.active) {
        stop_timeout = sys_timeout_add(NULL, rumble_timeout, duration_ms);
    }
    return 0;
}

void cyclone2_rumble_stop(void)
{
    if (stop_timeout != 0U) {
        sys_timeout_del(stop_timeout);
        stop_timeout = 0U;
    }
    rumble_timeout(NULL);
}

void cyclone2_rumble_get_status(struct cyclone2_rumble_status *status)
{
    if (status) *status = rumble_status;
}
