"""The tool device's answers on input reports: only this process's, refusals
sent again, lost pieces caught."""
import struct
import unittest
from unittest.mock import patch

import padtransport as t


def piece(client, tag, state, offset, data=b""):
    return struct.pack("<IBBH", client, tag, state, offset) + data.ljust(56, b"\0")


class FakeDevice:
    """Features on a device: each set() makes the reports answer() returns."""

    def __init__(self, answer):
        self.answer, self.sets, self.queue = answer, [], []

    def set(self, data, full=True):
        self.sets.append(bytes(data))
        _, flags, tag, client = struct.unpack_from("<HBBI", data, 16)
        self.queue += self.answer(len(self.sets), client, tag)

    def read(self, timeout):
        return self.queue.pop(0) if self.queue else None

    def drain(self):
        self.queue.clear()

    def close(self):
        pass


class InputChoiceTests(unittest.TestCase):
    def test_input_answers_on_linux_and_windows_with_an_input_report(self):
        tools = {"a": ("pad", 64, 64), "b": ("pad", 512, 0)}
        for platform, want in (("linux", True), ("win32", True), ("darwin", False)):
            with patch.dict(t._tools, tools, clear=True), patch.object(t.sys, "platform", platform):
                self.assertEqual(t._input("a"), want, platform)
                self.assertFalse(t._input("b"), platform)
                self.assertEqual(t.data_max("a"), 64 - t.TOOL_HEAD_IN)

    def test_a_device_without_input_report_is_refused(self):
        with patch.dict(t._tools, {"/dev/hidraw9": ("pad", 512, 0)}, clear=True), \
                patch.object(t.sys, "platform", "linux"):
            with self.assertRaisesRegex(OSError, "no input report"):
                t._tool("/dev/hidraw9", [0xf5, 0], 88, 100, None)


class InputAnswerTests(unittest.TestCase):
    def command(self, device, length=88, out=None, timeout_ms=500):
        with patch.dict(t._open, {"dev": device}), patch.object(t, "_input", return_value=True):
            return t._tool("dev", [0xf5, 0], length, timeout_ms, out)

    def test_others_answers_are_passed_by(self):
        body = bytes(range(88))

        def answer(n, client, tag):
            return [piece(client ^ 1, tag, t.TOOL_LAST, 0, b"other"),
                    piece(client, tag ^ 1, t.TOOL_LAST, 0, b"stale"),
                    piece(client, tag, t.TOOL_MORE, 0, body[:56]),
                    piece(client ^ 1, tag, t.TOOL_MORE, 56, b"other"),
                    piece(client, tag, t.TOOL_LAST, 56, body[56:])]
        device = FakeDevice(answer)
        self.assertEqual(self.command(device), body)
        self.assertEqual(device.sets[0][16:24], struct.pack("<HBBI", 88, t.TOOL_INPUT, device.sets[0][19], t._client))

    def test_refused_command_is_sent_again(self):
        def answer(n, client, tag):
            return [piece(client, tag, t.TOOL_REFUSED, 0)] if n < 3 else [piece(client, tag, t.TOOL_LAST, 0, b"ok")]
        device = FakeDevice(answer)
        self.assertEqual(self.command(device, length=2), b"ok")
        self.assertEqual(len(device.sets), 3)

    def test_lost_piece_is_an_error(self):
        device = FakeDevice(lambda n, client, tag: [piece(client, tag, t.TOOL_LAST, 56, b"late")])
        with self.assertRaisesRegex(OSError, "lost a piece"):
            self.command(device)

    def test_write_carries_its_data_after_the_client(self):
        device = FakeDevice(lambda n, client, tag: [piece(client, tag, t.TOOL_LAST, 0)])
        self.assertEqual(self.command(device, out=b"data"), b"")
        self.assertEqual(device.sets[0][18] & 3, t.TOOL_INPUT | t.TOOL_OUT)
        self.assertEqual(device.sets[0][t.TOOL_HEAD_IN:], b"data")

    def test_no_answer_times_out(self):
        with self.assertRaisesRegex(OSError, "no answer"):
            self.command(FakeDevice(lambda n, client, tag: []), timeout_ms=50)

    def test_stale_reports_are_drained_before_sending(self):
        device = FakeDevice(lambda n, client, tag: [piece(client, tag, t.TOOL_LAST, 0, b"ok")])
        device.queue = [piece(t._client, 0, t.TOOL_LAST, 0, b"old")] * 64
        self.assertEqual(self.command(device, length=2), b"ok")


if __name__ == "__main__":
    unittest.main()
