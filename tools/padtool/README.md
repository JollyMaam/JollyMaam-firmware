# padtool GUI

Run from a checkout, with Python 3.10 or newer and Tk:

```sh
python tools/padtool/padgui.pyw
```

On Linux, `./tools/padtool/padgui.pyw` also works. On Windows, double-click
`padgui.pyw` to launch without a console window using the Python file association.
Windows Python's installer includes Tcl/Tk by default. `make padtool-dist`
builds a zip for Windows and Linux with padtool, bleak (64-bit Windows and
x86-64 Linux, Python 3.11 to 3.14; on Linux a system bleak comes first),
the RAM loaders and every board's firmware; its README.txt says how to use it. On Linux install your
Python distribution's Tk package (often `python3-tk`). With Nixpkgs:

```sh
nix develop
python tools/padtool/padgui.pyw
```

There are no pip requirements for configuration, image flashing or backups.
Advanced diagnostics and CPU profiling are available through the command-line
padtool. CLI profiling needs `pyelftools` and the ELF matching the running
firmware; add `ps.pyelftools` to the Nix package list if needed.

The command-line padtool also reaches a pad in config mode (Home+M held for
3 s, or `padtool config`) over Bluetooth LE: put `--ble` after the command,
alone for the first pad advertising or with its name or address
(`padtool get slot-a --ble`). It needs `bleak` (`ps.bleak`). Settings, the
log, status and the other commands work as on USB; `find`, `tool`,
`tool-only`, `hub`, `latency`, `peek` and `keep` need USB. On Linux padtool
leaves the Bluetooth link up when it is done, and the next run uses it, so a
command takes a fraction of a second; the pad leaves config mode a minute
after the last command (or on Home, or `padtool disconnect --ble`).

Devices appear by themselves: every second padtool looks at our tool devices
(an IDENTIFY each) and, on Linux, at disks it has not seen before, so ROM
loaders appear too. **Refresh** in the toolbar looks at everything again; on
Windows it is how ROM loaders are found (asking every drive is slow there).
With `bleak` installed (`ps.bleak`), Refresh also looks for pads in config
mode over Bluetooth LE (Home+M held for 3 s) for 3 seconds; one found shows
as "Bluetooth" in the tree (a pad in its updater too) and stays while its link is up (the GUI's
status reads keep config mode open). The flash loader needs USB. A
pad's settings load when it is selected (its slots) and when a page needs them
(palette, calibration, a remap slot). Each second padtool also reads a pad's
settings change counter (STATUS), and when another program or the pad itself
changed a setting, the loaded settings are read again; pending edits stay.
Unchanged devices keep their cached settings and pending edits. **Open device path…** at the bottom of the
device tree lets you enter a path manually, including reloading that device.
Linux labels show USB topology when available. Configuration and Show console use the selected device. A USB gamepad session must end before vendor commands
become available. The Linux HID tool-mode button can request this for HID
profiles; on Windows end the session using the pad first.

Selecting a pad shows its slots, each with a status button, type dropdown and
Unpair. Apply, Discard and Reset all to defaults below the device actions affect
all settings on that pad.
Select a slot or settings group in the tree to edit its detailed settings. Unpair is disabled without a pairing. It stages an action: on Apply, firmware clears the entire
Classic/LE bond and generates and saves a replacement Bluetooth address.
Address renewal hashes the complete previous address, including its prefix,
and the current tick to roll all six bytes while retaining the local/unicast
bits. There is no fixed per-pad prefix or generation counter. Changing a slot’s
type also renews its identity rather than returning to an earlier type’s
address; type changes through the dongle retain its existing pairing.
Discard cancels Unpair without touching the pad. Slot and settings-group entries
open detailed settings. The same firmware action is available from the CLI:

```sh
python tools/padtool/padtool.py unpair slot-a
```

**Developer fields** at the bottom expands or collapses advanced settings,
retaining pending edits. **Show console** on a running pad or dongle opens a
separate window with its live firmware log. That window stays with the device
when browsing other devices, follows it if its device path changes, and pauses
USB reads during settings or flash operations. Selecting Show console again
raises the existing window. Scrolling up pauses automatic scrolling to the end.
Closing the window stops its log polling. Advanced debug tools use the CLI.
Dongle and ROM-loader actions remain on their main entries.
There is no minimum window size. Toolbar and global device controls wrap in
narrow windows. Slot rows and detailed setting rows stay on one line and scroll
horizontally. Shift+wheel also scrolls horizontally.
The mouse wheel scrolls the current page anywhere in the window, including
over dropdowns and number fields, without changing their values. Shift+wheel
scrolls horizontally. Edits persist
while switching slots and devices. The global toolbar **Apply** validates all edited
values, checks device identities, then commits all pending changes once per
device. A failure on one device does not undo a previous device's commit;
uncommitted edits remain pending. The global toolbar **Discard** reverts all pending edits to the
last loaded or applied values without writing to devices. The top toolbar shows
the loaded device count and pending changes. Discard updates existing controls
from the cached values without reading devices or rebuilding pages. Default
fills the value box with its firmware default and stages removal of the saved override; editing the box afterwards
stages that new value instead. Closing asks before discarding edits.
Disconnected devices with pending edits remain in the tree until they reconnect or their edits are discarded.

A `*` on a setting status button means it has a staged edit; clicking it discards
that setting's edits. `✓` means its saved value differs from the firmware default;
clicking it stages the default, changing the marker to `*`. Blank means already
at the default. `!` marks an invalid edit and also offers discard. `?` means the
firmware could not supply its default; pending edits can still be discarded.
Hover over a status for its meaning and action. Curve points and their enable
flag share a status button. A `*` on a tree entry retains its separate meaning:
the entry contains values different from firmware defaults; colour marks drafts.
Firmware provides full defaults through a read-only query.

Slot colour fields have a palette dropdown with all 16 colour swatches and a
separate brightness dropdown. The selector and swatches show the full palette
colour at every brightness level.
Brightness is shown as steps `1/15` through `15/15`, plus Off.
Palette RGB fields still accept hex values and show swatches;
all previews follow staged palette edits.

On detailed settings pages, the leftmost status button replaces the separate
Default button and legend. A blank status is disabled; saved custom values can
be set to default, and staged edits can be undone from the same button.
The device overview uses the same status-button logic for slot types. Apply,
Discard and Reset all to defaults beside the slot, Palette or Calibration title
affect only that page’s settings. The header stays visible while settings scroll vertically, and follows
the horizontal scrollbar in narrow windows. Device-page
Apply and Discard cover the selected pad, including all slots, palette and
calibration. The global toolbar controls cover all devices. Other drafts are
retained when using a narrower scope. Page Apply and Discard are disabled
without pending edits in their scope. Reset requires defaults to be available
for all settings in its scope. Discard uses cached originals and keeps the
existing editors. Help is hidden behind each row's `?` button.
Setting names use readable labels.
Numeric inputs show their valid range beside the box, using firmware-reported
limits.

Remap base and Remap A–right are tree pages after Calibration. A connection
slot's **Remap** setting picks the one each connect starts with (base by
default); Home+M then a slot key picks another while connected, Home or M the
base. A remap slot takes every setting it has not set from Remap base, so the
base is where to set what all of them share; a setting's default on a remap
slot is the base's value. Names are each remap's own, and Remap A keeps its
"Swap A/B X/Y" keys. Remap base at its defaults sends the keys as they are and
the sticks exactly as calibrated.

A remap page has a **Name** (shown in the tree), then a row per key, "**KEY**
maps to [KEY / Nothing]", which costs no rule entries. Its status box covers
the key's mapping and rules together: click it to discard their edits or to
stage the default. **Add rule** adds a rule under that key: [is held / is held for / is pressed / is
released / is tapped under / is double-tapped within] [effect] [keys]. "Is
held" and "is held for" go with hold, turbo and turbo modifier; the others with
tap, toggle hold, toggle turbo and toggle turbo modifier. Turbo runs at 1–30
Hz, or at max, which flips the keys every report. A turbo modifier's keys are
the ones you press: while it is on, whatever they send pulses at its rate, each
from its own press, and other keys are left alone. For example, "R4 is held
turbo modifier A+B at 15 Hz", with R4 mapped to Nothing. The keys list stays
open while you tick keys; Done, Escape or a click outside closes it. Rules add
to the key's mapping; map it to Nothing to drop the key's own press. 16 rule entries fit. **Select preset** sets common rows (swap
A/B and X/Y, swap A/B, swap X/Y, Capture off) and keeps the rules; **Import
preset…** and **Export preset…** load and save a whole page as JSON, in the
CLI's text.

Below the rows is a box per stick (see below). Its curve has 15 points, with
fixed zero and full-scale endpoints; its deadzones are percent, 0–25. The Linear
preset is no curve at all: calibrated input passes through exactly. Any other
preset, a dragged point or pasted points turn the curve on. **Point values** opens the 15
sliders and percentage inputs at 6.25% travel intervals. **Copy points** copies
the comma-separated output bytes; **Paste points** accepts commas or spaces and
stages them on another stick or remap. Linear, Concave and S-curve presets are
defined in `padcurves.py`, shared by CLI and GUI.

Each stick has its own box, starting like a key's row: "**Left stick** maps
to [Left stick / Right stick / Nothing]". Then come **Invert X axis** and
**Invert Y axis** (check boxes), the inner and outer deadzones and the curve.
Every setting in a box belongs to that physical stick, wherever it is mapped:
map the left stick to the right and the right to the left to swap them. If both sticks go to one host stick, the one pushed
further wins. L3 and R3 stay where they are; map them too if you want them
swapped.

Each trigger has a box too: "**L2** trigger digital at [5] %" and its
**Analog mode**. The press point is where the trigger counts as pressed: the ZL/ZR
of the Switch profiles, the L2/R2 buttons of PlayStation, the mouse's clicks;
it lets go 3% short of it. Analog modes: **Default** sends the trigger as it is;
**Hair** sends it full from the press point and not at all below (also on
analog profiles such as Xbox); **Rapid** is Hair that lets go as soon as the
trigger rises 5% and presses again as soon as it goes 5% down, wherever it is
in its travel, for fast repeated presses.

The CLI sets the same fields in the same text (`padremap.py`):

```sh
python tools/padtool/padtool.py set remap-b name "Mario Kart" map swap-ab-xy
python tools/padtool/padtool.py set remap-b rules "r4 tapped:300 tap a; r4 held_for:300 hold l1; l4 pressed toggle_turbo a@15"
python tools/padtool/padtool.py set remap-b left_curve concave left_inner_deadzone 5
python tools/padtool/padtool.py set remap-b rules "r4 held turbo_modifier a+b+x@15" map r4=none
python tools/padtool/padtool.py set remap-b left_maps_to "right stick" right_maps_to "left stick" right_y_axis inverted
python tools/padtool/padtool.py set remap-b l2_press 5 r2_press 5 r2_mode hair
python tools/padtool/padtool.py set slot-a remap b
```

`left_curve concave` fills and enables points; `disabled`/`enabled` only toggle
the flag; `left_curve_points s-curve` fills points without changing the flag.

Curves were a connection slot's setting and deadzones calibration's in older
firmware; those values are not carried over, so set them again on a remap
page.

Connect and Pair use saved settings, so apply a slot's pending edits first.
`padtool disconnect` ends the current session and keeps its pairing;
`padtool connect` starts it again. While USB is powered, disconnect leaves
the tool device available. On battery the normal idle sleep policy applies.

The Ultimate 2C has four slots, A/B/X/Y, defaulting to Generic BLE,
DualSense Edge BLE, Xbox BLE and Switch 2 in that order, with separate identities and pairings. One slot runs at a time. Home from idle
reconnects the last selected slot; Home+View disconnects on release. While
offline, Home+A/B/X/Y selects that slot on release; holding the chord for
three seconds pairs it with a renewed identity. Sync pairs the current slot.
The old pairing stays saved until a new pairing succeeds. The last selected
slot survives reboot and battery sleep. The CLI can select a slot directly,
including while connected: `padtool connect B` or `padtool pair Y`.
Its types are those four; other types or transports and disabled slots
are refused.
Home+L3 or Home+R3 while offline starts calibration.
Home alone while connected reaches the host, including a quick tap.
Home+M quick settings and the d-pad slots are not enabled on this pad.

Sensor calibration is global, with one **Calibrate sensors** action in the
**Calibration** tree page. Leave sticks and triggers untouched
for 2.5 seconds after starting, then circle both sticks around their edges and
press both triggers fully. A or B saves; Home cancels. Calibration settings and
palette settings each have their own global entry in the tree.

On the pad’s device overview, backup and restore sit next to **Enter flash
loader**. **Reset all to defaults** sits beside Apply and Discard and stages
defaults for this pad’s slots, palette, calibration and pairing data. On a slot
page, the same button resets only that slot, including its developer settings
and pairing data. **Apply** saves the changes; **Discard** reverts them. Other
slots’ and devices’ edits outside the selected scope are retained.

**Back up pad settings…** reads all slots, the global palette and calibration
from the selected pad, including pairing data. It excludes pending GUI edits.
**Restore pad settings…** restores the targets in that backup across the pad.
Restores include calibration; run calibration again if needed. Dongle firmware has no
settings table; its pairing and target address controls are on its main page.

## Firmware

Full flash backup and image flashing run entirely in Python. Each chip's
ROM loader names the chip in its INQUIRY: `BR23` (the pads) or `BD19` (the
Nova 2 Lite dongle, an AC632N). Its RAM loader comes from the setting **RAM
loaders (SDK or folder)**: the SDK (`cpu/<chip>/tools/<chip>loader.bin`), a
folder of `br23loader.bin` / `bd19loader.bin`, or one of those files with the
others beside it. The default is `JIELI_SDK` (or this workspace's SDK). The
loaders are not tracked in version control; a future distribution can put
them beside the Python scripts, which then take precedence.
No external flashing executable is needed.
The loader is uploaded to RAM through UBOOT1.00; it provides SPI flash access.
It is separate from the `uboot.boot` contained inside firmware images.

Pads and dongles running this firmware are HID devices (`16c0:05df`,
manufacturer `JollyMaam`); Linux needs read/write access to their
`/dev/hidraw*` node, e.g. a udev rule matching `ATTRS{manufacturer}=="JollyMaam*"`.
The ROM loaders are mass storage: Windows
reaches them through SCSI pass-through on the standard storage driver, which
needs admin: `padgui.pyw` asks for it with the UAC prompt and starts again
elevated (`--no-elevate` skips that). Run the CLI from an Administrator terminal.
Linux needs read/write access to the matching `/dev/sg*` node. Where the
nodes are somewhere else (a container exporting them into one directory),
set `DEV_ROOT` to that directory. Windows
discovery currently probes PhysicalDrive0 through PhysicalDrive63.

**Flash Firmware…** accepts a packed `firmware.img`, or extracts the
flash.bin entry of a CRC-checked UFW. Prepare images with the firmware build
before using the GUI; it does not pack builds or call an external flasher.
The image’s product ID must match the selected device. If its chip key differs,
the shared GUI/CLI flasher validates and converts the application and resources
in memory for the connected chip, updating the key record and checksums before
any erase. The original image file stays unchanged. Conversion supports our
generated builds; stock images with secondary application directories or check
records are rejected rather than converted with incomplete format support.

Discovery names each device by product, from
`padknown.py`: this firmware by the PID it reports ("GameSir Nova 2 Lite ·
this firmware"), ROM loaders by the PID in their flash once read, and stock
pads and dongles that show themselves only as HID by their USB identity, in
each USB mode ("GameSir Cyclone 2 · stock, Switch (red)"). A stock device's
page shows its mode, product ID and USB port, and how it enters its loader:
**Enter flash loader** in the mode that has a command for it (both stock
pads: Xbox 360, green), otherwise which mode to switch to, and the way by
hand where one is known. The Nova 2 Lite's dongle is named, with no way into
a loader known.

Discovery detects `UBOOT1.00` ROM loaders. Selecting one uploads the
RAM helper and reads the product ID stored in flash; the page displays the actual
ID rather than a board selector. Invalid or blank headers show an unavailable ID.
Discovery itself only probes device identity and does not upload code.
A running pad or dongle, or a stock pad in a mode with a command, offers
**Enter flash loader**, which checks its identity (this firmware), requests
the reboot, waits for the ROM loader, then selects it automatically.
Other devices’ pending edits are retained; apply or discard edits on the
rebooting device first. On Linux, the USB port chain identifies the returning
loader even if its `sg` number changes. Where port information is unavailable,
only a single newly appearing loader is selected; ambiguous results require
manual selection.

Only a detected ROM loader offers flashing or a full flash backup in the GUI.
Its main page places **Back up full flash…** before **Flash Firmware…**. Flash
Firmware opens the sole image picker and immediately checks its product ID.
The image's PID must be a known product and the one in the device's flash, with capacity and chip-key conversion checks
before erasing. The selected loader path stays fixed during the operation; on
Linux its USB port is also checked to reject a reused device path. Settings
and the live firmware console are unavailable while the device is in loader mode.

The shared progress display shows waiting for the loader, loading the flash
helper, backing up, erasing, writing, verifying and restarting. Transfer stages
show byte-based percentages; waiting stages show an activity bar. Failures
stop the bar and keep the error in the console. Reconnect USB to leave loader
mode after a standalone backup.

Flashing asks whether to save a full pre-flash backup. Yes opens the destination
picker; No flashes without a backup; Cancel stops. When requested, the backup
completes before erasing. Verification always checks the written bytes.
Settings in erased sectors are replaced by the image; use a separate settings
backup to restore them after installation. The GUI exposes no PID/key override.

The Python backend can also run from a terminal:

```sh
python tools/padtool/padflash.py backup pad-backup.bin --pid GS_C2_ADC_DEVICE
python tools/padtool/padflash.py flash build/gamesir-cyclone-2-pad/firmware.img --pid GS_C2_ADC_DEVICE --backup before-flash.bin
```

Omit `--backup` to flash without creating a backup.

`--pid` names the product (`GS_C2_Dongle` for the Cyclone 2 dongle,
`AC695X_TRANS` for the Nova 2 Lite, `AC632N_TRANS` for its dongle); running
firmware for it, and a stock pad in a mode with a command, is asked into its
loader. `--loader` overrides the SDK or packaged loaders, as the setting
above. A standalone backup leaves the board in its loader; reconnect USB
afterwards. Stock firmware restoration is outside the GUI's scope. The C
`jollymaam` tool retains its own flashing, backup and Windows support.

If automatic loader entry fails, use the way by hand on the device's page
(Cyclone 2: hold Home + Start while connecting USB, which on this firmware
starts its updater; this firmware's dongle:
hold its button). Wait for each operation to
finish before closing the GUI. A failed write or verification leaves the device
in its loader for recovery.

### Installing: padinstall

`padinstall.py` is the plain way to put this firmware on a device the first
time: it waits for a pad or dongle on USB, says what it is (stock firmware
and its mode, this firmware and its build, a ROM loader or the 8BitDo boot
loader), asks, and writes the `firmware.img` for it from the zip's
`firmware` folder or this tree's `build`. A stock mode with no command for
its loader gets the way there instead (switch mode, or by hand). Jieli
boards: from the flash's start, after a backup of the whole flash into
`backups/` if wanted (asked; yes by default from stock firmware). The
8BitDo: above the boot loader. Only the image's sectors are written, so
settings and pairings, past it, stay. Then it waits for the device on this firmware and
says its build; a device installed or declined is not asked again while it
stays connected.

```sh
sudo python3 tools/padtool/padinstall.py      # Linux; Windows: "Install firmware.py" in the zip (asks for admin)
```

### The 8BitDo boot loader

The Ultimate 2C Wireless has no UBOOT1.00; its flash loader is 8BitDo's boot
loader (USB `2dc8:3208`, HID; the light under Home blinks). It appears in the
tree by itself, named by its model. **Enter flash loader** on the pad (this firmware) restarts it there; by hand: USB
unplugged, hold L1+R1, tap Home, plug in USB. Its page has **Flash firmware…**
(a build's `firmware.img`: the updater and the app from `0x6000`; its product
must be the loader's; settings and pairings, above it, stay; sectors that
already hold their part are not written again) and **Leave boot loader**,
which starts the firmware there without flashing. From a terminal:

```sh
python tools/padtool/padtool.py boot                  # the boot loaders
python tools/padtool/padtool.py boot start            # leave it
python tools/padtool/padtool.py boot flash build/8bitdo-ultimate2c-wireless-pad/firmware.img
```

`pad8bitdo.py` has the boot loader's protocol, on `padhid.Device` (hidraw;
Windows overlapped HID reads and writes); `test_pad8bitdo.py` simulates the
loader. `tools/8bitdoboot.py` uses it for `make
8bitdo-ultimate2c-wireless-pad-flash` and the stock pad's entry.

## Code and validation

`padgui.pyw` owns the UI and serializes operations on a worker thread.
`paddevices.py` finds new, changed and removed devices (every second, and everything on Refresh), and waits for other USB operations.
`padconsole.py` owns the live console windows and shares the USB operation lock.
`tools/bootloop.py` is a development stress-test helper, separate from padtool.
`padwidgets.py` supplies the palette and stick curve editors.
`padcurves.py` shares preset definitions and validation with
the CLI; `python -m unittest discover -s padtool -p test_padcurves.py -v` tests it.
`padknown.py` is the table of known products and stock USB identities (it
mirrors `jollymaam`'s); `padhid.py` lists HID interfaces and writes output
reports (Linux hidraw, Windows hid.dll); `test_padknown.py` tests the matching.
`padtool.py` supplies the firmware protocol and CLI. `padtransport.py` supplies
the tool device's reports (through `padhid.py`): on Linux, with firmware that
has the input report, each process's answers come addressed to it, so several
programs can use one device at once (`test_padtransport.py` tests this);
elsewhere answers are read back in feature reports, one program at a time. It
also supplies Linux SG_IO and
Windows buffered SCSI pass-through for the ROM loaders. `padflash.py` supplies the
BR23 RAM-loader protocol, image identity checks, backup, erase/write and readback
verification. It is ported from our `jollymaam.c`; build packing remains in C.
An Android client will need its own USB host transport and UI, and can reuse
these protocol definitions.

Run the flasher tests (no display or hardware required):

```sh
python -m unittest discover -s padtool -p test_padflash.py -v
```

Run GUI boundary tests on a display or under Xvfb:

```sh
python -m unittest discover -s padtool -p test_padgui.py -v
```

The flasher tests simulate a SCSI device to check RAM upload, chip info, exact
writes, backup-before-erase, identity/key rejection, short reads, ambiguous
loaders and failure to verify. GUI tests cover worker failures, settings
multi-slot drafts, identity checks, partial commits, global calibration
capability checks, optional flash backups and discovery filtering. Windows USB I/O still needs hardware validation on Windows.

## Battery logging

The Ultimate 2C Wireless battery sensor uses the stock firmware's PB6 ADC
conversion. Firmware with battery sampling reports the measured voltage through
STATUS. To record it every 30 seconds:

```sh
DEV_ROOT=/dev/bwrap python3 tools/padtool/battery.py --output build/battery.csv
```

Each CSV row contains a UTC timestamp, elapsed seconds, battery millivolts, USB
power, device uptime and connection status. Output is printed immediately and
appended to the optional file. `--interval 5` changes the period; `--samples 10`
stops after ten rows. Ctrl-C stops recording. If several matching pads are
connected, select one with `--device PATH` or its full flash ID. Reconnects
follow that same ID. `--product` selects another product running our firmware.
The monitor sends only IDENTIFY and STATUS commands.

USB power means a cable supplies power; it is not a charging/full indication.
Voltage uses the stock conversion rather than a calibrated capacity estimate.
Firmware without a battery reading continues to report its existing zero value.

## Android

The native Kotlin USB host app is in [`android/`](../android/README.md). It shares
settings-backup and remap-preset formats with these Python tools. Build commands,
USB setup and the Android hardware-validation boundary are described there.
