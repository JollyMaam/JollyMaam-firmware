/* Shared by the board and the host schema exporter. */
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
