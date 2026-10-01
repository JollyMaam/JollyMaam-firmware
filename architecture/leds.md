# LEDs

`leds.c` (prefix `led_`) has no state.  The controller keeps what it decides
about the lights, and every millisecond asks `leds.c` for the frame to show.

Lights: Home, the four player lights under it (top to bottom), the left and
right grips, and one unused position in the chain (driven dark).

## What the controller keeps

```c
struct led_state {
    enum led_scene_id scene;   /* follows the controller state */
    u32 scene_since_ms;        /* animations count from here */
    u8 slot;                   /* whose colour and bar position */
    u8 calibrating;
};
```

The controller sets `scene` and `scene_since_ms` whenever its state changes.
Nothing else changes this struct.  The lights the host set (player number,
lightbar) are not kept here: the loop reads them from the profile every poll
(`profile->host()`, [controller.md](controller.md)) and passes them in, or
passes none when there is no session.

## The one call

```c
void led_frame(const struct led_state *s, const struct profile_host *host,
               u32 now, const struct led_inputs *in, struct led_frame *out);

struct led_inputs {            /* what else the picture depends on */
    const struct pad *pad;     /* grips can follow input */
    u8 rumble_left, rumble_right;   /* grips can follow rumble */
    u8 battery_low, usb_present, charging;
    u8 cal_rgb[3]; u8 cal_lit;  /* the calibration routine's light (cal_led()) */
};
```

`led_frame()` is pure: the same arguments always give the same frame.  The
controller passes the result to `plat_leds()`.  This is the module's unit test:
each scene at a few times, each overlay.

## Scenes

What each controller state looks like is the table `ux_scenes[]` in
`settings_ux.c`.  One row per scene:

```c
[LED_SCENE_CONNECTING] = {
    .colour = LED_COLOUR_SLOT,                 /* profile.#.colour */
    .home   = LED_BLINK(500, 500),
    .bar    = { LED_BAR_SLOT, LED_SOLID },     /* the slot's light */
    .grips  = { LED_GRIPS_WAVE, LED_OFF },
},
```

Waves, timed from `scene_since_ms`: `LED_OFF`, `LED_SOLID`,
`LED_BLINK(on, off)`, `LED_BREATHE(period)`, `LED_SWEEP(step)`.  Bar: all four,
the slot's light, or the player number the host set.  Grips: a wave, or, when
connected, the slot's `grips` setting (rumble, input, lightbar).

Scenes, one per controller state: boot, idle, connecting (including a Switch 1
wake), pairing, connected, stopping (switching to another slot), sleeping,
rebooting.

Overlays drawn on top, in this order: host lights (player number, lightbar),
the calibration light, low battery, the USB charging cue.
