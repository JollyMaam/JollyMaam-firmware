# Stock power management

Evidence from the stock pad application image, PID `AC695X_TRANS`, VID `N2LT`.
Addresses here belong to this image and are not portable to other builds.

## Address mapping

Flash code is at application file offset plus `0x01e00120`. Startup copies
`0x5d68` bytes from file offset `0x59408` to SRAM address zero. RAM code below
is represented at alias `0x02000000`; initialized RAM data is addressed without
that alias. Disassembling the copied bytes at their file position gives wrong
relative branch destinations.

## Sleep callbacks and wake

Initialization at `0x01e25d76..0x01e25da0` installs these power callbacks:

| Callback | Address |
|---|---|
| Sleep entry | `0x01e42b8c` |
| Sleep exit | `0x01e42b9a` |
| Soft power-off pin setup | `0x01e42c54` |

The callbacks are stored at offsets `384`, `388` and `400` in the power state
at SRAM `0xc2a8`. The wake table passed during initialization is at
`0x01e536e8`. Its port 1 and port 2 records point to SRAM `0x55d4` and
`0x55d9`:

| Port | Record bytes | Meaning |
|---|---|---|
| 1 | `01 01 02 11 01` | PB1, falling edge, pulls enabled, Bluetooth resume, filter enabled |
| 2 | `00 00 02 00 01` | PA0, rising edge, pulls disabled, Bluetooth resume, filter enabled |

The charge and sub-wakeup records both have attribute `2` (Bluetooth resume).

There is also another wake table at `0x01e4dc98`, used by the path at
`0x01e400be`. Its PA0 record specifies a falling edge. The circumstances
selecting that path are not established; it is not the initialization table.

The presence of sleep callbacks does not establish the stock pad's actual
sleep duty cycle or current.

## Soft power-off

The wrapper at `0x01e4064c` calls RAM code at `0x02001d04` and then loops.
That RAM routine calls `0x01e4034c`, which invokes the registered pin callback
through the power state's offset `400`. It then disables peripheral registers
and repeatedly enters the hardware shutdown sequence. This is a soft
power-off path, distinct from ordinary CPU idle.

The pin callback at `0x01e42c54` parks GPIOs while protecting flash pins.
Its later per-pin input setup includes PB1 with a pull-up and PA0 without
pulls. It also configures PB11 with a pull-up and PA9 with a pull-down.
The helper at `0x01e366e2` configures inputs and pulls; the PA9 operation is
not an output-low power-latch operation.

The callback also clears bits 1 and 2 of P33 `0x02` (`P3_ANA_CON2`) at
`0x01e42e20..0x01e42e2c`, and bit 0 of P33 `0x13` (`RC32K_EN`) at
`0x01e42e30..0x01e42e34`. The purpose of `P3_ANA_CON2` bits 1 and 2 is not
established. The common shutdown routine additionally zeros P33 `0x13` and
`0x14` when its RTC/timer conditions are false.
The stock PR0 input initialization also sets P33 `0x02` bits 0..2 to `1`,
clearing bits 1 and 2 at that point; see [re-battery.md](re-battery.md).

At `0x01e42df8` the callback reads PB1 and loops while it remains low, waiting
for Home release before returning. A separate battery-disconnecting latch
operation in this shutdown path is not established.

Whole-board off current, peripheral supply switching, and the stock firmware's
inactivity and emergency shutdown thresholds are not established. Its voltage
and charge indications are described in [re-battery.md](re-battery.md).
