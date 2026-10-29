#!/usr/bin/env python3
"""Install this firmware on a pad or dongle over USB: it waits for one, says
what it is, asks, and writes the firmware.img for it from the zip's
firmware folder (or this tree's build).  For the first flash from stock
firmware; on this firmware already it reinstalls it.

    sudo python3 padtool/padinstall.py     Linux (root, or udev rules for
                                           every device it uses)
    Install firmware.py                    Windows, in the zip: asks for
                                           admin (the ROM loader is a disk)

GameSir Cyclone 2, Nova 2 Lite and their dongles (Jieli): from stock
firmware in a USB mode with a command for the ROM loader, from this
firmware, or from the ROM loader (UBOOT1.00) entered by hand, after a
backup of the whole flash if wanted.  8BitDo Ultimate 2C Wireless: from
stock firmware, this firmware or its boot loader.  Only the image's sectors
are written: settings and pairings, past it, stay."""
import argparse
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pad8bitdo  # noqa: E402
import padflash  # noqa: E402
import padhid  # noqa: E402
import padknown  # noqa: E402
import padtool as pad  # noqa: E402
from padknown import BUILDS, PRODUCTS, command_mode, firmware_dir, product_name, stock_name  # noqa: E402
from padtransport import usb_location  # noqa: E402

HERE = Path(__file__).resolve().parent
BACKUPS = HERE.parent / "backups"
POLL = 2.0 if sys.platform == "win32" else 0.5     # asking every Windows drive is slow
BOOT_HINT = "USB unplugged, hold L1+R1, tap Home, plug in USB"


def image_for(pid):
    """This package's firmware.img for a product, or None."""
    folder = firmware_dir(pid)
    if pid not in BUILDS or not folder or Path(folder).name != BUILDS[pid]:
        return None
    file = Path(folder) / "firmware.img"
    return file if file.is_file() else None


class Found:
    """A device on USB: what it is (kind: stock, ours, rom, stock8, ours8,
    boot), its product, and how to tell it again (key)."""
    def __init__(self, kind, key, pid, what, path, location=None, entry=None):
        self.kind, self.key, self.pid, self.what = kind, key, pid, what
        self.path, self.location, self.entry = path, location, entry


class Scanner:
    """What is on USB.  A device is asked what it is once (a ROM loader has
    the RAM loader uploaded for it then), while it stays."""
    def __init__(self):
        self.asked = {}                  # path: Found or None (not ours to handle)

    def ask(self, path, fn):
        if path not in self.asked:
            try:
                self.asked[path] = fn()
            except (OSError, ValueError, SystemExit):
                self.asked[path] = None
        return self.asked[path]

    def look(self):
        found, seen = [], set()
        for path, name, identity in pad.discover_devices(include_loaders=True):
            seen.add(path)
            if identity is None:
                found.append(self.ask(path, lambda: self.rom(path)))
                continue
            pid = identity[40:56].split(b"\0")[0].decode(errors="replace")
            build = identity[8:24].split(b"\0")[0].decode(errors="replace")
            board = next((b for b, n in pad.BOARD_IDS.items() if n == identity[6]), "")
            if identity[7] == 1:
                continue                 # its updater: it goes on by itself
            kind = "ours8" if pid in pad8bitdo.MODELS else "ours"
            found.append(Found(kind, "id:" + identity[24:40].hex(), pid,
                               f"{product_name(pid)} ({board}), this firmware, build {build}", path, usb_location(path)))
        hids = padhid.devices()
        for hid, entry in padknown.stock_devices(hids):
            seen.add(hid["path"])
            mode = f", {entry.mode} mode" if entry.mode else ""
            found.append(Found("stock", hid["path"], entry.pid, f"{stock_name(entry)}, stock firmware{mode}",
                               hid["path"], hid["location"], entry))
        for hid in hids:
            path = hid["path"]
            if (hid["vid"], hid["product"]) == pad8bitdo.LOADER:
                seen.add(path)
                found.append(self.ask(path, lambda: self.boot(hid)))
            elif hid["vid"] == 0x2dc8 and hid["product"] in pad8bitdo.STOCK_PIDS:
                seen.add(path)
                found.append(self.ask(path, lambda: self.stock8(hid)))
        for path in list(self.asked):
            if path not in seen:
                self.asked.pop(path)
        return [f for f in found if f]

    def rom(self, path):
        loader = padflash.inspect_loader(path, padflash.DEFAULT_LOADER, report=lambda m: None)
        pid = loader.product_id
        found = Found("rom", path, pid, f"{product_name(pid) if pid else 'a pad or dongle'} in its ROM loader (UBOOT1.00)",
                      path, usb_location(path))
        found.loader = loader
        return found

    def boot(self, hid):
        with pad8bitdo.Loader(hid["path"]) as loader:
            pid = pad8bitdo.model_pid(loader.identify())
        return Found("boot", hid["path"], pid, f"{product_name(pid)} in the 8BitDo boot loader", hid["path"], hid["location"])

    def stock8(self, hid):
        found = pad8bitdo.find_stock([hid])
        if not found:
            return None                  # not its vendor interface
        path, location, pid = found[0]
        return Found("stock8", path, pid, f"8BitDo {product_name(pid)}, stock firmware", path, location)


def ask(question, default=False):
    answer = input(f"{question} [{'Y/n' if default else 'y/N'}] ").strip().lower()
    return default if not answer else answer.startswith("y")


class Bar:
    """One line of progress: the stage and a bar."""
    def __init__(self):
        self.stage = None

    def __call__(self, stage, done=0, total=None):
        if stage != self.stage and self.stage is not None:
            print()
        self.stage = stage
        if total:
            filled = int(30 * done / total)
            print(f"\r  {stage:<26} [{'#' * filled}{'.' * (30 - filled)}] {done * 100 // total:3d}%", end="", flush=True)
        else:
            print(f"\r  {stage}…", end="", flush=True)

    def end(self):
        if self.stage is not None:
            print()
        self.stage = None


def wait_for(fn, seconds, what):
    """fn() until it gives something, for that long."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = fn()
        if result:
            return result
        time.sleep(0.25)
    raise OSError(f"{what} did not appear within {seconds} s")


def pick(found, old, location):
    """The one that appeared: on the device's USB port when it is known, else
    the only new one."""
    matches = [p for p, loc in found if loc == location] if location else [p for p, loc in found if p not in old]
    if len(matches) > 1:
        raise OSError("more than one loader appeared: connect one device at a time")
    return matches[0] if matches else None


def rom_paths():
    return [(p, usb_location(p)) for p, name, identity in pad.discover_devices(include_loaders=True) if identity is None]


def boot_paths(pid):
    return [(p, loc) for p, loc, model in pad8bitdo.find() if model == pid]


def install_jieli(device, image, bar):
    data = padflash.load_image(image)
    if padflash.image_identity(data)[0] != device.pid:
        raise ValueError(f"{image} is not for the {product_name(device.pid)}")
    backup = None
    if ask("Back up its whole flash first" + (" (the stock firmware)" if device.kind == "stock" else "") + "?",
           default=device.kind != "ours"):
        BACKUPS.mkdir(exist_ok=True)
        backup = BACKUPS / f"{device.pid}-{time.strftime('%Y%m%d-%H%M%S')}.bin"
    if device.kind == "rom":
        loader = device.loader
    else:
        old = {p for p, loc in rom_paths()}
        bar("Restarting into the ROM loader")
        if device.kind == "stock":
            padhid.write(device.path, device.entry.report.ljust(device.entry.length, b"\0"))
        else:
            try:
                pad.cmd(device.path, 0x11)
            except OSError:
                pass                     # it may go before it answers
        hint = PRODUCTS[device.pid].by_hand if device.pid in PRODUCTS else ""
        path = wait_for(lambda: pick(rom_paths(), old, device.location), 30,
                        "The ROM loader" + (f" (by hand: {hint})" if hint else ""))
        loader = padflash.inspect_loader(path, padflash.DEFAULT_LOADER, report=lambda m: None)
    loader.progress = bar
    loader.flash(data, backup)
    bar.end()
    if backup:
        print(f"  Backup: {backup}")


def install_8bitdo(device, image, bar):
    data = image.read_bytes()
    if pad8bitdo.image_pid(data) != device.pid:
        raise ValueError(f"{image} is not for the {product_name(device.pid)}")
    path = device.path
    if device.kind != "boot":
        old = {p for p, loc in boot_paths(device.pid)}
        bar("Restarting into the boot loader")
        if device.kind == "stock8":
            with pad8bitdo.Stock(device.path) as stock:
                stock.reboot_loader()
        else:
            try:
                pad.cmd(device.path, 0x11)
            except OSError:
                pass
        path = wait_for(lambda: pick(boot_paths(device.pid), old, device.location), 30,
                        f"The boot loader (by hand: {BOOT_HINT})")
    pad8bitdo.flash(path, data, lambda done, total: bar("Writing firmware", done, total))
    bar.end()


def back(scanner, device, seconds=30):
    """The device on this firmware again: its Found, or None."""
    def ours():
        return next((f for f in scanner.look() if f.kind in ("ours", "ours8") and f.pid == device.pid and
                     (not device.location or f.location == device.location)), None)
    try:
        return wait_for(ours, seconds, "it")
    except OSError:
        return None


def run():
    sys.stdout.reconfigure(line_buffering=True)    # each line as it is told, piped too
    print(f"padinstall: this firmware for {', '.join(product_name(p) for p in BUILDS if image_for(p))}")
    if not any(image_for(p) for p in BUILDS):
        sys.exit("padinstall: no firmware.img found (the zip's firmware folder, or this tree's build: make)")
    if sys.platform == "linux" and os.geteuid() != 0:
        print("Not root: only devices you may open are seen (run it with sudo if nothing appears).")
    print("Connect a pad or dongle by USB. Ctrl-C quits.\n")
    scanner, declined, done, told = Scanner(), set(), set(), set()
    waiting = True
    while True:
        found = scanner.look()
        keys = {f.key for f in found}
        declined &= keys                 # gone: asked again when it comes back
        told &= keys
        new = [f for f in found if f.key not in declined | done]
        if not new:
            if not waiting:
                print("Waiting for a pad or dongle…")
                waiting = True
            time.sleep(POLL)
            continue
        waiting = False
        device = new[0]
        print(f"Found: {device.what}" + (f" (USB {device.location})" if device.location else ""))
        image = image_for(device.pid) if device.pid else None
        if device.kind == "stock" and not device.entry.usage_page:
            mode = command_mode(device.pid)
            hint = PRODUCTS[device.pid].by_hand if device.pid in PRODUCTS else ""
            print(f"  This mode has no command for its loader: switch it to {mode} mode." if mode else
                  f"  It has no command for its loader; enter it by hand: {hint}")
            declined.add(device.key)
            continue
        if not image:
            print(f"  No firmware here for {product_name(device.pid) if device.pid else 'it'}; leaving it as it is.")
            declined.add(device.key)
            continue
        jieli = device.kind in ("stock", "ours", "rom")
        note = " (settings and pairings stay)" if device.kind in ("ours", "ours8") else ""
        if not ask(f"Install {image.parent.name}/firmware.img on it?{note}"):
            declined.add(device.key)
            print()
            continue
        bar = Bar()
        try:
            (install_jieli if jieli else install_8bitdo)(device, image, bar)
        except (OSError, ValueError) as error:
            bar.end()
            print(f"  Failed: {error}")
            print("  Nothing more is done to it. Unplug it, or Ctrl-C.\n")
            declined.add(device.key)
            continue
        print("  Written and verified; waiting for it to start…")
        again = back(scanner, device)
        if again:
            done.add(again.key)
            print(f"  Done: {again.what}.\n")
        else:
            print("  Done, but it did not come back on USB within 30 s: unplug it and plug it in again.\n")
        done.add(device.key)


def elevate():
    """Windows: the ROM loader is a disk, which needs admin; start again
    elevated in a new console (the UAC prompt).  True: that one runs now."""
    import ctypes
    import subprocess
    if ctypes.windll.shell32.IsUserAnAdmin():
        return False
    params = subprocess.list2cmdline([str(Path(__file__).resolve()), *sys.argv[1:], "--elevated"])
    if ctypes.windll.shell32.ShellExecuteW(None, "runas", sys.executable, params, str(HERE.parent), 1) > 32:
        return True
    print("padinstall: not elevated (declined?); the ROM loader may not be seen.")
    return False


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--no-elevate", action="store_true", help="Windows: do not ask for admin")
    p.add_argument("--elevated", action="store_true", help=argparse.SUPPRESS)
    a = p.parse_args()
    if sys.platform == "win32" and not a.no_elevate and not a.elevated and elevate():
        return
    try:
        run()
    except KeyboardInterrupt:
        print()
    except SystemExit as error:
        print(error)
    finally:
        if a.elevated:                   # its own console closes with it
            input("Press Enter to close.")


if __name__ == "__main__":
    main()
