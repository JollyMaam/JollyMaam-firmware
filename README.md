# Cyclone 2 firmware

An SDK-based replacement firmware project for the Gamesir Cyclone 2 (Jieli
BR23). The Jieli SDK is kept as an external, pinned input; only our source and
the small audited SDK patch are tracked here.

Current baseline behavior:

- Home + Start at cold boot enters MaskROM `UBOOT1.00`, but only with USB/VBUS
  present.
- The same early check runs after an SDK soft-off wake/reset, so holding Start
  while waking with Home enters `UBOOT1.00` when USB is connected.
- A debounced press of M (PB7) calls the SDK's `power_set_soft_poweroff()`.
- Home (PB11) is the SDK soft-off wake pin.
- Solid red LEDs show that the SDK control task is alive. M changes them to
  magenta briefly, turns them off, and then enters soft-off.
- The stock SDK HID/BT sample remains the main application for now.

## Build

Requirements are currently expected at their workspace paths:

- SDK: `/work/github.com-Jieli-Tech/fw-AC63_BT_SDK`
- PI32 toolchain: `/work/jieli-linux-toolchains-20250324.1/pi32v2/bin`
- firmware tools: `/work/github.com-kagaimiq/jl-misctools`
- stock 3.52 template: `/work/firmware/fw_3.52.bin_unpack`

Run:

```sh
./build.sh
```

The script builds from a clean archive of the pinned SDK commit and never
modifies the SDK checkout. Build products go in `build/`; verified flashable
output goes in `dist/`.

This is an early hardware-validation firmware. Keep the pinhole reset and JL
forced-USB recovery method available when flashing it.

## First hardware test

1. Flash `dist/cyclone2-sdk-fw.bin` using the already-tested update path.
2. A normal start should reach solid red once the SDK control task runs.
3. Press M: the LEDs should briefly turn magenta and then go dark in SDK
   soft-off.
4. Press Home: the chip should wake/reboot and return to solid red.
5. With USB connected, boot or wake while holding Home + Start: the firmware
   should perform the proven detach/reset sequence and enumerate as
   `BR23UBOOT1.00`.

If USB is absent, Home + Start intentionally does not select UBOOT. This avoids
leaving MaskROM waiting invisibly for a cable.
