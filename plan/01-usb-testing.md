# USB-controlled test baseline

Own `src/controller.c`, `src/controller.h`, `src/usb.c`, and
`docs/usb-control.md`. Read existing code before editing; preserve recovery.

Implement `connect 0|1|2|3` through the existing profile selection/reset path.
This explicitly requests reconnect with the saved bond, never re-pairing or
forgetting keys. `connect status` prints slot, profile type, connection state,
local address, and whether USB is present. Reject invalid input. Ensure command
acknowledgement is queued before reset. A request for an already connected
slot can be a no-op. An unconnected active slot may use the existing reset path.

Guard every sleep path while USB is physically present, including explicit
Home hold and the final delayed poweroff callback. Use the proven PC3 sense
when appropriate; USB may be attached before enumeration completes.

Add a tiny deterministic injection facility, not a macro engine:
`inject a PERIOD_MS DURATION_MS`, `inject stop`, `inject status`. Default example
`inject a 100 10000`: physical A is held for the first half of each 100-ms cycle
and released for the second half, for 10 seconds. Validate bounded duration and
period (at least 20 ms); do not sleep or spin in a callback. Compose a synthetic
A bit with the physical report after Home/chord processing, so synthetic input
cannot trigger chords and releasing it cannot release a physical hold. Stop on
disconnect/profile change. Use absolute deadlines and count missed transitions;
do not pretend busy/rejected reports were delivered. Status exposes emitted
edges and overruns. Keep this in controller.c unless a separate file clearly
reduces complexity. Document device time versus host time: a repeating schedule
alone does not measure true one-way latency across unsynchronized clocks.

Build affected objects. Provide exact console grammar and completion notes to
coordinator. Do not change input.c or profile modules in this first stage.
