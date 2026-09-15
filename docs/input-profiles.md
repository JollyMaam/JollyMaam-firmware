# Input profiles

The firmware currently has two build-time Bluetooth HID profiles:

- `make BT_PROFILE=ble` builds the generic BLE gamepad (the default).
- `make O=out-switch BT_PROFILE=switch` builds the experimental Nintendo
  Switch-compatible Bluetooth Classic HID gamepad.

Use separate output directories when alternating profiles. Object files are
compiled with different transport configuration macros and therefore cannot
be shared safely between the two builds.

## Generic BLE profile and SDL

The generic profile deliberately keeps the GameSir identity (`3537:0575`).
Pretending to be an unrelated controller just to inherit its SDL mapping can
select a device-specific driver with incompatible reports.

SDL's built-in Nintendo Switch Pro mappings do not expose rear paddles. A
device-local SDL mapping can expose every Cyclone 2 control except M:

```
05000000373500007505000000010000,GameSir Cyclone 2 BLE,a:b0,b:b1,x:b3,y:b4,back:b10,start:b11,guide:b12,leftshoulder:b6,rightshoulder:b7,leftstick:b13,rightstick:b14,misc1:b2,paddle1:b8,paddle2:b9,leftx:a0,lefty:a1,lefttrigger:a2,rightx:a3,righty:a4,righttrigger:a5,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,
```

The GUID above is the expected Linux Bluetooth GUID from bus `0x0005`, VID
`0x3537`, PID `0x0575`, and version `0x0100`. Confirm it on the host with
`SDL_JoystickGetGUIDString()` (SDL2) or `SDL_GetJoystickGUIDString()` (SDL3)
before submitting it to SDL's community database.

The relevant extra SDL controls are:

- Screenshot -> `misc1`
- L4 -> `paddle1`
- R4 -> `paddle2`
- M -> intentionally omitted

## Switch profile

The Switch profile uses Nintendo's Bluetooth Classic HID protocol, not merely
Nintendo's VID/PID. It currently implements:

- the stock Cyclone 2 Switch report descriptor;
- simple (`0x3f`) and full (`0x30`) input reports;
- `0x21` acknowledgements for device-info, report-mode, SPI-read, player-light,
  home-light, IMU, and vibration subcommands;
- virtual factory stick calibration at SPI address `0x603d`;
- the Nintendo Pro Controller PnP identity (`057e:2009`).

The original Pro Controller packet has no rear-paddle fields, so L4 and R4 are
not visible in this mode. Analog triggers are reduced to the Switch's digital
ZL/ZR buttons. IMU replies are accepted but sensor samples are presently zero.
This is an experimental interoperability milestone; it still needs testing
against both SDL's Switch HIDAPI driver and a physical Nintendo Switch.
