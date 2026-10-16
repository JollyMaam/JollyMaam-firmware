"""Tk editors for palette references and custom stick response curves."""
import tkinter as tk
import math
from tkinter import ttk

BRIGHTNESS = (0, 1, 2, 3, 4, 5, 7, 10, 14, 20, 28, 40, 56, 80, 128, 255)
BRIGHTNESS_LABELS = tuple('Off' if i == 0 else f'{i} · {v / 255:.1%}' for i, v in enumerate(BRIGHTNESS))


class PaletteEditor(ttk.Frame):
    def __init__(self, parent, app, record, entry):
        super().__init__(parent)
        self.app, self.record, self.entry = app, record, entry
        self.popup = None
        self.palette_button = ttk.Button(self, text='Choose colour…', command=self.open_palette, width=16)
        self.palette_button.pack(side='left')
        self.preview = app.color_swatch(self, record, entry)
        self.preview.pack(side='left', padx=6)
        ttk.Label(self, text='Brightness').pack(side='left', padx=(6, 4))
        self.level = tk.StringVar()
        self.brightness = ttk.Combobox(self, textvariable=self.level, values=BRIGHTNESS_LABELS, state='readonly', width=13)
        self.brightness.pack(side='left')
        self.brightness.bind('<<ComboboxSelected>>', self.choose_brightness)
        self.bind('<Destroy>', self.destroy_popup, add=True)
        self.sync()

    def current(self):
        raw = self.app.value_bytes(self.entry)
        if len(raw) != 2 or raw[0] > 15 or raw[1] > 15:
            raise ValueError('Invalid palette reference')
        return raw

    def sync(self):
        try:
            palette, level = self.current()
            self.palette_button.configure(text=f'Palette {palette} ▾')
            self.level.set(BRIGHTNESS_LABELS[level])
            if self.popup and self.popup.winfo_exists():
                self.draw_palette()
        except (Exception, SystemExit):
            self.palette_button.configure(text='Choose colour…')
            self.level.set('')

    def choose_colour(self, palette):
        if self.app.busy:
            return
        _, level = self.current()
        self.entry['var'].set(f'{palette}:{level}')
        self.destroy_popup()

    def choose_brightness(self, event=None):
        if self.app.busy:
            return
        palette, _ = self.current()
        self.entry['var'].set(f'{palette}:{BRIGHTNESS_LABELS.index(self.level.get())}')

    def open_palette(self):
        if self.app.busy:
            return
        if self.popup and self.popup.winfo_exists():
            self.popup.lift()
            return
        self.popup = tk.Toplevel(self)
        self.popup.title('Choose a palette colour')
        self.popup.transient(self.winfo_toplevel())
        self.popup.resizable(False, False)
        self.popup.protocol('WM_DELETE_WINDOW', self.destroy_popup)
        ttk.Label(self.popup, text='Palette colour above · selected brightness below', padding=8).pack()
        grid = ttk.Frame(self.popup, padding=(8, 0, 8, 8))
        grid.pack()
        self.tiles = []
        for index in range(16):
            tile = tk.Canvas(grid, width=76, height=54, highlightthickness=0, cursor='hand2')
            tile.grid(row=index // 4, column=index % 4, padx=3, pady=3)
            tile.bind('<Button-1>', lambda e, i=index: self.choose_colour(i))
            tile.bind('<Return>', lambda e, i=index: self.choose_colour(i))
            tile.configure(takefocus=True)
            self.tiles.append(tile)
        self.draw_palette()
        self.popup.geometry(f'+{self.winfo_rootx()}+{self.winfo_rooty() + self.winfo_height()}')
        self.popup.bind('<Escape>', lambda e: self.destroy_popup())

    def draw_palette(self):
        selected, level = self.current()
        for index, tile in enumerate(self.tiles):
            tile.delete('all')
            try:
                rgb = self.app.value_bytes(self.record['draft'][8, f'palette{index}'])
                colour = '#' + rgb.hex()
                dim = bytes((c * BRIGHTNESS[level] + 127) // 255 for c in rgb)
                brightness = '#' + dim.hex()
                foreground = '#ffffff' if sum(dim) < 380 else '#111111'
                tile.create_rectangle(2, 2, 74, 18, fill=colour, outline='')
                tile.create_rectangle(2, 18, 74, 52, fill=brightness, outline='')
                tile.create_text(38, 35, text=f'{index}' + (' ✓' if index == selected else ''), fill=foreground)
            except (Exception, SystemExit):
                tile.create_text(38, 27, text=f'{index}: invalid')
            if index == selected:
                tile.create_rectangle(1, 1, 75, 53, outline='#2185d0', width=3)

    def destroy_popup(self, event=None):
        if event is not None and event.widget is not self:
            return
        popup, self.popup = self.popup, None
        if popup and popup.winfo_exists():
            popup.destroy()


class CurveEditor(ttk.Frame):
    """Seven monotone output points, with fixed zero and full-scale endpoints."""
    def __init__(self, parent, app, entry):
        super().__init__(parent)
        self.app, self.entry = app, entry
        self.syncing = False
        self.dragging = None
        self.graph = tk.Canvas(self, width=250, height=190, background='#ffffff', highlightthickness=1, highlightbackground='#cccccc')
        self.graph.grid(row=0, column=0, sticky='nw', padx=(0, 10))
        controls = ttk.Frame(self)
        controls.grid(row=0, column=1, sticky='nw')
        ttk.Label(controls, text='Stick travel').grid(row=0, column=0)
        ttk.Label(controls, text='Output %').grid(row=0, column=1, columnspan=2)
        self.sliders, self.outputs, self.spins = [], [], []
        for index in range(7):
            ttk.Label(controls, text=f'{(index + 1) * 12.5:g}%').grid(row=index + 1, column=0, sticky='e', padx=(0, 6))
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
        ttk.Label(self, text='Drag the graph or adjust outputs. Select the custom curve mode to use these points.',
                  wraplength=460).grid(row=1, column=0, columnspan=2, sticky='w', pady=(5, 0))
        self.graph.bind('<Button-1>', self.start_drag)
        self.graph.bind('<B1-Motion>', self.drag)
        self.graph.bind('<ButtonRelease-1>', lambda e: setattr(self, 'dragging', None))
        self.sync()

    def points(self):
        raw = self.app.value_bytes(self.entry)
        if len(raw) != 7 or any(raw[i] > raw[i + 1] for i in range(6)):
            raise ValueError('Curve outputs must increase with stick travel')
        return list(raw)

    def set_point(self, index, percent):
        if self.syncing or self.app.busy:
            return
        points = self.points()
        lower, upper = points[index - 1] if index else 0, points[index + 1] if index < 6 else 255
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
        self.dragging = max(0, min(6, round((event.x - 35) / 195 * 8) - 1))
        self.drag(event)

    def drag(self, event):
        if self.dragging is not None:
            self.set_point(self.dragging, (160 - event.y) / 140 * 100)

    def sync(self, force=False):
        self.syncing = True
        try:
            points = self.points()
            for i, value in enumerate(points):
                self.sliders[i].set(value / 255 * 100)
                if force or self.entry['reset'].get() or self.focus_get() is not self.spins[i]:
                    self.outputs[i].set(f'{value / 255 * 100:.1f}')
            self.graph.delete('all')
            for percent in (0, 25, 50, 75, 100):
                x, y = 35 + 195 * percent / 100, 160 - 140 * percent / 100
                self.graph.create_line(x, 20, x, 160, fill='#eeeeee')
                self.graph.create_line(35, y, 230, y, fill='#eeeeee')
                self.graph.create_text(22, y, text=str(percent), fill='#666666')
                self.graph.create_text(x, 173, text=str(percent), fill='#666666')
            self.graph.create_text(133, 187, text='Stick travel %', fill='#666666')
            self.graph.create_text(133, 8, text='Output %', fill='#666666')
            self.graph.create_line(35, 160, 230, 20, fill='#bbbbbb', dash=(4, 4))
            coords = [35, 160]
            for i, value in enumerate(points):
                coords += [35 + (i + 1) / 8 * 195, 160 - value / 255 * 140]
            coords += [230, 20]
            self.graph.create_line(*coords, fill='#2185d0', width=2)
            for i in range(1, 8):
                x, y = coords[i * 2:i * 2 + 2]
                self.graph.create_oval(x - 4, y - 4, x + 4, y + 4, fill='#2185d0', outline='#ffffff')
        except (Exception, SystemExit):
            self.graph.delete('all')
            self.graph.create_text(125, 90, text='Invalid curve points. Use Reset to restore defaults.', width=210)
        finally:
            self.syncing = False
