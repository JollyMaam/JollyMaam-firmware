# Console

`console.c` (prefix `con_`) is a line-based text console on the USB serial
port (CDC ACM).  It is how settings are changed, slots are paired and
connected from a computer, and how the pad says what it is doing.

The USB device itself (enumeration, CDC endpoints) is platform code; the
console sees a byte stream:

```c
int  plat_usb_read(u8 *buf, int max);      /* non-blocking */
void plat_usb_write(const u8 *buf, int len);
int  plat_usb_present(void);               /* a host is sending frames */
```

## Interface

```c
void con_poll(void);                         /* from ctl_poll: read, run lines */
void con_log(const char *fmt, ...);          /* any thread; "log.console" gates it */
```

`con_log()` is the only way any module prints.  Modules do not include the
console otherwise.

## Commands

A table, one row per command: name, argument summary, help, handler.

| Command | Does |
|---|---|
| `help [COMMAND]` | the table, or one command's help |
| `status` | one block: state, slot, profile, address, USB, battery, calibration, report rate, link quality, pending settings |
| `config [PATH]` | show settings under PATH (all if none) |
| `config PATH VALUE` | set one |
| `config reset [PATH]` | back to defaults |
| `config commit` | write now (when `global.autocommit` is off) |
| `connect SLOT` / `pair SLOT [--fresh]` / `disconnect` | as the Home gestures |
| `wake SLOT [CHANNEL] [SECONDS]` | Switch 1 console wake |
| `cal start\|done\|cancel` | the calibration routine |
| `rumble L R MS`, `led MASK RRGGBB` | hardware check |
| `reboot`, `uboot` | restart; restart into the flash loader |

Slots are named A B X Y up down left right, after the Home chord that selects
them, or 0..7.

Replies end in a line `OK` or `ERR <reason>` so tools can tell when a command
is done (`tools/padbus.py` relies on that).

Dropped from today's console: tab completion, per-namespace help, the
`status` tree, and the `debug` namespace (probe, inject, ps, bounce, timing,
peek/poke, bond store dumps).  `status` is printed directly instead of through
the settings engine.
