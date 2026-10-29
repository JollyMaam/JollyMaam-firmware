#!/usr/bin/env python3
"""Package padtool for Windows and Linux: make padtool_dist.

    tools/padtool_dist.py --build build --version ID --out build/padtool-ID.zip

The zip holds padtool (CLI and GUI, without its tests), the Jieli RAM
loaders the GUI's flasher needs (br23loader.bin, bd19loader.bin, beside
padflash.py, where it looks first), bleak with its WinRT backend for 64-bit
Windows and its BlueZ one (dbus_fast) for x86-64 Linux, Python 3.11-3.14
(padtool/lib, which padble puts on the path: on Linux after the system's
own), and every board's firmware from the build.  bleak comes from PyPI
through uv (cross-platform resolution; pip judges the markers by this
machine), cached in the build directory.

Python itself is not included: on Windows python.org's 64-bit installer,
with Tk."""
import argparse
import filecmp
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parent.parent
BLEAK = "bleak==2.1.1"            # the version padble was tested with (BlueZ)
PYTHONS = ["3.11", "3.12", "3.13", "3.14"]   # 3.10 gets an older winrt, other files
PLATFORMS = {"win": "x86_64-pc-windows-msvc", "linux": "x86_64-unknown-linux-gnu"}
COMPILED = (".pyd", ".so")
SDK = ROOT / "external" / "jieli-fw-AC63_BT_SDK"
LOADERS = [SDK / "cpu" / "br23" / "tools" / "br23loader.bin", SDK / "cpu" / "bd19" / "tools" / "bd19loader.bin"]
# Board build directory: the files a user flashes (firmware.img through the
# ROM loader or the 8BitDo's boot loader, update.bin through the updater:
# padtool update).
BOARDS = {
    "gamesir-cyclone-2-pad": ["firmware.img", "update.bin"],
    "gamesir-cyclone-2-dongle": ["firmware.img", "update.bin"],
    "gamesir-nova-2-lite-pad": ["firmware.img", "update.bin"],
    "gamesir-nova-2-lite-dongle": ["firmware.img", "update.bin"],
    "8bitdo-ultimate2c-wireless-pad": ["firmware.img", "update.bin"],
}

# py (the launcher) if there is one, else python; the GUI through .pyw's
# file association (pyw is not always on the path).
PADTOOL_CMD = ('@echo off\r\nwhere py >nul 2>nul\r\n'
               'if not errorlevel 1 (py -3 "%~dp0padtool\\padtool.py" %*) else (python "%~dp0padtool\\padtool.py" %*)\r\n')
# The GUI: .pyw launchers beside the folder (a .cmd flashes a console).
PADGUI_PYW = """# padgui: double-click (pythonw, no console window).
import os, runpy, sys
gui = os.path.join(os.path.dirname(os.path.abspath(__file__)), "padtool", "padgui.pyw")
sys.path.insert(0, os.path.dirname(gui))
sys.argv[0] = gui{extra}
runpy.run_path(gui, run_name="__main__")
"""
README = """padtool {version} for Windows and Linux

Needs 64-bit Python 3.11 to 3.14 from python.org (with "tcl/tk" and the
"py launcher", both on by default).  Nothing to install: bleak, for
Bluetooth, is in padtool\\lib.

  padgui.pyw               the GUI (double-click); it asks for admin, for
                           the ROM loader's disk
  padgui (no admin).pyw    the GUI without asking: USB tool devices and
                           Bluetooth; whether the ROM loader works so is
                           not established
  padtool.cmd --help       the command line, from a Command Prompt here

Bluetooth: hold Home+M on the pad for 3 seconds (config mode), then
Refresh in the GUI, or put --ble after a command:  padtool.cmd get --ble

Linux (x86-64, Python 3.11 to 3.14; the GUI needs tkinter): the same files,
  python3 padgui.pyw  and  python3 padtool/padtool.py --help
padtool\\lib's bleak is used when the system has none.  BlueZ and
permission for the tool devices (padtool/README.md) as for a checkout.

firmware\\<board>\\ holds this build's firmware:
  update.bin     padtool.cmd update firmware\\<board>\\update.bin
                 (a pad or dongle already running this firmware, on USB)
  firmware.img   for the GUI's flasher: the whole flash (UBOOT1.00 ROM
                 loader); the 8BitDo's updater and app (its boot loader, or
                 padtool.cmd boot flash).  Leave boot loader in the GUI
                 (padtool.cmd boot start) starts it without flashing

padtool\\README.md has the rest.
"""


def bleak_for(platform, version, cache):
    """bleak and its backend for one Python on one platform, installed into
    the cache once."""
    target = cache / platform / f"cp{version.replace('.', '')}"
    if not (target / "bleak").is_dir():
        uv = shutil.which("uv")
        if not uv:
            sys.exit("padtool_dist: needs uv to fetch bleak (nix-shell -p uv, or pip install uv)")
        shutil.rmtree(target, ignore_errors=True)
        subprocess.run([uv, "pip", "install", "--quiet", "--python-platform", PLATFORMS[platform], "--python-version", version,
                        "--only-binary", ":all:", "--target", str(target), BLEAK], check=True)
    return target


def merge(trees, lib):
    """The trees in one: each Python's compiled modules side by side (their
    names carry the version and platform), every other file once, the same
    in every tree that has it.  The wheels' records are left out (they name
    one tree's compiled modules)."""
    for t in trees:
        for f in sorted(t.rglob("*")):
            rel = f.relative_to(t)
            if not f.is_file() or "__pycache__" in rel.parts or rel.parts[0].endswith(".dist-info"):
                continue
            dest = lib / rel
            if dest.exists():
                if f.suffix not in COMPILED and not filecmp.cmp(f, dest, shallow=False):
                    sys.exit(f"padtool_dist: {rel} differs between {trees[0]} and {t}")
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, dest)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--build", type=Path, default=ROOT / "build")
    p.add_argument("--version", default="dev")
    p.add_argument("--out", type=Path)
    a = p.parse_args()
    out = a.out or a.build / f"padtool-{a.version}.zip"
    stage = a.build / "padtool-dist" / f"padtool-{a.version}"
    shutil.rmtree(stage, ignore_errors=True)
    tool = stage / "padtool"
    tool.mkdir(parents=True)

    for f in sorted((ROOT / "padtool").iterdir()):
        if f.is_file() and (f.suffix in (".py", ".pyw") or f.name == "README.md") and not f.name.startswith("test_"):
            shutil.copy2(f, tool / f.name)
    for f in LOADERS:
        if not f.is_file():
            sys.exit(f"padtool_dist: no {f} (git submodule update --init external/jieli-fw-AC63_BT_SDK)")
        shutil.copy2(f, tool / f.name)

    cache = a.build / "padtool-dist" / "wheels"
    merge([bleak_for(p, v, cache) for p in PLATFORMS for v in PYTHONS], tool / "lib")

    for board, names in BOARDS.items():
        for name in names:
            f = a.build / board / name
            if not f.is_file():
                sys.exit(f"padtool_dist: no {f} (make {board})")
            (stage / "firmware" / board).mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, stage / "firmware" / board / name)

    (stage / "padtool.cmd").write_text(PADTOOL_CMD, newline="")
    (stage / "padgui.pyw").write_text(PADGUI_PYW.format(extra=""))
    (stage / "padgui (no admin).pyw").write_text(PADGUI_PYW.format(extra='\nsys.argv.append("--no-elevate")'))
    (stage / "README.txt").write_text(README.format(version=a.version).replace("\n", "\r\n"), newline="")

    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".zip.tmp")
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(stage.rglob("*")):
            if f.is_file():
                z.write(f, f.relative_to(stage.parent))
    os.replace(tmp, out)
    print(f"{out}: {out.stat().st_size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
