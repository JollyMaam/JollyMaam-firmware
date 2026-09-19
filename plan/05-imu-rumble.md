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

## Current implementation and validation

The console baseline is implemented in `src/imu.c` and `src/rumble.c`.
`rumble LEFT RIGHT MS` drives the verified PA9/PB0 MCPWM channels independently
at 25 kHz, has a two-second maximum duration, and is stopped on timeout,
disconnect, profile reset, and poweroff. Zero/zero is a valid explicit stop.
Physical left-only/right-only confirmation is still required.

Switch output reports are decoded. Nintendo's two carrier-frequency fields are
intentionally ignored because the board has ordinary ERM motors; the independent
amplitudes are mapped to PWM duty. Switch targets latch until the host sends a
zero target, matching hid-nintendo behavior, while console test pulses retain
their mandatory timeout. PWM changes occur only from `app_core`; the Bluetooth
callback publishes a latest target. Linux `fftest` produced `left=140,right=0`
for its strong effect and `left=0,right=188` for its weak effect, then an
explicit zero when stopped. The associated report run accepted 3,461 of 3,461
Bluetooth submissions without busy/deferred/error results.

The QMI8658A is present at write address `0xd4`, reports ID `0x05`, and accepts
the configuration recovered from the stock firmware. `imu init` enables an
asynchronous acquisition task; `imu sample` only copies the latest completed
sample and never performs I2C in `app_core`. The fixed history contains eight
samples. `app_core` now runs at priority 2 and the IMU task at priority 1, below
Bluetooth priorities 3 and 4, so UART parsing and report scheduling preempt a
sensor transaction. The BR23 hardware-I2C START needs no software delay; only
the QMI soft reset has an explicit 20 ms wait.

Live hardware results with the saved Switch bond connected:

- continuous acquisition reaches approximately 95--100 samples/second;
- ID, accel, and gyro values are coherent, with no I2C errors observed;
- a measured window produced over 150,000 valid UART frames with zero dropped
  bytes, CRC errors, or ring overflows (maximum backlog 224 bytes);
- 4,169 of 4,169 Bluetooth submissions were accepted at the 5 ms report
  setting, with no busy, deferred, or error results.

The current task reads on each 10 ms OS tick and does not yet check the
QMI8658A data-ready register. Add that check and protocol-specific unit/axis
conversion when motion samples are first placed in a HID report.
