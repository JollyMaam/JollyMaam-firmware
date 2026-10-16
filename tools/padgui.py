#!/usr/bin/env python3
"""Tkinter front end for padtool and jlnewfw. Run: python tools/padgui.py."""
import contextlib
import io
import os
from pathlib import Path
import queue
import shutil
import subprocess
import threading
import tempfile
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import padtool as pad
import padflash

ROOT = Path(__file__).resolve().parent.parent
SDK = Path(os.environ.get("JIELI_SDK", "/shared/github.com-Jieli-Tech/fw-AC63_BT_SDK")) / "cpu/br23/tools"
PIDS = padflash.PIDS


class App:
    def __init__(self, root):
        self.root = root
        root.title("Cyclone 2 · padtool")
        root.geometry("1040x850")
        root.minsize(900, 800)
        self.events = queue.Queue()
        self.busy = False
        self.table = []
        self.editors = {}
        self.board = tk.StringVar(value="pad")
        self.device = tk.StringVar()
        self.target = tk.StringVar(value=pad.TARGETS[0])
        self.dev = tk.BooleanVar()
        self.notice = tk.StringVar(value="Choose a board, then scan for devices.")
        top = ttk.Frame(root, padding=10)
        top.pack(fill="x")
        ttk.Label(top, text="Board").pack(side="left")
        self.boards = ttk.Combobox(top, textvariable=self.board, values=list(PIDS), state="readonly", width=9)
        self.boards.pack(side="left", padx=8)
        self.boards.bind("<<ComboboxSelected>>", self.change_board)
        self.devices = ttk.Combobox(top, textvariable=self.device, state="readonly", width=35)
        self.devices.pack(side="left", padx=8)
        self.devices.bind("<<ComboboxSelected>>", lambda e: self.clear_settings())
        ttk.Button(top, text="Scan", command=self.scan).pack(side="left")
        ttk.Button(top, text="Status", command=self.status).pack(side="left", padx=8)
        ttk.Label(root, textvariable=self.notice, padding=(10, 0, 10, 8), wraplength=980).pack(fill="x")
        self.tabs = ttk.Notebook(root)
        self.tabs.pack(fill="both", expand=True, padx=10)
        self.settings = ttk.Frame(self.tabs, padding=10)
        self.firmware = ttk.Frame(self.tabs, padding=10)
        self.diagnostics = ttk.Frame(self.tabs, padding=10)
        self.tabs.add(self.settings, text="Settings")
        self.tabs.add(self.firmware, text="Firmware")
        self.tabs.add(self.diagnostics, text="Diagnostics")
        self.build_settings()
        self.build_firmware()
        self.build_diagnostics()
        self.progress = ttk.Progressbar(root, mode="indeterminate")
        self.progress.pack(fill="x", padx=10, pady=6)
        self.output = tk.Text(root, height=9, wrap="word", state="disabled")
        self.output.pack(fill="both", padx=10, pady=(0, 10))
        root.protocol("WM_DELETE_WINDOW", self.close)
        self.poll = root.after(80, self.drain)
        self.change_board()

    def log(self, text):
        self.output.configure(state="normal")
        self.output.insert("end", text.rstrip() + "\n")
        if int(self.output.index("end-1c").split(".")[0]) > 5000:
            self.output.delete("1.0", "1000.0")
        self.output.see("end")
        self.output.configure(state="disabled")

    def controls(self, widget):
        for child in widget.winfo_children():
            if isinstance(child, (ttk.Button, ttk.Combobox, ttk.Entry, ttk.Checkbutton)):
                yield child
            yield from self.controls(child)

    def run(self, label, work, done=None):
        if self.busy:
            return
        self.busy = True
        self.notice.set(label)
        self.saved_states = [(w, str(w.cget("state"))) for w in self.controls(self.root)]
        for w, _ in self.saved_states:
            w.configure(state="disabled")
        self.progress.start()
        board = self.board.get()

        def worker():
            stream = io.StringIO()
            try:
                pad.BOARD = board
                with contextlib.redirect_stdout(stream):
                    result = work()
                self.events.put(("done", (result, done, stream.getvalue())))
            except (Exception, SystemExit) as error:
                self.events.put(("error", (str(error), stream.getvalue())))
        threading.Thread(target=worker, daemon=True).start()

    def drain(self):
        try:
            while True:
                kind, data = self.events.get_nowait()
                if kind == "log":
                    self.log(data)
                    continue
                self.busy = False
                self.progress.stop()
                for w, state in self.saved_states:
                    if w.winfo_exists():
                        w.configure(state=state)
                if kind == "error":
                    error, output = data
                    self.log(output + "\n" + error)
                    self.notice.set("Operation failed. See the log.")
                    messagebox.showerror("padtool", error)
                else:
                    result, done, output = data
                    if output:
                        self.log(output)
                    self.notice.set("Ready")
                    if done:
                        done(result)
        except queue.Empty:
            pass
        self.poll = self.root.after(80, self.drain)

    def close(self):
        if self.busy:
            messagebox.showinfo("Operation in progress", "Wait for the current operation to finish before closing.")
        else:
            self.root.after_cancel(self.poll)
            self.root.destroy()

    def clear_settings(self):
        self.table = []
        self.editors = {}
        for w in self.form.winfo_children():
            w.destroy()

    def change_board(self, event=None):
        self.device.set("")
        self.devices.configure(values=())
        self.clear_settings()
        self.bundle.set(str(ROOT / "build" / self.board.get()))
        self.elf.set(str(ROOT / "build" / self.board.get() / "app.elf"))
        if self.board.get() == "dongle":
            self.tabs.tab(self.settings, state="disabled")
            self.tabs.select(self.diagnostics)
            self.notice.set("Dongle: pairing, diagnostics, backup and flashing. Firmware exposes no settings table.")
        else:
            self.tabs.tab(self.settings, state="normal")
            self.notice.set("Pad: scan and choose a device. End an active USB gamepad session to access tool mode.")

    def path(self):
        path = self.device.get()
        if not path:
            messagebox.showinfo("Choose a device", "Scan and choose a running firmware device first.")
        return path

    def scan(self):
        def found(devices):
            paths = [p for p, name in devices]
            self.devices.configure(values=paths)
            self.device.set(paths[0] if paths else "")
            self.clear_settings()
            self.notice.set(f"Found {len(paths)} device(s)." if paths else
                            "No running firmware found. Check USB, tool mode and device permissions. Firmware operations can also find ROM loaders.")
            for path, name in devices:
                self.log(f"{path}: {name}")
        self.run("Scanning…", pad.devices, found)

    def status(self):
        path = self.path()
        if path:
            self.run("Reading status…", lambda: pad.identify(path) + "\n" + pad.status(path), self.log)

    def build_settings(self):
        bar = ttk.Frame(self.settings)
        bar.pack(fill="x")
        ttk.Combobox(bar, textvariable=self.target, values=pad.TARGETS, state="readonly", width=16).pack(side="left")
        ttk.Checkbutton(bar, text="Developer fields", variable=self.dev).pack(side="left", padx=8)
        ttk.Button(bar, text="Load", command=self.load_settings).pack(side="left")
        ttk.Button(bar, text="Apply changes", command=self.apply_settings).pack(side="left", padx=8)
        ttk.Label(self.settings, text="Load a target before editing. Use Reset per field to return it to its firmware default.").pack(anchor="w", pady=8)
        frame = ttk.Frame(self.settings)
        frame.pack(fill="both", expand=True)
        canvas = tk.Canvas(frame, highlightthickness=0)
        scroll = ttk.Scrollbar(frame, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)
        self.form = ttk.Frame(canvas)
        window = canvas.create_window((0, 0), window=self.form, anchor="nw")
        self.form.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>", lambda e: canvas.itemconfigure(window, width=e.width))
        bar = ttk.Frame(self.settings)
        bar.pack(fill="x", pady=(8, 0))
        ttk.Button(bar, text="Back up settings…", command=self.backup_settings).pack(side="left")
        ttk.Button(bar, text="Restore settings…", command=self.restore_settings).pack(side="left", padx=8)
        self.calibration = tk.BooleanVar()
        ttk.Checkbutton(bar, text="Include calibration on restore", variable=self.calibration).pack(side="left")

    def load_settings(self):
        path = self.path()
        if not path:
            return
        target = pad.target_index(self.target.get())
        developer = self.dev.get()
        def load():
            table = pad.fields(path)
            return table, pad.read_target(path, table, target)
        def display(result):
            self.clear_settings()
            self.table, values = result
            self.loaded = (path, target)
            for f in self.table:
                if f["name"] not in values or (f["dev"] and not developer) or f["kind"] != pad.kind_of(target):
                    continue
                raw, explicit = values[f["name"]]
                row = len(self.editors)
                value = pad.show_value(f, raw)
                var = tk.StringVar(value=value)
                reset = tk.BooleanVar()
                ttk.Label(self.form, text=f["name"]).grid(row=row, column=0, sticky="w", padx=4, pady=4)
                if f["type"] == pad.T_ENUM:
                    editor = ttk.Combobox(self.form, textvariable=var, values=f["names"], state="readonly", width=24)
                else:
                    editor = ttk.Entry(self.form, textvariable=var, width=26)
                editor.grid(row=row, column=1, sticky="ew", padx=4)
                ttk.Checkbutton(self.form, text="Reset", variable=reset).grid(row=row, column=2)
                ttk.Label(self.form, text=("Set · " if explicit else "Default · ") + f["help"], wraplength=440).grid(row=row, column=3, sticky="w", padx=8)
                self.editors[f["name"]] = (f, var, reset, value)
            self.form.columnconfigure(3, weight=1)
        self.run("Loading settings…", load, display)

    def apply_settings(self):
        path = self.path()
        target = pad.target_index(self.target.get())
        if not self.table or (path, target) != self.loaded:
            messagebox.showinfo("Load settings", "Load this device and target before applying changes.")
            return
        try:
            items = [(target, f["id"], b"" if reset.get() else pad.parse_value(f, var.get()))
                     for f, var, reset, original in self.editors.values()
                     if reset.get() or var.get() != original]
        except (Exception, SystemExit) as error:
            messagebox.showerror("Invalid setting", str(error))
            return
        if items:
            self.run("Applying settings…", lambda: pad.send(path, items), lambda _: self.load_settings())

    def backup_settings(self):
        path = self.path()
        if path:
            file = filedialog.asksaveasfilename(defaultextension=".json", filetypes=[("Settings", "*.json")])
            if file:
                self.run("Backing up settings…", lambda: pad.backup(path, file))

    def restore_settings(self):
        path = self.path()
        if not path:
            return
        file = filedialog.askopenfilename(filetypes=[("Settings", "*.json")])
        calibration = self.calibration.get()
        if file and messagebox.askyesno("Restore settings", "Replace saved settings and pairing data from this file?" +
                                       (" Calibration will also be restored." if calibration else " Calibration will be kept.")):
            self.run("Restoring settings…", lambda: pad.restore(path, file, calibration), lambda _: self.clear_settings())

    def file_row(self, parent, row, label, var, directory=False):
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", pady=5)
        ttk.Entry(parent, textvariable=var).grid(row=row, column=1, sticky="ew", padx=8)
        def browse():
            path = filedialog.askdirectory() if directory else filedialog.askopenfilename()
            if path:
                var.set(path)
        ttk.Button(parent, text="Browse…", command=browse).grid(row=row, column=2)

    def build_firmware(self):
        f = self.firmware
        f.columnconfigure(1, weight=1)
        ttk.Label(f, text="Full flash backup and verified firmware installation", font=("TkDefaultFont", 12, "bold")).grid(row=0, column=0, columnspan=3, sticky="w")
        ttk.Label(f, text="Python flasher with bundled BR23 loader. Operations refuse ambiguous loader matches.\n"
                  "If automatic loader entry fails: pad—hold Home + Start while plugging in USB; dongle—hold its button.\n"
                  "Backup leaves the device in its loader; reconnect USB when finished.", wraplength=940).grid(row=1, column=0, columnspan=3, sticky="w", pady=10)
        exe = ROOT / "build/host" / ("jlnewfw.exe" if os.name == "nt" else "jlnewfw")
        self.flasher = tk.StringVar(value=str(exe))
        self.loader = tk.StringVar(value=str(padflash.DEFAULT_LOADER))
        self.image = tk.StringVar()
        self.bundle = tk.StringVar(value=str(ROOT / "build/pad"))
        self.uboot = tk.StringVar(value=str(SDK / "uboot.boot"))
        self.cfgtool = tk.StringVar(value=str(SDK / "cfg_tool.bin"))
        self.file_row(f, 3, "BR23 loader", self.loader)
        self.file_row(f, 4, "Firmware image (.img / .ufw)", self.image)
        ttk.Button(f, text="Flash image…", command=lambda: self.flash(False)).grid(row=5, column=1, sticky="w", pady=8)
        ttk.Separator(f).grid(row=6, column=0, columnspan=3, sticky="ew", pady=8)
        ttk.Label(f, text="Optional: pack our build with jlnewfw for the connected chip's key:").grid(row=7, column=0, columnspan=3, sticky="w")
        self.file_row(f, 8, "jlnewfw (build packing only)", self.flasher)
        self.file_row(f, 9, "Build folder (app.bin + isd_config.ini)", self.bundle, True)
        self.file_row(f, 10, "SDK uboot.boot", self.uboot)
        self.file_row(f, 11, "SDK cfg_tool.bin", self.cfgtool)
        ttk.Button(f, text="Pack and flash build…", command=lambda: self.flash(True)).grid(row=12, column=1, sticky="w", pady=8)
        ttk.Button(f, text="Back up full flash…", command=self.backup_flash).grid(row=5, column=2, sticky="e", pady=8)
        ttk.Label(f, text="Flashing requires a backup destination and verifies every written byte. Image flashing enforces the chip key.\n"
                  "Select a build folder explicitly when switching boards. Settings in erased sectors are replaced by the image.", wraplength=940).grid(row=13, column=0, columnspan=3, sticky="w", pady=8)

    def loader_blob(self):
        blob = Path(self.loader.get()).read_bytes()
        if not blob:
            raise ValueError("Select a nonempty BR23 loader blob.")
        return blob

    def flash_report(self, message):
        self.events.put(("log", message))

    def invoke(self, args):
        self.events.put(("log", "Running: " + subprocess.list2cmdline(args)))
        with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              errors="replace", bufsize=1) as proc:
            for line in proc.stdout:
                self.events.put(("log", line))
            if proc.wait():
                raise RuntimeError(f"jlnewfw packing failed with status {proc.returncode}. See the log.")

    def backup_flash(self):
        try:
            blob = self.loader_blob()
        except (ValueError, OSError) as error:
            messagebox.showerror("Firmware", str(error))
            return
        board = self.board.get()
        file = filedialog.asksaveasfilename(defaultextension=".bin", initialfile=f"{board}-flash-backup.bin")
        if not file:
            return
        if Path(file).resolve() == Path(self.loader.get()).resolve():
            messagebox.showerror("Backup destination", "Choose a file different from the BR23 loader.")
            return
        def backup():
            loader = padflash.select_loader(board, blob, report=self.flash_report)
            loader.backup(file)
            self.flash_report("Backup complete. Reconnect USB to leave the loader.")
        self.run("Backing up flash…", backup, lambda _: self.clear_settings())

    def flash(self, pack):
        try:
            blob = self.loader_blob()
            board = self.board.get()
            if pack:
                exe = shutil.which(self.flasher.get()) or self.flasher.get()
                bundle = Path(self.bundle.get())
                files = [bundle / "app.bin", bundle / "isd_config.ini", Path(self.uboot.get()), Path(self.cfgtool.get())]
                if not Path(exe).is_file() or not all(p.is_file() for p in files):
                    raise ValueError("Build packing needs jlnewfw, app.bin, isd_config.ini, uboot.boot and cfg_tool.bin.")
                pack_args = [exe, "pack", "--flash-size", "0x100000" if board == "pad" else "0x80000",
                             "--code-limit", "0x50000", "-c", str(files[1]), "-u", str(files[2]), str(files[0]), str(files[3])]
            else:
                image = Path(self.image.get())
                if not image.is_file():
                    raise ValueError("Select an existing firmware image.")
                files = [image]
        except (ValueError, OSError) as error:
            messagebox.showerror("Firmware", str(error))
            return
        backup = filedialog.asksaveasfilename(title="Save automatic pre-flash backup", defaultextension=".bin", initialfile=f"{board}-before-flash.bin")
        if not backup:
            return
        inputs = [p.resolve() for p in files + [Path(self.loader.get())]]
        if pack:
            inputs.append(Path(exe).resolve())
        if Path(backup).resolve() in inputs:
            messagebox.showerror("Backup destination", "Choose a backup file different from the firmware input files.")
            return
        if not messagebox.askyesno("Flash firmware", f"Back up and flash the {board}?\n\n"
                                  "The flasher selects by firmware PID and chip key. Connect only the intended board. "
                                  "Settings in erased sectors will be replaced. Keep USB connected until verification finishes."):
            return
        def flash():
            with tempfile.TemporaryDirectory(prefix="padtool-") as temp:
                packed = Path(temp) / "firmware.img"
                if pack:
                    self.invoke(pack_args + ["-k", "0xffff", "-o", str(packed)])
                data = padflash.load_image(packed if pack else image)
                pid, key = padflash.image_identity(data)
                if pid != PIDS[board]:
                    raise ValueError("Image does not match the selected board")
                loader = padflash.select_loader(board, blob, report=self.flash_report)
                if pack and key != loader.key:
                    self.invoke(pack_args + ["-k", hex(loader.key), "-o", str(packed)])
                    data = padflash.load_image(packed)
                loader.flash(data, board, backup)
        self.run("Backing up and flashing…", flash, lambda _: self.clear_settings())

    def build_diagnostics(self):
        f = self.diagnostics
        ttk.Label(f, text="Operations use the selected running firmware device.").pack(anchor="w", pady=6)
        bar = ttk.Frame(f)
        bar.pack(fill="x")
        for title, function in [("Read log", lambda p: pad.log(p, False)),
                                ("Statistics (pad)", lambda p: pad.show_stats(pad.read_stats(p))),
                                ("Timing trace", lambda p: pad.trace(p, 0.5, 40))]:
            ttk.Button(bar, text=title, command=lambda fn=function, name=title: self.diagnostic(name, fn)).pack(side="left", padx=(0, 8))
        bar = ttk.Frame(f)
        bar.pack(fill="x", pady=12)
        self.slot = tk.StringVar(value=pad.SLOT_NAMES[0])
        ttk.Combobox(bar, textvariable=self.slot, values=pad.SLOT_NAMES, state="readonly", width=10).pack(side="left", padx=(0, 8))
        ttk.Button(bar, text="Connect slot (pad)", command=self.connect).pack(side="left", padx=(0, 8))
        ttk.Button(bar, text="Pair…", command=self.pair).pack(side="left")
        bar = ttk.Frame(f)
        bar.pack(fill="x", pady=8)
        ttk.Button(bar, text="Reboot", command=lambda: self.diagnostic("Reboot", lambda p: pad.cmd(p, 0x10))).pack(side="left")
        ttk.Button(bar, text="Enter ROM loader", command=lambda: self.diagnostic("Enter loader", lambda p: pad.cmd(p, 0x11))).pack(side="left", padx=8)
        ttk.Button(bar, text="Tool mode (Linux HID)", command=lambda: self.run("Switching to tool mode…", lambda: pad.tool([]))).pack(side="left")
        targetbar = ttk.Frame(f)
        targetbar.pack(fill="x", pady=8)
        self.address = tk.StringVar()
        ttk.Label(targetbar, text="Dongle target address").pack(side="left")
        ttk.Entry(targetbar, textvariable=self.address, width=24).pack(side="left", padx=8)
        ttk.Button(targetbar, text="Set target", command=self.set_dongle_target).pack(side="left")
        advanced = ttk.LabelFrame(f, text="CPU profiling · requires pyelftools", padding=8)
        advanced.pack(fill="x", pady=12)
        advanced.columnconfigure(1, weight=1)
        self.elf = tk.StringVar(value=str(ROOT / "build/pad/app.elf"))
        self.file_row(advanced, 0, "Running build ELF", self.elf)
        ttk.Button(advanced, text="Profile for 3 seconds", command=self.profile).grid(row=1, column=1, sticky="w", pady=8)
        ttk.Label(f, text="Pairing replaces the selected slot's host after a successful pairing. The dongle has one bond.\n"
                  "End USB gamepad sessions to expose tool mode; then scan again. Logs appear below.", wraplength=900).pack(anchor="w", pady=12)

    def set_dongle_target(self):
        if self.board.get() != "dongle":
            messagebox.showinfo("Dongle only", "Select a dongle to set its target address.")
            return
        try:
            parts = self.address.get().split(":")
            if len(parts) != 6 or any(len(p) != 2 for p in parts):
                raise ValueError
            address = bytes(int(p, 16) for p in parts)
        except ValueError:
            messagebox.showerror("Address", "Use AA:BB:CC:DD:EE:FF.")
            return
        self.diagnostic("Set dongle target", lambda p: pad.scsi(p, bytes([pad.OP, 0x30]) + address, 0))

    def profile(self):
        elf = self.elf.get()
        self.diagnostic("CPU profile", lambda p: pad.profile(p, 3, 2000, 40, elf))

    def diagnostic(self, label, fn):
        if label == "Statistics (pad)" and self.board.get() != "pad":
            messagebox.showinfo("Unavailable", "Dongle firmware does not expose pad statistics.")
            return
        path = self.path()
        if path:
            self.run(label + "…", lambda: fn(path))

    def connect(self):
        if self.board.get() == "pad":
            slot = pad.SLOT_NAMES.index(self.slot.get())
            self.diagnostic("Connect", lambda p: pad.cmd(p, 0x13, slot))

    def pair(self):
        board, slot = self.board.get(), pad.SLOT_NAMES.index(self.slot.get())
        if messagebox.askyesno("Pair", "Make this slot pairable? The dongle forgets its existing bond immediately; a pad slot replaces its host when pairing succeeds."):
            self.diagnostic("Pair", lambda p: pad.cmd(p, 0x17, slot if board == "pad" else 0))


if __name__ == "__main__":
    App(tk.Tk()).root.mainloop()
