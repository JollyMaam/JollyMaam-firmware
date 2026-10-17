"""Tk editors for palette references and custom stick response curves."""
import tkinter as tk
import math
import re
import padcurves
from tkinter import ttk, messagebox

BRIGHTNESS = (0, 1, 2, 3, 4, 5, 7, 10, 14, 20, 28, 40, 56, 80, 128, 255)
BRIGHTNESS_LABELS = tuple('Off' if i == 0 else f'{i}/15' for i in range(len(BRIGHTNESS)))


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
        ttk.Label(self, text='Brightness').pack(side='left', padx=(6, 4))
        self.level = tk.StringVar()
        self.brightness = ttk.Combobox(self, textvariable=self.level, values=BRIGHTNESS_LABELS, state='readonly', width=7)
        self.brightness.pack(side='left')
        self.brightness.bind('<<ComboboxSelected>>', self.choose_brightness)
        self.sync()

    def current(self):
        raw = self.app.value_bytes(self.entry)
        if len(raw) != 2 or raw[0] > 15 or raw[1] > 15:
            raise ValueError('Invalid palette reference')
        return raw

    def sync(self):
        try:
            palette, level = self.current()
            self.palette_button.configure(text=f'Palette {palette}')
            self.level.set(BRIGHTNESS_LABELS[level])
            self.draw_palette()
        except (Exception, SystemExit):
            self.palette_button.configure(text='Choose colour…')
            self.level.set('')

    def choose_colour(self, palette):
        if self.app.busy:
            return
        _, level = self.current()
        self.entry['var'].set(f'{palette}:{level}')

    def choose_brightness(self, event=None):
        if self.app.busy:
            return
        palette, _ = self.current()
        self.entry['var'].set(f'{palette}:{BRIGHTNESS_LABELS.index(self.level.get())}')

    def draw_palette(self):
        try:
            selected, _ = self.current()
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
    """Tool presets and monotone points, with fixed zero/full endpoints."""
    def __init__(self, parent, app, entry, enabled=None):
        super().__init__(parent)
        self.app, self.entry, self.enabled = app, entry, enabled
        self.count = entry["field"]["size"]
        self.syncing = False
        self.dragging = None
        bar = ttk.Frame(self)
        bar.grid(row=0, column=0, columnspan=2, sticky='w', pady=(0, 6))
        if enabled:
            ttk.Checkbutton(bar, text='Use points', variable=enabled['var'], onvalue='enabled', offvalue='disabled').pack(side='left', padx=(0, 10))
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
        if self.enabled:
            self.enabled['var'].set('enabled')
        self.sync()

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
            self.caption.configure(text='Curve active. Drag points or open Point values.' if active else 'Points disabled: input passes through unchanged. Your saved points are kept and can still be edited.')
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
