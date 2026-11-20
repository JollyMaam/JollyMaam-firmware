"""A firmware package (jollymaam package; make firmware builds build/firmware.zip):
a folder with manifest.txt, or a zip of one.  manifest.txt v2:

    jollymaam-firmware 2
    NAME ROLE PATH SIZE SHA256        (NAME: a product ID, or BR23/BD19 for a loader)

The GUI reads it to know which pads it has firmware for, and takes the
update.bin from it for an upgrade over Bluetooth; everything on USB is
jollymaam's, given the package's path."""
import hashlib
import os
from pathlib import Path
import shutil
import sys
import zipfile

HERE = Path(__file__).resolve().parent


class Package:
    def __init__(self, path):
        self.path = Path(path)
        if self.path.is_dir():
            self._read = lambda name: (self.path / name).read_bytes()
        else:
            z = zipfile.ZipFile(self.path)
            self._read = z.read
        try:
            lines = self._read("manifest.txt").decode().splitlines()
        except (KeyError, OSError) as error:
            raise ValueError(f"{self.path}: not a firmware package (no manifest.txt)") from error
        if not lines or lines[0].split() != ["jollymaam-firmware", "2"]:
            raise ValueError(f"{self.path}: not a version 2 firmware package")
        self.files = {}
        for line in lines[1:]:
            parts = line.split()
            if len(parts) == 5:
                name, role, file, size, digest = parts
                self.files[name, role] = (file, int(size), digest)

    @property
    def products(self):
        """The product IDs it has install images for."""
        return sorted({name for name, role in self.files if role == "install"})

    def has(self, pid, role):
        return (pid, role) in self.files

    def read(self, pid, role):
        file, size, digest = self.files[pid, role]
        data = self._read(file)
        if len(data) != size or hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"{self.path}: {file} does not match its manifest")
        return data

    def __str__(self):
        return self.path.name if self.path.is_file() else str(self.path)


def jollymaam():
    """The jollymaam program, which installs, backs up, updates and restores
    over USB: $JOLLYMAAM, else beside padtool (the zip), else this tree's
    build, else on PATH; None if none."""
    exe = "jollymaam.exe" if sys.platform == "win32" else "jollymaam"
    if os.environ.get("JOLLYMAAM"):
        return os.environ["JOLLYMAAM"]
    root = HERE.parents[1]
    for path in (HERE / "bin" / exe, root / "build" / "host" / exe, root / "build" / "dist" / "bin" / exe):
        if path.is_file():
            if not os.access(path, os.X_OK):      # a zip unpacked without its modes
                path.chmod(path.stat().st_mode | 0o755)
            return str(path)
    return shutil.which("jollymaam")


def default():
    """The package that comes with this padtool (its firmware folder in the
    zip), else this tree's build/firmware.zip; None if neither is there."""
    for path in (HERE.parent / "firmware", HERE.parent / "firmware.zip",
                 HERE.parents[1] / "build" / "firmware.zip"):
        if (path / "manifest.txt").is_file() if path.is_dir() else path.is_file():
            try:
                return Package(path)
            except (ValueError, OSError, zipfile.BadZipFile):
                continue
    return None
