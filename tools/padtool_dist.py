#!/usr/bin/env python3
"""Package padtool for Windows: make padtool_dist.

    tools/padtool_dist.py --build build --version ID --out build/padtool-ID.zip

The zip holds padtool (CLI and GUI, without its tests), the Jieli RAM
loaders the GUI's flasher needs (br23loader.bin, bd19loader.bin, beside
padflash.py, where it looks first), bleak and its WinRT backend for 64-bit
Windows Python 3.11-3.14 (padtool/lib, which padble puts on the path on
Windows), and every board's firmware from the build.  bleak comes from PyPI
through uv (cross-platform resolution; pip judges the markers by this
machine), cached in the build directory.

Python itself is not included: python.org's 64-bit installer, with Tk."""
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
PLATFORM = "x86_64-pc-windows-msvc"
SDK = ROOT / "external" / "jieli-fw-AC63_BT_SDK"
LOADERS = [SDK / "cpu" / "br23" / "tools" / "br23loader.bin", SDK / "cpu" / "bd19" / "tools" / "bd19loader.bin"]
# Board build directory: the files a user flashes (firmware.img/.bin through
# a ROM or boot loader, update.bin through the updater: padtool update).
BOARDS = {
    "gamesir-cyclone-2-pad": ["firmware.img", "update.bin"],
    "gamesir-cyclone-2-dongle": ["firmware.img", "update.bin"],
    "gamesir-nova-2-lite-pad": ["firmware.img", "update.bin"],
    "gamesir-nova-2-lite-dongle": ["firmware.img", "update.bin"],
    "8bitdo-ultimate2c-wireless-pad": ["firmware.bin", "update.bin"],
}

# py (the launcher) if there is one, else python; the GUI through .pyw's
# file association (pyw is not always on the path).
PADTOOL_CMD = ('@echo off\r\nwhere py >nul 2>nul\r\n'
               'if not errorlevel 1 (py -3 "%~dp0padtool\\padtool.py" %*) else (python "%~dp0padtool\\padtool.py" %*)\r\n')
PADGUI_CMD = '@start "" "%~dp0padtool\\padgui.pyw" %*\r\n'
PADGUI_NOADMIN_CMD = '@start "" "%~dp0padtool\\padgui.pyw" --no-elevate\r\n'
README = """padtool {version} for Windows

Needs 64-bit Python 3.11 to 3.14 from python.org (with "tcl/tk" and the
"py launcher", both on by default).  Nothing to install: bleak, for
Bluetooth, is in padtool\\lib.

  padgui.cmd               the GUI (or double-click padtool\\padgui.pyw); it
                           asks for admin, for the ROM loader's disk
  padgui (no admin).cmd    the GUI without asking: USB tool devices and
                           Bluetooth; whether the ROM loader works so is
                           not established
  padtool.cmd --help       the command line, from a Command Prompt here

Bluetooth: hold Home+M on the pad for 3 seconds (config mode), then
Refresh in the GUI, or put --ble after a command:  padtool.cmd get --ble

firmware\\<board>\\ holds this build's firmware:
  update.bin     padtool.cmd update firmware\\<board>\\update.bin
                 (a pad or dongle already running this firmware, on USB)
  firmware.img   the whole flash, for the GUI's flasher (UBOOT1.00 loader)
  firmware.bin   the 8BitDo's whole image, for its boot loader

padtool\\README.md has the rest.
"""


def bleak_for(version, cache):
    """bleak and its WinRT backend for one Windows Python, installed into
    the cache once."""
    target = cache / f"cp{version.replace('.', '')}"
    if not (target / "bleak").is_dir():
        uv = shutil.which("uv")
        if not uv:
            sys.exit("padtool_dist: needs uv to fetch bleak for Windows (nix-shell -p uv, or pip install uv)")
        shutil.rmtree(target, ignore_errors=True)
        subprocess.run([uv, "pip", "install", "--quiet", "--python-platform", PLATFORM, "--python-version", version,
                        "--only-binary", ":all:", "--target", str(target), BLEAK], check=True)
    return target


def compiled(folder):
    return {p.relative_to(folder) for p in folder.rglob("*.pyd")}


def same_but_compiled(a, b):
    """The two trees are the same but for their .pyd files (and the wheels'
    records, which name them)."""
    files = lambda f: {p.relative_to(f) for p in f.rglob("*") if p.is_file() and p.suffix != ".pyd"
                       and "__pycache__" not in p.parts and not p.parts[len(f.parts)].endswith(".dist-info")}
    fa, fb = files(a), files(b)
    return fa == fb and all(filecmp.cmp(a / p, b / p, shallow=False) for p in fa)


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

    # One tree of bleak and winrt, and each Python's own compiled module.
    cache = a.build / "padtool-dist" / "wheels"
    trees = [bleak_for(v, cache) for v in PYTHONS]
    for t in trees[1:]:
        if not same_but_compiled(trees[0], t):
            sys.exit(f"padtool_dist: {t.name} differs from {trees[0].name} beyond its .pyd files")
    lib = tool / "lib"
    shutil.copytree(trees[0], lib, ignore=shutil.ignore_patterns("__pycache__", "*.pyd"))
    for t in trees:
        for rel in compiled(t):
            (lib / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(t / rel, lib / rel)

    for board, names in BOARDS.items():
        for name in names:
            f = a.build / board / name
            if not f.is_file():
                sys.exit(f"padtool_dist: no {f} (make {board})")
            (stage / "firmware" / board).mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, stage / "firmware" / board / name)

    (stage / "padtool.cmd").write_text(PADTOOL_CMD, newline="")
    (stage / "padgui.cmd").write_text(PADGUI_CMD, newline="")
    (stage / "padgui (no admin).cmd").write_text(PADGUI_NOADMIN_CMD, newline="")
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
