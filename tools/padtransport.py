"""USB mass-storage vendor commands, independent of pad protocol and GUI.

Linux uses SG_IO; Windows uses the built-in storage driver and SCSI pass-through.
Only inquiry/identify probes are sent during discovery.
"""
import ctypes
import glob
import os
import sys

if sys.platform == "linux":
    import fcntl


def device_paths():
    if sys.platform == "win32":
        return [rf"\\.\PhysicalDrive{i}" for i in range(64)]
    if sys.platform == "linux":
        return sorted(set(glob.glob("/dev/sg*") + glob.glob("/dev/bwrap/sg*")))
    raise OSError("Device access supports Linux and Windows")


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


def _linux_scsi(path, cdb, length, timeout_ms=3000, out=None):
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
    return buf.raw[:length - h.resid] if length and out is None else b""



class ScsiPassThrough(ctypes.Structure):
    _fields_ = [("Length", ctypes.c_ushort), ("ScsiStatus", ctypes.c_ubyte),
                ("PathId", ctypes.c_ubyte), ("TargetId", ctypes.c_ubyte),
                ("Lun", ctypes.c_ubyte), ("CdbLength", ctypes.c_ubyte),
                ("SenseInfoLength", ctypes.c_ubyte), ("DataIn", ctypes.c_ubyte),
                ("DataTransferLength", ctypes.c_uint32), ("TimeOutValue", ctypes.c_uint32),
                ("DataBufferOffset", ctypes.c_size_t), ("SenseInfoOffset", ctypes.c_uint32),
                ("Cdb", ctypes.c_ubyte * 16)]


def _windows_scsi(path, cdb, length, timeout_ms=3000, out=None):
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
        return packet.raw[offset:offset + h.DataTransferLength] if out is None else b""
    finally:
        k.CloseHandle(handle)


def scsi(path, cdb, length, timeout_ms=3000, out=None):
    if not 1 <= len(cdb) <= 16:
        raise ValueError("SCSI CDB must be 1–16 bytes")
    if sys.platform == "win32":
        return _windows_scsi(path, bytes(cdb).ljust(16, b"\0"), length, timeout_ms, out)
    if sys.platform == "linux":
        return _linux_scsi(path, cdb, length, timeout_ms, out)
    raise OSError("Device access supports Linux and Windows")
