/* What the lights show: the scene table, and the renderer that turns the
 * controller state and the other inputs into a frame.  No SDK code here;
 * tools/lights_test.c runs it on the host. */
#include <stddef.h>
#include <string.h>

#include "config.h"
#include "lights.h"

#define WHITE { 0xff, 0xff, 0xff }

/* What each state looks like.  The colour is the slot's own
 * (profile.N.colour) unless a scene fixes one; everything is scaled by
 * led.brightness afterwards. */
const struct cyclone2_scene cyclone2_scenes[CYCLONE2_SCENE_COUNT] = {
    [CYCLONE2_SCENE_BOOT] = {
        .name = "boot",
        .colour = { CYCLONE2_COLOUR_FIXED, WHITE },
        .home = CYCLONE2_SOLID,
        .bar = { CYCLONE2_BAR_ALL, CYCLONE2_OFF },
        .grips = { CYCLONE2_GRIPS_WAVE, CYCLONE2_OFF },
    },
    /* Awake with no slot chosen: Home+A/B/X/Y picks one. */
    [CYCLONE2_SCENE_IDLE] = {
        .name = "idle",
        .colour = { CYCLONE2_COLOUR_FIXED, WHITE },
        .home = CYCLONE2_BREATHE(4000),
        .bar = { CYCLONE2_BAR_ALL, CYCLONE2_OFF },
        .grips = { CYCLONE2_GRIPS_WAVE, CYCLONE2_OFF },
    },
    [CYCLONE2_SCENE_CONNECTING] = {
        .name = "connecting",
        .colour = { CYCLONE2_COLOUR_SLOT },
        .home = CYCLONE2_BLINK(500, 500),
        .bar = { CYCLONE2_BAR_SLOT, CYCLONE2_SOLID },
        .grips = { CYCLONE2_GRIPS_WAVE, CYCLONE2_OFF },
    },
    [CYCLONE2_SCENE_PAIRING] = {
        .name = "pairing",
        .colour = { CYCLONE2_COLOUR_SLOT },
        .home = CYCLONE2_BLINK(120, 120),
        .bar = { CYCLONE2_BAR_ALL, CYCLONE2_SWEEP(120) },
        .grips = { CYCLONE2_GRIPS_WAVE, CYCLONE2_OFF },
    },
    [CYCLONE2_SCENE_CONNECTED] = {
        .name = "connected",
        .colour = { CYCLONE2_COLOUR_SLOT },
        .home = CYCLONE2_SOLID,
        .bar = { CYCLONE2_BAR_PLAYER, CYCLONE2_SOLID },
        .grips = { CYCLONE2_GRIPS_LIGHTBAR },
    },
    /* The link went away and the pad is not trying (it stays awake on USB;
     * on battery it goes to sleep instead). */
    [CYCLONE2_SCENE_DISCONNECTED] = {
        .name = "disconnected",
        .colour = { CYCLONE2_COLOUR_SLOT },
        .home = CYCLONE2_BREATHE(3000),
        .bar = { CYCLONE2_BAR_SLOT, CYCLONE2_OFF },
        .grips = { CYCLONE2_GRIPS_WAVE, CYCLONE2_OFF },
    },
    [CYCLONE2_SCENE_WAKING] = {
        .name = "waking",
        .colour = { CYCLONE2_COLOUR_SLOT },
        .home = CYCLONE2_BLINK(60, 240),
        .bar = { CYCLONE2_BAR_SLOT, CYCLONE2_BLINK(60, 240) },
        .grips = { CYCLONE2_GRIPS_WAVE, CYCLONE2_OFF },
    },
    /* The bar already shows the slot being switched to. */
    [CYCLONE2_SCENE_SWITCHING] = {
        .name = "switching",
        .colour = { CYCLONE2_COLOUR_SLOT },
        .home = CYCLONE2_OFF,
        .bar = { CYCLONE2_BAR_SLOT, CYCLONE2_SOLID },
        .grips = { CYCLONE2_GRIPS_WAVE, CYCLONE2_OFF },
    },
    [CYCLONE2_SCENE_OFF] = {
        .name = "off",
        .colour = { CYCLONE2_COLOUR_FIXED },
        .home = CYCLONE2_OFF,
        .bar = { CYCLONE2_BAR_ALL, CYCLONE2_OFF },
        .grips = { CYCLONE2_GRIPS_WAVE, CYCLONE2_OFF },
    },
};

/* Overlays, drawn over whatever the scene shows. */
static const struct cyclone2_rgb low_battery_colour = { 0xff, 0x00, 0x00 };
static const struct cyclone2_wave low_battery_wave = CYCLONE2_BLINK(150, 2850);
/* Switch player lights the host set flashing. */
static const struct cyclone2_wave player_flash_wave = CYCLONE2_BLINK(250, 250);

enum cyclone2_scene_id cyclone2_scene_for(
    const struct cyclone2_controller_state *state, u32 since_boot_ms)
{
    switch (state->kind) {
    case CYCLONE2_CONTROLLER_IDLE:
        return since_boot_ms < CYCLONE2_BOOT_SCENE_MS ?
               CYCLONE2_SCENE_BOOT : CYCLONE2_SCENE_IDLE;
    case CYCLONE2_CONTROLLER_CONNECTING: return CYCLONE2_SCENE_CONNECTING;
    case CYCLONE2_CONTROLLER_PAIRING: return CYCLONE2_SCENE_PAIRING;
    case CYCLONE2_CONTROLLER_CONNECTED: return CYCLONE2_SCENE_CONNECTED;
    case CYCLONE2_CONTROLLER_DISCONNECTED: return CYCLONE2_SCENE_DISCONNECTED;
    case CYCLONE2_CONTROLLER_WAKING: return CYCLONE2_SCENE_WAKING;
    case CYCLONE2_CONTROLLER_STOPPING:
        return state->as.stopping.after == CYCLONE2_AFTER_STOP_START ||
               state->as.stopping.after == CYCLONE2_AFTER_STOP_WAKE ?
               CYCLONE2_SCENE_SWITCHING : CYCLONE2_SCENE_OFF;
    default:
        return CYCLONE2_SCENE_OFF;
    }
}

/* 0..255 at time t into the state, for light `position` of `count` lit
 * together (a sweep moves across them). */
static u8 wave_level(const struct cyclone2_wave *wave, u32 t, u32 position,
                     u32 count)
{
    u32 phase;
    u32 steps;
    u32 lit;

    switch (wave->kind) {
    case CYCLONE2_WAVE_SOLID:
        return 255U;
    case CYCLONE2_WAVE_BLINK:
        steps = (u32)wave->as.blink.on_ms + wave->as.blink.off_ms;
        if (!steps) return 0U;
        return t % steps < wave->as.blink.on_ms ? 255U : 0U;
    case CYCLONE2_WAVE_BREATHE: {
        u32 period = wave->as.breathe.period_ms ? wave->as.breathe.period_ms : 1U;
        u32 rise;

        /* Up and down again, squared so it lingers near dark as an eye
         * expects; it starts lit, as the state begins. */
        phase = (t + period / 2U) % period;
        rise = phase < period / 2U ? phase * 510U / period :
               (period - phase) * 510U / period;
        if (rise > 255U) rise = 255U;
        return (u8)(rise * rise / 255U);
    }
    case CYCLONE2_WAVE_SWEEP:
        if (count < 2U) return 255U;
        steps = 2U * (count - 1U);
        phase = (t / (wave->as.sweep.step_ms ? wave->as.sweep.step_ms : 1U)) % steps;
        lit = phase < count ? phase : steps - phase;
        return position == lit ? 255U : 0U;
    default:
        return 0U;
    }
}

static struct cyclone2_rgb scaled(struct cyclone2_rgb colour, u32 level,
                                  u32 brightness)
{
    u32 scale = level * brightness;
    struct cyclone2_rgb out;

    out.red = (u8)((colour.red * scale + 255U * 50U) / (255U * 100U));
    out.green = (u8)((colour.green * scale + 255U * 50U) / (255U * 100U));
    out.blue = (u8)((colour.blue * scale + 255U * 50U) / (255U * 100U));
    return out;
}

static u32 count_bits(u32 mask)
{
    u32 count = 0U;

    for (; mask; mask &= mask - 1U) count++;
    return count;
}

static void render_bar(const struct cyclone2_scene *scene,
                       const struct cyclone2_lights_input *in, u32 t,
                       int has_slot, u8 slot, struct cyclone2_rgb colour,
                       u32 brightness, struct cyclone2_light_frame *frame)
{
    u32 on = 0U;
    u32 flash = 0U;
    u32 count;
    u32 position = 0U;
    u32 light;

    switch (scene->bar.shows) {
    case CYCLONE2_BAR_ALL:
        on = (1U << CYCLONE2_PLAYER_LIGHTS) - 1U;
        break;
    case CYCLONE2_BAR_PLAYER:
        if (in->host.has_players) {
            on = in->host.players_on & 0x0fU;
            flash = in->host.players_flash & 0x0fU & ~on;
        } else if (has_slot) {
            on = 1U << slot;        /* until the host gives a number */
        }
        break;
    case CYCLONE2_BAR_SLOT:
        if (has_slot) on = 1U << slot;
        break;
    }
    count = count_bits(on);
    for (light = 0U; light < CYCLONE2_PLAYER_LIGHTS; light++) {
        u32 level = 0U;

        if (on & (1U << light))
            level = wave_level(&scene->bar.wave, t, position++, count);
        else if (flash & (1U << light))
            level = wave_level(&scene->bar.wave, t, 0U, 1U) *
                    wave_level(&player_flash_wave, in->now_ms, 0U, 1U) / 255U;
        frame->light[CYCLONE2_LIGHT_PLAYER_0 + light] =
            scaled(colour, level, brightness);
    }
}

static void render_grips(const struct cyclone2_scene *scene,
                         const struct cyclone2_lights_input *in, u32 t,
                         struct cyclone2_rgb colour, u32 brightness,
                         struct cyclone2_light_frame *frame)
{
    u32 side;

    if (scene->grips.shows == CYCLONE2_GRIPS_WAVE) {
        u32 level = wave_level(&scene->grips.wave, t, 0U, 1U);

        frame->light[CYCLONE2_LIGHT_GRIP_LEFT] = scaled(colour, level, brightness);
        frame->light[CYCLONE2_LIGHT_GRIP_RIGHT] = scaled(colour, level, brightness);
        return;
    }
    if (in->host.has_lightbar) colour = in->host.lightbar;
    for (side = 0U; side < 2U; side++) {
        u32 level = in->host.has_lightbar ? 255U : 0U;

        if ((in->grip_effects & CONFIG_GRIPS_RUMBLE) && in->rumble[side] > level)
            level = in->rumble[side];
        if ((in->grip_effects & CONFIG_GRIPS_INPUT) && in->reach[side] > level)
            level = in->reach[side];
        frame->light[CYCLONE2_LIGHT_GRIP_LEFT + side] =
            scaled(colour, level, brightness);
    }
}

void cyclone2_lights_render(const struct cyclone2_lights_input *in,
                            struct cyclone2_light_frame *frame)
{
    const struct cyclone2_scene *scene =
        &cyclone2_scenes[cyclone2_scene_for(in->state, in->since_boot_ms)];
    u32 t = in->now_ms - in->state->entered_ms;
    u32 brightness = in->brightness;
    struct cyclone2_rgb colour = scene->colour.fixed;
    u8 slot = 0U;
    int has_slot = cyclone2_controller_slot(in->state, &slot);

    memset(frame, 0, sizeof(*frame));
    if (brightness > 100U) brightness = 100U;
    if (scene->colour.from == CYCLONE2_COLOUR_SLOT) {
        struct cyclone2_rgb white = WHITE;

        colour = has_slot ? in->slot_colour[slot] : white;
    }

    frame->light[CYCLONE2_LIGHT_HOME] =
        scaled(colour, wave_level(&scene->home, t, 0U, 1U), brightness);
    render_bar(scene, in, t, has_slot, slot, colour, brightness, frame);
    render_grips(scene, in, t, colour, brightness, frame);

    if (in->low_battery && wave_level(&low_battery_wave, in->now_ms, 0U, 1U))
        frame->light[CYCLONE2_LIGHT_HOME] =
            scaled(low_battery_colour, 255U, brightness);
    if (in->has_calibration)
        frame->light[CYCLONE2_LIGHT_HOME] = in->calibration;
}
