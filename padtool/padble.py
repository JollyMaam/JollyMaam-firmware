"""The tool's commands over Bluetooth LE: to the updater
(src/updater/updater_ble.c), for padtool update --ble, and to config mode
(src/profiles/profile_config.c, Home+M held), for padtool --ble with any
other command.  Needs bleak (pip install bleak, or nix-shell -p
python3Packages.bleak).

A command is a write to REQUEST: u8 sub-command, u32 argument, then data; a
command with an answer gets it in REPLY notifications: u8 sub-command, then
a piece of the answer.  Config mode takes a frame of at most BT_WRITE_MAX
bytes, so more data goes in several, each but the last with bit 7 of the
sub-command set, and it sends an answer in pieces marked the same way; the
updater's frames and answers are one piece.  Every write waits for its response, which the pad sends once it has
run the command: one write in flight.

On Linux padtool leaves a config mode link up when it is done, and the next
run takes the link BlueZ still holds (no scan, no connection: about 0.1 s);
the pad ends config mode after a minute without a command.  Ending the link
each time cost 2 s (BlueZ waits before it disconnects), or, when the pad
ended it, sometimes 20 s of the kernel holding the next scan or connection
(seen with BlueZ on Linux; cause not found).  Elsewhere every link ends:
bleak's WinRT backend holds a connection with maintain_connection, and
Windows then connects to whatever comes up on that address (the pad
restarted into its updater, or in config mode again), so it never shows
advertising to a scan.  Writes without response flooded the
pad's HCI receive queue while it erased, and packets it dropped there were
pieced together wrongly."""
import asyncio, atexit, os, struct, sys

# The zip (make padtool-dist) carries bleak for Windows and Linux; on Linux
# the system's own comes first.
_LIB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lib")
if os.path.isdir(_LIB) and _LIB not in sys.path:
    if sys.platform == "win32":
        sys.path.insert(0, _LIB)
    else:
        sys.path.append(_LIB)

SERVICE = "c2550000-3e2d-66b1-8a4f-d5037c2b419e"
REQUEST = "c2550001-3e2d-66b1-8a4f-d5037c2b419e"
REPLY = "c2550002-3e2d-66b1-8a4f-d5037c2b419e"
MORE = 0x80
CONFIG_FRAME_DATA = 96 - 5       # BT_WRITE_MAX less the frame's header
CONFIG_DATA_MAX = 488            # profile_config.c's, as the tool device's
REBOOTING = (0x10, 0x11, 0x40)   # their answer may never come: the pad has gone
NOT_OVER_BLE = {0x15: "peek reads as many words as fit, registers too"}


class BleLink:
    def __init__(self, target, scan_s=10, updater=True, device=None):
        try:
            from bleak import BleakClient, BleakScanner
        except ImportError:
            sys.exit("padtool: --ble needs bleak (pip install bleak, or nix-shell -p python3Packages.bleak)")
        self.loop = asyncio.new_event_loop()
        self.replies = asyncio.Queue()
        what = "updater" if updater else "pad in config mode"

        async def connect():
            def wanted(d, adv):
                if SERVICE not in [u.lower() for u in adv.service_uuids]:
                    return False
                return not target or target.lower() in (d.address.lower(), (d.name or "").lower())
            nonlocal device
            if not device and not updater:
                device = next(iter(await connected(target)), None)
            if not device:
                device = await BleakScanner.find_device_by_filter(wanted, timeout=scan_s)
            if not device:
                sys.exit(f"padtool: no {what} advertising{' as ' + target if target else ''} " +
                         ("(Home+M on a pad that is not connected)" if updater else
                          "(hold Home+M for 3 s, or padtool config on USB)"))
            print(f"{what} {device.name or '?'} {device.address}", file=sys.stderr)
            self.address, self.name = device.address, device.name
            client = BleakClient(device)
            await client.connect()
            await client.start_notify(REPLY, lambda _, d: self.replies.put_nowait(bytes(d)))
            acquire = getattr(client._backend, "_acquire_mtu", None)    # BlueZ: else 23 is assumed
            if acquire:
                try:
                    await acquire()
                except Exception:
                    pass
            return client

        self.client = self.loop.run_until_complete(connect())
        # A frame: the MTU less the ATT write header (3) and the frame's (5);
        # config mode takes at most BT_WRITE_MAX, and collects several.
        self.chunk = self.frame_data = min(512, self.client.mtu_size - 8)
        identify = self.cmd(0x00, length=90)
        self.config = not identify[7]
        if self.config:
            self.frame_data = min(self.frame_data, CONFIG_FRAME_DATA)
            self.chunk = config_data_max(identify)

    def cmd(self, sub, arg=0, length=0, out=None, timeout_ms=3000):
        data = out or b""
        pieces = [data[i:i + self.frame_data] for i in range(0, len(data), self.frame_data)] or [b""]
        head = struct.pack("<I", arg)

        async def run():
            while not self.replies.empty():
                self.replies.get_nowait()
            for i, piece in enumerate(pieces):
                more = MORE if i < len(pieces) - 1 else 0
                await self.client.write_gatt_char(REQUEST, bytes([sub | more]) + head + piece, response=True)
            if not length:
                return b""
            answer = b""
            while True:
                try:
                    reply = await asyncio.wait_for(self.replies.get(), timeout_ms / 1000)
                except asyncio.TimeoutError:
                    raise OSError(f"no answer to command {sub:#04x} in {timeout_ms} ms") from None
                if reply[0] & ~MORE != sub:
                    continue
                answer += reply[1:]
                if not reply[0] & MORE:
                    return answer
        return self.loop.run_until_complete(run())

    def reopen(self, seconds):
        return False             # the app has no updater service

    def close(self, keep=None):
        """End the link: on Linux a config mode link stays up for the next
        run (keep None), unless keep is False."""
        if keep is None:
            keep = self.config and sys.platform == "linux"
        if keep:
            return
        try:
            self.loop.run_until_complete(self.client.disconnect())
        except Exception:
            pass


def config_data_max(identify):
    """The most data a command takes in config mode: IDENTIFY's u16 at 88
    (a pad with a smaller buffer), else CONFIG_DATA_MAX."""
    if len(identify) >= 90:
        return struct.unpack_from("<H", identify, 88)[0] or CONFIG_DATA_MAX
    return CONFIG_DATA_MAX


async def connected(target=""):
    """The pads BlueZ is still connected to that offer the service (one an
    earlier run left up), as bleak devices; none off Linux."""
    if sys.platform != "linux":
        return []
    try:
        from bleak.backends.device import BLEDevice
        from dbus_fast import BusType
        from dbus_fast.aio import MessageBus
        bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
        try:
            root = bus.get_proxy_object("org.bluez", "/", await bus.introspect("org.bluez", "/"))
            objects = await root.get_interface("org.freedesktop.DBus.ObjectManager").call_get_managed_objects()
        finally:
            bus.disconnect()
    except Exception:
        return []
    found = []
    for path, interfaces in sorted(objects.items()):
        d = interfaces.get("org.bluez.Device1")
        if not d or "Connected" not in d or not d["Connected"].value or SERVICE not in d["UUIDs"].value:
            continue
        address, name = d["Address"].value, d["Name"].value if "Name" in d else None
        if not target or target.lower() in (address.lower(), (name or "").lower()):
            found.append(BLEDevice(address, name, {"path": path, "props": {}}))
    return found


def scan(seconds=3):
    """The pads in config mode or their updater: connected (from an earlier
    run), or advertising for that long.  For padgui's Bluetooth scan."""
    try:
        from bleak import BleakScanner
    except ImportError:
        sys.exit("padtool: Bluetooth needs bleak (pip install bleak, or nix-shell -p python3Packages.bleak)")

    async def look():
        found = {d.address: d for d in await connected()}
        seen = await BleakScanner.discover(timeout=seconds, return_adv=True)
        for d, adv in seen.values():
            if SERVICE in [u.lower() for u in adv.service_uuids]:
                found.setdefault(d.address, d)
        return list(found.values())
    return asyncio.new_event_loop().run_until_complete(look())


def find_updater(flash_id, address, seconds=30):
    """After UPDATER: the updater of the pad with that flash ID (16 bytes,
    IDENTIFY's 24-40), on address or, failing that, any updater that
    answers with it."""
    try:
        link = BleLink(address, scan_s=seconds)
        if link.cmd(0x00, length=88)[24:40] == flash_id:
            return link
        link.close(keep=False)
    except SystemExit:
        pass
    for device in scan(5):
        if not (device.name or "").endswith(" Update"):
            continue
        try:
            link = BleLink(device.address, scan_s=5, device=device)
        except (Exception, SystemExit):
            continue
        if link.cmd(0x00, length=88)[24:40] == flash_id:
            return link
        link.close(keep=False)
    sys.exit(f"padtool: the pad's updater did not appear (looked {seconds} s at {address}, then every updater)")


_links = {}


@atexit.register
def _close():
    for l in _links.values():
        l.close()


def is_ble(path):
    return path.startswith("ble:")


def link(path, device=None):
    """The config mode link of a ble:[NAME|ADDRESS] path, connected once
    (to device, when scan() found it)."""
    if path not in _links:
        _links[path] = BleLink(path[4:], updater=False, device=device)
    return _links[path]


def alive(path):
    """A link to path is up: the device monitor looks at these only, and
    never connects (a scan takes seconds)."""
    l = _links.get(path)
    if l is None:
        return False
    l.loop.run_until_complete(asyncio.sleep(0.01))    # take in BlueZ's news
    if not l.client.is_connected:
        _links.pop(path, None)
        l.close(keep=False)      # Windows: or it connects again whenever the pad advertises
        return False
    return True


def name(path):
    l = _links.get(path)
    return (l.name if l else None) or "Bluetooth pad"


def scsi(path, cdb, length, timeout_ms=3000, out=None):
    """padtransport.scsi() for a ble: path: a tool command as a frame."""
    cdb = bytes(cdb).ljust(16, b"\0")
    if cdb[0] != 0xF5 or any(cdb[6:]):
        raise OSError(f"{path}: this command needs USB")
    if cdb[1] in NOT_OVER_BLE:
        raise OSError(f"{path}: not over Bluetooth ({NOT_OVER_BLE[cdb[1]]})")
    l = link(path)
    arg, = struct.unpack_from("<I", cdb, 2)
    if out is not None and len(out) > l.chunk:
        raise ValueError(f"config mode: at most {l.chunk} bytes to the pad")
    try:
        answer = l.cmd(cdb[1], arg, 0 if out is not None else length, out, timeout_ms)
    except Exception as error:
        if cdb[1] not in REBOOTING:
            if not l.client.is_connected:
                _links.pop(path, None)       # gone: the next command connects anew
                l.close(keep=False)
            raise OSError(f"{path}: {error or type(error).__name__}") from error
        answer = b""
    if cdb[1] in REBOOTING:       # its link has gone: let the system go of it too
        _links.pop(path, None)
        l.close(keep=False)
    return answer[:length].ljust(length, b"\0") if out is None else b""
