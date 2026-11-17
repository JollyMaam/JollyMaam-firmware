"""The tool's commands over Bluetooth LE: to the updater
(src/updater/updater_ble.c), for padtool update --ble, and to config mode
(src/profiles/profile_config.c, Home+M held), for padtool --ble with any
other command.  Needs bleak (pip install bleak, or nix develop).

A command is a write to REQUEST: u8 sub-command, u32 tag (random, picked
here), u32 argument, then data; its answer comes in REPLY notifications:
the first u8 FIRST, u8 the answer's number, u32 tag, u16 the answer's
length, then data; the next u8 index << 1, u8 number, then data; a refusal
is u8 REFUSAL, u32 tag.  Every program subscribed gets every notification
(and BlueZ one copy more per other program), and under load some get lost,
so a first piece or refusal with another tag is not ours, and after our
first only our answer's number with the next index is: a smaller one is a
copy, a bigger one a piece lost, and the command goes again.
Config mode takes a frame of at most BT_WRITE_MAX bytes, so more data goes
in several, each but the last with bit 7 of the sub-command set; it answers
every command (one with data with nothing), and refuses one while another
program's runs: it goes again with a new tag.  The updater's frames and
answers are one piece, and a command with data has no answer there.  Every
write waits for its response, which the pad sends once it has run the
command: one write in flight.

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
import asyncio, atexit, os, random, struct, sys

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
HEAD = 9                         # u8 sub-command, u32 tag, u32 argument
FIRST, REFUSAL = 0x01, 0x03     # a piece's u8 header, else index << 1 (profile_config.c)
LOSSES_MAX = 5                   # answers with a piece lost this often: give up
CONFIG_FRAME_DATA = 96 - HEAD    # BT_WRITE_MAX less the frame's header
REFUSALS_MAX = 100               # refused this often (10-50 ms apart): give up
BUSY_MAX = 100                   # BlueZ's "In Progress" this often: give up
DEBUG = bool(os.environ.get("PADBLE_DEBUG"))   # every frame to and from the pad on stderr
SILENT_S = 0.5                   # nothing of a command (config mode) this long: send it again
CONFIG_DATA_MAX = 488            # profile_config.c's, as the tool device's
REBOOTING = (0x10, 0x11, 0x40)   # their answer may never come: the pad has gone
NOT_OVER_BLE = {0x15: "peek reads as many words as fit, registers too"}


def named(d, adv):
    """A pad whose radio advertises only a name (the U2C Bluetooth's core):
    config mode as "<board> Config"."""
    return (adv.local_name or d.name or "").endswith(" Config")


class BleLink:
    def __init__(self, target, scan_s=10, updater=True, device=None):
        try:
            from bleak import BleakClient, BleakScanner
        except ImportError:
            sys.exit("padtool: --ble needs bleak (pip install bleak, or nix develop)")
        self.loop = asyncio.new_event_loop()
        self.replies = asyncio.Queue()
        self.config = not updater
        what = "updater" if updater else "pad in config mode"

        async def connect():
            def wanted(d, adv):
                if SERVICE not in [u.lower() for u in adv.service_uuids] and not named(d, adv):
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
            await busy_retry(client.connect)
            # BlueZ: shared notifications (StartNotify), not acquired ones,
            # which no other program (a browser) could then subscribe to.
            shared = {"bluez": {"use_start_notify": True}} if sys.platform == "linux" else {}
            def notified(_, d):
                if DEBUG:
                    print(f"ble RX {bytes(d).hex()}", file=sys.stderr, flush=True)
                self.replies.put_nowait(bytes(d))
            await busy_retry(lambda: client.start_notify(REPLY, notified, **shared))
            acquire = getattr(client._backend, "_acquire_mtu", None)    # BlueZ: else 23 is assumed
            if acquire:
                try:
                    await acquire()
                except Exception:
                    pass
            return client

        self.client = self.loop.run_until_complete(connect())
        # A frame: the MTU less the ATT write header (3) and the frame's;
        # config mode takes at most BT_WRITE_MAX, and collects several.
        self.chunk = self.frame_data = min(512, self.client.mtu_size - 3 - HEAD)
        identify = self.cmd(0x00, length=90)
        self.config = not identify[7]
        if self.config:
            self.frame_data = min(self.frame_data, CONFIG_FRAME_DATA)
            self.chunk = config_data_max(identify)

    def cmd(self, sub, arg=0, length=0, out=None, timeout_ms=3000, response=True):
        data = out or b""
        pieces = [data[i:i + self.frame_data] for i in range(0, len(data), self.frame_data)] or [b""]
        # Config mode answers every command; the updater none with data.
        wanted = length or (self.config and data)

        async def once(watch):
            tag = random.getrandbits(32)
            while not self.replies.empty():
                self.replies.get_nowait()
            head = struct.pack("<II", tag, arg)
            for i, piece in enumerate(pieces):
                more = MORE if i < len(pieces) - 1 else 0
                await self.write(bytes([sub | more]) + head + piece, response)
            if not wanted:
                return b""
            answer, size, number, index = b"", None, None, 1
            loop = asyncio.get_running_loop()
            end = loop.time() + timeout_ms / 1000
            # Config mode: nothing of the command for SILENT_S, it or its
            # refusal was lost; an answer stopping that long, its last pieces.
            silent = loop.time() + SILENT_S if watch and self.config else None
            while True:
                wait = (min(silent, end) if silent else end) - loop.time()
                try:
                    reply = await asyncio.wait_for(self.replies.get(), max(wait, 0))
                except asyncio.TimeoutError:
                    if silent and loop.time() < end:
                        return SILENT if size is None else LOST
                    raise OSError(f"no answer to command {sub:#04x} in {timeout_ms} ms") from None
                if len(reply) < 2:
                    continue
                if reply[0] == REFUSAL:
                    if len(reply) >= 5 and struct.unpack_from("<I", reply, 1)[0] == tag:
                        return None                   # refused: another program's command runs
                    continue
                if reply[0] == FIRST:
                    if size is not None or len(reply) < 8 or struct.unpack_from("<I", reply, 2)[0] != tag:
                        continue                      # another program's, or a copy of ours
                    number = reply[1]
                    size, = struct.unpack_from("<H", reply, 6)
                    data = reply[8:]
                else:
                    if size is None or reply[0] & 1 or reply[1] != number or reply[0] >> 1 < index:
                        continue                      # another's answer, or a copy
                    if reply[0] >> 1 > index:
                        return LOST                   # a piece before it never came
                    data = reply[2:]
                    index += 1
                if silent:
                    silent = loop.time() + SILENT_S
                answer += data[:size - len(answer)]
                if len(answer) >= size:
                    return answer

        async def run():
            silent = lost = 0
            for tries in range(1, REFUSALS_MAX + 1):
                answer = await once(silent < 2)
                if answer is SILENT:
                    silent += 1
                    continue
                if answer is LOST:
                    lost += 1
                    if lost >= LOSSES_MAX:
                        raise OSError(f"command {sub:#04x}: a piece of its answer lost {lost} times")
                    continue
                if answer is not None:
                    return answer
                await asyncio.sleep(0.01 + random.random() * 0.04)
            raise OSError(f"command {sub:#04x} refused {REFUSALS_MAX} times: another program keeps the pad busy")
        return self.loop.run_until_complete(run())

    async def write(self, frame, response):
        """A frame to REQUEST (BlueZ: see busy_retry())."""
        if DEBUG:
            print(f"ble TX {frame.hex()}", file=sys.stderr, flush=True)
        return await busy_retry(lambda: self.client.write_gatt_char(REQUEST, frame, response=response))

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


async def busy_retry(op):
    """op() again while BlueZ says "In Progress": it keeps one write pending
    per characteristic, and one connect per device, whichever program asked."""
    for tries in range(1, BUSY_MAX + 1):
        try:
            return await op()
        except Exception as error:
            if "InProgress" not in str(getattr(error, "dbus_error", "")) + str(error) or tries == BUSY_MAX:
                raise
            await asyncio.sleep(0.005 + random.random() * 0.045)


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
        sys.exit("padtool: Bluetooth needs bleak (pip install bleak, or nix develop)")

    async def look():
        found = {d.address: d for d in await connected()}
        seen = await BleakScanner.discover(timeout=seconds, return_adv=True)
        for d, adv in seen.values():
            if SERVICE in [u.lower() for u in adv.service_uuids] or named(d, adv):
                found.setdefault(d.address, d)
        return list(found.values())
    return asyncio.new_event_loop().run_until_complete(look())


NINTENDO = 0x0553


def switch2_wake(address, data):
    """A Switch 2 controller's wake advert, from its address (as BlueZ gives
    it, most significant byte first) and its Nintendo manufacturer data:
    (address and product ID as the switch2_wake setting stores them, the
    console's address least significant byte first), or None.  The data:
    01 00 03 7e 05 <product, 2> 00 01 81 <console, 6> ..."""
    if len(data) < 16 or data[9] != 0x81 or data[3:5] != b"\x7e\x05":
        return None
    mine = bytes(reversed(bytes.fromhex(address.replace(":", ""))))
    return mine + data[5:7], bytes(data[10:16])


def listen_switch2_wake(seconds=60):
    """The first Switch 2 wake advert heard: switch2_wake()'s pair, or None."""
    try:
        from bleak import BleakScanner
    except ImportError:
        sys.exit("padtool: Bluetooth needs bleak (pip install bleak, or nix develop)")

    async def listen():
        heard = asyncio.get_running_loop().create_future()

        def seen(d, adv):
            data = adv.manufacturer_data.get(NINTENDO)
            found = data and switch2_wake(d.address, data)
            if found and not heard.done():
                heard.set_result(found)
        async with BleakScanner(seen):
            try:
                return await asyncio.wait_for(heard, seconds)
            except asyncio.TimeoutError:
                return None
    return asyncio.new_event_loop().run_until_complete(listen())


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


SILENT, LOST = object(), object()
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
