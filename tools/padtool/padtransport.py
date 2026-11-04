"""The firmware's vendor commands, independent of pad protocol and GUI.

Our firmware's tool device is a HID interface (usage page 0xffc2): each
command is a feature report.  Where the device has an input report (Linux and
Windows), the answer comes on input reports addressed to this process, so
several programs can use one device at once; else it is read back in more
feature reports (usb.c, the platform's tool device).  The Jieli ROM loaders (UBOOT1.00) and stock Jieli
firmware are USB mass storage: Linux uses SG_IO; Windows uses the built-in
storage driver and SCSI pass-through.  Only INQUIRY (disks) and IDENTIFY
(tool devices) are sent during discovery.
"""
import ctypes
import glob
import itertools
import os
from pathlib import Path
import re
import struct
import sys
import time

import padble
import padhid

if sys.platform == "linux":
    import fcntl


TOOL_USAGE_PAGE = 0xffc2
TOOL_REPORT = 512                           # the Jieli chips'; a device's own is in its descriptor
TOOL_HEAD = 20
TOOL_HEAD_IN = 24                           # with the client
TOOL_IN_HEAD = 8
TOOL_BUSY, TOOL_MORE, TOOL_LAST, TOOL_REFUSED = 1, 2, 3, 4
TOOL_OUT, TOOL_INPUT = 1, 2
_tools = {}                                 # path: (product name, report size, input report size), from the last scan
_open = {}                                  # path: its Features, kept: an open costs ~50 ms
_tags = itertools.count(1)
_client = int.from_bytes(os.urandom(4), "little")   # this process, in the answers it takes


def tool_paths():
    """Our firmware's tool devices (HID)."""
    _tools.clear()
    for d in padhid.devices():
        if d.get("usage_page") == TOOL_USAGE_PAGE or d.get("descriptor", b"")[:3] == b"\x06\xc2\xff":
            _tools[d["path"]] = (d["product_name"], d.get("feature_len") or TOOL_REPORT, d.get("input_len") or 0)
    return sorted(_tools)


def _report(path):
    if path not in _tools:
        tool_paths()
    return _tools.get(path, ("", TOOL_REPORT))[1]


def _input(path):
    """Answers on input reports: the device has one, and this system's HID
    access reads it (hidraw, or Windows' overlapped ReadFile)."""
    if path not in _tools:
        tool_paths()
    return sys.platform in ("linux", "win32") and _tools.get(path, ("", 0, 0))[2] >= 64


def tool_name(path):
    """A tool device's product string: the board."""
    if path not in _tools:
        tool_paths()
    return _tools.get(path, ("",))[0]


def data_max(path):
    """The most a command may carry to the device: its report less the header."""
    if padble.is_ble(path):
        return padble.link(path).chunk
    return _report(path) - (TOOL_HEAD_IN if _input(path) else TOOL_HEAD)


def is_tool(path):
    return "hidraw" in path or "hid#" in path.lower()


def device_paths():
    if sys.platform == "win32":
        return tool_paths() + [rf"\\.\PhysicalDrive{i}" for i in range(64)]
    if sys.platform == "linux":
        return tool_paths() + sorted(glob.glob(padhid.dev_path("sg*")))
    raise OSError("Device access supports Linux and Windows")


def usb_location(path):
    """Linux USB port chain, stable across device renumbering; None if unknown."""
    if sys.platform != "linux" or padble.is_ble(path):
        return None
    kind = "hidraw" if is_tool(path) else "scsi_generic"
    topology = Path("/sys/class", kind, Path(path).name, "device").resolve()
    ports = [part for part in topology.parts if re.fullmatch(r"[0-9]+-[0-9]+(?:\.[0-9]+)*", part)]
    return ports[-1] if ports else None


class SgIoHdr(ctypes.Structure):
    _fields_ = [
        ("interface_id", ctypes.c_int), ("dxfer_direction", ctypes.c_int),
        ("cmd_len", ctypes.c_ubyte), ("mx_sb_len", ctypes.c_ubyte),
        ("iovec_count", ctypes.c_short), ("dxfer_len", ctypes.c_uint),
        ("dxferp", ctypes.c_void_p), ("cmdp", ctypes.c_void_p), ("sbp", ctypes.c_void_p),
        ("timeout", ctypes.c_uint), ("flags", ctypes.c_uint), ("pack_id", ctypes.c_int),
        ("usr_ptr", ctypes.c_void_p), ("status", ctypes.c_ubyte), ("masked_status", ctypes.c_ubyte),
        ("msg_status", ctypes.c_ubyte), ("sb_len_wr", ctypes.c_ubyte),
        ("host_status", ctypes.c_ushort), ("driver_status", ctypes.c_ushort),
        ("resid", ctypes.c_int), ("duration", ctypes.c_uint), ("info", ctypes.c_uint),
    ]


def _linux_scsi(path, cdb, length, timeout_ms=3000, out=None, raw_length=False):
    """Run one command: data in (length bytes, returned), or out (bytes sent)."""
    cdb = bytes(cdb).ljust(16, b"\0")
    if out is not None:
        length = len(out)
    buf = ctypes.create_string_buffer(bytes(out) if out else b"", max(length, 1))
    sense = ctypes.create_string_buffer(32)
    cmd = ctypes.create_string_buffer(cdb, 16)
    direction = -1 if not length else -2 if out is not None else -3
    h = SgIoHdr(interface_id=ord("S"), dxfer_direction=direction, cmd_len=16,
                mx_sb_len=32, dxfer_len=length, dxferp=ctypes.addressof(buf),
                cmdp=ctypes.addressof(cmd), sbp=ctypes.addressof(sense), timeout=timeout_ms)
    fd = os.open(path, os.O_RDWR)
    try:
        fcntl.ioctl(fd, 0x2285, h)
    finally:
        os.close(fd)
    if h.status or h.host_status or h.driver_status & ~0x8:
        raise OSError(f"{path}: SCSI status {h.status} host {h.host_status} driver {h.driver_status}")
    return buf.raw[:length if raw_length else length - h.resid] if length and out is None else b""



class ScsiPassThrough(ctypes.Structure):
    _fields_ = [("Length", ctypes.c_ushort), ("ScsiStatus", ctypes.c_ubyte),
                ("PathId", ctypes.c_ubyte), ("TargetId", ctypes.c_ubyte),
                ("Lun", ctypes.c_ubyte), ("CdbLength", ctypes.c_ubyte),
                ("SenseInfoLength", ctypes.c_ubyte), ("DataIn", ctypes.c_ubyte),
                ("DataTransferLength", ctypes.c_uint32), ("TimeOutValue", ctypes.c_uint32),
                ("DataBufferOffset", ctypes.c_size_t), ("SenseInfoOffset", ctypes.c_uint32),
                ("Cdb", ctypes.c_ubyte * 16)]


def _windows_scsi(path, cdb, length, timeout_ms=3000, out=None, raw_length=False):
    # Buffered SPT keeps alignment the storage driver's responsibility.
    from ctypes import wintypes as w
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p,
                             w.DWORD, w.DWORD, w.HANDLE]
    k.CreateFileW.restype = w.HANDLE
    k.DeviceIoControl.argtypes = [w.HANDLE, w.DWORD, ctypes.c_void_p, w.DWORD,
                                 ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD), ctypes.c_void_p]
    k.DeviceIoControl.restype = w.BOOL
    k.CloseHandle.argtypes = [w.HANDLE]
    handle = k.CreateFileW(path, 0xc0000000, 3, None, 3, 0, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        length = len(out) if out is not None else length
        offset = ctypes.sizeof(ScsiPassThrough) + 32
        packet = ctypes.create_string_buffer(offset + max(length, 1))
        h = ScsiPassThrough.from_buffer(packet)
        h.Length = ctypes.sizeof(h)
        h.CdbLength = len(cdb)
        h.SenseInfoLength = 32
        h.SenseInfoOffset = ctypes.sizeof(h)
        h.DataIn = 2 if not length else 0 if out is not None else 1
        h.DataTransferLength = length
        h.TimeOutValue = max(1, (timeout_ms + 999) // 1000)
        h.DataBufferOffset = offset
        h.Cdb[:len(cdb)] = cdb
        if out:
            ctypes.memmove(ctypes.addressof(packet) + offset, bytes(out), length)
        returned = w.DWORD()
        if not k.DeviceIoControl(handle, 0x4d004, packet, len(packet), packet,
                                 len(packet), ctypes.byref(returned), None):
            raise ctypes.WinError(ctypes.get_last_error())
        if h.ScsiStatus:
            raise OSError(f"{path}: SCSI status {h.ScsiStatus}")
        return packet.raw[offset:offset + (length if raw_length else h.DataTransferLength)] if out is None else b""
    finally:
        k.CloseHandle(handle)


def _tool(path, cdb, length, timeout_ms, out):
    """A command on the tool device: its report, then the answer's pieces.
    A reboot's answer may never come: the device has gone."""
    cdb = bytes(cdb).ljust(16, b"\0")
    if cdb[0] == 0x12:           # the firmware would take it for a loader opcode
        raise OSError(f"{path}: a tool device has no INQUIRY")
    data = bytes(out) if out is not None else b""
    if len(data) > data_max(path):
        raise ValueError(f"tool device: at most {data_max(path)} bytes to the device")
    tag = next(_tags) & 0xff
    rebooting = cdb[0] != 0xf5 or cdb[1] in (0x10, 0x11, 0x40)
    answer = b""
    f = _open.get(path)
    if f is None:
        f = _open[path] = padhid.Features(path, _report(path))
    if _input(path):
        return _tool_input(path, f, cdb, length, timeout_ms, out, data, rebooting)
    try:
        f.set(cdb + struct.pack("<HBB", len(data) if out is not None else length, out is not None, tag) + data)
        end = time.monotonic() + timeout_ms / 1000
        while True:
            try:
                r = f.get()
            except OSError:
                if rebooting:
                    _drop(path)
                    return b""
                raise
            n = r[2] | r[3] << 8
            if r[0] == tag and r[1] in (TOOL_MORE, TOOL_LAST):
                answer += r[4:4 + n]
                if r[1] == TOOL_LAST:
                    break
                continue
            if r[0] != tag or r[1] != TOOL_BUSY:
                raise OSError(f"{path}: the tool device lost the command")
            if time.monotonic() > end:
                raise OSError(f"{path}: no answer in {timeout_ms} ms")
            time.sleep(0.0005)
    except BaseException:
        _drop(path)              # gone or confused: the next command opens it anew
        raise
    if rebooting:
        _drop(path)
    return answer.ljust(length, b"\0") if out is None else b""


def _tool_input(path, f, cdb, length, timeout_ms, out, data, rebooting):
    """The command, its answer on input reports with this process's client
    and the command's tag; others' answers pass by.  Refused while another
    program's command runs: sent again."""
    want = 0 if out is not None else min(length, TOOL_REPORT)
    end = time.monotonic() + timeout_ms / 1000
    try:
        while True:
            tag = next(_tags) & 0xff
            f.drain()
            f.set(cdb + struct.pack("<HBBI", len(data) if out is not None else length,
                                    TOOL_INPUT | (out is not None), tag, _client) + data, full=False)
            answer = b""
            while True:
                r = f.read(end - time.monotonic())
                if r is None:
                    if rebooting:
                        _drop(path)
                        return b""
                    raise OSError(f"{path}: no answer in {timeout_ms} ms")
                if len(r) < TOOL_IN_HEAD:
                    continue
                client, rtag, state, offset = struct.unpack_from("<IBBH", r)
                if client != _client or rtag != tag:
                    continue
                if state == TOOL_REFUSED:
                    break
                if state not in (TOOL_MORE, TOOL_LAST) or offset != len(answer):
                    raise OSError(f"{path}: the tool device lost a piece of the answer")
                answer += r[TOOL_IN_HEAD:TOOL_IN_HEAD + max(min(len(r) - TOOL_IN_HEAD, want - offset), 0)]
                if state == TOOL_LAST:
                    if rebooting:
                        _drop(path)
                    return answer.ljust(length, b"\0") if out is None else b""
            if time.monotonic() > end:
                raise OSError(f"{path}: busy for {timeout_ms} ms (another program's commands)")
            time.sleep(0.001 + int.from_bytes(os.urandom(1), "little") / 100000)
    except OSError:
        if rebooting:
            _drop(path)
            return b""
        _drop(path)
        raise
    except BaseException:
        _drop(path)
        raise


def _drop(path):
    f = _open.pop(path, None)
    if f is not None:
        f.close()


def scsi(path, cdb, length, timeout_ms=3000, out=None, raw_length=False):
    """raw_length is for BR23 loader transfers only: its CSW reports full
    residue even after delivering data. Return the requested buffer, as the
    C flasher does; callers validate command echoes and flash readback.
    On a tool device (HID), the same command as a feature report."""
    if not 1 <= len(cdb) <= 16:
        raise ValueError("SCSI CDB must be 1–16 bytes")
    if padble.is_ble(path):
        return padble.scsi(path, cdb, length, timeout_ms, out)
    if is_tool(path):
        return _tool(path, cdb, length, timeout_ms, out)
    if sys.platform == "win32":
        return _windows_scsi(path, bytes(cdb).ljust(16, b"\0"), length, timeout_ms, out, raw_length)
    if sys.platform == "linux":
        return _linux_scsi(path, cdb, length, timeout_ms, out, raw_length)
    raise OSError("Device access supports Linux and Windows")
