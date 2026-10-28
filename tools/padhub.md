# Linux pad hub

Build the dongle firmware and Linux bridge:

```sh
make -j8 gamesir-cyclone-2-dongle padhub
make gamesir-cyclone-2-dongle-flash DEVICE=5-2.4
build/host/padhub
```

Use your dongle's USB port for `DEVICE` (`build/host/jlnewfw scan`).
The bridge needs writable `/dev/uhid`, the dongle's `/dev/hidraw*`, and its
SCSI tool node `/dev/sg*` (or those in `$DEV_ROOT` when it is set).
Load the Linux `uhid` module on the host if its node is missing. Start with the
dongle showing its tool device. If it already presents a USB controller, end
that session or use `padtool tool` to bring the tool interface back first.
Disable `padtool --dongle tool-only` before using padhub.

The dongle has six bond slots, numbered 0–5. Normal mode enables slot 0;
hub mode enables all six. These are one shared flash bond table. Changing
modes preserves the bonds; pairing replaces the selected slot. Pairing a pad
already in another slot moves its bond. Hub mode itself is RAM-only: padhub
enables it, and clean shutdown or a dongle reboot disables it. Explicit
`padtool --dongle ephemeral` suppresses dongle bond writes until reboot.
A pad in ephemeral mode likewise does not save its half of a new pairing.

For each pad, configure a pad slot with `transport dongle`, then pair that
pad slot with a dongle slot, one pad at a time. For example, with two pads
connected by USB and padhub running:

```sh
python padtool/padtool.py -d 5-2.1 pair slot-x
python padtool/padtool.py --dongle pair 0
python padtool/padtool.py -d 5-2.2 pair slot-x
python padtool/padtool.py --dongle pair 1
```

`slot-x` is a pad setting; `0` and `1` are dongle bond slots. They are separate
number spaces. Once paired, `connect slot-x` reconnects that pad. Dongle
pairing works through the hub's HID feature report while its SCSI interface
is absent. The dongle button pairs slot 0.

Each linked pad becomes a separate UHID device using its configured profile.
Supported HID USB profiles are generic/genericble, mouse, Switch, DualShock 4,
and DualSense/dualsenseble. Xbox/Xbox BLE use XInput on USB and Switch 2 needs a
USB bulk handshake; padhub reports these as unsupported. Choose a supported
profile for those pad slots. The firmware's profiles handle reports, feature
reads, rumble, lights and motion; one worker process per slot isolates their
static state. Short disconnects retain the device with controls at rest;
after three seconds it is removed. A returning pad recreates it.

Two pads have been exercised simultaneously, including dongle slot 5. Six
slots are allocated throughout the app, BTstack and radio pools; operation
with six simultaneous pads is not established. Intervals grow beyond two
links to reserve radio time. Encryption remains enabled.

Run `make padhub-test` for host protocol/lifecycle checks without hardware;
`make test` includes these checks. For hardware bring-up without UHID, use
`padtool --dongle tool-only on` and `hub on`, connect pads, and inspect
`padtool --dongle log`. Restore `tool-only off` to expose the USB hub.
