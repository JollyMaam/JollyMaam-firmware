# Audio blocks

The BR23 has an on-chip stereo DAC and an on-chip microphone ADC, plus SBC and
mSBC codec hardware. The SDK enables them with `CONFIG_AUDIO_ONCHIP`,
`CONFIG_SBC_CODEC_HW` and `CONFIG_MSBC_CODEC_HW`.

## Registers

In the AUDIO block (`0x1e2f00`, [soc.md](soc.md)), from instructions in
stock images:

| Address | Register |
|---|---|
| `0x001e2f00` | `DAC_CON` |
| `0x001e2f04` | `DAC_ADR` |
| `0x001e2f30` | `DAC_DTV` |
| `0x001e2f40` | `ADC_CON` |

A Ghidra export with the SDK's MMIO names labels `0x001e2f00` as
`JL_AUDIO_DAC_DTV`; that label is wrong.

## Configuration

- DAC output mode 2 is stereo L/R (`DAC_OUTPUT_LR`).
- The microphone's configuration bytes follow the SDK's `adc_platform_data`:
  channel selector, LDO current selector, capless flag, bias-resistance
  selector, flags byte (bit 2 tested before the bias-resistance value is set;
  the byte also carries the microphone LDO voltage code). These are selector
  values, not voltages or resistances.
- Older SDK builds put the packed DAC settings word at the start of the DAC
  platform object. The current `dac_platform_data` starts with two callback
  pointers, followed by the settings word. The two layouts are not
  interchangeable.
