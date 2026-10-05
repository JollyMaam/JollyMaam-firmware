# Plan: what is left

A working plan to iterate on, not documentation: delete it when the items are
done.  Mark notes with `[ed:]` as before.

Suggested order, smallest and most independent first:

1. Prune the SDK config headers
2. Loop profiling and timing stats
3. Runtime settings in the firmware — done
4. Configuration tools: Python, then Android
5. Battery and charging
6. Rewrite `docs/`

Item 6 does not depend on anything and can move.  Item 5 starts with
measurements that need you and the pad; the rest is mine until a test step.

---

## 2. Loop profiling and timing stats — done (commit 7ea1229)

Always on, cheap enough to leave in, read over USB.  Not the old `debug`
console: counters in RAM, a USB command, and `padtool stats`.

**What is measured** (with `plat_us()`, 1 µs):

| Stat | How | Answers |
|---|---|---|
| loop pass time | `plat_us()` at the start and end of each pass; max and a histogram | is the loop ever slow, and which step (per-step max: input, bt_poll, outputs, ...) |
| wake latency | deadline vs. when the pass started | how late `plat_wait()` returns (the control task sits below the controller's) |
| report interval | time between successive `bt_send_input()` calls; min/max/histogram | jitter against `report_hz` |
| report age | time from the newest input change to the report that carries it | the latency the player feels, on the pad side |
| sends dropped | newest-wins slot overwritten before the controller took it; IN_FLIGHT full | link congestion |
| input samples | button ticks with a change, stick frames per second, CRC errors, IMU samples per report | whether each source keeps up |
| air (if available) | acks per second from the controller (`num_completed_packets`) | what actually left the radio |

Histograms: 16 log-spaced buckets (e.g. <125 µs ... >8 ms), u16 counts.
Reset on read or by command.  The cost per pass is a few `plat_us()` reads and
increments; measured before and after, and the stat itself says.

**Interface.**  `struct stats` in `controller.c` (the loop owns it); `bt.c`
keeps its own small counters and returns them with `bt_stats()`.  USB
sub-command `0x03 STATS` returns both, versioned.  `padtool stats [-w SECONDS]`
prints a table, `-w` a running view.

**Open.**  Whether "report age" should start at the GPIO change (tick ISR
timestamp, more accurate) or at the loop seeing it.

---

## 4. Configuration tools

### Python (`padtool`)

Grows from the existing tool, same file:

```
padtool get [SLOT] [NAME]          A rumble → 100
padtool set SLOT NAME VALUE ...    padtool set A type xbox rumble 60
padtool set pad brightness 40
padtool reset [SLOT]
padtool backup-settings FILE / restore-settings FILE   (JSON)
```

Plus the flashing it already does, `stats` and `log`.  Linux only (SG_IO).
`[ed:]` Windows: SCSI pass-through needs a different path in the tool, and possibly administrator rights; worth it now, or later? [ed: ANSWER: windows support (untested) is good... pretty sure jl-uboot-tool has it /shared/github.com-kagaimiq/jl-uboot-tool/scsiio/win32port.py]

### Android

A small app (Kotlin, one activity) using the USB host API: claim the
mass-storage interface, speak bulk-only transport (CBW/CSW) itself, send the
same vendor CDBs.  No root, no driver.  Screens:

1. Pad: identify, battery, firmware version; update firmware (bundled image
   or picked file), with the backup kept on the phone.
2. Slots: list A–down/left/right; tap one to edit, fields built from the
   schema (colour pickers for RGB, sliders for percent, lists for enums).
3. Log and stats, for bug reports.

Flashing from Android means speaking the ROM loader's protocol (the 0xFB/
0xFC/0xFD commands `jluboottool` uses) in Kotlin; it needs a port of the
loader conversation, which is the largest part of the app.  `[ed:]` settings
first and flashing second, or both for the first release?

Where it lives: its own repository, like the ESP32 tools.

---

## 5. Battery and charging

**What is known** (`docs/power-reset.md`): the app turns off the BR23's
internal charger; something else charges the battery; the VBAT reading is
4.76 V with USB in, which is not a cell voltage; USB presence is detected
from host frames (SOF), so a charger-only supply looks like no USB.  The stock
firmware has charge-state handling that was not fully recovered.

**Wanted:** know whether it is charging or full; on full, stop the charging
blink (solid or dark); battery percentage in the status; a correct low-battery
warning.

**Step 1: find the signals (needs you).**  A probe build that logs, once a
second, every unassigned GPIO's input level and every ADC channel, plus VBAT.
Runs: plug into a charger, a PC, unplug; leave it plugged until full.  What
to look for: a pin that changes when the charger starts and when it finishes
(charger ICs usually have CHRG and STDBY outputs), and a VBAT reading that
tracks the cell off USB.  Alongside: look in the stock image for what its
charge code reads (the recovered function at `0x01e3a806`).

**Step 2: implement.**  `plat_power(struct plat_power *)` replacing
`plat_usb_present()`/`plat_battery_mv()`: supply present, host present,
charging, full, cell mV.  The controller turns it into the LED inputs and the
status; the scenes in `settings_ux.c` decide what full looks like.

**Open.**  Whether full is even visible to the BR23; if not, a voltage
threshold with a hold time is the fallback.

---

## 6. Rewrite `docs/`

The facts stay; the voice changes.  The current pages argue with themselves
("This proves X; it does not prove Y; Z is plausible, not established"),
repeat their own caveats and lead with evidence instead of the fact.

**The new style:**

* State the fact first, plainly: "The pad runs on an external charger; the
  BR23's own charger is switched off."
* Mark uncertainty once, briefly, where it applies: "(not measured)" or a
  short "Unknown:" line, not a paragraph.
* Tables for pins, registers and addresses; prose only for how things fit
  together.
* No history of how something was found, no session notes, no references to
  old debugging commands that no longer exist (`debug bounce`, the CDC
  console).
* Evidence paths (Ghidra projects, backup images) collected at the end of
  each page under "Sources", not inline.

Same seven pages, same scope.  I rewrite one page first (`power-reset.md`) for
you to judge the tone before doing the rest.

---

## Not in this plan (noted for later)

* Switch 1 wake on a real Switch 1 console.
* Disconnect reason codes per console (needs logs from each).
* Whether retained RAM survives sleep (Home alone after sleep).
* An nRF port.
