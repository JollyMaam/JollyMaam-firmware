# Headset audio hardware and stock firmware

Stock pad 3.52 uses the BR23's on-chip stereo DAC and microphone ADC, with
PA8 controlling the path described by its logs as the audio power amplifier.
Headset detection uses the SAR ADC on PA12; microphone capture configures PC6
as an analog input. These are recovered software behaviours, **not PCB
continuity or live audio measurements**.

Proposed features and software design are in
[Audio ideas](../architecture/audio_ideas.md).

## Inputs and method

Evidence from stock pad 3.52 and dongle 1.21:

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
is unconfirmed. PC7 is the pad's L1 button in the working pin map; the current
SDK's optional internal microphone bias uses that same pin.

The stock DAC initializer is especially useful, but its configuration ABI is
older than the current public SDK. Stock reads the packed settings word at
the beginning of the object. The current `dac_platform_data` begins with two
callback pointers, followed by its settings word. Copying `22 83 06 00` into
the beginning of the current struct would produce an invalid callback pointer.
The two configuration layouts are not interchangeable. Stereo output is firmly
established; electrical settings have not been fully mapped to the current ABI.

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
The counter producer and the delay in milliseconds remain unresolved.

Audio initialization at `0x01e522a2` calls the DAC initializer and restores
stock VM item 88, a two-byte adjustment used by the DAC path, and item 92,
eight bytes of DAC trim data. Invalid/missing trim triggers a calibration and
write-back. These VM IDs describe stock storage, not hardware constants.

### Unresolved hardware details

- Package DAC L/R pins, jack contact routing, coupling components, amplifier
  identity and the electrical effect of PA8.
- Microphone bias supply/network, jack pinout and microphone-to-PC6 continuity.
- PA12 voltages with the jack empty and with TRS/TRRS plugs inserted; whether
  detection uses a mechanical contact or an impedance network.
- Gain, DC offset, mute behaviour, insertion pops and output levels with the
  recovered stereo configuration.

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
path, beyond a build flag or unused codec name. Its buffer/format and interrupt
ABI have not been fully recovered.

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
strongly suggest this stock data route:

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

## Capacity relevant to audio

The pad has 1 MiB physical flash; the dongle has 512 KiB. The inspected stock
dongle application occupies about 282 KiB and contains audio code. Internal
RAM is 192 KiB in the BR23 SDK address map; populated external PSRAM on the
pad has not been established. See [Board and GPIO](board.md) and
[Memory map](memory-map.md).

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
The generic SDK board settings describe the reference board, not measured
electrical settings for this pad.
