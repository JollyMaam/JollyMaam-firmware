"""Config mode's frames over Bluetooth LE: a tag in every frame, long data
in marked frames, an answer from tagged pieces, copies and refusals."""
import asyncio
import struct
import unittest

import padble


class FakePad:
    """profile_config.c's write(): u8 sub, u8 tag, u32 arg, data; collects
    frames with bit 7 set, runs the last, answers every command in pieces of
    157 (u8 tag << 2 | parity << 1 | more); refuses while busy."""

    def __init__(self, link, answer):
        self.link, self.answer, self.frames, self.held = link, answer, [], b""
        self.copies, self.busy, self.others = 1, 0, False

    def send(self, piece):
        for _ in range(self.copies):
            self.link.replies.put_nowait(piece)

    async def write_gatt_char(self, uuid, frame, response):
        self.frames.append(bytes(frame))
        tag = frame[1] & 0x3f
        if self.busy:
            self.busy -= 1
            self.send(bytes([tag << 2 | 1]))
            return
        self.held += frame[padble.HEAD:]
        if frame[0] & padble.MORE:
            return
        data, self.held = self.held, b""
        a = b"" if data else self.answer(frame[0])
        pieces = [a[i:i + 157] for i in range(0, len(a), 157)] or [b""]
        for i, p in enumerate(pieces):
            if self.others:
                self.send(bytes([(tag ^ 1) << 2]) + b"not ours")
            self.send(bytes([tag << 2 | (i & 1) << 1 | (i < len(pieces) - 1)]) + p)


def link(answer, frame_data=90):
    l = padble.BleLink.__new__(padble.BleLink)
    l.loop = asyncio.new_event_loop()
    l.replies = asyncio.Queue()
    l.frame_data, l.chunk = frame_data, padble.CONFIG_DATA_MAX
    l.config, l.tag = True, None
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
        self.assertEqual(b"".join(f[padble.HEAD:] for f in frames), data)
        self.assertEqual(len({f[1] for f in frames}), 1, "one tag for the command's frames")

    def test_a_short_command_is_one_frame(self):
        l = link(lambda sub: b"x")
        self.assertEqual(l.cmd(0x13, 0xfe), b"")
        f, = l.client.frames
        self.assertEqual(f[0], 0x13)
        self.assertLess(f[1], 64)
        self.assertEqual(f[2:], struct.pack("<I", 0xfe))

    def test_copies_and_other_programs_answers_are_dropped(self):
        answer = bytes(i & 0xff for i in range(500))
        l = link(lambda sub: answer)
        l.client.copies, l.client.others = 2, True
        self.assertEqual(l.cmd(0x02, length=512), answer)
        l.client.copies = 4
        self.assertEqual(l.cmd(0x02, length=512), answer)

    def test_a_refused_command_goes_again_with_a_new_tag(self):
        l = link(lambda sub: b"ok")
        l.client.busy = 3
        self.assertEqual(l.cmd(0x01, length=16), b"ok")
        tags = [f[1] for f in l.client.frames]
        self.assertEqual(len(tags), 4)
        self.assertTrue(all(a != b for a, b in zip(tags, tags[1:])))

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
