# padtool GUI

Run from a checkout, with Python 3.10 or newer and Tk:

```sh
python tools/padgui.py
```

Windows Python's installer includes Tcl/Tk by default. On Linux install your
Python distribution's Tk package (often `python3-tk`). There are no pip
requirements for configuration or flashing. CPU profiling additionally needs
`pyelftools` and the ELF matching the running firmware.

Select **pad** or **dongle**, scan, then select a device. Configuration and
diagnostics use that selected device. A USB gamepad session must end before
vendor commands become available. The Linux HID tool-mode button can request
this for HID profiles; on Windows end the session using the pad first, then scan.

The Settings tab loads the field descriptions, choices, values and help from
the pad firmware. Load each target before editing, apply changed fields together,
or tick Reset to use a field's default. Settings backups include pairing data.
Restores keep calibration unless explicitly selected. Dongle firmware has no
settings table; use Diagnostics for its pairing and target address.

## Firmware

The GUI calls `jlnewfw` directly for full flash backup and installation. It does
not call `make`, build firmware, or invoke the SDK's Windows flasher. Its log
shows the invoked command and output. Choose the executable and BR23 loader on
the Firmware tab. On Linux the default executable is `build/host/jlnewfw`:

```sh
make build/host/jlnewfw
```

For Windows, compile `tools/jlnewfw.c` with MinGW-w64 (or use the cross-compiled
`build/host/jlnewfw.exe` when supplied). From a MinGW terminal:

```sh
mkdir -p build/host
gcc -O2 -std=c11 -Wall -Wextra -o build/host/jlnewfw.exe tools/jlnewfw.c
```

Windows device access uses SCSI pass-through through the standard storage driver;
run Python in an Administrator terminal. Linux needs read/write access to the
matching `/dev/sg*` node. Inside our development container `/dev/bwrap/sg*` works.
Windows discovery currently probes PhysicalDrive0 through PhysicalDrive63.

The SDK files are `cpu/br23/tools/br23loader.bin`, `uboot.boot` and `cfg_tool.bin`.
The GUI derives defaults from `JIELI_SDK` if set; otherwise it uses this
workspace's SDK location. Browse to those files when running elsewhere.

Two install paths are available:

- **Flash image:** a packed raw image or UFW. The image's PID must match the
  selected board, and its chip key must match the device. A generic image from
  another chip may fail this check.
- **Pack and flash build:** select a folder with `app.bin` and `isd_config.ini`,
  plus the SDK's `uboot.boot` and `cfg_tool.bin`. `jlnewfw` packs for the connected
  chip's key. Selecting a board sets the default build folder for that board.

Firmware operations discover ROM loaders separately from the running-device
selector. Connect only the intended board: selection uses the board PID and
refuses multiple matching loaders. Flashing asks for a full pre-flash backup,
reads it before erasing, then writes and verifies the result. The GUI exposes
no PID/key override. Settings in erased sectors are replaced by the image;
back up settings separately if you want to restore them after installation.

Full flash backup is also available without installing firmware:

```sh
build/host/jlnewfw backup --pid GS_C2_ADC_DEVICE --enter-loader --wait 30 \
  --loader /path/to/br23loader.bin pad-backup.bin
```

Use `GS_C2_Dongle` for the dongle. An explicit `--device PATH` can back up a loader
without a recognized firmware PID. A backup leaves the board in its loader;
reconnect USB afterwards. Stock firmware restoration is outside the GUI's scope.

If automatic loader entry fails, unplug USB and hold Home + Start on the pad,
or the dongle's button, while plugging USB back in. Wait for each operation to
finish before closing the GUI. A failed write or verification leaves the device
in its loader for recovery.

## Code and validation

`padgui.py` owns the UI and serializes operations on a worker thread.
`padtool.py` supplies the firmware protocol and CLI. `padtransport.py` supplies
Linux SG_IO and Windows buffered SCSI pass-through. `jlnewfw.c` keeps packing,
loader protocol, chip-key checks and readback verification in one implementation.
An Android client will need its own USB host transport and UI; it can reuse the
wire protocol and potentially the C image/loader code.

Run GUI boundary tests on a display or under Xvfb:

```sh
python -m unittest discover -s tools -p test_padgui.py -v
```

These tests do not access USB. They check worker failures, settings selection,
changed-field commits, flash backup requirements and discovery filtering.
Cross-compilation checks the Windows C code, but Windows USB I/O still needs
hardware validation on Windows.
