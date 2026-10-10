# Stock firmware: recovered code

Routine addresses, evidence and method from disassembly of the stock images. The
facts these support are in the other pages: audio in [audio.md](audio.md), power
in [power.md](power.md), the image format in
[re-application-image.md](re-application-image.md). The images are listed there
too.

## Method

Address conventions:

- **Pad:** flash code at application file offset + `0x01e00120`. Startup copies
  `0xb6f1` bytes from file offset `0x67310` to SRAM address zero. The analysis
  represents that copied code at alias `0x02000000`. RAM initializers are at
  file offset `0x67310 + ram`.
- **Dongle:** flash code at application file offset + `0x01e000c0`. The string
  table at file offset `0x15a20` is addressed as `0x01e15ae0`, which supports
  that base. Startup copies `0x4f40` bytes from file offset `0x41a54` (source
  address `0x01e41b14`) to SRAM address zero.
- Stock RAM globals are build-specific. Do not reuse them as addresses.

Problems with the Ghidra export, found while checking it against the
instructions:

- Audio MMIO names are wrong. `0x001e2f00` is `DAC_CON`, labelled
  `JL_AUDIO_DAC_DTV`. `DAC_ADR` is `0x001e2f04`, `DAC_DTV` is `0x001e2f30`,
  `ADC_CON` is `0x001e2f40`.
- Some bit-clear instructions decompile as `register & mask`.
- Scaled halfword indexes give wrong global addresses.

Artifacts (not in this repository): the stock 3.52 flash backup, the Ghidra
project for the 3.52 image, and the decompilation index with SDK byte matches,
all under `/work/`. The images are in `/shared/gamesir-firmware/`.

Use raw instruction effects for these findings. Disassemble with the toolchain's
`cc`, `ld` and `objdump` (`pi32v2`), wrapping each image in an ELF section at the
base above. Disassembly of data regions is not evidence of instructions.

Reference check (Python, reads the images and prints the values below):

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
for image, offsets in [(pad, (0x617e4, 0x61818)), (dongle, (0x1f776, 0x1f7aa))]:
    for offset in offsets:
        print(hex(offset), image[offset:offset + 20].hex(' '))
```

Run it with `python3 -I`. The `-I` flag keeps the script's directory out of the import path.

## Audio

The signals, their pins and the open questions are in [audio.md](audio.md).
This section gives where stock code drives them.

| Signal | Routine and evidence |
|---|---|
| DAC mode | DAC init at `0x01e2cec2` reads configuration word `0x00068322` at RAM `0xa174`. Low nibble 2 is `DAC_OUTPUT_LR`. The output selector at `0x01e2c85a` agrees. |
| Amplifier enable (PA8) | Disable at `0x01e342ca` clears GPIO 8. The delayed enable at `0x01e489b8..0x01e489d2` sets it, then logs `enable audio power amplifier!!!!!`. |
| Headset detect (PA12) | Scan table at `0x01e59110`, slot 4 = channel 4. The SAR IRQ at SRAM alias `0x02001554` writes results to RAM `0xf520 + 2 × slot`. Headset logic at `0x01e462b2` reads slot 4 at `0xf528`. |
| Microphone (PC6) | Open at `0x01e2dbb8` sets PC6 DIR and clears DIE, PU and PD. Capture open at `0x01e45da0`, call `0x01e45ef4`. |
| Microphone config | `00 02 00 10 02 00 00 00` at RAM `0xa178`. ADC init at `0x01e2e048` installs the pointer. Mic open reads bytes 1–4. |

The microphone bytes fit the SDK's `adc_platform_data`: channel selector 0, LDO
current selector 2, capless disabled, bias-resistance selector 16, flags byte 2.
Byte 4 selects microphone LDO voltage code 2. The stock driver tests bit 2 of
that byte before setting the bias-resistance value. These are selector values,
not voltage or resistance.

### Jack state

The scan table holds 8-byte records (slot, ADC channel):

```text
slot:       0   1   2   3   4   5        6
ADC channel 6   9   0   5   4   0x0f     0x50000f
                         ^
                         headset detector, PA12
```

At runtime `0x01e462b2` forms `u16(sample << 2)`. Values above 512 are treated as
the inserted side of a transition, with a debounce counter, before emitting
`headset inster` or `headset remove`. Boot-time handling at `0x01e3bc3a` uses
the same sample with threshold 3711. These are separate decisions. Neither is
a calibrated voltage. The old pseudocode's `0xf064` address and `0x3fc`
threshold do not match these instructions.

### Amplifier sequencing

The enable helper at `0x01e3539e` sets an enable-pending flag, clears a counter,
and sets bit 16 of analog register `0x001e19cc`. The main task drives PA8 high
only after the counter exceeds 300. Disabling clears PA8 at once. The counter's
producer and the time per increment are not established.

Audio initialization at `0x01e522a2` calls the DAC initializer, restores VM item
88 (a two-byte adjustment used by the DAC path), and VM item 92 (eight bytes of
DAC trim). Missing or invalid trim triggers calibration and a write-back. The
VM IDs are stock storage, not hardware constants.

The stock DAC configuration uses an older ABI than the current SDK. Stock reads
the packed settings word at the start of the object. The current
`dac_platform_data` starts with two callback pointers, followed by the settings
word. Copying `22 83 06 00` to the start of the current struct gives an invalid
callback pointer. The two layouts are not interchangeable.

### Stock audio routes

Pad playback and capture:

- `joypadtask` handles event `0x400005` with a mode guard and logs `bt audio init`.
  That path sets up microphone and playback buffers, starts an SBC encoder, and
  opens an SBC decoder task. Landmarks: encoder start around `0x01e46c56`,
  decoder open around `0x01e47198..0x01e47200`.
- USB playback opens through `0x01e42b18` and `0x01e42c68`. The stream and mixer
  end at the on-chip DAC.
- `0x01e45d3e` defaults the microphone stream rate to 16000. `0x01e45da0` opens
  capture with a word holding sample rate and channel count.
- The capture callback at `0x01e4f9ae` contains the `bt mic data cbuf full` path.

Dongle:

- The initializer at `0x01e01f92` logs `usb audio demo init`, initializes audio,
  and reaches the DAC initializer at `0x01e3d806`. It starts an encoder and
  opens a decoder (logs near `0x01e020f4` and `0x01e0210a..0x01e02148`).
- The microphone open path logs sample rate and channels near `0x01e10410`.
- `0x01e08d1a..0x01e08d70` checks mask `0x60` in a state byte, debounces it, and
  logs `Joypad Headset Is Insert Usb Change!!` or the removal message. The full
  effect on USB reconfiguration is not traced.

Inferred route (not established):

```text
host USB playback PCM -> dongle SBC encode -> wireless -> pad SBC decode -> DAC
host USB microphone  <- dongle SBC decode <- wireless <- pad SBC encode <- ADC
```

Strings such as `tx ps4 f0 with audio` and `tx ps5 f1 sub id 03 with audio` show
mode-specific packet handling. They do not identify the transport. SBC does not
identify A2DP, HFP or BLE ISO. The SDK's generic SCO code is not evidence of SCO.

Existing SDK byte matches in the decompilation index identify `sbc_decoder_hw`,
`sbc_encoder`, `sbc_hwaccel`, the mixer, DAC, ADC and sample rate conversion
routines. The buffer format and interrupt ABI are not recovered.

### Stock descriptor bytes

Both images contain byte-identical UAC Type I format and endpoint descriptors
(the table of formats is in [audio.md](audio.md)):

- Pad `0x617e4`, dongle `0x1f776`: 48 kHz playback descriptor
  `0b 24 02 01 02 02 10 01 80 bb 00`, followed by OUT `0x03`.
- Pad `0x61818`, dongle `0x1f7aa`: 16 kHz microphone descriptor
  `0b 24 02 01 01 02 10 01 80 3e 00`, followed by IN `0x83`.

A 48 kHz mono capture template is also present. Which host mode selects each
configuration needs enumeration of the stock devices.

## Power and charging

- Stock 3.52 contains pedestal and dock ADC detection, power-control code and
  charge-state strings. Its power-on function at `0x01e3a806` waits for a
  four-byte status block and enters charge and mode handling.
- The register writes for dock detection and the charger connection were not
  recovered.
- The SDK's charge driver is uninitialized in this firmware. Calls to
  `get_ldo5v_online_hw()`, `adc_check_vbat_lowpower()` and
  `get_charge_full_value()` blocked when it was.

## Dongle USB reconfiguration

The dongle's headset-state path (above) changes its USB audio behaviour. Which
descriptor set it advertises, and when, is not traced.
