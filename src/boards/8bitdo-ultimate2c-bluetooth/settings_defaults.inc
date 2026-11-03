/* Shared by the board and the host schema exporter. */
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
             1u << PROFILE_DS5 | 1u << PROFILE_MOUSE | 1u << PROFILE_XBOX | 1u << PROFILE_NONE,
    .transports = 1u << TRANSPORT_BT | 1u << TRANSPORT_USB | 1u << TRANSPORT_BT_USB,
};
