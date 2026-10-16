# padtool GUI

Run from a checkout, with Python 3.10 or newer and Tk:

```sh
python tools/padgui.py
```

Windows Python's installer includes Tcl/Tk by default. On Linux install your
Python distribution's Tk package (often `python3-tk`). With Nixpkgs:

```sh
nix-shell -p 'python3.withPackages (ps: [ ps.tkinter ])'
python tools/padgui.py
```

There are no pip requirements for configuration, image flashing or backups.
CPU profiling additionally needs `pyelftools` and the ELF matching the running
firmware; add `ps.pyelftools` to the Nix package list if needed.

Click **Scan and load** to discover both pads and dongles and read every pad
slot, palette and global calibration setting. Select a device or settings group
in the tree; Linux device labels also show USB topology when available.
Configuration and diagnostics use the selected device. A USB gamepad session
must end before vendor commands become available. The Linux HID tool-mode
button can request this for HID profiles; on Windows end the session using the
pad first, then scan.

The Slots page gives each slot a type dropdown, Settings, Connect and Pair.
Details contains the remaining settings and developer fields. Edits persist
while switching slots and devices. **Apply changes** validates all edited
values, checks device identities, then commits all pending changes once per
device. A failure on one device does not undo a previous device's commit;
uncommitted edits remain pending. Reset fills the value box with its firmware
default and stages removal of the saved override; editing the box afterwards
stages that new value instead. Rescanning or closing asks before discarding edits.

A `*` beside a setting or tree entry means its effective value differs from the
firmware default, including already-saved custom values. This is independent of
the pending-changes count. Explicitly saved values equal to their default do not
get a `*`. `!` indicates an invalid edited value; `?` means an older firmware
could not supply the default. Protocol 4 provides full defaults for every field,
including pairing bytes, custom curves and the calibration blob, through a
read-only query. On older firmware, only known defaults can be reset in the GUI.

Slot colour fields have a palette selector and a brightness dropdown. The
selector shows the pad's 16 palette colours, with the full colour above and the
selected brightness below. Brightness percentages use the firmware's actual
nonlinear levels. Palette RGB fields still accept hex values and show swatches;
all previews follow staged palette edits.

Each setting has its own Apply button; it saves only that setting and retains
all other drafts. Apply changes at the top saves all pending edits. Help is
hidden behind each row's `?` button. Setting names use readable labels.

Custom stick curve points have a graph with draggable handles, seven sliders,
and percentage inputs. The inputs correspond to 12.5% through 87.5% stick
travel. Outputs stay in increasing order, and the 0% and 100% endpoints stay
fixed. Reset restores the default curve. Choose custom in the corresponding
stick curve mode setting to activate the points.

Connect and Pair use saved settings, so apply a slot's pending edits first.
Sensor calibration is global, with one **Calibrate sensors** action on the
pad's device page. USB calibration requires pad firmware protocol 3 or newer;
older firmware shows the action disabled. Leave sticks and triggers untouched
for 2.5 seconds after starting, then circle both sticks around their edges and
press both triggers fully. A or B saves; Home cancels. Calibration settings and
palette settings each have their own global entry in the tree.

Settings backups read the saved device values, and include pairing data.
Restores keep calibration unless explicitly selected. Dongle firmware has no
settings table; its pairing and target address controls are in Diagnostics.

## Firmware

Full flash backup and image flashing run entirely in Python. The BR23 RAM
loader is bundled in `tools/loaders/` with the SDK's Apache 2.0 license and
provenance. No SDK checkout or external executable is needed for these actions.
The loader is uploaded to RAM through UBOOT1.00; it provides SPI flash access.
It is separate from the `uboot.boot` contained inside firmware images.

Windows device access uses SCSI pass-through through the standard storage driver;
run Python in an Administrator terminal. Linux needs read/write access to the
matching `/dev/sg*` node. Inside our development container `/dev/bwrap/sg*` works.
Windows discovery currently probes PhysicalDrive0 through PhysicalDrive63.

Two install paths are available:

- **Flash image:** writes a packed `firmware.img` unchanged, or extracts and
  writes the flash.bin entry of a CRC-checked UFW. The image's PID must match
  the selected board, and its chip key must match the device. A generic image
  from another chip may fail this check; use build packing in that case.
- **Pack and flash build:** optionally calls `jlnewfw pack` to prepare an image
  for the connected chip's key, then flashes it through the Python backend.
  Select a folder with `app.bin` and `isd_config.ini`, plus the SDK's
  `uboot.boot` and `cfg_tool.bin`. Selecting a board sets its default build folder.

For optional build packing, compile `jlnewfw` on Linux:

```sh
make build/host/jlnewfw
```

For Windows, compile `tools/jlnewfw.c` with MinGW-w64 (or use the cross-compiled
`build/host/jlnewfw.exe` when supplied). From a MinGW terminal:

```sh
mkdir -p build/host
gcc -O2 -std=c11 -Wall -Wextra -o build/host/jlnewfw.exe tools/jlnewfw.c
```

The packing resources are `cpu/br23/tools/uboot.boot` and `cfg_tool.bin`. The GUI
derives defaults from `JIELI_SDK` if set; otherwise it uses this workspace's SDK
location. Browse to those files when packing elsewhere.

Firmware operations discover ROM loaders separately from the running-device
selector. Connect only the intended board: selection uses the board PID and
refuses multiple matching loaders. Flashing asks for a full pre-flash backup,
reads and saves it before erasing, then writes and verifies the result. The GUI
exposes no PID/key override. Settings in erased sectors are replaced by the
image; back up settings separately to restore them after installation.

The Python backend can also run from a terminal:

```sh
python tools/padflash.py backup pad-backup.bin --board pad
python tools/padflash.py flash build/pad/firmware.img --board pad --backup before-flash.bin
```

Use `--board dongle` for the dongle and `--loader FILE` to override the bundled
loader. A standalone backup leaves the board in its loader; reconnect USB
afterwards. Stock firmware restoration is outside the GUI's scope. The C
`jlnewfw` tool retains its own flashing, backup and Windows support.

If automatic loader entry fails, unplug USB and hold Home + Start on the pad,
or the dongle's button, while plugging USB back in. Wait for each operation to
finish before closing the GUI. A failed write or verification leaves the device
in its loader for recovery.

## Code and validation

`padgui.py` owns the UI and serializes operations on a worker thread.
`padwidgets.py` supplies the palette/brightness and stick curve editors.
`padtool.py` supplies the firmware protocol and CLI. `padtransport.py` supplies
Linux SG_IO and Windows buffered SCSI pass-through. `padflash.py` supplies the
BR23 RAM-loader protocol, image identity checks, backup, erase/write and readback
verification. It is ported from our `jlnewfw.c`; build packing remains in C.
An Android client will need its own USB host transport and UI, and can reuse
these protocol definitions.

Run the flasher tests (no display or hardware required):

```sh
python -m unittest discover -s tools -p test_padflash.py -v
```

Run GUI boundary tests on a display or under Xvfb:

```sh
python -m unittest discover -s tools -p test_padgui.py -v
```

The flasher tests simulate a SCSI device to check RAM upload, chip info, exact
writes, backup-before-erase, identity/key rejection, short reads, ambiguous
loaders and failure to verify. GUI tests cover worker failures, settings
multi-slot drafts, identity checks, partial commits, global calibration
capability checks, flash backup requirements and discovery filtering. Windows USB I/O still needs hardware validation on Windows.
