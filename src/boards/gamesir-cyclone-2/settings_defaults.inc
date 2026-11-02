/* Shared by the board and the host schema exporter. */
/* From testing (54a377a). */
const struct board_led_brightness board_led_brightness = {
    .steady = 10, .animation = 400, .connect = 50,
    .idle = 50, .battery = 50,
};

const struct board_rumble board_rumble = {
    .min = { { 15, 20, 25, 25, 25 }, { 15, 15, 15, 15, 15 } },
    .max = { { 35, 55, 75, 80, 90 }, { 50, 60, 70, 80, 90 } },
    .kick_ms = { 0, 0, 0, 25, 30 },
};

/* Slots A-Y: plain HID over LE, DualSense, Xbox through the dongle, Switch 2. */
const struct board_slots board_slots = {
    .type = { PROFILE_GENERIC_BLE, PROFILE_DS5, PROFILE_XBOX, PROFILE_SWITCH2,
              PROFILE_NONE, PROFILE_NONE, PROFILE_NONE, PROFILE_NONE },
    .transport = { TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_DONGLE, TRANSPORT_BT_USB,
                   TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_BT_USB, TRANSPORT_BT_USB },
};
