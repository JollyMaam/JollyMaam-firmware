# Runtime cleanup and deadline plan

Status: proposed; awaiting user sign-off. This document is a plan only. No
firmware behavior changes should be made until the first milestone is agreed.

The current runtime has one 5 ms `app_core` poll that performs USB command
handling, UART draining, GPIO/ADC sampling, debounce, Home/chord policy,
injection, rumble expiry, Bluetooth report submission, LED status updates, and
timeout handling. Bluetooth tasks remain above it and the IMU task remains
below it. The UART ISR only records DMA activity and wakes the poll. This is a
reasonable latency baseline; the cleanup should make ownership and timing
explicit without multiplying tasks.

## Goals

1. Make connect/disconnect/reconnect deterministic and diagnosable.
2. Give every periodic operation an owner, period, deadline, and policy for
   late execution.
3. Measure poll runtime, lateness, pending work, and useful idle slack before
   changing priorities or adding tasks.
4. Separate raw acquisition from stable input state and protocol policy.
5. Give LEDs a fixed-storage effect state machine that can skip animation
   frames safely when the system is busy.
6. Preserve recovery, pairing data, USB control, and the current report format.

## Non-goals for this cleanup

- No allocator, generic event bus, macro language, or large framework.
- No GPIO interrupt sampling until measurements show 5 ms polling is
  insufficient.
- No Bluetooth protocol changes as part of scheduler cleanup.
- No automatic sleep or low-power behavior changes based only on a new metric.
- No claim that a timer deadline equals end-to-end host latency.

## Priority model

The priority is an execution policy, not a FreeRTOS priority for every item.
Bluetooth interrupts/tasks retain precedence over application work. Within the
5 ms application poll, work is classified as follows:

| Class | Work | Late policy |
| --- | --- | --- |
| Must-run | lifecycle transitions, bounded USB command receive, GPIO/button sample, report deadline | run once; never catch up in a loop |
| Freshness-critical | UART DMA drain/parser, ADC snapshot, rumble expiry | run bounded work; count backlog/overrun; keep newest continuous data |
| Best-effort | one LED effect step, debug output, housekeeping/statistics | skip a frame/run and count it |
| Background | IMU initialization/retry and sensor read | own low-priority task; stale samples may be dropped |

The order is deliberately conservative: lifecycle and physical button edges
must not wait behind LED SPI or debug output. UART draining must be bounded so
it cannot starve button sampling, but overflow and parser resynchronization
must remain visible.

## Milestone 0: baseline and timing telemetry (no behavior change)

Before changing reconnect behavior, add a compact runtime snapshot exposed by
the existing CDC console. It should include:

- poll count, maximum/average observed poll runtime, late-poll count, and
  maximum lateness;
- time spent or approximate duration for USB polling, UART drain, input sample,
  report submission, LED update, and rumble update;
- UART DMA backlog/overflow/parser counters;
- report attempts, accepted, busy, deferred, and error counts;
- LED steps requested/applied/skipped;
- the next report deadline and the amount of positive schedule slack;
- radio state, lifecycle generation, current phase, last event, last HCI/HID
  reason, attempt number, and elapsed time.

Use `sys_timer_get_ms()` for the first version because it is already proven in
the application. If millisecond resolution cannot distinguish the expensive
sections, first identify a genuinely free microsecond counter or SDK timing
API; do not assume Timer3 is free or repurpose a peripheral during this work.
The telemetry command must snapshot counters without printing from an ISR and
must be cheap enough not to perturb the measurements.

Acceptance: a connected 30-second run and ten connect/disconnect cycles produce
usable numbers, with no firmware behavior change and no additional UART CRC or
DMA overflow errors.

## Milestone 1: radio lifecycle state machine (P0)

Move high-level radio ownership out of scattered conditionals into a small
fixed-state manager. Profile modules continue to perform SDK-specific protocol
work, but the manager owns requests, lifecycle generation, deadlines, and
terminal results.

Suggested states:

```
OFF -> STARTING -> WAITING_FOR_LINK -> WAITING_FOR_HID -> CONNECTED
 ^       |             |                  |                |
 |       +-------------+------------------+----------------+
 |                         failure/disconnect
 +------ STOPPING <-------- requested stop/profile change
```

Required rules:

- `connect N` is idempotent when slot N is already usable.
- Every start/stop request gets a monotonically changing generation. Delayed
  callbacks and timeout handlers from an older generation become no-ops.
- ACL connected is not reported as usable; HID channels/profile readiness are
  required.
- Stop waits for the profile disconnect/exit sequence or a bounded timeout;
  it cannot leave an old timeout capable of changing a new connection.
- A failed connection records the exact phase and reason and applies a small,
  bounded retry policy. USB-present mode remains awake for diagnosis; no
  unbounded retry storm and no flash erase are allowed.
- Profile switch, pairing, disconnect, sleep, and USB commands all enter this
  same request path. Bluetooth callbacks only publish events and perform the
  SDK-mandated immediate work; they do not start a competing lifecycle.

Keep the existing reset handoff as a recovery fallback until repeated tests
show that an in-place stop/start is equally safe. Do not remove it in the
first lifecycle change.

Acceptance: 20 `disconnect`/`connect 0` cycles with USB attached; no stale
state, no reboot, no lost CDC, no pairing loss, and a usable HID node after
each successful attempt. Also test a failed pairing, a cable removal, and a
profile change while stopping. The console must expose the phase and terminal
reason so a failure is actionable.

## Milestone 2: explicit input/acquisition state machines (P1)

Split `input.c` responsibilities into three small fixed-storage layers without
changing the public report format:

1. **UART stream:** `NO_DATA -> SYNCING -> TRACKING -> OVERFLOW_RECOVERY`.
   The ISR only advances DMA state/sets a flag. The poll drains a bounded
   amount, retains a sliding parser window, timestamps each accepted axis, and
   discards to a valid frame after overflow.
2. **Button sampler/debouncer:** raw active-low sample, per-button candidate
   value/deadline, stable value, and changed mask. The Home/chord policy reads
   stable state and remains separate from electrical sampling.
3. **Analog snapshot:** ADC and trigger-switch values carry a freshness
   timestamp. Cached ADC values are never presented as a new conversion merely
   because they were read again.

Start with the existing 5 ms poll. A 1 ms sampler or GPIO interrupt is a
   measured follow-up only if the telemetry shows missed button deadlines or
   unacceptable physical-to-report latency. Continuous axes remain latest-state
   data; button transitions and protocol replies remain bounded events.

Acceptance: 60-second UART run with zero unexplained CRC/dropped-byte/overflow
  increments; button bounce tests show independent debounce; simultaneous
  buttons do not delay one another; trigger microswitch overrides remain
  correct; report counters remain unchanged from the baseline.

## Milestone 3: deadline-aware application poll (P1)

Introduce a small internal work table, not a general scheduler. Each item has
`period`, `next_due`, `class`, and counters for runs, skips, late starts, and
overruns. The 5 ms poll runs due work once and advances deadlines from the
intended schedule, never by replaying missed periods.

The initial table should cover only existing work:

- USB command receive;
- UART drain;
- input sample/debounce and Home policy;
- report submission;
- rumble expiry;
- one LED state-machine step;
- debug/statistics flush.

Use a conservative application budget measured in Milestone 0. If the budget
is exhausted, only best-effort work may be skipped. A skipped LED frame is
safe; a skipped button sample or UART drain is not. Report submission checks
transport capacity and never queues an unbounded backlog. Expose all skips and
deadline misses in `runtime status`.

Do not create a second controller task yet. The existing task priorities and
the proven lossless UART/IMU measurements are the simpler design. Reconsider a
separate controller task only after a reproducible starvation or latency
failure.

## Milestone 4: LED effect state machine (P2)

Make `led.c` the sole owner of LED output. A fixed array of effect steps holds
duration, RGB/mask, repeat, and priority. A request replaces or layers an
effect according to priority; the status effect, host player lights, pairing,
and diagnostics no longer call SPI directly from unrelated modules.

The effect runner is best-effort. If it runs late, it advances to the step
that should be active now instead of emitting every missed frame. DMA buffer
ownership remains inside `led.c`; unchanged frames are not resent. Disconnect,
sleep, and profile changes submit explicit stop/indicator effects.

Acceptance: existing solid/blink indicators are visually unchanged; a test
effect with several steps survives deliberately delayed polls without blocking
USB, input, or Bluetooth; no LED SPI call occurs from an ISR or Bluetooth
callback.

## Milestone 5: idle accounting and low-power review (P2)

Use the telemetry to report two different concepts clearly:

- **application slack:** time before the next hard deadline, and
- **system idle eligibility:** whether no application work is pending and no
  transport/IMU/lifecycle operation requires service.

Initially keep the low-power target conservative (`is_idle` may remain false)
and only report eligibility. After the report is stable, validate any change
to `cyclone2_idle_query()` with USB connected, Bluetooth connected, UART
streaming, IMU enabled, and Home wake tests. Never infer actual CPU idle time
from a missed poll; use the SDK idle hook/counter only after confirming its
semantics for this BR23 build.

## Milestone 6: later extensions

- IMU data-ready/ODR tuning only after the runtime budget is visible.
- Rumble envelopes remain latest-target plus expiry; complex waveforms are
  optional best-effort effects.
- Macro injection reuses the existing semantic input-composition point. It
  must not run in an ISR, steal physical button ownership, or create an
  unbounded event queue.
- End-to-end latency work adds a single correlated pulse/sequence test after
  the lifecycle and scheduler counters are trustworthy.

## Sign-off gates

The next implementation turn should begin only after agreeing to:

1. Milestone 0 first, with no functional scheduling changes.
2. Reconnect state-machine work as the first behavior change.
3. Keeping the current task layout unless measurements prove it inadequate.
4. Treating LED animation as lower priority than input/UART/radio work.
5. Measuring idle slack before enabling any new low-power behavior.
