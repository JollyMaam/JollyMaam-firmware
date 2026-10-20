#!/usr/bin/env python3
"""padtool: talk to a Cyclone 2 pad or dongle running this firmware.

    padtool [--board pad|dongle] find    devices running this firmware
    padtool --board dongle target ADDR  dongle: page this device (e.g. our pad in pairing)
    padtool --board dongle pair   dongle: forget its bond and pair with a pad
                                  whose dongle slot is pairing (30 s), as a
                                  press of its button does
    padtool identify | status     what the pad reports
    padtool log [-f]              the pad's log (-f: follow)
    padtool stats [-w SECONDS] [--reset]   loop timing and counters (-w: every N s)
    padtool calibrate            start shared sensor calibration (protocol 3+)
    padtool reboot | loader       restart; restart into the ROM loader (for
                                  jlnewfw flash)
    padtool connect [SLOT]        connect a slot (default: the last), as Home does
    padtool pair SLOT             make the slot pairable; its host is replaced once one pairs
                                  (same address)
    padtool unpair SLOT           forget the host and save a new Bluetooth address (protocol 6+)
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
    padtool tool [HIDRAW]         a pad or dongle that is a game controller on
                                  USB (a HID one): back to the tool device, via
                                  feature report 0xf5 on each hidraw (or HIDRAW)

    padtool fields [--dev]               every setting: name, values, default, help
                                         (--dev: the developer settings too)
    padtool get [TARGET [NAME]]          values; * marks a set one (else default)
    padtool set TARGET NAME VALUE [NAME VALUE ...]   change and commit
    padtool reset TARGET [NAME ...]      back to defaults: the fields, or all but the pairing
    padtool backup-settings FILE | restore-settings FILE [--calibration]
    padtool factory-reset                every slot, the palette and the remap slots (bonds too)
    padtool ephemeral                    from now until the next boot nothing is saved
    padtool keep                         reboot, then send what ephemeral mode tried

TARGET: a slot (A B X Y up down left right), palette, calibration or a remap
slot (remap-a ... remap-right).  VALUE:
a name for a choice, a number, ENTRY:LEVEL for a colour (palette entry 0-15,
brightness 0-15), #rrggbb for a palette entry, aa:bb:cc:dd:ee:ff for an
address, 15 comma-separated output bytes or a preset (linear, concave, s-curve) for stick curve points, hex for other
bytes; "default" puts a field back to its default.

A remap slot's map is "KEY held: hold KEY" (or nothing), one per key:
"x=y y=x capture=none"; its rules are ';'-separated
"INPUT TRIGGER[:MS] EFFECT [OUT+OUT][@HZ|@max]":
    triggers held, held_for:MS  with  hold, turbo, turbo_modifier, nothing
             pressed, released, tapped:MS, double_tapped:MS  with  tap, toggle_hold, toggle_turbo,
                                                               toggle_turbo_modifier
    e.g. "r4 held hold l1+r1; a held turbo a@max; r4 tapped:300 tap a; r4 held_for:300 hold l1"
A turbo modifier's keys are the ones pressed: "r4 held turbo_modifier a+b@15"
pulses whatever A and B send while R4 is held.
Rules add to a key's map row; r4=none drops R4's own press.  A remap slot
also has, per physical stick, left_/right_maps_to ("left stick", "right
stick", nothing), left_/right_x_/y_axis (normal, inverted), the inner and
outer deadzones and left_/right_curve (as a slot had).  Keys: a b x y up down left right l1 r1
l4 r4 l3 r3 minus plus home m capture (or screenshot).

The pad is a mass-storage device with no medium; commands are vendor SCSI
CDBs (see the comment at the top of src/usb.c).  Flashing is tools/jlnewfw.c's (make flash).
"""
import argparse, collections, glob, json, os, struct, sys, time
from padtransport import scsi, device_paths
import padcurves
import padremap
if sys.platform == "linux":
    import fcntl

OP = 0xF5
STATES = ["boot", "idle", "connecting", "pairing", "connected", "stopping", "sleeping", "rebooting"]


def inquiry(path):
    try:
        data = scsi(path, [0x12, 0, 0, 0, 36], 36, 1000)
    except OSError:
        return None
    return data[8:32].decode("ascii", "replace").strip()


def discover_devices(include_loaders=False, paths=None):
    """Yield path, name, IDENTIFY; opted-in ROM loaders have no IDENTIFY."""
    for path in device_paths() if paths is None else paths:
        name = inquiry(path)
        if name and name.startswith("BR23") and "UBOOT" in name:
            if include_loaders:
                yield path, name, None
            continue
        if name and name.startswith("BR23"):
            try:
                identity = scsi(path, [OP, 0x00], 64, 1000)
                if len(identity) >= 40 and identity[:4] == b"C2PD" and identity[6] in BOARD_IDS.values():
                    yield path, name, identity
            except OSError:
                pass


def devices(first=False):
    """Running devices for the selected board; stop early with first=True."""
    found = []
    for path, name, identity in discover_devices():
        if identity[6] == BOARD_IDS[BOARD]:
            found.append((path, name))
            if first:
                break
    return found


def find_device():
    found = devices(first=True)
    return found[0][0] if found else None


def need_device():
    path = find_device()
    if not path:
        sys.exit(f"padtool: no {BOARD} found (is it plugged in and running this firmware?)")
    return path


def wait_for_device(seconds, every=0.5):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        path = find_device()
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
    if version != 6:
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
     s["anchors"], s["late"], s["buttons"], s["frames"], s["resyncs"]) = struct.unpack_from("<12I", d, o)
    o += 48
    for name in ("ack_wait", "slot_air", "ack_gap", "want_send"):
        v = struct.unpack_from(HIST, d, o)
        s[name] = {"sum": v[0], "count": v[1], "max": v[2], "buckets": v[3:]}
        o += struct.calcsize(HIST)
    s["link"] = dict(zip(("transport", "phy_tx", "phy_rx", "mode", "interval", "latency", "timeout",
                          "tx_octets", "tx_time", "rx_octets", "rx_time", "acl_buffers", "poll_us", "lead_us", "period_us"),
                         struct.unpack_from("<4B8HIII", d, o)))
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
    if k["transport"]:
        print(f"just in time: reports built {k['lead_us']} us before their anchor, every {k['period_us']} us"
              if k["lead_us"] else "just in time: off (built when the radio has room)")
    else:
        print("link: none")


def show_stats(s):
    secs = max((s["now"] - s["since"]) / 1000, 0.001)
    busy = s["pass"]["sum"] / 1e6 / secs * 100
    print(f"over {secs:.1f} s: {s['passes']} loop passes ({s['passes'] / secs:.0f}/s), busy {busy:.2f} %")
    print(f"{'':16} {'count':>7} {'avg us':>8} {'max us':>8}  distribution (us)")
    for name, label in (("pass", "loop pass"), ("interval", "report interval"),
                        ("age", "report age"), ("ack_wait", "sent to ack"),
                        ("slot_air", "slot to air"), ("ack_gap", "between acks"),
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
          f"packets on air {s['acked']} ({s['acked'] / secs:.1f}/s), "
          f"late {s['late']}, anchors {s['anchors'] / secs:.1f}/s")
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
               7: "ack", 8: "ctl sleep", 9: "stick", 10: "handed",
               11: "anchor"}


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


def read_log(path, seq):
    """Return the first sequence and text in one firmware log chunk."""
    data = cmd(path, 0x02, seq, 512)
    if len(data) < 8:
        raise ValueError("Short firmware log header")
    first, size = struct.unpack_from("<II", data)
    if size > len(data) - 8:
        raise ValueError("Short firmware log data")
    return first, data[8:8 + size].decode("utf-8", "replace")


def log(path, follow):
    seq = 1
    while True:
        try:
            first, text = read_log(path, seq)
        except OSError:
            if not follow:
                raise
            print(f"-- {BOARD} gone {clock()}; waiting --", flush=True)
            path = wait_for_device(3600, 0.1)
            if not path:
                return
            print(f"-- {BOARD} back {clock()} --", flush=True)
            continue
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


# ---- settings ------------------------------------

SLOT_NAMES = ["A", "B", "X", "Y", "up", "down", "left", "right"]
REMAP_NAMES = ["remap-" + n.lower() for n in SLOT_NAMES]
TARGETS = SLOT_NAMES + ["palette", "calibration"] + REMAP_NAMES
REMAP_TARGET = 10
KINDS = ["slot", "palette", "calibration", "remap"]
T_ENUM, T_NUMBER, T_COLOUR, T_RGB, T_BYTES, T_TEXT = range(6)
RESULTS = ["ok", "unknown field", "bad length", "out of range", "too many changes at once"]
JOURNAL = os.path.expanduser("~/.cache/padtool/ephemeral.json")
PAIRING = ("address", "bond")   # kept by a whole-slot reset


def target_index(name):
    for i, t in enumerate(TARGETS):
        if t.lower() == name.lower():
            return i
    sys.exit(f"padtool: no target {name!r}; one of {' '.join(TARGETS)}")


def kind_of(t):
    return 0 if t < 8 else 3 if t >= REMAP_TARGET else t - 7


def targets(table):
    """The targets this firmware has: remap slots only where it describes them."""
    return list(range(len(TARGETS) if any(f["kind"] == 3 for f in table) else REMAP_TARGET))


def remap_field(f, name):
    return f["kind"] == 3 and f["name"] == name


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
    if t == T_TEXT:
        return padremap.show_name(raw)
    if remap_field(f, "map"):
        return padremap.show_map(raw)
    if remap_field(f, "rules"):
        return padremap.show_rules(raw)
    if t == T_ENUM:
        return f["names"][raw[0]] if raw[0] < len(f["names"]) else str(raw[0])
    if t == T_NUMBER:
        return str(int.from_bytes(raw, "little"))
    if t == T_COLOUR:
        return f"{raw[0]}:{raw[1]}"
    if t == T_RGB:
        return "#" + raw.hex()
    if f["name"] in ("left_curve_points", "right_curve_points"):
        return ",".join(str(b) for b in raw)
    if f["name"] == "address":
        return "derived" if not any(raw) else ":".join(f"{b:02x}" for b in reversed(raw))
    return "none" if not any(raw) else raw.hex()


STICK_FULL = 32767         # CAL_STICK_MAX: a stick's full deflection, and the deadzone scale


def describe_calibration(raw):
    """struct set_cal (src/pad.h) in words: sensor readings and the measured
    outer edge of each stick, each trigger's travel.  Lines of text."""
    raw = bytes(raw)
    if len(raw) < 112:
        return ["(not the calibration layout)"]
    words = struct.unpack_from("<56H", raw)
    lines = []
    for side, base in (("Left", 0), ("Right", 24)):
        minimum, center, maximum = words[base:base + 2], words[base + 2:base + 4], words[base + 4:base + 6]
        gate = words[base + 6:base + 22]
        if all(minimum[a] < center[a] < maximum[a] for a in (0, 1)) and all(gate):
            edge = [g * 100 / STICK_FULL for g in gate]
            lines.append(f"{side} stick: X {minimum[0]}–{center[0]}–{maximum[0]}, "
                         f"Y {minimum[1]}–{center[1]}–{maximum[1]} (sensor low–rest–high); "
                         f"outer edge {min(edge):.0f}–{max(edge):.0f}% of full around 16 directions")
        else:
            lines.append(f"{side} stick: not calibrated (built-in defaults)")
    for side, base in (("Left", 48), ("Right", 52)):
        rest, full, inner, outer = words[base:base + 4]
        if rest > full:
            lines.append(f"{side} trigger: rest {rest}, fully pressed {full} (sensor); "
                         f"deadzones {inner * 100 / STICK_FULL:.0f}% at rest, {outer * 100 / STICK_FULL:.0f}% at the end")
        else:
            lines.append(f"{side} trigger: not calibrated (built-in handling)")
    return lines


def parse_value(f, text):
    """Bytes for a field, or b"" for its default."""
    t = f["type"]
    if text == "default":
        return b""
    try:
        if t == T_TEXT:
            return padremap.parse_name(text, f["size"])
        if remap_field(f, "map"):
            return padremap.parse_map(text)
        if remap_field(f, "rules"):
            return padremap.parse_rules(text)
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
        elif f["name"] in ("left_curve_points", "right_curve_points"):
            v = padcurves.preset(text, f["size"]) if text in padcurves.PRESET_NAMES else bytes(int(x) for x in text.split(",")) if "," in text else bytes.fromhex(text)
            v = padcurves.validate(v, f["size"])
        elif f["name"] == "address":
            v = bytes(reversed(bytes.fromhex(text.replace(":", ""))))
        else:
            v = bytes.fromhex(text)
    except padremap.RemapError as error:
        sys.exit(f"padtool: {f['name']}: {error}")
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


def read_target(path, table, t, defaults=False):
    """{name: (value bytes, set)} in table order."""
    d = cmd(path, 0x27 if defaults else 0x21, t, 512)
    if len(d) < 3:
        return {}
    count, o, by_id, out = d[2], 3, {}, {}
    for _ in range(count):
        fid, is_set, size = d[o], d[o + 1], d[o + 2]
        by_id[fid] = (d[o + 3:o + 3 + size], bool(is_set))
        o += 3 + size
    for f in table:
        if f["kind"] == kind_of(t) and f["id"] in by_id:
            out[f["name"]] = by_id[f["id"]]
    return out


UNPAIR_ID = 0xfe
UNPAIR_PROTOCOL = 6


def unpair(path, slot):
    """Firmware clears the bond and generates a new address in the same commit."""
    if not 0 <= slot < 8:
        raise ValueError("Unpair requires a pad slot")
    identity = cmd(path, 0, length=64)
    if len(identity) < 40 or identity[:4] != b"C2PD" or identity[6] != BOARD_IDS["pad"]:
        raise ValueError("Unpair requires a pad")
    if int.from_bytes(identity[4:6], "little") < UNPAIR_PROTOCOL:
        raise ValueError("Unpair with a new address requires updated firmware (protocol 6+)")
    send(path, [(slot, UNPAIR_ID, b"")])


def send(path, items, journal=True):
    """items: [(target, id, value bytes)], value b"" for a default; id 0 resets
    the target; UNPAIR_ID unpairs a slot and renews its address. Written in
    chunks, then committed; nothing applies if any is refused."""
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
            with open(JOURNAL) as file:
                j = json.load(file)
        except (OSError, ValueError):
            j = []
        saved, table = [], None
        for t, fid, v in items:
            if fid == UNPAIR_ID:
                # Replay the resulting identity, not an action that would
                # generate a different address when `keep` reboots the pad.
                if table is None:
                    table = fields(path)
                values = read_target(path, table, t)
                for name in PAIRING:
                    field = field_named(table, t, name)
                    raw, explicit = values[name]
                    saved.append([t, field["id"], raw.hex() if explicit else ""])
            else:
                saved.append([t, fid, v.hex()])
        j.append(saved)
        with open(JOURNAL, "w") as file:
            json.dump(j, file)


def ephemeral_now(path):
    return bool(cmd(path, 0x01, length=16)[12])


DEV = False
BOARD_IDS = {"pad": 0, "dongle": 1}    # IDENTIFY byte 6
BOARD = "pad"


def show_fields(path, dev=False):
    table = fields(path)
    shown_dev = False
    for f in sorted(table, key=lambda f: f["dev"]):
        if f["dev"] and not dev:
            continue
        if f["dev"] and not shown_dev:
            print("\ndeveloper settings: not for normal use")
            shown_dev = True
        values = (" | ".join(f["names"]) if f["type"] == T_ENUM else
                  f"0-{f['max']}" if f["type"] == T_NUMBER else
                  "ENTRY:LEVEL" if f["type"] == T_COLOUR else
                  "#rrggbb" if f["type"] == T_RGB else f"text, {f['size']} bytes" if f["type"] == T_TEXT else
                  "KEY=KEY ... | identity" if remap_field(f, "map") else
                  "INPUT TRIGGER EFFECT OUT; ... | none (padtool -h)" if remap_field(f, "rules") else
                  f"{f['size']} bytes")
        defs = ""
        if f["defaults"] and f["type"] != T_BYTES:
            shown = [show_value(f, d) for d in f["defaults"]]
            defs = f"  default {shown[0]}" if len(set(shown)) == 1 else \
                   "  default " + " ".join(f"{SLOT_NAMES[i]}={v}" for i, v in enumerate(shown))
        print(f"{KINDS[f['kind']]:11} {f['name']:16} {values}{defs}\n{'':28} {f['help']}")


def get(path, target, name):
    table = fields(path)
    for t in [target_index(target)] if target else targets(table):
        values = read_target(path, table, t)
        for f in table:
            if f["kind"] != kind_of(t) or (name and f["name"] != name) or f["name"] not in values:
                continue
            if f["dev"] and not name and not DEV and not values[f["name"]][1]:
                continue                     # developer settings at default: --dev shows them
            raw, is_set = values[f["name"]]
            if f["kind"] == 2 and f["name"] == "calibration":
                print(f"{TARGETS[t]:11} {f['name']:16} {'*' if is_set else ' '} " +
                      f"\n{'':30}".join(describe_calibration(raw)))
                continue
            print(f"{TARGETS[t]:11} {f['name']:16} {'*' if is_set else ' '} {show_value(f, raw)}"
              + ("   (developer)" if f["dev"] else ""))


def set_values(path, target, pairs):
    if not pairs or len(pairs) % 2:
        sys.exit("padtool set TARGET NAME VALUE [NAME VALUE ...]")
    table, t = fields(path), target_index(target)
    items = []
    for name, text in zip(pairs[::2], pairs[1::2]):
        if name in ("left_curve", "right_curve") and any(f["name"] == name + "_enabled" for f in table):
            enabled = field_named(table, t, name + "_enabled")
            points = field_named(table, t, name + "_points")
            if text in ("disabled", "off"):
                items.append((t, enabled["id"], parse_value(enabled, "disabled")))
            elif text in ("custom", "enabled", "on"):
                items.append((t, enabled["id"], parse_value(enabled, "enabled")))
            elif text in padcurves.PRESET_NAMES:
                items.extend([(t, points["id"], padcurves.preset(text, points["size"])),
                              (t, enabled["id"], parse_value(enabled, "enabled"))])
            else:
                sys.exit(f"padtool: {name}: use linear, concave, s-curve, custom, enabled or disabled")
            continue
        f = field_named(table, t, name)
        items.append((t, f["id"], parse_value(f, text)))
        if f["dev"]:
            print(f"note: {name} is a developer setting")
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
    for t in targets(table):
        name = TARGETS[t]
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
        if t not in targets(table):
            print(f"{name}: not in this firmware, skipped")
            continue
        if t < 8 and any(f["kind"] == 0 and f["name"] == "left_curve_enabled" for f in table):
            size = field_named(table, t, "left_curve_points")["size"]
            values = padcurves.migrate_slot(values, size)
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


def prepare_curve_upgrade(board, backup_file):
    """Capture legacy modes before entering the loader; shared by flash UIs."""
    if board != "pad":
        return None
    found = [(path, ident) for path, _, ident in discover_devices() if ident[6] == BOARD_IDS["pad"]]
    if len(found) != 1 or int.from_bytes(found[0][1][4:6], "little") >= 5:
        return None
    file = str(backup_file) + ".settings.json"
    backup(found[0][0], file)
    return file


def finish_curve_upgrade(file):
    if not file:
        return
    path = wait_for_device(30)
    if not path:
        raise RuntimeError(f"Pad did not return to tool mode; restore settings from {file}")
    identity = cmd(path, 0, length=64)
    with open(file) as stream:
        saved = json.load(stream)
    if identity[24:40].hex() != saved["flash_id"]:
        raise RuntimeError(f"The detected pad differs from the upgrade backup; restore settings explicitly from {file}")
    if int.from_bytes(identity[4:6], "little") >= 5:
        restore(path, file, calibration=True)
        print("Legacy curve settings migrated; settings and calibration restored")


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
        path = wait_for_device(30)
        if not path or ephemeral_now(path):
            sys.exit("padtool: the pad did not come back in normal mode")
    for b in batches:
        send(path, [(t, fid, bytes.fromhex(v)) for t, fid, v in b], journal=False)
    os.remove(JOURNAL)
    print(f"kept {len(batches)} changes")


def tool(paths):
    """Feature report 0xf5 "tool": a USB personality (src/pad.h) ends and the
    tool device comes back.  Only our firmware knows it; others refuse it."""
    if sys.platform != "linux":
        raise OSError("Tool-mode switching via HID is currently Linux-only; end the USB session on the pad first")
    report = b"\xf5tool"
    hidiocsfeature = 0xC0004806 | (len(report) << 16)
    if not paths:
        paths = sorted(glob.glob("/dev/bwrap/hidraw*") + glob.glob("/dev/hidraw*"))
    for path in paths:
        try:
            with open(path, "rb+", buffering=0) as f:
                fcntl.ioctl(f, hidiocsfeature, bytearray(report))
            print(f"{path}: sent")
        except OSError as e:
            # Ours leaves the bus while answering: the request ends in EPROTO.
            print(f"{path}: {'sent' if e.errno == 71 else e.strerror}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command", choices=["target", "find", "identify", "status", "log", "stats", "calibrate", "reboot", "loader", "relink", "connect", "pair", "unpair", "profile", "peek", "trace",
                                       "fields", "get", "set", "reset", "backup-settings",
                                       "restore-settings", "factory-reset", "ephemeral", "keep", "tool"])
    p.add_argument("args", nargs="*")
    p.add_argument("--calibration", action="store_true", help="restore-settings: calibration too")
    p.add_argument("-f", "--follow", action="store_true")
    p.add_argument("-w", "--watch", type=float, metavar="SECONDS", help="stats: show and reset every N seconds")
    p.add_argument("--reset", action="store_true", help="stats: reset the counters after reading")
    p.add_argument("--hz", type=int, default=2000, help="profile: samples per second")
    p.add_argument("--dev", action="store_true",
                   help="fields, get: include the developer settings")
    board = p.add_mutually_exclusive_group()
    board.add_argument("--board", choices=BOARD_IDS, default="pad",
                       help="board to talk to (default: pad)")
    board.add_argument("--dongle", dest="board", action="store_const", const="dongle",
                       help="alias for --board dongle")
    p.add_argument("--top", type=int, default=40, help="profile: functions to show")
    p.add_argument("--by", choices=["file", "group", "line"],
                   help="profile: add up per source file, per part, or per source line (zoom first)")
    p.add_argument("--elf", help="profile: the running build's ELF (default: selected board's build)")
    a = p.parse_args()
    global DEV, BOARD
    DEV = a.dev
    BOARD = a.board
    if a.elf is None:
        a.elf = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", BOARD, "app.elf")
    if a.command == "target":
        if BOARD != "dongle":
            p.error("target requires --board dongle")
        if len(a.args) != 1:
            p.error("target requires an address: AA:BB:CC:DD:EE:FF")
        parts = a.args[0].split(":")
        try:
            if len(parts) != 6 or any(len(part) != 2 for part in parts):
                raise ValueError
            address = bytes(int(part, 16) for part in parts)
        except ValueError:
            p.error("invalid address; expected AA:BB:CC:DD:EE:FF")
    if BOARD == "dongle" and a.command in {
        "stats", "calibrate", "relink", "connect", "fields", "get", "set", "reset",
        "backup-settings", "restore-settings", "factory-reset", "keep",
    }:
        p.error(f"{a.command} is not supported by dongle firmware")
    if a.command == "tool":
        tool(a.args)
        return
    if a.command == "target":    # dongle: the device to page, AA:BB:CC:DD:EE:FF
        scsi(need_device(), bytes([OP, 0x30]) + address, 0)
    elif a.command == "find":
        for path, name in devices():
            print(f"{BOARD} {path}  {name}")
    elif a.command == "identify":
        print(identify(need_device()))
    elif a.command == "status":
        print(status(need_device()))
    elif a.command == "log":
        log(need_device(), a.follow)
    elif a.command == "stats":
        try:
            stats(need_device(), a.watch, a.reset)
        except KeyboardInterrupt:
            pass
    elif a.command == "trace":
        trace(need_device(), a.watch or 0.5, a.top)
    elif a.command == "peek":
        addr, words = int(a.args[0], 16), int(a.args[1]) if len(a.args) > 1 else 4
        d = cmd(need_device(), 0x15, addr, 4 * words)
        for i in range(words):
            print(f"{addr + 4 * i:08x}: {struct.unpack_from('<I', d, 4 * i)[0]:08x}")
    elif a.command == "profile":
        zoom = (int(a.args[0], 16), int(a.args[1]) if len(a.args) > 1 else 1) if a.args else None
        profile(need_device(), a.watch or 10, a.hz, a.top, a.elf, zoom, a.by)
    elif a.command == "unpair":
        if BOARD != "pad" or len(a.args) != 1 or a.args[0] not in SLOT_NAMES:
            p.error("unpair requires a pad slot: A B X Y up down left right")
        try:
            unpair(need_device(), SLOT_NAMES.index(a.args[0]))
        except ValueError as error:
            p.error(str(error))
    elif a.command == "pair":
        names = "A B X Y up down left right".split()
        if BOARD == "dongle" and not a.args:
            slot = 0                         # forget the dongle's single bond
        elif len(a.args) == 1 and a.args[0] in names:
            slot = names.index(a.args[0])
        else:
            p.error("pair requires a slot: A B X Y up down left right (optional for dongle)")
        cmd(need_device(), 0x17, slot)
    elif a.command == "connect":
        names = "A B X Y up down left right".split()
        slot = names.index(a.args[0]) if a.args else 0xfe
        cmd(need_device(), 0x13, slot)
    elif a.command == "relink":
        cmd(need_device(), 0x12)
    elif a.command == "calibrate":
        path = need_device()
        if int.from_bytes(cmd(path, 0, length=64)[4:6], "little") < 3:
            p.error("calibrate needs pad firmware protocol 3 or newer")
        cmd(path, 0x18)
        print("Leave sticks and triggers untouched for 2.5 seconds; then circle both sticks and press both triggers fully. A or B saves; Home cancels.")
    elif a.command in ("reboot", "loader"):
        cmd(need_device(), 0x10 if a.command == "reboot" else 0x11)
    elif a.command == "fields":
        show_fields(need_device(), a.dev)
    elif a.command == "get":
        get(need_device(), a.args[0] if a.args else None, a.args[1] if len(a.args) > 1 else None)
    elif a.command == "set":
        if not a.args:
            sys.exit("padtool set TARGET NAME VALUE [NAME VALUE ...]")
        set_values(need_device(), a.args[0], a.args[1:])
    elif a.command == "reset":
        if not a.args:
            sys.exit("padtool reset TARGET [NAME ...]")
        reset(need_device(), a.args[0], a.args[1:])
    elif a.command in ("backup-settings", "restore-settings"):
        if len(a.args) != 1:
            sys.exit(f"padtool {a.command} FILE")
        (backup if a.command == "backup-settings" else
         lambda p, f: restore(p, f, a.calibration))(need_device(), a.args[0])
    elif a.command == "factory-reset":
        path = need_device()
        table = fields(path)
        items = []
        for t in targets(table):
            if TARGETS[t] == "calibration":
                continue
            items.append((t, 0, b""))
            if t < 8:
                items += [(t, field_named(table, t, n)["id"], b"") for n in PAIRING]
        send(path, items)
        print("every slot, the palette and the remap slots back to defaults; calibration kept")
    elif a.command == "ephemeral":
        path = need_device()
        cmd(path, 0x25)
        if BOARD == "pad":
            try:
                os.remove(JOURNAL)
            except OSError:
                pass
        print("ephemeral: nothing is saved until the next boot" +
              (" (Home winks)" if BOARD == "pad" else " (bond changes stay in RAM)"))
    elif a.command == "keep":
        keep(need_device())


if __name__ == "__main__":
    main()
