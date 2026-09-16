# Input profiles

## Code flow

The intended reading order is:

1. `src/main.c` supplies the SDK entry point and starts the registered app.
2. `src/controller.c` owns input timing, Home/chord policy, LEDs, sleep, and
   profile selection.
3. `src/profile.c` presents one transport-independent profile interface.
4. `src/profile_switch.c` and `src/profile_generic.c` contain their respective
   Bluetooth setup, event handling, report format, and send path.
5. `src/input.c`, `src/usb_control.c`, and `src/control.c` contain physical
   input, the CDC console, and board output support.

Adding a controller protocol should normally mean adding one profile module
and one operations-table entry in `profile.c`, without adding protocol checks
to the controller policy loop.

One firmware image contains both the Nintendo Switch-compatible Bluetooth
Classic controller and the generic BLE gamepad. Four independent connection
slots are selected with the physical face buttons:

| Chord | Slot | Default controller type |
| --- | ---: | --- |
| Home+A | 1 | Nintendo Switch |
| Home+B | 2 | Nintendo Switch |
| Home+X | 3 | Generic BLE |
| Home+Y | 4 | Generic BLE |

Release a chord to reconnect the selected slot. Hold it for five seconds to
discard that slot's bond and enter pairing mode. Each slot has a derived local
Bluetooth address and separate bond storage, so the hosts see four independent
controllers. The slot-to-controller assignments live in syscfg item 48. Their
defaults are currently initialized in `settings_load()`; a user-facing
configuration transport has not been added yet.

Selecting the first profile after boot starts its Bluetooth stack directly.
Switching profiles while a stack is already running uses a 16-byte internal
SRAM reservation as a one-shot soft-reset handoff. It is outside `.bss` and
the heap, is not present in the flash image, and is invalidated before the
replacement stack starts. Profile switching therefore does not write syscfg.

The firmware deliberately does not remember the active slot. After boot or
wake it waits for an explicit Home+face chord. Home and the four face-button
GPIOs retain their input configuration during soft-poweroff, and a chord is
sampled at the beginning of board initialization. This lets a short wake chord
select its slot without waiting for the rest of SDK initialization. The Home
LED indicates radio state:

- slow dim-red blink: reconnecting;
- fast dim-red blink: pairing;
- steady dim red: connected.

If a connection attempt times out, or an established host disconnects, the
controller goes to sleep unless USB is connected; USB keeps the CDC diagnostic
and recovery console alive. Hold Home alone for five seconds to disconnect and
sleep explicitly. Home wakes it again, but does not implicitly select a slot.

## Home-key arbitration

Home is delayed by 150 ms so a face button pressed immediately afterward can
form a profile chord. If Home is released during that grace period, the
firmware emits a short Home down/up pair. Otherwise Home remains held in the
HID report normally; this preserves the Nintendo Switch's one-second Home menu.
Once a profile chord is recognized, both Home and the face button are hidden
from the host.

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

## Input and report rates

Physical inputs are sampled every 5 ms. HID report submission is scheduled
independently and defaults to 10 ms. The saved report period can be changed to
5, 8, or 10 ms through the USB console. All chord, debounce, timeout, and LED
policy uses millisecond timestamps and therefore does not change with the HID
report rate.

The generic profile requests a 7.5-ms BLE connection interval with zero slave
latency, followed by compatible fallback requests. The host chooses the actual
parameters; SDK debug output reports the negotiated interval. An 8-ms report
setting is the practical generic-BLE target. The Switch profile uses Bluetooth
Classic rather than BLE and can exercise the 5-ms/200-Hz setting.

Faster submission does not prove that the host receives reports at that rate.
Use `report status` to inspect attempted, accepted, and busy/full submissions;
end-to-end latency and host arrival cadence still need external measurement.

## Switch profile

The Switch profile uses Nintendo's Bluetooth Classic HID protocol, not merely
Nintendo's VID/PID. It currently implements:

- the stock Cyclone 2 Switch report descriptor;
- simple (`0x3f`) and full (`0x30`) input reports;
- `0x21` acknowledgements for device-info, report-mode, SPI-read, player-light,
  home-light, IMU, and vibration subcommands;
- virtual factory stick calibration at SPI addresses `0x603d` (left) and
  `0x6046` (right), including partial-range reads;
- the Nintendo Pro Controller PnP identity (`057e:2009`).

The original Pro Controller packet has no rear-paddle fields, so L4 and R4 are
not visible in this mode. Analog triggers are reduced to the Switch's digital
ZL/ZR buttons. IMU replies are accepted but sensor samples are presently zero.
This is an experimental interoperability milestone; it still needs testing
against both SDL's Switch HIDAPI driver and a physical Nintendo Switch.
