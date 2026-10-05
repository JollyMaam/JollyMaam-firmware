# LEDs

`leds.c` (prefix `led_`) has no state.  The controller keeps what it decides
about the lights, and at each LED deadline (every 10 ms while anything
animates) asks `leds.c` for the frame to show.

Lights: Home, the four player lights under it (top to bottom), the left and
right grips, and one unused position in the chain (driven dark).

## What the controller keeps, and the call

```c
struct led_state {
    u8 scene;              /* enum ctl_kind */
    u8 slot;               /* SLOT_NONE: none */
    u32 since_ms;
};

struct led_inputs {
    struct rgb connect_colour, primary_colour, secondary_colour, idle_colour, battery_colour;
    u8 grips, player_lights;   /* the slot's settings */
    u8 ephemeral;
    u8 rumble[2];
    u8 reach[2];
    u8 usb_powered, usb_plug_flash, low_battery;
    u8 has_players, players, players_flash;
    u8 has_lightbar;
    struct rgb lightbar;
    u8 cal_lit;
    struct rgb cal;
};

void led_frame(const struct led_scene *scene, const struct led_state *s, u32 now,
               const struct led_inputs *in, struct led_frame *out);
void led_reach(u8 reach[2], const struct pad *pad);   /* how far the sticks reach: grips can follow input */
```

The controller sets `scene` and `since_ms` whenever its state changes (the
STOPPING scene shows the slot it is switching to).  Nothing else changes
`led_state`.  The lights the host set (player number, lightbar) are not kept
here: the loop reads them from the profile every pass (`profile->host()`,
[controller.md](controller.md)) and passes them in `led_inputs`, or passes
none when not connected.

`led_frame()` is pure: the same arguments always give the same frame.  The
controller calls it at most every `led_frame_ms` and passes a changed result
to `plat_leds()`.  The unit test checks each scene at a few times, and each
overlay.

## Scenes

What each controller state looks like is the table `ux_scenes[]` in
`settings_ux.c`, indexed by controller state.  One row per state:

```c
/*                   colour          home                 bar      bar wave             grips       grips wave */
[CTL_CONNECTING] = { COLOUR_CONNECT, LED_BLINK(500, 500), BAR_ALL, LED_BLINK(500, 500), GRIPS_WAVE, LED_OFF },
```

Colour: `COLOUR_IDLE` (`ux_colours.idle`), `COLOUR_CONNECT` (the slot's
`connect_colour`: connecting, pairing, stopping) or `COLOUR_PRIMARY` (the
slot's `primary_colour`, dim red by default).

Waves, timed from `since_ms`: `LED_OFF`, `LED_SOLID`,
`LED_BLINK(on, off)`, `LED_BREATHE(period)`, `LED_SWEEP(step)`.  Bar: all four,
the slot's light, or (`BAR_PLAYER`, connected) the slot's `players` setting:
off, host (the player number in the secondary colour, the other lights in
the primary; dark until the host sets one), fade, primary, secondary.  Grips: a wave, or, when connected, the slot's `grips` setting:
off, host (the lightbar colour; dark if none), fade, rumble, input,
rumble+input (from the secondary colour towards the primary), primary,
secondary.  Fade is a slow breathe between the secondary and primary colours.

Scenes, one per controller state: boot, idle, connecting (including a Switch 1
wake), pairing, connected, stopping (switching to another slot), sleeping,
rebooting.

Overlays drawn on top of Home, in this order: the USB charging cue, low
battery, the USB plug flash, the ephemeral wink (dark for 100 ms twice every
4 s), the calibration light.
