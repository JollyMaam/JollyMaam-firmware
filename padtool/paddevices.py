"""Background device discovery: unchanged devices never reload their settings."""
import threading
import time

import padtool as pad
from padtransport import usb_location


def read_device(path, name, identity):
    if identity is None:
        return {"path": path, "name": name, "board": "loader", "identity": None, "fields": [],
                "values": {}, "defaults": {}, "status": "UBOOT1.00 ROM loader", "location": usb_location(path)}
    board = next(board for board, number in pad.BOARD_IDS.items() if number == identity[6])
    table = pad.fields(path) if board == "pad" else []
    values = {t: pad.read_target(path, table, t) for t in range(len(pad.TARGETS))} if table else {}
    defaults = {t: pad.read_target(path, table, t, defaults=True) for t in range(len(pad.TARGETS))} if table and int.from_bytes(identity[4:6], "little") >= 4 else {}
    return {"path": path, "name": name, "board": board, "identity": identity[:40], "fields": table,
            "values": values, "defaults": defaults, "status": pad.status(path), "location": usb_location(path)}


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
        self.thread = None

    def invalidate(self):
        self.generation += 1
        self.last_reported = None

    def start(self):
        if self.thread is None:
            self.thread = threading.Thread(target=self.watch, daemon=True)
            self.thread.start()

    def watch(self):
        while not self.stop.wait(1):
            try:
                snapshot = self.poll_once()
                if snapshot is not None:
                    self.events.put(("devices", snapshot))
            except (Exception, SystemExit) as error:
                self.events.put(("log", f"Device detection: {error}"))

    def poll_once(self):
        if self.busy.is_set():
            return None
        with self.lock:
            if self.busy.is_set():
                return None
            generation = self.generation
            paths = sorted(set(pad.device_paths()) | self.manual_paths)
            found = list(pad.discover_devices(include_loaders=True, paths=paths))
            present = {path: identity[:40] if identity is not None else None for path, name, identity in found}
            records, errors = [], []
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
            if present == self.last_reported and not records and not errors:
                return None
            self.last_reported = present
            return generation, present, records, errors
