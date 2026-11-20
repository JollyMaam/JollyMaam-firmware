# JollyMaam: open source gamepad firmware

|Supported Devices|
|--|
|Gamesir Cyclone 2|
|Gamesir Nova 2 Lite|
|8BitDo Ultimate 2C Wireless|
|8BitDo Ultimate 2C Bluetooth|BLE only|

|Question|Answer|
|--|--|
|Did a human write this README?|Yes.|
|Is it vibe-coded?|Yes.|
|Why did you make this?|I was annoyed by the stock Cyclone 2 firmware and got carried away.|
|Who are you?|A linux geek and professional embedded linux engineer for 10+ years.|

Main features that have been added:
  - Nintendo Switch 2 Pro controller support. This includes waking the console, the back paddle buttons, the C button, and motion controls.
  - DualSense controller support.
  - Up to 8 different connection slots. Each slot can be a different controller type. Connect with a specific slot by using Home+ABXY or Home+UpDownLeftRight when not currently connected.
  - Up to 8 different remap slots. Each remap slot can map any input to any output. Additionally there can be rules like "if L4 is held for 200ms, enable turbo modifier for buttons A and B". A remap slot is independent from a connection slot. Each connection slot has a default remap slot assigned but can be switched at runtime. All remap slots inherit from a base remap config.
  - Show the "player number" LEDs so you know which pad corresponds to which player.
  - Configuration and firmware upgrades through USB or bluetooth
  - Support multiple controllers connecting through a single dongle on linux (see tools/padhub.c)

## Building / Installing

```
Build firmware:
$ nix develop .# -c make firmware

Build android APK:
$ nix develop .# -c make android

Install from cmdline:
$ ./build/host/jollymaam install
```

[Latest Release](./releases/latest)

## How to enter recovery mode

|Device|Instructions|USB Product|
|--|--|--|
|Gamesir Cyclone 2|Hold Logo/Home + Menu/Start button while plugging in USB.|BR23UBOOT1.00 mass storage|
|Gamesir Nova 2 Lite|Hold Logo/Home + Menu/Start button while plugging in USB.|BR23UBOOT1.00 mass storage|
|8BitDo Ultimate 2C Wireless|Unplug USB. Hold Logo/Home button for 8 seconds to turn off pad. Hold L1+R1 and tap Logo/Home button. Red pairing LED blinks.|8BitDo Boot HID|
|8BitDo Ultimate 2C Bluetooth|Unplug USB. Hold Logo/Home button for 8 seconds to turn off pad. Hold L1+R1 and tap Logo/Home button. Red pairing LED blinks.|8BitDo Boot HID|

## How to use

|Legend|Description|
|--|--|
|Slot Key|A, B, X, Y, Dpad-Up, Dpad-Down, Dpad-Left, Dpad-Right|
|Macro Key|On Gamesir pads, the M button at the bottom, 8BitDo pads the star turbo button|
|Button|One of A, B, X, Y, L1/R1, L3/R3 (sticks), L4/R4 (paddles)

|When|Key|Action|
|--|--|--|
|Any|Home+Turbo for 3 seconds|Enter configuration mode|
|Configuration Mode|Home|Exit configuration mode|
|Disconnected|Home|Connect to default slot|
|Disconnected|Home+Slot|Connect to specific slot|
|Connected|Home+Select/Capture|Disconnect|
|Connected|Home+Turbo|Enter Quick Settings|
|Quick Settings|Home|Exit Quit Settings|
|Quick Settings|Home+Slot|Use remap slot|
|Quick Settings|Home+L1/R1|Cycle through rumble strength|
|Quick Settings|Macro+Button|Record macro|
|Quick Settings|Double tap Macro and hold, then Button|Record repeating macro|
|Quick Settings|Button|Cycle through turbo modes|

Note: A Quick Setting only lasts until the next disconnect. It does not persist in settings.

## To Configure

[Web-based Configuration](https://jollymaam.github.io). Uses WebHID and WebBluetooth, supported on Chrome browsers.

Or use the android app.
