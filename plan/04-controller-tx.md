# Controller task and transmit pacing (after baseline)

Status: complete. Live testing did not justify creating a separate controller
task: steady acquisition stays within its DMA ring and every attempted Classic
HID report was accepted. Radio startup/teardown can discard old continuous
axis samples, which is explicitly allowed by the latest-state model. Revisit a
priority-2 task only if later IMU work or measured physical-input latency shows
steady scheduling starvation.

Start only when CDC recovery, reconnect and injection have passed on hardware.
Read current ownership after stages 1/2; coordinate all controller/profile edits.

Use one controller task below Bluetooth priorities and above app_core only if
SDK task signaling is verified interrupt-safe. A 1-ms timer samples GPIO or
signals bounded work; no blocking peripheral, USB, flash or BT calls in ISR.
Keep lifecycle operations in app_core and serialize profile teardown against
report submission. Avoid introducing multiple owners of SDK profile state.

Debounce buttons independently. Preserve Home/chord policy and pending short
Home taps until a report is actually accepted. Inputs retain source timestamps;
ADC getter values are cached samples, not necessarily fresh conversions.

Use Classic send-ready/busy APIs and BLE CAN_SEND_NOW for retry wakeups. Verify
callback context and existing hooks before installing callbacks. Readiness is
capacity, not host acknowledgement. Keep pacing configurable at 5/8/10 ms with
absolute deadlines; target 5 ms first. Continuous state can replace pending
state, but retain bounded button transitions and protocol replies separately.
Count overflow and converge back to current real button state. Never send an
unbounded backlog. Connection generation discards obsolete asynchronous work.

Compare injection edge counts and timing before/after; repeatedly verify
reconnect and UBOOT. Do not claim 1-ms end-to-end latency from a 1-ms timer.
