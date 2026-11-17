# Boot loader

8BitDo's USB boot loader on the U2CB, at `0x01000000..0x01017000`. It
speaks the same request framing as the Ultimate 2C Wireless's loader
([../8bitdo-ultimate2c-wireless/](../8bitdo-ultimate2c-wireless/), "Boot
loader"), with different addresses, replies and flash handling.

USB `2dc8:3208`; identify (command 8) returns `1a 30`.

## Entry

- By hand: with USB unplugged, hold L1 and R1, tap Home, plug in USB.
- Stock v1.01 has no host command for it (no `0x81` output report).
- The loader stays (does not start the app) when, in order
  (main `0x01005970`, checks at `0x01002af8`):
  1. the chip-bound signature in the version page is bad (`0x01004e94`);
  2. RAM `0x2fff0` holds `0x5a5aa5a5` (it clears it); `0x2fff4` =
     `0x20210624` also sets its flag at `0x200d9`;
  3. the version word at `0x01017040` is 0 or all ones;
  4. L1 and R1 read down twice, 100 ms apart (`0x01002eec`).
  Otherwise it sets `0xf8564` = 1 and jumps to `0x01018001`.
- RAM survives a soft reset, so the mailbox words or L1+R1 held through a
  soft reset (USB plugged in) bring up the loader without the unplug.
  Another button held through it does not.
- Stock enters it at `0x01021410`: writes both mailbox words, stops
  SysTick, sets `0xf8564` = 2 and jumps to `0x01000201` (no chip reset: that
  would clear RAM). It sends radio core command `0x12` first.

## Interrupt routing

Mode register `0xf8564`:

- 2: the loader's own interrupt and fault handlers run.
- otherwise: forwarded to the app: faults to app + 4 + offset, IRQ n to
  app + `0x9c` with r0 = 4n.

Command 7 (start the app after a flash) leaves it at 2.

The ROM's HardFault handler (`0x11c`): with OTP word 0 = `0x7777` and
`0xf853c` bit 0 clear, a chip reset; otherwise it pushes {r4, lr} and calls
the loader's stub `0x01000205` (in mode 2: `b .` forever).

Reset vector `0x01000400`, initial SP `0x2ff00`.

## Commands

| Command | Does |
|---|---|
| 2 | version (not established) |
| 3 | program a stream (below) |
| 4 | erase |
| 5 | read |
| 7 | start the app (no reply) |
| 8 | identify, length 2: `1a 30` |

Replies to 2, 3 and 4 are status only: `00 05 <u16 status> <u16 command>
...`; the length field of an erase reply is 4. A refusal is status 1 with a
code at reply offset 8.

### Read (5)

Takes CPU addresses and returns flash as the CPU sees it (deciphered).

- Mask ROM `0x0..0x7000`, SRAM `0x20000..`, flash `0x01017000..0x01080000`.
- Refused: the loader itself (`0x01000000..0x01017000`) and from
  `0x01080000`.
- `0x7000` resets the chip; `0x20170` hangs the loader.
- The CPU can read the loader's code itself.

### Writing the app

The sequence that writes an image:

1. command 8, length 2, total 2;
2. command 4 at `0x01018000`, total = the image length;
3. command 3 in 46-byte chunks from `0x01018000`, total = the same length,
   each chunk enciphered with the 8BitDo Yichip transform (alphaorderly's
   8cryptdo `firmware.py`, `transform(..., encrypt=True)`), the block
   counter counted from `0x01018000`.

- Only CPU addresses work; logical offsets (`0x58000`) are refused (code 2).
- Command 3 not starting at `0x01018000`, or with a total other than the
  erase's, is refused (code 2; code 0 for total `0x64000` off the base). A
  refused write ends the stream: later writes are acknowledged and do
  nothing.
- Only whole 512-byte blocks of the stream are programmed: a short last
  block is dropped.
- The stream's last 512-byte block does not read back as written (it reads
  `28 99 ff ff ff ff 28 99 ...`); cause not established. Its blocks before
  it do.
- The erase covers the total rounded up to 64 bytes. Erases shorter than
  32 KiB go 32 bytes at a time (`0x01001b60`) and are slow.
- The loader's erase of a 32 KiB window (`0x01005518`) reads the 16 bytes
  it shares with each neighbouring window first and writes them back (see
  the cipher in [../firmware/updater.md](../firmware/updater.md)).
- Command 4 alone in a fresh session (`0x01070000`, total 4096) erases
  that sector. Right after a full stream it got no reply and the loader
  stopped answering identify.

Without step 2, the loader acknowledges every write but erases sectors
that held data and programs none of them; once it programmed blank sectors
with a different transform (per 512-byte block one key `K = seedword(e) ^
(block << 11)`, `w0 = c0 ^ K`, `wi = ci ^ c(i-1) ^ K`). Writing without the
erase first destroys the app.
