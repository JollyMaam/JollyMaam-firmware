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

