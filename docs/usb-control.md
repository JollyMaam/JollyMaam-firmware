# USB configuration console

The firmware exposes a USB CDC ACM serial port for configuration and recovery.
CDC ACM uses operating-system serial drivers and can be used directly from a
terminal on Linux, macOS, and Windows; no controller-specific GUI or driver is
required. The selected baud rate is ignored because USB CDC is not a physical
UART.

On Linux the device normally appears as `/dev/ttyACM0`. For an interactive
session, use any serial terminal, for example:

```sh
picocom -b 115200 /dev/ttyACM0
```

The console has conventional character echo, backspace handling, CR/LF line
input, and a `c2> ` prompt. Its initial commands are:

```text
help     show the command list
debug on|off|status
         control runtime Bluetooth diagnostics
report 5|8|10|status
         select or inspect the saved HID report period and counters
connect 0|1|2|3
         reconnect the selected slot using its saved bond (never pairs)
connect status
         show slot, profile, connection state, derived local address, and USB
disconnect
         stop Bluetooth without sleeping or deleting the saved bond
input status
         show stick-DMA framing/drop counters and the last axes timestamp
inject a PERIOD_MS DURATION_MS
         repeat synthetic A for the requested duration (even period 20..1000 ms,
         duration 1..60000 ms; requires a usable HID connection)
inject stop|status
         stop injection or show edge/overrun counters
status   show the reset mailbox and MaskROM NVRAM
uboot    reboot into BR23 UBOOT1.00
```

For example, `inject a 100 10000` holds synthetic A for the first 50 ms of
each 100-ms cycle and releases it for the next 50 ms. The schedule uses device
millisecond deadlines; `edges` counts only transitions accepted by the profile,
while `overruns` counts scheduled transitions skipped when the control loop was
late. Neither counter proves delivery to the host. Injection is stopped on
disconnect and profile changes. `inject stop`
schedules a final synthetic release while preserving any physical A hold. Its
schedule is not a one-way latency measurement: device and host clocks are unsynchronized,
and host receipt time must be measured separately.

Runtime diagnostics default to enabled and report Bluetooth status/HCI events
and EDR HID-channel state. The locally compiled SDK EDR, EDR HID, and BLE
module messages are routed into the same stream. Messages are buffered until
CDC is ready, so early events following a profile-selection reset remain
visible. Each line is timestamped with milliseconds since boot. Debug output
does not emit another command prompt after every line. Automatic Bluetooth
timeouts and disconnects do not enter soft
poweroff while USB is connected; this keeps the console and recovery command
available. Explicit Home holds and delayed poweroff also check cable presence;
sleep is suppressed while USB is attached.

`make build/c2ctl` builds the noninteractive Linux client. It searches
`/dev/bwrap/ttyACM*` before `/dev/ttyACM*`, or accepts an explicit path:

```sh
build/c2ctl status
build/c2ctl -p /dev/ttyACM1 uboot
build/c2ctl -p /dev/bwrap/ttyACM0 'connect 0'
```

## Recovery sequence

The build makes a one-line correction to a generated copy of SDK `cdc.c`:
`SET_CONTROL_LINE_STATE` no longer reinitializes bulk endpoints. The SDK reset
handler already configures them. Resetting their DATA toggles on each tty open
caused alternating commands to disappear; repeated opens worked after this
correction. The SDK source checkout is unchanged.

The `uboot` command first writes MaskROM USB boot state 2 and reports a
16-byte snapshot of the NVRAM block at `0x10800`. Bytes 4 through 11 should be
the token `75 73 62 ed 63 81 67 ef`.

After allowing the response to reach the host, the firmware forces a
host-visible USB detach, rewrites the token, and deliberately expires the
watchdog. The watchdog reset is important: BR23 software-reset paths tested
during development either lost the NVRAM token or returned to the application,
while watchdog reset reliably entered UBOOT1.00. If VBUS disappears during the
detach interval, the firmware waits instead of resetting into an unavailable
USB loader.
