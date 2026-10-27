"""The updater's commands over Bluetooth LE (src/updater/updater_ble.c), for
padtool update --ble.  Needs bleak (pip install bleak, or nix-shell -p
python3Packages.bleak).

A command is a write to REQUEST: u8 sub-command, u32 argument, then data; a
command with an answer gets it in a REPLY notification: u8 sub-command, the
answer.  Every write waits for its response, which the pad sends once it has
run the command: one write in flight.  Writes without response flooded the
pad's HCI receive queue while it erased, and packets it dropped there were
pieced together wrongly."""
import asyncio, struct, sys

SERVICE = "c2550000-3e2d-66b1-8a4f-d5037c2b419e"
REQUEST = "c2550001-3e2d-66b1-8a4f-d5037c2b419e"
REPLY = "c2550002-3e2d-66b1-8a4f-d5037c2b419e"


class BleLink:
    def __init__(self, target, scan_s=10):
        try:
            from bleak import BleakClient, BleakScanner
        except ImportError:
            sys.exit("padtool: --ble needs bleak (pip install bleak, or nix-shell -p python3Packages.bleak)")
        self.loop = asyncio.new_event_loop()
        self.replies = asyncio.Queue()

        async def connect():
            def wanted(d, adv):
                if SERVICE not in [u.lower() for u in adv.service_uuids]:
                    return False
                return not target or target.lower() in (d.address.lower(), (d.name or "").lower())
            device = await BleakScanner.find_device_by_filter(wanted, timeout=scan_s)
            if not device:
                sys.exit(f"padtool: no updater advertising{' as ' + target if target else ''} "
                         f"(Home+M on a pad that is not connected)")
            print(f"updater {device.name or '?'} {device.address}")
            client = BleakClient(device)
            await client.connect()
            await client.start_notify(REPLY, lambda _, d: self.replies.put_nowait(bytes(d)))
            return client

        self.client = self.loop.run_until_complete(connect())
        # The MTU less the ATT write header (3) and the frame's (5).
        self.chunk = min(512, self.client.mtu_size - 8)

    def cmd(self, sub, arg=0, length=0, out=None, timeout_ms=3000):
        frame = bytes([sub]) + struct.pack("<I", arg) + (out or b"")

        async def run():
            await self.client.write_gatt_char(REQUEST, frame, response=True)
            if not length:
                return b""
            while True:
                reply = await asyncio.wait_for(self.replies.get(), timeout_ms / 1000)
                if reply[0] == sub:
                    return reply[1:]
        return self.loop.run_until_complete(run())

    def reopen(self, seconds):
        return False             # the app has no updater service

    def close(self):
        try:
            self.loop.run_until_complete(self.client.disconnect())
        except Exception:
            pass
