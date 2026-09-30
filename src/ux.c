/* What Home does.  Every Home gesture is a row of the table below: what is
 * held with Home, when the row applies, how long to hold it and what it
 * does.  Tune the pad here; the engine under the table needs no change for a
 * new row.  Each gesture that fires is logged to the USB console as
 * "ux: NAME".
 *
 * Not in the table, because they are not Home gestures of the running pad:
 * Home+Start while USB is plugged in enters UBOOT (controller.c, usb_poll),
 * and holding the pad still keeps it awake only on USB (controller.c). */
#include "calibration.h"
#include "controller.h"
#include "ux.h"
#include "usb.h"

/* Connected, Home alone for longer than this goes to the host as it is held
 * (a console's Home long-press works), and chords no longer start. */
#define UX_HOME_GRACE_MS    150U
/* A Home released within the grace time is sent to the host as a press this
 * long, so a host sampling at its own rate sees it. */
#define UX_HOME_TAP_MS       60U
/* The slot a Home tap connects when none has been used since power was
 * lost: CYCLONE2_UX_NO_SLOT does nothing, or 0..7 for A B X Y up down left
 * right. */
#define UX_HOME_DEFAULT_SLOT CYCLONE2_UX_NO_SLOT

/* Held with Home: one slot key - A B X Y or the d-pad's up, down, left and
 * right - and the action gets its slot. */
#define UX_SLOT_KEY          0x10000U
/* hold_ms: fire when Home is let go without anything else held or fired. */
#define UX_TAP               0xffffffffU

enum ux_when {
    UX_ALWAYS,
    UX_CONNECTED,       /* a host has the link */
    UX_NOT_CONNECTED,   /* anything else: idle, connecting, pairing, ... */
    UX_OFFLINE,         /* nothing going on the radio: idle or disconnected */
};

struct ux_gesture {
    const char *name;
    /* Held together with Home: report buttons and/or UX_SLOT_KEY; 0 is Home
     * alone.  Exactly these; other buttons held do not matter. */
    u32 with;
    /* Judged from the state when Home went down. */
    enum ux_when when;
    /* From when the chord formed: 0 fires at once, UX_TAP on release. */
    u32 hold_ms;
    void (*action)(u8 slot);
};

static void home_to_host(u8 slot);
static void connect_last(u8 slot);
static void connect_slot(u8 slot);
static void pair_slot(u8 slot);
static void disconnect_host(u8 slot);
static void sleep_now(u8 slot);
static void calibrate(u8 slot);
static void tap_home(void);

static const struct ux_gesture ux_gestures[] = {
    /* Connected: Home reaches the host. */
    { .name = "home", .with = 0U, .when = UX_CONNECTED,
      .hold_ms = UX_TAP, .action = home_to_host },
    /* The slot last started since power-up, kept in RAM only. */
    { .name = "connect last", .with = 0U, .when = UX_OFFLINE,
      .hold_ms = UX_TAP, .action = connect_last },
    /* On USB power the pad stays awake and only disconnects. */
    { .name = "sleep", .with = 0U, .when = UX_ALWAYS,
      .hold_ms = 5000U, .action = sleep_now },
    { .name = "connect", .with = UX_SLOT_KEY, .when = UX_NOT_CONNECTED,
      .hold_ms = 0U, .action = connect_slot },
    /* Held, so a stray Home+button in a game does not drop the link. */
    { .name = "switch", .with = UX_SLOT_KEY, .when = UX_CONNECTED,
      .hold_ms = 2000U, .action = connect_slot },
    /* The slot waits for a host to pair with it. */
    { .name = "pair", .with = UX_SLOT_KEY, .when = UX_ALWAYS,
      .hold_ms = 5000U, .action = pair_slot },
    { .name = "disconnect", .with = CYCLONE2_REPORT_BUTTON_SELECT,
      .when = UX_ALWAYS, .hold_ms = 2000U, .action = disconnect_host },
    { .name = "calibrate", .with = CYCLONE2_REPORT_BUTTON_M,
      .when = UX_ALWAYS, .hold_ms = 3000U, .action = calibrate },
};

#define UX_GESTURE_COUNT (sizeof(ux_gestures) / sizeof(ux_gestures[0]))

static void home_to_host(u8 slot)
{
    (void)slot;
    tap_home();
}

static void connect_last(u8 slot)
{
    (void)slot;
    slot = cyclone2_last_slot();
    if (slot == CYCLONE2_UX_NO_SLOT) slot = UX_HOME_DEFAULT_SLOT;
    if (slot != CYCLONE2_UX_NO_SLOT) cyclone2_connect_request(slot);
}

static void connect_slot(u8 slot)
{
    cyclone2_connect_request(slot);
}

static void pair_slot(u8 slot)
{
    cyclone2_pair_request(slot);
}

static void disconnect_host(u8 slot)
{
    (void)slot;
    cyclone2_disconnect_request();
}

static void sleep_now(u8 slot)
{
    (void)slot;
    cyclone2_sleep_request();
}

static void calibrate(u8 slot)
{
    (void)slot;
    cyclone2_calibration_start();
}

/* The engine.  A gesture starts when Home goes down and ends when it comes
 * up.  The first thing held with Home is its chord; a chord that changes
 * after that (a key let go, another added) ends the gesture, and nothing
 * more fires until Home and the chord are all let go.  Each row fires at
 * most once per gesture, so a slot key held on goes connect, then pair. */

#define UX_FACE_BUTTONS \
    (CYCLONE2_REPORT_BUTTON_A | CYCLONE2_REPORT_BUTTON_B | \
     CYCLONE2_REPORT_BUTTON_X | CYCLONE2_REPORT_BUTTON_Y)

static struct {
    u8 active;      /* Home is down, and the gesture is ours */
    u8 live;        /* Home alone past the grace: it goes to the host */
    u8 draining;    /* over: wait for Home and the chord to be let go */
    u8 chorded;     /* something was held with Home */
    u8 fired;       /* a row fired */
    u8 slot;
    u8 tap;
    enum cyclone2_controller_kind kind;  /* when Home went down */
    u32 home_ms;
    u32 chord_ms;
    u32 chord;
    u32 done;       /* rows fired, a bit each */
    u32 tap_ms;
    u32 now;
} ux;

/* The buttons some row holds with Home: the rest are not the gesture's. */
static u16 chord_buttons(void)
{
    u32 buttons = 0U;
    unsigned i;

    for (i = 0U; i < UX_GESTURE_COUNT; i++) buttons |= ux_gestures[i].with;
    return (u16)(buttons & ~(u32)(CYCLONE2_REPORT_BUTTON_HOME | UX_FACE_BUTTONS));
}

/* Slot keys as a bit per slot: A B X Y, then the d-pad's up, down, left and
 * right.  A diagonal is two of them. */
static u8 slot_keys(const struct cyclone2_input_report *in)
{
    static const u8 hat_keys[CYCLONE2_HAT_NEUTRAL] = {
        0x10U, 0x10U | 0x80U, 0x80U, 0x20U | 0x80U,
        0x20U, 0x20U | 0x40U, 0x40U, 0x10U | 0x40U,
    };
    u8 keys = 0U;

    if (in->buttons & CYCLONE2_REPORT_BUTTON_A) keys |= 0x01U;
    if (in->buttons & CYCLONE2_REPORT_BUTTON_B) keys |= 0x02U;
    if (in->buttons & CYCLONE2_REPORT_BUTTON_X) keys |= 0x04U;
    if (in->buttons & CYCLONE2_REPORT_BUTTON_Y) keys |= 0x08U;
    if (in->hat < CYCLONE2_HAT_NEUTRAL) keys |= hat_keys[in->hat];
    return keys;
}

/* What is held with Home, and the slot if it is one slot key alone. */
static u32 held(const struct cyclone2_input_report *in, u8 *slot)
{
    u8 keys = slot_keys(in);
    u8 i;

    *slot = CYCLONE2_UX_NO_SLOT;
    for (i = 0U; i < 8U; i++)
        if (keys == (u8)(1U << i)) *slot = i;
    return (in->buttons & chord_buttons()) | (keys ? UX_SLOT_KEY : 0U);
}

static int applies(enum ux_when when, enum cyclone2_controller_kind kind)
{
    switch (when) {
    case UX_CONNECTED: return kind == CYCLONE2_CONTROLLER_CONNECTED;
    case UX_NOT_CONNECTED: return kind != CYCLONE2_CONTROLLER_CONNECTED;
    case UX_OFFLINE:
        return kind == CYCLONE2_CONTROLLER_IDLE ||
               kind == CYCLONE2_CONTROLLER_DISCONNECTED;
    default: return 1;
    }
}

static int matches(const struct ux_gesture *g, u32 chord, u8 slot,
                   enum cyclone2_controller_kind kind)
{
    return g->with == chord && applies(g->when, kind) &&
           (!(chord & UX_SLOT_KEY) || slot != CYCLONE2_UX_NO_SLOT);
}

static void fire(unsigned i, u8 slot)
{
    ux.done |= 1U << i;
    ux.fired = 1U;
    cyclone2_usb_debug("ux: %s", ux_gestures[i].name);
    ux_gestures[i].action(slot);
}

/* Rows due now: the taps on a release, else the holds. */
static void fire_due(u32 now, int release, u32 chord, u8 slot)
{
    unsigned i;

    for (i = 0U; i < UX_GESTURE_COUNT; i++) {
        const struct ux_gesture *g = &ux_gestures[i];

        if ((ux.done & (1U << i)) || !matches(g, chord, slot, ux.kind))
            continue;
        if (release ? g->hold_ms == UX_TAP :
            g->hold_ms != UX_TAP && now - ux.chord_ms >= g->hold_ms)
            fire(i, slot);
    }
}

void cyclone2_ux_poll(u32 now, const struct cyclone2_input_report *in,
                      enum cyclone2_controller_kind kind)
{
    int home = (in->buttons & CYCLONE2_REPORT_BUTTON_HOME) != 0U;
    u8 slot;
    u32 chord = held(in, &slot);

    ux.now = now;
    if (ux.draining) {
        if (!home && !chord) ux.draining = 0U;
        return;
    }
    if (!ux.active) {
        if (!home) return;
        ux.active = 1U;
        ux.live = ux.fired = 0U;
        ux.chorded = chord != 0U;
        ux.kind = kind;
        ux.home_ms = ux.chord_ms = now;
        ux.chord = chord;
        ux.slot = slot;
        ux.done = 0U;
    }
    if (!home) {
        ux.active = 0U;
        if (ux.live) return;
        if (!ux.chorded && !ux.fired) fire_due(now, 1, 0U, slot);
        ux.draining = chord != 0U;
        return;
    }
    if (ux.live) {
        /* Only Home alone counts now; the rest goes to the host. */
        fire_due(now, 0, 0U, CYCLONE2_UX_NO_SLOT);
        return;
    }
    if (chord != ux.chord || slot != ux.slot) {
        if (ux.chorded || ux.fired) {
            ux.active = 0U;
            ux.draining = 1U;
            return;
        }
        ux.chorded = 1U;
        ux.chord = chord;
        ux.slot = slot;
        ux.chord_ms = now;
    } else if (!ux.chorded && !ux.fired &&
               ux.kind == CYCLONE2_CONTROLLER_CONNECTED &&
               now - ux.home_ms >= UX_HOME_GRACE_MS) {
        ux.live = 1U;
    }
    fire_due(now, 0, ux.chord, ux.slot);
}

void cyclone2_ux_filter(struct cyclone2_input_report *report, u32 now)
{
    if ((ux.active && !ux.live) || ux.draining) {
        report->buttons &= (u16)~(CYCLONE2_REPORT_BUTTON_HOME |
                                  UX_FACE_BUTTONS | chord_buttons());
        report->hat = CYCLONE2_HAT_NEUTRAL;
    }
    if (ux.tap) {
        if (now - ux.tap_ms < UX_HOME_TAP_MS)
            report->buttons |= CYCLONE2_REPORT_BUTTON_HOME;
        else ux.tap = 0U;
    }
}

static void tap_home(void)
{
    ux.tap = 1U;
    ux.tap_ms = ux.now;
}

void cyclone2_ux_block(void)
{
    ux.active = ux.live = 0U;
    ux.draining = 1U;
}

void cyclone2_ux_boot(int early_home, u8 early_slot,
                      const struct cyclone2_input_report *in,
                      enum cyclone2_controller_kind kind)
{
    /* Still held: the first poll takes it from here. */
    if (!early_home || (in->buttons & CYCLONE2_REPORT_BUTTON_HOME)) return;
    ux.kind = kind;
    ux.chord_ms = 0U;
    ux.done = 0U;
    if (early_slot != CYCLONE2_UX_NO_SLOT) {
        /* Only what fires at once: the press is over. */
        unsigned i;

        for (i = 0U; i < UX_GESTURE_COUNT; i++)
            if (ux_gestures[i].hold_ms == 0U &&
                matches(&ux_gestures[i], UX_SLOT_KEY, early_slot, kind))
                fire(i, early_slot);
    } else {
        fire_due(0U, 1, 0U, CYCLONE2_UX_NO_SLOT);
    }
    ux.draining = held(in, &early_slot) != 0U;
}
