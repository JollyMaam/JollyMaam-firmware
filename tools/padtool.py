#!/usr/bin/env python3
"""padtool: talk to a Cyclone 2 running this firmware.

    padtool find                  pads running this firmware
    padtool identify | status     what the pad reports
    padtool log [-f]              the pad's log (-f: follow)
    padtool stats [-w SECONDS] [--reset]   loop timing and counters (-w: every N s)
    padtool reboot | loader       restart; restart into the ROM loader (for
                                  jlnewfw flash)
    padtool connect [SLOT]        connect a slot (default: the last), as Home does
    padtool pair SLOT             forget the slot's host and make it pairable
                                  (same address)
    padtool profile [ZOOM [SHIFT]] [-w SECONDS] [--hz N] [--top N] [--elf FILE]
                                  sample where the CPU runs for SECONDS (10) at
                                  N per second (2000); per function, from the ELF's
                                  DWARF (needs pyelftools); ZOOM: only flash from
                                  that hex address, in 1 << SHIFT (2) byte buckets
    padtool peek ADDR [WORDS]     32-bit words from a (hex) address: registers
    padtool trace [-w SECONDS] [--top N]   record the timing markers (0.5 s),
                                  print the last N and the pull round trips
    padtool relink                connected: drop the link and connect again
                                  (new link settings apply; wakes nothing)

    padtool fields [--dev]               every setting: name, values, default, help
                                         (--dev: the developer settings too)
    padtool get [TARGET [NAME]]          values; * marks a set one (else default)
    padtool set TARGET NAME VALUE [NAME VALUE ...]   change and commit
    padtool reset TARGET [NAME ...]      back to defaults: the fields, or all but the pairing
    padtool backup-settings FILE | restore-settings FILE [--calibration]
    padtool factory-reset                every slot and the palette (bonds too)
    padtool ephemeral                    from now until the next boot nothing is saved
    padtool keep                         reboot, then send what ephemeral mode tried

TARGET: a slot (A B X Y up down left right), palette or calibration.  VALUE:
a name for a choice, a number, ENTRY:LEVEL for a colour (palette entry 0-15,
brightness 0-15), #rrggbb for a palette entry, aa:bb:cc:dd:ee:ff for an
address, hex for other bytes; "default" puts a field back to its default.

The pad is a mass-storage device with no medium; commands are vendor SCSI
CDBs (architecture/usb.md).  Flashing is tools/jlnewfw.c's (make flash).
"""
import argparse, collections, ctypes, fcntl, glob, json, os, struct, sys, time

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


def scsi(path, cdb, length, timeout_ms=3000, out=None):
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


def inquiry(path):
    try:
        data = scsi(path, [0x12, 0, 0, 0, 36], 36, 1000)
    except OSError:
        return None
    return data[8:32].decode("ascii", "replace").strip()


def pads():
    """SCSI devices that answer IDENTIFY as this firmware."""
    found = []
    for path in sorted(glob.glob("/dev/sg*") + glob.glob("/dev/bwrap/sg*")):
        name = inquiry(path)
        if name and "UBOOT" not in name:
            try:
                if scsi(path, [OP, 0x00], 64, 1000)[:4] == b"C2PD":
                    found.append((path, name))
            except OSError:
                pass
    return found


def find_pad():
    return pads()[0][0] if pads() else None


def need_pad():
    path = find_pad()
    if not path:
        sys.exit("padtool: no pad found (is it plugged in and running this firmware?)")
    return path


def wait_for_pad(seconds, every=0.5):
    end = time.time() + seconds
    while time.time() < end:
        path = find_pad()
        if path:
            return path
        time.sleep(every)
    return None


def cmd(path, sub, arg=0, length=0, out=None):
    return scsi(path, [OP, sub] + list(struct.pack("<I", arg)), length, out=out)


def identify(path):
    d = cmd(path, 0x00, length=64)
    proto, = struct.unpack_from("<H", d, 4)
    build = d[8:24].split(b"\0")[0].decode()
    return f"protocol {proto} build {build} flash-id {d[24:40].hex()}"


def status(path):
    d = cmd(path, 0x01, length=16)
    state, slot, usb, low, mv, up, eph, staged = struct.unpack_from("<BBBBHxxIBB", d)
    slot = "-" if slot == 0xFF else "ABXY^v<>"[slot] if slot < 8 else str(slot)
    return (f"state {STATES[state] if state < len(STATES) else state} slot {slot} "
            f"usb {usb} battery {mv} mV{' low' if low else ''} uptime {up / 1000:.1f} s"
            f"{' EPHEMERAL' if eph else ''}{f' ({staged} uncommitted)' if staged else ''}")


STEPS = ["input", "bt", "usb", "power", "timeouts", "report", "outputs", "settings"]
HIST = "<QII16H"


def read_stats(path, reset=False):
    d = cmd(path, 0x03, 1 if reset else 0, 512)
    version, size = struct.unpack_from("<HH", d)
    if version != 5:
        sys.exit(f"padtool: stats version {version} not known to this padtool")
    d = d[4:4 + size]
    s = {}
    s["since"], s["now"], s["passes"] = struct.unpack_from("<III", d, 0)
    o = 16
    for name in ("pass", "interval", "age"):
        v = struct.unpack_from(HIST, d, o)
        s[name] = {"sum": v[0], "count": v[1], "max": v[2], "buckets": v[3:]}
        o += struct.calcsize(HIST)
    s["steps"] = dict(zip(STEPS, struct.unpack_from("<8I", d, o)))
    s["step_sums"] = dict(zip(STEPS, struct.unpack_from("<8I", d, o + 32)))
    o += 64
    (s["reports"], s["imu"], s["sent"], s["replaced"], s["acked"], s["loop_runs"], s["loop_us"],
     s["buttons"], s["frames"], s["resyncs"]) = struct.unpack_from("<10I", d, o)
    o += 40
    for name in ("ack_wait", "slot_ack", "ack_gap", "want_send"):
        v = struct.unpack_from(HIST, d, o)
        s[name] = {"sum": v[0], "count": v[1], "max": v[2], "buckets": v[3:]}
        o += struct.calcsize(HIST)
    s["link"] = dict(zip(("transport", "phy_tx", "phy_rx", "mode", "interval", "latency", "timeout",
                          "tx_octets", "tx_time", "rx_octets", "rx_time", "acl_buffers", "poll_us"),
                         struct.unpack_from("<4B8HI", d, o)))
    return s


def show_link(k):
    if k["transport"] == 2:
        print(f"link: LE interval {k['interval'] * 1.25:g} ms latency {k['latency']} "
              f"timeout {k['timeout'] * 10} ms, phy tx {k['phy_tx']} rx {k['phy_rx']}, "
              f"tx {k['tx_octets']} octets/{k['tx_time']} us rx {k['rx_octets']}/{k['rx_time']} us, "
              f"{k['acl_buffers']} ACL buffers")
    elif k["transport"] == 1:
        mode = f"sniff {k['interval'] * 0.625:g} ms" if k["mode"] == 2 else f"mode {k['mode']}"
        print(f"link: Classic {mode}, accepted QoS poll {k['poll_us']} us, {k['acl_buffers']} ACL buffers")
    else:
        print("link: none")


def show_stats(s):
    secs = max((s["now"] - s["since"]) / 1000, 0.001)
    busy = s["pass"]["sum"] / 1e6 / secs * 100
    print(f"over {secs:.1f} s: {s['passes']} loop passes ({s['passes'] / secs:.0f}/s), busy {busy:.2f} %")
    print(f"{'':16} {'count':>7} {'avg us':>8} {'max us':>8}  distribution (us)")
    for name, label in (("pass", "loop pass"), ("interval", "report interval"),
                        ("age", "report age"), ("ack_wait", "sent to ack"),
                        ("slot_ack", "slot to ack"), ("ack_gap", "between acks"),
                        ("want_send", "want to send")):
        h = s[name]
        avg = h["sum"] / h["count"] if h["count"] else 0
        dist = "  ".join(f"<{32 << i}:{n}" if i < 15 else f">={32 << 14}:{n}"
                         for i, n in enumerate(h["buckets"]) if n)
        print(f"{label:16} {h['count']:7} {avg:8.0f} {h['max']:8}  {dist}")
    n = max(s["passes"], 1)
    print(f"{'step':16} {'avg us':>8} {'max us':>8} {'share':>6}")
    total = max(sum(s["step_sums"].values()), 1)
    for k in STEPS:
        print(f"{k:16} {s['step_sums'][k] / n:8.0f} {s['steps'][k]:8} {100 * s['step_sums'][k] / total:5.1f}%")
    per = s["imu"] / s["reports"] if s["reports"] else 0
    iv = s["interval"]
    rate = f"{1e6 * iv['count'] / iv['sum']:.1f}/s while sending" if iv["count"] and iv["sum"] else "none sent"
    print(f"reports {s['reports']} ({rate}), {per:.1f} imu samples each")
    print(f"bluetooth: sent {s['sent']}, replaced before sending {s['replaced']}, "
          f"packets on air {s['acked']} ({s['acked'] / secs:.1f}/s)")
    print(f"bluetooth thread: {s['loop_runs'] / secs:.0f} passes/s, "
          f"{s['loop_us'] / 1e6 / secs * 100:.1f} % of the time awake")
    show_link(s["link"])
    print(f"input: button changes {s['buttons']}, stick frames {s['frames']} "
          f"({s['frames'] / secs:.0f}/s), stick resyncs {s['resyncs']}")


PROF_HEADER = "<HHBBBBIIIIIIIII8I"


def read_profile(path):
    d = cmd(path, 0x14, 0, 512)                 # byte 2: 0 read; bytes 3-6: offset
    first = struct.unpack_from(PROF_HEADER, d)
    total = struct.calcsize(PROF_HEADER) + 2 * (first[7] + first[9] + first[11])
    while len(d) < total:
        d += cmd(path, 0x14, len(d) << 8, 512)
    v = struct.unpack_from(PROF_HEADER, d)
    h = dict(zip(("version", "hz", "flash_shift", "ram_shift", "rom_shift", "running",
                  "flash_begin", "flash_buckets", "ram_begin", "ram_buckets", "rom_begin",
                  "rom_buckets", "samples", "other", "saturated"), v[:15]))
    h["other_pc"] = v[15:]
    o = struct.calcsize(PROF_HEADER)
    for name in ("flash", "ram", "rom"):
        n = h[name + "_buckets"]
        h[name] = struct.unpack_from(f"<{n}H", d, o)
        o += 2 * n
    return h


def functions(elf):
    """(low, high, name, file) of every function the ELF's DWARF describes."""
    try:
        from elftools.elf.elffile import ELFFile
    except ImportError:
        sys.exit("padtool profile: needs pyelftools (pip install pyelftools)")
    out = []
    with open(elf, "rb") as f:
        for cu in ELFFile(f).get_dwarf_info().iter_CUs():
            top = cu.get_top_DIE().attributes
            unit = top["DW_AT_name"].value.decode() if "DW_AT_name" in top else "?"
            for die in cu.iter_DIEs():
                at = die.attributes
                if die.tag != "DW_TAG_subprogram" or "DW_AT_low_pc" not in at or "DW_AT_high_pc" not in at:
                    continue
                lo, hi = at["DW_AT_low_pc"].value, at["DW_AT_high_pc"]
                hi = hi.value if hi.form == "DW_FORM_addr" else lo + hi.value
                name = at["DW_AT_name"].value.decode() if "DW_AT_name" in at else "?"
                out.append((lo, hi, name, unit))
    out.sort()
    return out


def line_table(elf):
    """Sorted (address, "file:line") rows of the ELF's DWARF line programs."""
    from elftools.elf.elffile import ELFFile
    rows = []
    with open(elf, "rb") as f:
        dwarf = ELFFile(f).get_dwarf_info()
        for cu in dwarf.iter_CUs():
            lp = dwarf.line_program_for_CU(cu)
            if lp is None:
                continue
            names = lp["file_entry"]
            base = 1 if lp.header.version < 5 else 0
            for e in lp.get_entries():
                st = e.state
                if st is None or st.end_sequence:
                    continue
                i = st.file - base
                name = names[i].name.decode() if 0 <= i < len(names) else "?"
                rows.append((st.address, f"{name.rsplit('/', 1)[-1]}:{st.line}"))
    rows.sort()
    return rows


def unit_group(unit):
    """Which part of the firmware a compilation unit belongs to."""
    u = unit.replace("\\", "/")
    for key, name in (("/btctrler/", "controller"), ("port/br23/baseband", "controller"),
                      ("cyclone2-btstack", "btstack"), ("/usb/", "usb (sdk)"),
                      ("crypto_toolbox", "crypto (sdk)"), ("/FreeRTOS", "os"), ("/lib/system/", "os"),
                      ("/lib/driver/", "drivers (sdk)"), ("fw-AC63_BT_SDK", "sdk app code")):
        if key in u:
            return name
    return "app " + u.rsplit("/", 1)[-1]


def profile(path, seconds, hz, top, elf, zoom=None, by=None):
    import bisect
    zoom_base, zoom_shift = zoom or (0, 0)
    scsi(path, [OP, 0x14, 1, hz & 0xff, hz >> 8] + list(struct.pack("<I", zoom_base)) + [zoom_shift], 0)
    time.sleep(seconds)
    cmd(path, 0x14, 2)
    h = read_profile(path)
    funcs = functions(elf)
    lows = [f[0] for f in funcs]
    per = collections.Counter()
    key = (lambda f: f[2]) if not by else (lambda f: f[3]) if by == "file" else (lambda f: unit_group(f[3]))
    if by == "line":
        rows = line_table(elf)
        raddr = [r[0] for r in rows]
    for name in ("flash", "ram", "rom"):
        base, shift = h[name + "_begin"], h[name + "_shift"]
        for i, n in enumerate(h[name]):
            if not n:
                continue
            a = base + (i << shift)
            if name == "rom":
                per[f"rom+{a - base:#06x}"] += n
                continue
            if by == "line":
                # the sample address is where the interrupt returns: the line
                # of the instruction before it
                k = bisect.bisect_right(raddr, a + (1 << shift) // 2 - 1) - 1
                per[rows[k][1] if k >= 0 else f"{a:#x}"] += n
                continue
            # a bucket's samples go to the functions it overlaps, by overlap
            end = a + (1 << shift)
            parts = []
            k = bisect.bisect_right(lows, a) - 1
            k = max(k, 0)
            while k < len(funcs) and funcs[k][0] < end:
                lo, hi = funcs[k][0], funcs[k][1]
                o = min(hi, end) - max(lo, a)
                if o > 0:
                    parts.append((o, key(funcs[k])))
                k += 1
            if not parts:
                per[f"{a:#x}"] += n
                continue
            span = sum(o for o, _ in parts)
            for o, fn in parts:
                per[fn] += n * o / span
    if zoom:
        print(f"flash window {zoom_base:#x} + {4096 << zoom_shift:#x} bytes in {1 << zoom_shift}-byte buckets")
    total = max(h["samples"], 1)
    print(f"{h['samples']} samples over {seconds:g} s ({h['samples'] / seconds:.0f}/s), "
          f"{h['other']} outside the known code, {h['saturated']} lost to full buckets")
    for name, n in per.most_common(top):
        print(f"{100 * n / total:6.2f} %  {n:7.0f}  {name}")
    if h["other"]:
        print("outside: " + " ".join(f"{a:#x}" for a in h["other_pc"] if a))


TRACE_NAMES = {1: "want", 2: "bt wake", 3: "bt sleep", 4: "ctl wake", 5: "built", 6: "sent",
               7: "ack", 8: "ctl sleep", 9: "stick"}


def trace(path, seconds, last):
    cmd(path, 0x16, 1)                       # byte 2: 1 start
    time.sleep(seconds)
    cmd(path, 0x16, 2)
    d = b""
    while len(d) < 4 + 8 * 1024:
        d += cmd(path, 0x16, len(d) << 8, 512)
    count = struct.unpack_from("<I", d)[0]
    entries = [struct.unpack_from("<IHB", d, 4 + 8 * i) for i in range(1024)]
    if count > 1024:
        k = count % 1024
        entries = entries[k:] + entries[:k]
    else:
        entries = entries[:count]
    if not entries:
        print("no markers")
        return
    t0 = entries[0][0]
    rel = [((us - t0) & 0xffffffff, TRACE_NAMES.get(m, m), arg) for us, arg, m in entries]
    print(f"{count} markers, showing the last {min(last, len(rel))} of {len(rel)} (µs from the first kept)")
    prev = None
    for us, name, arg in rel[-last:]:
        print(f"{us:10} {'' if prev is None else f'+{us - prev:6}':>8}  {name}{f' {arg}' if arg else ''}")
        prev = us
    # each want: time to the next of each marker
    after = collections.defaultdict(list)
    for i, (us, name, _) in enumerate(rel):
        if name != "want":
            continue
        seen = set()
        for us2, name2, _ in rel[i + 1:]:
            if name2 == "want":
                break
            if name2 not in seen:
                seen.add(name2)
                after[name2].append(us2 - us)
    print("from want to the next:")
    for name in ("bt sleep", "ctl wake", "built", "sent", "ack"):
        v = sorted(after[name])
        if v:
            print(f"  {name:9} n {len(v):5}  median {v[len(v) // 2]:6} µs  p90 {v[len(v) * 9 // 10]:6}  max {v[-1]:6}")


def stats(path, every, reset):
    if not every:
        show_stats(read_stats(path, reset))
        return
    read_stats(path, True)
    while True:
        time.sleep(every)
        print(f"-- {clock()}")
        show_stats(read_stats(path, True))


def clock():
    t = time.time()
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)) + f".{int(t * 1000) % 1000:03d}"


def log(path, follow):
    seq = 1
    while True:
        try:
            d = cmd(path, 0x02, seq, 512)
        except OSError:
            if not follow:
                raise
            print(f"-- pad gone {clock()}; waiting --", flush=True)
            path = wait_for_pad(3600, 0.1)
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
            time.sleep(0.1)


# ---- settings (architecture/settings.md) ------------------------------------

SLOT_NAMES = ["A", "B", "X", "Y", "up", "down", "left", "right"]
TARGETS = SLOT_NAMES + ["palette", "calibration"]
KINDS = ["slot", "palette", "calibration"]
T_ENUM, T_NUMBER, T_COLOUR, T_RGB, T_BYTES = range(5)
RESULTS = ["ok", "unknown field", "bad length", "out of range", "too many changes at once"]
JOURNAL = os.path.expanduser("~/.cache/padtool/ephemeral.json")
PAIRING = ("address", "bond")   # kept by a whole-slot reset


def target_index(name):
    for i, t in enumerate(TARGETS):
        if t.lower() == name.lower():
            return i
    sys.exit(f"padtool: no target {name!r}; one of {' '.join(TARGETS)}")


def kind_of(t):
    return 0 if t < 8 else t - 7


def fields(path):
    """The field table, as the firmware describes it."""
    out, n = [], 0
    while True:
        d = cmd(path, 0x20, n, 512)
        if len(d) < 8 or not any(d):
            return out
        _, kind, fid, typ, size, flags, mx = struct.unpack_from("<BBBBBBH", d)
        o = 8

        def string():
            nonlocal o
            end = d.index(b"\0", o)
            v, o = d[o:end].decode(), end + 1
            return v

        name, help_ = string(), string()
        names = [string() for _ in range(mx + 1)] if typ == T_ENUM else []
        nd = 8 if kind == 0 else 1
        defs = [d[o + i * size:o + (i + 1) * size] for i in range(nd)] if size <= 4 else []
        out.append({"kind": kind, "id": fid, "type": typ, "size": size, "max": mx, "dev": flags & 1,
                    "name": name, "help": help_, "names": names, "defaults": defs})
        n += 1


def show_value(f, raw):
    t = f["type"]
    if t == T_ENUM:
        return f["names"][raw[0]] if raw[0] < len(f["names"]) else str(raw[0])
    if t == T_NUMBER:
        return str(int.from_bytes(raw, "little"))
    if t == T_COLOUR:
        return f"{raw[0]}:{raw[1]}"
    if t == T_RGB:
        return "#" + raw.hex()
    if f["name"] == "address":
        return "derived" if not any(raw) else ":".join(f"{b:02x}" for b in reversed(raw))
    return "none" if not any(raw) else raw.hex()


def parse_value(f, text):
    """Bytes for a field, or b"" for its default."""
    t = f["type"]
    if text == "default":
        return b""
    try:
        if t == T_ENUM:
            if text in f["names"]:
                return bytes([f["names"].index(text)])
            sys.exit(f"padtool: {f['name']}: one of {' '.join(f['names'])}")
        if t == T_NUMBER:
            return int(text, 0).to_bytes(f["size"], "little")
        if t == T_COLOUR:
            e, l = text.split(":")
            return bytes([int(e), int(l)])
        if t == T_RGB:
            v = bytes.fromhex(text.lstrip("#")) if "," not in text else bytes(int(x) for x in text.split(","))
        elif f["name"] == "address":
            v = bytes(reversed(bytes.fromhex(text.replace(":", ""))))
        else:
            v = bytes.fromhex(text)
    except (ValueError, OverflowError):
        sys.exit(f"padtool: {f['name']}: cannot read {text!r}")
    if len(v) != f["size"]:
        sys.exit(f"padtool: {f['name']}: {f['size']} bytes, not {len(v)}")
    return v


def field_named(table, t, name):
    for f in table:
        if f["kind"] == kind_of(t) and f["name"] == name:
            return f
    sys.exit(f"padtool: {TARGETS[t]} has no setting {name!r} (padtool fields)")


def read_target(path, table, t):
    """{name: (value bytes, set)} in table order."""
    d = cmd(path, 0x21, t, 512)
    count, o, by_id, out = d[2], 3, {}, {}
    for _ in range(count):
        fid, is_set, size = d[o], d[o + 1], d[o + 2]
        by_id[fid] = (d[o + 3:o + 3 + size], bool(is_set))
        o += 3 + size
    for f in table:
        if f["kind"] == kind_of(t) and f["id"] in by_id:
            out[f["name"]] = by_id[f["id"]]
    return out


def send(path, items, journal=True):
    """items: [(target, id, value bytes)], value b"" for a default; id 0 resets
    the target.  Written in chunks, then committed; nothing applies if any is
    refused."""
    chunk, chunks = b"", []
    for t, fid, v in items:
        item = bytes([t, fid, len(v)]) + v
        if len(chunk) + len(item) > 512:
            chunks.append(chunk)
            chunk = b""
        chunk += item
    if chunk:
        chunks.append(chunk)
    for c in chunks:
        cmd(path, 0x22, out=c)
        r = cmd(path, 0x26, length=256)
        bad = [i for i, code in enumerate(r[1:1 + r[0]]) if code]
        if r[0] == 0 or bad:
            cmd(path, 0x24)
            what = ", ".join(RESULTS[r[1 + i]] if r[1 + i] < len(RESULTS) else str(r[1 + i]) for i in bad)
            sys.exit(f"padtool: the pad refused the change ({what or 'nothing accepted'}); nothing changed")
    cmd(path, 0x23)
    if journal and ephemeral_now(path):
        os.makedirs(os.path.dirname(JOURNAL), exist_ok=True)
        try:
            j = json.load(open(JOURNAL))
        except (OSError, ValueError):
            j = []
        j.append([[t, fid, v.hex()] for t, fid, v in items])
        json.dump(j, open(JOURNAL, "w"))


def ephemeral_now(path):
    return bool(cmd(path, 0x01, length=16)[12])


DEV = False


def show_fields(path, dev=False):
    table = fields(path)
    shown_dev = False
    for f in sorted(table, key=lambda f: f["dev"]):
        if f["dev"] and not dev:
            continue
        if f["dev"] and not shown_dev:
            print("\ndeveloper settings: for measuring the link, not for normal use")
            shown_dev = True
        values = (" | ".join(f["names"]) if f["type"] == T_ENUM else
                  f"0-{f['max']}" if f["type"] == T_NUMBER else
                  "ENTRY:LEVEL" if f["type"] == T_COLOUR else
                  "#rrggbb" if f["type"] == T_RGB else f"{f['size']} bytes")
        defs = ""
        if f["defaults"] and f["type"] != T_BYTES:
            shown = [show_value(f, d) for d in f["defaults"]]
            defs = f"  default {shown[0]}" if len(set(shown)) == 1 else \
                   "  default " + " ".join(f"{SLOT_NAMES[i]}={v}" for i, v in enumerate(shown))
        print(f"{KINDS[f['kind']]:11} {f['name']:16} {values}{defs}\n{'':28} {f['help']}")


def get(path, target, name):
    table = fields(path)
    for t in [target_index(target)] if target else range(len(TARGETS)):
        values = read_target(path, table, t)
        for f in table:
            if f["kind"] != kind_of(t) or (name and f["name"] != name) or f["name"] not in values:
                continue
            if f["dev"] and not name and not DEV and not values[f["name"]][1]:
                continue                     # developer settings at default: --dev shows them
            raw, is_set = values[f["name"]]
            print(f"{TARGETS[t]:11} {f['name']:16} {'*' if is_set else ' '} {show_value(f, raw)}"
              + ("   (developer)" if f["dev"] else ""))


def set_values(path, target, pairs):
    if not pairs or len(pairs) % 2:
        sys.exit("padtool set TARGET NAME VALUE [NAME VALUE ...]")
    table, t = fields(path), target_index(target)
    items = []
    for name, text in zip(pairs[::2], pairs[1::2]):
        f = field_named(table, t, name)
        items.append((t, f["id"], parse_value(f, text)))
        if f["dev"]:
            print(f"note: {name} is a developer setting, for measuring the link")
    send(path, items)
    print("changed" + (" (ephemeral: until the next boot)" if ephemeral_now(path) else ""))


def reset(path, target, names):
    table, t = fields(path), target_index(target)
    send(path, [(t, field_named(table, t, n)["id"], b"") for n in names] if names else [(t, 0, b"")])


def backup(path, file):
    table = fields(path)
    ident = cmd(path, 0x00, length=64)
    out = {"format": 1, "build": ident[8:24].split(b"\0")[0].decode(), "flash_id": ident[24:40].hex(),
           "notes": {}, "targets": {}}
    for t, name in enumerate(TARGETS):
        values = read_target(path, table, t)
        out["targets"][name] = {n: {"value": show_value(field_named(table, t, n), raw), "set": s}
                                for n, (raw, s) in values.items()}
    json.dump(out, open(file, "w"), indent=1)
    print(f"saved to {file}")


def restore(path, file, calibration):
    table, data = fields(path), json.load(open(file))
    for name, values in data["targets"].items():
        t = target_index(name)
        if name == "calibration" and not calibration:
            continue
        items = [(t, 0, b"")]
        for f in table:           # table order: type before address and bond
            v = values.get(f["name"])
            if f["kind"] != kind_of(t) or not v:
                continue
            if v["set"]:
                items.append((t, f["id"], parse_value(f, v["value"])))
            elif f["name"] in PAIRING:
                items.append((t, f["id"], b""))   # a reset keeps these
        send(path, items)
    print("restored" + ("" if calibration else " (calibration left as it was)"))


def keep(path):
    try:
        batches = json.load(open(JOURNAL))
    except (OSError, ValueError):
        sys.exit("padtool: nothing tried in ephemeral mode")
    if ephemeral_now(path):
        print("rebooting out of ephemeral mode")
        try:
            cmd(path, 0x10)
        except OSError:
            pass
        time.sleep(2)
        path = wait_for_pad(30)
        if not path or ephemeral_now(path):
            sys.exit("padtool: the pad did not come back in normal mode")
    for b in batches:
        send(path, [(t, fid, bytes.fromhex(v)) for t, fid, v in b], journal=False)
    os.remove(JOURNAL)
    print(f"kept {len(batches)} changes")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command", choices=["find", "identify", "status", "log", "stats", "reboot", "loader", "relink", "connect", "pair", "profile", "peek", "trace",
                                       "fields", "get", "set", "reset", "backup-settings",
                                       "restore-settings", "factory-reset", "ephemeral", "keep"])
    p.add_argument("args", nargs="*")
    p.add_argument("--calibration", action="store_true", help="restore-settings: calibration too")
    p.add_argument("-f", "--follow", action="store_true")
    p.add_argument("-w", "--watch", type=float, metavar="SECONDS", help="stats: show and reset every N seconds")
    p.add_argument("--reset", action="store_true", help="stats: reset the counters after reading")
    p.add_argument("--hz", type=int, default=2000, help="profile: samples per second")
    p.add_argument("--dev", action="store_true",
                   help="fields, get: include the developer settings (for measuring the link)")
    p.add_argument("--top", type=int, default=40, help="profile: functions to show")
    p.add_argument("--by", choices=["file", "group", "line"],
                   help="profile: add up per source file, per part, or per source line (zoom first)")
    p.add_argument("--elf", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "app.elf"),
                   help="profile: the running build's ELF")
    a = p.parse_args()
    global DEV
    DEV = a.dev
    if a.command == "find":
        for path, name in pads():
            print(f"pad {path}  {name}")
    elif a.command == "identify":
        print(identify(need_pad()))
    elif a.command == "status":
        print(status(need_pad()))
    elif a.command == "log":
        log(need_pad(), a.follow)
    elif a.command == "stats":
        try:
            stats(need_pad(), a.watch, a.reset)
        except KeyboardInterrupt:
            pass
    elif a.command == "trace":
        trace(need_pad(), a.watch or 0.5, a.top)
    elif a.command == "peek":
        addr, words = int(a.args[0], 16), int(a.args[1]) if len(a.args) > 1 else 4
        d = cmd(need_pad(), 0x15, addr, 4 * words)
        for i in range(words):
            print(f"{addr + 4 * i:08x}: {struct.unpack_from('<I', d, 4 * i)[0]:08x}")
    elif a.command == "profile":
        zoom = (int(a.args[0], 16), int(a.args[1]) if len(a.args) > 1 else 1) if a.args else None
        profile(need_pad(), a.watch or 10, a.hz, a.top, a.elf, zoom, a.by)
    elif a.command == "pair":
        if not a.args:
            sys.exit("padtool pair SLOT")
        cmd(need_pad(), 0x17, "A B X Y up down left right".split().index(a.args[0]))
    elif a.command == "connect":
        names = "A B X Y up down left right".split()
        slot = names.index(a.args[0]) if a.args else 0xfe
        cmd(need_pad(), 0x13, slot)
    elif a.command == "relink":
        cmd(need_pad(), 0x12)
    elif a.command in ("reboot", "loader"):
        cmd(need_pad(), 0x10 if a.command == "reboot" else 0x11)
    elif a.command == "fields":
        show_fields(need_pad(), a.dev)
    elif a.command == "get":
        get(need_pad(), a.args[0] if a.args else None, a.args[1] if len(a.args) > 1 else None)
    elif a.command == "set":
        if not a.args:
            sys.exit("padtool set TARGET NAME VALUE [NAME VALUE ...]")
        set_values(need_pad(), a.args[0], a.args[1:])
    elif a.command == "reset":
        if not a.args:
            sys.exit("padtool reset TARGET [NAME ...]")
        reset(need_pad(), a.args[0], a.args[1:])
    elif a.command in ("backup-settings", "restore-settings"):
        if len(a.args) != 1:
            sys.exit(f"padtool {a.command} FILE")
        (backup if a.command == "backup-settings" else
         lambda p, f: restore(p, f, a.calibration))(need_pad(), a.args[0])
    elif a.command == "factory-reset":
        path = need_pad()
        table = fields(path)
        items = []
        for t in range(len(TARGETS) - 1):
            items.append((t, 0, b""))
            if t < 8:
                items += [(t, field_named(table, t, n)["id"], b"") for n in PAIRING]
        send(path, items)
        print("every slot and the palette back to defaults; calibration kept")
    elif a.command == "ephemeral":
        path = need_pad()
        cmd(path, 0x25)
        try:
            os.remove(JOURNAL)
        except OSError:
            pass
        print("ephemeral: nothing is saved until the next boot (Home winks)")
    elif a.command == "keep":
        keep(need_pad())


if __name__ == "__main__":
    main()
