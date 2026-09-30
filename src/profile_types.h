#ifndef CYCLONE2_PROFILE_TYPES_H
#define CYCLONE2_PROFILE_TYPES_H

/* What a profile slot can pretend to be (config.profile[].type).  Stored in
 * flash by value: append, never renumber. */
enum cyclone2_profile_type {
    CYCLONE2_PROFILE_SWITCH,
    CYCLONE2_PROFILE_GENERIC,
    CYCLONE2_PROFILE_SWITCH2,
    CYCLONE2_PROFILE_XBOX,
    CYCLONE2_PROFILE_DUALSENSE,
    CYCLONE2_PROFILE_XBOX_BLE,
    CYCLONE2_PROFILE_STICKMOUSE,
    CYCLONE2_PROFILE_GYROMOUSE,
    CYCLONE2_PROFILE_TYPE_COUNT,
};

#endif
