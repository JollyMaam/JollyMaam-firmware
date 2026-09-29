#ifndef CYCLONE2_LIGHTS_H
#define CYCLONE2_LIGHTS_H

#include "generic/typedef.h"
#include "controller_state.h"

/* The pad's lights, worked out from scratch every frame: a pure function of
 * the controller state, the settings and what the host asked for.  There is
 * no stored "LED mode" to fall out of step with the controller.
 *
 * lights_render.c holds the scene table (what each state looks like) and the
 * renderer; it is free of SDK code and checked on the host
 * (tools/lights_test.c).  lights.c gathers the inputs and sends frames. */

enum cyclone2_light {
    CYCLONE2_LIGHT_HOME,
    CYCLONE2_LIGHT_PLAYER_0,        /* the bar under Home, top to bottom */
    CYCLONE2_LIGHT_PLAYER_1,
    CYCLONE2_LIGHT_PLAYER_2,
    CYCLONE2_LIGHT_PLAYER_3,
    CYCLONE2_LIGHT_GRIP_LEFT,
    CYCLONE2_LIGHT_GRIP_RIGHT,
    CYCLONE2_LIGHT_UNUSED,          /* driven dark; nothing visible there */
    CYCLONE2_LIGHT_COUNT,
};

#define CYCLONE2_PLAYER_LIGHTS 4U

struct cyclone2_rgb {
    u8 red;
    u8 green;
    u8 blue;
};

struct cyclone2_light_frame {
    struct cyclone2_rgb light[CYCLONE2_LIGHT_COUNT];
};

/* How a light's level moves over time, counted from when the controller
 * entered its state.  Each kind has only its own timing. */
enum cyclone2_wave_kind {
    CYCLONE2_WAVE_OFF,
    CYCLONE2_WAVE_SOLID,
    CYCLONE2_WAVE_BLINK,
    CYCLONE2_WAVE_BREATHE,
    /* One light at a time, back and forth across a group (the bar). */
    CYCLONE2_WAVE_SWEEP,
};

struct cyclone2_wave {
    enum cyclone2_wave_kind kind;
    union {
        struct { u16 on_ms; u16 off_ms; } blink;
        struct { u16 period_ms; } breathe;
        struct { u16 step_ms; } sweep;
    } as;
};

#define CYCLONE2_OFF        { .kind = CYCLONE2_WAVE_OFF }
#define CYCLONE2_SOLID      { .kind = CYCLONE2_WAVE_SOLID }
#define CYCLONE2_BLINK(on, off) \
    { .kind = CYCLONE2_WAVE_BLINK, .as.blink = { (on), (off) } }
#define CYCLONE2_BREATHE(period) \
    { .kind = CYCLONE2_WAVE_BREATHE, .as.breathe = { (period) } }
#define CYCLONE2_SWEEP(step) \
    { .kind = CYCLONE2_WAVE_SWEEP, .as.sweep = { (step) } }

/* Which of the four player lights a scene's bar uses. */
enum cyclone2_bar_shows {
    CYCLONE2_BAR_ALL,
    /* The one for the slot: A, B, X, Y top to bottom. */
    CYCLONE2_BAR_SLOT,
    /* The player number the host set; the slot's light until it does. */
    CYCLONE2_BAR_PLAYER,
};

struct cyclone2_bar_look {
    enum cyclone2_bar_shows shows;
    struct cyclone2_wave wave;
};

enum cyclone2_grips_show {
    CYCLONE2_GRIPS_WAVE,
    /* A nonblack host lightbar request lights the grips in the slot's
     * colour; they also answer to rumble and input (led.grips). */
    CYCLONE2_GRIPS_LIGHTBAR,
};

struct cyclone2_grips_look {
    enum cyclone2_grips_show shows;
    struct cyclone2_wave wave;      /* for CYCLONE2_GRIPS_WAVE */
};

enum cyclone2_colour_from {
    /* The colour of the slot the state is about (profile.N.colour). */
    CYCLONE2_COLOUR_SLOT,
    CYCLONE2_COLOUR_IDLE,
};

/* What the pad shows in one state. */
struct cyclone2_scene {
    const char *name;
    enum cyclone2_colour_from colour;
    struct cyclone2_wave home;
    struct cyclone2_bar_look bar;
    struct cyclone2_grips_look grips;
};

enum cyclone2_scene_id {
    CYCLONE2_SCENE_BOOT,
    CYCLONE2_SCENE_IDLE,
    CYCLONE2_SCENE_CONNECTING,
    CYCLONE2_SCENE_PAIRING,
    CYCLONE2_SCENE_CONNECTED,
    CYCLONE2_SCENE_DISCONNECTED,
    CYCLONE2_SCENE_WAKING,
    CYCLONE2_SCENE_SWITCHING,       /* one slot's session down, the next up */
    CYCLONE2_SCENE_OFF,             /* going to sleep or rebooting */
    CYCLONE2_SCENE_COUNT,
};

extern const struct cyclone2_scene cyclone2_scenes[CYCLONE2_SCENE_COUNT];

/* What the host set for this session.  Each part is optional. */
struct cyclone2_host_lights {
    u8 has_players;
    u8 players_on;                  /* bit 0 = top light */
    u8 players_flash;
    u8 has_lightbar;
    struct cyclone2_rgb lightbar;
};

/* Everything a frame depends on. */
struct cyclone2_lights_input {
    const struct cyclone2_controller_state *state;
    u32 now_ms;
    u32 since_boot_ms;
    struct cyclone2_rgb slot_colour[CYCLONE2_PLAYER_LIGHTS];
    struct cyclone2_rgb idle_colour;
    struct cyclone2_rgb battery_colour;
    u8 grip_effects;                /* enum config_grip_effects */
    struct cyclone2_host_lights host;
    u8 low_battery;
    /* Per grip, left then right, 0..255. */
    u8 rumble[2];
    u8 reach[2];                    /* nearby sticks, triggers, buttons */
    /* Calibration's own Home colour, already at its final level. */
    u8 has_calibration;
    struct cyclone2_rgb calibration;
};

#define CYCLONE2_BOOT_SCENE_MS 500U

enum cyclone2_scene_id cyclone2_scene_for(
    const struct cyclone2_controller_state *state, u32 since_boot_ms);
void cyclone2_lights_render(const struct cyclone2_lights_input *input,
                            struct cyclone2_light_frame *frame);

/* ---- runtime (lights.c) ---- */

void cyclone2_lights_initialize(void);
/* From the control poll, every tick; sends at most every frame period. */
struct cyclone2_input_report;
void cyclone2_lights_poll(u32 now_ms,
                          const struct cyclone2_input_report *input);

/* Host requests, from the profile code (any task).  They belong to the
 * session: cyclone2_lights_host_clear() drops them when a session starts
 * or its link goes down. */
void cyclone2_lights_host_players(u8 on, u8 flash);
void cyclone2_lights_host_lightbar(u8 red, u8 green, u8 blue);
void cyclone2_lights_host_clear(void);
void cyclone2_lights_host_get(struct cyclone2_host_lights *host);

/* For `status led`. */
struct cyclone2_lights_status {
    u8 scene;                       /* enum cyclone2_scene_id */
    u8 debug;
    u8 low_battery;
    u8 has_players;
    u8 players_on;
    u8 players_flash;
    u8 has_lightbar;
    u8 lightbar[3];
    u8 lit[CYCLONE2_LIGHT_COUNT * 3];   /* what was last sent, red green blue */
};
void cyclone2_lights_status_get(struct cyclone2_lights_status *status);

/* debug led: lights in mask show colour, the rest dark, until mask 0. */
void cyclone2_lights_debug(u32 mask, struct cyclone2_rgb colour);
void cyclone2_lights_debug_legend(void);

#endif
