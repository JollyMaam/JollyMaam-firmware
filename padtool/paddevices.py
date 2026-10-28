"""Device discovery: a light look every second (our tool devices, and on
Linux new disks), everything on Refresh.  A device is read lightly when it
appears; its settings load when the GUI wants them, and again when its
settings change counter moves."""
import os
import sys
import threading
import time

import padhid
import padknown
import padtool as pad
from padtransport import tool_paths, usb_location


def read_device(path, name, identity):
    if identity is None:
        return {"path": path, "name": name, "board": "loader", "identity": None, "fields": [],
                "values": {}, "defaults": {}, "status": "UBOOT1.00 ROM loader", "location": usb_location(path)}
    board = next(board for board, number in pad.BOARD_IDS.items() if number == identity[6])
    status, changes = pad.status_info(path)
    # A pad's field table and targets: later, load_targets() (loaded: which are in).
    return {"path": path, "name": name, "board": board, "identity": identity[:56],
            "fields": None if board == "pad" else [], "values": {}, "defaults": {}, "loaded": set(),
            "status": status, "changes": changes, "location": usb_location(path),
            "product_id": identity[40:56].split(b"\0")[0].decode(errors="replace")}


def load_targets(path, table, targets):
    """The field table (table None: read it) and the targets' values and
    defaults: (table, values, defaults)."""
    if table is None:
        table = pad.fields(path)
    targets = [t for t in targets if t in pad.targets(table)]
    return (table, {t: pad.read_target(path, table, t) for t in targets},
            {t: pad.read_target(path, table, t, defaults=True) for t in targets})


def stock_records():
    """Stock pads and dongles known by their USB identity (padknown.STOCK),
    one per USB device.  Their identity names the product and the mode, so a
    mode change is a new device."""
    records = []
    for hid, entry in padknown.stock_devices(padhid.devices()):
        records.append({"path": hid["path"], "name": f"{hid['manufacturer']} {hid['product_name']}".strip(),
                        "board": "stock", "identity": f"stock:{entry.pid}:{entry.name}:{entry.mode}".encode(),
                        "fields": [], "values": {}, "defaults": {}, "status": entry.mode,
                        "location": hid["location"], "product_id": entry.pid, "stock": entry})
    return records


class DeviceMonitor:
    def __init__(self, events):
        self.events = events
        self.lock = threading.Lock()
        self.busy = threading.Event()
        self.stop = threading.Event()
        self.known = {}
        self.manual_paths = frozenset()
        self.generation = 0
        self.last_reported = None
        self.retry_after = {}
        self.disks = {}                  # Linux disk node: a ROM loader's identity (None) or False
        self.changes = {}                # pad path: its settings change counter, last seen

    def start(self, interval=1.0):
        """Look lightly every interval seconds, until stop."""
        def loop():
            last_error = None
            while not self.stop.wait(interval):
                try:
                    snapshot = self.poll_once(full=False)
                    last_error = None
                    if snapshot is not None:
                        self.events.put(("devices", snapshot))
                except (Exception, SystemExit) as error:
                    if str(error) != last_error:
                        self.events.put(("log", f"Device detection: {error}"))
                    last_error = str(error)
        threading.Thread(target=loop, daemon=True).start()

    def invalidate(self):
        self.generation += 1
        self.last_reported = None

    def refresh(self):
        """Look once, in the background; ends with a "refreshed" event."""
        def once():
            try:
                self.retry_after.clear()
                snapshot = self.poll_once(full=True)
                if snapshot is not None:
                    self.events.put(("devices", snapshot))
            except (Exception, SystemExit) as error:
                self.events.put(("log", f"Device detection: {error}"))
            self.events.put(("refreshed", None))
        threading.Thread(target=once, daemon=True).start()

    def paths(self, full):
        """What to look at: everything (full), else our tool devices, and on
        Linux the disks not seen before (a disk is asked only once, and the
        Windows drive scan is slow); disks found to be ROM loaders stay
        present while their node does."""
        if full:
            self.disks.clear()
            return sorted(set(pad.device_paths()) | self.manual_paths), []
        paths = set(tool_paths()) | self.manual_paths
        loaders = []
        if sys.platform == "linux":
            for disk in pad.device_paths():
                if pad.is_tool(disk):
                    continue
                if disk not in self.disks:
                    paths.add(disk)
                elif self.disks[disk] is None:
                    loaders.append(disk)
            for disk in list(self.disks):
                if not os.path.exists(disk):
                    del self.disks[disk]
        else:
            loaders = [p for p, identity in self.known.items() if identity is None and not pad.is_tool(p)]
        return sorted(paths), loaders

    def poll_once(self, full=True):
        """(generation, present, records, errors, changes): what is there, the
        devices new or changed since known, and the pads whose settings change
        counter moved, {path: (status, counter)}; None when nothing is new."""
        if self.busy.is_set():
            return None
        with self.lock:
            if self.busy.is_set():
                return None
            generation = self.generation
            paths, loaders = self.paths(full)
            found = list(pad.discover_devices(include_loaders=True, paths=paths))
            found += [(path, "ROM loader", None) for path in loaders if path not in {f[0] for f in found}]
            for path, name, identity in found:
                if not pad.is_tool(path):
                    self.disks[path] = None
            for path in paths:
                if not pad.is_tool(path) and path not in self.disks:
                    self.disks[path] = False
            present = {path: identity[:56] if identity is not None else None for path, name, identity in found}
            records, errors, changes = [], [], {}
            for path, identity in present.items():
                if identity is None or identity[6] != pad.BOARD_IDS["pad"] or self.known.get(path) != identity:
                    continue
                try:
                    status, counter = pad.status_info(path)
                except (Exception, SystemExit):
                    continue
                if self.changes.get(path) != counter:
                    self.changes[path] = counter
                    changes[path] = (status, counter)
            for path, name, identity in found:
                if self.busy.is_set() or generation != self.generation:
                    return None
                if path in self.known and self.known[path] == present[path]:
                    continue
                key = (path, present[path])
                if self.retry_after.get(key, 0) > time.monotonic():
                    continue
                try:
                    records.append(read_device(path, name, identity))
                except (Exception, SystemExit) as error:
                    errors.append(f"{path}: {error}")
                    self.retry_after[key] = time.monotonic() + 5
            try:
                stock = stock_records()
            except (Exception, SystemExit) as error:
                stock = []
                errors.append(f"HID devices: {error}")
            for record in stock:
                present[record["path"]] = record["identity"]
                if self.known.get(record["path"]) != record["identity"]:
                    records.append(record)
            if present == self.last_reported and not records and not errors and not changes:
                return None
            self.last_reported = present
            return generation, present, records, errors, changes
