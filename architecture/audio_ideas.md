# Audio ideas

Audio could extend the pad with wired headset support, a standard Bluetooth
audio profile, or a custom dongle link. The recovered hardware configuration,
stock pad/dongle audio paths, binary addresses and reproducible byte checks
are in [Headset audio hardware](../docs/audio.md).

These are proposed features; audio support has not been implemented or tested
on hardware in the app.

## Possible features for the app

| Feature | Why it is useful | Main dependencies |
|---|---|---|
| Local PCM playback and microphone capture | Establish the analog path with small, inspectable components | DAC/ADC driver ABI, PA8 sequencing, calibration, PA12 detector |
| USB headset on the pad | Useful wired audio and an independent test of both directions | Composite USB HID/UAC, isochronous endpoints, PCM buffers and clock handling |
| A2DP Sink experiment | Learn standard Bluetooth stereo playback using a phone/PC | BTstack AVDTP/A2DP/SBC and a BR23 PCM output backend; measure buffering latency |
| HFP Hands-Free experiment | Learn a standard bidirectional voice profile | Microphone/output drivers, SCO/eSCO transport and voice codecs |
| Custom dongle headset + gamepad | Control wireless latency, both audio directions and input scheduling | Both firmwares, USB HID/UAC on dongle, framed compressed audio and clock synchronization |
| LE Audio experiment | Learn LC3 and isochronous Bluetooth audio | Proven controller ISO commands/data transport, LC3 and LE Audio services |

BTstack already supplies `example/a2dp_sink_demo.c` and
`example/hfp_hf_demo.c` in `/work/cyclone2-btstack`. A2DP playback uses the
ACL/L2CAP path and has no microphone return channel. HFP requires additional
transport work: the app currently rejects outgoing `HCI_SCO_DATA_PACKET`
in [btstack_hci_jieli.c](../src/btstack_hci_jieli.c). Reference:
[BTstack profiles](https://bluekitchen-gmbh.com/btstack/profiles).
Controller `ll_cis`/`ll_bis` names do not establish a working LE Audio platform;
verify actual ISO capabilities before choosing that path. Reference:
[LE Audio specifications](https://www.bluetooth.com/learn-about-bluetooth/feature-enhancements/le-audio/le-audio-specifications/).

For a custom dongle, the initial architecture could be:

```text
PC <-> USB Audio + USB HID <-> dongle <-> timed compressed packets <-> pad
                                                                  DAC/ADC
                                                                  controls
```

Start with a custom service over a supported Bluetooth connection if practical.
Owning both ends does not by itself provide a usable raw-radio driver. Select
the bearer after measuring payload throughput and scheduling under simultaneous
audio and gamepad traffic; recovering stock packetization can inform that choice.

The radio link needs sequence numbers, sample timestamps, bounded queues,
loss handling, and explicit scheduling room for input. Audio packets become
obsolete when their playback deadline passes. Handle clock drift with a
measured rate-control/resampling strategy, and include USB endpoint
synchronization in that design. Keep the input path's newest-wins behaviour.
Use fixed-point processing: this BR23 firmware emulates floating point.

48 kHz stereo 16-bit PCM is **1.536 Mbit/s**; adding a 16 kHz mono microphone
adds **0.256 Mbit/s**, before radio overhead. Compression is therefore an
attractive starting point. Stock SBC is worth investigating first; a simpler
ADPCM experiment could help separate transport problems from SBC integration.
Choose frame size and buffering from latency measurements, not codec defaults.

The [stock image and capacity evidence](../docs/audio.md#capacity-relevant-to-audio)
makes dongle audio plausible, but does not estimate the app plus BTstack's size.
Measure linked flash, RAM, stack and CPU use with the proposed USB/profile/codec
combination; leave
room for the loader, configuration and persistent storage. Audio buffers consume
RAM, and flash capacity does not determine ongoing stream bandwidth.

Keep audio as a separate timed service, with chip-dependent DAC/ADC/codec/USB
operations behind the platform boundary. Gamepad profile code should continue
to handle host protocol bytes. Let the controller own audio start/stop and
jack-state policy, while DMA completion and bounded audio work run separately
from the input report loop. Whether to expose an experimental audio-only slot
or combine HID and audio services can be decided after standalone bring-up.

## Suggested milestones

1. Trace the jack circuit and verify PA8/PA12/PC6. Translate the stock DAC and
   ADC settings into the current SDK ABI, including calibration and sequencing.
   Establish quiet output before enabling the amplifier, and recover the stock
   delay counter's timebase before choosing a delay in milliseconds.
2. Play quiet PCM, then a low-level stereo test signal. Capture microphone PCM
   and observe PA12 transitions. Measure rates and buffer health.
3. Bring up wired USB playback/capture, or A2DP Sink if learning Bluetooth is
   the immediate priority. Keep gamepad timing measurements alongside audio.
4. Recover stock SBC frame settings and accelerator API. Compare a software
   decoder with the SDK path for CPU use, memory and latency.
5. Prototype custom dongle audio with bounded queues, clock-drift correction,
   loss handling, and prioritized gamepad traffic. Measure end-to-end latency
   in both directions before optimizing.
6. Explore HFP or LE Audio once their controller transport prerequisites work.
