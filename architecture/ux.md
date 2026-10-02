# UX tables and gestures

`settings_ux.c` (prefix `ux_`) is where the pad's feel is tuned.  It holds
three `const` tables and the small engine that reads the Home gestures.
Changing the feel means editing this file and rebuilding; nothing here is a
user setting.

| Table | What |
|---|---|
| `gestures[]` | Home chords and what they ask for |
| `ux_timeouts` | every fixed time in the firmware |
| `ux_scenes[]` | what the lights show in each state ([leds.md](leds.md)) |

## Gestures

The table, as it is in `settings_ux.c` (static: nothing outside reads it):

```c
#define SLOT_KEY 0x10000u        /* in .with: one of A B X Y up down left right */
#define TAP      0xffffffffu     /* in .hold_ms: on a release, nothing else fired */

enum { ALWAYS, CONNECTED, NOT_CONNECTED, OFFLINE };

static const struct gesture {
    const char *name;
    u32 with;                    /* held with Home; 0 = Home alone */
    u8 when;                     /* judged when Home went down */
    u32 hold_ms;                 /* 0 = at once */
    u8 request;
    u8 slot;                     /* for rows without a slot key */
} gestures[] = {
    { "home",         0,          CONNECTED,     TAP,  REQ_HOME_TAP,   SLOT_NONE },
    { "connect last", 0,          OFFLINE,       TAP,  REQ_CONNECT,    SLOT_LAST },
    { "sleep",        0,          ALWAYS,        5000, REQ_SLEEP,      SLOT_NONE },
    { "connect",      SLOT_KEY,   NOT_CONNECTED, 0,    REQ_CONNECT,    SLOT_NONE },
    { "switch",       SLOT_KEY,   CONNECTED,     2000, REQ_CONNECT,    SLOT_NONE },
    { "pair",         SLOT_KEY,   ALWAYS,        5000, REQ_PAIR,       SLOT_NONE },
    { "disconnect",   PAD_SELECT, ALWAYS,        2000, REQ_DISCONNECT, SLOT_NONE },
    { "calibrate",    PAD_M,      ALWAYS,        3000, REQ_CALIBRATE,  SLOT_NONE },
};
```

Rows are data only: they name a request, and the controller decides what the
request means now.

### The engine

```c
struct ctl_request ux_step(struct pad *pad, u32 now, u8 state);
u32 ux_deadline(void);         /* when ux_step may next fire a hold; 0: none */
void ux_ignore_held(void);     /* what is held now starts nothing until let go */
struct ctl_request ux_boot(u16 buttons, u8 hat, u8 state);   /* the press that woke the pad */
```

`ux_step()` is called whenever the buttons change, and at its own deadline
(`ux_deadline()`: the next hold time that could fire).  It follows Home and what is held
with it and returns at most one request.  With `keys_to_host` 0 it also
removes from `pad` what the host must not see (Home and the chord keys during
a gesture).
Rules:

* A gesture starts when Home goes down and ends when it comes up.
* The first thing held with Home is the chord.  A chord that changes (a key
  added or let go) ends the gesture; nothing more fires until all is let go.
* Each row fires at most once per gesture, so holding a slot key goes
  connect, then pair.
* Buttons not used by any row are ignored and pass through to the host.
* `keys_to_host` (next to the table) decides what the host sees.  1 (the
  default): Home and the chord keys go to the host exactly as pressed, and
  gestures fire as well.  0: they are held back during a gesture; connected,
  Home alone goes to the host once held longer than `home_grace_ms` (and
  chords stop counting), and a shorter press is shown to the host as a tap.

The engine keeps its own tracking state (when Home went down, the chord, the
rows fired); only the buttons change it.  `ux_ignore_held()` is how the
controller says "ignore this press" (after calibration ends, after a reset
handoff).

`ux_boot()` replays a press that woke the pad and was released before
the first poll: the rows that fire at once, or the tap rows if Home was alone.

Not a table row: Home+Start while USB is present enters the ROM loader.  The
controller checks it in `BOOT` and on USB plug-in, because it must work even
if this table is broken.

### Unit test

The table as shipped, driven a millisecond at a time: offline tap connects
the last slot; Home+B connects at once when not connected and needs 2 s when
connected; held on it pairs; Home+Select for 2 s disconnects; Home alone for
5 s sleeps without a tap; a changed chord fires nothing more; a boot press is
replayed.

## Timeouts

```c
const struct ux_timeouts ux_timeouts = {
    .home_grace_ms = 150,        /* connected: Home alone longer goes to the host */
    .home_tap_ms = 60,           /* how long a short Home press is shown to the host */
    .connect_ms = 30000,
    .pair_ms = 180000,
    .idle_sleep_ms = 300000,     /* connected, no input, on battery */
    .offline_sleep_ms = 15000,   /* radio off, on battery */
    .unplug_sleep_ms = 5000,     /* after USB goes away */
    .stop_max_ms = 12000,
    .boot_scene_ms = 500,
    .uboot_window_ms = 3000,     /* Home+Start at boot: USB may arrive this late */
    .led_frame_ms = 20,
    .usb_plug_flash_ms = 150,
};
```

Times that belong to a protocol, not to the feel, stay with that protocol (a
Switch keep-alive period in `profile_switch.c`, the Switch 1 wake duration in
`profile_switch_wake.c`, the cap on an Xbox one-shot rumble in
`profile_xbox.c`).
