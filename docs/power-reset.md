# Power, charging, reset and USB loader

## Charging evidence

The app clears `CHARGE_EN` and `CHGBG_EN`: bits 0 and 1 of the BR23 PMU charge
register `P3_CHG_CON0` (P33 address `0x15`). It does not initialize the SDK
charge driver. This proves that the app does not manage the **internal**
charger; it does not prove that the board cannot charge.

The pad works unplugged, establishing stored battery energy. That observation
alone does not measure charging current or prove that charging took place under
this firmware. The charger IC, wiring, charge termination and battery capacity
remain unconfirmed. An external charger is plausible, not established.

The SDK VBAT reading (`AD_CH_VBAT`, multiplied by four) was about **4.76 V**, not
a credible single-cell state-of-charge measurement. Uninitialized charge-driver
flags (`charge=0`, `poweron=255`) do not describe the board. Calls to
`get_ldo5v_online_hw()`, `adc_check_vbat_lowpower()` and `get_charge_full_value()`
blocked with that driver uninitialized.

Stock 3.52 firmware contains pedestal/dock ADC detection, power control and
charge-state strings. Its recovered power-on function `0x01e3a806` waits for a
four-byte status block and enters charge/mode handling. The relevant register
writes and charger connection were not recovered. Evidence artifact:
`/work/cyclone2-flash-backups/stock-3.52-20260921-141040.bin`; Ghidra project
`/work/analysis/cyclone2-fw-3.52-two-segment-v2.gpr`.

## Sleep and wake inputs

Home (PB11) is configured as a filtered falling-edge wake source with pull-up.
The low-power configuration enables sleep, keeps the Bluetooth oscillator
available and selects LDO15. These are configured settings, not measurements
of sleep current. Normal-operation USB sensing on PC3 was unreliable; USB OTG
state and CDC activity are additional evidence that power should stay on.

## Reset causes

The app samples `P3_RST_SRC` (P33 `0x12`) and `R3_RST_SRC` (RTC domain) early in
`app_main`; a bootloader or earlier reader may already have consumed a cause.
The SDK decoder returned zero on this pad and is not sufficient by itself.

| Observed event | P33 | RTC |
|---|---|---|
| loader reset after flash | `40` | `01` |
| deliberate `p33_soft_reset()` | `40` | `01` |
| unwired OS API call/fault | `40` | `01` |
| pinhole reset | `01` | not recorded |

**`p33=0x40` does not distinguish a deliberate reset from a fault.** The SDK's
labels are `01` P33 reset, `02` DVDD power-on reset, `20` software reset,
`40` PLPM; those labels alone do not establish the physical cause of an event.
`uxTaskGetStackHighWaterMark()` links to an unwired OS thunk and reproducibly
resets this pad. A linkable SDK symbol is not proof that its runtime API works.

The retained boot counter plus restarted uptime establish an app restart.
USB re-enumeration or loss of console output alone is not conclusive. Retained
SRAM survives the measured software/loader resets; complete power loss does
not preserve it. Ordinary `.bss` is reinitialized at each app boot.

## USB and MaskROM loader

The app uses USB0 CDC. The stick UART is a separate peripheral. Register
addresses used by the loader-entry path:

| Address | Register/use |
|---|---|
| `0x001e1800` | USB_CON0 |
| `0x001e1804` | USB_CON1 |
| `0x001e5100` | USB_IO_CON0; bit 11 forces SE0 |
| `0x00010800` | 16-byte MaskROM boot-state/token area in SRAM |

`nvram_set_boot_state(2)` arms USB loader entry; the app flushes that SRAM
region's data cache. The entry sequence stops the radio, disables interrupts,
clears USB control registers, forces SE0 for 1000 ms, releases it, waits
250 ms and for USB sense, rearms the token, then expires a 256 ms watchdog.
The resulting loader identifies as **UBOOT1.00**. The sequence is the current
working code, not a statement that every delay is the hardware minimum.
Entering with the Bluetooth controller still running hung the pad off USB in
a measured 2026-09-27 run. Stopping advertising alone is insufficient.

Sources: `src/main.c`, `src/init.c`, `src/usb.c`, `src/sdkconfig/board_config.h`.
