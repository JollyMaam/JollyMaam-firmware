"""Per-device live console windows; USB reads share the app's operation lock."""
import queue
import threading
import time
import tkinter as tk
from tkinter import ttk

import padtool as pad


def read_console(path, identity, seq, previous_uptime):
    found = next(pad.discover_devices(paths=[path]), None)
    if found is None or found[2][:40] != identity:
        raise OSError("Device disconnected or its path now belongs to another device")
    status = pad.cmd(path, 0x01, length=16)
    if len(status) < 12:
        raise ValueError("Short device status")
    uptime = int.from_bytes(status[8:12], "little")
    restarted = previous_uptime is not None and uptime < previous_uptime
    if restarted:
        seq = 1
    first, text = pad.read_log(path, seq)
    return first, text, uptime, restarted, seq


class LiveConsole:
    def __init__(self, root, monitor, identity, board, resolve, on_close):
        self.monitor, self.identity = monitor, identity
        self.resolve, self.on_close = resolve, on_close
        self.events = queue.Queue()
        self.closed = self.reading = False
        self.seq, self.uptime = 1, None
        self.next_read = 0
        self.connection = None
        self.window = tk.Toplevel(root)
        self.window.title(f"{board.title()} console")
        self.window.geometry("850x450")
        self.window.protocol("WM_DELETE_WINDOW", self.close)
        self.status = ttk.Label(self.window, text="Connecting…", padding=6)
        self.status.pack(fill="x")
        frame = ttk.Frame(self.window)
        frame.pack(fill="both", expand=True)
        self.output = tk.Text(frame, state="disabled", wrap="none", font="TkFixedFont")
        vertical = ttk.Scrollbar(frame, orient="vertical", command=self.output.yview)
        horizontal = ttk.Scrollbar(frame, orient="horizontal", command=self.output.xview)
        self.output.configure(yscrollcommand=vertical.set, xscrollcommand=horizontal.set)
        frame.rowconfigure(0, weight=1)
        frame.columnconfigure(0, weight=1)
        self.output.grid(row=0, column=0, sticky="nsew")
        vertical.grid(row=0, column=1, sticky="ns")
        horizontal.grid(row=1, column=0, sticky="ew")
        self.poll = self.window.after(0, self.tick)

    def append(self, text):
        follow = self.output.yview()[1] >= .999
        self.output.configure(state="normal")
        self.output.insert("end", text)
        if int(self.output.index("end-1c").split(".")[0]) > 5000:
            self.output.delete("1.0", "1000.0")
        if follow:
            self.output.see("end")
        self.output.configure(state="disabled")

    def tick(self):
        if self.closed:
            return
        try:
            while True:
                path, result, error = self.events.get_nowait()
                self.reading = False
                if error:
                    self.status.configure(text=f"Waiting for device · {error}")
                    if self.connection is not None:
                        self.append("\n-- Device disconnected; waiting --\n")
                    self.connection = None
                elif result is not None:
                    first, text, uptime, restarted, requested = result
                    if self.connection != path:
                        self.append(f"\n-- Connected · {path} --\n")
                    self.connection = path
                    self.status.configure(text=f"Live · {path}")
                    if restarted:
                        self.append("\n-- Device restarted --\n")
                    if first > requested and requested > 1:
                        self.append(f"-- {first - requested} log lines lost --\n")
                    self.append(text)
                    self.seq = first + len(text.splitlines())
                    self.uptime = uptime
                    if text:
                        self.next_read = 0  # Drain a backlog without waiting a full interval.
        except queue.Empty:
            pass
        if not self.reading and time.monotonic() >= self.next_read:
            path = self.resolve(self.identity)
            if self.monitor.busy.is_set():
                self.status.configure(text="Paused during device operation")
            elif path is None:
                self.status.configure(text="Device disconnected · waiting")
                if self.connection is not None:
                    self.append("\n-- Device disconnected; waiting --\n")
                self.connection = None
            else:
                self.reading = True
                self.next_read = time.monotonic() + .25
                # Capture only non-Tk objects: a late worker never owns the window.
                events, monitor = self.events, self.monitor
                identity, seq, uptime = self.identity, self.seq, self.uptime

                def read():
                    result = error = None
                    try:
                        with monitor.lock:
                            if not monitor.busy.is_set() and not monitor.stop.is_set():
                                result = read_console(path, identity, seq, uptime)
                    except (Exception, SystemExit) as failure:
                        error = str(failure)
                    events.put((path, result, error))

                threading.Thread(target=read, daemon=True).start()
        self.poll = self.window.after(80, self.tick)

    def close(self):
        if not self.closed:
            self.closed = True
            self.window.after_cancel(self.poll)
            self.on_close()
            self.window.destroy()
