# Board and GPIO

The controller uses a Jieli BR23-family SoC. The packed image identifies the
chip family as `AC635N`; this is an image field, not a transcription of the
package marking. Physical SPI NOR capacity is **1 MiB** (measured by full flash
dumps). The SDK exposes 192 KiB of internal RAM. No populated external PSRAM
has been established. The board configuration uses a 24 MHz oscillator and a
24 MHz system-clock setting; runtime clocks can differ.

## Pin assignments

These assignments are those used by the working input code in
[`src/input.h`](../src/input.h), with qualifications below.

| Signal | Pin |
|---|---|
| button a | PA6 |
| dpad up | PA2 |
| dpad down | PA4 |
| button l4 | PA5 |
| button b | PA0 |
| button x | PA7 |
| dpad right | PA10 |
| button y | PA13 |
| button start | PA14 |
| button select | PA15 |
| trigger l2 | PB3 |
| button m | PB7 |
| dpad left | PB8 |
| trigger r2 | PB10 |
| button home | PB11 |
| button r1 | PC0 |
| button l3 | PC1 |
| button r3 | PC2 |
| button r4 | PC4 |
| button l1 | PC7 |
| button screenshot | PD5 |
| stick uart rx | PA3 |
| sensor power | PB5 |
| i2c scl | PB4 |
| i2c sda | PB6 |
| usb sense | PC3 |
| led power | PB9 |
| led spi di | PC3 |
| led spi clock | PC4 |
| led spi data | PC5 |
| left motor PWM | PA9 |
| right motor PWM | PB0 |
| configured reset input | PB1 |
| left trigger stop switch | PR1 |
| right trigger stop switch | PR0 |

Digital buttons and D-pad inputs are active low, with internal pull-ups enabled.
Trigger travel is analog on PB3/PB10, read through `AD_CH_PB3` / `AD_CH_PB10`;
the PR stop switches are separate active-low inputs. The sensor supply enable
PB5 is driven high. LED supply enable PB9 is active low.

## Pin multiplexing and unresolved labels

- Stick reception is UART2 RX on PA3. SDK UART setup can disturb PA2 (D-pad up);
  the app restores PA2 as a pulled-up input after opening the UART.
- PC3 is named both USB sense and LED SPI DI. PC4 is named both R4 and LED SPI
  clock. These SPI labels describe the peripheral pin group, **not evidence
  that an external LED module uses a clock or data-in wire**. The SDK LED
  driver only configures PC5 as output for its one-wire waveform.
- PC3 did not reliably assert as a USB-presence signal during normal app use.
  USB OTG state and CDC activity were needed to avoid accidental sleep. The
  loader-entry code still waits on PC3 after tearing USB down; behavior in
  that state should not be inferred from normal-operation readings.
- PB1 is configured as an active-low reset input with `CONFIG_RESET_TIME=8`.
  This is the configuration value, not a measured duration in seconds.
- Home is PB11, also configured as a filtered falling-edge low-power wake input.

Sources: `src/input.c`, `src/init.c`, `src/rumble.c`,
`src/sdkconfig/board_config.h`, SDK `cpu/br23/led_spi.c`.
