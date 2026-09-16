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
status   show the reset mailbox and MaskROM NVRAM
uboot    reboot into BR23 UBOOT1.00
```

Runtime diagnostics default to enabled and report Bluetooth status/HCI events
and EDR HID-channel state. The locally compiled SDK EDR, EDR HID, and BLE
module messages are routed into the same stream. Messages are buffered until
CDC is ready, so early events following a profile-selection reset remain
visible. Each line is timestamped with milliseconds since boot. Debug output
does not emit another command prompt after every line. Automatic Bluetooth
timeouts and disconnects do not enter soft
poweroff while USB is connected; this keeps the console and recovery command
available. An explicit long Home button hold can still request sleep.

`out/c2ctl` is an optional noninteractive Linux client. It finds the first
`/dev/ttyACM*` device, or accepts an explicit path:

```sh
out/c2ctl status
out/c2ctl uboot /dev/ttyACM1
```

## Recovery sequence

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
