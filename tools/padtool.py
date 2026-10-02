#!/usr/bin/env python3
"""padtool: talk to a Cyclone 2 running this firmware, or to its ROM loader.

    padtool find                  devices: the pad and/or its loader
    padtool identify | status     what the pad reports
    padtool log [-f]              the pad's log (-f: follow)
    padtool reboot | loader       restart; restart into the ROM loader
    padtool flash IMAGE [--backup]

The pad is a mass-storage device with no medium; commands are vendor SCSI
CDBs (architecture/usb.md).  Flashing goes through the ROM loader with
jl-uboot-tool.
"""
import argparse, ctypes, fcntl, glob, os, struct, subprocess, sys, tempfile, time

JL_DIR = os.environ.get("JLUBOOT_DIR", "/shared/github.com-kagaimiq/jl-uboot-tool")
JL_PYTHON = os.environ.get("JLUBOOT_PYTHON", sys.executable)
FLASH_SIZE = 0x100000
OP = 0xF5
STATES = ["boot", "idle", "connecting", "pairing", "connected", "stopping", "sleeping", "rebooting"]


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


def scsi(path, cdb, length, timeout_ms=3000):
    """Run one data-in command; returns the bytes read."""
    cdb = bytes(cdb).ljust(16, b"\0")
    buf = ctypes.create_string_buffer(max(length, 1))
    sense = ctypes.create_string_buffer(32)
    cmd = ctypes.create_string_buffer(cdb, 16)
    h = SgIoHdr(interface_id=ord("S"), dxfer_direction=-3 if length else -1, cmd_len=16,
                mx_sb_len=32, dxfer_len=length, dxferp=ctypes.addressof(buf),
                cmdp=ctypes.addressof(cmd), sbp=ctypes.addressof(sense), timeout=timeout_ms)
    fd = os.open(path, os.O_RDWR)
    try:
        fcntl.ioctl(fd, 0x2285, h)
    finally:
        os.close(fd)
    if h.status or h.host_status or h.driver_status & ~0x8:
        raise OSError(f"{path}: SCSI status {h.status} host {h.host_status} driver {h.driver_status}")
    return buf.raw[:length - h.resid] if length else b""


def inquiry(path):
    try:
        data = scsi(path, [0x12, 0, 0, 0, 36], 36, 1000)
    except OSError:
        return None
    return data[8:32].decode("ascii", "replace").strip()


def devices():
    found = []
    for path in sorted(glob.glob("/dev/sg*") + glob.glob("/dev/bwrap/sg*")):
        name = inquiry(path)
        if not name:
            continue
        if "UBOOT" in name:
            found.append(("loader", path, name))
        else:
            try:
                if scsi(path, [OP, 0x00], 64, 1000)[:4] == b"C2PD":
                    found.append(("pad", path, name))
            except OSError:
                pass
    return found


def find(kind):
    for k, path, _ in devices():
        if k == kind:
            return path
    return None


def need_pad():
    path = find("pad")
    if not path:
        sys.exit("padtool: no pad found (is it plugged in and running this firmware?)")
    return path


def wait_for(kind, seconds):
    end = time.time() + seconds
    while time.time() < end:
        path = find(kind)
        if path:
            return path
        time.sleep(0.5)
    return None


def cmd(path, sub, arg=0, length=0):
    return scsi(path, [OP, sub] + list(struct.pack("<I", arg)), length)


def identify(path):
    d = cmd(path, 0x00, length=64)
    proto, = struct.unpack_from("<H", d, 4)
    build = d[8:24].split(b"\0")[0].decode()
    return f"protocol {proto} build {build} flash-id {d[24:40].hex()}"


def status(path):
    d = cmd(path, 0x01, length=16)
    state, slot, usb, low, mv, up = struct.unpack_from("<BBBBHxxI", d)
    slot = "-" if slot == 0xFF else "ABXY^v<>"[slot] if slot < 8 else str(slot)
    return (f"state {STATES[state] if state < len(STATES) else state} slot {slot} "
            f"usb {usb} battery {mv} mV{' low' if low else ''} uptime {up / 1000:.1f} s")


def clock():
    t = time.time()
    return time.strftime("%H:%M:%S", time.localtime(t)) + f".{int(t * 1000) % 1000:03d}"


def log(path, follow):
    seq = 1
    while True:
        try:
            d = cmd(path, 0x02, seq, 512)
        except OSError:
            if not follow:
                raise
            print(f"-- pad gone {clock()}; waiting --", flush=True)
            path = wait_for("pad", 3600)
            if not path:
                return
            print(f"-- pad back {clock()} --", flush=True)
            continue
        first, n = struct.unpack_from("<II", d)
        text = d[8:8 + n].decode("utf-8", "replace")
        if first > seq and seq > 1:
            print(f"... {first - seq} lines lost")
        lines = text.splitlines()
        for line in lines:
            print(line, flush=True)
        seq = first + len(lines)
        if not lines:
            if not follow:
                return
            time.sleep(0.2)


def flash(image, backup):
    data = open(image, "rb").read()
    loader = find("loader")
    if not loader:
        pad = find("pad")
        if pad:
            print("asking the pad to restart into its loader")
            try:
                cmd(pad, 0x11)
            except OSError:
                pass
        else:
            print("no pad and no loader: hold Home+Start and plug the pad in")
        loader = wait_for("loader", 120)
        if not loader:
            sys.exit("padtool: no loader appeared")
    print(f"loader at {loader}")
    with tempfile.TemporaryDirectory() as tmp:
        readback = os.path.join(tmp, "readback.bin")
        args = [JL_PYTHON, os.path.join(JL_DIR, "jluboottool.py"), "--device", loader]
        if backup:
            os.makedirs(os.path.dirname(backup), exist_ok=True)
            args.append(f"read 0 {FLASH_SIZE:#x} {backup}")
        args += [f"write 0 {image}", f"read 0 {len(data):#x} {readback}", "reset"]
        subprocess.run(args, check=True)
        if open(readback, "rb").read() != data:
            sys.exit("padtool: readback differs; the pad is left in the loader")
    print("written and verified")
    pad = wait_for("pad", 30)
    print(f"pad back at {pad}: {identify(pad)}" if pad else "the pad did not come back within 30 s")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command", choices=["find", "identify", "status", "log", "reboot", "loader", "flash"])
    p.add_argument("image", nargs="?")
    p.add_argument("-f", "--follow", action="store_true")
    p.add_argument("--backup", help="save the whole flash here before writing")
    a = p.parse_args()
    if a.command == "find":
        for kind, path, name in devices():
            print(f"{kind:6} {path}  {name}")
    elif a.command == "identify":
        print(identify(need_pad()))
    elif a.command == "status":
        print(status(need_pad()))
    elif a.command == "log":
        log(need_pad(), a.follow)
    elif a.command in ("reboot", "loader"):
        cmd(need_pad(), 0x10 if a.command == "reboot" else 0x11)
    elif a.command == "flash":
        if not a.image:
            sys.exit("padtool flash IMAGE")
        flash(a.image, a.backup)


if __name__ == "__main__":
    main()
