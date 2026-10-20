"""GUI/device boundary tests. Use an X display (Xvfb is sufficient). No USB I/O."""
import ctypes
import gc
import time
import tkinter as tk
import unittest
from unittest.mock import patch

from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path

# .pyw is a Windows GUI launch extension; Linux imports require an explicit loader.
_gui_spec = spec_from_loader("padgui", SourceFileLoader("padgui", str(Path(__file__).with_name("padgui.pyw"))))
padgui = module_from_spec(_gui_spec)
_gui_spec.loader.exec_module(padgui)
import padtransport
import padwidgets


def record(path="fake", board="pad", protocol=3):
    fields = [{"kind": 0, "id": 1, "name": "type", "type": padgui.pad.T_ENUM,
               "names": ["switch", "hid"], "help": "Profile", "dev": False},
              {"kind": 0, "id": 2, "name": "rumble", "type": padgui.pad.T_NUMBER,
               "size": 1, "help": "Strength", "dev": False}]
    identity = b"C2PD" + protocol.to_bytes(2, "little") + bytes([padgui.pad.BOARD_IDS[board], 0]) + b"build".ljust(16, b"\0") + path.encode().ljust(16, b"\0")[:16]
    values = {t: {"type": (b"\0", False), "rumble": (b"\x64", False)} if t < 8 else {}
              for t in range(10)} if board == "pad" else {}
    return {"path": path, "board": board, "identity": identity, "fields": fields if board == "pad" else [],
            "values": values, "status": "idle", "name": "BR23 UDISK"}


def remap_record(path="fake"):
    r = record(path)
    r["fields"] += [{"kind": 3, "id": 1, "name": "name", "type": padgui.pad.T_TEXT, "size": 24, "help": "Name", "dev": False},
                    {"kind": 3, "id": 2, "name": "map", "type": padgui.pad.T_BYTES, "size": 20, "help": "Map", "dev": False},
                    {"kind": 3, "id": 3, "name": "rules", "type": padgui.pad.T_BYTES, "size": 160, "help": "Rules", "dev": False}]
    sticks = []
    for i, side in enumerate(("left", "right")):
        names = ["left stick", "right stick", "nothing"]
        sticks += [(f"{side}_maps_to", names if side == "left" else [names[1], names[0], names[2]]),
                   (f"{side}_x_axis", ["normal", "inverted"]), (f"{side}_y_axis", ["normal", "inverted"])]
        r["fields"] += [{"kind": 3, "id": 12 + i, "name": f"{side}_maps_to", "type": padgui.pad.T_ENUM,
                         "names": sticks[-3][1], "size": 1, "help": "Maps to", "dev": False}]
        r["fields"] += [{"kind": 3, "id": 14 + 2 * i + a, "name": f"{side}_{axis}_axis", "type": padgui.pad.T_ENUM,
                         "names": ["normal", "inverted"], "size": 1, "help": "Axis", "dev": False}
                        for a, axis in enumerate("xy")]
        r["fields"] += [{"kind": 3, "id": 8 + 2 * i + n, "name": f"{side}_{which}_deadzone", "type": padgui.pad.T_NUMBER,
                         "size": 1, "max": 25, "help": "Deadzone", "dev": False}
                        for n, which in enumerate(("inner", "outer"))]
        r["fields"] += [{"kind": 3, "id": 4 + i, "name": f"{side}_curve_enabled", "type": padgui.pad.T_ENUM,
                         "names": ["disabled", "enabled"], "size": 1, "help": "Curve", "dev": False},
                        {"kind": 3, "id": 6 + i, "name": f"{side}_curve_points", "type": padgui.pad.T_BYTES,
                         "size": 15, "help": "Points", "dev": False}]
    linear = padgui.pad.padcurves.preset("linear")
    for t in range(10, 18):
        r["values"][t] = {"name": (bytes(24), False), "map": (bytes(20), False), "rules": (bytes(160), False),
                          "left_curve_enabled": (b"\0", False), "right_curve_enabled": (b"\0", False),
                          "left_curve_points": (linear, False), "right_curve_points": (linear, False),
                          "left_inner_deadzone": (b"\0", False), "left_outer_deadzone": (b"\0", False),
                          "right_inner_deadzone": (b"\0", False), "right_outer_deadzone": (b"\0", False)}
        r["values"][t].update({name: (b"\0", False) for name, _ in sticks})
    r["defaults"] = {t: dict(r["values"][t]) for t in range(10, 18)}
    return r


def loader_record(path="rom"):
    return {"path": path, "board": "loader", "identity": None, "fields": [], "values": {}, "defaults": {},
            "status": "UBOOT1.00 ROM loader", "name": "BR23 UBOOT1.00"}


class UnpairTests(unittest.TestCase):
    def test_cli_unpair_uses_firmware_action_and_rejects_unsupported_devices(self):
        for board, protocol in (("pad", 6), ("pad", 5), ("dongle", 6)):
            identity = record(board=board, protocol=protocol)["identity"]
            with self.subTest(board=board, protocol=protocol), \
                 patch.object(padgui.pad, "cmd", return_value=identity), \
                 patch.object(padgui.pad, "send") as send:
                if board == "pad" and protocol >= 6:
                    padgui.pad.unpair("fake", 1)
                    send.assert_called_once_with("fake", [(1, padgui.pad.UNPAIR_ID, b"")])
                else:
                    with self.assertRaises(ValueError):
                        padgui.pad.unpair("fake", 1)
                    send.assert_not_called()

    def test_ephemeral_unpair_journals_firmware_result_instead_of_repeating_action(self):
        import json
        import tempfile
        table = [{"kind": 0, "id": 12, "name": "address"},
                 {"kind": 0, "id": 13, "name": "bond"}]
        address = bytes.fromhex("102030405002")
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(padgui.pad, "JOURNAL", str(Path(directory) / "journal.json")), \
             patch.object(padgui.pad, "cmd", return_value=b"\x01\0"), \
             patch.object(padgui.pad, "ephemeral_now", return_value=True), \
             patch.object(padgui.pad, "fields", return_value=table), \
             patch.object(padgui.pad, "read_target", return_value={"address": (address, True), "bond": (bytes(68), False)}):
            padgui.pad.send("fake", [(1, padgui.pad.UNPAIR_ID, b"")])
            self.assertEqual(json.loads(Path(padgui.pad.JOURNAL).read_text()),
                             [[[1, 12, address.hex()], [1, 13, ""]]])


class GuiCase(unittest.TestCase):
    """A withdrawn app per test; helpers for loading records and waiting."""
    def setUp(self):
        # Collect Tk objects from earlier tests on the UI thread, before workers start.
        gc.collect()
        self.root = tk.Tk()
        self.root.withdraw()
        self.app = padgui.App(self.root, auto_scan=False, auto_identify=False)
        self.errors = []
        self.root.report_callback_exception = lambda *args: self.errors.append(args)

    def tearDown(self):
        with patch.object(padgui.messagebox, "askyesno", return_value=True):
            self.app.close()
        self.assertEqual(self.errors, [])

    def finish(self):
        deadline = time.monotonic() + 3
        while self.app.busy and time.monotonic() < deadline:
            self.root.update()
            time.sleep(0.01)
        self.assertFalse(self.app.busy)

    def load(self, records):
        for r in records:
            self.app.install_values(r, r["values"])
            self.app.cache[r["path"]] = r
        self.app.render_tree()
        self.root.update()



class GuiTests(GuiCase):
    def test_startup_scans_without_a_click(self):
        self.app.close()
        self.root = tk.Tk()
        self.root.withdraw()
        with patch.object(padgui.App, "scan") as scan:
            self.app = padgui.App(self.root)
            self.root.update()
            scan.assert_called_once_with()

    def test_console_windows_are_bound_to_device_and_reused(self):
        from unittest.mock import Mock
        one, two = record(), record("dongle", "dongle")
        self.load([one, two])
        first, second = Mock(), Mock()
        with patch.object(padgui, "LiveConsole", side_effect=[first, second]) as create:
            self.app.show_console()
            self.app.show_console()
            self.assertEqual(create.call_count, 1)
            first.window.lift.assert_called_once()
            self.app.tree.selection_set("device1")
            self.app.select_node()
            self.app.show_console()
            self.assertEqual(create.call_count, 2)
            self.assertEqual(create.call_args_list[0].args[2], one["identity"])
            self.assertEqual(create.call_args_list[1].args[2], two["identity"])
            self.assertEqual(self.app.console_path(one["identity"]), "fake")
            create.call_args_list[0].args[5]()  # Closing one leaves the other console open.
            self.assertNotIn(one["identity"], self.app.consoles)
            self.assertIn(two["identity"], self.app.consoles)
            create.call_args_list[1].args[5]()

    def test_slot_header_stays_visible_and_shares_horizontal_scroll(self):
        self.load([record()])
        self.root.deiconify()
        self.root.geometry("440x300")
        self.app.show_target(0)
        padgui.ttk.Frame(self.app.form, width=900, height=1000).grid(row=1000, column=0, columnspan=4)
        self.root.update()
        header_y = self.app.details_header.winfo_rooty()
        canvas = self.app.scroll_canvases[self.app.settings]
        canvas.yview_moveto(1)
        canvas.xview_moveto(.3)
        self.root.update()
        self.assertEqual(self.app.details_header.winfo_rooty(), header_y)
        self.assertGreater(self.app.header_canvas.xview()[0], 0)
        self.assertAlmostEqual(self.app.header_canvas.xview()[0], canvas.xview()[0], places=2)
        titles = [w.cget("text") for w in self.app.controls(self.app.details_header) if isinstance(w, padgui.ttk.Button)]
        self.assertEqual(titles, ["Apply", "Discard", "Reset all to defaults"])
        self.assertTrue(all(w.cget("text") not in ("Apply", "Discard") for w in self.app.controls(self.app.form)
                            if isinstance(w, padgui.ttk.Button)))

    def test_tree_switches_pages_and_firmware_has_no_tabs(self):
        self.load([record(), record("dongle", "dongle")])
        self.assertIs(self.app.active_page, self.app.overview)
        self.app.show_target(0)
        self.assertIs(self.app.active_page, self.app.settings)
        self.app.tree.selection_set("device1")
        self.app.select_node()
        self.assertIs(self.app.active_page, self.app.dongle_page)
        self.app.log("Shared operation log")
        self.assertIn("Shared operation log", self.app.loader_output.get("1.0", "end"))
        self.assertEqual(self.app.device.get(), "dongle")
        self.assertEqual(len(self.app.tree.get_children("device0")), padgui.pad.REMAP_TARGET)
        self.assertFalse(hasattr(self.app, "tools_page"))
        self.assertFalse(any(target == "tools" for path, target in self.app.nodes.values()))
        self.app.tree.selection_set("device0")
        self.app.select_node()
        self.assertIs(self.app.active_page, self.app.overview)
        self.assertEqual(self.app.board.get(), "pad")
        self.assertFalse(hasattr(self.app, "tabs"))
        self.assertFalse(hasattr(self.app, "progress"))

    def test_wheel_over_inputs_scrolls_without_editing(self):
        self.load([record()])
        self.root.deiconify()
        self.app.show_target(0)
        self.root.update()
        # Ensure a page tall enough to scroll and include all input classes.
        form = padgui.ttk.Frame(self.app.form)
        form.grid(row=1000, column=0)
        variable = tk.StringVar(value="one")
        combo = padgui.ttk.Combobox(form, values=["one", "two"], textvariable=variable, state="readonly")
        combo.pack()
        number = tk.StringVar(value="50")
        spin = padgui.ttk.Spinbox(form, from_=0, to=100, textvariable=number)
        spin.pack()
        scale = padgui.ttk.Scale(form, from_=0, to=100)
        scale.pack()
        scale.set(50)
        padgui.ttk.Frame(form, height=2000).pack()
        self.root.update()
        canvas = self.app.scroll_canvases[self.app.settings]
        for widget in (combo, spin, scale, self.app.tree):
            canvas.yview_moveto(0)
            widget.event_generate("<Button-5>")
            self.root.update()
            self.assertGreater(canvas.yview()[0], 0)
            self.assertEqual(variable.get(), "one")
            self.assertEqual(number.get(), "50")
            self.assertEqual(scale.get(), 50)
        canvas.yview_moveto(0)
        combo.event_generate("<MouseWheel>", delta=-120)
        self.root.update()
        self.assertGreater(canvas.yview()[0], 0)
        self.assertEqual(variable.get(), "one")

    def test_discard_reverts_all_drafts_without_writing(self):
        one, two = record(), record("other")
        one["values"][2]["rumble"] = (b"\x64", True)
        self.load([one, two])
        self.assertEqual(self.app.summary.get(), "Loaded 2 device(s). No pending changes.")
        self.assertTrue(self.app.apply_all_button.instate(["disabled"]))
        one["draft"][0, "rumble"]["var"].set("invalid")
        two["draft"][1, "type"]["var"].set("hid")
        one["draft"][2, "rumble"]["reset"].set(True)
        self.assertIn("3 pending", self.app.summary.get())
        self.assertFalse(self.app.apply_all_button.instate(["disabled"]))
        self.app.show_target(0)
        original_widgets = self.app.form.winfo_children()
        with patch.object(padgui.pad, "send") as send, patch.object(padgui.pad, "cmd") as cmd, \
             patch.object(self.app, "render_tree") as tree, \
             patch.object(self.app, "render_details") as details, \
             patch.object(self.app, "render_overview") as overview, \
             patch.object(self.app, "update_previews", wraps=self.app.update_previews) as previews:
            self.app.discard_changes()
            send.assert_not_called()
            cmd.assert_not_called()
            tree.assert_not_called()
            details.assert_not_called()
            overview.assert_not_called()
            previews.assert_called_once()
        self.assertEqual(self.app.form.winfo_children(), original_widgets)
        self.assertEqual(self.app.editors["rumble"]["var"].get(), "100")
        self.assertEqual(two["draft"][1, "type"]["var"].get(), "switch")
        self.assertFalse(one["draft"][2, "rumble"]["reset"].get())
        self.assertEqual(self.app.dirty_count(), 0)
        self.assertTrue(self.app.apply_all_button.instate(["disabled"]))
        self.assertEqual(self.app.summary.get(), "Loaded 2 device(s). No pending changes.")

    def test_setting_default_and_slot_discard_keep_unrelated_drafts(self):
        r = record()
        r["values"][0]["rumble"] = (b"\x50", True)
        r["defaults"] = {0: {"rumble": (b"\x64", False)}}
        self.load([r])
        self.app.show_target(0)
        entry = r["draft"][0, "rumble"]
        marker = next(w for e, w in self.app.indicators if e is entry and w.winfo_exists())
        row = int(marker.master.grid_info()["row"])
        actions = next(w for w in self.app.form.winfo_children()
                       if w.grid_info().get("row") == row and w.grid_info().get("column") == 3)
        buttons = {w.cget("text"): w for w in actions.winfo_children()}
        self.assertEqual(set(buttons), {"?"})
        self.assertIsInstance(marker, padgui.ttk.Button)
        self.assertEqual(marker.cget("text"), "✓")
        self.assertIn("Click to stage", marker.tooltip.text())
        page_buttons = {action: button for rec, t, action, button in self.app.scope_buttons
                        if rec is r and t == 0 and button.winfo_exists()}
        self.assertFalse(marker.instate(["disabled"]))
        self.assertTrue(page_buttons["Discard"].instate(["disabled"]))
        r["draft"][1, "type"]["var"].set("hid")
        r["draft"][0, "type"]["var"].set("hid")
        with patch.object(padgui.pad, "cmd") as cmd:
            marker.invoke()
            self.assertEqual(entry["var"].get(), "100")
            self.assertTrue(entry["reset"].get())
            self.assertFalse(marker.instate(["disabled"]))
            self.assertEqual(marker.cget("text"), "*")
            self.assertIn("Click to discard", marker.tooltip.text())
            marker.invoke()
            self.assertEqual(entry["var"].get(), "80")
            self.assertEqual(marker.cget("text"), "✓")
            marker.invoke()
            self.assertEqual(marker.cget("text"), "*")
            self.assertFalse(page_buttons["Discard"].instate(["disabled"]))
            page_buttons["Discard"].invoke()
            cmd.assert_not_called()
        self.assertEqual(entry["var"].get(), "80")
        self.assertFalse(entry["reset"].get())
        self.assertEqual(r["draft"][0, "type"]["var"].get(), "switch")
        self.assertTrue(page_buttons["Discard"].instate(["disabled"]))
        self.assertFalse(marker.instate(["disabled"]))
        self.assertEqual(r["draft"][1, "type"]["var"].get(), "hid")
        entry["var"].set("invalid")
        self.assertFalse(marker.instate(["disabled"]))
        self.assertEqual(marker.cget("text"), "!")
        self.assertIn("Invalid value", marker.tooltip.text())
        marker.invoke()
        self.assertEqual(entry["var"].get(), "80")
        self.assertEqual(marker.cget("text"), "✓")
        entry["default_raw"] = entry["default_text"] = None
        self.app.mark_dirty()
        self.assertEqual(marker.cget("text"), "?")
        self.assertTrue(marker.instate(["disabled"]))
        self.assertIn("default unavailable", marker.tooltip.text())
        entry["var"].set("90")
        self.assertEqual(marker.cget("text"), "*")
        self.assertFalse(marker.instate(["disabled"]))
        marker.invoke()
        self.assertEqual(marker.cget("text"), "?")
        self.assertTrue(marker.instate(["disabled"]))
        labels = [w.cget("text") for w in self.app.form.winfo_children() if isinstance(w, padgui.ttk.Label)]
        self.assertFalse(any("differs from default" in text for text in labels))

    def test_disabled_status_tooltip_and_navigation_cleanup(self):
        self.load([record()])
        self.root.deiconify()
        self.app.show_target(0)
        self.root.update()
        entry = self.app.editors["rumble"]
        marker = next(w for e, w in self.app.indicators if e is entry and w.winfo_exists())
        self.assertTrue(marker.instate(["disabled"]))
        self.assertEqual(marker.cget("text"), "")
        marker.event_generate("<Enter>")
        self.assertIsNotNone(marker.tooltip.timer)
        marker.tooltip.hide()
        marker.tooltip.show()
        self.root.update()
        self.assertIsNotNone(marker.tooltip.window)
        self.assertIn("At the firmware default", marker.tooltip.window.winfo_children()[0].cget("text"))
        self.app.show_target(1)
        self.root.update()
        self.assertIsNone(marker.tooltip.window)
        self.assertIsNone(marker.tooltip.timer)

    def test_main_status_button_toggles_default_and_discard(self):
        r = record()
        r["values"][0]["type"] = (b"\1", True)
        r["defaults"] = {0: {"type": (b"\0", False)}}
        self.load([r])
        entry = r["draft"][0, "type"]
        marker = next(w for e, w in self.app.indicators if e is entry and w.winfo_exists())
        self.assertIsInstance(marker, padgui.ttk.Button)
        self.assertEqual(marker.cget("text"), "✓")
        self.assertNotIn("Default", [w.cget("text") for w in self.app.controls(self.app.overview_form)
                                     if isinstance(w, padgui.ttk.Button)])
        with patch.object(padgui.pad, "cmd") as cmd:
            marker.invoke()
            self.assertEqual(entry["var"].get(), "switch")
            self.assertEqual(marker.cget("text"), "*")
            marker.invoke()
            self.assertEqual(entry["var"].get(), "hid")
            self.assertEqual(marker.cget("text"), "✓")
            cmd.assert_not_called()

    def test_slot_reset_defaults_is_scoped_and_staged(self):
        one, two = record(), record("other")
        one["values"][0]["rumble"] = (b"\x50", True)
        one["defaults"] = {0: {"rumble": (b"\x64", False)}}
        self.load([one, two])
        self.app.show_target(0)
        one["draft"][1, "rumble"]["var"].set("70")
        two["draft"][0, "rumble"]["var"].set("60")
        # Unknown defaults elsewhere must not block resetting this slot.
        one["draft"][1, "type"]["default_raw"] = None
        one["draft"][1, "type"]["default_text"] = None
        button = next(w for r, t, action, w in self.app.scope_buttons
                      if r is one and t == 0 and action == "Reset all to defaults" and w.winfo_exists())
        widgets = self.app.form.winfo_children()
        with patch.object(padgui.pad, "cmd") as cmd, patch.object(padgui.pad, "send") as send:
            button.invoke()
            cmd.assert_not_called()
            send.assert_not_called()
        self.assertEqual(self.app.form.winfo_children(), widgets)
        self.assertEqual(one["draft"][0, "rumble"]["var"].get(), "100")
        self.assertTrue(one["draft"][0, "rumble"]["reset"].get())
        self.assertEqual(one["draft"][1, "rumble"]["var"].get(), "70")
        self.assertEqual(two["draft"][0, "rumble"]["var"].get(), "60")
        self.app.discard_scope("fake", 0)
        self.assertEqual(one["draft"][0, "rumble"]["var"].get(), "80")

    def test_device_apply_saves_all_its_slots_and_preserves_other_devices(self):
        one, two = record(), record("other")
        self.load([one, two])
        one["draft"][0, "type"]["var"].set("hid")
        one["draft"][1, "rumble"]["var"].set("80")
        two["draft"][0, "rumble"]["var"].set("invalid")
        titles = [w.cget("text") for w in self.app.controls(self.app.overview_form) if isinstance(w, padgui.ttk.Button)]
        self.assertEqual(titles.count("Apply"), 1)
        self.assertEqual(titles.count("Discard"), 1)
        self.assertNotIn("Settings", titles)
        button = next(w for r, t, action, w in self.app.scope_buttons
                      if r is one and t is None and action == "Apply" and w.winfo_exists())
        updated = dict(one["values"])
        updated[0] = {"type": (b"\1", True), "rumble": (b"\x64", False)}
        updated[1] = {"type": (b"\0", False), "rumble": (b"\x50", True)}
        with patch.object(padgui.pad, "cmd", return_value=one["identity"]) as cmd, \
             patch.object(padgui.pad, "send") as send, \
             patch.object(padgui.pad, "read_target", side_effect=lambda p, table, t: updated[t]):
            button.invoke()
            self.finish()
            send.assert_called_once_with("fake", [(0, 1, b"\1"), (1, 2, b"\x50")])
            self.assertEqual({call.args[0] for call in cmd.call_args_list}, {"fake"})
        self.assertEqual(two["draft"][0, "rumble"]["var"].get(), "invalid")
        self.assertEqual(self.app.dirty_count(), 1)

    def test_slot_apply_and_device_discard_have_separate_scopes(self):
        one, two = record(), record("other")
        self.load([one, two])
        self.app.show_target(0)
        one["draft"][0, "type"]["var"].set("hid")
        one["draft"][0, "rumble"]["var"].set("80")
        one["draft"][1, "rumble"]["var"].set("invalid")
        two["draft"][0, "rumble"]["var"].set("90")
        titles = [w.cget("text") for w in self.app.controls(self.app.form) if isinstance(w, padgui.ttk.Button)]
        self.assertNotIn("Apply", titles)
        self.assertNotIn("Discard", titles)
        header_titles = [w.cget("text") for w in self.app.controls(self.app.details_header) if isinstance(w, padgui.ttk.Button)]
        self.assertEqual(header_titles.count("Apply"), 1)
        self.assertEqual(header_titles.count("Discard"), 1)
        self.assertIs(self.app.details_actions.master, self.app.details_header)
        button = next(w for r, t, action, w in self.app.scope_buttons
                      if r is one and t == 0 and action == "Apply" and w.winfo_exists())
        updated = dict(one["values"])
        updated[0] = {"type": (b"\1", True), "rumble": (b"\x50", True)}
        with patch.object(padgui.pad, "cmd", return_value=one["identity"]), \
             patch.object(padgui.pad, "send") as send, \
             patch.object(padgui.pad, "read_target", side_effect=lambda p, table, t: updated[t]):
            button.invoke()
            self.finish()
            send.assert_called_once_with("fake", [(0, 1, b"\1"), (0, 2, b"\x50")])
        self.assertEqual(one["draft"][1, "rumble"]["var"].get(), "invalid")
        page_buttons = {action: w for r, t, action, w in self.app.scope_buttons
                        if r is one and t == 0 and w.winfo_exists()}
        self.assertTrue(all(page_buttons[action].instate(["disabled"]) for action in ("Apply", "Discard")))
        device_discard = next(w for r, t, action, w in self.app.scope_buttons
                              if r is one and t is None and action == "Discard" and w.winfo_exists())
        widgets = self.app.form.winfo_children()
        with patch.object(padgui.pad, "cmd") as cmd:
            device_discard.invoke()
            cmd.assert_not_called()
        self.assertEqual(self.app.form.winfo_children(), widgets)
        self.assertEqual(self.app.dirty_count(), 1)
        self.assertEqual(two["draft"][0, "rumble"]["var"].get(), "90")

    def test_main_page_unpair_stages_only_paired_slot_and_is_discardable(self):
        r = record(protocol=6)
        r["fields"].append({"kind": 0, "id": 13, "name": "bond", "type": padgui.pad.T_BYTES,
                            "size": 68, "help": "Paired host", "dev": True})
        classic = b"\x11" * 6 + bytes(62)
        le = bytes(23) + b"\x22" * 6 + bytes(39)
        for target, raw in ((0, classic), (1, le), (2, bytes(68))):
            r["values"][target]["bond"] = (raw, bool(any(raw)))
        self.load([r])
        buttons = [w for w in self.app.controls(self.app.overview_form) if isinstance(w, padgui.ttk.Button)]
        self.assertNotIn("Connect", [w.cget("text") for w in buttons])
        self.assertNotIn("Pair…", [w.cget("text") for w in buttons])
        unpair = [w for w in buttons if w.cget("text") == "Unpair"]
        self.assertEqual(len(unpair), 8)
        self.assertFalse(unpair[0].instate(["disabled"]))
        self.assertFalse(unpair[1].instate(["disabled"]))
        self.assertTrue(all(w.instate(["disabled"]) for w in unpair[2:]))
        r["draft"][3, "type"]["var"].set("hid")
        with patch.object(padgui.pad, "cmd") as cmd:
            unpair[0].invoke()
            cmd.assert_not_called()
        self.assertEqual(self.app.value_bytes(r["draft"][0, "bond"]), bytes(68))
        self.assertEqual(self.app.value_bytes(r["draft"][1, "bond"]), le)
        self.assertTrue(unpair[0].instate(["disabled"]))
        self.assertEqual(r["draft"][3, "type"]["var"].get(), "hid")
        self.app.discard_setting((r["draft"][0, "bond"],))
        self.assertEqual(self.app.value_bytes(r["draft"][0, "bond"]), classic)
        self.assertFalse(unpair[0].instate(["disabled"]))
        unpair[1].invoke()
        self.assertEqual(self.app.value_bytes(r["draft"][1, "bond"]), bytes(68))
        refreshed = record(protocol=6)
        refreshed["fields"] = r["fields"]
        refreshed["values"] = r["values"]
        self.app.merge_device(refreshed)
        r = refreshed
        self.assertTrue(r["draft"][1, "bond"]["unpair"])
        r["values"][1]["bond"] = (bytes(68), False)
        with patch.object(padgui.pad, "cmd", return_value=r["identity"]), \
             patch.object(padgui.pad, "send") as send, \
             patch.object(padgui.pad, "read_target", side_effect=lambda p, table, t: r["values"][t]):
            self.app.apply_settings(("fake", 1, "bond"))
            self.finish()
            send.assert_called_once_with("fake", [(1, padgui.pad.UNPAIR_ID, b"")])
        self.assertEqual(r["draft"][3, "type"]["var"].get(), "hid")
        unpair = [w for w in self.app.controls(self.app.overview_form)
                  if isinstance(w, padgui.ttk.Button) and w.cget("text") == "Unpair"]
        self.assertEqual(len(unpair), 8)
        self.assertFalse(unpair[0].instate(["disabled"]))
        self.assertTrue(all(w.instate(["disabled"]) for w in unpair[1:]))

    def test_rom_loader_scan_has_only_recovery_actions(self):
        with patch.object(padgui.pad, "discover_devices", return_value=[("rom", "BR23 UBOOT1.00", None)]), \
             patch.object(padgui.pad, "fields") as fields, patch.object(padgui.pad, "status") as status:
            self.app.scan()
            self.finish()
            fields.assert_not_called()
            status.assert_not_called()
        self.assertEqual(self.app.record()["board"], "loader")
        self.assertIn("UBOOT1.00", self.app.tree.item("device0", "text"))
        self.assertEqual(self.app.tree.get_children("device0"), ())
        buttons = [w.cget("text") for w in self.app.controls(self.app.loader_form) if isinstance(w, padgui.ttk.Button)]
        self.assertIn("Flash Firmware…", buttons)
        self.assertIn("Back up full flash…", buttons)
        self.assertIn("Read product ID", buttons)
        self.assertIs(self.app.active_page, self.app.loader_page)
        self.assertFalse(hasattr(self.app, "diagnostics"))
        self.app.board.set("dongle")
        self.app.select_node()
        self.assertEqual(self.app.board.get(), "")
        self.assertEqual(self.app.product_id.get(), "Not read")
        self.assertFalse(hasattr(self.app, "boards"))

    def test_flash_picker_derives_board_and_rejects_unknown_product(self):
        self.load([loader_record()])
        with patch.object(padgui.filedialog, "askopenfilename", return_value="firmware.img") as picker, \
             patch.object(padgui.padflash, "load_image", return_value=b"image"), \
             patch.object(padgui.padflash, "image_identity", return_value=(padgui.PIDS["pad"], 0xffff)), \
             patch.object(self.app, "flash") as flash:
            self.app.choose_firmware()
            picker.assert_called_once()
            flash.assert_called_once_with()
            self.assertEqual(self.app.image.get(), "firmware.img")
        with patch.object(padgui.filedialog, "askopenfilename", return_value="dongle.img"), \
             patch.object(padgui.padflash, "load_image", return_value=b"image"), \
             patch.object(padgui.padflash, "image_identity", return_value=("Unknown product", 0xffff)), \
             patch.object(padgui.messagebox, "showerror") as error, patch.object(self.app, "flash") as flash:
            self.app.choose_firmware()
            error.assert_called_once()
            flash.assert_not_called()

    def test_narrow_window_wraps_controls_and_scrolls_horizontally(self):
        self.load([record()])
        self.root.deiconify()
        self.root.geometry("440x360")
        self.root.update()
        self.assertEqual(self.root.winfo_width(), 440)
        buttons = [w for w in self.app.controls(self.app.overview_form) if isinstance(w, padgui.ttk.Button)]
        reset = next(w for w in buttons if w.cget("text") == "Reset all to defaults")
        self.assertEqual([w.cget("text") for w in reset.master.winfo_children()],
                         ["Apply", "Discard", "Reset all to defaults"])
        self.app.show_target(0)
        self.root.update()
        padgui.ttk.Frame(self.app.form, width=600, height=1).grid(row=1000, column=0, columnspan=4)
        self.root.update()
        canvas = self.app.scroll_canvases[self.app.settings]
        horizontal = next(w for w in canvas.master.winfo_children()
                          if isinstance(w, padgui.ttk.Scrollbar) and str(w.cget("orient")) == "horizontal")
        self.assertTrue(horizontal.winfo_ismapped())
        self.assertLess(canvas.xview()[1], 1)
        combo = next(w for w in self.app.controls(self.app.form) if isinstance(w, padgui.ttk.Combobox))
        value = combo.get()
        combo.event_generate("<MouseWheel>", delta=-120, state=1)
        self.root.update()
        self.assertGreater(canvas.xview()[0], 0)
        self.assertEqual(combo.get(), value)
        self.root.geometry("1250x850")
        self.root.update()
        self.assertFalse(horizontal.winfo_ismapped())
        self.assertEqual(canvas.xview(), (0, 1))

    def test_product_id_is_read_from_selected_loader_not_previous_board(self):
        from unittest.mock import Mock
        self.load([loader_record()])
        self.app.board.set("dongle")
        loader = Mock(product_id=padgui.PIDS["pad"])
        with patch.object(padgui.padflash, "inspect_loader", return_value=loader) as inspect:
            self.app.detect_loader_product_id()
            self.finish()
            self.assertEqual(inspect.call_args.args[0], "rom")
            self.assertEqual(self.app.product_id.get(), padgui.PIDS["pad"])
            self.assertEqual(self.app.record()["product_id"], padgui.PIDS["pad"])
            loader.flash.assert_not_called()

    def test_automatic_disconnect_and_reconnect_preserves_drafts(self):
        one = record()
        self.load([one])
        one["draft"][0, "rumble"]["var"].set("80")
        self.app.update_devices((0, {}, [], []))
        self.assertFalse(self.app.record()["online"])
        self.assertEqual(self.app.dirty_count(), 1)
        self.assertIn("Loaded 0 device(s)", self.app.summary.get())
        new = record("renumbered")
        new["identity"] = one["identity"]
        self.app.update_devices((0, {new["path"]: new["identity"]}, [new], []))
        self.assertNotIn("fake", self.app.cache)
        self.assertEqual(new["draft"][0, "rumble"]["var"].get(), "80")
        self.assertEqual(self.app.dirty_count(), 1)

    def test_reused_path_keeps_old_drafts_separate_from_new_device(self):
        one = record()
        self.load([one])
        one["draft"][0, "rumble"]["var"].set("80")
        replacement = record("different-id")
        replacement["path"] = "fake"
        self.app.update_devices((0, {"fake": replacement["identity"]}, [replacement], []))
        self.assertEqual(self.app.cache["fake"]["draft"][0, "rumble"]["var"].get(), "100")
        offline = next(r for r in self.app.cache.values() if not r.get("online", True))
        self.assertEqual(offline["draft"][0, "rumble"]["var"].get(), "80")
        with patch.object(padgui.messagebox, "showerror"), patch.object(padgui.pad, "send") as send:
            self.app.apply_settings()
            send.assert_not_called()

    def test_manual_open_reused_path_preserves_old_drafts(self):
        one = record()
        self.load([one])
        one["draft"][0, "rumble"]["var"].set("80")
        replacement = record("different-id")
        replacement["path"] = "fake"
        with patch.object(padgui.simpledialog, "askstring", return_value="fake"), \
             patch.object(padgui.pad, "discover_devices", return_value=[("fake", replacement["name"], replacement["identity"])]), \
             patch.object(padgui, "read_device", return_value=replacement):
            self.app.open_device_path()
            self.finish()
        offline = next(r for r in self.app.cache.values() if not r.get("online", True))
        self.assertEqual(offline["draft"][0, "rumble"]["var"].get(), "80")
        self.assertEqual(self.app.cache["fake"]["draft"][0, "rumble"]["var"].get(), "100")
        self.app.discard_changes()
        self.assertEqual(list(self.app.cache), ["fake"])

    def test_manual_device_path_opens_and_is_monitored(self):
        one = record("manual")
        with patch.object(padgui.simpledialog, "askstring", return_value="manual"), \
             patch.object(padgui.pad, "discover_devices", return_value=[("manual", one["name"], one["identity"])]), \
             patch.object(padgui, "read_device", return_value=one):
            self.app.open_device_path()
            self.finish()
            self.assertEqual(self.app.device.get(), "manual")
            self.assertIn("manual", self.app.monitor.manual_paths)
            self.assertEqual(self.app.monitor.known["manual"], one["identity"])

    def test_worker_failure_restores_controls(self):
        self.load([record()])
        def refused():
            raise SystemExit("device refused change")
        with patch.object(padgui.messagebox, "showerror") as error:
            self.app.run("Apply", refused)
            self.assertEqual(str(self.app.enter_loader_button.cget("state")), "disabled")
            self.finish()
            error.assert_called_once()
        self.assertEqual(str(self.app.enter_loader_button.cget("state")), "normal")

    def test_slot_edits_survive_navigation_and_type_is_shared(self):
        one, two = record(), record("other")
        self.load([one, two])
        self.app.show_target(0)
        self.app.editors["type"]["var"].set("hid")
        self.app.editors["rumble"]["var"].set("80")
        self.app.show_target(1)
        self.app.tree.selection_set("device1")
        self.app.select_node()
        self.app.tree.selection_set("device0/0")
        self.app.select_node()
        self.assertEqual(self.app.editors["type"]["var"].get(), "hid")
        self.assertEqual(self.app.editors["rumble"]["var"].get(), "80")
        self.assertIs(self.app.editors["type"]["var"], one["draft"][0, "type"]["var"])
        self.assertEqual(self.app.dirty_count(), 2)

    def test_apply_batches_changes_from_all_slots(self):
        r = record()
        self.load([r])
        r["draft"][0, "rumble"]["var"].set("80")
        r["draft"][1, "type"]["var"].set("hid")
        updated = dict(r["values"])
        updated[0] = {"type": (b"\0", False), "rumble": (b"\x50", True)}
        updated[1] = {"type": (b"\1", True), "rumble": (b"\x64", False)}
        with patch.object(padgui.pad, "cmd", return_value=r["identity"]), \
             patch.object(padgui.pad, "send") as send, \
             patch.object(padgui.pad, "read_target", side_effect=lambda p, table, t: updated[t]):
            self.app.apply_settings()
            self.finish()
            send.assert_called_once_with("fake", [(0, 2, b"\x50"), (1, 1, b"\1")])
        self.assertEqual(self.app.dirty_count(), 0)

    def test_identity_mismatch_blocks_every_commit(self):
        one, two = record(), record("other")
        self.load([one, two])
        for r in (one, two):
            r["draft"][0, "rumble"]["var"].set("80")
        with patch.object(padgui.pad, "cmd", side_effect=[one["identity"], b"different device"]), \
             patch.object(padgui.pad, "send") as send, patch.object(padgui.messagebox, "showerror"):
            self.app.apply_settings()
            self.finish()
            send.assert_not_called()
        self.assertEqual(self.app.dirty_count(), 2)

    def test_partial_apply_keeps_unsaved_device_edits(self):
        one, two = record(), record("other")
        self.load([one, two])
        for r in (one, two):
            r["draft"][0, "rumble"]["var"].set("80")
        updated = dict(one["values"])
        updated[0] = {"type": (b"\0", False), "rumble": (b"\x50", True)}
        with patch.object(padgui.pad, "cmd", side_effect=[one["identity"], two["identity"]]), \
             patch.object(padgui.pad, "send", side_effect=[None, OSError("unplugged")]), \
             patch.object(padgui.pad, "read_target", side_effect=lambda p, table, t: updated[t]), \
             patch.object(padgui.messagebox, "showerror"):
            self.app.apply_settings()
            self.finish()
        self.assertEqual(self.app.dirty_count(), 1)
        self.assertTrue(self.app.changed(two["draft"][0, "rumble"]))

    def test_refresh_button_rescans_and_nothing_polls(self):
        buttons = [w for w in self.app.controls(self.app.root) if isinstance(w, padgui.ttk.Button) and w.cget("text") == "Refresh"]
        self.assertEqual(len(buttons), 1)
        one = record()
        with patch.object(padgui.pad, "discover_devices", return_value=[(one["path"], one["name"], one["identity"])]), \
             patch("paddevices.read_device", return_value=one):
            buttons[0].invoke()
            self.assertEqual(self.app.tree_status.cget("text"), "Looking…")
            self.assertNotIn("Looking", self.app.summary.get())
            for _ in range(200):
                self.root.update()
                if not self.app.looking:
                    break
                time.sleep(0.01)
        self.assertIn(one["path"], self.app.cache)
        self.assertEqual(self.app.tree_status.cget("text"), "")

    def test_device_tree_says_while_looking_and_when_empty(self):
        self.assertIn("None found", self.app.tree_status.cget("text"))
        with patch.object(padgui.pad, "discover_devices", side_effect=lambda **kw: time.sleep(0.2) or []):
            self.app.scan()
            self.assertEqual(self.app.tree_status.cget("text"), "Looking…")
            self.assertNotIn("Scanning", self.app.summary.get())
            self.finish()
        self.assertIn("None found", self.app.tree_status.cget("text"))
        self.load([record()])
        self.app.render_tree()
        self.assertEqual(self.app.tree_status.cget("text"), "")

    def test_scan_loads_both_boards_and_all_pad_targets(self):
        one, two = record(), record("dongle", "dongle")
        with patch.object(padgui.pad, "discover_devices", return_value=[(r["path"], r["name"], r["identity"]) for r in (one, two)]), \
             patch.object(padgui.pad, "fields", return_value=one["fields"]), \
             patch.object(padgui.pad, "read_target", side_effect=lambda p, table, t: one["values"][t]) as read, \
             patch.object(padgui.pad, "status", return_value="idle"):
            self.app.scan()
            self.finish()
            self.assertEqual(read.call_count, 10)
        self.assertEqual(len(self.app.cache), 2)
        self.assertEqual(len(self.app.tree.get_children("device0")), padgui.pad.REMAP_TARGET)
        self.assertEqual(len(self.app.tree.get_children("device1")), 0)

    def test_actions_target_slot_and_calibration_is_global(self):
        r = record()
        self.load([r])
        overview_buttons = [w.cget("text") for w in self.app.controls(self.app.overview_form) if isinstance(w, padgui.ttk.Button)]
        self.assertIn("Back up pad settings…", overview_buttons)
        self.assertNotIn("Calibrate sensors…", overview_buttons)
        self.assertNotIn("Palette settings", overview_buttons)
        self.app.show_target(9)
        calibration_buttons = [w.cget("text") for w in self.app.controls(self.app.settings) if isinstance(w, padgui.ttk.Button)]
        self.assertIn("Calibrate sensors…", calibration_buttons)
        self.assertNotIn("Back up pad settings…", calibration_buttons)
        self.assertFalse(hasattr(self.app, "calibration"))
        with patch.object(padgui.pad, "cmd", return_value=r["identity"]) as cmd:
            self.app.slot_action("connect", 5)
            self.finish()
            self.assertEqual(cmd.call_args.args, ("fake", 0x13, 5))
        with patch.object(padgui.pad, "cmd", return_value=r["identity"]) as cmd, \
             patch.object(padgui.messagebox, "askokcancel", return_value=True):
            self.app.calibrate()
            self.finish()
            self.assertEqual(cmd.call_args.args, ("fake", 0x18, 0))
    def test_global_restore_always_includes_calibration(self):
        self.load([record()])
        with patch.object(padgui.filedialog, "askopenfilename", return_value="settings.json"), \
             patch.object(padgui.messagebox, "askyesno", return_value=True), \
             patch.object(padgui.pad, "restore") as restore:
            self.app.restore_settings()
            self.finish()
            restore.assert_called_once_with("fake", "settings.json", True)

    def test_reset_all_stages_pairing_palette_and_calibration_for_selected_pad(self):
        one, other = record(), record("other")
        one["fields"].extend([
            {"kind": 0, "id": 10, "name": "bond", "type": padgui.pad.T_BYTES, "size": 4, "help": "Bond", "dev": True},
            {"kind": 1, "id": 11, "name": "palette0", "type": padgui.pad.T_RGB, "size": 3, "help": "Colour", "dev": False},
            {"kind": 2, "id": 12, "name": "calibration_blob", "type": padgui.pad.T_BYTES, "size": 4, "help": "Calibration", "dev": False}])
        one["values"][0]["bond"] = (b"bond", True)
        one["values"][8] = {"palette0": (b"\xff\0\0", True)}
        one["values"][9] = {"calibration_blob": (b"cal!", True)}
        one["defaults"] = {0: {"bond": (b"\0" * 4, False)},
                           8: {"palette0": (b"\0\xff\0", False)},
                           9: {"calibration_blob": (b"\0" * 4, False)}}
        self.load([one, other])
        one["draft"][1, "rumble"]["var"].set("bad input")
        other["draft"][0, "rumble"]["var"].set("80")
        with patch.object(padgui.pad, "send") as send, patch.object(padgui.pad, "cmd") as cmd:
            self.app.reset_all_defaults()
            send.assert_not_called()
            cmd.assert_not_called()
        for key in ((0, "bond"), (8, "palette0"), (9, "calibration_blob")):
            entry = one["draft"][key]
            self.assertTrue(entry["reset"].get())
            self.assertEqual(self.app.value_bytes(entry), entry["default_raw"])
        self.assertEqual(one["draft"][1, "rumble"]["var"].get(), "100")
        self.assertEqual(other["draft"][0, "rumble"]["var"].get(), "80")
        self.assertEqual(self.app.notice.get(), "")
        self.assertFalse(self.app.batch_edits)

    def test_effective_derived_address_is_visible_without_a_custom_setting(self):
        r = record()
        r["fields"].append({"kind": 0, "id": 12, "name": "address", "type": padgui.pad.T_BYTES,
                            "size": 6, "help": "Address", "dev": True})
        raw = bytes.fromhex("102030405002")
        r["values"][0]["address"] = (raw, False)
        r["defaults"] = {0: {"address": (bytes(6), False)}}
        self.load([r])
        entry = r["draft"][0, "address"]
        self.assertEqual(entry["var"].get(), "02:50:40:30:20:10")
        self.assertFalse(self.app.changed(entry))
        self.assertFalse(self.app.differs_from_default(entry))
        self.assertEqual(self.app.value_bytes(entry), raw)

    def test_reset_shows_default_and_later_edit_overrides_reset(self):
        r = record()
        r["values"][0]["rumble"] = (b"\x50", True)
        r["defaults"] = {0: {"rumble": (b"\x64", False)}}
        self.load([r])
        entry = r["draft"][0, "rumble"]
        self.assertTrue(self.app.differs_from_default(entry))
        self.assertEqual(self.app.tree.item("device0/0", "text"), "Slot A")
        self.assertFalse(self.app.tree.item("device0", "text").endswith(" *"))
        self.app.reset_default(entry)
        self.assertEqual(entry["var"].get(), "100")
        self.assertTrue(entry["reset"].get())
        self.assertFalse(self.app.differs_from_default(entry))
        self.assertFalse(self.app.tree.item("device0/0", "text").endswith(" *"))
        entry["var"].set("99")
        self.assertFalse(entry["reset"].get())
        with patch.object(padgui.pad, "cmd", return_value=r["identity"]), \
             patch.object(padgui.pad, "send") as send, \
             patch.object(padgui.pad, "read_target", side_effect=lambda p, table, t: r["values"][t]):
            self.app.apply_settings()
            self.finish()
            send.assert_called_once_with("fake", [(0, 2, b"\x63")])

    def test_explicit_default_has_no_custom_indicator_but_reset_clears_override(self):
        r = record()
        r["values"][0]["rumble"] = (b"\x64", True)
        r["defaults"] = {0: {"rumble": (b"\x64", False)}}
        self.load([r])
        entry = r["draft"][0, "rumble"]
        self.assertFalse(self.app.differs_from_default(entry))
        self.assertFalse(self.app.changed(entry))
        self.app.reset_default(entry)
        self.assertTrue(self.app.changed(entry))
        with patch.object(padgui.pad, "cmd", return_value=r["identity"]), \
             patch.object(padgui.pad, "send") as send, \
             patch.object(padgui.pad, "read_target", side_effect=lambda p, table, t: r["values"][t]):
            self.app.apply_settings()
            self.finish()
            send.assert_called_once_with("fake", [(0, 2, b"")])

    def test_colour_preview_follows_staged_palette_at_every_brightness(self):
        r = record()
        r["fields"] += [
            {"kind": 0, "id": 3, "name": "primary_colour", "type": padgui.pad.T_COLOUR,
             "size": 2, "help": "Colour", "dev": False},
            {"kind": 1, "id": 4, "name": "palette3", "type": padgui.pad.T_RGB,
             "size": 3, "help": "Palette", "dev": False}]
        r["values"][0]["primary_colour"] = (bytes([3, 15]), False)
        r["values"][8] = {"palette3": (bytes([255, 0, 0]), False)}
        self.load([r])
        self.app.show_target(0)
        colour = r["draft"][0, "primary_colour"]
        palette = r["draft"][8, "palette3"]
        self.assertEqual(self.app.color_preview(r, colour), "#ff0000")
        palette["var"].set("#00ff00")
        self.assertEqual(self.app.color_preview(r, colour), "#00ff00")
        colour["var"].set("3:5")
        self.assertEqual(self.app.color_preview(r, colour), "#00ff00")
        swatch = next(w for rec, entry, w in self.app.swatches if entry is colour and w.winfo_exists())
        self.assertEqual(swatch.itemcget("colour", "fill"), "#00ff00")
        colour["var"].set("3:")
        self.assertEqual(swatch.itemcget("invalid", "text"), "?")

    def test_single_apply_preserves_other_drafts_on_same_device(self):
        r = record()
        self.load([r])
        r["draft"][0, "type"]["var"].set("hid")
        r["draft"][0, "rumble"]["var"].set("80")
        r["draft"][1, "rumble"]["var"].set("90")
        updated = dict(r["values"])
        updated[0] = {"type": (b"\0", False), "rumble": (b"\x50", True)}
        with patch.object(padgui.pad, "cmd", return_value=r["identity"]), \
             patch.object(padgui.pad, "send") as send, \
             patch.object(padgui.pad, "read_target", side_effect=lambda p, table, t: updated[t]):
            self.app.apply_settings(("fake", 0, "rumble"))
            self.finish()
            send.assert_called_once_with("fake", [(0, 2, b"\x50")])
        self.assertEqual(r["draft"][0, "type"]["var"].get(), "hid")
        self.assertEqual(r["draft"][1, "rumble"]["var"].get(), "90")
        self.assertEqual(self.app.dirty_count(), 2)

    def test_single_apply_reload_failure_retains_other_edits(self):
        r = record()
        self.load([r])
        r["draft"][0, "type"]["var"].set("hid")
        r["draft"][0, "rumble"]["var"].set("80")
        with patch.object(padgui.pad, "cmd", return_value=r["identity"]), \
             patch.object(padgui.pad, "send"), \
             patch.object(padgui.pad, "read_target", side_effect=OSError("disconnected")), \
             patch.object(padgui.messagebox, "showerror"):
            self.app.apply_settings(("fake", 0, "rumble"))
            self.finish()
        self.assertFalse(self.app.changed(r["draft"][0, "rumble"]))
        self.assertTrue(self.app.changed(r["draft"][0, "type"]))
        self.assertEqual(r["draft"][0, "type"]["var"].get(), "hid")
        self.assertEqual(self.app.dirty_count(), 1)

    def test_palette_picker_preserves_brightness_and_uses_staged_colours(self):
        r = record()
        r["fields"] += [
            {"kind": 0, "id": 3, "name": "primary_colour", "type": padgui.pad.T_COLOUR,
             "size": 2, "help": "Colour", "dev": False},
            {"kind": 1, "id": 4, "name": "palette3", "type": padgui.pad.T_RGB,
             "size": 3, "help": "Palette", "dev": False},
            {"kind": 1, "id": 5, "name": "palette4", "type": padgui.pad.T_RGB,
             "size": 3, "help": "Palette", "dev": False}]
        r["values"][0]["primary_colour"] = (bytes([3, 15]), False)
        r["values"][8] = {"palette3": (bytes([255, 0, 0]), False), "palette4": (bytes([0, 0, 255]), False)}
        self.load([r])
        self.app.show_target(0)
        editor = next(w for w in self.app.special_editors if w.winfo_exists() and isinstance(w, padgui.PaletteEditor))
        from padwidgets import BRIGHTNESS_LABELS
        editor.level.set(BRIGHTNESS_LABELS[5])
        editor.choose_brightness()
        editor.draw_palette()
        self.assertIsInstance(editor.palette_button, padgui.ttk.Menubutton)
        self.assertEqual(editor.palette_menu.index("end"), 15)
        self.assertEqual(editor.palette_images[3].get(10, 8), (255, 0, 0))
        r["draft"][8, "palette4"]["var"].set("#00ff00")
        self.assertEqual(editor.palette_images[4].get(10, 8), (0, 255, 0))
        editor.palette_menu.invoke(4)
        self.assertEqual(r["draft"][0, "primary_colour"]["var"].get(), "4:5")
        self.assertEqual(editor.level.get(), BRIGHTNESS_LABELS[5])
        self.assertFalse(any(isinstance(w, tk.Toplevel) for w in editor.winfo_children()))

    def test_palette_entry_picker_has_no_brightness(self):
        r = record()
        r["fields"] += [
            {"kind": 0, "id": 3, "name": "primary_colour", "type": padgui.pad.T_ENTRY,
             "size": 1, "help": "Colour", "dev": False},
            {"kind": 1, "id": 4, "name": "palette3", "type": padgui.pad.T_RGB,
             "size": 3, "help": "Palette", "dev": False},
            {"kind": 1, "id": 5, "name": "palette4", "type": padgui.pad.T_RGB,
             "size": 3, "help": "Palette", "dev": False}]
        r["values"][0]["primary_colour"] = (bytes([3]), False)
        r["values"][8] = {"palette3": (bytes([255, 0, 0]), False), "palette4": (bytes([0, 0, 255]), False)}
        self.load([r])
        self.app.show_target(0)
        editor = next(w for w in self.app.special_editors if w.winfo_exists() and isinstance(w, padgui.PaletteEditor))
        self.assertFalse(editor.with_level)
        self.assertFalse(any(isinstance(w, padgui.ttk.Combobox) for w in editor.winfo_children()))
        colour = r["draft"][0, "primary_colour"]
        self.assertEqual(self.app.color_preview(r, colour), "#ff0000")
        editor.palette_menu.invoke(4)
        self.assertEqual(colour["var"].get(), "4")
        self.assertEqual(self.app.color_preview(r, colour), "#0000ff")
        self.assertEqual(self.app.value_bytes(colour), bytes([4]))

    def test_curve_controls_clamp_points_and_follow_reset(self):
        r = record()
        import padcurves
        default = padcurves.preset("linear")
        r["fields"].extend([
            {"kind": 0, "id": 36, "name": "left_curve_enabled", "type": padgui.pad.T_ENUM,
             "names": ["disabled", "enabled"], "size": 1, "help": "Enable curve", "dev": False},
            {"kind": 0, "id": 38, "name": "left_curve_points", "type": padgui.pad.T_BYTES,
             "size": 15, "help": "Curve", "dev": False}])
        r["values"][0]["left_curve_enabled"] = (b"\0", False)
        r["values"][0]["left_curve_points"] = (default, False)
        self.load([r])
        self.app.show_target(0)
        editor = next(w for w in self.app.special_editors if w.winfo_exists() and isinstance(w, padgui.CurveEditor))
        self.assertFalse(any(isinstance(w, tk.Checkbutton) or type(w).__name__ == "Checkbutton"
                             for w in self.app.controls(editor)))
        editor.set_point(3, 0)
        self.assertEqual(editor.points()[3], 48)
        self.assertEqual(editor.enabled["var"].get(), "enabled")      # a drawn curve is on
        editor.set_point(3, 100)
        self.assertEqual(editor.points()[3], 80)
        self.app.reset_default(editor.entry)
        self.assertEqual(editor.points(), list(default))
        self.assertFalse(self.app.changed(editor.entry))
        editor.outputs[7].set("55")
        self.assertEqual(editor.points()[7], round(.55 * 255))
        editor.preset_name.set("Concave")
        editor.choose_preset()
        self.assertEqual(bytes(editor.points()), padcurves.preset("concave"))
        self.assertEqual(editor.enabled["var"].get(), "enabled")
        editor.preset_name.set("Linear")
        editor.choose_preset()
        self.assertEqual(editor.enabled["var"].get(), "disabled")     # Linear is no curve
        editor.preset_name.set("Concave")
        editor.choose_preset()
        editor.preset_name.set("Custom")
        editor.choose_preset()
        editor.sync()
        self.assertEqual(editor.preset_name.get(), "Custom")
        self.assertFalse(editor.presets.selection_present())
        entries = self.app.setting_entries(r, 0, "left_curve_points", editor.entry)
        r["draft"][1, "type"]["var"].set("hid")
        with patch.object(padgui.pad, "cmd") as cmd:
            self.app.discard_setting(entries)
            cmd.assert_not_called()
        self.assertEqual(editor.points(), list(default))
        self.assertEqual(editor.enabled["var"].get(), "disabled")
        self.assertEqual(editor.preset_name.get(), "Linear")
        self.assertEqual(r["draft"][1, "type"]["var"].get(), "hid")
        editor.preset_name.set("Concave")
        editor.choose_preset()
        editor.copy_points()
        self.assertEqual(self.root.clipboard_get(), ','.join(map(str, padcurves.preset("concave"))))
        # A second stick can take the same clipboard; a pasted curve is on.
        from padwidgets import CurveEditor
        target_entry = dict(editor.entry, var=tk.StringVar(value=','.join(map(str, default))))
        target_enabled = dict(editor.enabled, var=tk.StringVar(value="disabled"))
        other = CurveEditor(self.app.form, self.app, target_entry, target_enabled)
        other.paste_points()
        self.assertEqual(bytes(other.points()), padcurves.preset("concave"))
        self.assertEqual(target_enabled["var"].get(), "enabled")
        self.root.clipboard_clear()
        self.root.clipboard_append("4,3,2")
        with patch("padwidgets.messagebox.showerror") as error:
            other.paste_points()
            error.assert_called_once()
        self.assertEqual(bytes(other.points()), padcurves.preset("concave"))
        other.destroy()
        with patch.object(padgui.pad, "cmd", return_value=r["identity"]), \
             patch.object(padgui.pad, "send") as send, \
             patch.object(padgui.pad, "read_target", side_effect=lambda p, table, t: r["values"][t]):
            self.app.apply_settings(("fake", 0, "left_curve_points"))
            self.finish()
            self.assertEqual(dict((fid, value) for t, fid, value in send.call_args.args[1]),
                             {36: b"\1", 38: padcurves.preset("concave")})
        with patch.object(padgui.messagebox, "showinfo") as help_dialog:
            self.app.show_help(editor.entry)
            self.assertIn("Drag a point", help_dialog.call_args.args[1])

    def test_requested_flash_backup_cannot_overwrite_input_and_cancel_aborts(self):
        self.load([loader_record()])
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "image.img"
            from test_padflash import image as make_image
            image.write_bytes(make_image())
            self.app.image.set(str(image))
            with patch.object(self.app, "loader_blob", return_value=b"loader"), \
                 patch.object(padgui.messagebox, "askyesnocancel", return_value=True), \
                 patch.object(padgui.filedialog, "asksaveasfilename", return_value=str(image)), \
                 patch.object(padgui.messagebox, "showerror") as error, \
                 patch.object(self.app, "run") as run:
                self.app.flash()
                error.assert_called_once()
                run.assert_not_called()
            with patch.object(self.app, "loader_blob", return_value=b"loader"), \
                 patch.object(padgui.messagebox, "askyesnocancel", return_value=True), \
                 patch.object(padgui.filedialog, "asksaveasfilename", return_value=""), \
                 patch.object(self.app, "run") as run:
                self.app.flash()
                run.assert_not_called()

    def test_flash_can_skip_backup_and_cancel_before_device_access(self):
        self.load([loader_record()])
        import tempfile
        from test_padflash import image as make_image
        from unittest.mock import Mock
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "image.img"
            image.write_bytes(make_image())
            self.app.image.set(str(image))
            loader = Mock()
            with patch.object(self.app, "loader_blob", return_value=b"loader"), \
                 patch.object(padgui.messagebox, "askyesnocancel", return_value=False), \
                 patch.object(padgui.messagebox, "askyesno") as confirm, \
                 patch.object(padgui.filedialog, "asksaveasfilename") as destination, \
                 patch.object(self.app, "open_loader", return_value=loader), \
                 patch.object(padgui.pad, "prepare_curve_upgrade") as upgrade, \
                 patch.object(self.app, "run") as run:
                self.app.flash()
                run.assert_called_once()
                confirm.assert_not_called()      # only the backup question
                run.call_args.args[1]()
                destination.assert_not_called()
                upgrade.assert_not_called()
                loader.flash.assert_called_once_with(make_image(), "pad", None)
            with patch.object(self.app, "loader_blob", return_value=b"loader"), \
                 patch.object(padgui.messagebox, "askyesnocancel", return_value=None), \
                 patch.object(self.app, "run") as run:
                self.app.flash()
                run.assert_not_called()

    def test_gui_flashes_dongle_image_with_a_different_chip_key(self):
        from test_padflash import Device, image as make_image
        import tempfile
        self.load([loader_record()])
        device = Device()
        device.flash[:8192] = make_image('GS_C2_Dongle', key=0x4317)
        loader = padgui.padflash.Loader('rom', lambda _: None)
        loader.key, loader.flash_size = 0x4317, 0x80000
        original = make_image('GS_C2_Dongle')
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / 'dongle.img'
            image.write_bytes(original)
            self.app.image.set(str(image))
            with patch.object(self.app, 'loader_blob', return_value=b'loader'), \
                 patch.object(self.app, 'open_loader', return_value=loader), \
                 patch.object(padgui.messagebox, 'askyesnocancel', return_value=False), \
                 patch.object(padgui.messagebox, 'askyesno', return_value=True), \
                 patch.object(padgui.padflash, 'scsi', side_effect=device.scsi), \
                 patch.object(self.app, 'run') as run:
                self.app.flash()
                run.assert_called_once()
                run.call_args.args[1]()
            self.assertEqual(image.read_bytes(), original)
        self.assertEqual(device.flash[:8192], make_image('GS_C2_Dongle', key=0x4317))
        self.assertEqual(device.commands[-1], 0xfc0c)

    def test_running_device_only_offers_loader_entry(self):
        self.load([record()])
        self.assertEqual(str(self.app.flash_button.cget("state")), "disabled")
        self.assertEqual(str(self.app.backup_flash_button.cget("state")), "disabled")
        self.assertEqual(str(self.app.enter_loader_button.cget("state")), "normal")
        titles = [w.cget("text") for w in self.app.controls(self.app.overview_form) if isinstance(w, padgui.ttk.Button)]
        self.assertIn("Enter flash loader", titles)
        self.assertNotIn("Flash Firmware…", titles)
        with patch.object(padgui.filedialog, "askopenfilename") as picker, patch.object(padgui.messagebox, "showinfo"):
            self.app.choose_firmware()
            picker.assert_not_called()

    def test_loader_entry_handles_renumbering_and_refreshes_without_losing_other_drafts(self):
        one, other = record("old"), record("other")
        self.load([one, other])
        other["draft"][0, "rumble"]["var"].set("80")
        self.app.board.set("dongle")  # A previous selector value must not survive the pad transition.
        with patch.object(padgui, "usb_location", return_value=None), \
             patch.object(padgui.pad, "cmd", return_value=one["identity"]) as cmd, \
             patch.object(padgui.pad, "discover_devices", return_value=[("new", "BR23 UBOOT1.00", None), ("other", other["name"], other["identity"])]), \
             patch.object(padgui.pad, "fields", return_value=other["fields"]), \
             patch.object(padgui.pad, "read_target", side_effect=lambda p, table, t: other["values"][t]), \
             patch.object(padgui.pad, "status", return_value="idle"), \
             patch.object(padgui.messagebox, "askyesno") as discard:
            self.app.enter_flash_loader()
            self.finish()
            self.assertEqual(cmd.call_args_list[-1].args, ("old", 0x11))
            self.assertEqual(self.app.device.get(), "new")
            self.assertEqual(self.app.record()["board"], "loader")
            self.assertEqual(self.app.board.get(), "")
            self.assertEqual(self.app.product_id.get(), "Not read")
            self.assertEqual(self.app.cache["other"]["draft"][0, "rumble"]["var"].get(), "80")
            discard.assert_not_called()
            self.assertEqual(str(self.app.flash_button.cget("state")), "normal")

    def test_loader_entry_rejects_ambiguous_new_loaders(self):
        one = record()
        self.load([one])
        with patch.object(padgui, "usb_location", return_value=None), \
             patch.object(padgui.pad, "cmd", return_value=one["identity"]), \
             patch.object(padgui.pad, "discover_devices", return_value=[("one", "UBOOT1.00", None), ("two", "UBOOT1.00", None)]), \
             patch.object(padgui.messagebox, "showerror") as error, \
             patch.object(self.app, "scan") as scan:
            self.app.enter_flash_loader()
            self.finish()
            error.assert_called_once()
            scan.assert_not_called()
            self.assertFalse(self.app.flash_active)
            self.assertIn("failed", self.app.flash_status.get())

    def test_loader_entry_uses_usb_port_when_other_loaders_appear(self):
        one = record()
        one["location"] = "5-2.1"
        self.load([one])
        with patch.object(padgui, "usb_location", side_effect=lambda path: "5-2.1" if path == "new" else "5-2.4"), \
             patch.object(padgui.pad, "cmd", return_value=one["identity"]), \
             patch.object(padgui.pad, "discover_devices", return_value=[("other", "UBOOT1.00", None), ("new", "UBOOT1.00", None)]), \
             patch.object(self.app, "scan") as scan:
            self.app.enter_flash_loader()
            self.finish()
            scan.assert_called_once_with(automatic=True, select_path="new")

    def test_flash_progress_updates_bar_and_stage(self):
        self.load([loader_record()])
        self.app.start_flash_progress("Preparing")
        self.app.flash_progress("Writing firmware", 25, 100)
        # Queue-only updates are safe for the worker; the UI applies them in drain.
        self.root.after_cancel(self.app.poll)
        self.app.drain()
        self.assertEqual(self.app.flash_bar["value"], 25)
        self.assertIn("Writing firmware", self.app.flash_status.get())
        self.assertTrue(self.app.flash_progress_frame.winfo_manager())
        self.assertIs(self.app.active_page, self.app.loader_page)


class TransportTests(unittest.TestCase):
    def test_loader_discovery_only_sends_inquiry(self):
        inquiry = b"\0" * 8 + b"BR23    " + b"UBOOT1.00".ljust(16, b" ") + b"\0" * 4
        with patch.object(padgui.pad, "device_paths", return_value=["rom"]), \
             patch.object(padgui.pad, "scsi", return_value=inquiry) as scsi:
            self.assertEqual(list(padgui.pad.discover_devices()), [])
            self.assertEqual(list(padgui.pad.discover_devices(include_loaders=True)), [("rom", "BR23    UBOOT1.00", None)])
            self.assertEqual(scsi.call_count, 2)
            self.assertTrue(all(call.args[1][0] == 0x12 for call in scsi.call_args_list))

    def test_windows_packet_layout(self):
        h = padtransport.ScsiPassThrough
        if ctypes.sizeof(ctypes.c_void_p) == 8:
            self.assertEqual(ctypes.sizeof(h), 56)
            self.assertEqual(h.DataBufferOffset.offset, 24)
            self.assertEqual(h.Cdb.offset, 36)

    def test_discovery_does_not_send_vendor_commands_to_other_disks(self):
        with patch.object(padgui.pad, "device_paths", return_value=["disk"]), \
             patch.object(padgui.pad, "inquiry", return_value="ATA System Disk"), \
             patch.object(padgui.pad, "scsi") as scsi:
            self.assertEqual(padgui.pad.devices(), [])
            scsi.assert_not_called()


class FlasherTests(unittest.TestCase):
    def test_build_pid_mismatch_is_rejected_before_device_access(self):
        import subprocess
        import tempfile
        from pathlib import Path
        exe = Path(__file__).resolve().parent.parent / "build/host/jlnewfw"
        if not exe.is_file():
            self.skipTest("build jlnewfw first")
        with tempfile.TemporaryDirectory() as tmp:
            ini = Path(tmp) / "isd_config.ini"
            ini.write_text("[EXTRA_CFG_PARAM]\nPID=GS_C2_Dongle;\n")
            result = subprocess.run([str(exe), "flashpack", "--pid", "GS_C2_ADC_DEVICE",
                                     "-c", str(ini)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("image PID does not match", result.stderr)
            self.assertNotIn("ROM loader", result.stderr)



class CalibrationViewTests(GuiCase):
    def test_calibration_reads_as_words(self):
        import struct
        r = record()
        r["fields"].append({"kind": 2, "id": 1, "name": "calibration", "type": padgui.pad.T_BYTES,
                            "size": 112, "help": "Calibration", "dev": False})
        stick = [5944, 7433, 16555, 16339, 26834, 26851] + [32767] * 16 + [0, 0]
        blob = struct.pack("<56H", *(stick + stick + [482, 12, 1638, 983] + [0, 0, 0, 0]))
        r["values"][9] = {"calibration": (blob, True)}
        self.load([r])
        self.app.show_target(9)
        view = next(w for w in self.app.special_editors if w.winfo_exists() and isinstance(w, padgui.CalibrationView))
        text = view.cget("text")
        self.assertIn("Left stick: X 5944–16555–26834", text)
        self.assertIn("outer edge 100–100%", text)
        self.assertIn("Left trigger: rest 482, fully pressed 12", text)
        self.assertIn("deadzones 5% at rest, 3% at the end", text)
        self.assertIn("Right trigger: not calibrated", text)
        r["draft"][9, "calibration"]["var"].set(bytes(112).hex())
        self.app.mark_dirty()
        self.assertIn("Left stick: not calibrated", view.cget("text"))


class RemapGuiTests(GuiCase):
    def open_remap(self, r, t=11):
        self.load([r])
        self.app.render_tree()
        self.app.show_target(t)
        return self.editor()

    def editor(self):
        return next(w for w in self.app.special_editors if w.winfo_exists() and type(w).__name__ == "RemapEditor")

    def test_remap_slots_are_tree_nodes_named_after_their_name(self):
        r = remap_record()
        self.load([r])
        self.app.render_tree()
        children = self.app.tree.get_children("device0")
        self.assertEqual(len(children), 18)
        self.assertEqual(self.app.tree.item(children[11], "text"), "Remap B")
        r["draft"][11, "name"]["var"].set("Mario Kart")
        self.assertEqual(self.app.tree.item(children[11], "text"), "Remap B · Mario Kart")

    def test_remap_page_has_the_stick_curves_and_deadzones(self):
        r = remap_record()
        self.open_remap(r)
        self.root.update()
        curves = [w for w in self.app.special_editors if w.winfo_exists() and type(w).__name__ == "CurveEditor"]
        self.assertEqual(len(curves), 2)
        self.assertIn("left_inner_deadzone", self.app.editors)
        r["draft"][11, "left_inner_deadzone"]["var"].set("5")
        self.assertTrue(self.app.changed(r["draft"][11, "left_inner_deadzone"]))

    def test_every_key_has_a_base_row_and_rules_add_to_it(self):
        r = remap_record()
        editor = self.open_remap(r)
        mapping, rules = r["draft"][11, "map"], r["draft"][11, "rules"]
        self.assertEqual(len(editor.base), 19)
        self.assertEqual(len(editor.adds), 19)
        self.assertIn(" 0 of 16 used", editor.slots.cget("text"))
        editor.change(lambda: editor.base.update(x="y", y="x", capture=None))
        self.assertEqual((mapping["var"].get(), rules["var"].get()), ("capture=none x=y y=x", "none"))
        self.assertIn(" 0 of 16 used", editor.slots.cget("text"))
        editor.adds[11].invoke()                          # R4's +
        self.assertEqual(editor.rules[0]["input"], "r4")
        rule = editor.rules[0]
        editor.change(lambda: editor.retrigger(rule, "tapped"))
        editor.change(lambda: editor.output(rule, "a", True))
        self.assertEqual(rules["var"].get(), "r4 tapped:200 tap a")
        self.assertIn(" 1 of 16 used", editor.slots.cget("text"))
        editor.change(lambda: editor.retrigger(rule, "held"))
        editor.change(lambda: rule.update(effect="turbo", rate=0))
        self.assertEqual(rules["var"].get(), "r4 held turbo a@max")
        # Reloaded from the entries: the same table.
        editor.shown = None
        editor.sync()
        self.assertEqual((editor.base["x"], editor.base["capture"]), ("y", None))
        self.assertEqual([padgui.pad.padremap.show_rule(x) for x in editor.rules], ["r4 held turbo a@max"])
        editor.change(lambda: editor.rules.pop(0))
        editor.change(lambda: editor.base.update(x="x", y="y", capture="capture"))
        self.assertEqual((mapping["var"].get(), rules["var"].get()), ("identity", "none"))
        self.assertFalse(self.app.changed(mapping) or self.app.changed(rules))
        self.assertFalse(self.errors)

    def test_turbo_modifier_keys_list_stays_open_while_picking(self):
        r = remap_record()
        editor = self.open_remap(r)
        rules = r["draft"][11, "rules"]
        editor.adds[11].invoke()                          # R4's Add rule
        editor = self.editor()
        editor.change(lambda: editor.rules[0].update(effect="turbo_modifier"))
        self.root.update()
        editor = self.editor()
        picker = editor.pickers[0]
        picker.invoke()
        self.root.update()
        boxes = [w for w in picker.panel.winfo_children()[0].winfo_children() if isinstance(w, padgui.ttk.Checkbutton)]
        self.assertEqual(len(boxes), 19)
        boxes[0].invoke()                                 # A
        boxes[1].invoke()                                 # B
        self.root.update()
        self.assertTrue(picker.panel and picker.panel.winfo_exists())
        self.assertEqual(picker.cget("text"), "A+B")
        self.assertEqual(rules["var"].get(), "r4 held turbo_modifier a+b@15")
        boxes[0].invoke()
        self.assertEqual(rules["var"].get(), "r4 held turbo_modifier b@15")
        done = next(w for w in picker.panel.winfo_children()[0].winfo_children()
                    if isinstance(w, padgui.ttk.Button) and w.cget("text") == "Done")
        done.invoke()
        self.assertIsNone(picker.panel)
        editor.change(lambda: editor.retrigger(editor.rules[0], "pressed"))
        self.assertEqual(rules["var"].get(), "r4 pressed toggle_turbo_modifier b@15")
        editor = self.editor()
        editor.pickers[0].invoke()
        self.root.update()
        editor.pickers[0].invoke()                        # the button again closes it
        self.assertIsNone(editor.pickers[0].panel)
        self.assertFalse(self.errors)

    def test_each_stick_has_its_settings_in_one_box(self):
        r = remap_record()
        self.open_remap(r)
        self.root.update()
        boxes = [self.app.stick_boxes[side] for side in ("left", "right")]
        self.assertEqual([str(b.cget("relief")) for b in boxes], ["groove", "groove"])
        for box, side in zip(boxes, ("left", "right")):
            labels = [l.cget("text") for f in box.winfo_children() if type(f) is padgui.ttk.Frame
                      for l in f.winfo_children() if type(l) is padgui.ttk.Label]
            self.assertEqual(labels, [f"{side.capitalize()} stick", "maps to", "Invert X axis", "Invert Y axis",
                                      "Inner deadzone", "Outer deadzone", "Curve points"])
            curve = next(w for w in box.winfo_children() if type(w).__name__ == "CurveEditor")
            self.assertIs(curve.entry, r["draft"][11, f"{side}_curve_points"])
        left, x = boxes[0], r["draft"][11, "left_x_axis"]
        combo = next(w for w in left.winfo_children() if isinstance(w, padgui.ttk.Combobox))
        self.assertEqual((combo.get(), list(combo.cget("values"))), ("Left stick", ["Left stick", "Right stick", "Nothing"]))
        combo.set("Right stick")
        self.assertEqual(r["draft"][11, "left_maps_to"]["var"].get(), "right stick")
        self.assertTrue(self.app.changed(r["draft"][11, "left_maps_to"]))
        self.assertEqual(self.app.value_bytes(r["draft"][11, "left_maps_to"]), b"\1")
        self.assertEqual(self.app.value_bytes(r["draft"][11, "right_maps_to"]), b"\0")
        check = next(w for w in left.winfo_children() if isinstance(w, padgui.ttk.Checkbutton))
        check.invoke()
        self.assertEqual((x["var"].get(), self.app.value_bytes(x)), ("inverted", b"\1"))
        x["var"].set("normal")                            # discard, default: the box follows
        self.assertFalse(check.instate(["selected"]))
        r["draft"][11, "left_maps_to"]["var"].set("nothing")
        self.assertEqual(combo.get(), "Nothing")
        self.assertFalse(self.errors)

    def test_columns_do_not_move_as_rules_come_and_go(self):
        r = remap_record()
        self.open_remap(r)
        self.root.update()
        before = [b.winfo_x() for b in self.editor().adds]
        r["draft"][11, "rules"]["var"].set("a double_tapped:300 toggle_turbo b@15; x tapped:200 tap a+b+x+y")
        self.app.mark_dirty()
        self.root.update()
        self.assertEqual([b.winfo_x() for b in self.editor().adds], before)

    def test_each_key_has_a_status_box_for_its_mapping_and_rules(self):
        r = remap_record()
        r["values"][11]["map"] = (padgui.pad.padremap.parse_map("x=y"), True)        # saved, not default
        editor = self.open_remap(r)
        self.assertEqual((editor.status("a")[0], editor.status("x")[0]), ("", "✓"))
        editor.change(lambda: editor.base.update(a="b"), redraw=False)
        editor.change(lambda: editor.rules.append(padgui.pad.padremap.rule("a", "held", "turbo", ["a"], rate=0)))
        editor = self.editor()
        self.assertEqual(editor.marks["a"].cget("text"), "*")
        editor.toggle("a")                                # discard A's edits: mapping and rules
        editor = self.editor()
        self.assertEqual((editor.base["a"], editor.rules), ("a", []))
        self.assertEqual(editor.marks["a"].cget("text"), "")
        editor.toggle("x")                                # back to the firmware default
        editor = self.editor()
        self.assertEqual(editor.base["x"], "x")
        self.assertEqual(r["draft"][11, "map"]["var"].get(), "identity")
        self.assertEqual(editor.marks["x"].cget("text"), "*")   # pending until Apply
        editor.change(lambda: editor.base.update(y=None), redraw=False)
        self.assertEqual(r["draft"][11, "map"]["var"].get(), "y=none")
        self.assertEqual(editor.marks["y"].cget("text"), "*")

    def test_plus_buttons_stop_at_sixteen_rule_entries(self):
        r = remap_record()
        editor = self.open_remap(r)
        for _ in range(16):
            editor.adds[0].invoke()
            editor = self.editor()
        self.assertEqual(len(editor.rules), 16)
        self.assertEqual({str(b.cget("state")) for b in editor.adds}, {"disabled"})
        self.assertIn(" 16 of 16 used", editor.slots.cget("text"))

    def test_preset_sets_base_rows_and_keeps_rules(self):
        r = remap_record()
        editor = self.open_remap(r)
        editor.change(lambda: editor.rules.append(padgui.pad.padremap.rule("a", "held", "turbo", ["a"], rate=0)))
        editor.preset("swap-ab-xy")
        self.assertEqual(r["draft"][11, "map"]["var"].get(), "a=b b=a x=y y=x")
        self.assertEqual(r["draft"][11, "rules"]["var"].get(), "a held turbo a@max")

    def test_export_then_import_a_preset_file(self):
        import json
        import tempfile
        r = remap_record()
        editor = self.open_remap(r)
        r["draft"][11, "name"]["var"].set("Mario Kart")
        editor.preset("swap-ab-xy")
        editor.change(lambda: editor.rules.append(padgui.pad.padremap.rule("r4", "tapped", "tap", ["a"], ms=300)))
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / "kart.json")
            with patch.object(padwidgets.filedialog, "asksaveasfilename", return_value=path) as ask:
                editor.export_file()
                self.assertEqual(ask.call_args.kwargs["initialfile"], "Mario-Kart.json")
            data = json.loads(Path(path).read_text())
            self.assertEqual(data, {"format": 1, "name": "Mario Kart", "map": "a=b b=a x=y y=x",
                                    "rules": "r4 tapped:300 tap a"})
            self.app.show_target(12)
            self.root.update()
            other = self.editor()
            with patch.object(padwidgets.filedialog, "askopenfilename", return_value=path):
                other.import_file()
        self.assertEqual(r["draft"][12, "name"]["var"].get(), "Mario Kart")
        self.assertEqual(r["draft"][12, "map"]["var"].get(), "a=b b=a x=y y=x")
        self.assertEqual(r["draft"][12, "rules"]["var"].get(), "r4 tapped:300 tap a")
        self.assertEqual([padgui.pad.padremap.show_rule(x) for x in other.rules], ["r4 tapped:300 tap a"])

    def test_import_refuses_a_bad_file_and_changes_nothing(self):
        import tempfile
        r = remap_record()
        editor = self.open_remap(r)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.json"
            path.write_text('{"format": 1, "map": "a=q"}')
            with patch.object(padwidgets.filedialog, "askopenfilename", return_value=str(path)), \
                 patch.object(padwidgets.messagebox, "showerror") as error:
                editor.import_file()
                error.assert_called_once()
        self.assertEqual(r["draft"][11, "map"]["var"].get(), "identity")

    def test_default_marker_resets_map_and_rules_together(self):
        r = remap_record()
        self.open_remap(r)
        r["draft"][11, "map"]["var"].set("x=y")
        r["draft"][11, "rules"]["var"].set("a held turbo a@max")
        entries = self.app.setting_entries(r, 11, "map", r["draft"][11, "map"])
        self.assertEqual(len(entries), 2)
        self.app.discard_setting(entries)
        self.assertEqual((r["draft"][11, "map"]["var"].get(), r["draft"][11, "rules"]["var"].get()), ("identity", "none"))
        self.assertEqual(self.editor().rules, [])
        self.assertEqual(self.editor().base["x"], "x")

    def test_apply_sends_remap_bytes(self):
        r = remap_record()
        self.load([r])
        r["draft"][12, "rules"]["var"].set("l4 pressed toggle_turbo a@15")
        r["draft"][12, "name"]["var"].set("Zelda")
        with patch.object(padgui.pad, "cmd", return_value=r["identity"]), \
             patch.object(padgui.pad, "send") as send, \
             patch.object(padgui.pad, "read_target", side_effect=lambda p, table, t: r["values"][t]):
            self.app.apply_settings()
            self.finish()
        items = dict(((t, fid), v) for t, fid, v in send.call_args[0][1])
        self.assertEqual(items[12, 1], b"Zelda".ljust(24, b"\0"))
        self.assertEqual(items[12, 3][:10], bytes([3, 8, 0, 5, 1, 0, 0, 0, 15, 0]))

if __name__ == "__main__":
    unittest.main()
