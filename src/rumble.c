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
static volatile u8 requested_left;
static volatile u8 requested_right;
static volatile u16 requested_duration_ms;
static volatile u8 request_generation;
static u8 applied_generation;
static u32 expires_at_ms;

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

static void motors_stop(void)
{
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
    requested_left = 0U;
    requested_right = 0U;
    requested_duration_ms = 0U;
    request_generation = 0U;
    applied_generation = 0U;
    expires_at_ms = 0U;
    motors_stop();
}

static void publish_target(u8 left, u8 right, u16 duration_ms)
{
    /* Bluetooth output callbacks only publish a latest target. The app_core
     * poll owns all PWM register changes. generation is written last. */
    requested_left = left;
    requested_right = right;
    requested_duration_ms = duration_ms;
    request_generation++;
    rumble_status.last_request_ms = sys_timer_get_ms();
    rumble_status.updates++;
    rumble_status.left = left;
    rumble_status.right = right;
    rumble_status.duration_ms = duration_ms;
    rumble_status.active = left != 0U || right != 0U;
}

int cyclone2_rumble_set(u8 left, u8 right, u16 duration_ms)
{
    if (duration_ms > CYCLONE2_RUMBLE_MAX_DURATION_MS ||
        ((left != 0U || right != 0U) && duration_ms == 0U)) return -1;
    publish_target(left, right, duration_ms);
    return 0;
}

void cyclone2_rumble_set_latched(u8 left, u8 right)
{
    publish_target(left, right, 0U);
}

void cyclone2_rumble_stop(void)
{
    cyclone2_rumble_set(0U, 0U, 0U);
}

void cyclone2_rumble_poll(u32 now_ms)
{
    if (applied_generation != request_generation) {
        u8 generation = request_generation;
        u8 left = requested_left;
        u8 right = requested_right;
        u16 duration_ms = requested_duration_ms;

        motor_set(&left_motor, left);
        motor_set(&right_motor, right);
        expires_at_ms = now_ms + duration_ms;
        applied_generation = generation;
        rumble_status.last_apply_ms = now_ms;
        rumble_status.last_apply_latency_ms =
            now_ms - rumble_status.last_request_ms;
        if (rumble_status.last_apply_latency_ms >
            rumble_status.max_apply_latency_ms) {
            rumble_status.max_apply_latency_ms =
                rumble_status.last_apply_latency_ms;
        }
    }
    if (rumble_status.active && rumble_status.duration_ms != 0U &&
        (s32)(now_ms - expires_at_ms) >= 0) {
        motors_stop();
    }
}

void cyclone2_rumble_get_status(struct cyclone2_rumble_status *status)
{
    if (status) *status = rumble_status;
}
