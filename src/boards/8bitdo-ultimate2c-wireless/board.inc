/* The board as the firmware and the tools see it: its controls, then its
 * defaults.  board_pad.c wires the pins to these inputs; the host schema
 * exporter builds this file too and checks the layout. */

/* Xbox order; the names docs/8bitdo-ultimate2c-wireless gives the buttons.
 * L4 and R4 are the extra buttons on top, M the Turbo button.  Not placed
 * yet. */
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
    { IN_L1,         LAYOUT_SHOULDER,     "",        0, 0 },
    { IN_R1,         LAYOUT_SHOULDER,     "",        0, 0 },
    { IN_L4,         LAYOUT_TOP,          "",        0, 0 },
    { IN_R4,         LAYOUT_TOP,          "",        0, 0 },
    { IN_SELECT,     LAYOUT_MENU,         "View",    0, 0 },
    { IN_START,      LAYOUT_MENU,         "Menu",    0, 0 },
    { IN_HOME,       LAYOUT_MENU,         "",        0, 0 },
    { IN_SCREENSHOT, LAYOUT_MENU,         "Capture", 0, 0 },
    { IN_M,          LAYOUT_MENU,         "Turbo",   0, 0 },
};
const u8 board_layout_count = LAYOUT_COUNT(board_layout);

/* The base remap the pad starts with: every key sends itself. */
const struct set_remap board_remap_base = { .name = "" };

/* Lights in tenths of their smallest step: the Home ring is plain on this
 * pad, 24 of 255 looks right lit steadily. */
const struct board_led_brightness board_led_brightness = {
    .steady = 240, .animation = 400, .connect = 240,
    .idle = 240, .battery = 240,
};

/* Both motors start at 40% and grow stronger up to 100%: step 3 (the
 * default) spans that, lower steps cap it, higher ones raise the floor.
 * controller_lite has no kick. */
const struct board_rumble board_rumble = {
    .min = { { 32, 32, 32, 45, 55 }, { 32, 32, 32, 45, 55 } },
    .max = { { 65, 85, 100, 100, 100 }, { 65, 85, 100, 100, 100 } },
    .kick_ms = { 0, 0, 0, 0, 0 },
};

/* Four LE slots share Generic BLE until the other profile tables fit. */
const struct board_slots board_slots = {
    .type = { PROFILE_GENERIC_BLE, PROFILE_DS5EDGE_BLE, PROFILE_XBOX_BLE, PROFILE_SWITCH2,   /* A B X Y */
              PROFILE_NONE, PROFILE_NONE, PROFILE_NONE, PROFILE_NONE },
    .transport = { TRANSPORT_BT, TRANSPORT_BT, TRANSPORT_BT, TRANSPORT_BT,
                   TRANSPORT_BT, TRANSPORT_BT, TRANSPORT_BT, TRANSPORT_BT },
    .types = 1u << PROFILE_GENERIC_BLE | 1u << PROFILE_DS5EDGE_BLE | 1u << PROFILE_XBOX_BLE |
             1u << PROFILE_SWITCH2 | 1u << PROFILE_NONE,      /* controller_lite.c */
    /* the stock 8BitDo receiver is the rear switch's 2.4G, not a slot's */
    .transports = 1u << TRANSPORT_BT,
    .static_random = 1,
};
