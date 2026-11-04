/* The board as the firmware and the tools see it: its controls, then its
 * defaults.  board_pad.c wires the pins to these inputs; the host schema
 * exporter builds this file too and checks the layout. */

/* Nintendo order: B south, A east, Y west, X north.  The others as the
 * U2CW's (L4 and R4 on top); M is the Star button; L2 and R2 are digital.
 * Only the face buttons' labels are established.  Not placed yet. */
const struct board_control board_layout[] = {
    /* input         kind                 label      x  y */
    { IN_SOUTH,      LAYOUT_FACE,         "B",       0, 0 },
    { IN_EAST,       LAYOUT_FACE,         "A",       0, 0 },
    { IN_WEST,       LAYOUT_FACE,         "Y",       0, 0 },
    { IN_NORTH,      LAYOUT_FACE,         "X",       0, 0 },
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
    { IN_SELECT,     LAYOUT_MENU,         "",        0, 0 },
    { IN_START,      LAYOUT_MENU,         "",        0, 0 },
    { IN_HOME,       LAYOUT_MENU,         "",        0, 0 },
    { IN_SCREENSHOT, LAYOUT_MENU,         "",        0, 0 },
    { IN_M,          LAYOUT_MENU,         "",        0, 0 },
};
const u8 board_layout_count = LAYOUT_COUNT(board_layout);

/* The base remap the pad starts with: every key sends itself. */
const struct set_remap board_remap_base = { .name = "" };

/* The lights are on or off (plat_leds): the four player lights white, so
 * the host's player number shows alone; the cable and pairing signs on the
 * lights below Home and beside it (red), as the U2CW's. */
const struct board_led_brightness board_led_brightness = {
    .steady = 240, .animation = 400, .connect = 240,
    .idle = 240, .battery = 240, .players_mono = 1, .home_signs = 1,
};

/* As the U2CW's motors until these are measured. */
const struct board_rumble board_rumble = {
    .min = { { 32, 32, 32, 45, 55 }, { 32, 32, 32, 45, 55 } },
    .max = { { 65, 85, 100, 100, 100 }, { 65, 85, 100, 100, 100 } },
    .kick_ms = { 0, 0, 0, 0, 0 },
};

/* Classic Bluetooth (the radio core's HID device) and USB (HID and Xbox 360
 * personalities; no Switch 2 one): Switch Pro, generic HID, DualShock 4,
 * DualSense, Xbox; each slot USB while a host is there, else Bluetooth. */
const struct board_slots board_slots = {
    .type = { PROFILE_SWITCH, PROFILE_GENERIC, PROFILE_DS4, PROFILE_DS5,   /* A B X Y */
              PROFILE_NONE, PROFILE_NONE, PROFILE_NONE, PROFILE_NONE },
    .transport = { TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_BT_USB,
                   TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_BT_USB },
    .types = 1u << PROFILE_SWITCH | 1u << PROFILE_GENERIC | 1u << PROFILE_DS4 |
             1u << PROFILE_DS5 | 1u << PROFILE_MOUSE | 1u << PROFILE_XBOX | 1u << PROFILE_SWITCH2 |
             1u << PROFILE_NONE,
    .transports = 1u << TRANSPORT_BT | 1u << TRANSPORT_USB | 1u << TRANSPORT_BT_USB,
};
