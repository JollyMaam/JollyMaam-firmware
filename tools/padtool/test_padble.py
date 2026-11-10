"""Config mode's frames over Bluetooth LE: a tag in every frame, long data
in marked frames, an answer from tagged pieces, copies and refusals."""
import asyncio
import struct
import unittest

import padble


class FakePad:
    """profile_config.c's write(): u8 sub, u8 tag, u32 arg, data; collects
    frames with bit 7 set, runs the last, answers every command in pieces of
    up to 157 bytes after the header byte: the first u8 1, u8 number, u32
    tag, u16 length; the next u8 index << 1, u8 number; refuses while busy
    (u8 3, u32 tag)."""

    def __init__(self, link, answer):
        self.link, self.answer, self.frames, self.held = link, answer, [], b""
        self.copies, self.busy, self.others, self.lose, self.drop = 1, 0, False, (), 0

    def send(self, piece):
        for _ in range(self.copies):
            self.link.replies.put_nowait(piece)

    async def write_gatt_char(self, uuid, frame, response):
        self.frames.append(bytes(frame))
        tag, = struct.unpack_from("<I", frame, 1)
        if self.drop:                    # lost on the way, or its refusal
            self.drop -= 1
            return
        if self.busy:
            self.busy -= 1
            self.send(b"\x03" + struct.pack("<I", tag))
            return
        self.held += frame[padble.HEAD:]
        if frame[0] & padble.MORE:
            return
        data, self.held = self.held, b""
        a = b"" if data else self.answer(frame[0])
        self.number = (getattr(self, "number", 0) + 1) & 0xff
        pieces, o = [], 0
        while True:
            head = (b"\x01" + bytes([self.number]) + struct.pack("<IH", tag, len(a)) if not pieces
                    else bytes([len(pieces) << 1, self.number]))
            room = 157 - (len(head) - 1)
            pieces.append(head + a[o:o + room])
            o += room
            if o >= len(a):
                break
        for i, p in enumerate(pieces):
            if self.others:              # another program's: its answer's end (before ours), its refusal
                if i == 0:
                    self.send(bytes([2, self.number - 1]) + b"not ours")
                    self.send(b"\x01\x77" + struct.pack("<IH", tag ^ 1, 8) + b"not ours")
                self.send(b"\x03" + struct.pack("<I", tag ^ 1))
            if i not in self.lose:
                self.send(p)
        self.lose = ()                   # once


def link(answer, frame_data=87):
    l = padble.BleLink.__new__(padble.BleLink)
    l.loop = asyncio.new_event_loop()
    l.replies = asyncio.Queue()
    l.frame_data, l.chunk = frame_data, padble.CONFIG_DATA_MAX
    l.config = True
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
        self.assertEqual(len({f[1:5] for f in frames}), 1, "one tag for the command's frames")

    def test_a_short_command_is_one_frame(self):
        l = link(lambda sub: b"x")
        self.assertEqual(l.cmd(0x13, 0xfe), b"")
        f, = l.client.frames
        self.assertEqual(f[0], 0x13)
        self.assertEqual(f[5:], struct.pack("<I", 0xfe))

    def test_copies_and_other_programs_answers_are_dropped(self):
        answer = bytes(i & 0xff for i in range(500))
        l = link(lambda sub: answer)
        l.client.copies, l.client.others = 2, True
        self.assertEqual(l.cmd(0x02, length=512), answer)
        l.client.copies = 4
        self.assertEqual(l.cmd(0x02, length=512), answer)

    def test_a_lost_piece_sends_the_command_again(self):
        answer = bytes(i & 0xff for i in range(500))
        l = link(lambda sub: answer)
        l.client.lose = (1, 2)               # two: the parity bit missed those
        self.assertEqual(l.cmd(0x02, length=512), answer)
        self.assertEqual(len(l.client.frames), 2)

    def test_a_command_nothing_comes_of_goes_again(self):
        l = link(lambda sub: b"ok")
        l.client.drop = 1
        self.assertEqual(l.cmd(0x01, length=16), b"ok")
        self.assertEqual(len(l.client.frames), 2)

    def test_a_refused_command_goes_again_with_a_new_tag(self):
        l = link(lambda sub: b"ok")
        l.client.busy = 3
        self.assertEqual(l.cmd(0x01, length=16), b"ok")
        tags = [f[1:5] for f in l.client.frames]
        self.assertEqual(len(tags), 4)
        self.assertTrue(all(a != b for a, b in zip(tags, tags[1:])))

    def test_answer_from_pieces(self):
        answer = bytes(i & 0xff for i in range(500))
        self.assertEqual(link(lambda sub: answer).cmd(0x02, length=512), answer)

    def test_an_exact_piece_ends(self):
        answer = bytes(151)
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
