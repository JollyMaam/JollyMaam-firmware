# Finding out whether (and why) the pad resets

`make test` fails when the chip's preserved boot counter moves, which is the
right check but a poor explanation: "the boot counter changed" can mean a
watchdog, a brownout, a fault, or a tooling artefact.  This page is about
narrowing that down.

## The three signals

| signal | where it comes from | what it means |
|---|---|---|
| the console goes away | `/dev/bwrap/ttyjlN` disappears, or reads/writes fail with `EIO` | the CDC device re-enumerated, so the chip reset (it also often comes back as a *different* node number) |
| `boots=` | `status`, preserved SRAM counter | how many boots this power cycle has seen; any change is a reset |
| `uptime=` | `status`, milliseconds since boot | a reset shows up as uptime starting over, which dates it |

The console dropping is the fastest signal - it is visible immediately, where
the boot counter needs a `status` round trip - but it is not sufficient on its
own, because USB re-enumeration has other causes.  The boot counter is the
authoritative one.

## The reset source

The PMU keeps the reset cause in two registers, `P3_RST_SRC` (0x12) and
`R3_RST_SRC` (RTC domain).  `src/main.c` samples both at the top of `app_main`
- as early as the application can look, because whoever reads them first may
clear them - and `status` prints them:

    reset-source=0 p33=64 rtc=1 uptime=6290 ms

`reset-source` is the SDK's own decoder (`power_reset_source_dump()`), which
reads 0 on this part and is kept only for comparison; `p33` and `rtc` are the
raw registers.

Calibration so far, all on this pad:

| event | p33 | rtc |
|---|---|---|
| first boot after `make flash` (loader reset) | 64 (0x40) | 1 |
| deliberate `clearkeys` (our own `p33_soft_reset()`) | 64 (0x40) | 1 |

So `p33=0x40` is what this chip reports for a *requested* reset either way.
The vendor's own decoder names the bits as 0x01 P33 reset, 0x02 DVDD POR
(power), 0x20 SOFTR (software) and 0x40 PLPM, so a spontaneous reset that
reports something *other* than 0x40 - 0x02 in particular - would point at
power rather than at a hang.  That is the thing to watch for.

## Watching for one

    make rebootwatch                            # 5 minutes, prints a timeline
    make rebootwatch WATCH_FLAGS="--seconds 600"

`tools/rebootwatch.py` keeps the console open, polls liveness with a short
`connect status` (the full `status` line carries a 900-entry bring-up ring, so
it is read only every few seconds), and reports every reset it sees with how
long the previous boot survived and the latched `p33`/`rtc`:

    09:31:03  start        boots=88ed4059 p33=64 rtc=1 uptime=35810 ms
    09:40:12  console lost  (uptime was 41320 ms) - re-enumeration means the chip reset
    09:40:14  console back  /dev/bwrap/ttyjl1 after 1.8s down: boots 88ed4059 -> 89ed4059,
              previous boot survived 41320 ms, p33=64 rtc=1

Correlate the timestamps with what the pad was doing: a reset while the
console is connected, with the radio busy, at a suspiciously regular interval,
or immediately after plugging something in, all point in different directions.

## Related

The bring-up ring in `status` (`bt-log`) records the milestones of each boot,
so a boot that died can be read back after the fact: the entries are
`(code << 16) | boot counter`, oldest first, and the end of the new boot's line
is where the previous one stopped.  See `docs/profile-switch.md` for what the
profile transitions record and `docs/bond-store.md` for the link-key codes.
