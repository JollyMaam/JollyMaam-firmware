/* Shared by the board and the host schema exporter. */
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
