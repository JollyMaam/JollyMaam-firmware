"""HID interfaces by their USB identity, and output reports to them.

Linux: hidraw nodes and sysfs.  Windows: SetupAPI and hid.dll; there each
top-level collection is its own device, with its usage page.  Nothing is
read from a device but its attributes and strings.
"""
import ctypes
import os
from pathlib import Path
import sys


def devices():
    """Each HID interface: path, vid, product, bcd, manufacturer,
    product_name, location (USB port; None if unknown) and descriptor (Linux)
    or usage_page and out_len (Windows)."""
    if sys.platform == "linux":
        return _linux_devices()
    if sys.platform == "win32":
        return _windows_devices()
    return []


def write(path, data):
    """One output report: the report ID byte (0: none), then the report."""
    if sys.platform == "win32":
        _windows_write(path, data)
        return
    fd = os.open(path, os.O_RDWR)
    try:
        if os.write(fd, bytes(data)) != len(data):
            raise OSError(f"{path}: short HID write")
    finally:
        os.close(fd)


def _read(path, binary=False):
    try:
        return path.read_bytes() if binary else path.read_text().strip()
    except OSError:
        return b"" if binary else ""


def _linux_devices():
    found = []
    for sys_node in sorted(Path("/sys/class/hidraw").glob("hidraw*")):
        name = sys_node.name
        # The node this process may use: the container's first.
        path = next((p for p in (f"/dev/bwrap/{name}", f"/dev/{name}")
                     if os.access(p, os.R_OK | os.W_OK)), None)
        if path is None:
            continue
        hid = {"path": path, "vid": 0, "product": 0, "bcd": 0, "manufacturer": "", "product_name": "",
               "location": None, "descriptor": _read(sys_node / "device/report_descriptor", True)}
        for line in _read(sys_node / "device/uevent").splitlines():
            if line.startswith("HID_ID="):
                _, vid, product = line[7:].split(":")
                hid["vid"], hid["product"] = int(vid, 16), int(product, 16)
        # .../5-2.1/5-2.1:1.2/0003:3537:100F.0042: the USB device is above
        # the interface.
        usb = (sys_node / "device").resolve().parent.parent
        if (usb / "idVendor").exists():
            hid["bcd"] = int(_read(usb / "bcdDevice") or "0", 16)
            hid["manufacturer"] = _read(usb / "manufacturer")
            hid["product_name"] = _read(usb / "product")
            hid["location"] = usb.name
        found.append(hid)
    return found


class _Guid(ctypes.Structure):
    _fields_ = [("d1", ctypes.c_uint32), ("d2", ctypes.c_uint16), ("d3", ctypes.c_uint16),
                ("d4", ctypes.c_ubyte * 8)]


class _InterfaceData(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_uint32), ("guid", _Guid), ("flags", ctypes.c_uint32),
                ("reserved", ctypes.c_void_p)]


class _Attributes(ctypes.Structure):
    _fields_ = [("Size", ctypes.c_uint32), ("VendorID", ctypes.c_ushort),
                ("ProductID", ctypes.c_ushort), ("VersionNumber", ctypes.c_ushort)]


class _Caps(ctypes.Structure):
    _fields_ = [("Usage", ctypes.c_ushort), ("UsagePage", ctypes.c_ushort),
                ("InputReportByteLength", ctypes.c_ushort), ("OutputReportByteLength", ctypes.c_ushort),
                ("FeatureReportByteLength", ctypes.c_ushort), ("Reserved", ctypes.c_ushort * 17),
                ("Counts", ctypes.c_ushort * 10)]


def _windows_open(path, access):
    from ctypes import wintypes as w
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
    k.CreateFileW.restype = w.HANDLE
    handle = k.CreateFileW(path, access, 3, None, 3, 0, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    return k, handle


def _windows_devices():
    from ctypes import wintypes as w
    setup = ctypes.WinDLL("setupapi", use_last_error=True)
    hid = ctypes.WinDLL("hid")
    setup.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(_Guid), w.LPCWSTR, w.HWND, w.DWORD]
    setup.SetupDiGetClassDevsW.restype = ctypes.c_void_p
    setup.SetupDiEnumDeviceInterfaces.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(_Guid),
                                                  w.DWORD, ctypes.POINTER(_InterfaceData)]
    setup.SetupDiGetDeviceInterfaceDetailW.argtypes = [ctypes.c_void_p, ctypes.POINTER(_InterfaceData),
                                                       ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD),
                                                       ctypes.c_void_p]
    setup.SetupDiDestroyDeviceInfoList.argtypes = [ctypes.c_void_p]
    hid.HidD_GetPreparsedData.argtypes = [w.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
    hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
    hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Caps)]
    for name in ("HidD_GetManufacturerString", "HidD_GetProductString"):
        getattr(hid, name).argtypes = [w.HANDLE, ctypes.c_void_p, w.ULONG]
    hid.HidD_GetAttributes.argtypes = [w.HANDLE, ctypes.POINTER(_Attributes)]
    guid = _Guid()
    hid.HidD_GetHidGuid(ctypes.byref(guid))
    info = setup.SetupDiGetClassDevsW(ctypes.byref(guid), None, None, 0x12)  # PRESENT | DEVICEINTERFACE
    found = []
    if info in (None, ctypes.c_void_p(-1).value):
        return found
    try:
        index = 0
        while True:
            data = _InterfaceData(cbSize=ctypes.sizeof(_InterfaceData))
            if not setup.SetupDiEnumDeviceInterfaces(info, None, ctypes.byref(guid), index, ctypes.byref(data)):
                break
            index += 1
            detail = ctypes.create_string_buffer(4 + 2 * 512)
            # cbSize: the fixed part, DWORD plus one WCHAR, as the platform packs it.
            ctypes.c_uint32.from_buffer(detail).value = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
            if not setup.SetupDiGetDeviceInterfaceDetailW(info, ctypes.byref(data), detail, len(detail), None, None):
                continue
            path = ctypes.wstring_at(ctypes.addressof(detail) + 4)
            try:
                k, handle = _windows_open(path, 0)   # attributes and strings need no access
            except OSError:
                continue
            try:
                attributes = _Attributes(Size=ctypes.sizeof(_Attributes))
                if not hid.HidD_GetAttributes(handle, ctypes.byref(attributes)):
                    continue
                caps = _Caps()
                parsed = ctypes.c_void_p()
                if hid.HidD_GetPreparsedData(handle, ctypes.byref(parsed)):
                    hid.HidP_GetCaps(parsed, ctypes.byref(caps))
                    hid.HidD_FreePreparsedData(parsed)
                strings = []
                for name in ("HidD_GetManufacturerString", "HidD_GetProductString"):
                    text = ctypes.create_unicode_buffer(128)
                    strings.append(text.value if getattr(hid, name)(handle, text, ctypes.sizeof(text)) else "")
                found.append({"path": path, "vid": attributes.VendorID, "product": attributes.ProductID,
                              "bcd": attributes.VersionNumber, "manufacturer": strings[0],
                              "product_name": strings[1], "location": None,
                              "usage_page": caps.UsagePage, "out_len": caps.OutputReportByteLength})
            finally:
                k.CloseHandle(handle)
    finally:
        setup.SetupDiDestroyDeviceInfoList(info)
    return found


def _windows_write(path, data):
    from ctypes import wintypes as w
    k, handle = _windows_open(path, 0xc0000000)
    try:
        k.WriteFile.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD), ctypes.c_void_p]
        done = w.DWORD()
        buf = ctypes.create_string_buffer(bytes(data), len(data))
        if not k.WriteFile(handle, buf, len(data), ctypes.byref(done), None) or done.value != len(data):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        k.CloseHandle(handle)
