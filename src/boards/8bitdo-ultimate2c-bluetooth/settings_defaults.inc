/* Shared by the board and the host schema exporter. */
/* The four player lights are on or off (plat_leds): any brightness lights
 * them. */
const struct board_led_brightness board_led_brightness = {
    .steady = 240, .animation = 400, .connect = 240,
    .idle = 240, .battery = 240,
};

/* As the U2CW's motors until these are measured. */
const struct board_rumble board_rumble = {
    .min = { { 32, 32, 32, 45, 55 }, { 32, 32, 32, 45, 55 } },
    .max = { { 65, 85, 100, 100, 100 }, { 65, 85, 100, 100, 100 } },
    .kick_ms = { 0, 0, 0, 0, 0 },
};

/* Classic Bluetooth only (the radio core's HID device): Switch Pro,
 * generic HID, DualShock 4, DualSense.  USB as a game controller is not
 * here yet. */
const struct board_slots board_slots = {
    .type = { PROFILE_SWITCH, PROFILE_GENERIC, PROFILE_DS4, PROFILE_DS5,   /* A B X Y */
              PROFILE_NONE, PROFILE_NONE, PROFILE_NONE, PROFILE_NONE },
    .transport = { TRANSPORT_BT, TRANSPORT_BT, TRANSPORT_BT, TRANSPORT_BT,
                   TRANSPORT_BT, TRANSPORT_BT, TRANSPORT_BT, TRANSPORT_BT },
    .types = 1u << PROFILE_SWITCH | 1u << PROFILE_GENERIC | 1u << PROFILE_DS4 |
             1u << PROFILE_DS5 | 1u << PROFILE_MOUSE | 1u << PROFILE_NONE,
    .transports = 1u << TRANSPORT_BT,
};
