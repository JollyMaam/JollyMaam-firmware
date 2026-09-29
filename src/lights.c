/* The lights at run time: gather what a frame depends on, render it
 * (lights_render.c) and send it when it differs from what is lit. */
#include <system/includes.h>
#include <asm/adc_api.h>

#include "calibration.h"
#include "config.h"
#include "controller.h"
#include "input.h"
#include "led.h"
#include "lights.h"
#include "rumble.h"

/* 50 frames a second.  A send busy-waits ~0.33 ms on the SPI (reset gap
 * plus 192 data bytes), so it is not done every control tick. */
#define LIGHTS_FRAME_MS            20U
/* On battery.  No discharge curve has been measured; these are the usual
 * single-cell figures.  On USB the reading is the supply, so it is not
 * judged at all. */
#define LIGHTS_LOW_BATTERY_MV    3500U
#define LIGHTS_BATTERY_OK_MV     3600U
#define LIGHTS_BATTERY_CHECK_MS  1000U
/* How fast a grip fades after its side's input stops, per frame. */
#define LIGHTS_REACH_DECAY         12U
/* Stick or trigger travel (of 255) that is rest and drift, not use. */
#define LIGHTS_REACH_QUIET         24U

static volatile u32 host_players;       /* 1 << 16 | flash << 8 | on */
static volatile u32 host_lightbar;      /* 1 << 24 | red << 16 | green << 8 | blue */

static u8 debug_active;
static struct cyclone2_light_frame debug_frame;

static struct cyclone2_light_frame shown;
static u8 shown_valid;
static u32 boot_ms;
static u32 next_frame_ms;
static u32 battery_checked_ms;
static u8 low_battery;
static u8 reach[2];
static u8 reach_peak[2];

void cyclone2_lights_initialize(void)
{
    boot_ms = sys_timer_get_ms();
    next_frame_ms = boot_ms;
    battery_checked_ms = boot_ms - LIGHTS_BATTERY_CHECK_MS;
}

void cyclone2_lights_host_players(u8 on, u8 flash)
{
    host_players = 1UL << 16 | (u32)(flash & 0x0fU) << 8 | (on & 0x0fU);
}

void cyclone2_lights_host_lightbar(u8 red, u8 green, u8 blue)
{
    host_lightbar = 1UL << 24 | (u32)red << 16 | (u32)green << 8 | blue;
}

void cyclone2_lights_host_clear(void)
{
    host_players = 0U;
    host_lightbar = 0U;
}

void cyclone2_lights_host_get(struct cyclone2_host_lights *host)
{
    u32 players = host_players;
    u32 lightbar = host_lightbar;

    host->has_players = (players >> 16) & 1U;
    host->players_on = (u8)players;
    host->players_flash = (u8)(players >> 8);
    host->has_lightbar = (lightbar >> 24) & 1U;
    host->lightbar.red = (u8)(lightbar >> 16);
    host->lightbar.green = (u8)(lightbar >> 8);
    host->lightbar.blue = (u8)lightbar;
}

void cyclone2_lights_debug(u32 mask, struct cyclone2_rgb colour)
{
    u32 light;

    memset(&debug_frame, 0, sizeof(debug_frame));
    for (light = 0U; light < CYCLONE2_LIGHT_COUNT; light++)
        if (mask & (1U << light)) debug_frame.light[light] = colour;
    debug_active = mask != 0U;
}

void cyclone2_lights_debug_legend(void)
{
    static const struct cyclone2_rgb legend[CYCLONE2_LIGHT_COUNT] = {
        { 48, 0, 0 }, { 0, 48, 0 }, { 0, 0, 48 }, { 48, 48, 0 },
        { 0, 48, 48 }, { 48, 0, 48 }, { 40, 40, 40 }, { 60, 16, 0 },
    };

    memcpy(debug_frame.light, legend, sizeof(legend));
    debug_active = 1U;
}

static u8 stick_reach(s16 x, s16 y)
{
    s32 ax = x < 0 ? -(s32)x : x;
    s32 ay = y < 0 ? -(s32)y : y;
    s32 most = ax > ay ? ax : ay;

    return (u8)(most >= CYCLONE2_STICK_MAX ? 255 : most >> 7);
}

static u8 larger(u8 a, u8 b)
{
    return a > b ? a : b;
}

static u8 beyond_rest(u8 level)
{
    return level < LIGHTS_REACH_QUIET ? 0U : level;
}

/* How much is going on near each grip: sticks and triggers by how far they
 * are pushed, buttons fully. */
static void sample_reach(const struct cyclone2_input_report *in)
{
    static const u16 left_buttons = CYCLONE2_REPORT_BUTTON_L1 |
        CYCLONE2_REPORT_BUTTON_L4 | CYCLONE2_REPORT_BUTTON_L3 |
        CYCLONE2_REPORT_BUTTON_SELECT;
    static const u16 right_buttons = CYCLONE2_REPORT_BUTTON_R1 |
        CYCLONE2_REPORT_BUTTON_R4 | CYCLONE2_REPORT_BUTTON_R3 |
        CYCLONE2_REPORT_BUTTON_START | CYCLONE2_REPORT_BUTTON_A |
        CYCLONE2_REPORT_BUTTON_B | CYCLONE2_REPORT_BUTTON_X |
        CYCLONE2_REPORT_BUTTON_Y;
    u8 left = beyond_rest(larger(stick_reach(in->left_x, in->left_y),
                                 cyclone2_trigger_u8(in->left_trigger)));
    u8 right = beyond_rest(larger(stick_reach(in->right_x, in->right_y),
                                  cyclone2_trigger_u8(in->right_trigger)));

    if ((in->buttons & left_buttons) || in->hat != CYCLONE2_HAT_NEUTRAL)
        left = 255U;
    if (in->buttons & right_buttons) right = 255U;
    reach_peak[0] = larger(reach_peak[0], left);
    reach_peak[1] = larger(reach_peak[1], right);
}

static void update_reach(void)
{
    u32 side;

    for (side = 0U; side < 2U; side++) {
        u8 faded = reach[side] > LIGHTS_REACH_DECAY ?
                   (u8)(reach[side] - LIGHTS_REACH_DECAY) : 0U;

        reach[side] = larger(reach_peak[side], faded);
        reach_peak[side] = 0U;
    }
}

static void update_battery(u32 now)
{
    u32 millivolts;

    if (cyclone2_usb_present()) {
        low_battery = 0U;
        return;
    }
    if (now - battery_checked_ms < LIGHTS_BATTERY_CHECK_MS) return;
    battery_checked_ms = now;
    millivolts = adc_get_voltage(AD_CH_VBAT) * 4U;
    if (millivolts < LIGHTS_LOW_BATTERY_MV) low_battery = 1U;
    else if (millivolts >= LIGHTS_BATTERY_OK_MV) low_battery = 0U;
}

static void gather(u32 now, struct cyclone2_lights_input *in)
{
    struct cyclone2_rumble_status rumble;
    u32 slot;

    memset(in, 0, sizeof(*in));
    in->state = cyclone2_controller_state();
    in->now_ms = now;
    in->since_boot_ms = now - boot_ms;
    for (slot = 0U; slot < CYCLONE2_PLAYER_LIGHTS; slot++) {
        in->slot_colour[slot].red = config.profile[slot].colour[0];
        in->slot_colour[slot].green = config.profile[slot].colour[1];
        in->slot_colour[slot].blue = config.profile[slot].colour[2];
    }
    in->brightness = config.led.brightness;
    in->grip_effects = config.led.grips;
    cyclone2_lights_host_get(&in->host);
    in->low_battery = low_battery;
    cyclone2_rumble_get_status(&rumble);
    if (rumble.active) {
        in->rumble[0] = rumble.left;
        in->rumble[1] = rumble.right;
    }
    in->reach[0] = reach[0];
    in->reach[1] = reach[1];
    in->has_calibration = (u8)cyclone2_calibration_led(
        now, &in->calibration.red, &in->calibration.green,
        &in->calibration.blue);
}

void cyclone2_lights_poll(u32 now, const struct cyclone2_input_report *input)
{
    struct cyclone2_lights_input in;
    struct cyclone2_light_frame frame;

    sample_reach(input);
    if ((s32)(now - next_frame_ms) < 0) return;
    next_frame_ms = now + LIGHTS_FRAME_MS;
    update_reach();
    update_battery(now);

    if (debug_active) {
        frame = debug_frame;
    } else {
        gather(now, &in);
        cyclone2_lights_render(&in, &frame);
    }
    if (shown_valid && !memcmp(&frame, &shown, sizeof(frame))) return;
    shown = frame;
    shown_valid = 1U;
    cyclone2_led_write(&frame);
}

void cyclone2_lights_status_get(struct cyclone2_lights_status *status)
{
    struct cyclone2_host_lights host;
    u32 light;

    cyclone2_lights_host_get(&host);
    status->scene = (u8)cyclone2_scene_for(cyclone2_controller_state(),
                                           sys_timer_get_ms() - boot_ms);
    status->debug = debug_active;
    status->low_battery = low_battery;
    status->has_players = host.has_players;
    status->players_on = host.players_on;
    status->players_flash = host.players_flash;
    status->has_lightbar = host.has_lightbar;
    status->lightbar[0] = host.lightbar.red;
    status->lightbar[1] = host.lightbar.green;
    status->lightbar[2] = host.lightbar.blue;
    for (light = 0U; light < CYCLONE2_LIGHT_COUNT; light++) {
        status->lit[light * 3U] = shown.light[light].red;
        status->lit[light * 3U + 1U] = shown.light[light].green;
        status->lit[light * 3U + 2U] = shown.light[light].blue;
    }
}
