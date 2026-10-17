# padtool GUI

Run from a checkout, with Python 3.10 or newer and Tk:

```sh
python padtool/padgui.pyw
```

On Linux, `./padtool/padgui.pyw` also works. On Windows, double-click
`padgui.pyw` to launch without a console window using the Python file association.
Windows Python's installer includes Tcl/Tk by default. On Linux install your
Python distribution's Tk package (often `python3-tk`). With Nixpkgs:

```sh
nix-shell -p 'python3.withPackages (ps: [ ps.tkinter ])'
python padtool/padgui.pyw
```

There are no pip requirements for configuration, image flashing or backups.
CPU profiling additionally needs `pyelftools` and the ELF matching the running
firmware; add `ps.pyelftools` to the Nix package list if needed.

Startup reads every pad slot, palette and global calibration setting. Devices
are detected automatically about once per second; unchanged devices keep their
cached settings and pending edits. **Open device path…** at the bottom of the
device tree lets you enter a path manually, including reloading that device.
Linux labels show USB topology when available. Configuration and diagnostics
use the selected device. A USB gamepad session must end before vendor commands
become available. The Linux HID tool-mode button can request this for HID
profiles; on Windows end the session using the pad first.

Selecting a pad shows its slots, each with a type dropdown, Apply, Settings,
Connect and Pair. Slot and settings-group entries open detailed settings.
**Developer fields** at the bottom expands or collapses advanced settings,
retaining pending edits. **Debug tools** under running pads contains diagnostics
and the operation log. Dongle and ROM-loader actions are on their main entries.
Connect and Pair are on the pad overview.
There is no minimum window size. Toolbar and global device controls wrap in
narrow windows. Slot rows and detailed setting rows stay on one line and scroll
horizontally. Shift+wheel also scrolls horizontally.
The mouse wheel scrolls the current page anywhere in the window, including
over dropdowns and number fields, without changing their values. Shift+wheel
scrolls horizontally. Edits persist
while switching slots and devices. **Apply** validates all edited
values, checks device identities, then commits all pending changes once per
device. A failure on one device does not undo a previous device's commit;
uncommitted edits remain pending. **Discard** reverts all pending edits to the
last loaded or applied values without writing to devices. The top toolbar shows
the loaded device count and pending changes. Reset fills the value box with its firmware
default and stages removal of the saved override; editing the box afterwards
stages that new value instead. Closing asks before discarding edits. Disconnected devices with pending edits
remain in the tree until they reconnect or their edits are discarded.

A `*` beside a setting or tree entry means its effective value differs from the
firmware default, including already-saved custom values. This is independent of
the pending-changes count. Explicitly saved values equal to their default do not
get a `*`. `!` indicates an invalid edited value; `?` means an older firmware
could not supply the default. Protocol 4 provides full defaults for every field,
including pairing bytes, custom curves and the calibration blob, through a
read-only query. On older firmware, only known defaults can be reset in the GUI.

Slot colour fields have a palette dropdown with all 16 colour swatches and a
separate brightness dropdown. The selector and swatches show the full palette
colour at every brightness level.
Brightness is shown as steps `1/15` through `15/15`, plus Off.
Palette RGB fields still accept hex values and show swatches;
all previews follow staged palette edits.

Each setting has its own Apply button; it saves only that setting and retains
all other drafts. Apply at the top saves all pending edits. Help is
hidden behind each row's `?` button. Setting names use readable labels.
Numeric inputs show their valid range beside the box, using firmware-reported
limits.

Each stick has a 15-point curve, with fixed zero and full-scale endpoints.
**Use points** enables it; switching that off gives exact calibrated linear
passthrough and retains all saved points. The graph shows active input in blue
and, when disabled, the retained curve as a dashed line. **Point values** opens
the 15 sliders and percentage inputs at 6.25% travel intervals. Reset restores
the default points and disables the curve. The curve's Apply button saves its
points and enable flag together. **Copy points** copies the comma-separated
output bytes; **Paste points** accepts commas or spaces and stages them on another
slot or stick. Pasting retains that stick’s Use points setting; Apply saves the edit.

Linear, Concave and S-curve presets are defined in `padcurves.py`, shared by
CLI and GUI. Selecting a preset fills the points and enables them; manual edits
show Custom. Selecting Custom keeps the current points for editing. Firmware contains point interpolation and bypass only. The curved
presets approximate the previous formulas, with maximum full-scale differences
under 0.3% at 15 points. To get exact linear passthrough, turn Use points off.

The CLI uses the same presets and validation:

```sh
python padtool/padtool.py set A left_curve concave
python padtool/padtool.py set A left_curve disabled
python padtool/padtool.py set A left_curve enabled
python padtool/padtool.py set A left_curve_points s-curve
```

The first command fills and enables points. Disabled/enabled only toggle the
flag; the last command fills points without changing the enable flag.

When the command-line flasher upgrades from protocol 4 or older with a running
pad connected and a pre-flash backup requested, it saves a `.settings.json`
next to the flash backup and restores it after the new firmware starts. Shared host migration converts old formula
modes to presets and resamples seven-point custom curves, retaining their knots.
Inactive custom points remain saved with the enable flag off. If the pad is
already in the ROM loader, or another flasher is used, back up settings before
the upgrade and restore them with the updated padtool afterwards. The firmware
can resample legacy custom records itself; old Concave/S-curve modes require
host conversion. Never-reused field IDs separate old and new layouts.

Connect and Pair use saved settings, so apply a slot's pending edits first.
Sensor calibration is global, with one **Calibrate sensors** action in the
**Calibration** tree page. Leave sticks and triggers untouched
for 2.5 seconds after starting, then circle both sticks around their edges and
press both triggers fully. A or B saves; Home cancels. Calibration settings and
palette settings each have their own global entry in the tree.

On the pad’s device overview, backup, restore and **Reset all to defaults** sit
next to **Enter flash loader**. Reset all stages defaults for this pad’s slots,
palette, calibration and pairing data. **Apply** saves the changes; **Discard**
reverts them. Other devices’ pending edits are retained.

**Back up pad settings…** reads all slots, the global palette and calibration
from the selected pad, including pairing data. It excludes pending GUI edits.
**Restore pad settings…** restores the targets in that backup across the pad.
Restores include calibration; run calibration again if needed. Dongle firmware has no
settings table; its pairing and target address controls are on its main page.

## Firmware

Full flash backup and image flashing run entirely in Python. The BR23 RAM
loader is read from `JIELI_SDK/cpu/br23/tools/br23loader.bin` during development
(or this workspace’s SDK location). It is not tracked in version control.
A future distribution can include `padtool/br23loader.bin` beside the Python
scripts; that untracked packaged copy takes precedence.
No external flashing executable is needed.
The loader is uploaded to RAM through UBOOT1.00; it provides SPI flash access.
It is separate from the `uboot.boot` contained inside firmware images.

Windows device access uses SCSI pass-through through the standard storage driver;
run Python in an Administrator terminal. Linux needs read/write access to the
matching `/dev/sg*` node. Inside our development container `/dev/bwrap/sg*` works.
Windows discovery currently probes PhysicalDrive0 through PhysicalDrive63.

**Flash Firmware…** accepts a packed `firmware.img` unchanged, or extracts the
flash.bin entry of a CRC-checked UFW. Prepare images with the firmware build
before using the GUI; it does not pack builds or call an external flasher.
The image’s product ID and chip key must match the selected device.

Automatic discovery detects `UBOOT1.00` ROM loaders. Selecting one uploads the
RAM helper and reads the product ID stored in flash; the page displays the actual
ID rather than a board selector. Invalid or blank headers show an unavailable ID.
Discovery itself only probes device identity and does not upload code.
A running pad or dongle offers **Enter flash loader**, which checks its identity,
requests the reboot, waits for the ROM loader, then selects it automatically.
Other devices’ pending edits are retained; apply or discard edits on the
rebooting device first. On Linux, the USB port chain identifies the returning
loader even if its `sg` number changes. Where port information is unavailable,
only a single newly appearing loader is selected; ambiguous results require
manual selection. For a stock pad, connect USB while holding Home + Plus.

Only a detected ROM loader offers flashing or a full flash backup in the GUI.
Its main page places **Back up full flash…** before **Flash Firmware…**. Flash
Firmware opens the sole image picker and immediately checks its product ID.
The image selects pad versus dongle, with PID, capacity and chip-key checks
before erasing. The selected loader path stays fixed during the operation; on
Linux its USB port is also checked to reject a reused device path. Settings
and diagnostics are disabled while the device is in loader mode.

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
python padtool/padflash.py backup pad-backup.bin --board pad
python padtool/padflash.py flash build/pad/firmware.img --board pad --backup before-flash.bin
```

Omit `--backup` to flash without creating a backup.

Use `--board dongle` for the dongle and `--loader FILE` to override the SDK or packaged
loader. A standalone backup leaves the board in its loader; reconnect USB
afterwards. Stock firmware restoration is outside the GUI's scope. The C
`jlnewfw` tool retains its own flashing, backup and Windows support.

If automatic loader entry fails, unplug USB and hold Home + Start on the pad,
or the dongle's button, while plugging USB back in. Wait for each operation to
finish before closing the GUI. A failed write or verification leaves the device
in its loader for recovery.

## Code and validation

`padgui.pyw` owns the UI and serializes operations on a worker thread.
`paddevices.py` monitors device presence, pausing during other USB operations.
`tools/bootloop.py` is a development stress-test helper, separate from padtool.
`padwidgets.py` supplies the palette/brightness and stick curve editors.
`padcurves.py` shares preset definitions, validation and legacy conversion with
the CLI; `python -m unittest discover -s padtool -p test_padcurves.py -v` tests it.
`padtool.py` supplies the firmware protocol and CLI. `padtransport.py` supplies
Linux SG_IO and Windows buffered SCSI pass-through. `padflash.py` supplies the
BR23 RAM-loader protocol, image identity checks, backup, erase/write and readback
verification. It is ported from our `jlnewfw.c`; build packing remains in C.
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
