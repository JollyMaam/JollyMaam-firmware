"""padinstall's loop, with devices and answers simulated."""
import unittest
from unittest.mock import patch

import padinstall


class Stop(Exception):
    pass


def device(kind="ours8", key="id:1", pid="U2C_WIRELESS", entry=None):
    return padinstall.Found(kind, key, pid, f"a {kind}", "path", "5-2.1", entry)


class LoopTests(unittest.TestCase):
    def run_loop(self, looks, answers, installed=None):
        """run() over these scans (one per loop), answering in turn; the
        prompts asked and the devices installed."""
        looks, answers, asked = iter(looks), iter(answers), []

        def look(self):
            try:
                return next(looks)
            except StopIteration:
                raise Stop()

        def ask(question, default=False):
            asked.append(question)
            return next(answers)
        with patch.object(padinstall.Scanner, "look", look), \
             patch.object(padinstall, "ask", side_effect=ask), \
             patch.object(padinstall, "image_for", return_value=padinstall.Path("x/firmware.img")), \
             patch.object(padinstall, "install_8bitdo", side_effect=lambda d, image, bar: installed.append(d.key)), \
             patch.object(padinstall, "back", return_value=device(key="id:new")), \
             patch.object(padinstall.time, "sleep"), \
             patch("builtins.print"):
            with self.assertRaises(Stop):
                padinstall.run()
        return asked

    def test_declined_is_asked_again_only_after_it_goes(self):
        installed = []
        pad = device()
        asked = self.run_loop([[pad], [pad], [], [pad]], [False, True], installed)
        self.assertEqual(len(asked), 2)
        self.assertEqual(installed, ["id:1"])

    def test_installed_is_not_asked_again_on_this_firmware(self):
        installed = []
        asked = self.run_loop([[device(kind="boot", key="hid")], [device(key="id:new")], [device(key="id:new")]],
                              [True], installed)
        self.assertEqual(len(asked), 1)

    def test_stock_mode_without_a_command_is_told_how_not_asked(self):
        entry = next(e for e in padinstall.padknown.STOCK if e.pid == "GS_C2_ADC_DEVICE" and not e.usage_page)
        asked = self.run_loop([[device("stock", "hid", "GS_C2_ADC_DEVICE", entry)]] * 3, [])
        self.assertEqual(asked, [])

    def test_new_loader_is_picked_by_port_else_the_only_new_one(self):
        self.assertEqual(padinstall.pick([("a", "5-2.4"), ("b", "5-2.1")], set(), "5-2.1"), "b")
        self.assertEqual(padinstall.pick([("a", None), ("b", None)], {"a"}, None), "b")
        with self.assertRaises(OSError):
            padinstall.pick([("a", None), ("b", None)], set(), None)


if __name__ == "__main__":
    unittest.main()
