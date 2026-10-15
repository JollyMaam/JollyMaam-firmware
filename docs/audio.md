# Audio hardware

The BR23 has an on-chip stereo DAC and an on-chip microphone ADC. The SDK build
enables the on-chip audio path (`CONFIG_AUDIO_ONCHIP`) and the SBC and mSBC
codec hardware (`CONFIG_SBC_CODEC_HW`, `CONFIG_MSBC_CODEC_HW`).

The pins and settings below are what the stock pad firmware (3.52) drives.
They are from the stock firmware's code, not from schematics or from
measurements of the board. The PCB and jack wiring are not established.

## Pins and paths

| Signal | Pin | Stock behaviour |
|---|---|---|
| DAC output | on-chip | stereo L/R (DAC output mode 2, `DAC_OUTPUT_LR`) |
| Amplifier enable | PA8 | active high. Drives the audio power amplifier. The amplifier part is not established. |
| Headset detect | PA12 | SAR ADC channel 4 (`AD_CH_PA12`) |
| Microphone input | PC6 | configured as analog input for capture |

With the DAC in stereo mode (`DAC_OUTPUT_LR`) and PA8 high, DAC output is
audible on headphones in the pad's jack.

With PA12 as an analog input without pulls, the 10-bit SAR reading is about
1015 with a three-contact headphone plug in the jack and 0 with the jack empty.

The amplifier enable is sequenced by stock firmware: it is set after a delay
following the enable request, and cleared immediately on disable. The delay's
time base is not established.

The microphone uses the on-chip ADC. The stock configuration bytes for the
microphone path are `00 02 00 10 02 00 00 00`. Byte 4 selects microphone LDO
voltage code 2 and leaves the internal bias off. The SDK's `adc_platform_data`
layout is the reference for these fields. The external bias network is not
established.

PC7 is L1 on the pad ([board.md](board.md)). The SDK's optional internal
microphone bias also uses PC7.

## Not established

- DAC L/R package pins, jack contact routing, and coupling components.
- Amplifier part number and the electrical effect of PA8.
- Microphone bias supply and network. Jack pinout, and which contact is the
  microphone.
- The headset-detect reading with a four-contact (TRRS) plug. Whether
  detection is a mechanical contact or an impedance network.
- Gain, DC offset, mute behaviour, insertion pops, and output level with the
  stereo configuration.

## USB audio

Both the stock pad and the stock dongle carry the same UAC Type I descriptors.
The playback and capture formats are:

| Direction | Format | Endpoint | Max packet | Interval |
|---|---|---|---|---|
| Playback (host to device) | 48 kHz, 2 channels, 16-bit PCM | OUT `0x03`, attributes `0x09` | 192 bytes | 1 |
| Capture (device to host) | 16 kHz, 1 channel, 16-bit PCM | IN `0x83`, attributes `0x05` | 32 bytes | 1 |

At full-speed USB, 192 bytes is one millisecond of 48 kHz stereo 16-bit PCM,
and 32 bytes is one millisecond of 16 kHz mono 16-bit PCM.

The stock pad offers its audio interfaces only while a plug is in the jack. It
re-enumerates on USB when the plug goes in or out. Its feature units report
playback volume and microphone capture volume as 0 to 100 in steps of 1.

On the stock pad, the amplifier hiss stops as soon as the host closes the
playback stream, and starts again when the host opens it.

Wired in its Xbox 360 mode, with a plug in the jack, the stock pad (3.52)
enumerates as `3537:1053`, `bcdDevice` `0x0352`, product string "Xbox 360
Controller for Windows", with five interfaces:

| Interface | Class | Endpoints |
|---|---|---|
| 0 | vendor `ff/5d/01` (XInput) | interrupt IN `0x82`, OUT `0x02`, 32 bytes |
| 1 | HID, report descriptor 214 bytes | interrupt IN `0x84`, OUT `0x04`, 64 bytes |
| 2 | audio control | none |
| 3 | audio streaming, playback | OUT `0x03` as in the table above |
| 4 | audio streaming, capture | IN `0x83` as in the table above |

In this mode the playback output terminal is a speaker (`0x0301`). Capture
runs from a microphone input terminal (`0x0201`) through a feature unit and a
selector unit. Linux binds xpad to interface 0, usbhid to interface 1 and
snd-usb-audio to interfaces 2 to 4.

The stock images also contain a 48 kHz mono capture descriptor. Which host mode
advertises it is not established.

## Transport between pad and dongle

The stock dongle and pad both contain SBC encoder and decoder code and
mode-specific audio packet handling. The wireless transport for audio between
them is not established. The codec parameters, packet format, and whether the
link uses A2DP, HFP, BLE ISO, or a proprietary radio format are not established.
