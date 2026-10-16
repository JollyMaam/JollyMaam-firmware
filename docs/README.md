# Cyclone 2 hardware reference

Hardware facts for the GameSir Cyclone 2 pad and the GameSir dongle: the SoC,
memory, flash, pins, peripherals, the Bluetooth radio, the stock pad and dongle
protocol, and how software runs on the chip.

| Page | Covers |
|---|---|
| [soc.md](soc.md) | chip, CPU core, clocks, power domains, boot chain, peripheral register blocks |
| [sdk-toolchain.md](sdk-toolchain.md) | SDK, compiler, linker, libraries, image build, toolchain pitfalls |
| [memory-map.md](memory-map.md) | CPU address space, internal RAM, retained SRAM |
| [flash-map.md](flash-map.md) | SPI NOR layout and reserved regions |
| [board.md](board.md) | GPIO assignments for the pad and the dongle |
| [peripherals.md](peripherals.md) | stick UART, IMU and I2C, EEPROM, LEDs, rumble motors |
| [audio.md](audio.md) | DAC, microphone ADC, jack detect, amplifier enable, USB audio descriptors |
| [power.md](power.md) | charger registers, sleep and wake inputs, reset sources, USB pin control and loader entry |
| [radio.md](radio.md) | Bluetooth controller capabilities, TX power, baseband frame registers |
| [stock-pad-dongle.md](stock-pad-dongle.md) | how a stock pad connects to a host, and the stock dongle's HID and keepalive traffic |

Reverse-engineering notes on the stock images and the chip's radio:

| Page | Covers |
|---|---|
| [re-stock-firmware.md](re-stock-firmware.md) | recovered stock behaviour: audio routines, jack detect, power, USB descriptors, method |
| [re-application-image.md](re-application-image.md) | the application image: container format, JLFS, scrambling, configuration records, stock placement, the image tool |
| [re-radio-wake.md](re-radio-wake.md) | the Switch 1 wake transmission's construction; raw transmit constraints |

## Conventions

- Addresses are CPU addresses unless the text says "flash offset". Ranges are
  half-open: `0x0..0x2c000` ends before `0x2c000`.
- `PAn`, `PBn`, `PCn`, `PDn` are GPIO ports. `PR0`, `PR1` are RTC-domain pins.
- Build-specific addresses (`.data`, `.bss`, heap, overlays) change with the
  build. The linker map from the build lists the current values.
- Where a value comes from stock firmware or a binary rather than the SDK, the
  text says so.
- "Not established" marks a point that no source confirms.
