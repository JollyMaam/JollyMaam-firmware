# Host tools for repeated checks

Own `tools/c2ctl.c`, a small `tools/latency.py`, and `docs/latency.md`.
Do not edit firmware or Makefile. Coordinator adds build targets if needed.

Extend c2ctl to send a quoted console command (`"connect 0"`, etc.), explicitly
select a path, and discover `/dev/bwrap/ttyACM*` before normal `/dev/ttyACM*`.
Always configure raw mode to prevent tty echo loops. Bound total response time
even with continuous debug logs; handle unplug/hangup/partial writes properly.

Provide a small standard-library Linux Python tester using explicit CDC and
evdev paths. No D-Bus dependency, no sudo, no system configuration changes.
Discover input nodes if available, identify the requested device by name/unique
address rather than consuming arbitrary keyboard events. Use nonblocking I/O.
Record host CLOCK_MONOTONIC timestamps and evdev event timestamps in the same
clock when supported. Firmware command is `inject a 100 10000`, `inject stop`,
`inject status`; check press/release counts and spacing. Repeated per-command
USB-to-host measurement may be added later once firmware has a single-pulse
command and correlation. Do NOT label periodic edge jitter as end-to-end latency;
device and host clocks need correlation. Report missing/duplicate edges and
percentiles of interarrival timing, preserve raw CSV for later analysis.

No hardware access by this agent. Use synthetic event streams/CLI checks and
document exactly what remains to measure on the device. Keep the script modest.
