# Cyclone 2 hardware reference

This folder records the hardware and firmware-image facts known for the tested
GameSir Cyclone 2, as of 2026-10-09. It replaces the software plans and session
logs. “Measured” means observed on the pad; “recovered” means read from stock
firmware or SDK code; configuration alone is not proof of a board connection.

- [Board and GPIO](board.md): SoC, buttons, pin assignments and pin conflicts.
- [Peripherals](peripherals.md): stick UART, IMU/I2C, LEDs, rumble and EEPROM.
- [Headset audio](audio.md): recovered DAC/ADC settings, jack and amplifier pins, and stock pad/dongle audio paths.
- [Power and reset](power-reset.md): charging evidence, wake sources, reset causes and USB loader entry.
- [Memory map](memory-map.md): CPU address space, retained SRAM and boot workspace.
- [Flash map](flash-map.md): physical NOR layout and reserved regions.
- [Image formats](image-formats.md): JLFS, scrambling and configuration records.
- [Radio](radio.md): measured radio behavior, raw frame fields and wake limitations.
- [Stock pad and dongle](stock-pad-dongle.md): Bluetooth connection sequence,
  keepalive format, captured HID reports and the measured input rate.

The app source and generated `build/sdk.ld` / `build/app.map` remain the authority
for the current build's allocation. Historical addresses within `.data`, `.bss`
or heap are not hardware constants. External evidence paths identify the
original artifacts; those artifacts are not bundled with this repository.
