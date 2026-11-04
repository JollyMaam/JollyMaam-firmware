/* The board as the firmware and the tools see it: its controls, then its
 * defaults.  board_pad.c wires the pins to these inputs; the host schema
 * exporter builds this file too and checks the layout. */

/* Xbox order; the names docs/gamesir-nova-2-lite gives the buttons.  L4 and
 * R4 are the back paddles, the screenshot button the circle under Home.
 * Not placed yet. */
const struct board_control board_layout[] = {
    /* input         kind                 label      x  y */
    { IN_SOUTH,      LAYOUT_FACE,         "A",       0, 0 },
    { IN_EAST,       LAYOUT_FACE,         "B",       0, 0 },
    { IN_WEST,       LAYOUT_FACE,         "X",       0, 0 },
    { IN_NORTH,      LAYOUT_FACE,         "Y",       0, 0 },
    { IN_UP,         LAYOUT_DPAD,         "",        0, 0 },
    { IN_DOWN,       LAYOUT_DPAD,         "",        0, 0 },
    { IN_LEFT,       LAYOUT_DPAD,         "",        0, 0 },
    { IN_RIGHT,      LAYOUT_DPAD,         "",        0, 0 },
    { IN_LSTICK,     LAYOUT_STICK,        "",        0, 0 },
    { IN_L3,         LAYOUT_STICK_CLICK,  "",        0, 0 },
    { IN_RSTICK,     LAYOUT_STICK,        "",        0, 0 },
    { IN_R3,         LAYOUT_STICK_CLICK,  "",        0, 0 },
    { IN_L2,         LAYOUT_TRIGGER,      "",        0, 0 },
    { IN_R2,         LAYOUT_TRIGGER,      "",        0, 0 },
    { IN_L1,         LAYOUT_SHOULDER,     "LB",      0, 0 },
    { IN_R1,         LAYOUT_SHOULDER,     "RB",      0, 0 },
    { IN_L4,         LAYOUT_BACK,         "",        0, 0 },
    { IN_R4,         LAYOUT_BACK,         "",        0, 0 },
    { IN_SELECT,     LAYOUT_MENU,         "View",    0, 0 },
    { IN_START,      LAYOUT_MENU,         "Menu",    0, 0 },
    { IN_HOME,       LAYOUT_MENU,         "",        0, 0 },
    { IN_SCREENSHOT, LAYOUT_MENU,         "",        0, 0 },
    { IN_M,          LAYOUT_MENU,         "M",       0, 0 },
};
const u8 board_layout_count = LAYOUT_COUNT(board_layout);

/* The base remap the pad starts with: every key sends itself. */
const struct set_remap board_remap_base = { .name = "" };

const struct board_led_brightness board_led_brightness = {
    .steady = 40, .animation = 1600, .connect = 200,
    .idle = 200, .battery = 200,
};

const struct board_rumble board_rumble = {
    .min = { { 65, 65, 65, 70, 75 }, { 65, 65, 65, 70, 75 } },
    .max = { { 75, 85, 100, 100, 100 }, { 75, 85, 100, 100, 100 } },
    .kick_ms = { 0, 0, 0, 0, 0 },
};

/* Slots A-Y: plain HID over LE, DualSense, Xbox through the dongle, Switch 2. */
const struct board_slots board_slots = {
    .type = { PROFILE_GENERIC_BLE, PROFILE_DS5, PROFILE_XBOX, PROFILE_SWITCH2,
              PROFILE_NONE, PROFILE_NONE, PROFILE_NONE, PROFILE_NONE },
    .transport = { TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_DONGLE, TRANSPORT_BT_USB,
                   TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_BT_USB },
};
