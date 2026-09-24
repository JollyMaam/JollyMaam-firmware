# The shared pad session (`tools/padbus.py` and `tools/pad_tui.py`)

The pad's USB console is a single terminal.  Two programs reading it split each
other's output, and a command from one lands in the middle of the other's
trace, so a session driven from both sides is hard to follow - and dumping
`status` (which prints the whole 2000-entry bring-up ring, about 8 KB) makes it
worse.

So one process owns the port and everyone else talks to it:

    tools/padbus.py serve -p /dev/bwrap/ttyjl0     # the daemon, once
    tools/pad_tui.py                               # interactive view for a person

Starting `serve` when one is already running does not create a second owner:
the new daemon connects, sends the incumbent a `{"op": "quit"}` request, and
waits (up to five seconds) for the socket to stop answering before it binds.
The incumbent logs `# shutting down`, closes its clients and listener, and
unlinks the socket.  `tools/padbus.py quit` does the same to a running daemon
by hand.  A daemon old enough not to know `quit` is left alone and the new one
reports that it is "still answering ... after being asked to quit".

The TUI keeps two lines at the bottom for itself - a status line (who holds
control, which filters are on, the last notice) and a command line - and never
writes log output over them.  Function keys drive it:

The prompt is always on the bottom line, the status line above it carries the
control state, the active filters and the key hints, and log output is never
written over either.

| key | effect |
|---|---|
| F1 | take control, or give it back - a toggle |
| F2 / F3 / F4 | fill in `/filter`, `/hide`, `/clear` |
| PgUp/PgDn | scroll the log |

Typing goes to the prompt and it edits like readline: Ctrl-A/Ctrl-E,
Ctrl-B/Ctrl-F by character, Alt-B/Alt-F by word, Ctrl-U/Ctrl-K/Ctrl-W to
delete, Ctrl-R to search the history, Home/End, and Up/Down for history.
Python's `readline` module cannot be pointed at a curses window (it writes to
the process's stdout, which curses owns), so those bindings are implemented
here rather than borrowed - the behaviour is the same, the plumbing is not.

The history search behaves like a shell's: typing narrows to the most recent
match, a motion key (an arrow, Home/End, Ctrl-A/E, Alt-B/F) ends the search and
leaves the line there to edit, and Enter runs the match as it stands.  Ctrl-C
clears the line and never leaves the TUI.

A line starting with `/` is a TUI command rather than a pad command:

| TUI command | effect |
|---|---|
| `/help` | print the TUI command list into the log |
| `/control` | take control, or give it back |
| `/filter REGEX` / `/hide REGEX` | show only / hide matching lines (empty clears) |
| `/clear` | clear both filters |
| `/save [FILE]` | write the current view to a file |
| `/quit`, `/exit` | leave |

Anything else is sent to the pad (`help` lists the pad's commands; `stat` is
the one-line status).

    tools/padbus.py request codex                  # take control
    tools/padbus.py send codex stat                # only the holder may send
    tools/padbus.py release codex
    tools/padbus.py tail -n 40                     # read the shared log
    tools/padbus.py watch                          # stream it

Everything either side does is appended to one log (`/work/nslog/padbus.log`)
and pushed to every attached client, so both windows show the same narrative:
commands as `>>> [who] command`, the pad's output as it arrives.  Control is
exclusive - `send` from anyone else is refused with who holds it - which is
what keeps the trace readable.

## The protocol

Newline-delimited JSON over an AF_UNIX socket (default `/tmp/padbus.sock`).
Requests carry `op` and get exactly one reply; events are pushed as
`{"ev": ...}`.  A client must ignore `ev` messages while waiting for its reply,
because both share the connection.

| request | effect |
|---|---|
| `{"op":"hello","client":C}` | reply carries the owner and the last 200 log lines |
| `{"op":"request","client":C}` | take control if free (or already C's) |
| `{"op":"release","client":C}` | drop control if C holds it |
| `{"op":"send","client":C,"line":L}` | send L to the pad; refused unless C holds control |
| `{"op":"tail","n":N}` | reply carries the last N log lines |

Events: `{"ev":"log","line":...}` and `{"ev":"owner","owner":...}`.

## Cheaper polling

`stat` is one short line for hosts that poll:

    stat up=178380 boots=5962ee28 state=0 addr=fa093af3eded reset=0 p33=64

Use it instead of `status` unless the bring-up ring is what you are after.
`p33` is the reset-cause register, so a session tool sees a reset immediately
as `up` going backwards and `boots` changing.

## Spotting a reboot without polling for it

The daemon watches for resets two ways, and writes what it finds into the
shared log so both sides see it at the same moment:

* **The console disappearing.**  A chip reset re-enumerates the USB device, so
  the daemon's read fails and it says so: `# console lost - the CDC device
  re-enumerated`.  This is the fast signal - it lands within a couple of hundred
  milliseconds, long before anyone would think to ask the pad anything.
* **The boot counter.**  On the way back (and every few seconds when the console
  is otherwise quiet) it asks for `stat` and compares `boots=`.  A change is a
  reset and is announced as such; no change is announced as
  `# same boot counter ... - the re-enumeration was not a chip reset`.

That second part matters: measuring it showed the pad's CDC device re-enumerating
while the chip kept running (uptime continued straight through), so "the console
went away" on its own is *not* proof of a reboot.  The boot counter is the
authority.

The heartbeat is idle-only.  If anything else has talked to the pad in the last
few seconds it stays quiet, and its own echo and response are swallowed, so a
quiet console produces no log traffic at all - only an announcement when
something actually changed.  `serve --heartbeat SECONDS` sets the interval
(0 disables it).

## The liveness ping (ENQ/ACK), and what a reset looks like

Waiting for the USB device to go away says *that* the pad died.  To say *when*,
the pad answers a ping: the host writes ENQ (`0x05`) and the firmware replies
with a single ACK (`0x06`).  Neither is echoed and neither touches the command
line (see `consume_input()` in `src/usb.c`), so pinging twice a second leaves no
mark on the console.  The answer comes from the application poll, so it means
the firmware is running - not merely that USB is still enumerated.

`serve --ping-ms MS` sets the interval (default 500; 0 disables).  The daemon
records the **wall-clock time of the last answer**, not the time it noticed the
silence, so the numbers bound the failure rather than describe the reporting.
A reset therefore reads like this:

    # pad USB device gone - reset, reflash, or unplugged
    # no ACK at 09:55:30.285 - pad not answering; last answer 09:55:29.186 (1099 ms earlier)
    # pad USB device back - reading the boot counter
    # RESET: uptime went backwards (41550 -> 1550 ms)
    # RESET: boot counter 5962ee2f -> 5962ee30
    # ACK again at 09:55:31.286 - silent for 96 ms

which brackets the reset to [last answer, first answer] - about two seconds at
the default rate, or half a second at `--ping-ms 200`.  The boot counter and the
uptime are what make it authoritative: the USB device coming and going on its
own is not proof of a reset (measured: it re-enumerated while the chip kept
running), but a new boot counter is.

## What has actually been measured with it

* The RAM-only build does **not** reset across a disconnect/reconnect: boot
  counter unchanged, uptime climbing through the whole cycle.
* A reconnect with an empty bond store is refused by the console
  (`link key rejected (reason=5)`), as expected - the pad has nothing to answer
  the challenge with.
* The default (flash-backed) build **does** reset when it pairs; see
  `docs/reboot-hunting.md`.
