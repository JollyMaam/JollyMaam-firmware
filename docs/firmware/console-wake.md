# Raw radio transmission: console wake

Reverse-engineered construction of the Switch 1 wake transmission as this
firmware sends it. The BR23 raw transmit path and its constraints are in
[../jieli/radio.md](../jieli/radio.md).

## Raw console wake transmission

The Switch 1 wake transmission is built as follows. The Switch 1 reference
transmits a code with these properties:

- The access code uses the console's sync word with its lower 32 bits inverted.
  Parity bytes 0–3 are XORed with `0xff`. Byte 4 is unchanged.
- The phase-one body repeats a rotated 32-bit wake code in DH3 and DH5 packets,
  one every six slots (3.75 ms).
- The whitening seed is not verified. The app sweeps 32 seeds, dwelling 250 ms
  on each, and pre-cancels the payload with the same seed.

What is verified: the wake access code is emitted on the air. Not verified: the
payload, the whitening, and that a console wakes. Phase two (receive and clock
extraction) and phase three (transmit) are not implemented.

Switch 2 does not use raw transmission. It wakes from a flag in its LE
advertisement, with wake byte `0x81` and the paired console address. Do not
substitute the Classic raw wake for it.
