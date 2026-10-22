# Board and GPIO

## Pad (Cyclone 2)

The pad's SoC is the Jieli BR23 ([../jieli/soc.md](../jieli/soc.md)).

Digital buttons and D-pad inputs are active low with internal pull-ups.

### Buttons and D-pad

| Function | Pin | Notes |
|---|---|---|
| button A (south) | PA0 | |
| button B (east) | PA6 | |
| button X (west) | PA7 | |
| button Y (north) | PA13 | |
| D-pad up | PA2 | UART setup can disturb it; see below |
| D-pad down | PA4 | |
| D-pad left | PB8 | |
| D-pad right | PA10 | |
| L1 | PC7 | |
| R1 | PC0 | |
| L3 (stick click) | PC1 | |
| R3 (stick click) | PC2 | |
| L4 | PA5 | |
| R4 | PC4 | |
| M | PB7 | |
| Start | PA14 | |
| Select | PA15 | |
| Screenshot | PD5 | |
| Home | PB11 | filtered falling-edge wake input with pull-up |

### Analog and motion

| Function | Pin | Notes |
|---|---|---|
| L2 trigger | PB3 | SAR ADC channel `AD_CH_PB3`, analog |
| R2 trigger | PB10 | SAR ADC channel `AD_CH_PB10`, analog |
| L2 stop switch | PR1 | RTC-domain input, active low |
| R2 stop switch | PR0 | RTC-domain input, active low |
| IMU / EEPROM I2C SCL | PB4 | I2C, route group C |
| IMU / EEPROM I2C SDA | PB6 | I2C, route group C |
| Sensor power enable | PB5 | driven high |
| Stick UART RX | PA3 | UART2 RX; no TX pin used |

### Rumble, LEDs, power and reset

| Function | Pin | Notes |
|---|---|---|
| left motor PWM | PA9 | MCPWM channel 4, timer 4 |
| right motor PWM | PB0 | MCPWM channel 1, timer 1 |
| LED power enable | PB9 | active low |
| LED data | PC5 | SPI1 group B data out |
| USB sense | PC3 | see [power.md](power.md) for what it reads |
| Configured reset input | PB1 | active low; reset time 8 (configuration value) |

### Pin groups and conflicts

- **PC3** is named both `USB sense` and `LED SPI DI` in the SDK's pin groups.
  The LED driver drives only PC5. PC3 is not used as LED data.
- **PC4** is named both `R4` and `LED SPI clock`. Same situation.
- **PA2** (D-pad up) is changed by SDK UART setup. The app restores it as a
  pulled-up input after opening UART2.
- **PC7** (L1) is also the optional SDK microphone bias pin. See
  [audio.md](audio.md).
- Home (PB11) is a wake input. PB1 is the reset input. See [power.md](power.md).

### Not established

- Whether a PSRAM chip is populated.

### Face button labels

The face buttons use the Xbox layout: A south, B east, X west, Y north. The
pins were identified by pressing each printed label.

## Dongle (GameSir)

| Function | Pin | Notes |
|---|---|---|
| button | PA0 | active low, pull-up |
| LED | PD3 or PA13 | depends on board revision; not driven by the current firmware |

Flash is 512 KiB ([flash-map.md](flash-map.md)). The dongle is also a BR23.
