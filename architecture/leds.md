# LEDs

`leds.c` (prefix `led_`) works out what the eight lights show, from scratch
every frame.  There is no stored "LED mode": a frame is a function of the
controller state, the settings, what the host asked for, and the time.

Lights: Home, the four player lights under it (top to bottom), the left and
right grips, and one unused position in the chain (driven dark).

## Scenes

What each controller state looks like is the table `ux_scenes[]` in
`settings_ux.c`.  One row per state:

```c
[LED_SCENE_CONNECTING] = {
    .colour = LED_COLOUR_SLOT,                 /* profile.#.colour */
    .home   = LED_BLINK(500, 500),
    .bar    = { LED_BAR_SLOT, LED_SOLID },     /* the slot's light */
    .grips  = { LED_GRIPS_WAVE, LED_OFF },
},
```

Waves: `LED_OFF`, `LED_SOLID`, `LED_BLINK(on, off)`, `LED_BREATHE(period)`,
`LED_SWEEP(step)`, timed from when the controller entered its state.  Bar
modes: all four, the slot's light, or the player number the host set.  Grips:
a wave, or, when linked, the slot's `grips` setting (rumble, input, lightbar).

Scenes: boot, idle, connecting, pairing, linked, disconnected, waking,
switching (between slots), off (going to sleep or rebooting).

Overlays drawn on top of the scene, in this order: host lights (player number,
lightbar), the calibration routine's light (`cal_led()`), low battery, the
USB charging cue.

## Interface

```c
void led_poll(u32 now, const struct ctl_state *s, const struct pad *pad);
void led_render(const struct led_inputs *in, struct led_frame *out);  /* pure */

/* latches, safe from any thread (profiles' write handlers) */
void led_host_players(u8 mask, u8 flash);
void led_host_lightbar(u8 r, u8 g, u8 b);
void led_host_clear(void);
```

`led_poll()` gathers the inputs, calls `led_render()` and passes the frame to
`plat_leds(const struct led_frame *)`, which drives the LED chain over SPI.
`led_render()` is pure and carries the unit test (each state's look, each
overlay).
