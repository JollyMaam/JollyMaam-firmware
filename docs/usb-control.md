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

The console has echo, a `c2> ` prompt, line editing (Backspace, Ctrl-U
clears the line, Ctrl-W a word, Ctrl-C abandons it, Up/Down for history) and
Tab completion - commands, setting paths, and after `=` the choices; a second
Tab lists the candidates.  Log lines never land in the middle of a line being
typed: the line is erased, the log printed, and the prompt and what was
typed redrawn.  `help` lists the commands; `help COMMAND`, `COMMAND -h` and
`COMMAND --help` show one command's usage.

The console has namespaces, and everything follows the same pattern: a
namespace alone, or with `help`, `-h` or `--help`, explains itself and lists
its subcommands; a command with `help` shows its usage; `help WORDS` is
`WORDS help`.

    config                  settings: config.PATH, config.PATH=VALUE, and
                            config list|export|import|reset|commit|pending
    status                  state, read-only: status.PATH, status list
    debug                   diagnostics and tests: debug log|hcidump|stat|
                            sensors|wake|inject|rumble|imu|input|bleadv|bringup|
                            bondstore|clearkeys|nvscan|delall|peek|poke|complete
    connect, disconnect, pair, cal, reboot, uboot, help

`debug.imu` works as another spelling of `debug imu`.  Settings are
`config.PATH=VALUE` (`docs/config.md`), so the old commands map as: `report
5` is `config.report_ms=5`, `profile 1 xbox` is `config.profile.B.type=xbox`,
`connect status` is `status.radio`, `report status` is `status.report`,
`input status` is `status.input`, `keys` is `config export`, `setaddr`/
`restore`/`bond`/`setkey` are `config import` (or single
`config.profile.A.bond.*` lines), `newaddr` and `unkey` are `pair A
--fresh`, `store ...` is `config.autocommit`/`config commit`, `factory-reset`
is `config reset`, `unset PATH` is `config reset PATH`, the old `status` dump
is `debug bringup`, `stat` is `debug stat`, and the battery reading is
`status.battery`.

## Machine mode

A program talking to the console sends STX (0x02) to switch it into machine
mode and SOH (0x01) to switch back; ENQ (0x05) is answered with ACK (0x06) in
either mode, from the application poll, as a liveness ping.  In machine mode
there is no echo and no prompt, commands may carry an id, and every line that
comes back is tagged:

```text
@7 config.report_ms = 5         in
=7 OK config.report_ms = 5 ...  the command's output
.7 OK                           done (or ".7 ERR message")
! [12.345] start profile=0 ...  a log line
```

`tools/padbus.py` keeps the port in machine mode, routes each command's
output back to whoever sent it, and offers `padbus.py cmd CLIENT LINE`, which
prints exactly that command's output and exits 1 on `ERR`
(`docs/session-bus.md`).  Opening the port afresh (DTR rising) puts the
console back in human mode.

For example, `debug inject a 100 10000` holds synthetic A for the first 50 ms of
each 100-ms cycle and releases it for the next 50 ms. The schedule uses device
millisecond deadlines; `edges` counts only transitions accepted by the profile,
while `overruns` counts scheduled transitions skipped when the control loop was
late. Neither counter proves delivery to the host. Injection is stopped on
disconnect and profile changes. `debug inject stop`
schedules a final synthetic release while preserving any physical A hold. Its
schedule is not a one-way latency measurement: device and host clocks are unsynchronized,
and host receipt time must be measured separately.

Runtime diagnostics default to enabled and report Bluetooth status/HCI events
and EDR HID-channel state. The locally compiled SDK EDR, EDR HID, and BLE
module messages are routed into the same stream. Messages are buffered until
CDC is ready, so early events following a profile-selection reset remain
visible. Each line is timestamped with milliseconds since boot. Debug output
is printed around the line being typed, as above. Automatic Bluetooth
timeouts and disconnects do not enter soft
poweroff while USB is connected; this keeps the console and recovery command
available. A five-second Home hold still disconnects Bluetooth, but suppresses
the final sleep while USB is attached so the CDC console remains available.
Repeated connect/profile commands are rejected while teardown or reset is
already in progress.

`status.report` distinguishes reports accepted by the SDK from temporary
queue-full (`busy`), intentionally deferred (`deferred`, such as an unchanged
BLE report), and unexpected profile/stack errors. Attempts are counted only
while the profile is in the usable connected state; `polls` continues to show
the application control-loop count.

`make build/c2ctl` builds the noninteractive Linux client. It searches
`/dev/bwrap/ttyACM*` before `/dev/ttyACM*`, or accepts an explicit path:

```sh
build/c2ctl stat
build/c2ctl -p /dev/ttyACM1 uboot
build/c2ctl -p /dev/bwrap/ttyACM0 'connect 0'
```

## Recovery sequence

`make flash` (or `tools/flash.sh`) drives the whole write path: it asks the
running firmware to reboot into UBOOT1.00, reads the current 512 KiB flash to
`/work/cyclone2-flash-backups/` first, writes the image at offset 0, reads the
written span back and compares it, then resets and waits for this console to
reappear. The write covers only the code span, so the VM, BTIF and key/mac
regions above the code limit (`CODE_LIMIT`, `0x50000`) keep their contents.

If the controller does not answer and no loader device is present, the script
waits: unplug the controller and plug it back in, or power-cycle it, so the
chip's ROM loader appears on USB. `--no-backup`, `--image`, `--wait` and
`--loader` adjust the behaviour.

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
