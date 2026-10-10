# Power, charging, reset and loader entry

## Internal charger

The BR23 PMU has an internal charger. Its control register is `P3_CHG_CON0`
(P33 address `0x15`):

| Bit | Name | Meaning |
|---|---|---|
| 0 | `CHARGE_EN` | charger enable |
| 1 | `CHGBG_EN` | charger background enable |

This firmware clears both bits and does not initialize the SDK charge driver.
Under this firmware the internal charger is therefore off.

Not established: the external charger IC, its wiring, the charge termination
method, and the battery capacity.

The SDK's VBAT reading (`AD_CH_VBAT`) is scaled by 4 in the driver. A calibrated
state-of-charge reading is not established.

## Sleep and wake inputs

| Pin | Function | Configuration |
|---|---|---|
| PB11 (Home) | wake | filtered falling-edge, pull-up |
| PC3 | wake | filtered rising-edge; kept as an input during soft power-off |

Whether USB insertion drives PC3 and wakes the pad is not established.

The low-power configuration is in [soc.md](soc.md). Sleep current has not been
measured.

### USB presence signals

PC3 reads low while USB is attached during normal app operation. The OTG
state is `IDLE_MODE` and the PMU 5 V detector reads low in the same condition.
None of these signals distinguishes an attached cable from none. Software can
detect an active host from USB start-of-frame (SOF) activity instead.

A power-only USB source cannot currently be told apart from a disconnected
cable.

### Sleep policy

The timeouts are the app's, not hardware's:

- No host frames for 250 ms means no host.
- While idle, or after a Bluetooth disconnect, the pad sleeps after 15 seconds
  without host frames. It confirms the absence over four checks before power-off.
- After USB goes away, it sleeps after 5 seconds.

## Reset

### Reset sources

Reset causes are read from:

- `P33 RST_SRC` (P33 register `0x12`)
- `R3_RST_SRC` (RTC domain)

A bootloader or earlier code may read and clear these first. The app samples
them early in `app_main`.

SDK labels for `P33 RST_SRC` bits:

| Bit | Label |
|---|---|
| `0x01` | P33 reset |
| `0x02` | DVDD power-on reset |
| `0x20` | software reset |
| `0x40` | PLPM |

The register does not identify the cause alone. A reset from the loader, a
deliberate `p33_soft_reset()` and an unexpected fault all read `P33 = 0x40`.
A pin reset read `P33 = 0x01`.

### Reset pin

PB1 is the configured reset input, active low. `CONFIG_RESET_TIME` is 8. This
is the configuration value. The resulting duration is not established.

### Retained memory

SRAM at `0x20000` and `0x20010` survives software resets and loader resets while
power is on. See [memory-map.md](memory-map.md). Ordinary `.bss` is cleared at
every application boot.

### Boot counter and restarts

The app keeps a boot counter in retained SRAM. A restart is established when
the counter increases and uptime restarts together. USB re-enumeration or loss
of console output alone does not prove a restart.

## USB loader entry

The MaskROM USB loader is entered by a sequence the app runs before reset. The
loader identifies itself as `UBOOT1.00`.

### Registers

| Address | Register | Use in this sequence |
|---|---|---|
| `0x001e1800` | `USB_CON0` | cleared |
| `0x001e1804` | `USB_CON1` | cleared |
| `0x001e5100` | `USB_IO_CON0` | bit 11 forces SE0 on the USB lines |
| `0x00010800` | MaskROM boot-state token (16 bytes, SRAM) | set to 2 |

### Sequence

1. `nvram_set_boot_state(2)` writes the token and flushes the data cache.
2. Stop the Bluetooth controller. Stopping advertising alone is not enough.
3. Disable interrupts and clear the USB control registers.
4. Force SE0 for 1000 ms, then release it.
5. Wait 250 ms.
6. Wait for USB sense (PC3).
7. Re-arm the boot-state token.
8. Let a 256 ms watchdog expire to reset.

The delays are those of the current code. They are not established as hardware
minimums.
