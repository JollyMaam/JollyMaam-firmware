# Our sources that are not a platform's, as units: an image adds the ones it
# uses, then its board's and its platform's (make/<platform>.mk).

# Every image, the updaters too.
CORE_SOURCES :=
CORE_SOURCES += src/core/utils.c
CORE_SOURCES += src/core/battery.c

# A pad: the control loop, its inputs and outputs, settings and USB.
PAD_SOURCES :=
PAD_SOURCES += src/core/controller.c
PAD_SOURCES += src/core/input.c
PAD_SOURCES += src/core/input_calibration.c
PAD_SOURCES += src/core/motion.c
PAD_SOURCES += src/core/leds.c
PAD_SOURCES += src/core/remap.c
PAD_SOURCES += src/core/quick.c
PAD_SOURCES += src/core/macro.c
PAD_SOURCES += src/core/settings.c
PAD_SOURCES += src/core/settings_ux.c
PAD_SOURCES += src/core/usb.c

# A dongle: USB and the motion it forwards.
DONGLE_SOURCES :=
DONGLE_SOURCES += src/core/usb.c
DONGLE_SOURCES += src/core/motion.c

# The profiles a pad and a dongle both speak to a host.
PROFILE_SOURCES :=
PROFILE_SOURCES += src/profiles/profile_hid.c
PROFILE_SOURCES += src/profiles/profile_link.c
PROFILE_SOURCES += src/profiles/profile_playstation.c
PROFILE_SOURCES += src/profiles/profile_switch.c
PROFILE_SOURCES += src/profiles/profile_xbox.c
# A pad's own: config mode.
PROFILE_PAD_SOURCES :=
PROFILE_PAD_SOURCES += src/profiles/profile_config.c
