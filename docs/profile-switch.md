# Profile switches without a chip reset

## Symptom

Every profile change reset the chip.  `connect N`, `pair N`, `unkey`, `peer`,
and the Home+face chord all went through `profile_select()`, which wrote a
request into the reset mailbox and called `p33_soft_reset()` about 100 ms later.

That reset is what made the pad look like it crashed: the USB console dropped
and re-enumerated, every Bluetooth session restarted from scratch, and a reset
timed badly (during SSP, or while the controller library was mid-command) could
leave the radio wedged.

## Why the reset was there

The BTstack port was written with a task that parks between radio sessions
(`btstack_jl_core_run()`), but nothing ever started a second session in place:
`btstack_run_loop_init()` asserts that no run loop is installed, the transport
only linked its RX data source once, and the shutdown request triggered the run
loop exit before the controller had been powered down.  The reset sidestepped
all three by starting the firmware over.

## What a profile switch does now

`profile_select()` stops the active radio session and starts the new one on the
same firmware image:

1. `profile_request_transition()` marks the radio as stopping, asks the profile
   for a protocol-level disconnect, and schedules `profile_transition_now()`
   `CYCLONE2_PROFILE_SWITCH_MS` (100 ms) later.
2. `profile_transition_now()` calls `cyclone2_profile_exit()` and then
   `start_selected_profile()` for the queued slot/type.  No `p33_soft_reset()`.
3. `cyclone2_profile_exit()` reaches `btstack_edr_exit()`, which asks the
   run-loop task to power the controller down; the task stops the run loop once
   it sees `HCI_STATE_OFF` and then tears BTstack down.
4. The next start re-initialises BTstack and powers the controller back up on
   the same task.

To make that safe, three things changed in the stack layer:

* `btstack_jl_core_teardown()` deinitialises HID, SDP, L2CAP, HCI and the run
  loop between sessions.  Upstream requires this before a new init, and
  `btstack_run_loop_init()` asserts without it.
* `btstack_jl_core_request_shutdown()` no longer triggers the run loop exit
  itself.  It only wakes the loop; the exit happens from the `HCI_STATE_OFF`
  event so the halt (and the transport close) finishes first.  A shutdown for
  an already-parked session is a no-op, because the run loop is gone by then.
* The BR23 HCI transport re-links its RX data source on every session and
  ignores packets once the controller is closed.

The run-loop identity is tracked with `jl_run_loop_up`, and the link-key
commands are ignored while the stack is parked, so a console command issued
between sessions cannot touch freed state.

## Still a reset: `clearkeys`

`clearkeys` is a factory reset of the Bluetooth controller's own pairing
records: it zeroes the bond store in retention memory at 0x10700 and restarts
so the controller library re-reads it.  It is not a profile switch and still
calls `p33_soft_reset()`.  It is the only remaining soft reset in the firmware.

## Verification

`make test` runs the whole thing against the pad that is plugged in (see
`tools/stress.py` for what it covers).  With the pad on USB, no console is
needed: the test drives switches, the pairing/disconnect/connecting cycle, a
33 s dwell past the connect timeout and rapid bursts, and fails if the console
stops answering or if `status`'s preserved boot counter (`boots=`) moves.

The boot counter is the real check.  A chip reset increments it; a profile
switch must not.  The console greeting is not used, because the firmware
re-sends it whenever the host's CDC line state flaps, which happens for reasons
unrelated to the pad.

A live-console pairing and reconnect still needs a real Switch.
