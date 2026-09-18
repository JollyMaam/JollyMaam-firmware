# Controller implementation plan

Source: `/work/PLANNED_ARCHITECTURE.md`. Keep this small: fixed buffers, explicit
ownership, existing SDK mechanisms, and one hardware-verified change at a time.
Implementation agents use `gpt-5.6-luna`, medium reasoning. The coordinating
agent owns integration, builds, flashing, and this progress record.

## Order and progress

| Step | Brief | State |
| --- | --- | --- |
| 1 | [USB testing access](01-usb-testing.md) | assigned |
| 2 | [Nonblocking acquisition](02-acquisition.md) | assigned; integrate after 1 |
| 3 | [Host test tools](03-host-tests.md) | assigned |
| 4 | [Controller scheduling and TX](04-controller-tx.md) | pending hardware baseline |
| 5 | [IMU and rumble](05-imu-rumble.md) | pending stable scheduling |

The starting controller is paired in Switch slot A (slot 0), address
`13:11:DB:58:28:18`. Preserve its syscfg/VM/BTIF and address derivation. Never
erase the whole chip during iteration. Existing UBOOT recovery is proven;
do not refactor that sequence as part of this work.

## Integration checks

1. Build; retain the previous known-working image outside the build output.
2. Query CDC `status`/`help`; request UBOOT; write only the packed image's code
   span, keeping reserved flash regions untouched; read back and compare.
3. Reset; wait for CDC; request `connect 0`; require an actual usable HID
   connection, not just an ACL connection. Check reports on the host input node.
4. Exercise repeated injected presses/releases, check missing/extra edges and
   latency. Repeat recovery after each meaningful integration milestone.
5. Record exact image hash, commands, outcomes, and unavailable hardware here.

Only the coordinator accesses hardware. Agents may build individual objects;
coordinate full builds to avoid racing generated files. Each brief defines file
ownership. No commits from agents; coordinator reviews and commits together.

## Design limits

Keep lifecycle/USB/flash writes in app_core. A future controller task owns
debounce, synthetic-input composition and report scheduling. Interrupts only
capture data and signal work. Do not call Bluetooth or log from an interrupt.
Use bounded queues only where events must be retained: UART bytes, button
transitions, protocol replies, and IMU history. Continuous axes use latest state.
Do not add a generic event bus, allocator, plugin system, or macro language.

## Validation log

- Initial checkout: `5abe596`, clean. At initial discovery only
  `/dev/bwrap/ttyACM0` exists; loader/input nodes may appear on enumeration.
  Host D-Bus is not exposed. Firmware-initiated reconnect avoids requiring it.
