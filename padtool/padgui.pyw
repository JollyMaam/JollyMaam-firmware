#!/usr/bin/env python3
"""Tkinter front end for padtool and the Python BR23 flasher. Run: python padtool/padgui.pyw."""
import contextlib
import io
from pathlib import Path
import queue
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, simpledialog, ttk

import padtool as pad
import padflash
from padtransport import usb_location
from paddevices import DeviceMonitor, read_device
from padconsole import LiveConsole
from padwidgets import PaletteEditor, CurveEditor, RemapEditor, Tooltip

PIDS = padflash.PIDS


class CalibrationView(ttk.Label):
    """The calibration blob in words; read-only, refreshed with the editors."""
    def __init__(self, parent, app, entry):
        super().__init__(parent, justify="left")
        self.app, self.entry = app, entry
        self.sync()

    def sync(self):
        try:
            text = "\n".join(pad.describe_calibration(self.app.value_bytes(self.entry)))
        except (Exception, SystemExit):
            text = "Unreadable calibration. Run Calibrate sensors."
        if self.cget("text") != text:
            self.configure(text=text)


class App:
    def __init__(self, root, auto_scan=True, auto_identify=True):
        self.root = root
        self.auto_identify = auto_identify
        root.title("Cyclone 2 · padtool")
        root.geometry("1250x850")
        self.events = queue.Queue()
        self.monitor = DeviceMonitor(self.events)
        self.busy = False
        self.flash_active = False
        self.batch_edits = False
        self.scroll_canvases = {}
        self.scroll_updaters = {}            # a scroll form's scroll region, brought up to date now
        self.responsive_rows = {}
        self.active_page = None
        self.cache = {}
        self.consoles = {}
        self.nodes = {}
        self.editors = {}
        self.indicators = []
        self.swatches = []
        self.special_editors = []
        self.scope_buttons = []
        self.default_buttons = []
        self.unpair_buttons = []
        self.board = tk.StringVar()
        self.loader = tk.StringVar(value=str(padflash.DEFAULT_LOADER))
        self.image = tk.StringVar()
        self.product_id = tk.StringVar(value="Not read")
        self.address = tk.StringVar()
        self.loader_title = tk.StringVar()
        self.dongle_title = tk.StringVar()
        self.dongle_status = tk.StringVar()
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
        self.apply_all_button = ttk.Button(top, text="Apply", command=self.apply_settings, state="disabled")
        toolbar_buttons = [ttk.Button(top, text="Refresh", command=self.refresh_devices),
                           self.apply_all_button,
                           ttk.Button(top, text="Discard", command=self.discard_changes)]
        toolbar_status = ttk.Label(top, textvariable=self.summary)
        def resize_toolbar(event):
            columns = 4 if event.width >= 400 else 3 if event.width >= 300 else 2 if event.width >= 200 else 1
            for index, button in enumerate(toolbar_buttons):
                button.grid(row=index // columns, column=index % columns, sticky="w", padx=(0, 8))
            wide = event.width >= 700
            toolbar_status.configure(wraplength=max(80, min(750, event.width - (300 if wide else 20))))
            toolbar_status.grid(row=0 if wide else (len(toolbar_buttons) + columns - 1) // columns,
                                column=len(toolbar_buttons) if wide else 0, columnspan=1 if wide else columns, sticky="w")
        top.bind("<Configure>", resize_toolbar)
        pane = ttk.Panedwindow(root, orient="horizontal")
        pane.pack(fill="both", expand=True, padx=10)
        self.pane_width = None
        def resize_navigation(event):
            if event.width != self.pane_width and len(pane.panes()) == 2:
                self.pane_width = event.width
                pane.sashpos(0, min(250, max(90, event.width // 4)))
        pane.bind("<Configure>", resize_navigation)
        navigation = ttk.Frame(pane, width=250)
        pane.add(navigation, weight=0)
        heading = ttk.Frame(navigation)
        heading.pack(anchor="w", fill="x")
        ttk.Label(heading, text="Devices and slots", padding=6).pack(side="left", anchor="n")
        # That we are looking, or that nothing is there: beside the heading.
        self.tree_status = ttk.Label(heading, padding=(0, 6, 6, 6), foreground="#666666", wraplength=150)
        self.tree_status.pack(side="left", anchor="n")
        ttk.Button(navigation, text="Open device path…", command=self.open_device_path).pack(side="bottom", fill="x", pady=6)
        self.tree = ttk.Treeview(navigation, show="tree", selectmode="browse", height=22)
        scrollbar = ttk.Scrollbar(navigation, orient="vertical", command=self.tree.yview)
        scrollbar.pack(side="right", fill="y")
        horizontal = ttk.Scrollbar(navigation, orient="horizontal", command=self.tree.xview)
        horizontal.pack(side="bottom", fill="x")
        self.tree.configure(yscrollcommand=scrollbar.set, xscrollcommand=horizontal.set)
        self.tree.pack(fill="both", expand=True)
        self.looking = False
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
        self.loader_page = ttk.Frame(content, padding=10)
        self.dongle_page = ttk.Frame(content, padding=10)
        for page in (self.overview, self.settings, self.loader_page, self.dongle_page):
            page.grid(row=0, column=0, sticky="nsew")
        self.build_settings()
        self.build_loader_page()
        self.build_dongle_page()
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
        output = self.loader_output
        output.configure(state="normal")
        output.insert("end", text.rstrip() + "\n")
        if int(output.index("end-1c").split(".")[0]) > 5000:
            output.delete("1.0", "1000.0")
        output.see("end")
        output.configure(state="disabled")

    def show_console(self):
        record = self.record()
        if not record or record["board"] not in PIDS:
            return
        identity = record["identity"]
        if identity in self.consoles:
            window = self.consoles[identity].window
            window.deiconify()
            window.lift()
            return
        self.consoles[identity] = LiveConsole(self.root, self.monitor, identity, record["board"],
                                             self.console_path, lambda: self.consoles.pop(identity, None))

    def console_path(self, identity):
        matches = [r["path"] for r in self.cache.values()
                   if r.get("online", True) and r["identity"] == identity and r["board"] in PIDS]
        return matches[0] if len(matches) == 1 else None

    def console_output(self, parent):
        frame = ttk.Frame(parent)
        frame.pack(fill="both", expand=True)
        output = tk.Text(frame, height=12, wrap="word", state="disabled")
        scrollbar = ttk.Scrollbar(frame, orient="vertical", command=output.yview)
        output.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side="right", fill="y")
        output.pack(fill="both", expand=True)
        return output

    def controls(self, widget):
        for child in widget.winfo_children():
            if isinstance(child, (ttk.Button, ttk.Menubutton, ttk.Combobox, ttk.Entry, ttk.Checkbutton, ttk.Scale, ttk.Spinbox)):
                yield child
            yield from self.controls(child)

    def run(self, label, work, done=None):
        if self.busy:
            return
        self.busy = True
        self.monitor.busy.set()
        self.monitor.invalidate()
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
                with self.monitor.lock, contextlib.redirect_stdout(stream):
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
                if kind == "refreshed":
                    self.looking = False
                    self.update_tree_status()
                    continue
                if kind == "devices":
                    if not self.busy and data[0] == self.monitor.generation:
                        self.update_devices(data)
                    continue
                if kind == "product_id":
                    path, pid = data
                    record = self.cache.get(path)
                    if record and record["board"] == "loader":
                        record["product_id"] = pid
                        if self.device.get() == path:
                            self.product_id.set(pid or "Unavailable")
                    continue
                if kind == "flash_progress":
                    stage, done, total = data
                    self.flash_status.set(stage + (f" · {done / total:.0%}" if total else "…"))
                    self.notice.set(self.flash_status.get())
                    self.flash_bar.stop()
                    self.flash_bar.configure(mode="determinate" if total else "indeterminate")
                    if total:
                        self.flash_bar["value"] = done / total * 100
                    else:
                        self.flash_bar.start()
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
                            entry.pop("unpair", None)
                            entry["reset"].set(False)
                        record["error"] = "Setting(s) saved, but reload failed. Other edits remain pending. Reconnect or use Open device path to reload; other pending edits are retained."
                    else:
                        record = self.cache[path]
                        pending = {key: (entry["var"].get(), entry["reset"].get(), entry.get("unpair", False))
                                   for key, entry in record["draft"].items()
                                   if key not in applied_keys and self.changed(entry)}
                        self.install_values(record, values)
                        for key, (text, reset, unpair) in pending.items():
                            if key in record["draft"]:
                                record["draft"][key]["var"].set(text)
                                record["draft"][key]["reset"].set(reset)
                                record["draft"][key]["unpair"] = unpair
                    self.mark_dirty()
                    continue
                completed_flash = self.flash_active
                if self.flash_active:
                    self.flash_bar.stop()
                    self.flash_bar.configure(mode="determinate", value=100 if kind == "done" else 0)
                    self.flash_status.set("Complete" if kind == "done" else "Operation failed · see console")
                    self.flash_active = False
                self.busy = False
                self.monitor.busy.clear()
                self.tree.state(["!disabled"])
                for w, state in self.saved_states:
                    if w.winfo_exists():
                        w.configure(state=state)
                if kind == "error":
                    self.looking = False
                    self.update_tree_status()
                    error, output = data
                    self.log(output + "\n" + error)
                    self.notice.set("Operation failed.")
                    self.rerender_in_place()
                    messagebox.showerror("padtool", error)
                else:
                    result, done, output = data
                    if output:
                        self.log(output)
                    self.notice.set("Operation complete." if completed_flash else "")
                    if done:
                        done(result)
                self.update_apply_state()
                self.update_previews()
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
            self.monitor.stop.set()
            for console in list(self.consoles.values()):
                console.close()
            self.flash_bar.stop()
            self.root.destroy()

    def record(self):
        return self.cache.get(self.device.get())

    def clear_settings(self):
        self.cache.pop(self.device.get(), None)
        self.sync_devices()
        self.device.set("")
        self.render_tree()
        self.render_overview()
        self.render_details()
        self.mark_dirty()

    def path(self):
        path = self.device.get()
        if not path:
            messagebox.showinfo("Choose a device", "Choose a device in the tree, or use Open device path.")
        elif self.record() and not self.record().get("online", True):
            messagebox.showinfo("Device disconnected", "Reconnect the device to use this action. Pending edits are retained.")
            return None
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
                if f["name"] == "address" and not explicit:
                    default_raw = raw  # The firmware returns the effective derived identity.
                var, reset = tk.StringVar(value=original), tk.BooleanVar()
                entry = {"field": f, "var": var, "reset": reset, "original": original,
                         "original_raw": raw, "explicit": explicit, "default_raw": default_raw,
                         "default_text": pad.show_value(f, default_raw) if default_raw is not None else None}
                draft[t, f["name"]] = entry
                def edited(*_, e=entry):
                    e.pop("unpair", None)
                    e["reset"].set(False)
                    self.mark_dirty()
                var.trace_add("write", edited)
                reset.trace_add("write", lambda *_: self.mark_dirty())
                if f["kind"] == 3 and f["name"] == "name":
                    var.trace_add("write", lambda *_, r=record, t=t: self.rename_node(r, t))
        record["draft"] = draft
        record["values"] = values

    def value_bytes(self, entry):
        if entry.get("unpair"):
            return bytes(len(entry["original_raw"]))
        text = entry["var"].get()
        if text == entry["original"]:
            return entry["original_raw"]
        if entry["default_raw"] is not None and (text == entry["default_text"] or text == "default"):
            return entry["default_raw"]
        return pad.parse_value(entry["field"], text)

    def changed(self, entry):
        if entry.get("unpair"):
            return True
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
        entry.pop("unpair", None)
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

    def default_status(self, entries):
        try:
            for entry in entries:
                self.value_bytes(entry)
        except (Exception, SystemExit):
            return "!", "Invalid value."
        if any(self.changed(e) for e in entries):
            return "*", "Pending change."
        if any(e["default_raw"] is None for e in entries):
            return "?", "Firmware default unavailable."
        try:
            custom = any(self.differs_from_default(e) for e in entries)
        except (Exception, SystemExit):
            return "!", "Invalid value."
        return ("✓", "Saved value differs from the firmware default.") if custom else ("", "At the firmware default.")

    def default_tooltip(self, entries):
        symbol, text = self.default_status(entries)
        if symbol == "?":
            return text + "\nThis firmware did not provide the default, so it cannot be restored here."
        if self.busy:
            return text + "\nWait for the current operation to finish."
        if symbol in ("*", "!"):
            text += "\nClick to discard this setting’s edits."
        elif symbol == "✓":
            text += "\nClick to stage the firmware default. Apply saves the change."
        if len(entries) > 1:
            text += "\nThese settings are handled together."
        return text

    def toggle_setting_status(self, entries):
        if self.busy:
            return
        if any(self.changed(e) for e in entries):
            self.discard_setting(entries)
        else:
            for entry in entries:
                self.reset_default(entry)

    def default_marker(self, parent, entry, record, target, name):
        entries = self.setting_entries(record, target, name, entry)
        widget = ttk.Button(parent, width=2, padding=0, command=lambda: self.toggle_setting_status(entries))
        self.default_buttons.append((entries, widget))
        widget.default_entries = entries
        widget.tooltip = Tooltip(widget, lambda: self.default_tooltip(entries))
        self.indicators.append((entry, widget))
        widget.configure(text=self.default_status(entries)[0])
        self.schedule_previews()
        return widget

    def color_preview(self, record, entry):
        raw = self.value_bytes(entry)
        if entry["field"]["type"] == pad.T_RGB and len(raw) == 3:
            rgb = "#" + raw.hex()
            return rgb
        if (entry["field"]["type"] == pad.T_COLOUR and len(raw) == 2) or (entry["field"]["type"] == pad.T_ENTRY and len(raw) == 1):
            palette, level = raw[0], raw[1] if len(raw) == 2 else 0
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
        self.paint_swatch(record, entry, swatch)
        self.schedule_previews()
        return swatch

    def paint_swatch(self, record, entry, swatch):
        try:
            colour = self.color_preview(record, entry)
            swatch.itemconfigure("colour", fill=colour)
            swatch.itemconfigure("invalid", text="")
        except (Exception, SystemExit):
            swatch.itemconfigure("colour", fill="#eeeeee")
            swatch.itemconfigure("invalid", text="?")

    def schedule_previews(self):
        """One update_previews once the page is built, not one per widget:
        each pass walks every widget on the page."""
        if not getattr(self, "previews_pending", None):
            self.previews_pending = self.root.after_idle(self.scheduled_previews)

    def scheduled_previews(self):
        self.previews_pending = None
        self.update_previews()

    def update_previews(self, force=False):
        self.indicators = [(e, w) for e, w in self.indicators if w.winfo_exists()]
        for entry, widget in self.indicators:
            symbol, _ = self.default_status(widget.default_entries)
            widget.configure(text=symbol)
        self.swatches = [(r, e, w) for r, e, w in self.swatches if w.winfo_exists()]
        for record, entry, swatch in self.swatches:
            self.paint_swatch(record, entry, swatch)

        self.special_editors = [w for w in self.special_editors if w.winfo_exists()]
        for widget in self.special_editors:
            if isinstance(widget, CurveEditor):
                force_curve = force is True or (force is not False and any(widget.entry is e for e in force))
                widget.sync(force=force_curve)
            else:
                widget.sync()
        self.unpair_buttons = [(e, w) for e, w in self.unpair_buttons if w.winfo_exists()]
        for entry, button in self.unpair_buttons:
            try:
                paired = self.has_pairing(self.value_bytes(entry))
            except (Exception, SystemExit):
                paired = True
            button.configure(state="normal" if paired and not self.busy else "disabled")
        self.default_buttons = [(entries, w) for entries, w in self.default_buttons if w.winfo_exists()]
        for entries, button in self.default_buttons:
            available = all(e["default_text"] is not None for e in entries)
            try:
                custom = any(self.differs_from_default(e) for e in entries)
            except (Exception, SystemExit):
                custom = True
            pending = any(self.changed(e) for e in entries)
            button.configure(state="normal" if not self.busy and (pending or (available and custom)) else "disabled")
        self.scope_buttons = [(r, t, action, w) for r, t, action, w in self.scope_buttons if w.winfo_exists()]
        for record, target, action, button in self.scope_buttons:
            entries = [e for (t, name), e in record.get("draft", {}).items() if target is None or t == target]
            pending = any(self.changed(e) for e in entries)
            available = action == "Discard" or (record.get("online", True) and not record.get("error"))
            if action == "Reset all to defaults":
                pending = bool(entries)
                available = not record.get("error") and all(e["default_text"] is not None for e in entries)
            button.configure(state="normal" if pending and available and not self.busy else "disabled")

    def scope_actions(self, parent, record, target=None):
        bar = ttk.Frame(parent)
        commands = {"Apply": lambda: self.apply_settings(device_path=record["path"], target=target),
                    "Discard": lambda: self.discard_scope(record["path"], target),
                    "Reset all to defaults": lambda: self.reset_scope_defaults(record["path"], target)}
        for action in ("Apply", "Discard", "Reset all to defaults"):
            button = ttk.Button(bar, text=action, command=commands[action], state="disabled")
            button.pack(side="left", padx=(0, 8))
            self.scope_buttons.append((record, target, action, button))
        self.schedule_previews()
        return bar

    def setting_entries(self, record, target, name, entry):
        companion = record["draft"].get((target, name.replace("_points", "_enabled"))) if name.endswith("_curve_points") else \
                    record["draft"].get((target, "rules")) if entry["field"]["kind"] == 3 and name == "map" else None
        return (entry, companion) if companion is not None else (entry,)

    def show_help(self, entry):
        field = entry["field"]
        text = field["help"]
        if field["type"] == pad.T_ENTRY:
            text = ("Choose one of the 16 colours from the pad's palette. How bright it is lit is the "
                    "palette's: steady, animation and connecting brightness on the Palette page. "
                    "Palette edits also update these previews. The Palette page Apply saves palette edits; the global toolbar Apply saves all devices’ edits.\n\n" + text)
        elif field["type"] == pad.T_COLOUR:
            text = ("Choose one of the 16 colours from the pad's palette, then select its brightness. "
                    "Previews show the full palette colour at every brightness level. "
                    "Palette edits also update these previews. The Palette page Apply saves palette edits; the global toolbar Apply saves all devices’ edits.\n\n" + text)
        elif field["name"] in ("left_curve_points", "right_curve_points"):
            text = ("These 15 points control stick output at 6.25% intervals from 6.25% through 93.75% travel. "
                    "Drag a point on the graph, move its slider, or enter an output percentage and press Enter. "
                    "Outputs stay in increasing order; zero and full travel stay fixed.\n\n"
                    "Linear is no curve at all: calibrated input passes through exactly. Any other curve is used as drawn. Presets are shared with the CLI. "
                    "Apply at the top of this page saves this remap slot; the toolbar Apply saves all devices’ pending edits.")
        messagebox.showinfo(field["name"].replace("_", " ").capitalize(), text or "No extra help is available.")

    def dirty_count(self):
        return sum(self.changed(e) for r in self.cache.values() for e in r.get("draft", {}).values())

    def update_summary(self):
        text = f"Loaded {sum(r.get('online', True) for r in self.cache.values())} device(s). {self.pending.get()}."
        if self.notice.get():
            text += " " + self.notice.get()
        self.summary.set(text)

    def restore_entries(self, entries):
        previous_batch = self.batch_edits
        self.batch_edits = True
        try:
            for entry in entries:
                entry.pop("unpair", None)
                entry.pop("custom_curve", None)
                if entry["var"].get() != entry["original"]:
                    entry["var"].set(entry["original"])
                if entry["reset"].get():
                    entry["reset"].set(False)
        finally:
            self.batch_edits = previous_batch

    def discard_setting(self, entries):
        if self.busy:
            return
        self.restore_entries(entries)
        self.mark_dirty(force_previews=entries)
        self.notice.set("")

    def discard_scope(self, path, target=None):
        record = self.cache.get(path)
        if self.busy or record is None:
            return
        entries = tuple(e for (t, name), e in record.get("draft", {}).items() if target is None or t == target)
        self.discard_setting(entries)

    def discard_changes(self):
        if self.busy:
            return
        self.restore_entries(entry for record in self.cache.values() for entry in record.get("draft", {}).values())
        online = {path: record for path, record in self.cache.items() if record.get("online", True)}
        if len(online) != len(self.cache):
            self.cache = online
            self.render_tree()
        self.mark_dirty(force_previews=True)
        self.notice.set("")

    def update_apply_state(self, count=None):
        if count is None:
            count = self.dirty_count()
        self.apply_all_button.configure(state="normal" if not self.busy and count else "disabled")

    def mark_dirty(self, force_previews=False):
        if self.batch_edits:
            return
        count = self.dirty_count()
        self.update_apply_state(count)
        self.pending.set(f"{count} pending change(s)" if count else "No pending changes")
        self.update_previews(force=force_previews)
        for node, (path, target) in self.nodes.items():
            record = self.cache.get(path)
            if not record or not self.tree.exists(node):
                continue
            dirty = any(self.changed(e) for (t, name), e in record.get("draft", {}).items()
                        if target is None or target == t)
            self.tree.item(node, tags=("dirty",) if dirty else ())

    def refresh_devices(self):
        """Look for devices now: only new or changed ones are read, and
        edits to devices still there are kept."""
        if not self.busy:
            self.looking = True
            self.update_tree_status()
            self.monitor.refresh()

    def scan(self, automatic=False, select_path=None):
        if self.busy:
            return
        if not automatic and self.dirty_count() and not messagebox.askyesno("Reload devices", "Discard pending edits and scan again?"):
            return
        def load():
            records, errors = [], []
            paths = sorted(set(pad.device_paths()) | self.monitor.manual_paths)
            for path, name, identity in pad.discover_devices(include_loaders=True, paths=paths):
                try:
                    records.append(read_device(path, name, identity))
                except (Exception, SystemExit) as error:
                    errors.append(f"{path}: {error}")
            return records, errors
        def display(result):
            records, errors = result
            self.looking = False
            if automatic:
                self.update_devices((self.monitor.generation,
                                     {r["path"]: r["identity"] for r in records}, records, errors))
                self.render_tree(select_path)
                self.notice.set("")
                return
            self.cache = {}
            for record in records:
                self.install_values(record, record["values"])
                self.cache[record["path"]] = record
            self.device.set("")
            self.render_tree(select_path)
            self.mark_dirty()
            self.notice.set("")
            self.sync_devices()
            for error in errors:
                self.log(error)
            if errors:
                messagebox.showerror("Some devices could not be loaded", "\n".join(errors))
        self.looking = True
        self.update_tree_status()
        self.run("", load, display)

    def sync_devices(self):
        self.monitor.known = {path: record["identity"] for path, record in self.cache.items() if record.get("online", True)}

    def park_device(self, path, record):
        record["online"] = False
        offline_path = "offline:" + record["identity"].hex()
        record["original_path"] = record.get("original_path", path)
        record["path"] = offline_path
        self.cache.pop(path)
        self.cache[offline_path] = record
        for node, (node_path, target) in list(self.nodes.items()):
            if node_path == path:
                self.nodes[node] = (offline_path, target)

    def merge_device(self, record):
        path = record["path"]
        old = self.cache.get(path)
        if old and old["identity"] != record["identity"]:
            if any(self.changed(entry) for entry in old.get("draft", {}).values()):
                self.park_device(path, old)
            old = None
        if old is None and record["identity"] is not None:
            matches = [r for r in self.cache.values() if not r.get("online", True) and r["identity"] == record["identity"]]
            if len(matches) == 1:
                old = matches[0]
                self.cache.pop(old["path"])
        pending = {key: (entry["var"].get(), entry["reset"].get(), entry.get("custom_curve"), entry.get("unpair", False))
                   for key, entry in old.get("draft", {}).items() if self.changed(entry)} if old and old["identity"] == record["identity"] else {}
        self.install_values(record, record["values"])
        self.cache[path] = record
        for key, (text, reset, custom, unpair) in pending.items():
            if key in record["draft"]:
                entry = record["draft"][key]
                entry["var"].set(text)
                entry["reset"].set(reset)
                entry["custom_curve"] = custom
                entry["unpair"] = unpair

    def update_devices(self, snapshot):
        generation, present, records, errors = snapshot
        changed = False
        for path, record in list(self.cache.items()):
            if path not in present or record["identity"] != present[path]:
                if any(self.changed(entry) for entry in record.get("draft", {}).values()):
                    changed = changed or record.get("online", True)
                    record["online"] = False
                    if path in present and record["identity"] != present[path]:
                        self.park_device(path, record)
                else:
                    self.cache.pop(path)
                    changed = True
        for record in records:
            self.merge_device(record)
            changed = True
        if changed:
            self.render_tree()
            self.mark_dirty()
        self.sync_devices()
        for error in errors:
            self.log(error)

    def open_device_path(self):
        if self.busy:
            return
        path = simpledialog.askstring("Open device", "Device path (for example /dev/sg2 or \\\\.\\PhysicalDrive2):", parent=self.root)
        if not path or not path.strip():
            return
        path = path.strip()
        def load():
            devices = list(pad.discover_devices(include_loaders=True, paths=[path]))
            if not devices:
                raise ValueError(f"No supported pad, dongle or UBOOT1.00 device found at {path}")
            return read_device(*devices[0])
        def display(record):
            self.monitor.manual_paths = self.monitor.manual_paths | {path}
            self.merge_device(record)
            self.sync_devices()
            self.render_tree(select_path=path)
            self.mark_dirty()
        self.run("Opening device…", load, display)

    def update_tree_status(self):
        """Above the device tree: that we are looking, or that nothing is there."""
        text = "Looking…" if self.looking else "None found" if not self.cache else ""
        if not self.cache and not self.device.get():
            self.render_overview()
        self.tree_status.configure(text=text)

    def render_tree(self, select_path=None):
        selected = self.tree.selection()
        previous = self.nodes.get(selected[0]) if selected else None
        self.tree.delete(*self.tree.get_children())
        self.nodes = {}
        for i, (path, record) in enumerate(self.cache.items()):
            device_node = f"device{i}"
            port = record.get("location") or usb_location(path)
            location = f"USB {port} · " if port else ""
            label = "ROM loader (UBOOT1.00)" if record["board"] == "loader" else record["board"].title()
            if not record.get("online", True):
                label += " (disconnected)"
            self.tree.insert("", "end", iid=device_node, text=f"{location}{label} · {record.get('original_path', path)}", open=True)
            self.nodes[device_node] = (path, None)
            targets = pad.targets(record["fields"]) if record["board"] == "pad" else []
            for t in targets:
                node = f"{device_node}/{t}"
                self.tree.insert(device_node, "end", iid=node, text=self.target_label(record, t))
                self.nodes[node] = (path, t)
        nodes = self.tree.get_children()
        if nodes:
            selected = next((node for node, key in self.nodes.items() if key == ((select_path, None) if select_path else previous)), nodes[0])
            self.tree.selection_set(selected)
            self.select_node()
        else:
            self.device.set("")
            self.board.set("")
            self.render_overview()
            self.render_details()
            self.update_loader_actions()
            self.show_page(self.overview)
        self.update_tree_status()
        self.mark_dirty()

    @staticmethod
    def target_label(record, t):
        if t < 8:
            return f"Slot {pad.TARGETS[t]}"
        if t < pad.REMAP_TARGET:
            return pad.TARGETS[t].title()
        label = f"Remap {pad.SLOT_NAMES[t - pad.REMAP_TARGET]}"
        entry = record.get("draft", {}).get((t, "name"))
        name = entry["var"].get().strip() if entry else ""
        return f"{label} · {name}" if name else label

    def rename_node(self, record, t):
        for node, key in self.nodes.items():
            if key == (record["path"], t) and self.tree.exists(node):
                self.tree.item(node, text=self.target_label(record, t))

    def select_node(self, event=None):
        if self.busy or not self.tree.selection():
            return
        path, target = self.nodes[self.tree.selection()[0]]
        record = self.cache[path]
        self.device.set(path)
        self.board.set(record["board"] if record["board"] in PIDS else "")
        if isinstance(target, int):
            self.target.set(pad.TARGETS[target])
        self.render_overview()
        self.render_details()
        self.update_loader_actions()
        main = self.loader_page if record["board"] == "loader" else self.dongle_page if record["board"] == "dongle" else self.overview
        self.show_page(main if target is None else self.settings)
        if record["board"] == "loader" and self.auto_identify and not record.get("id_checked"):
            record["id_checked"] = True
            self.root.after_idle(self.detect_loader_product_id)

    def scroll_form(self, parent):
        frame = ttk.Frame(parent)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(0, weight=1)
        canvas = tk.Canvas(frame, highlightthickness=0)
        vertical = ttk.Scrollbar(frame, orient="vertical", command=canvas.yview)
        horizontal = ttk.Scrollbar(frame, orient="horizontal", command=canvas.xview)
        canvas.configure(yscrollcommand=vertical.set, xscrollcommand=horizontal.set)
        canvas.grid(row=0, column=0, sticky="nsew")
        vertical.grid(row=0, column=1, sticky="ns")
        horizontal.grid(row=1, column=0, sticky="ew")
        horizontal.grid_remove()
        form = ttk.Frame(canvas)
        window = canvas.create_window((0, 0), window=form, anchor="nw")
        self.responsive_rows[parent] = []

        def wrap_labels(widget, width):
            for child in widget.winfo_children():
                if isinstance(child, ttk.Label) and float(child.cget("wraplength") or 0) > 0:
                    if not hasattr(child, "original_wraplength"):
                        child.original_wraplength = int(float(child.cget("wraplength")))
                    child.configure(wraplength=min(child.original_wraplength, max(120, width - 32)))
                wrap_labels(child, width)

        def update_region(event=None):
            available = canvas.winfo_width()
            wrap_labels(form, available)
            for layout in self.responsive_rows[parent]:
                layout(available)
            width = max(available, form.winfo_reqwidth())
            if parent is self.settings:
                width = max(width, self.details_header.winfo_reqwidth())
                height = self.details_header.winfo_reqheight()
                self.header_canvas.configure(height=height, scrollregion=(0, 0, width, height))
                if int(self.header_canvas.itemcget(self.header_window, "width")) != width:
                    self.header_canvas.itemconfigure(self.header_window, width=width)
            if int(canvas.itemcget(window, "width")) != width:
                canvas.itemconfigure(window, width=width)
            if width > available:
                horizontal.grid()
            else:
                horizontal.grid_remove()
            bounds = canvas.bbox(window)
            if bounds:
                canvas.configure(scrollregion=(0, 0, width, max(bounds[3], canvas.winfo_height())))

        def resize(event):
            update_region()

        form.bind("<Configure>", update_region)
        canvas.bind("<Configure>", resize)
        if parent is self.settings:
            # Keep the title/actions fixed vertically, but scroll them with the
            # settings horizontally when a tiling window is forced too narrow.
            self.header_canvas.pack_configure(padx=(0, vertical.winfo_reqwidth()))
            self.details_header.bind("<Configure>", update_region)

            def horizontal_position(first, last):
                horizontal.set(first, last)
                self.header_canvas.xview_moveto(float(first))

            canvas.configure(xscrollcommand=horizontal_position)
        self.scroll_canvases[parent] = canvas
        self.scroll_updaters[parent] = update_region
        return form

    def wrap_row(self, parent, widgets, page):
        for widget in widgets:
            if widget.winfo_manager() == "pack":
                widget.pack_forget()
        def layout(width):
            row, column, used = 0, 0, 0
            for widget in widgets:
                needed = widget.winfo_reqwidth() + 8
                if column and used + needed > max(1, width - 24):
                    row, column, used = row + 1, 0, 0
                info = widget.grid_info()
                if not info or info["row"] != row or info["column"] != column:
                    widget.grid(row=row, column=column, sticky="w", padx=(0, 8), pady=(0, 4))
                used += needed
                column += 1
        self.responsive_rows[page].append(layout)
        layout(self.scroll_canvases[page].winfo_width())

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
        horizontal = bool(getattr(event, "state", 0) & 1)
        target = event.widget if isinstance(event.widget, tk.Text) else self.scroll_canvases.get(self.active_page)
        if target is not None:
            (target.xview_scroll if horizontal else target.yview_scroll)(steps, "units")
        return "break"

    def build_settings(self):
        self.overview_form = self.scroll_form(self.overview)
        self.header_canvas = tk.Canvas(self.settings, height=1, highlightthickness=0,
                                       background=ttk.Style().lookup("TFrame", "background") or self.root.cget("background"))
        self.header_canvas.pack(fill="x", pady=(0, 8))
        self.details_header = bar = ttk.Frame(self.header_canvas)
        self.header_window = self.header_canvas.create_window((0, 0), window=bar, anchor="nw")
        self.details_actions = None
        self.details_title = tk.StringVar(value="Select a slot or settings group in the tree")
        ttk.Label(bar, textvariable=self.details_title, font=("TkDefaultFont", 12, "bold")).pack(side="left")
        self.form = self.scroll_form(self.settings)

    def show_target(self, target):
        for node, key in self.nodes.items():
            if key == (self.device.get(), target):
                self.tree.selection_set(node)
                self.select_node()
                break

    def render_overview(self):
        self.responsive_rows[self.overview] = []
        for widget in self.overview_form.winfo_children():
            widget.destroy()
        f = self.overview_form
        record = self.record()
        if not record:
            ttk.Label(f, text="Looking for devices…" if self.looking else
                      "Connect a pad, dongle or ROM loader, then press Refresh, or use Open device path.", wraplength=800).pack(anchor="w", pady=12)
            return
        if record["board"] == "loader":
            self.loader_title.set(f"ROM loader (UBOOT1.00) · {record['path']}")
            self.product_id.set(record.get("product_id") or ("Unavailable" if record.get("id_checked") else "Not read"))
            return
        if record["board"] == "dongle":
            self.dongle_title.set(f"Dongle · {record['path']}")
            self.dongle_status.set(record["status"])
            return
        header = ttk.Frame(f)
        header.pack(fill="x", pady=(0, 4))
        ttk.Label(header, text=f"{record['board'].title()} · {record.get('original_path', record['path'])}",
                  font=("TkDefaultFont", 13, "bold")).pack(side="left")
        ttk.Label(header, text=record["status"] if record.get("online", True) else "Disconnected · pending edits retained").pack(side="left", padx=(16, 0))
        actions = ttk.Frame(f)
        actions.pack(fill="x", pady=(4, 8))
        self.enter_loader_button = ttk.Button(actions, text="Enter flash loader", command=self.enter_flash_loader,
                                              state="normal" if record.get("online", True) else "disabled")
        self.enter_loader_button.pack(side="left", padx=(0, 8))
        ttk.Button(actions, text="Show console", command=self.show_console,
                   state="normal" if record.get("online", True) else "disabled").pack(side="left", padx=(0, 8))
        if record["board"] == "pad":
            ttk.Button(actions, text="Back up pad settings…", command=self.backup_settings, state="normal" if record.get("online", True) else "disabled").pack(side="left", padx=(0, 8))
            ttk.Button(actions, text="Restore pad settings…", command=self.restore_settings, state="normal" if record.get("online", True) else "disabled").pack(side="left", padx=(0, 8))
        self.wrap_row(actions, actions.winfo_children(), self.overview)
        self.scope_actions(f, record).pack(anchor="w", pady=(0, 10))
        if record.get("error"):
            ttk.Label(f, text=record["error"]).pack(anchor="w", pady=8)
            return
        ttk.Label(f, text="Choose each slot's type here. Select a slot in the tree for transport, colours, response and other options.", wraplength=850).pack(anchor="w", pady=(0, 6))
        for target in range(8):
            row = ttk.Frame(f, padding=(0, 6))
            row.pack(fill="x")
            types = ttk.Frame(row)
            links = ttk.Frame(row)
            entry = record["draft"].get((target, "type"))
            if entry:
                self.default_marker(types, entry, record, target, "type").pack(side="left", padx=(0, 4))
            ttk.Label(types, text=f"Slot {pad.TARGETS[target]}", width=12).pack(side="left")
            if entry:
                ttk.Combobox(types, textvariable=entry["var"], values=entry["field"]["names"], state="readonly", width=20).pack(side="left")
            bond = record["draft"].get((target, "bond"))
            button = ttk.Button(links, text="Unpair", state="disabled", command=lambda t=target: self.unpair_slot(t))
            button.pack(side="left", padx=(0, 8))
            if bond is not None and int.from_bytes(record["identity"][4:6], "little") >= pad.UNPAIR_PROTOCOL:
                self.unpair_buttons.append((bond, button))
            types.grid(row=0, column=0, sticky="w", padx=(0, 8))
            links.grid(row=0, column=1, sticky="w")

    def render_details(self):
        self.responsive_rows[self.settings] = []
        for widget in self.form.winfo_children():
            widget.destroy()
        self.editors = {}
        if self.details_actions is not None:
            self.details_actions.destroy()
            self.details_actions = None
        record = self.record()
        supported = record and record["board"] == "pad" and not record.get("error")
        if not supported:
            self.details_title.set("Select a pad slot in the tree")
            return
        target = pad.target_index(self.target.get())
        self.details_title.set(f"{record['path']} · " + self.target_label(record, target).split(" · ")[0])
        advanced = ttk.Frame(self.form)
        self.details_actions = self.scope_actions(self.details_header, record, target)
        self.details_actions.pack(side="left", padx=(12, 0))
        rows = {self.form: 0, advanced: 0}
        sticks = {}                      # a remap page: each stick's settings in its own box
        self.stick_boxes = sticks
        for var, trace in getattr(self, "page_traces", ()):
            try:
                var.trace_remove("write", trace)
            except tk.TclError:
                pass
        self.page_traces, self.page_vars = [], []

        def mirror(entry, view, to_view, to_entry):
            """view shows entry's value; each follows the other."""
            def show(*_):
                if view.get() != to_view(entry["var"].get()):
                    view.set(to_view(entry["var"].get()))

            def store(*_):
                value = to_entry(view.get())
                if entry["var"].get() != value:
                    entry["var"].set(value)
            show()
            view.trace_add("write", store)
            self.page_traces.append((entry["var"], entry["var"].trace_add("write", show)))
            self.page_vars.append(view)

        def stick_box(side):
            if side not in sticks:
                box = sticks[side] = ttk.Frame(self.form, padding=(6, 4, 6, 6), relief="groove", borderwidth=2)
                box.grid(row=rows[self.form], column=0, columnspan=4, sticky="ew", padx=4, pady=(12, 0))
                box.columnconfigure(3, weight=1)
                box.columnconfigure(0, minsize=170)
                rows[self.form] += 1
                rows[box] = 0
            return sticks[side]
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
            if f["kind"] == 3 and name == "rules" and (t, "map") in record["draft"]:
                continue                     # one table with the map
            parent = advanced if f["dev"] else self.form
            side = name.split("_")[0] if target >= pad.REMAP_TARGET and name.startswith(("left_", "right_")) else None
            if side and not f["dev"]:
                parent = stick_box(side)
            row = rows[parent]
            rows[parent] += 1
            if f["kind"] == 3 and name == "map":
                editor = RemapEditor(parent, self, entry, record["draft"][target, "rules"],
                                     record["draft"].get((target, "name")), pad.TARGETS[target])
                editor.grid(row=row, column=0, columnspan=4, sticky="w", padx=4, pady=(12, 4))
                self.special_editors.append(editor)
                self.editors[name] = entry
                continue
            label = ttk.Frame(parent)
            label.grid(row=row, column=0, sticky="nw", padx=4, pady=8)
            self.default_marker(label, entry, record, target, name).pack(side="left", padx=(0, 4))
            shown = name[len(side) + 1:] if side and parent is sticks.get(side) else name
            in_box = side and parent is sticks.get(side)
            if in_box and shown == "maps_to":     # like a key's row: Left stick maps to [Right stick]
                ttk.Label(label, text=f"{side.capitalize()} stick", font=("TkDefaultFont", 10, "bold")).pack(side="left")
                ttk.Label(label, text="maps to").pack(side="left", padx=(4, 0))
            elif in_box and shown in ("x_axis", "y_axis"):
                ttk.Label(label, text=f"Invert {shown[0].upper()} axis").pack(side="left")
            else:
                ttk.Label(label, text=shown.replace("_", " ").capitalize()).pack(side="left")
            if in_box and shown == "maps_to":
                names = f["names"]
                view = tk.StringVar()
                mirror(entry, view, lambda v: v.capitalize(),
                       lambda v, names=names: next((n for n in names if n.capitalize() == v), names[0]))
                editor = ttk.Combobox(parent, textvariable=view, values=[n.capitalize() for n in names],
                                      state="readonly", width=14)
            elif in_box and shown in ("x_axis", "y_axis"):
                names = f["names"]
                view = tk.BooleanVar()
                mirror(entry, view, lambda v, names=names: v == names[1],
                       lambda on, names=names: names[1] if on else names[0])
                editor = ttk.Checkbutton(parent, variable=view)
            elif f["type"] in (pad.T_COLOUR, pad.T_ENTRY):
                editor = PaletteEditor(parent, self, record, entry)
                self.special_editors.append(editor)
            elif name in ("left_curve_points", "right_curve_points"):
                editor = CurveEditor(parent, self, entry, record["draft"].get((target, name.replace("_points", "_enabled"))))
                self.special_editors.append(editor)
            elif f["kind"] == 2 and name == "calibration":
                editor = CalibrationView(parent, self, entry)
                self.special_editors.append(editor)
            elif f["type"] == pad.T_ENUM:
                editor = ttk.Combobox(parent, textvariable=entry["var"], values=f["names"], state="readonly", width=23)
            else:
                editor = ttk.Entry(parent, textvariable=entry["var"], width=25)
            editor.grid(row=row, column=1, columnspan=2 if isinstance(editor, (PaletteEditor, CurveEditor, CalibrationView)) else 1,
                        sticky="w" if in_box and not isinstance(editor, CurveEditor) else "ew", padx=4, pady=4)
            extra = None
            if f["type"] == pad.T_NUMBER:
                maximum = f.get("max", (1 << (8 * f["size"])) - 1)
                extra = ttk.Label(parent, text=f"0–{maximum}", foreground="#666666")
                extra.grid(row=row, column=2, sticky="w", padx=6)
            if f["type"] == pad.T_RGB:
                extra = self.color_swatch(parent, record, entry)
                extra.grid(row=row, column=2, padx=4)
            actions = ttk.Frame(parent)
            actions.grid(row=row, column=3, sticky="ne", padx=4, pady=4)
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

    def apply_settings(self, only=None, *, device_path=None, target=None):
        if self.busy:
            return
        jobs = []
        try:
            allowed = {only} if only else None
            if only and only[2].endswith("_curve_points"):
                allowed.add((only[0], only[1], only[2].replace("_points", "_enabled")))

            def in_scope(path, key):
                return ((device_path is None or path == device_path) and
                        (target is None or key[0] == target) and
                        (allowed is None or (path, *key) in allowed))

            selected_entries = {id(entry) for path, record in self.cache.items()
                                for key, entry in record.get("draft", {}).items() if in_scope(path, key)}
            for widget in self.special_editors:
                if widget.winfo_exists() and isinstance(widget, CurveEditor) and id(widget.entry) in selected_entries:
                    widget.commit_pending()
            for path, record in self.cache.items():
                keys = {key for key, entry in record.get("draft", {}).items()
                        if self.changed(entry) and in_scope(path, key)}
                items = [(t, pad.UNPAIR_ID if entry.get("unpair") else entry["field"]["id"],
                          b"" if entry.get("unpair") or entry["reset"].get() else self.value_bytes(entry))
                         for (t, name), entry in record.get("draft", {}).items() if (t, name) in keys]
                if items:
                    if not record.get("online", True):
                        raise ValueError("A device with pending edits is disconnected. Reconnect it before applying.")
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
                    values = {t: pad.read_target(path, record["fields"], t) for t in pad.targets(record["fields"])}
                except (Exception, SystemExit) as error:
                    self.events.put(("applied", (path, None, keys)))
                    raise RuntimeError(f"{path}: settings saved, but reload failed: {error}")
                self.events.put(("applied", (path, values, keys)))
                self.flash_report(f"{path}: applied {len(items)} setting(s)")
        def finished(_):
            self.rerender_in_place()
            self.mark_dirty()
        self.run("Applying settings…" if only or device_path else "Applying pending changes to all edited devices…", apply, finished)

    def rerender_in_place(self):
        """Rebuild the overview and details of the same device and page,
        scrolled where they were (a rebuild would show their tops)."""
        pages = (self.overview, self.settings)
        where = {page: (self.scroll_canvases[page].xview()[0], self.scroll_canvases[page].yview()[0])
                 for page in pages if page in self.scroll_canvases}
        self.render_overview()
        self.render_details()
        self.root.update_idletasks()
        for page, (x, y) in where.items():
            self.scroll_updaters[page]()
            self.scroll_canvases[page].xview_moveto(x)
            self.scroll_canvases[page].yview_moveto(y)

    @staticmethod
    def has_pairing(bond):
        # bt_bond stores the Classic peer at 0 and the LE peer at 23.
        return any(bond[:6]) or any(bond[23:29])

    def unpair_slot(self, target):
        record = self.record()
        if self.busy or not record or record["board"] != "pad":
            return
        entry = record["draft"].get((target, "bond"))
        if entry is None:
            return
        if int.from_bytes(record["identity"][4:6], "little") < pad.UNPAIR_PROTOCOL:
            messagebox.showinfo("Update firmware", "Unpair with a new address requires updated firmware (protocol 6+).")
            return
        previous_batch = self.batch_edits
        self.batch_edits = True
        try:
            # Firmware generates and saves the replacement address only at COMMIT.
            # Discarding this draft does not touch the pad or consume an address.
            address = record["draft"].get((target, "address"))
            if address is not None:
                self.restore_entries((address,))
            entry["var"].set(bytes(len(entry["original_raw"])).hex())
            entry["unpair"] = True
        finally:
            self.batch_edits = previous_batch
        self.mark_dirty()

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
                raise ValueError("Device changed; wait for device detection before sending an action")
            pad.cmd(path, command, target if action != "calibrate" else 0)
        def finished(_):
            if action == "calibrate":
                self.notice.set("Calibration started.")
        self.run(action.title() + "…", perform, finished)

    def reset_all_defaults(self):
        self.reset_scope_defaults(self.device.get())

    def reset_scope_defaults(self, path, target=None):
        record = self.cache.get(path)
        if self.busy or not record or record["board"] != "pad":
            return
        entries = tuple(e for (t, name), e in record["draft"].items() if target is None or t == target)
        if record.get("error") or any(entry["default_text"] is None for entry in entries):
            messagebox.showerror("Defaults unavailable", "Reconnect this pad to load its defaults before resetting settings.")
            return
        previous_batch = self.batch_edits
        self.batch_edits = True
        try:
            for entry in entries:
                self.reset_default(entry)
        finally:
            self.batch_edits = previous_batch
        self.mark_dirty(force_previews=entries)
        self.notice.set("")

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

    def file_row(self, parent, row, label, var, directory=False):
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", pady=5)
        ttk.Entry(parent, textvariable=var).grid(row=row, column=1, sticky="ew", padx=8)
        def browse():
            path = filedialog.askdirectory() if directory else filedialog.askopenfilename()
            if path:
                var.set(path)
        ttk.Button(parent, text="Browse…", command=browse).grid(row=row, column=2)

    def build_loader_page(self):
        self.loader_form = self.scroll_form(self.loader_page)
        f = self.loader_form
        ttk.Label(f, textvariable=self.loader_title, font=("TkDefaultFont", 13, "bold")).pack(anchor="w", pady=(0, 8))
        identity = ttk.Frame(f)
        identity.pack(fill="x", pady=(0, 8))
        ttk.Label(identity, text="Product ID:").pack(side="left", padx=(0, 8))
        ttk.Label(identity, textvariable=self.product_id).pack(side="left", padx=(0, 8))
        ttk.Button(identity, text="Read product ID", command=self.detect_loader_product_id).pack(side="left")
        files = ttk.Frame(f)
        files.pack(fill="x")
        files.columnconfigure(1, weight=1)
        self.file_row(files, 0, "BR23 loader", self.loader)
        actions = ttk.Frame(f)
        actions.pack(fill="x", pady=8)
        self.backup_flash_button = ttk.Button(actions, text="Back up full flash…", command=self.backup_flash)
        self.backup_flash_button.pack(side="left", padx=(0, 8))
        self.flash_button = ttk.Button(actions, text="Flash Firmware…", command=self.choose_firmware)
        self.flash_button.pack(side="left")
        self.wrap_row(actions, actions.winfo_children(), self.loader_page)
        self.flash_progress_frame = ttk.Frame(f)
        self.flash_status = tk.StringVar()
        ttk.Label(self.flash_progress_frame, textvariable=self.flash_status).pack(anchor="w")
        self.flash_bar = ttk.Progressbar(self.flash_progress_frame, maximum=100)
        self.flash_bar.pack(fill="x", pady=(4, 0))
        ttk.Label(f, text="Choose a firmware image to flash this device. Chip-key conversion is automatic.\nBackup is optional; every written byte is verified. Reconnect USB to leave loader mode after a standalone backup.", wraplength=850).pack(anchor="w", pady=8)
        self.loader_output = self.console_output(f)

    def build_dongle_page(self):
        f = self.scroll_form(self.dongle_page)
        ttk.Label(f, textvariable=self.dongle_title, font=("TkDefaultFont", 13, "bold")).pack(anchor="w", pady=(0, 4))
        ttk.Label(f, text=f"Product ID: {PIDS['dongle']}").pack(anchor="w")
        ttk.Label(f, textvariable=self.dongle_status, wraplength=850).pack(anchor="w", pady=8)
        actions = ttk.Frame(f)
        actions.pack(fill="x", pady=8)
        for text, command in (("Enter flash loader", self.enter_flash_loader),
                              ("Pair dongle…", lambda: self.slot_action("pair", 0)),
                              ("Reboot", lambda: self.device_action("Reboot", lambda p: pad.cmd(p, 0x10))),
                              ("Show console", self.show_console)):
            ttk.Button(actions, text=text, command=command).pack(side="left")
        self.wrap_row(actions, actions.winfo_children(), self.dongle_page)
        target = ttk.Frame(f)
        target.pack(fill="x", pady=8)
        ttk.Label(target, text="Target address").pack(side="left", padx=(0, 8))
        ttk.Entry(target, textvariable=self.address, width=24).pack(side="left", padx=(0, 8))
        ttk.Button(target, text="Set target", command=self.set_dongle_target).pack(side="left")

    def open_loader(self, record, blob):
        loader = padflash.inspect_loader(record["path"], blob, report=self.flash_report,
                                         progress=self.flash_progress, expected_location=record.get("location"))
        self.events.put(("product_id", (record["path"], loader.product_id)))
        return loader

    def detect_loader_product_id(self):
        record = self.record()
        if self.busy or not record or record["board"] != "loader":
            return
        record["id_checked"] = True
        try:
            blob = self.loader_blob()
        except (ValueError, OSError) as error:
            self.product_id.set("Unavailable")
            self.log(str(error))
            return
        self.product_id.set("Reading…")
        self.start_flash_progress("Reading product ID")
        self.run("Reading product ID…", lambda: self.open_loader(record, blob))

    def update_loader_actions(self):
        record = self.record()
        in_loader = record is not None and record["board"] == "loader" and record.get("online", True)
        for button in (self.backup_flash_button, self.flash_button):
            button.configure(state="normal" if in_loader else "disabled")

    def choose_firmware(self):
        if self.busy or not self.require_loader():
            return
        file = filedialog.askopenfilename(title="Select firmware to flash", filetypes=[("Firmware", "*.img *.ufw"), ("All files", "*")])
        if not file:
            return
        if self.select_firmware_image(file):
            self.flash()

    def select_firmware_image(self, file):
        try:
            pid, _ = padflash.image_identity(padflash.load_image(file))
            board = next((name for name, value in PIDS.items() if value == pid), None)
            if board is None:
                raise ValueError("This firmware image is not for a supported pad or dongle.")
            record = self.record()
            if record and record.get("product_id") and record["product_id"] != pid:
                raise ValueError(f"Image product ID {pid} does not match this device ({record['product_id']}).")
        except (ValueError, OSError) as error:
            messagebox.showerror("Firmware", str(error))
            return False
        self.image.set(file)
        self.board.set(board)
        return True

    def require_loader(self):
        record = self.record()
        if record and record["board"] == "loader":
            return True
        messagebox.showinfo("Enter flash loader first", "Select a UBOOT1.00 device. Use Enter flash loader on running firmware, or connect the pad holding Home + Plus; it will appear automatically.")
        return False

    def start_flash_progress(self, stage):
        self.flash_active = True
        self.flash_status.set(stage)
        self.flash_progress_frame.pack(fill="x", pady=8, before=self.loader_output.master)
        self.flash_bar.configure(mode="indeterminate", value=0)
        self.flash_bar.start()
        if self.record() and self.record()["board"] == "loader":
            self.show_page(self.loader_page)
            self.scroll_canvases[self.loader_page].yview_moveto(0)

    def flash_progress(self, stage, done=0, total=None):
        self.events.put(("flash_progress", (stage, done, total)))

    def enter_flash_loader(self):
        record = self.record()
        if self.busy or not record or not record.get("online", True) or record["board"] not in PIDS:
            return
        if any(self.changed(entry) for entry in record.get("draft", {}).values()):
            messagebox.showinfo("Pending edits", "Apply or discard this device’s pending edits before entering flash loader.")
            return
        path, identity = record["path"], record["identity"]
        location = record.get("location") or usb_location(path)
        old_loaders = {r["path"] for r in self.cache.values() if r["board"] == "loader"}
        def enter():
            if pad.cmd(path, 0, length=64)[:40] != identity:
                raise ValueError("Device changed; wait for device detection before entering flash loader")
            self.flash_progress("Entering flash loader")
            try:
                pad.cmd(path, 0x11)
            except OSError:
                pass  # The reboot can disconnect before acknowledging.
            self.flash_progress("Waiting for UBOOT1.00")
            deadline = time.monotonic() + 30
            while True:
                loaders = [p for p, name, ident in pad.discover_devices(include_loaders=True) if ident is None]
                matches = [p for p in loaders if usb_location(p) == location] if location else [p for p in loaders if p not in old_loaders]
                if len(matches) == 1:
                    return matches[0]
                if len(matches) > 1:
                    raise ValueError("Multiple ROM loaders appeared. Select the intended device from the tree.")
                if time.monotonic() >= deadline:
                    raise OSError("UBOOT1.00 did not appear. Reconnect USB holding Home + Plus, then wait for device detection.")
                time.sleep(0.25)
        self.start_flash_progress("Entering flash loader")
        self.run("Entering flash loader…", enter, lambda path: self.scan(automatic=True, select_path=path))

    def loader_blob(self):
        blob = Path(self.loader.get()).read_bytes()
        if not blob:
            raise ValueError("Select a nonempty BR23 loader blob.")
        return blob

    def flash_report(self, message):
        self.events.put(("log", message))

    def backup_flash(self):
        if self.busy or not self.require_loader():
            return
        try:
            blob = self.loader_blob()
        except (ValueError, OSError) as error:
            messagebox.showerror("Firmware", str(error))
            return
        file = filedialog.asksaveasfilename(defaultextension=".bin", initialfile="flash-backup.bin")
        if not file:
            return
        if Path(file).resolve() == Path(self.loader.get()).resolve():
            messagebox.showerror("Backup destination", "Choose a file different from the BR23 loader.")
            return
        record = self.record()
        def backup():
            loader = self.open_loader(record, blob)
            loader.backup(file)
            self.flash_report("Backup complete. Reconnect USB to leave the loader.")
        self.start_flash_progress("Backing up flash")
        self.run("Backing up flash…", backup)

    def flash(self):
        if self.busy or not self.require_loader():
            return
        record = self.record()
        try:
            blob = self.loader_blob()
            image = Path(self.image.get())
            data = padflash.load_image(image)
            pid, key = padflash.image_identity(data)
            board = next((b for b, value in PIDS.items() if value == pid), None)
            if board is None or (record.get("product_id") and record["product_id"] != pid):
                raise ValueError("Image product ID does not match this device.")
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
            if Path(backup).resolve() in (image.resolve(), Path(self.loader.get()).resolve()):
                messagebox.showerror("Backup destination", "Choose a backup file different from the firmware input files.")
                return
        def write():
            loader = self.open_loader(record, blob)
            loader.flash(data, board, backup)
        self.start_flash_progress("Preparing firmware")
        self.run("Backing up and flashing… keep USB connected" if backup else "Flashing… keep USB connected", write, lambda _: self.clear_settings())

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
        self.device_action("Set dongle target", lambda p: pad.scsi(p, bytes([pad.OP, 0x30]) + address, 0))

    def device_action(self, label, fn):
        path = self.path()
        if path:
            record = self.record()
            if record and record["board"] == "loader":
                return
            def perform():
                if record and pad.cmd(path, 0, length=64)[:40] != record["identity"]:
                    raise ValueError("Device changed; wait for device detection before sending an action")
                return fn(path)
            self.run(label + "…", perform)



def elevate():
    """Windows: SCSI pass-through needs admin, so ask for it (the UAC prompt)
    and start again elevated.  Declined or --no-elevate: run as we are."""
    import ctypes
    import subprocess
    if sys.platform != "win32" or "--no-elevate" in sys.argv or ctypes.windll.shell32.IsUserAnAdmin():
        return False
    exe = Path(sys.executable)
    windowed = exe.with_name("pythonw.exe")
    params = subprocess.list2cmdline([str(Path(__file__).resolve()), *sys.argv[1:], "--no-elevate"])
    started = ctypes.windll.shell32.ShellExecuteW(None, "runas", str(windowed if windowed.exists() else exe),
                                                  params, str(Path(__file__).resolve().parent), 1)
    return started > 32


if __name__ == "__main__":
    if not elevate():
        App(tk.Tk()).root.mainloop()
