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
| deliberate `reboot` (our own `p33_soft_reset()`) | 64 (0x40) | 1 |

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

## `p33=64` is not proof of a deliberate reset

The table above reads `p33=0x40` as "a reset somebody asked for".  That is too
generous: a *fault* lands there too.  A console command added on 2026-09-23
called `uxTaskGetStackHighWaterMark()` - a symbol the linker resolves into the
low-`.data` OS thunk table, but one this OS build never wires up - and the pad
reset on *every* invocation, with `status` afterwards reporting

    reset-source=0 p33=64 rtc=1        (boot counter moved, uptime restarted)

which is exactly what the "spontaneous" resets during a console session look
like.  So when a reset reports `p33=64`, treat "requested" as a hypothesis, not
a conclusion.  Two cheap ways to separate them:

* the bring-up ring says whether the reset landed on a *step* of a known
  sequence (a pairing, a connection, a profile start) or somewhere arbitrary;
* a reset that follows one command, repeatably, is a fault in that command.
  Bisect it the way the probe above did: print a marker before each call and
  see which marker is the last one printed.

This class of fault is easy to introduce by accident, because a call to an
entry point the shipped OS image does not provide still links - it resolves to
a table slot and jumps into rubbish at runtime.  `nm build/app.elf | grep sym`
shows such entries as `d` (data) at a low address rather than `t` (text), which
is worth checking before trusting a new API.  Nothing else in this project
calls one.

## Related

The bring-up ring in `status` (`bt-log`) records the milestones of each boot,
so a boot that died can be read back after the fact: the entries are
`(code << 16) | boot counter`, oldest first, and the end of the new boot's line
is where the previous one stopped.  See `docs/profile-switch.md` for what the
profile transitions record and `docs/bond-store.md` for the link-key codes.

## 2026-09-23: the flash-backed build resets when it pairs

The syscfg-backed bond store (`BOND_STORE_RAM=0`, the default) has a crash the
RAM-only build does not.  It is written down here because it spent a long time
hiding behind "the pad keeps resetting".

What reproduces it, on the default build only:

* `pair N` - entering pairing mode, which is also what the physical five-second
  Home+face chord does - resets the chip within a few seconds.  Measured:
  uptime 27320 ms before the command, 3790 ms after it, boot counter moved.
  The RAM-only build runs the same command and keeps going.
* A pairing with a console resets it too, right after
  `HCI_EVENT_LINK_KEY_NOTIFICATION`.  With the link-key store's flash access
  removed, the same pairing runs on to `hid channels open`.

What has been tried, all measured, none of it sufficient:

| change | result |
|---|---|
| stage the link key, write it from the app task 2 s later | still resets |
| ... and only when `radio_state` is OFF or STOPPING | still resets |
| ... and only after 3 s of settled idle | still resets |
| defer the peer-address write the pairing path makes | still resets |
| mask interrupts around every deferred write | still resets |
| remove the link-key store's flash access entirely | the *immediate* reset goes away |

The RAM-only build is immune, and it is immune because it touches no flash on
these paths at all.  Interrupt masking not helping points away from "an
interrupt taken while the flash is busy" and towards the syscfg write itself
disturbing the controller while it runs: the pad executes from the same SPI
flash the VM layer writes, and the controller library keeps its own state in
there as well.

Until this is understood, use `BOND_STORE_RAM=1` for anything that pairs or
connects.  The default build is fine to *read* bonds - `keys` and `setkey` on
an idle pad both work, measured - but must not be asked to pair.

## 2026-09-25: a received LMP_DETACH stalled until the supervision timeout

Measured against a Linux host (`tools/hostbt.py` + `tools/detach_round.sh`):
when the peer disconnected, the pad saw the `LMP_DETACH` (marker `7e0e`) but
only reported the disconnection about 12 s later, with reason `0x08`
(supervision timeout) instead of the peer's `0x13`.  One of these runs ended in
a reset about 3 s after the detach, with `7e0e` the last marker of that boot.

The cause is in `btctrler.a`.  `lmp_rx_detch()` only flags the connection;
`lmp_detach_check()` does the teardown, and skips a connection while its
received-ACL queue (`rx_acl_u`) is not empty.  In standard-HCI mode
(`config_btctler_hci_standard = 1`) `lmp_send_acl_u_packet_to_host()` hands
one packet to the host per call and marks it done, but only frees done
packets on its *next* call.  The last packet before a detach - the peer's
12-byte L2CAP Disconnection Request, queue markers `7301 722a 710c` - is
therefore still queued, nothing calls the function again, and the teardown
waits until the supervision timeout forces it.

`tools/patch_btctrler.py: patch_detach_drain_rx()` (part of every patch mode
except "none") drains the queue with that same function before the check.
Afterwards: `7e0e 7d20 7d21 7513 2a13` - teardown straight away, reason 0x13 -
in 18 consecutive rounds, both for an explicit disconnect and for BlueZ
dropping the idle link itself, with no resets.

Whether this was the *only* cause of the resets after a disconnect is not
proven; the one reset seen had the same start, and none have been seen since.
The long "no ACK" gaps padbus logged during these runs were its own ping loop
stalling (the gap is reported late and the next ping is answered within
~70 ms), not the pad.
