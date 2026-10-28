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
    product_name, location (USB port; None if unknown), feature_len and
    input_len (its feature and input report's bytes, without the ID) and
    descriptor (Linux) or usage_page and out_len (Windows)."""
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


def feature_bytes(descriptor, item=0xb0):
    """The feature reports' bytes in a report descriptor, short items only:
    report size times count for each feature item (item 0x80: input)."""
    size = count = bits = i = 0
    while i < len(descriptor):
        prefix = descriptor[i]
        n = (0, 1, 2, 4)[prefix & 3]
        value = int.from_bytes(descriptor[i + 1:i + 1 + n], "little")
        tag = prefix & 0xfc
        if tag == 0x74:
            size = value
        elif tag == 0x94:
            count = value
        elif tag == item:
            bits += size * count
        i += 1 + n
    return bits // 8


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
        hid["feature_len"] = feature_bytes(hid["descriptor"])
        hid["input_len"] = feature_bytes(hid["descriptor"], 0x80)
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
                              "usage_page": caps.UsagePage, "out_len": caps.OutputReportByteLength,
                              "feature_len": max(caps.FeatureReportByteLength - 1, 0),
                              "input_len": max(caps.InputReportByteLength - 1, 0)})
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


class Features:
    """An open HID interface's feature reports, without report IDs: set()
    sends the report, get() reads one of size bytes."""

    def __init__(self, path, size):
        self.path, self.size = path, size
        if sys.platform == "win32":
            from ctypes import wintypes as w
            self.k, self.handle = _windows_open(path, 0xc0000000)
            self.hid = ctypes.WinDLL("hid")
            for name in ("HidD_SetFeature", "HidD_GetFeature"):
                getattr(self.hid, name).argtypes = [w.HANDLE, ctypes.c_void_p, w.ULONG]
        else:
            self.fd = os.open(path, os.O_RDWR)

    def set(self, data, full=True):
        """full=False (Linux): only data's bytes, for a device that goes by
        the request's length."""
        data = bytes(data) if not full and sys.platform == "linux" else bytes(data).ljust(self.size, b"\0")
        buf = ctypes.create_string_buffer(b"\0" + data, len(data) + 1)
        if sys.platform == "win32":
            if not self.hid.HidD_SetFeature(self.handle, buf, self.size + 1):
                raise ctypes.WinError(ctypes.get_last_error())
        else:
            import fcntl
            fcntl.ioctl(self.fd, 0xc0004806 | len(buf) << 16, buf)   # HIDIOCSFEATURE

    def read(self, timeout):
        """One input report (Linux), or None after timeout seconds."""
        import select
        if not select.select([self.fd], [], [], max(timeout, 0))[0]:
            return None
        return os.read(self.fd, 4096)

    def drain(self):
        """Drop the input reports waiting (Linux): hidraw keeps at most 64
        a reader, and then drops new ones."""
        while self.read(0) is not None:
            pass

    def get(self):
        buf = ctypes.create_string_buffer(self.size + 1)
        if sys.platform == "win32":
            if not self.hid.HidD_GetFeature(self.handle, buf, self.size + 1):
                raise ctypes.WinError(ctypes.get_last_error())
        else:
            import fcntl
            fcntl.ioctl(self.fd, 0xc0004807 | (self.size + 1) << 16, buf)   # HIDIOCGFEATURE
        return buf.raw[1:]

    def close(self):
        if sys.platform == "win32":
            self.k.CloseHandle(self.handle)
        else:
            os.close(self.fd)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
