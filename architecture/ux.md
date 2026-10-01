# UX tables and gestures

`settings_ux.c` (prefix `ux_`) is where the pad's feel is tuned.  It holds
three `const` tables and the small engine that reads the Home gestures.
Changing the feel means editing this file and rebuilding; nothing here is a
user setting.

| Table | What |
|---|---|
| `ux_gestures[]` | Home chords and what they ask for |
| `ux_timeouts` | every fixed time in the firmware |
| `ux_scenes[]` | what the lights show in each state ([leds.md](leds.md)) |

## Gestures

```c
#define UX_SLOT_KEY  0x10000u      /* in .with: one of A B X Y up down left right */
#define UX_TAP       0xffffffffu   /* in .hold_ms: on release, nothing else fired */

enum ux_when {
    UX_ALWAYS,
    UX_CONNECTED,        /* a host has the link */
    UX_NOT_CONNECTED,    /* any other state */
    UX_OFFLINE,          /* IDLE: the radio is off */
};

struct ux_gesture {
    const char *name;    /* logged when it fires */
    u32 with;            /* buttons held with Home, and/or UX_SLOT_KEY; 0 = Home alone */
    enum ux_when when;   /* judged from the state when Home went down */
    u32 hold_ms;         /* 0 = at once; UX_TAP = on release */
    u8 request;          /* REQ_CONNECT, REQ_PAIR, ... (controller.md) */
    u8 slot;             /* for REQ_CONNECT/REQ_PAIR without a slot key: SLOT_LAST */
};
```

The table as it stands:

```c
const struct ux_gesture ux_gestures[] = {
    { "home",       0,            UX_CONNECTED,     UX_TAP, REQ_HOME_TAP },
    { "connect last", 0,          UX_OFFLINE,       UX_TAP, REQ_CONNECT, SLOT_LAST },
    { "sleep",      0,            UX_ALWAYS,        5000,   REQ_SLEEP },
    { "connect",    UX_SLOT_KEY,  UX_NOT_CONNECTED, 0,      REQ_CONNECT },
    { "switch",     UX_SLOT_KEY,  UX_CONNECTED,     2000,   REQ_CONNECT },
    { "pair",       UX_SLOT_KEY,  UX_ALWAYS,        5000,   REQ_PAIR },
    { "disconnect", PAD_SELECT,   UX_ALWAYS,        2000,   REQ_DISCONNECT },
    { "calibrate",  PAD_M,        UX_ALWAYS,        3000,   REQ_CALIBRATE },
};
```

Rows are data only: they name a request, and the controller decides what the
request means now.

### The engine

```c
struct ctl_request ux_step(struct pad *pad, u32 now, enum ctl_state_kind state);
void ux_ignore_held(void);     /* what is held now starts nothing until let go */
void ux_boot_chord(u8 home, u8 slot);   /* a Home press seen at power-on */
```

`ux_step()` is called whenever the buttons change, and at its own deadline
(`ux_next_deadline()`: the next hold time that could fire).  It follows Home and what is held
with it, returns at most one request, and removes from `pad` what the host
must not see (Home and the chord keys during a gesture), adding nothing.
Rules:

* A gesture starts when Home goes down and ends when it comes up.
* The first thing held with Home is the chord.  A chord that changes (a key
  added or let go) ends the gesture; nothing more fires until all is let go.
* Each row fires at most once per gesture, so holding a slot key goes
  connect, then pair.
* Buttons not used by any row are ignored and pass through to the host.
* Connected, Home alone for more than `home_grace_ms` goes to the host as it
  is held, and chords stop counting.  A release within the grace is a tap.

The engine keeps its own tracking state (when Home went down, the chord, the
rows fired); only the buttons change it.  `ux_ignore_held()` is how the
controller says "ignore this press" (after calibration ends, after a reset
handoff).

`ux_boot_chord()` replays a press that woke the pad and was released before
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
struct ux_timeouts {
    u16 home_grace_ms;      /* 150: connected, Home alone longer than this goes live */
    u16 home_tap_ms;        /* 60: how long a tap is shown to the host */
    u32 connect_ms;         /* 30 000: give up connecting or reconnecting */
    u32 pair_ms;            /* 180 000: give up pairing */
    u32 idle_sleep_ms;      /* 300 000: connected, no input, on battery */
    u32 offline_sleep_ms;   /* 15 000: radio off, on battery */
    u16 unplug_sleep_ms;    /* 5 000: after USB goes away */
    u16 stop_max_ms;        /* 12 000: radio did not confirm it is down */
    u16 boot_scene_ms;      /* 500 */
    u16 uboot_window_ms;    /* 3 000: Home+Start at boot, USB may arrive this late */
};
extern const struct ux_timeouts ux_timeouts;
```

Times that belong to a protocol, not to the feel, stay with that protocol (a
Switch keep-alive period in `profile_switch.c`, the Switch 1 wake duration in
`profile_switch_wake.c`, the cap on an Xbox one-shot rumble in
`profile_xbox.c`).
