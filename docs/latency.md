# Host-side injection checks

`tools/c2ctl.c` is a small Linux CDC client. It opens an explicitly supplied
path with `-p/--path`, or discovers `/dev/bwrap/ttyACM*` before
`/dev/ttyACM*`. It puts the tty in raw mode, asserts DTR/RTS when supported,
handles partial nonblocking writes, and bounds the complete exchange to three
seconds. A quiet 250 ms ends a successful response, even when debug output is
continuous.

Build and use it with a quoted command when the command has arguments:

```sh
make build/c2ctl
build/c2ctl status
build/c2ctl --path /dev/ttyACM0 "connect 0"
build/c2ctl -p /dev/ttyACM0 "inject status"
```

`tools/latency.py` requires an explicit CDC path. Give it an explicit evdev
node, or give an exact device name/unique address so it can safely discover a
matching `/dev/input/event*`; it never reads arbitrary keyboard nodes:

```sh
python3 tools/latency.py --cdc /dev/ttyACM0 --event /dev/input/event5 \
  --count 100 --csv run-001.csv
python3 tools/latency.py --cdc /dev/ttyACM0 \
  --device-name 'Gamesir Cyclone 2' --csv run-002.csv
```

The tester sends `inject status`, a validated `inject a PERIOD_MS DURATION_MS`
(defaults to `inject a 100 10000`, 100 cycles), and finally `inject stop`. It captures the selected key's press/release edges with
nonblocking I/O, preserves host-read and evdev timestamps in the CSV, and
prints missing, duplicate, and p50/p95/p99 interarrival statistics. The
default key code is `304` (`BTN_SOUTH`); select another code with `--code`.

The current Switch profile maps injected physical A to code **305**
(`BTN_EAST`). Tested command in this container:

```sh
/work/bin/python tools/latency.py --cdc /dev/bwrap/ttyACM0 \
  --event /dev/bwrap/event22 --code 305 --count 100 --period 100 \
  --csv /tmp/edges.csv
```

Event numbers may change after reconnect; use `--device-name 'Pro Controller'`
to discover the gamepad (as distinct from its IMU input node).
The period is validated as an even value from 20 through 1000 ms. A run is
failed if the firmware does not answer `OK injection started`, reports `ERR`,
the event stream reports `SYN_DROPPED`, or the expected press/release counts
do not match.

The tester requests `CLOCK_MONOTONIC` timestamps from evdev and labels a
kernel-clock fallback when that request is unavailable. These results are periodic edge
spacing/jitter only, not end-to-end USB latency: the firmware injection clock
and the host clock are not correlated. A future single-pulse command with a
correlation marker is needed for that measurement. Hardware results also
are recorded in `plan/README.md` after live integration checks.

Offline checks require no device:

```sh
cc -std=c11 -Wall -Wextra -Werror tools/c2ctl.c -o /tmp/c2ctl
python3 -m py_compile tools/latency.py
python3 tools/latency.py --help
python3 tools/latency.py --dry-run --count 100 --period 100
```
