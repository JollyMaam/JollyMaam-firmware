"""GUI/device boundary tests. Use an X display (Xvfb is sufficient). No USB I/O."""
import ctypes
import time
import tkinter as tk
import unittest
from unittest.mock import patch

import padgui
import padtransport


class GuiTests(unittest.TestCase):
    def setUp(self):
        self.root = tk.Tk()
        self.root.withdraw()
        self.app = padgui.App(self.root)
        self.errors = []
        self.root.report_callback_exception = lambda *args: self.errors.append(args)

    def tearDown(self):
        self.app.close()
        self.assertEqual(self.errors, [])

    def finish(self):
        deadline = time.monotonic() + 3
        while self.app.busy and time.monotonic() < deadline:
            self.root.update()
            time.sleep(0.01)
        self.assertFalse(self.app.busy)

    def test_worker_failure_restores_controls(self):
        def refused():
            raise SystemExit("device refused change")
        with patch.object(padgui.messagebox, "showerror") as error:
            self.app.run("Apply", refused)
            self.assertEqual(str(self.app.boards.cget("state")), "disabled")
            self.finish()
            error.assert_called_once()
        self.assertEqual(str(self.app.boards.cget("state")), "readonly")

    def test_settings_guard_and_apply_only_changed_fields(self):
        fields = [{"kind": 0, "id": 1, "name": "type", "type": padgui.pad.T_ENUM,
                   "names": ["switch", "hid"], "help": "Profile", "dev": False},
                  {"kind": 0, "id": 2, "name": "rumble", "type": padgui.pad.T_NUMBER,
                   "size": 1, "help": "Strength", "dev": False}]
        self.app.device.set("fake")
        with patch.object(padgui.pad, "fields", return_value=fields), \
             patch.object(padgui.pad, "read_target", return_value={"type": (b"\0", False), "rumble": (b"\x64", False)}):
            self.app.load_settings()
            self.finish()
        self.app.editors["rumble"][1].set("80")
        with patch.object(padgui.pad, "send") as send, patch.object(self.app, "load_settings"):
            self.app.apply_settings()
            self.finish()
            send.assert_called_once_with("fake", [(0, 2, b"\x50")])
        self.app.target.set("B")
        with patch.object(padgui.messagebox, "showinfo"), patch.object(padgui.pad, "send") as send:
            self.app.apply_settings()
            send.assert_not_called()

    def test_board_change_invalidates_device_and_fields(self):
        self.app.device.set("fake")
        self.app.board.set("dongle")
        self.app.change_board()
        self.assertEqual(self.app.device.get(), "")
        self.assertEqual(self.app.tabs.tab(self.app.settings, "state"), "disabled")
        self.assertTrue(self.app.bundle.get().endswith("dongle"))

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
                 patch.object(self.app, "invoke") as invoke:
                self.app.flash(False)
                error.assert_called_once()
                invoke.assert_not_called()
            with patch.object(self.app, "loader_blob", return_value=b"loader"), \
                 patch.object(padgui.filedialog, "asksaveasfilename", return_value=""), \
                 patch.object(self.app, "invoke") as invoke:
                self.app.flash(False)
                invoke.assert_not_called()


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
