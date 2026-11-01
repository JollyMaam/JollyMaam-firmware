# Runtime memory and storage

What this firmware keeps across resets and in flash, and its power policy. The
chip's memory map and VM format are in [../jieli/memory-map.md](../jieli/memory-map.md)
and [../jieli/flash.md](../jieli/flash.md).

## Retained SRAM sections

| Address | Size | Use |
|---|---|---|
| `0x20000` | 16 bytes | reset mailbox (`.reset_mailbox`): the app's request to the updater ([updater.md](updater.md)) |
| `0x20010` | `0x2100` bytes maximum | bring-up log and boot counter (`.retained`) |

The main heap is capped below `0x20000`. The linker asserts that the
Bluetooth controller's RAM does not cover the MaskROM boot-state token.
Bonds are stored in flash VM, not in retained SRAM.

## VM items

| ID | Length | Owner |
|---|---|---|
| 1-28 | up to 224 | settings records (`src/core/settings.c`) |

The SDK's own items (DAC trim, RF trim, oscillator offset) are listed in
[../jieli/flash.md](../jieli/flash.md#sdk-items).

## Charger

This firmware clears `CHARGE_EN` and `CHGBG_EN` and does not initialize the SDK
charge driver. Under this firmware the internal charger is therefore off.

## Sleep policy

The timeouts are this firmware's, not hardware's:

- No host frames for 250 ms means no host.
- While idle, or after a Bluetooth disconnect, the pad sleeps after 15 seconds
  without host frames. It confirms the absence over four checks before power-off.
- After USB goes away, it sleeps after 5 seconds.

## Boot counter and restarts

The app keeps a boot counter in retained SRAM. A restart is established when
the counter increases and uptime restarts together. USB re-enumeration or loss
of console output alone does not prove a restart.


## Ultimate 2C Wireless power policy

The default battery-idle action releases the board's power hold. No last-slot
record is read or written, either in flash or analog retention registers.
A Bluetooth boot with Home alone stays idle until Home plus a slot button
selects a slot. Its normal release connects; holding the chord performs the
usual pairing gesture. Once selected, the current RAM remembers the slot for
Home-only reconnects until power is lost. The rear switch's 2.4 GHz mode still
connects its receiver with Home and has no Bluetooth slot to select.
USB power prevents the ordinary inactivity power-off.

The sleep method and last-slot storage are independent build choices:

| Option | Choices | Default |
|---|---|---|
| `U2CW_SLEEP` | `poweroff`, `deep` | `poweroff` |
| `U2CW_LAST_SLOT` | `none`, `analog`, `flash` | `none` |

`deep` keeps the board's power hold and enters GPIO-woken deep sleep without
SRAM retention. Home low or USB power high wakes it. The motor/light outputs
are turned off, the ADC and USB pull-up are disabled, and the watchdog is
stopped before entry. A returned sleep call falls back to releasing the hold.
Probe and profiler builds force power-off even when `deep` is selected.

`analog` retains the slot in registers 0x38/0x39 with a validity check. It
survives deep sleep while the board remains powered; it does not survive a
power cut. `flash` uses settings-log key 63 and the legacy four-byte last-slot
record; unchanged selections are not rewritten. Ephemeral operation does not
write either form of last-slot storage. Ordinary settings and bonds continue
to use their existing flash records in all configurations.

The default does not read or erase an old last-slot flash record. Enabling
`flash` can reuse such a record. With either retention option, a missing or
disabled retained slot follows the existing fallback to a configured slot.
Whole-board sleep current and the cause of excessive discharge with the power
hold retained are not established. Chip deep-sleep specifications alone do
not establish the battery lifetime of the assembled pad.
