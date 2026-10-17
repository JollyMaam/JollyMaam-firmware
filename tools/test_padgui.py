"""GUI/device boundary tests. Use an X display (Xvfb is sufficient). No USB I/O."""
import ctypes
import time
import tkinter as tk
import unittest
from unittest.mock import patch

import padgui
import padtransport


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


class GuiTests(unittest.TestCase):
    def setUp(self):
        self.root = tk.Tk()
        self.root.withdraw()
        self.app = padgui.App(self.root)
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

    def test_worker_failure_restores_controls(self):
        def refused():
            raise SystemExit("device refused change")
        with patch.object(padgui.messagebox, "showerror") as error:
            self.app.run("Apply", refused)
            self.assertEqual(str(self.app.boards.cget("state")), "disabled")
            self.finish()
            error.assert_called_once()
        self.assertEqual(str(self.app.boards.cget("state")), "readonly")

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
        self.assertEqual(len(self.app.tree.get_children("device0")), 10)
        self.assertEqual(len(self.app.tree.get_children("device1")), 0)

    def test_actions_target_slot_and_old_firmware_blocks_calibration(self):
        r = record()
        self.load([r])
        with patch.object(padgui.pad, "cmd", return_value=r["identity"]) as cmd:
            self.app.slot_action("connect", 5)
            self.finish()
            self.assertEqual(cmd.call_args.args, ("fake", 0x13, 5))
        with patch.object(padgui.pad, "cmd", return_value=r["identity"]) as cmd, \
             patch.object(padgui.messagebox, "askokcancel", return_value=True):
            self.app.calibrate()
            self.finish()
            self.assertEqual(cmd.call_args.args, ("fake", 0x18, 0))
        r["identity"] = r["identity"][:4] + b"\2\0" + r["identity"][6:]
        with patch.object(padgui.pad, "cmd") as cmd, patch.object(padgui.messagebox, "showinfo"):
            self.app.slot_action("calibrate", 0)
            cmd.assert_not_called()

    def test_reset_shows_default_and_later_edit_overrides_reset(self):
        r = record()
        r["values"][0]["rumble"] = (b"\x50", True)
        r["defaults"] = {0: {"rumble": (b"\x64", False)}}
        self.load([r])
        entry = r["draft"][0, "rumble"]
        self.assertTrue(self.app.differs_from_default(entry))
        self.assertTrue(self.app.tree.item("device0/0", "text").endswith(" *"))
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
        editor.open_palette()
        self.assertEqual(len(editor.tiles), 16)
        editor.choose_colour(4)
        self.assertEqual(r["draft"][0, "primary_colour"]["var"].get(), "4:5")
        self.assertEqual(editor.level.get(), BRIGHTNESS_LABELS[5])
        self.assertIsNone(editor.popup)

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
        editor.set_point(3, 0)
        self.assertEqual(editor.points()[3], 48)
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

    def test_flash_backup_is_required_and_cannot_overwrite_input(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "image.img"
            image.write_bytes(b"fake")
            self.app.image.set(str(image))
            with patch.object(self.app, "loader_blob", return_value=b"loader"), \
                 patch.object(padgui.filedialog, "asksaveasfilename", return_value=str(image)), \
                 patch.object(padgui.messagebox, "showerror") as error, \
                 patch.object(self.app, "run") as run:
                self.app.flash(False)
                error.assert_called_once()
                run.assert_not_called()
            with patch.object(self.app, "loader_blob", return_value=b"loader"), \
                 patch.object(padgui.filedialog, "asksaveasfilename", return_value=""), \
                 patch.object(self.app, "run") as run:
                self.app.flash(False)
                run.assert_not_called()


class TransportTests(unittest.TestCase):
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
        exe = padgui.ROOT / "build/host/jlnewfw"
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



if __name__ == "__main__":
    unittest.main()
