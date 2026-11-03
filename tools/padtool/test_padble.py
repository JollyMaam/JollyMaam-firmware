"""Config mode's frames over Bluetooth LE: long data in marked frames, an
answer from marked pieces."""
import asyncio
import struct
import unittest

import padble


class FakePad:
    """profile_config.c's write(): collects frames with bit 7 set, runs the
    last, answers in pieces of 157."""

    def __init__(self, link, answer):
        self.link, self.answer, self.frames, self.held = link, answer, [], b""

    async def write_gatt_char(self, uuid, frame, response):
        self.frames.append(bytes(frame))
        self.held += frame[5:]
        if frame[0] & padble.MORE:
            return
        data, self.held = self.held, b""
        if data:
            return
        a = self.answer(frame[0])
        pieces = [a[i:i + 157] for i in range(0, len(a), 157)] or [b""]
        for i, p in enumerate(pieces):
            more = padble.MORE if i < len(pieces) - 1 else 0
            self.link.replies.put_nowait(bytes([frame[0] | more]) + p)


def link(answer, frame_data=91):
    l = padble.BleLink.__new__(padble.BleLink)
    l.loop = asyncio.new_event_loop()
    l.replies = asyncio.Queue()
    l.frame_data, l.chunk = frame_data, padble.CONFIG_DATA_MAX
    l.client = FakePad(l, answer)
    return l


class Frames(unittest.TestCase):
    def test_long_data_goes_in_marked_frames(self):
        l = link(lambda sub: b"")
        data = bytes(range(200))
        l.cmd(0x22, out=data)
        frames = l.client.frames
        self.assertEqual([f[0] for f in frames], [0xa2, 0xa2, 0x22])
        self.assertTrue(all(len(f) <= 96 for f in frames))
        self.assertEqual(b"".join(f[5:] for f in frames), data)

    def test_a_short_command_is_one_frame(self):
        l = link(lambda sub: b"x")
        self.assertEqual(l.cmd(0x13, 0xfe), b"")
        self.assertEqual(l.client.frames, [bytes([0x13]) + struct.pack("<I", 0xfe)])

    def test_answer_from_pieces(self):
        answer = bytes(i & 0xff for i in range(500))
        self.assertEqual(link(lambda sub: answer).cmd(0x02, length=512), answer)

    def test_an_exact_piece_ends_without_its_mark(self):
        answer = bytes(157)
        self.assertEqual(link(lambda sub: answer).cmd(0x20, length=512), answer)

    def test_an_empty_answer_ends(self):
        self.assertEqual(link(lambda sub: b"").cmd(0x20, 75, length=512), b"")

    def test_identify_gives_the_data_limit(self):
        self.assertEqual(padble.config_data_max(bytes(88)), padble.CONFIG_DATA_MAX)
        self.assertEqual(padble.config_data_max(bytes(88) + struct.pack("<H", 335)), 335)

    def test_scsi_pads_the_answer_and_refuses_usb_only(self):
        padble._links["ble:t"] = link(lambda sub: b"C2PD")
        try:
            self.assertEqual(padble.scsi("ble:t", [0xF5, 0x00], 8), b"C2PD\0\0\0\0")
            with self.assertRaises(OSError):
                padble.scsi("ble:t", [0xF5, 0x15, 0, 0, 0, 0], 16)
            with self.assertRaises(OSError):
                padble.scsi("ble:t", [0xF5, 0x1C, 1, 0, 0, 0, 5], 0)
        finally:
            del padble._links["ble:t"]


if __name__ == "__main__":
    unittest.main()


class NamedTest(unittest.TestCase):
    """A radio that advertises only a name (the U2C Bluetooth's core)."""

    class Thing:
        def __init__(self, name):
            self.name = self.local_name = name

    def test_named(self):
        self.assertTrue(padble.named(self.Thing("x"), self.Thing("U2CB Config")))
        self.assertTrue(padble.named(self.Thing("U2CB Config"), self.Thing(None)))
        self.assertFalse(padble.named(self.Thing("Pro Controller"), self.Thing(None)))
        self.assertFalse(padble.named(self.Thing(None), self.Thing(None)))
