#!/usr/bin/env python3
"""Tkinter front end for padtool and the Python BR23 flasher. Run: python padtool/padgui.pyw."""
import contextlib
import io
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import threading
import tempfile
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

import padtool as pad
import padflash
from padwidgets import PaletteEditor, CurveEditor

ROOT = Path(__file__).resolve().parent.parent
SDK = Path(os.environ.get("JIELI_SDK", "/shared/github.com-Jieli-Tech/fw-AC63_BT_SDK")) / "cpu/br23/tools"
PIDS = padflash.PIDS


class App:
    def __init__(self, root, auto_scan=True):
        self.root = root
        root.title("Cyclone 2 · padtool")
        root.geometry("1250x850")
        root.minsize(1120, 800)
        self.events = queue.Queue()
        self.busy = False
        self.scroll_canvases = {}
        self.active_page = None
        self.cache = {}
        self.nodes = {}
        self.editors = {}
        self.indicators = []
        self.swatches = []
        self.special_editors = []
        self.apply_buttons = []
        self.board = tk.StringVar(value="pad")
        self.device = tk.StringVar()
        self.target = tk.StringVar(value=pad.TARGETS[0])
        self.expanded_details = set()
        self.notice = tk.StringVar()
        self.pending = tk.StringVar(value="No pending changes")
        self.summary = tk.StringVar()
        self.notice.trace_add("write", lambda *_: self.update_summary())
        self.pending.trace_add("write", lambda *_: self.update_summary())
        self.update_summary()
        top = ttk.Frame(root, padding=10)
        top.pack(fill="x")
        ttk.Button(top, text="Refresh", command=self.scan).pack(side="left")
        ttk.Button(top, text="Apply", command=self.apply_settings).pack(side="left", padx=8)
        ttk.Button(top, text="Discard", command=self.discard_changes).pack(side="left")
        ttk.Label(top, textvariable=self.summary, wraplength=750).pack(side="left", padx=10, fill="x", expand=True)
        pane = ttk.Panedwindow(root, orient="horizontal")
        pane.pack(fill="both", expand=True, padx=10)
        navigation = ttk.Frame(pane, width=250)
        pane.add(navigation, weight=0)
        ttk.Label(navigation, text="Devices and slots", padding=6).pack(anchor="w")
        self.tree = ttk.Treeview(navigation, show="tree", selectmode="browse", height=22)
        scrollbar = ttk.Scrollbar(navigation, orient="vertical", command=self.tree.yview)
        scrollbar.pack(side="right", fill="y")
        self.tree.configure(yscrollcommand=scrollbar.set)
        self.tree.pack(fill="both", expand=True)
        self.tree.tag_configure("dirty", foreground="#b46300")
        self.tree.bind("<<TreeviewSelect>>", self.select_node)
        self.tree.bind("<Button-1>", lambda e: "break" if self.busy else None)
        self.tree.bind("<Key>", lambda e: "break" if self.busy else None)
        content = ttk.Frame(pane)
        pane.add(content, weight=1)
        content.rowconfigure(0, weight=1)
        content.columnconfigure(0, weight=1)
        self.overview = ttk.Frame(content, padding=10)
        self.settings = ttk.Frame(content, padding=10)
        self.tools_page = ttk.Frame(content, padding=10)
        for page in (self.overview, self.settings, self.tools_page):
            page.grid(row=0, column=0, sticky="nsew")
        tools_form = self.scroll_form(self.tools_page)
        ttk.Label(tools_form, text="Device tools", font=("TkDefaultFont", 13, "bold")).pack(anchor="w", pady=(0, 8))
        self.firmware = ttk.LabelFrame(tools_form, text="Firmware", padding=10)
        self.firmware.pack(fill="x")
        self.diagnostics = ttk.LabelFrame(tools_form, text="Diagnostics and console", padding=10)
        self.diagnostics.pack(fill="both", expand=True, pady=(12, 0))
        self.build_settings()
        self.build_firmware()
        self.build_diagnostics()
        self.render_overview()
        self.show_page(self.overview)
        self.render_tree()
        # Precede widget class bindings: wheels must never edit a value.
        self.wheel_tag = f"PadtoolWheel{id(self)}"
        for sequence in ("<MouseWheel>", "<Button-4>", "<Button-5>"):
            root.bind_class(self.wheel_tag, sequence, self.scroll_wheel)
        root.bind_all("<Map>", lambda event: self.tag_wheel_widget(event.widget), add="+")
        self.tag_wheel_tree(root)
        root.protocol("WM_DELETE_WINDOW", self.close)
        self.poll = root.after(80, self.drain)
        self.startup_scan = root.after_idle(self.scan) if auto_scan else None

    def log(self, text):
        self.output.configure(state="normal")
        self.output.insert("end", text.rstrip() + "\n")
        if int(self.output.index("end-1c").split(".")[0]) > 5000:
            self.output.delete("1.0", "1000.0")
        self.output.see("end")
        self.output.configure(state="disabled")

    def controls(self, widget):
        for child in widget.winfo_children():
            if isinstance(child, (ttk.Button, ttk.Menubutton, ttk.Combobox, ttk.Entry, ttk.Checkbutton, ttk.Scale, ttk.Spinbox)):
                yield child
            yield from self.controls(child)

    def run(self, label, work, done=None):
        if self.busy:
            return
        self.busy = True
        self.tree.state(["disabled"])
        self.notice.set(label)
        self.saved_states = [(w, str(w.cget("state"))) for w in self.controls(self.root)]
        for w, _ in self.saved_states:
            w.configure(state="disabled")
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
                if kind == "applied":
                    path, values, applied_keys = data
                    if values is None:
                        record = self.cache[path]
                        for key in applied_keys:
                            entry = record["draft"][key]
                            raw = self.value_bytes(entry)
                            was_reset = entry["reset"].get()
                            entry["original_raw"], entry["original"] = raw, entry["var"].get()
                            entry["explicit"] = not was_reset
                            entry["reset"].set(False)
                        record["error"] = "Setting(s) saved, but reload failed. Other edits remain pending. Reconnect and scan to reload; scanning will ask before discarding edits."
                    else:
                        record = self.cache[path]
                        pending = {key: (entry["var"].get(), entry["reset"].get())
                                   for key, entry in record["draft"].items()
                                   if key not in applied_keys and self.changed(entry)}
                        self.install_values(record, values)
                        for key, (text, reset) in pending.items():
                            if key in record["draft"]:
                                record["draft"][key]["var"].set(text)
                                record["draft"][key]["reset"].set(reset)
                    self.mark_dirty()
                    continue
                self.busy = False
                self.tree.state(["!disabled"])
                for w, state in self.saved_states:
                    if w.winfo_exists():
                        w.configure(state=state)
                if kind == "error":
                    error, output = data
                    self.log(output + "\n" + error)
                    self.notice.set("Operation failed. See the log.")
                    self.render_overview()
                    self.render_details()
                    messagebox.showerror("padtool", error)
                else:
                    result, done, output = data
                    if output:
                        self.log(output)
                    self.notice.set("")
                    if done:
                        done(result)
        except queue.Empty:
            pass
        self.poll = self.root.after(80, self.drain)

    def close(self):
        if self.busy:
            messagebox.showinfo("Operation in progress", "Wait for the current operation to finish before closing.")
        else:
            if self.dirty_count() and not messagebox.askyesno("Unsaved edits", "Discard pending edits and close?"):
                return
            if self.startup_scan is not None:
                self.root.after_cancel(self.startup_scan)
            self.root.after_cancel(self.poll)
            self.root.destroy()

    def record(self):
        return self.cache.get(self.device.get())

    def clear_settings(self):
        self.cache.pop(self.device.get(), None)
        self.device.set("")
        self.render_tree()
        self.render_overview()
        self.render_details()
        self.mark_dirty()

    def change_board(self, event=None):
        record = self.record()
        if not record or record["board"] != "loader":
            self.tree.selection_remove(self.tree.selection())
            self.device.set("")
        self.bundle.set(str(ROOT / "build" / self.board.get()))
        self.elf.set(str(ROOT / "build" / self.board.get() / "app.elf"))
        self.render_overview()
        self.render_details()
        self.update_tools()

    def path(self):
        path = self.device.get()
        if not path:
            messagebox.showinfo("Choose a device", "Refresh, then choose a device in the tree.")
        return path

    def install_values(self, record, values):
        draft = {}
        for t, target_values in values.items():
            for f in record["fields"]:
                if f["kind"] != pad.kind_of(t) or f["name"] not in target_values:
                    continue
                raw, explicit = target_values[f["name"]]
                original = pad.show_value(f, raw)
                defaults = record.get("defaults", {}).get(t, {})
                default_raw = defaults[f["name"]][0] if f["name"] in defaults else None
                if default_raw is None:
                    known = f.get("defaults", [])
                    index = t if f["kind"] == 0 else 0
                    if index < len(known) and len(known[index]) == len(raw):
                        default_raw = known[index]
                    elif not explicit:
                        default_raw = raw
                var, reset = tk.StringVar(value=original), tk.BooleanVar()
                entry = {"field": f, "var": var, "reset": reset, "original": original,
                         "original_raw": raw, "explicit": explicit, "default_raw": default_raw,
                         "default_text": pad.show_value(f, default_raw) if default_raw is not None else None}
                draft[t, f["name"]] = entry
                def edited(*_, e=entry):
                    e["reset"].set(False)
                    self.mark_dirty()
                var.trace_add("write", edited)
                reset.trace_add("write", lambda *_: self.mark_dirty())
        record["draft"] = draft
        record["values"] = values

    def value_bytes(self, entry):
        text = entry["var"].get()
        if text == entry["original"]:
            return entry["original_raw"]
        if entry["default_raw"] is not None and (text == entry["default_text"] or text == "default"):
            return entry["default_raw"]
        return pad.parse_value(entry["field"], text)

    def changed(self, entry):
        if entry["reset"].get() and entry["explicit"]:
            return True
        try:
            return self.value_bytes(entry) != entry["original_raw"]
        except (Exception, SystemExit):
            return True

    def differs_from_default(self, entry):
        if entry["default_raw"] is None:
            return None
        return self.value_bytes(entry) != entry["default_raw"]

    def reset_default(self, entry):
        entry.pop("custom_curve", None)
        if entry["field"]["name"] in ("left_curve_points", "right_curve_points"):
            for record in self.cache.values():
                for (target, name), candidate in record["draft"].items():
                    if candidate is entry:
                        enabled = record["draft"].get((target, name.replace("_points", "_enabled")))
                        if enabled:
                            self.reset_default(enabled)
        if entry["default_text"] is not None:
            entry["var"].set(entry["default_text"])
            entry["reset"].set(entry["explicit"])
            self.mark_dirty()

    def default_marker(self, parent, entry):
        widget = ttk.Label(parent, width=2, foreground="#b46300")
        self.indicators.append((entry, widget))
        self.update_previews()
        return widget

    def color_preview(self, record, entry):
        raw = self.value_bytes(entry)
        if entry["field"]["type"] == pad.T_RGB and len(raw) == 3:
            rgb = "#" + raw.hex()
            return rgb
        if entry["field"]["type"] == pad.T_COLOUR and len(raw) == 2:
            palette, level = raw
            if palette >= 16 or level >= 16:
                raise ValueError("Invalid palette entry or brightness")
            rgb = self.value_bytes(record["draft"][8, f"palette{palette}"])
            return "#" + rgb.hex()
        raise ValueError("Not a colour")

    def color_swatch(self, parent, record, entry):
        swatch = tk.Canvas(parent, width=50, height=20, highlightthickness=0)
        swatch.create_rectangle(1, 1, 49, 19, tags="colour", outline="#777777")
        swatch.create_text(25, 10, text="", tags="invalid")
        self.swatches.append((record, entry, swatch))
        self.update_previews()
        return swatch

    def update_previews(self):
        self.indicators = [(e, w) for e, w in self.indicators if w.winfo_exists()]
        for entry, widget in self.indicators:
            try:
                custom = self.differs_from_default(entry)
                widget.configure(text="?" if custom is None else "*" if custom else "")
            except (Exception, SystemExit):
                widget.configure(text="!")
        self.swatches = [(r, e, w) for r, e, w in self.swatches if w.winfo_exists()]
        for record, entry, swatch in self.swatches:
            try:
                colour = self.color_preview(record, entry)
                swatch.itemconfigure("colour", fill=colour)
                swatch.itemconfigure("invalid", text="")
            except (Exception, SystemExit):
                swatch.itemconfigure("colour", fill="#eeeeee")
                swatch.itemconfigure("invalid", text="?")

        self.special_editors = [w for w in self.special_editors if w.winfo_exists()]
        for widget in self.special_editors:
            widget.sync()
        self.apply_buttons = [(e, w, other) for e, w, other in self.apply_buttons if w.winfo_exists()]
        for entry, button, other in self.apply_buttons:
            button.configure(state="normal" if not self.busy and (self.changed(entry) or (other is not None and self.changed(other))) else "disabled")

    def setting_apply_button(self, parent, record, target, name, entry):
        button = ttk.Button(parent, text="Apply", width=6,
                            command=lambda: self.apply_settings((record["path"], target, name)))
        companion = record["draft"].get((target, name.replace("_points", "_enabled"))) if name.endswith("_curve_points") else None
        self.apply_buttons.append((entry, button, companion))
        self.update_previews()
        return button

    def show_help(self, entry):
        field = entry["field"]
        text = field["help"]
        if field["type"] == pad.T_COLOUR:
            text = ("Choose one of the 16 colours from the pad's palette, then select its brightness. "
                    "Previews show the full palette colour at every brightness level. "
                    "Palette edits also update these previews. Apply palette edits separately, or use Apply to save everything.\n\n" + text)
        elif field["name"] in ("left_curve_points", "right_curve_points"):
            text = ("These 15 points control stick output at 6.25% intervals from 6.25% through 93.75% travel. "
                    "Drag a point on the graph, move its slider, or enter an output percentage and press Enter. "
                    "Outputs stay in increasing order; zero and full travel stay fixed.\n\n"
                    "Use points enables the saved curve. Turning it off gives exact linear passthrough without losing points. Presets are shared with the CLI. "
                    "Apply beside a setting saves it; the toolbar Apply saves all pending edits.")
        messagebox.showinfo(field["name"].replace("_", " ").capitalize(), text or "No extra help is available.")

    def dirty_count(self):
        return sum(self.changed(e) for r in self.cache.values() for e in r.get("draft", {}).values())

    def update_summary(self):
        text = f"Loaded {len(self.cache)} device(s). {self.pending.get()}."
        if self.notice.get():
            text += " " + self.notice.get()
        self.summary.set(text)

    def discard_changes(self):
        if self.busy:
            return
        for record in self.cache.values():
            for entry in record.get("draft", {}).values():
                entry.pop("custom_curve", None)
                entry["var"].set(entry["original"])
                entry["reset"].set(False)
        self.render_overview()
        self.render_details()
        self.mark_dirty()
        self.notice.set("")

    def mark_dirty(self):
        count = self.dirty_count()
        self.pending.set(f"{count} pending change(s)" if count else "No pending changes")
        self.update_previews()
        for node, (path, target) in self.nodes.items():
            record = self.cache.get(path)
            if not record or not self.tree.exists(node):
                continue
            dirty = any(self.changed(e) for (t, name), e in record.get("draft", {}).items()
                        if target is None or target == t)
            custom = False
            for (t, name), entry in record.get("draft", {}).items():
                if target is not None and target != t:
                    continue
                try:
                    custom = custom or self.differs_from_default(entry) is True
                except (Exception, SystemExit):
                    pass
            label = self.tree.item(node, "text").removesuffix(" *")
            self.tree.item(node, text=label + (" *" if custom else ""), tags=("dirty",) if dirty else ())

    def scan(self):
        if self.dirty_count() and not messagebox.askyesno("Reload devices", "Discard pending edits and scan again?"):
            return
        def load():
            records, errors = [], []
            for path, name, identity in pad.discover_devices(include_loaders=True):
                if identity is None:
                    records.append({"defaults": {}, "path": path, "board": "loader", "name": name,
                                    "identity": None, "fields": [], "values": {}, "status": "UBOOT1.00 ROM loader"})
                    continue
                board = next(b for b, value in pad.BOARD_IDS.items() if value == identity[6])
                try:
                    table = pad.fields(path) if board == "pad" else []
                    values = {t: pad.read_target(path, table, t) for t in range(len(pad.TARGETS))} if table else {}
                    defaults = {t: pad.read_target(path, table, t, defaults=True) for t in range(len(pad.TARGETS))} if table and int.from_bytes(identity[4:6], "little") >= 4 else {}
                    records.append({"defaults": defaults, "path": path, "board": board, "name": name,
                                    "identity": identity[:40], "fields": table, "values": values,
                                    "status": pad.status(path)})
                except (Exception, SystemExit) as error:
                    errors.append(f"{path}: {error}")
            return records, errors
        def display(result):
            records, errors = result
            self.cache = {}
            for record in records:
                self.install_values(record, record["values"])
                self.cache[record["path"]] = record
            self.device.set("")
            self.render_tree()
            self.mark_dirty()
            self.notice.set("")
            for error in errors:
                self.log(error)
            if errors:
                messagebox.showerror("Some devices could not be loaded", "\n".join(errors))
        self.run("Scanning and loading all device settings…", load, display)

    def render_tree(self):
        self.tree.delete(*self.tree.get_children())
        self.nodes = {}
        for i, (path, record) in enumerate(self.cache.items()):
            device_node = f"device{i}"
            location = ""
            if os.name != "nt":
                topology = Path("/sys/class/scsi_generic", Path(path).name, "device").resolve()
                ports = [part for part in topology.parts if re.fullmatch(r"[0-9]+-[0-9]+(?:\.[0-9]+)*", part)]
                if ports:
                    location = f"USB {ports[-1]} · "
            label = "ROM loader (UBOOT1.00)" if record["board"] == "loader" else record["board"].title()
            self.tree.insert("", "end", iid=device_node, text=f"{location}{label} · {path}", open=True)
            self.nodes[device_node] = (path, None)
            targets = range(len(pad.TARGETS)) if record["board"] == "pad" else []
            for t in targets:
                label = f"Slot {pad.TARGETS[t]}" if t < 8 else pad.TARGETS[t].title()
                node = f"{device_node}/{t}"
                self.tree.insert(device_node, "end", iid=node, text=label)
                self.nodes[node] = (path, t)
            node = f"{device_node}/tools"
            self.tree.insert(device_node, "end", iid=node, text="Device tools")
            self.nodes[node] = (path, "tools")
        if not self.cache:
            self.tree.insert("", "end", iid="tools", text="Device tools")
            self.nodes["tools"] = (None, "tools")
        nodes = self.tree.get_children()
        if nodes:
            self.tree.selection_set(nodes[0])
            self.select_node()
        else:
            self.render_overview()
            self.render_details()
        self.mark_dirty()

    def select_node(self, event=None):
        if self.busy or not self.tree.selection():
            return
        path, target = self.nodes[self.tree.selection()[0]]
        if path is None:
            self.update_tools()
            self.show_page(self.tools_page)
            return
        record = self.cache[path]
        self.device.set(path)
        previous_board = self.board.get()
        if record["board"] in PIDS:
            self.board.set(record["board"])
        if record["board"] in PIDS and previous_board != record["board"]:
            self.bundle.set(str(ROOT / "build" / record["board"]))
            self.elf.set(str(ROOT / "build" / record["board"] / "app.elf"))
        if isinstance(target, int):
            self.target.set(pad.TARGETS[target])
            if target < 8:
                self.slot.set(pad.TARGETS[target])
        self.render_overview()
        self.render_details()
        self.update_tools()
        pages = {"tools": self.tools_page, None: self.overview}
        self.show_page(pages.get(target, self.settings))

    def scroll_form(self, parent):
        frame = ttk.Frame(parent)
        frame.pack(fill="both", expand=True)
        canvas = tk.Canvas(frame, highlightthickness=0)
        scrollbar = ttk.Scrollbar(frame, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)
        form = ttk.Frame(canvas)
        window = canvas.create_window((0, 0), window=form, anchor="nw")
        def update_region(event=None):
            bounds = canvas.bbox(window)
            if bounds:
                canvas.configure(scrollregion=(0, 0, bounds[2], max(bounds[3], canvas.winfo_height())))

        def resize(event):
            canvas.itemconfigure(window, width=event.width)
            update_region()

        form.bind("<Configure>", update_region)
        canvas.bind("<Configure>", resize)
        self.scroll_canvases[parent] = canvas
        return form

    def show_page(self, page):
        self.active_page = page
        page.tkraise()

    def tag_wheel_widget(self, widget):
        if isinstance(widget, tk.Misc):
            tags = widget.bindtags()
            if self.wheel_tag not in tags:
                widget.bindtags((self.wheel_tag,) + tags)

    def tag_wheel_tree(self, widget):
        self.tag_wheel_widget(widget)
        for child in widget.winfo_children():
            self.tag_wheel_tree(child)

    def scroll_wheel(self, event):
        number = getattr(event, "num", None)
        delta = getattr(event, "delta", 0)
        if number in (4, 5):
            steps = -1 if number == 4 else 1
        elif abs(delta) >= 120:
            steps = -int(delta / 120)
        else:
            steps = -1 if delta > 0 else 1 if delta < 0 else 0
        target = self.output if event.widget is self.output else self.scroll_canvases.get(self.active_page)
        if target is not None:
            target.yview_scroll(steps, "units")
        return "break"

    def build_settings(self):
        self.overview_form = self.scroll_form(self.overview)
        bar = ttk.Frame(self.settings)
        bar.pack(fill="x", pady=(0, 8))
        self.details_title = tk.StringVar(value="Select a slot or settings group in the tree")
        ttk.Label(bar, textvariable=self.details_title, font=("TkDefaultFont", 12, "bold")).pack(side="left")
        ttk.Label(self.settings, text="* differs from default · ! invalid value · ? default unavailable. Reset fills in the default; Apply saves it.", wraplength=850).pack(anchor="w", pady=(0, 8))
        self.form = self.scroll_form(self.settings)

    def show_target(self, target):
        for node, key in self.nodes.items():
            if key == (self.device.get(), target):
                self.tree.selection_set(node)
                self.select_node()
                break

    def render_overview(self):
        for widget in self.overview_form.winfo_children():
            widget.destroy()
        f = self.overview_form
        record = self.record()
        if not record:
            ttk.Label(f, text="Refresh to discover pads, dongles and slot settings.", wraplength=800).pack(anchor="w", pady=12)
            return
        ttk.Label(f, text=f"{record['board'].title()} · {record['path']}", font=("TkDefaultFont", 13, "bold")).pack(anchor="w", pady=(0, 2))
        ttk.Label(f, text=record["status"], wraplength=850).pack(anchor="w", pady=(0, 4))
        ttk.Button(f, text="Flash Firmware…", command=self.choose_firmware).pack(anchor="w", pady=(4, 8))
        if record["board"] == "loader":
            ttk.Label(f, text="This device is in ROM loader mode. Select a firmware image to flash it.\n"
                      "Settings and diagnostics are available after firmware starts. Reconnect USB to leave loader mode.", wraplength=850).pack(anchor="w", pady=6)
            return
        if record.get("error"):
            ttk.Label(f, text=record["error"]).pack(anchor="w", pady=8)
            return
        if record["board"] == "dongle":
            ttk.Label(f, text="Choose Device tools in the tree for dongle pairing and connection settings.").pack(anchor="w", pady=8)
            ttk.Button(f, text="Pair dongle…", command=lambda: self.slot_action("pair", 0)).pack(anchor="w")
            return
        ttk.Label(f, text="Choose each slot's type here. Open Settings for transport, colours, response and other options.", wraplength=850).pack(anchor="w", pady=(0, 6))
        for target in range(8):
            row = ttk.Frame(f, padding=(0, 6))
            row.pack(fill="x")
            entry = record["draft"].get((target, "type"))
            if entry:
                self.default_marker(row, entry).pack(side="left", padx=(0, 2))
            ttk.Label(row, text=f"Slot {pad.TARGETS[target]}", width=12).pack(side="left")
            if entry:
                ttk.Combobox(row, textvariable=entry["var"], values=entry["field"]["names"], state="readonly", width=20).pack(side="left")
                self.setting_apply_button(row, record, target, "type", entry).pack(side="left", padx=(8, 20))
            ttk.Button(row, text="Settings", command=lambda t=target: self.show_target(t)).pack(side="left", padx=(0, 8))
            ttk.Button(row, text="Connect", command=lambda t=target: self.slot_action("connect", t)).pack(side="left", padx=(0, 8))
            ttk.Button(row, text="Pair…", command=lambda t=target: self.slot_action("pair", t)).pack(side="left", padx=(0, 8))
        actions = ttk.Frame(f)
        actions.pack(fill="x", pady=(12, 0))
        ttk.Button(actions, text="Back up pad settings…", command=self.backup_settings).pack(side="left")
        ttk.Button(actions, text="Restore pad settings…", command=self.restore_settings).pack(side="left", padx=8)
        ttk.Label(f, text="Connect and Pair use saved settings; apply pending edits for that slot first.", wraplength=850).pack(anchor="w", pady=10)

    def render_details(self):
        for widget in self.form.winfo_children():
            widget.destroy()
        self.editors = {}
        record = self.record()
        supported = record and record["board"] == "pad" and not record.get("error")
        if not supported:
            self.details_title.set("Select a pad slot in the tree")
            return
        target = pad.target_index(self.target.get())
        self.details_title.set(f"{record['path']} · " + (f"Slot {pad.TARGETS[target]}" if target < 8 else pad.TARGETS[target].title()))
        advanced = ttk.Frame(self.form)
        rows = {self.form: 0, advanced: 0}
        if target == pad.target_index("calibration"):
            calibration = ttk.Frame(self.form, padding=(4, 4, 4, 12))
            calibration.grid(row=0, column=0, columnspan=4, sticky="w")
            ttk.Button(calibration, text="Calibrate sensors…", command=self.calibrate).pack(anchor="w")
            ttk.Label(calibration, text="Calibration is shared by all slots. Select Calibrate sensors for instructions.", wraplength=800).pack(anchor="w", pady=(6, 0))
            rows[self.form] = 1
        for (t, name), entry in record["draft"].items():
            f = entry["field"]
            if t != target:
                continue
            if name.endswith("_curve_enabled") and (t, name.replace("_enabled", "_points")) in record["draft"]:
                continue
            parent = advanced if f["dev"] else self.form
            row = rows[parent]
            rows[parent] += 1
            label = ttk.Frame(parent)
            label.grid(row=row, column=0, sticky="nw", padx=4, pady=8)
            self.default_marker(label, entry).pack(side="left", padx=(0, 2))
            ttk.Label(label, text=name.replace("_", " ").capitalize()).pack(side="left")
            if f["type"] == pad.T_COLOUR:
                editor = PaletteEditor(parent, self, record, entry)
                self.special_editors.append(editor)
            elif name in ("left_curve_points", "right_curve_points"):
                editor = CurveEditor(parent, self, entry, record["draft"].get((target, name.replace("_points", "_enabled"))))
                self.special_editors.append(editor)
            elif f["type"] == pad.T_ENUM:
                editor = ttk.Combobox(parent, textvariable=entry["var"], values=f["names"], state="readonly", width=23)
            else:
                editor = ttk.Entry(parent, textvariable=entry["var"], width=25)
            editor.grid(row=row, column=1, columnspan=2 if isinstance(editor, (PaletteEditor, CurveEditor)) else 1,
                        sticky="ew", padx=4, pady=4)
            if f["type"] == pad.T_NUMBER:
                maximum = f.get("max", (1 << (8 * f["size"])) - 1)
                ttk.Label(parent, text=f"0–{maximum}", foreground="#666666").grid(row=row, column=2, sticky="w", padx=6)
            if f["type"] == pad.T_RGB:
                self.color_swatch(parent, record, entry).grid(row=row, column=2, padx=4)
            actions = ttk.Frame(parent)
            actions.grid(row=row, column=3, sticky="ne", padx=4, pady=4)
            ttk.Button(actions, text="Reset", width=6,
                       state="normal" if entry["default_text"] is not None else "disabled",
                       command=lambda e=entry: self.reset_default(e)).pack(side="left", padx=2)
            self.setting_apply_button(actions, record, target, name, entry).pack(side="left", padx=2)
            ttk.Button(actions, text="?", width=2, command=lambda e=entry: self.show_help(e)).pack(side="left", padx=2)
            self.editors[name] = entry
        self.form.columnconfigure(1, weight=1)
        if rows[advanced]:
            key = (record["path"], target)
            row = rows[self.form]
            button = ttk.Button(self.form)
            button.grid(row=row, column=0, columnspan=4, sticky="w", padx=4, pady=(12, 6))
            advanced.grid(row=row + 1, column=0, columnspan=4, sticky="ew")
            advanced.columnconfigure(1, weight=1)

            def update_section():
                expanded = key in self.expanded_details
                button.configure(text="▾ Developer fields" if expanded else "▸ Developer fields")
                if expanded:
                    advanced.grid()
                else:
                    advanced.grid_remove()

            def toggle_section():
                if key in self.expanded_details:
                    self.expanded_details.remove(key)
                else:
                    self.expanded_details.add(key)
                update_section()

            button.configure(command=toggle_section)
            update_section()

    def apply_settings(self, only=None):
        jobs = []
        try:
            allowed = {only} if only else None
            if only and only[2].endswith("_curve_points"):
                allowed.add((only[0], only[1], only[2].replace("_points", "_enabled")))
            selected_entry = self.cache[only[0]]["draft"][only[1:]] if only else None
            for widget in self.special_editors:
                if widget.winfo_exists() and isinstance(widget, CurveEditor) and (only is None or widget.entry is selected_entry):
                    widget.commit_pending()
            for path, record in self.cache.items():
                keys = {key for key, entry in record.get("draft", {}).items()
                        if self.changed(entry) and (allowed is None or (path, *key) in allowed)}
                items = [(t, entry["field"]["id"], b"" if entry["reset"].get() else self.value_bytes(entry))
                         for (t, name), entry in record.get("draft", {}).items() if (t, name) in keys]
                if items:
                    if record.get("error"):
                        raise ValueError(f"{path}: reload this device before applying more settings")
                    jobs.append((path, record, items, keys))
        except (Exception, SystemExit) as error:
            messagebox.showerror("Invalid setting", str(error))
            return
        if not jobs:
            self.notice.set("")
            return
        def apply():
            # Check every identity before the first commit: USB paths can be reused.
            for path, record, items, keys in jobs:
                if pad.cmd(path, 0, length=64)[:40] != record["identity"]:
                    raise ValueError(f"{path}: device changed since loading; refresh again")
            for path, record, items, keys in jobs:
                pad.send(path, items)
                try:
                    values = {t: pad.read_target(path, record["fields"], t) for t in range(len(pad.TARGETS))}
                except (Exception, SystemExit) as error:
                    self.events.put(("applied", (path, None, keys)))
                    raise RuntimeError(f"{path}: settings saved, but reload failed: {error}")
                self.events.put(("applied", (path, values, keys)))
                self.flash_report(f"{path}: applied {len(items)} setting(s)")
        def finished(_):
            self.render_overview()
            self.render_details()
            self.mark_dirty()
        self.run("Applying setting…" if only else "Applying pending changes to all edited devices…", apply, finished)

    def calibrate(self):
        self.slot_action("calibrate", 0)

    def slot_action(self, action, target):
        path = self.path()
        if not path:
            return
        record = self.cache[path]
        if action in ("connect", "pair") and any(self.changed(e) for (t, name), e in record["draft"].items() if t == target):
            messagebox.showinfo("Apply slot changes first", "Apply pending changes for this slot before connecting or pairing.")
            return
        if action == "connect" and record["board"] != "pad":
            return
        if action == "calibrate":
            if record["board"] != "pad":
                return
            if not messagebox.askokcancel("Calibrate sensors", "Calibration is shared by all slots.\n\n"
                                           "After starting, leave sticks and triggers untouched for 2.5 seconds. Then circle both sticks along their edges and press both triggers fully.\n\n"
                                           "Press A or B on the pad to save; Home cancels. See the device log for results."):
                return
        if action == "pair" and not messagebox.askyesno("Pair", "Make this slot pairable? Its host is replaced after pairing succeeds. A dongle forgets its existing bond immediately."):
            return
        command = {"connect": 0x13, "pair": 0x17, "calibrate": 0x18}[action]
        def perform():
            if pad.cmd(path, 0, length=64)[:40] != record["identity"]:
                raise ValueError("Device changed; refresh again before sending an action")
            pad.cmd(path, command, target if action != "calibrate" else 0)
        def finished(_):
            if action == "calibrate":
                self.notice.set("Calibration started: hands off for 2.5 seconds, then sweep both sticks and triggers. A or B saves; Home cancels.")
        self.run(action.title() + "…", perform, finished)

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
        if file and messagebox.askyesno("Restore pad settings", "Restore this pad’s slots, palette, pairing data and calibration from this file?"):
            self.run("Restoring settings…", lambda: pad.restore(path, file, True), lambda _: self.clear_settings())

    def file_row(self, parent, row, label, var, directory=False, on_select=None):
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", pady=5)
        ttk.Entry(parent, textvariable=var).grid(row=row, column=1, sticky="ew", padx=8)
        def browse():
            path = filedialog.askdirectory() if directory else filedialog.askopenfilename()
            if path:
                if on_select:
                    on_select(path)
                else:
                    var.set(path)
        ttk.Button(parent, text="Browse…", command=browse).grid(row=row, column=2)

    def build_firmware(self):
        f = self.firmware
        f.columnconfigure(1, weight=1)
        ttk.Label(f, text="Full flash backup and verified firmware installation", font=("TkDefaultFont", 12, "bold")).grid(row=0, column=0, columnspan=3, sticky="w")
        ttk.Label(f, text="Python flasher using the BR23 loader from the SDK or distribution package.\n"
                  "If automatic loader entry fails: pad—hold Home + Start while plugging in USB; dongle—hold its button.\n"
                  "Backup leaves the device in its loader; reconnect USB when finished.", wraplength=940).grid(row=1, column=0, columnspan=3, sticky="w", pady=10)
        exe = ROOT / "build/host" / ("jlnewfw.exe" if os.name == "nt" else "jlnewfw")
        self.flasher = tk.StringVar(value=str(exe))
        self.loader = tk.StringVar(value=str(padflash.DEFAULT_LOADER))
        self.image = tk.StringVar()
        self.bundle = tk.StringVar(value=str(ROOT / "build/pad"))
        self.uboot = tk.StringVar(value=str(SDK / "uboot.boot"))
        self.cfgtool = tk.StringVar(value=str(SDK / "cfg_tool.bin"))
        ttk.Label(f, text="Firmware board").grid(row=2, column=0, sticky="w")
        self.boards = ttk.Combobox(f, textvariable=self.board, values=list(PIDS), state="readonly", width=12)
        self.boards.grid(row=2, column=1, sticky="w", padx=8)
        self.boards.bind("<<ComboboxSelected>>", self.change_board)
        self.file_row(f, 3, "BR23 loader", self.loader)
        self.file_row(f, 4, "Firmware image (.img / .ufw)", self.image, on_select=self.select_firmware_image)
        ttk.Button(f, text="Flash Firmware…", command=self.choose_firmware).grid(row=5, column=1, sticky="w", pady=8)
        ttk.Separator(f).grid(row=6, column=0, columnspan=3, sticky="ew", pady=8)
        ttk.Label(f, text="Optional: pack our build with jlnewfw for the connected chip's key:").grid(row=7, column=0, columnspan=3, sticky="w")
        self.file_row(f, 8, "jlnewfw (build packing only)", self.flasher)
        self.file_row(f, 9, "Build folder (app.bin + isd_config.ini)", self.bundle, True)
        self.file_row(f, 10, "SDK uboot.boot", self.uboot)
        self.file_row(f, 11, "SDK cfg_tool.bin", self.cfgtool)
        ttk.Button(f, text="Pack and flash build…", command=lambda: self.flash(True)).grid(row=12, column=1, sticky="w", pady=8)
        ttk.Button(f, text="Back up full flash…", command=self.backup_flash).grid(row=5, column=2, sticky="e", pady=8)
        ttk.Label(f, text="An optional full flash backup is offered before flashing. Every written byte is verified.\n"
                  "Select a build folder explicitly when switching boards. Settings in erased sectors are replaced by the image.", wraplength=940).grid(row=13, column=0, columnspan=3, sticky="w", pady=8)

    def update_tools(self):
        record = self.record()
        running = record is not None and record["board"] in PIDS
        for widget in self.controls(self.diagnostics):
            state = "readonly" if running and isinstance(widget, ttk.Combobox) else "normal" if running else "disabled"
            if record is None and isinstance(widget, ttk.Button) and widget.cget("text") == "Tool mode (Linux HID)":
                state = "normal"
            widget.configure(state=state)

    def choose_firmware(self):
        if self.busy:
            return
        file = filedialog.askopenfilename(title="Select firmware to flash", filetypes=[("Firmware", "*.img *.ufw"), ("All files", "*")])
        if not file:
            return
        if self.select_firmware_image(file):
            self.flash(False)

    def select_firmware_image(self, file):
        try:
            pid, _ = padflash.image_identity(padflash.load_image(file))
            board = next((name for name, value in PIDS.items() if value == pid), None)
            if board is None:
                raise ValueError("This firmware image is not for a supported pad or dongle.")
            record = self.record()
            if record and record["board"] in PIDS and record["board"] != board:
                raise ValueError("Image does not match the selected device.")
        except (ValueError, OSError) as error:
            messagebox.showerror("Firmware", str(error))
            return False
        self.image.set(file)
        self.board.set(board)
        return True

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
        record = self.record()
        loader_path = record["path"] if record and record["board"] == "loader" else None
        def backup():
            loader = padflash.select_loader(board, blob, report=self.flash_report, device_path=loader_path)
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
                if padflash.image_identity(padflash.load_image(image))[0] != PIDS[board]:
                    raise ValueError("Image product ID does not match the selected board.")
                files = [image]
        except (ValueError, OSError) as error:
            messagebox.showerror("Firmware", str(error))
            return
        make_backup = messagebox.askyesnocancel("Back up flash first?", "Would you like to save a full flash backup before flashing?")
        if make_backup is None:
            return
        backup = None
        if make_backup:
            backup = filedialog.asksaveasfilename(title="Save pre-flash backup", defaultextension=".bin", initialfile=f"{board}-before-flash.bin")
            if not backup:
                return
            inputs = [p.resolve() for p in files + [Path(self.loader.get())]]
            if pack:
                inputs.append(Path(exe).resolve())
            if Path(backup).resolve() in inputs:
                messagebox.showerror("Backup destination", "Choose a backup file different from the firmware input files.")
                return
        action = "Back up and flash" if backup else "Flash"
        if not messagebox.askyesno("Flash firmware", f"{action} the {board}?\n\n"
                                  "The flasher checks the firmware product ID and chip key. "
                                  "Settings in erased sectors will be replaced. Keep USB connected until verification finishes."):
            return
        record = self.record()
        loader_path = record["path"] if record and record["board"] == "loader" else None
        def flash():
            with tempfile.TemporaryDirectory(prefix="padtool-") as temp:
                packed = Path(temp) / "firmware.img"
                if pack:
                    self.invoke(pack_args + ["-k", "0xffff", "-o", str(packed)])
                data = padflash.load_image(packed if pack else image)
                pid, key = padflash.image_identity(data)
                if pid != PIDS[board]:
                    raise ValueError("Image does not match the selected board")
                curve_backup = None if loader_path or not backup else pad.prepare_curve_upgrade(board, backup)
                loader = padflash.select_loader(board, blob, report=self.flash_report, device_path=loader_path)
                if pack and key != loader.key:
                    self.invoke(pack_args + ["-k", hex(loader.key), "-o", str(packed)])
                    data = padflash.load_image(packed)
                loader.flash(data, board, backup)
                pad.finish_curve_upgrade(curve_backup)
        self.run("Backing up and flashing…" if backup else "Flashing…", flash, lambda _: self.clear_settings())

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

        console = ttk.Frame(f)
        console.pack(fill="both", expand=True)
        self.output = tk.Text(console, height=12, wrap="word", state="disabled")
        scrollbar = ttk.Scrollbar(console, orient="vertical", command=self.output.yview)
        self.output.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side="right", fill="y")
        self.output.pack(fill="both", expand=True)

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
            record = self.record()
            if record and record["board"] == "loader":
                return
            def perform():
                if record and pad.cmd(path, 0, length=64)[:40] != record["identity"]:
                    raise ValueError("Device changed; refresh again before sending an action")
                return fn(path)
            self.run(label + "…", perform)

    def connect(self):
        self.slot_action("connect", pad.SLOT_NAMES.index(self.slot.get()))

    def pair(self):
        self.slot_action("pair", pad.SLOT_NAMES.index(self.slot.get()) if self.board.get() == "pad" else 0)


if __name__ == "__main__":
    App(tk.Tk()).root.mainloop()
