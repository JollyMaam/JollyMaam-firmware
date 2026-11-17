"""Tk editors for palette references, custom stick response curves and remap slots."""
import tkinter as tk
import json
import math
import re
import padcurves
import padremap
from tkinter import ttk, messagebox, filedialog


class Tooltip:
    """Delayed hover text, including on disabled controls; owned by its widget."""
    def __init__(self, widget, text):
        self.widget, self.text = widget, text
        self.timer = None
        self.window = None
        widget.bind('<Enter>', self.schedule, add='+')
        for event in ('<Leave>', '<ButtonPress>', '<Unmap>', '<Destroy>'):
            widget.bind(event, self.hide, add='+')

    def schedule(self, event=None):
        self.hide()
        self.timer = self.widget.after(450, self.show)

    def show(self):
        self.timer = None
        if not self.widget.winfo_exists() or not self.widget.winfo_ismapped():
            return
        self.window = window = tk.Toplevel(self.widget)
        window.withdraw()
        window.overrideredirect(True)
        window.attributes('-topmost', True)
        ttk.Label(window, text=self.text(), padding=7, wraplength=360,
                  relief='solid', borderwidth=1).pack()
        window.update_idletasks()
        x = min(self.widget.winfo_pointerx() + 12, window.winfo_screenwidth() - window.winfo_reqwidth())
        y = min(self.widget.winfo_pointery() + 18, window.winfo_screenheight() - window.winfo_reqheight())
        window.geometry(f'+{max(0, x)}+{max(0, y)}')
        window.deiconify()

    def hide(self, event=None):
        if self.timer is not None:
            self.widget.after_cancel(self.timer)
            self.timer = None
        if self.window is not None:
            self.window.destroy()
            self.window = None


class PaletteEditor(ttk.Frame):
    def __init__(self, parent, app, record, entry):
        super().__init__(parent)
        self.app, self.record, self.entry = app, record, entry
        self.palette_menu = tk.Menu(self, tearoff=False, postcommand=self.draw_palette)
        self.palette_images = [tk.PhotoImage(master=self, width=22, height=16) for _ in range(16)]
        for index, swatch in enumerate(self.palette_images):
            self.palette_menu.add_command(label=f'Palette {index}', image=swatch, compound='left',
                                          command=lambda i=index: self.choose_colour(i))
        self.palette_button = ttk.Menubutton(self, text='Choose colour…', menu=self.palette_menu, direction='below', width=14)
        self.palette_button.pack(side='left')
        self.preview = app.color_swatch(self, record, entry)
        self.preview.pack(side='left', padx=6)
        # How bright the entry is lit is the palette's.
        self.sync()

    def current(self):
        raw = self.app.value_bytes(self.entry)
        if len(raw) != 1 or raw[0] > 15:
            raise ValueError('Invalid palette reference')
        return raw[0]

    def sync(self):
        try:
            self.palette_button.configure(text=f'Palette {self.current()}')
            self.draw_palette()
        except (Exception, SystemExit):
            self.palette_button.configure(text='Choose colour…')

    def choose_colour(self, palette):
        if self.app.busy:
            return
        self.entry['var'].set(str(palette))

    def draw_palette(self):
        try:
            selected = self.current()
        except (Exception, SystemExit):
            selected = None
        for index, swatch in enumerate(self.palette_images):
            try:
                rgb = self.app.value_bytes(self.record['draft'][8, f'palette{index}'])
                if len(rgb) != 3:
                    raise ValueError('Invalid palette colour')
                colour, state = '#' + rgb.hex(), 'normal'
                label = f'Palette {index}' + (' ✓' if index == selected else '')
            except (Exception, SystemExit):
                colour, state, label = '#cccccc', 'disabled', f'Palette {index} (invalid)'
            swatch.put(colour, to=(1, 1, 21, 15))
            self.palette_menu.entryconfigure(index, label=label, state=state)


class CurveEditor(ttk.Frame):
    """Tool presets and monotone points, with fixed zero/full endpoints.  The
    enable flag follows the points: Linear is off (calibrated input exactly),
    any other curve on."""
    def __init__(self, parent, app, entry, enabled=None):
        super().__init__(parent)
        self.app, self.entry, self.enabled = app, entry, enabled
        self.count = entry["field"]["size"]
        self.syncing = False
        self.dragging = None
        bar = ttk.Frame(self)
        bar.grid(row=0, column=0, columnspan=2, sticky='w', pady=(0, 6))
        ttk.Label(bar, text='Preset').pack(side='left', padx=(0, 4))
        self.preset_name = tk.StringVar()
        self.presets = presets = ttk.Combobox(bar, textvariable=self.preset_name, values=['Linear', 'Concave', 'S-curve', 'Custom'], state='readonly', width=11, exportselection=False)
        presets.pack(side='left')
        presets.bind('<<ComboboxSelected>>', self.choose_preset)
        self.point_toggle = ttk.Button(bar, text='Point values ▸', command=self.toggle_points)
        self.point_toggle.pack(side='left', padx=8)
        ttk.Button(bar, text='Copy points', command=self.copy_points).pack(side='left', padx=(0, 4))
        ttk.Button(bar, text='Paste points', command=self.paste_points).pack(side='left')
        self.graph = tk.Canvas(self, width=280, height=210, background='#ffffff', highlightthickness=1, highlightbackground='#cccccc')
        self.graph.grid(row=1, column=0, sticky='nw', padx=(0, 10))
        controls = ttk.Frame(self)
        controls.grid(row=1, column=1, sticky='nw')
        ttk.Label(controls, text='Stick travel').grid(row=0, column=0)
        ttk.Label(controls, text='Output %').grid(row=0, column=1, columnspan=2)
        self.sliders, self.outputs, self.spins = [], [], []
        for index in range(self.count):
            ttk.Label(controls, text=f'{(index + 1) * 100 / (self.count + 1):g}%').grid(row=index + 1, column=0, sticky='e', padx=(0, 6))
            slider = ttk.Scale(controls, from_=0, to=100, length=100,
                               command=lambda v, i=index: self.set_point(i, float(v)))
            slider.grid(row=index + 1, column=1, padx=3, pady=2)
            output = tk.StringVar()
            spin = ttk.Spinbox(controls, from_=0, to=100, increment=1, width=5, textvariable=output,
                               command=lambda i=index: self.commit_output(i))
            spin.grid(row=index + 1, column=2)
            spin.bind('<Return>', lambda e, i=index: self.commit_output(i))
            spin.bind('<FocusOut>', lambda e, i=index: self.commit_output(i))
            self.sliders.append(slider)
            self.outputs.append(output)
            self.spins.append(spin)
            output.trace_add('write', lambda *_, i=index: self.edit_output(i))
        self.point_controls = controls
        self.point_controls.grid_remove()
        self.caption = ttk.Label(self, wraplength=460)
        self.caption.grid(row=2, column=0, columnspan=2, sticky='w', pady=(5, 0))
        self.graph.bind('<Button-1>', self.start_drag)
        self.graph.bind('<B1-Motion>', self.drag)
        self.graph.bind('<ButtonRelease-1>', lambda e: setattr(self, 'dragging', None))
        self.sync()

    def choose_preset(self, event=None):
        if self.app.busy:
            return
        selected = self.preset_name.get().lower()
        self.entry['custom_curve'] = selected == 'custom'
        if selected != 'custom':
            self.entry['var'].set(','.join(map(str, padcurves.preset(selected, self.count))))
        self.follow()
        self.sync()

    def follow(self):
        """The enable flag for the points: off for Linear, else on."""
        if not self.enabled:
            return
        try:
            state = 'disabled' if self.points() == list(padcurves.preset('linear', self.count)) else 'enabled'
        except (Exception, SystemExit):
            return
        if self.enabled['var'].get() != state:
            self.enabled['var'].set(state)

    def copy_points(self):
        if self.app.busy:
            return
        try:
            self.commit_pending()
            points = self.points()
        except (ValueError, OverflowError) as error:
            messagebox.showerror('Invalid curve points', str(error))
            return
        self.clipboard_clear()
        self.clipboard_append(','.join(map(str, points)))

    def paste_points(self):
        if self.app.busy:
            return
        try:
            values = [int(value) for value in re.split(r'[\s,]+', self.clipboard_get().strip())]
            points = padcurves.validate(values, self.count)
        except (ValueError, tk.TclError) as error:
            messagebox.showerror('Invalid curve points', f'Paste {self.count} nondecreasing integers from 0 to 255, separated by commas or spaces.\n{error}')
            return
        self.entry['custom_curve'] = True
        self.entry['var'].set(','.join(map(str, points)))
        self.follow()
        self.sync(force=True)

    def toggle_points(self):
        if self.point_controls.winfo_manager():
            self.point_controls.grid_remove()
            self.point_toggle.configure(text='Point values ▸')
        else:
            self.point_controls.grid()
            self.point_toggle.configure(text='Point values ▾')

    def points(self):
        return list(padcurves.validate(self.app.value_bytes(self.entry), self.count))

    def set_point(self, index, percent):
        if self.syncing or self.app.busy:
            return
        points = self.points()
        lower, upper = points[index - 1] if index else 0, points[index + 1] if index < self.count - 1 else 255
        value = max(lower, min(upper, round(max(0, min(100, percent)) * 255 / 100)))
        if points[index] == value:
            self.sync()
            return
        points[index] = value
        self.entry['var'].set(','.join(map(str, points)))
        self.follow()

    def edit_output(self, index):
        if self.syncing or self.app.busy:
            return
        try:
            value = float(self.outputs[index].get())
            if math.isfinite(value) and 0 <= value <= 100:
                self.set_point(index, value)
        except (ValueError, OverflowError):
            pass  # Allow partial typing; validate before applying.

    def commit_pending(self):
        for output in self.outputs:
            value = float(output.get())
            if not math.isfinite(value) or not 0 <= value <= 100:
                raise ValueError('Curve outputs must be percentages from 0 to 100')
        self.sync(force=True)

    def commit_output(self, index):
        if self.syncing or self.app.busy or not self.winfo_exists():
            return
        try:
            self.set_point(index, float(self.outputs[index].get()))
            self.sync(force=True)
        except (ValueError, OverflowError):
            self.sync(force=True)

    def start_drag(self, event):
        if self.app.busy:
            return
        self.dragging = max(0, min(self.count - 1, round((event.x - 55) / 195 * (self.count + 1)) - 1))
        self.drag(event)

    def drag(self, event):
        if self.dragging is not None:
            self.set_point(self.dragging, (160 - event.y) / 140 * 100)

    def sync(self, force=False):
        self.syncing = True
        try:
            points = self.points()
            active = not self.enabled or self.enabled['var'].get() == 'enabled'
            name = 'Custom' if self.entry.get('custom_curve') else padcurves.identify(points).capitalize()
            if self.preset_name.get() != name:
                self.preset_name.set(name)
            self.presets.selection_clear()
            linear = points == list(padcurves.preset('linear', self.count))
            self.caption.configure(text='Curve active. Drag points or open Point values.' if active else
                                   'Linear: calibrated input passes through exactly. Choose a preset or drag a point for a curve.' if linear else
                                   'Curve off (set elsewhere): input passes through unchanged. Edit a point to turn it on.')
            for i, value in enumerate(points):
                self.sliders[i].set(value / 255 * 100)
                if force or self.entry['reset'].get() or self.focus_get() is not self.spins[i]:
                    self.outputs[i].set(f'{value / 255 * 100:.1f}')
            self.graph.delete('all')
            for percent in (0, 25, 50, 75, 100):
                x, y = 55 + 195 * percent / 100, 160 - 140 * percent / 100
                self.graph.create_line(x, 20, x, 160, fill='#eeeeee')
                self.graph.create_line(55, y, 250, y, fill='#eeeeee')
                self.graph.create_text(43, y, text=str(percent), fill='#666666')
                self.graph.create_text(x, 173, text=str(percent), fill='#666666')
            self.graph.create_text(153, 197, text='Stick travel (%) →', fill='#444444')
            self.graph.create_text(12, 90, text='Output (%)', angle=90, fill='#444444')
            self.graph.create_line(55, 160, 255, 160, fill='#666666', arrow='last')
            self.graph.create_line(55, 160, 55, 15, fill='#666666', arrow='last')
            self.graph.create_line(55, 160, 250, 20, fill='#bbbbbb', dash=(4, 4))
            coords = [55, 160]
            for i, value in enumerate(points):
                coords += [55 + (i + 1) / (self.count + 1) * 195, 160 - value / 255 * 140]
            coords += [250, 20]
            self.graph.create_line(*coords, fill='#2185d0' if active else '#999999', width=2, dash=() if active else (4, 4))
            if not active:
                self.graph.create_line(55, 160, 250, 20, fill='#2185d0', width=2)
            for i in range(1, self.count + 1):
                x, y = coords[i * 2:i * 2 + 2]
                self.graph.create_oval(x - 4, y - 4, x + 4, y + 4, fill='#2185d0', outline='#ffffff')
        except (Exception, SystemExit):
            self.graph.delete('all')
            self.graph.create_text(125, 90, text='Invalid curve points. Use Default to restore defaults.', width=210)
        finally:
            self.syncing = False


# ---- remap slots ----------------------------------------------------------------

KEY_ORDER = ('south', 'east', 'west', 'north', 'up', 'down', 'left', 'right', 'l1', 'r1', 'l2', 'r2', 'l4', 'r4',
             'l3', 'r3', 'minus', 'plus', 'home', 'm', 'capture')
ROLE_LABELS = {k: (k.upper() if k in ('m', 'l1', 'r1', 'l2', 'r2', 'l4', 'r4', 'l3', 'r3') else k.capitalize())
               for k in KEY_ORDER}
KEY_LABELS = dict(ROLE_LABELS)


def use_layout(layout):
    """Name the keys as the pad prints them (padtool.read_layout; None: by
    role, the face buttons by where they sit).  A printed label another key
    also shows gets its role after it."""
    printed = {e['name']: e['label'] for e in layout or () if e['label'] and e['name'] in ROLE_LABELS}
    labels = {k: printed.get(k) or ROLE_LABELS[k] for k in KEY_ORDER}
    shown = list(labels.values())
    KEY_LABELS.clear()
    for k in KEY_ORDER:
        twice = shown.count(labels[k]) > 1 and k in printed
        KEY_LABELS[k] = f'{labels[k]} ({ROLE_LABELS[k]})' if twice else labels[k]


def slot_label(layout, key):
    """A connection or remap slot by the key that picks it (padtool.SLOT_NAMES):
    its role, after the pad's printed label where it has one ("B (south)")."""
    role = ROLE_LABELS[key]
    printed = next((e['label'] for e in layout or () if e['name'] == key and e['label']), None)
    return f'{printed} ({role.lower()})' if printed and printed.lower() != role.lower() else role


NOTHING = 'Nothing'
TRIGGER_LABELS = {'held': 'is held', 'held_for': 'is held for', 'pressed': 'is pressed',
                  'released': 'is released', 'tapped': 'is tapped under',
                  'double_tapped': 'is double-tapped within'}
EFFECT_LABELS = {'hold': 'hold', 'turbo': 'turbo', 'nothing': 'nothing', 'tap': 'tap',
                 'toggle_hold': 'toggle hold', 'toggle_turbo': 'toggle turbo',
                 'turbo_modifier': 'turbo modifier', 'toggle_turbo_modifier': 'toggle turbo modifier'}
# Switching between a level and an event trigger keeps the effect's kind.
EFFECT_TWINS = {'hold': 'tap', 'tap': 'hold', 'toggle_hold': 'hold', 'turbo': 'toggle_turbo',
                'toggle_turbo': 'turbo', 'turbo_modifier': 'toggle_turbo_modifier',
                'toggle_turbo_modifier': 'turbo_modifier'}
RATES = tuple(str(hz) for hz in range(1, padremap.MAX_HZ + 1)) + ('max',)


def key_of(label):
    return next(k for k, v in KEY_LABELS.items() if v == label)


def value_of(labels, label):
    return next(k for k, v in labels.items() if v == label)


class KeyPicker(ttk.Button):
    """A button naming the chosen keys; it opens a panel of check boxes that
    stays open while keys are ticked, until Done, Escape or a click outside."""
    COLUMNS = 4

    def __init__(self, parent, chosen, on_toggle, empty='Choose keys…', width=14):
        super().__init__(parent, width=width, command=self.toggle_panel)
        self.chosen, self.on_toggle, self.empty = list(chosen), on_toggle, empty
        self.panel = None
        self.bind('<Destroy>', lambda e: self.close() if e.widget is self else None, add='+')
        self.label()

    def label(self):
        self.configure(text='+'.join(KEY_LABELS[k] for k in KEY_ORDER if k in self.chosen) or self.empty)

    def toggle_panel(self):
        if self.panel:
            self.close()
        else:
            self.open()

    def open(self):
        panel = self.panel = tk.Toplevel(self)
        panel.withdraw()
        panel.overrideredirect(True)
        panel.transient(self.winfo_toplevel())
        frame = ttk.Frame(panel, padding=6, relief='solid', borderwidth=1)
        frame.pack(fill='both', expand=True)
        self.vars = {}
        for n, key in enumerate(KEY_ORDER):
            var = tk.BooleanVar(master=panel, value=key in self.chosen)
            self.vars[key] = var
            ttk.Checkbutton(frame, text=KEY_LABELS[key], variable=var,
                            command=lambda k=key, v=var: self.pick(k, v.get())
                            ).grid(row=n // self.COLUMNS, column=n % self.COLUMNS, sticky='w', padx=4, pady=1)
        ttk.Button(frame, text='Done', command=self.close).grid(
            row=len(KEY_ORDER) // self.COLUMNS + 1, column=0, columnspan=self.COLUMNS, sticky='e', pady=(6, 0))
        panel.bind('<Escape>', lambda e: self.close())
        panel.bind('<ButtonPress>', self.click)
        panel.geometry(f'+{self.winfo_rootx()}+{self.winfo_rooty() + self.winfo_height()}')
        panel.deiconify()
        panel.lift()
        self.grab(panel, 20)

    def grab(self, panel, tries):
        """Grab once the panel is on screen; never wait for it in a nested loop."""
        if panel is not self.panel or not panel.winfo_exists():
            return
        try:
            panel.grab_set()
            panel.focus_set()
        except tk.TclError:          # not viewable yet
            if tries:
                panel.after(25, lambda: self.grab(panel, tries - 1))

    def click(self, event):
        """Under the grab every click comes here; one outside the panel closes it."""
        panel = self.panel
        if not panel:
            return
        x, y = event.x_root - panel.winfo_rootx(), event.y_root - panel.winfo_rooty()
        if not (0 <= x < panel.winfo_width() and 0 <= y < panel.winfo_height()):
            self.close()

    def pick(self, key, on):
        self.chosen = [k for k in KEY_ORDER if (k == key and on) or (k != key and k in self.chosen)]
        self.label()
        self.on_toggle(key, on)

    def close(self):
        panel, self.panel = self.panel, None
        if panel and panel.winfo_exists():
            try:
                panel.grab_release()
            except tk.TclError:
                pass
            panel.destroy()


class RemapEditor(ttk.Frame):
    """A remap slot as one table: every key's base row (the map: sends itself,
    another key or nothing while held), each followed by its rules.  Rows are
    redrawn from the editor's own model, reloaded only when the entries change
    from outside (default, discard, apply)."""
    def __init__(self, parent, app, map_entry, rules_entry, name_entry=None, file_name='remap'):
        super().__init__(parent)
        self.app, self.entries, self.name_entry, self.file_name = app, (map_entry, rules_entry), name_entry, file_name
        self.shown, self.saving = None, False
        self.base, self.rules = None, []
        self.sync()

    def texts(self):
        return tuple(e['var'].get() for e in self.entries)

    def capacity(self):
        """The rules this pad keeps."""
        return self.entries[1]['field']['size'] // padremap.RULE_SIZE

    def sync(self):
        if self.saving or self.texts() == self.shown:
            return
        self.shown = self.texts()
        try:
            map_raw, rules_raw = (self.app.value_bytes(e) for e in self.entries)
            self.base, self.rules = padremap.decode_map(map_raw), padremap.decode_rules(rules_raw)
            self.original = self.decode('original_raw')
            self.default = self.decode('default_raw')
        except (Exception, SystemExit):
            self.base = None
        self.draw()

    def decode(self, which):
        raws = [e.get(which) for e in self.entries]
        if any(raw is None for raw in raws):
            return None
        return padremap.decode_map(raws[0]), padremap.decode_rules(raws[1])

    # ---- each key's status box: its mapping and its rules together ----

    def state(self, key, base=None, rules=None):
        base, rules = (self.base, self.rules) if base is None else (base, rules)
        return base[key], [padremap.show_rule(r) for r in rules if r['input'] == key]

    def status(self, key):
        if self.state(key) != self.state(key, *self.original):
            return '*', 'Pending change.\nClick to discard this key\'s edits.'
        if self.default is None:
            return '?', 'Firmware default unavailable.'
        if self.state(key) != self.state(key, *self.default):
            return '✓', 'Saved value differs from the firmware default.\nClick to stage the firmware default. Apply saves the change.'
        return '', 'At the firmware default.'

    def toggle(self, key):
        symbol, _ = self.status(key)
        source = self.original if symbol == '*' else self.default if symbol == '✓' else None
        if source is None:
            return

        def restore():
            base, rules = source
            self.base[key] = base[key]
            self.rules[:] = [r for r in self.rules if r['input'] != key] + \
                            [dict(r, outputs=list(r['outputs'])) for r in rules if r['input'] == key]
        self.change(restore)

    def refresh(self):
        self.count()
        for key, mark in self.marks.items():
            if mark.winfo_exists():
                mark.configure(text=self.status(key)[0])

    def save(self, redraw=True):
        texts = (padremap.show_map(padremap.encode_map(self.base)),
                 padremap.show_rules(padremap.encode_rules(self.rules, self.capacity())))
        self.saving = True
        try:
            for entry, text in zip(self.entries, texts):
                entry['var'].set(text)
        finally:
            self.saving = False
        self.shown = self.texts()
        if redraw:
            self.draw()
        else:
            self.refresh()

    def change(self, edit, redraw=True):
        if self.app.busy:
            self.draw()
            return
        edit()
        self.save(redraw)

    def count(self):
        if self.slots.winfo_exists():
            self.slots.configure(text=f'Rule entries: {len(self.rules)} of {self.capacity()} used')

    def draw(self):
        for widget in self.winfo_children():
            widget.destroy()
        self.keep = []                 # Tk variables live as long as their Python objects
        self.adds, self.marks, self.pickers = [], {}, []
        if self.base is None:
            ttk.Label(self, text='This remap cannot be read. Reset it to defaults.').grid(sticky='w')
            return
        bar = ttk.Frame(self)
        bar.grid(row=0, column=0, sticky='w', pady=(0, 8))
        presets = ttk.Menubutton(bar, text='Select preset ▾', direction='below')
        menu = tk.Menu(presets, tearoff=False)
        for name, (label, _) in padremap.PRESETS.items():
            menu.add_command(label=label, command=lambda n=name: self.preset(n))
        presets['menu'] = menu
        presets.pack(side='left')
        ttk.Button(bar, text='Import preset…', command=self.import_file).pack(side='left', padx=(8, 0))
        ttk.Button(bar, text='Export preset…', command=self.export_file).pack(side='left', padx=(8, 0))
        self.slots = ttk.Label(bar, foreground='#666666')
        self.slots.pack(side='left', padx=10)
        self.count()
        body = ttk.Frame(self)
        body.grid(row=1, column=0, sticky='w')
        probe = ttk.Button(body, width=2, padding=0)  # rules start past the status boxes
        probe.update_idletasks()
        indent = probe.winfo_reqwidth() + 6 + 8
        probe.destroy()
        full = len(self.rules) >= self.capacity()
        for key in KEY_ORDER:
            base = ttk.Frame(body)
            base.pack(anchor='w', pady=(6, 1))
            self.base_row(base, key, full)
            mine = [(index, rule) for index, rule in enumerate(self.rules) if rule['input'] == key]
            if mine:
                table = ttk.Frame(body)
                table.pack(anchor='w', padx=(indent, 0))
                for line, (index, rule) in enumerate(mine):
                    self.rule_row(table, line, index, rule)

    def combo(self, parent, values, value, width, on_change):
        var = tk.StringVar(value=value)
        box = ttk.Combobox(parent, textvariable=var, values=values, state='readonly', width=width)
        var.trace_add('write', lambda *_: on_change(var.get()))
        self.keep.append(var)
        return box

    def base_row(self, row, key, full):
        """[status] KEY maps to [key or Nothing] [Add rule]; fixed widths, so
        every base row lines up and nothing moves."""
        to = self.base[key]
        mark = ttk.Button(row, width=2, padding=0, text=self.status(key)[0], command=lambda: self.toggle(key))
        mark.pack(side='left', padx=(0, 6))
        Tooltip(mark, lambda: self.status(key)[1])
        self.marks[key] = mark
        ttk.Label(row, text=KEY_LABELS[key], width=max(map(len, KEY_LABELS.values())), anchor='e',
                  font=('TkDefaultFont', 10, 'bold')).pack(side='left')
        ttk.Label(row, text='maps to').pack(side='left', padx=(4, 4))
        self.combo(row, [KEY_LABELS[k] for k in KEY_ORDER] + [NOTHING], KEY_LABELS[to] if to else NOTHING, 14,
                   lambda v: self.change(lambda: self.base.update({key: None if v == NOTHING else key_of(v)}), redraw=False)
                   ).pack(side='left', padx=2)
        add = ttk.Button(row, text='Add rule', state='disabled' if full else 'normal',
                         command=lambda: self.change(lambda: self.rules.append(padremap.rule(key, 'held', 'hold', []))))
        add.pack(side='left', padx=(12, 0))
        Tooltip(add, lambda: f'Add a rule for {KEY_LABELS[key]}' if not full else f'All {self.capacity()} rule entries are used')
        self.adds.append(add)

    # A key's rules, a small table of their own under its base row: 0 delete
    # and When, 2 trigger, 3 time, 4 effect, 5 keys, 6 turbo rate.
    def rule_row(self, table, line, index, rule):
        when = ttk.Frame(table)
        when.grid(row=line, column=0, sticky='w', pady=1)
        remove = ttk.Button(when, text='✕', width=2, padding=0, command=lambda: self.change(lambda: self.rules.pop(index)))
        remove.pack(side='left', padx=(0, 6))
        Tooltip(remove, lambda: 'Delete this rule')
        ttk.Label(when, text='When').pack(side='left')
        ttk.Label(when, text=KEY_LABELS[rule['input']], font=('TkDefaultFont', 10, 'bold')).pack(side='left', padx=(4, 6))
        self.combo(table, list(TRIGGER_LABELS.values()), TRIGGER_LABELS[rule['trigger']], 20,
                   lambda v: self.change(lambda: self.retrigger(rule, value_of(TRIGGER_LABELS, v)))).grid(row=line, column=2, sticky='w', padx=2)
        if rule['trigger'] in padremap.TIMED:
            timed = ttk.Frame(table)
            timed.grid(row=line, column=3, sticky='w')
            ms = tk.StringVar(value=str(rule['ms']))
            self.keep.append(ms)
            spin = ttk.Spinbox(timed, from_=10, to=2550, increment=10, width=5, textvariable=ms,
                               command=lambda: self.set_ms(rule, ms))
            spin.pack(side='left', padx=2)
            spin.bind('<Return>', lambda e: self.set_ms(rule, ms))
            spin.bind('<FocusOut>', lambda e: self.set_ms(rule, ms))
            ttk.Label(timed, text='ms').pack(side='left')
        effects = [e for e in padremap.effects_for(rule['trigger']) if e != 'nothing' or rule['effect'] == 'nothing']
        self.combo(table, [EFFECT_LABELS[e] for e in effects], EFFECT_LABELS[rule['effect']], 12,
                   lambda v: self.change(lambda: rule.update(effect=value_of(EFFECT_LABELS, v)))).grid(row=line, column=4, sticky='w', padx=(8, 2))
        if rule['effect'] != 'nothing':
            modifier = rule['effect'] in padremap.MODIFIERS
            picker = KeyPicker(table, rule['outputs'], empty='Choose keys…',
                               on_toggle=lambda k, on: self.change(lambda: self.output(rule, k, on), redraw=False))
            picker.grid(row=line, column=5, sticky='w', padx=2)
            Tooltip(picker, lambda: 'The keys that pulse while this is on: whatever they send while you hold them'
                    if modifier else 'The keys this sends')
            self.pickers.append(picker)
        if rule['effect'] in padremap.TURBO:
            rate = ttk.Frame(table)
            rate.grid(row=line, column=6, sticky='w', padx=(12, 0))
            ttk.Label(rate, text='at').pack(side='left', padx=(0, 2))
            box = self.combo(rate, RATES, str(rule['rate']) if rule['rate'] else 'max', 4,
                             lambda v: self.change(lambda: rule.update(rate=0 if v == 'max' else int(v))))
            box.pack(side='left')
            ttk.Label(rate, text='Hz' if rule['rate'] else '(every report)').pack(side='left', padx=(2, 0))

    @staticmethod
    def retrigger(rule, trigger):
        rule['trigger'] = trigger
        if rule['effect'] not in padremap.effects_for(trigger):
            twin = EFFECT_TWINS.get(rule['effect'])
            rule['effect'] = twin if twin in padremap.effects_for(trigger) else padremap.effects_for(trigger)[0]
        if trigger in padremap.TIMED and not rule['ms']:
            rule['ms'] = padremap.TIMED[trigger]

    @staticmethod
    def output(rule, key, on):
        rule['outputs'] = [k for k in KEY_ORDER if (k == key and on) or (k != key and k in rule['outputs'])]

    def set_ms(self, rule, var):
        try:
            ms = max(10, min(2550, round(int(var.get()) / 10) * 10))
        except ValueError:
            ms = rule['ms']
        var.set(str(ms))
        if ms != rule['ms']:
            self.change(lambda: rule.update(ms=ms), redraw=False)

    def export_file(self):
        name = self.name_entry['var'].get().strip() if self.name_entry else ''
        path = filedialog.asksaveasfilename(parent=self, title='Export remap preset', defaultextension='.json',
                                            initialfile=(re.sub(r'[^\w.-]+', '-', name).strip('-') or self.file_name) + '.json',
                                            filetypes=[('Remap preset', '*.json'), ('All files', '*')])
        if not path:
            return
        data = padremap.export_preset(name, padremap.encode_map(self.base), padremap.encode_rules(self.rules))
        try:
            with open(path, 'w', encoding='utf-8') as file:
                json.dump(data, file, indent=1, ensure_ascii=False)
                file.write('\n')
        except OSError as error:
            messagebox.showerror('Export failed', str(error))

    def import_file(self):
        if self.app.busy:
            return
        path = filedialog.askopenfilename(parent=self, title='Import remap preset',
                                          filetypes=[('Remap preset', '*.json'), ('All files', '*')])
        if not path:
            return
        try:
            with open(path, encoding='utf-8') as file:
                name, map_raw, rules_raw = padremap.import_preset(json.load(file), self.capacity())
            if self.name_entry:
                padremap.parse_name(name, self.name_entry['field']['size'])
        except (OSError, ValueError) as error:
            messagebox.showerror('Import failed', str(error))
            return
        if self.name_entry and name:
            self.name_entry['var'].set(name)
        self.base, self.rules = padremap.decode_map(map_raw), padremap.decode_rules(rules_raw)
        self.save()

    def preset(self, name):
        self.change(lambda: self.base.update(padremap.apply_preset(self.base, name)))
