# Input profiles

## Code flow

The intended reading order is:

1. `src/main.c` supplies the SDK entry point and starts the registered app.
2. `src/controller.c` owns input timing, Home/chord policy, LEDs, sleep, and
   profile selection.
3. `src/profile.c` presents one transport-independent profile interface.
4. `src/profile_switch.c`, `src/profile_switch2.c`, `src/profile_xbox.c`,
   `src/profile_dualsense.c` and `src/profile_generic.c` contain their
   respective report formats and send paths.  The three Bluetooth Classic HID
   ones (Switch, Xbox, DualSense Edge) share one session module,
   `src/profile_classic.c`: pairing, reconnecting by page, bond cleanup and
   opening the HID channels.  Each describes itself with a
   `struct cyclone2_classic_profile` (name, class/SSP/report descriptor,
   Device ID, output and GET_REPORT handlers).
5. `src/input.c`, `src/usb_control.c`, and `src/control.c` contain physical
   input, the CDC console, and board output support.

Adding a controller protocol should normally mean adding one profile module
and one operations-table entry in `profile.c`, without adding protocol checks
to the controller policy loop.

One firmware image contains the Nintendo Switch-compatible Bluetooth Classic
controller, the raw Nintendo Switch 2 BLE controller, and the generic BLE
gamepad. Four independent connection slots are selected with the physical
face buttons:

| Chord | Slot | Default controller type |
| --- | ---: | --- |
| Home+A | 1 | Nintendo Switch |
| Home+B | 2 | Nintendo Switch |
| Home+X | 3 | Generic BLE |
| Home+Y | 4 | Nintendo Switch 2 Pro |

Release a chord to reconnect the selected slot. Hold it for five seconds to
discard that slot's bond and enter pairing mode. Each slot has a derived local
Bluetooth address and separate bond storage, so the hosts see four independent
controllers. The slot-to-controller assignments live in syscfg item 48. Their
defaults are initialized in `settings_load()`; `profile N [type]` on the USB
console shows or changes one (types: `switch`, `switch2`, `xbox`,
`dualsense`, `generic`).  A change applies the next time the slot starts, and
the slot needs pairing again with its new host.

## Xbox and DualSense Edge (untested on hardware)

Both are Bluetooth Classic HID, built and reviewed but not yet run on the pad
(2026-09-26).

- `xbox`: Xbox Wireless Controller, Device ID `045e:02fd` version `0x0100`,
  the stock firmware's "bt xinput mode" identity and report descriptor
  (extracted from its image).  Windows binds `xinputhid`, so XInput games see
  it without a dongle.  XInput has no paddles or gyro; the extra buttons
  (capture, M, L4, R4) ride on unused HID buttons 3, 6, 9 and 10 for hosts
  that read raw HID.  Rumble: output report `0x03`.
- `dualsense`: DualSense Edge, `054c:0df2`.  SDL (HIDAPI PS5 driver), Steam
  Input and Linux hid-playstation report the back paddles (L4/R4), gyro and
  accelerometer (the QMI8658A's scale is declared in feature report `0x05`),
  rumble (output `0x31`).  M is the touchpad click and capture is mute.  It
  starts in extended reports (`DS_START_EXTENDED`) so SDL opens it in full
  mode; DirectInput-only readers then see nothing.  The descriptor was
  derived from the stock DualShock 4 one; lights and player LEDs from the host
  are ignored so far.

First things to check on hardware: the Device ID the host sees
(`bluetoothctl info`, `/sys/bus/hid/devices`), SDL's `testcontroller`
(paddles, gyro direction against the Switch profile, rumble), and for the
DualSense that hid-playstation accepts the feature reports' CRCs (dmesg).

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

Hold Home+M for three seconds to calibrate the sticks and triggers
(`docs/calibration.md`); the Home LED turns blue while it runs.

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
  `0x6046` (right), including partial-range reads; stick output is scaled to
  exactly the 0x800 ± 0x600 it declares (`docs/calibration.md`);
- the Nintendo Pro Controller PnP identity (`057e:2009`).

The original Pro Controller packet has no rear-paddle fields, so L4 and R4 are
not visible in this mode. Analog triggers are reduced to the Switch's digital
ZL/ZR buttons. IMU replies are accepted but sensor samples are presently zero.
This is an experimental interoperability milestone; it still needs testing
against both SDL's Switch HIDAPI driver and a physical Nintendo Switch.

The protocol details, including the HIDP framing, every report layout and the
subcommand list, are documented at the top of `src/profile_switch.c`.

The two binary blobs the protocol needs are the source of truth and live in
`src/`. They are the only place the bytes are written down; `tools/bin2header.c`
just spells them out as the C arrays the firmware compiles
(`build/nintendo_switch_pro_hid.h` and `build/nintendo_switch_pro_spi.h`), so
they can be decoded, hex-dumped and diffed with ordinary tools:

| File | Contents |
| --- | --- |
| `src/nintendo_switch_pro_hid.bin` | the 170-byte HID report descriptor |
| `src/nintendo_switch_pro_spi.bin` | the virtual SPI flash window starting at `0x6000`, indexed exactly like the controller's flash |

`read_virtual_spi()` serves the controller's SPI flash to the console and to
host drivers:

| Address | Contents |
| --- | --- |
| `0x6000` | device info / serial; mostly erased, with the Pro Controller type at `0x6012` |
| `0x6020` | 6-axis factory calibration (24 bytes) |
| `0x603d` | stick factory calibration, left then right (18 bytes) |
| `0x6050` | body/button/grip colours (12 bytes) |
| `0x6080` | 6-axis parameters (6 bytes) |
| `0x6086`, `0x6098` | left and right stick parameters (18 bytes each) |

Unlisted addresses read as erased flash (`0xff`), which is what keeps the
"user calibration" magic values at `0x8010`/`0x801b` unset. The two replies
that carry the local Bluetooth address use different byte orders: the pairing
reply sends it little endian, the device-information reply big endian. Both
orders go through named helpers because `bt_get_mac_addr()` stores the address
little endian (the reverse of the printed `aa:bb:cc:dd:ee:ff` form).

## Switch 2 profile

The Switch 2 profile is a raw BLE GATT implementation based on the
`ESP32-BLE5-NSController-Emulator` reference. It uses the Switch 2 Pro
controller's custom GATT service and 63-byte input report rather than BLE HID.
The JL stack exposes the same service/characteristic topology, including the
input report notification, command response notification, and vibration
write characteristic. The profile has a virtual calibration/NFC-style flash
image and answers the reference controller's initialization, feature, light,
IMU, and SPI-read commands.

The Cyclone 2 controls are translated as follows:

- A/B/X/Y use the same Nintendo face-button ordering as the Classic Switch
  profile.
- Screenshot becomes Capture.
- L4 and R4 become the Switch 2 GL and GR paddle buttons.
- M is sent as the Switch 2 C button.
- Stick axes are centered and scaled to the Switch 2 12-bit packed format.

The device advertises Nintendo's Switch 2 Pro identity (`057e:2069`). The
profile is selected by Home+Y and is not flashed or hardware-tested by this
change. The reference controller's proprietary LTK message exchange is
implemented, but JL bond persistence/LTK injection is not yet integrated, so
pairing and reconnect behavior with a physical console still needs validation.
