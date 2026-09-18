# IMU and rumble (after scheduling)

Make two small independent drivers, using verified register sequences from
`/work/ram_diagnostics/new_loader`. The working diagnostic identifies QMI8658A;
do not assume the earlier suggested LSM6DS3. Confirm pins and identity first.

IMU acquisition must have bounded I2C waits; use a separate low-priority sensor
task if SDK I2C blocks. Fixed small timestamped accel/gyro history, readiness
checks at configured sensor ODR, overrun/missing-device counters. Protocol code
consumes history; no I2C transactions in Bluetooth callbacks. Later add Switch
motion reporting with explicit unit/axis conversion.

Rumble uses a latest target (left, right, expiry). Driver owns PWM; profiles
decode output to motor targets. Use only the verified independent-channel
diagnostic implementation; earlier both-motors-at-full-strength code was faulty.
Start live tests with brief low intensity, then one motor at a time and zero.
Stop on disconnect, profile switch, expiry and sleep. Add console tests before
Nintendo rumble decoding. Document unsupported waveform details honestly.

Future macros reuse the synthetic-input composition point from stage 1. No
macro parser/program storage until latency, controls, and motors are measured.
