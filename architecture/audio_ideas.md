# Audio ideas and stock firmware evidence

Audio is a plausible extension of this firmware. Stock pad 3.52 uses the BR23's
on-chip stereo DAC and microphone ADC, with PA8 controlling the path described
by its logs as the audio power amplifier. Headset detection uses the SAR ADC
on PA12; microphone capture configures PC6 as an analog input. These are
recovered software behaviours, **not PCB continuity or live audio measurements**.

This page records the evidence and proposed architecture. No audio feature has
been implemented or tested on hardware as part of this investigation.

## Inputs and method

Investigated on 2026-10-07:

| Image | Application bytes | SHA-256 of `app.bin` |
|---|---:|---|
| `/shared/gamesir-firmware/fw_3.52.bin_unpack/files/app.bin` | 469545 | `4d1a2380362110d6e2c5802c2b1d56c11c742db5e53cbada4aa142249f3f5df3` |
| `/shared/gamesir-firmware/donglefw_1.21.bin_unpack/files/app.bin` | 289172 | `f59812a7544ccf08381a8ee2143e1c802c8bb9f2265573203937e4a00c118e2d` |

The pad image matches the one behind `/work/decompilation/firmware_functions.c`
and `/work/decompilation/recovered_01e46b16.c`. Those exports helped locate
functions. Findings below were checked against fresh PI32v2 disassembly and
raw bytes of the requested images, with public BR23 SDK headers as references.
Additional Ghidra decompilation used a read-only project; it did not change the
existing analysis project.

Address conventions matter:

- Pad flash code: application file offset + `0x01e00120`. Startup copies
  `0xb6f1` bytes from file offset `0x67310` to SRAM address zero. The existing
  analysis represents that SRAM code at alias `0x02000000`.
- Dongle flash code: application file offset + **`0x01e000c0`**. This differs
  from the pad. The string table beginning at file offset `0x15a20` is addressed
  as `0x01e15ae0`, corroborating that base. Startup copies `0x4f40` bytes from
  file offset `0x41a54` (source address `0x01e41b14`) to SRAM address zero.
- Stock RAM globals below are build-specific addresses, not addresses for the
  app to reuse. A RAM initializer in the pad is at file offset `0x67310 + ram`.

The old Ghidra export has misleading audio MMIO names and instruction semantics.
For example, `0x001e2f00` is **DAC_CON**, although the export labels it
`JL_AUDIO_DAC_DTV`; DAC_ADR is `0x001e2f04`, DAC_DTV is `0x001e2f30`, and
ADC_CON is `0x001e2f40`. Some bit-clear instructions become an incorrect
`register & mask` in pseudocode. Scaled halfword indexes also produce wrong
global addresses. Use raw instruction effects for these findings.

## Recovered pad hardware configuration

| Signal/path | Recovered behaviour | Evidence and qualification |
|---|---|---|
| Playback | On-chip DAC, output mode **2: stereo L/R** | DAC initialization at `0x01e2cec2` reads configuration word `0x00068322` at RAM `0xa174`. Its low nibble is 2; the stock output selector at `0x01e2c85a` and SDK `DAC_OUTPUT_LR` identify that mode. Package pins and jack routing remain unmeasured. |
| Amplifier control | **PA8, active high** in stock software | Disable at `0x01e342ca` resolves GPIO 8 and clears OUT bit 8. The delayed enable at `0x01e489b8..0x01e489d2` resolves GPIO 8 and sets OUT bit 8, followed by `enable audio power amplifier!!!!!`. This identifies a control signal, not the amplifier IC. |
| Jack detection | **PA12 / SAR ADC channel 4** | Scan table at `0x01e59110`, slot 4 = channel 4. SAR IRQ at SRAM alias `0x02001554` writes scan results to RAM `0xf520 + 2 * slot`; headset logic at `0x01e462b2` reads slot 4 at `0xf528`. SDK channel 4 is `AD_CH_PA12`. |
| Microphone input | **PC6** configured for analog capture | Microphone analog open at `0x01e2dbb8` sets PC6 DIR and clears DIE, PU and PD. The capture-open path at `0x01e45da0`, call `0x01e45ef4`, invokes it. Physical jack microphone conductor remains unmeasured. |
| Microphone configuration | Initial bytes `00 02 00 10 02 00 00 00` | RAM `0xa178`; ADC initialization at `0x01e2e048` installs this pointer. Microphone open consumes byte offsets 1–4. See interpretation below. |

The first five microphone bytes fit the SDK's `adc_platform_data` fields:
channel selector 0, LDO current selector 2, capless disabled, bias-resistance
selector 16, and flags byte 2. Under the current header's flag layout, byte 4
selects microphone LDO voltage code 2 and leaves internal bias selection off.
The stock driver tests bit 2 of that byte before installing the bias-resistance
setting, consistent with this interpretation. **Do not equate these selector
values with measured voltage or resistance.** The external bias network still
needs tracing. PC7 is the pad's L1 button in the working pin map; the current
SDK's optional internal microphone bias on PC7 should not be enabled casually.

The stock DAC initializer is especially useful, but its configuration ABI is
older than the current public SDK. Stock reads the packed settings word at
the beginning of the object. The current `dac_platform_data` begins with two
callback pointers, followed by its settings word. Copying `22 83 06 00` into
the beginning of the current struct would produce an invalid callback pointer.
Translate recovered fields into named current-SDK fields and verify the driver
ABI. Stereo output is firmly established; electrical settings need a careful
field-by-field translation.

### Jack state and amplifier sequencing

The scan table is eight-byte records containing slot number and ADC channel:

```text
slot:       0   1   2   3   4   5        6
ADC channel 6   9   0   5   4   0x0f     0x50000f
                         ^
                         headset detector, PA12
```

At runtime, `0x01e462b2` forms `u16(sample << 2)`, treats values above 512
as the inserted side of the state transition, and applies a debounce counter
before emitting `headset inster` / `headset remove` messages. Boot-time handling
at `0x01e3bc3a` uses the same sample with a different threshold, **3711**.
These are distinct stock decisions; neither threshold is a calibrated voltage
or a universal replacement detector setting. The old pseudocode's `0xf064`
address and `0x3fc` threshold do not match these instructions.

The stock enable helper at `0x01e3539e` sets an enable-pending flag, clears
a counter, and sets bit 16 of analog register `0x001e19cc`. The main task later
waits until that counter exceeds 300 before driving PA8 high. Disabling clears
PA8 immediately. The duration of a counter increment was not established here.
Preserve the ordering during bring-up: initialize/calibrate the DAC, establish
quiet output, then enable the amplifier. Recover the counter producer before
choosing a delay in milliseconds.

Audio initialization at `0x01e522a2` calls the DAC initializer and restores
stock VM item 88, a two-byte adjustment used by the DAC path, and item 92,
eight bytes of DAC trim data. Invalid/missing trim triggers a calibration and
write-back. This is evidence that analog calibration matters. Stock VM IDs
are not a proposed settings layout for the app.

### Remaining hardware questions

- Which package DAC L/R pins reach which jack contacts, and through what
  coupling components or amplifier? What does PA8 electrically enable?
- What supplies microphone bias? Confirm jack pinout, microphone contact and
  PC6 continuity; distinguish a TRS plug from a TRRS headset if necessary.
- Measure PA12 with the jack empty and with representative plugs inserted.
  Determine whether it senses a mechanical contact or an impedance network.
- Check gain, DC offset, mute behaviour, insertion pops and output levels with
  the recovered stereo configuration. Start with quiet output and low gain.

## What stock does with audio

### USB formats

Both images contain byte-identical UAC Type-I format/endpoint pairs:

| Image file offsets | Format | Endpoint immediately after format |
|---|---|---|
| Pad `0x617e4`; dongle `0x1f776` | 48 kHz, 2 channels, 16-bit PCM | OUT `0x03`, attributes `0x09`, maximum packet 192 bytes, interval 1 |
| Pad `0x61818`; dongle `0x1f7aa` | 16 kHz, 1 channel, 16-bit PCM | IN `0x83`, attributes `0x05`, maximum packet 32 bytes, interval 1 |

The playback descriptor is `0b 24 02 01 02 02 10 01 80 bb 00`; the microphone
descriptor is `0b 24 02 01 01 02 10 01 80 3e 00`. At full-speed USB those
packet sizes match one millisecond of the respective PCM streams. Other
descriptor templates include 48 kHz mono capture. **Template presence does
not establish which host mode advertises each configuration.** Enumerating
the stock devices in each mode is still needed for that mapping.

### Pad playback and capture

The pad's `joypadtask` entry at `0x01e46b16` handles event `0x400005` with a
mode/state guard and logs `bt audio init`. That path initializes microphone
and playback buffers, starts an SBC encoder, and opens an SBC decoder task.
Concrete landmarks are encoder-start/log code around `0x01e46c56` and
decoder-open code around `0x01e47198..0x01e47200`.

USB playback opens through `0x01e42b18` and `0x01e42c68`; the stream/mixer
path ultimately uses the on-chip DAC. `0x01e45d3e` defaults the microphone
stream rate to 16000, and `0x01e45da0` opens capture with an information word
carrying sample rate and channel count. It configures the on-chip microphone
driver and buffering. The capture callback at `0x01e4f9ae` includes the
`bt mic data cbuf full` path, corroborating microphone data movement into a
wireless-facing buffer.

Existing SDK byte matches in `/work/decompilation/functions.csv` identify
`sbc_decoder_hw`, `sbc_encoder`, `sbc_hwaccel`, audio mixer, DAC, ADC and sample
rate conversion routines. Stock therefore contains a hardware-oriented SBC
path, beyond a build flag or unused codec name. Integrating that accelerator
with BTstack still requires recovering its buffer/format and interrupt ABI.

### Dongle

The dongle initializer at `0x01e01f92` logs `usb audio demo init`, initializes
the audio subsystem, and reaches its DAC initializer at `0x01e3d806`. It also
starts an encoder and opens a decoder, with logs at `0x01e020f4` and
`0x01e0210a..0x01e02148`. Its microphone open path logs sample rate/channel
information around `0x01e10410`.

The dongle also processes headset state: `0x01e08d1a..0x01e08d70` checks mask
`0x60` in a received/state byte, debounces changes, and emits `Joypad Headset
Is Insert Usb Change!!` or the corresponding removal message. The full USB
reconfiguration effect needs tracing; the call sites establish that headset
state influences dongle behaviour.

Together, the USB descriptors and paired application encoder/decoder paths
strongly suggest this architecture:

```text
host USB playback PCM -> dongle SBC encode -> wireless -> pad SBC decode -> DAC
host USB microphone  <- dongle SBC decode <- wireless <- pad SBC encode <- ADC
```

This is an **inference about the complete route**, not a recovered wire
specification. Codec frame parameters, packet headers, wireless bearer,
scheduling, retransmission policy and end-to-end latency are unresolved.
Strings such as `tx ps4 f0 with audio` and `tx ps5 f1 sub id 03 with audio`
show mode-specific packet handling, but do not prove that the pad/dongle link
is A2DP, HFP, BLE ISO, or raw proprietary radio. SBC alone identifies none of
those transports. Likewise, generic controller SCO code is not evidence that
the dongle audio route uses SCO.

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

The pad has 1 MiB flash and the dongle 512 KiB. Stock dongle `app.bin` is about
282 KiB and contains audio code, so half-sized flash is not inherently fatal.
That is not a size estimate for the app plus BTstack. Measure linked flash,
RAM, stack and CPU use with the proposed USB/profile/codec combination; leave
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

## Reproducing the key byte checks

The following reads the supplied images, checks identity and prints the pad
hardware initializers, SAR scan table and matching USB audio descriptors:

```python
from pathlib import Path
from hashlib import sha256
from struct import unpack_from

root = Path('/shared/gamesir-firmware')
pad = (root / 'fw_3.52.bin_unpack/files/app.bin').read_bytes()
dongle = (root / 'donglefw_1.21.bin_unpack/files/app.bin').read_bytes()
assert sha256(pad).hexdigest() == '4d1a2380362110d6e2c5802c2b1d56c11c742db5e53cbada4aa142249f3f5df3'
assert sha256(dongle).hexdigest() == 'f59812a7544ccf08381a8ee2143e1c802c8bb9f2265573203937e4a00c118e2d'
print('DAC word:', hex(unpack_from('<I', pad, 0x67310 + 0xa174)[0]))
print('MIC config:', pad[0x67310 + 0xa178:0x67310 + 0xa180].hex(' '))
for slot in range(7):
    print('SAR:', unpack_from('<II', pad, 0x59110 - 0x120 + slot * 8))
for image, offsets in [(pad, (0x617e4, 0x61818)),
                       (dongle, (0x1f776, 0x1f7aa))]:
    for offset in offsets:
        print(hex(offset), image[offset:offset + 20].hex(' '))
```

For disassembly, wrap each image in an executable ELF section using
`/shared/jieli-linux-toolchains-20250324.1/pi32v2/bin/{cc,ld,objdump}`. Set
the link address to the appropriate base above, and use `objdump -d` with
bounded address ranges. Disassemble the pad's copied SRAM segment separately
at its analysis alias for the ADC IRQ. Linear disassembly of string/data
regions is not evidence of instructions.

SDK references are under
`/shared/github.com-Jieli-Tech/fw-AC63_BT_SDK/include_lib/`:
`driver/cpu/br23/asm/{br23.h,gpio.h,adc_api.h}` and
`media/media_develop/media/cpu/br23/asm/{dac.h,audio_adc.h}`.
The generic SDK board settings are a driver reference; the stock configuration
and board measurements should determine this pad's settings.
