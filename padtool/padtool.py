#!/usr/bin/env python3
"""padtool: talk to a Cyclone 2 pad or dongle running this firmware.

    padtool [--board pad|dongle] find    devices running this firmware
    padtool -d DEVICE ...         with several connected: the one at that path,
                                  or whose flash ID contains DEVICE (from find)
    padtool --board dongle tool-only [on | off]  dongle: stay this tool device until power
                                  loss (on), also while a pad is linked, for its log (the
                                  pad's type, or the hub, is not put on USB; padtool tool
                                  brings the tool device back first); no argument: show it
    padtool --board dongle hub [on | off]   dongle: hub mode until the next boot (padhub
                                  sets it itself): a linked pad is not its type on
                                  USB but its link packets, for build/host/padhub on
                                  a Linux PC (src/core/link.h); no argument: show it
    padtool --board dongle pair [SLOT]   dongle: forget the bond of SLOT (0, the
                                  dongle's own pad; 0-5 for a hub) and pair
                                  it with a pad whose dongle slot is pairing (30 s),
                                  as a press of its button does for 0; to a dongle
                                  that is the hub on USB, through its hidraw
    padtool identify | status     what the pad reports
    padtool log [-f]              the pad's log (-f: follow)
    padtool stats [-w SECONDS] [--reset]   loop timing and counters (-w: every N s)
    padtool calibrate            start shared sensor calibration
    padtool led-demo [list | N] [--seconds S]   the pad shows its LED animations
                                  in turn, from N: Enter for the next (or
                                  every S seconds); list: what there is
    padtool rumble LEFT [RIGHT] [--seconds S]   run the motors at LEFT and RIGHT
                                  percent of full power (default RIGHT = LEFT) for
                                  S seconds (1; at most 10); 0 stops
    padtool rumble-test [list | N] [--seconds S]   rumble strengths, one side,
                                  ramps and short pulses, from N: Enter for the
                                  next, r again (or all, S seconds apart)
    padtool rumble-sticks [--step N] [--left MIN-MAX] [--right MIN-MAX] [--kick MS] [--full]
                                  left stick up/down sets one strength, spread
                                  over each motor's range as the firmware does
                                  (the pad's base remap settings for step N, 3
                                  unless given;
                                  --full: 0-100, no kick), right stick
                                  left/right the balance;
                                  each stays where it is left (L1: strength 0,
                                  R1: even); Ctrl-C stops
    padtool input [-f]            buttons, sticks and triggers, calibrated and
                                  as read (-f: live)
    padtool reboot | loader       restart; restart into the ROM loader (for
                                  jlnewfw flash)
    padtool update FILE           rewrite the app in place through the pad's
                                  updater (a -split build's update.bin); a pad
                                  running the app restarts into the updater first
    padtool connect [SLOT]        connect a slot (default: the last), as Home does
    padtool disconnect            end the session, keeping its pairing
    padtool pair SLOT             make the slot pairable; its host is replaced once one pairs
                                  (same address)
    padtool unpair SLOT           forget the host and save a new Bluetooth address
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
    padtool latency [--seconds S] [--period MS] [--event PATH]
                                  end to end: the pad makes A edges at random
                                  times (one per MS, 20), the PC times them on
                                  the evdev node of whatever carries the slot
                                  (a dongle, its own USB, the PC's Bluetooth):
                                  pad, air and total latency, the rate the PC
                                  sees and edges lost.  Leave A alone; the pad
                                  stays on USB for its clock.  --throughput:
                                  A flips in every report the pad sends, for
                                  the most reports/s the link carries and the
                                  reports lost or merged on the way
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

TARGET: a slot (slot-a slot-b slot-x slot-y slot-up slot-down slot-left
slot-right; A, up, ... also work), palette, calibration, a remap slot
(remap-a ... remap-right) or remap-base, whose values the remap slots take
for every setting they have not set (but their names; remap-a keeps its swap).  VALUE:
a name for a choice, a number, a palette entry 0-15 for a colour (how bright
is the palette's: steady_brightness and the others), #rrggbb for a palette
entry, aa:bb:cc:dd:ee:ff for an
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
CDBs (see the comment at the top of src/core/usb.c).  Flashing is tools/jlnewfw.c's (make gamesir-cyclone-2-pad-flash).
"""
import argparse, collections, glob, json, os, struct, sys, threading, time
from padtransport import scsi, device_paths, usb_location
import padcurves
import padremap
from padknown import CHIPS
if sys.platform == "linux":
    import fcntl

OP = 0xF5
STATES = ["boot", "idle", "connecting", "pairing", "connected", "stopping", "sleeping", "rebooting"]


def inquiry(path):
    try:
        data = scsi(path, [0x12, 0, 0, 0, 40], 40, 1000)
    except OSError:
        return None
    return data


def discover_devices(include_loaders=False, paths=None):
    """Yield path, name, IDENTIFY; opted-in ROM loaders have no IDENTIFY."""
    for path in device_paths() if paths is None else paths:
        data = inquiry(path)
        if not data or len(data) < 32:
            continue
        vendor = data[8:16].decode("ascii", "replace").strip()
        name = data[8:32].decode("ascii", "replace").strip()
        if vendor in CHIPS and data[16:21] == b"UBOOT":
            if include_loaders:
                yield path, name, None
            continue
        if vendor in CHIPS or vendor == "GameSir" or data[36:40] == b"C2PD":
            try:
                identity = scsi(path, [OP, 0x00], 88, 1000)
                if len(identity) >= 56 and identity[:4] == b"C2PD" and identity[6] in BOARD_IDS.values():
                    yield path, name, identity
            except OSError:
                pass


DEVICE = None        # --device: a path or part of a flash ID
DEVICE_ID = None     # the flash ID of the one chosen: found again after a reboot


def devices(first=False, identities=False):
    """Running devices for the selected board; stop early with first=True."""
    found = []
    for path, name, identity in discover_devices():
        if identity[6] == BOARD_IDS[BOARD]:
            found.append((path, name, identity) if identities else (path, name))
            if first:
                break
    return found


def board_name(identity):
    """The board IDENTIFY names, e.g. "Nova 2 Lite" (older firmware: its image's PID)."""
    field = identity[56:88] if len(identity) >= 88 else identity[40:56]
    return field.split(b"\0")[0].decode(errors="replace")


def matching():
    """(path, flash ID, identity) of the devices --device (or the earlier
    choice) picks: by path, USB port, part of the flash ID or board name."""
    found = [(path, identity[24:40].hex(), identity) for path, _, identity in devices(identities=True)]
    if DEVICE_ID:
        return [d for d in found if d[1] == DEVICE_ID]
    if DEVICE:
        key = DEVICE.lower()
        return [d for d in found if d[0] == DEVICE or usb_location(d[0]) == DEVICE
                or key in d[1] or key in board_name(d[2]).lower()]
    return found


def is_path(device):
    """--device names a device node rather than part of a flash ID."""
    return device.startswith("\\\\.\\") or os.sep in device or "/" in device or os.path.exists(device)


def answers(path):
    """The flash ID of our firmware at path, else None."""
    try:
        identity = scsi(path, [OP, 0x00], 64, 1000)
    except OSError:
        return None
    return identity[24:40].hex() if len(identity) >= 56 and identity[:4] == b"C2PD" else None


def find_device():
    if DEVICE and is_path(DEVICE):
        here = answers(DEVICE)
        if here and (not DEVICE_ID or here == DEVICE_ID):
            return DEVICE
        found = matching() if DEVICE_ID else []   # renumbered after a reboot
        return found[0][0] if found else None
    found = matching()
    return found[0][0] if found else None


def need_device():
    global DEVICE_ID
    if DEVICE and is_path(DEVICE):    # used as given, found or not
        if not DEVICE.startswith("\\\\.\\") and not os.path.exists(DEVICE):
            sys.exit(f"padtool: {DEVICE} does not exist")
        DEVICE_ID = answers(DEVICE)
        return DEVICE
    found = matching()
    if not found:
        sys.exit(f"padtool: no {BOARD} {'matching ' + DEVICE + ' ' if DEVICE else ''}found "
                 f"(is it plugged in and running this firmware?)")
    if len(found) > 1:
        lines = "\n".join(f"  {path}  usb {usb_location(path) or '?'}  {board_name(ident)}  flash-id {fid}"
                           for path, fid, ident in found)
        sys.exit(f"padtool: {len(found)} {BOARD}s found; pick one with --device and its path, USB port, "
                 f"part of its board name or flash ID (padtool find):\n{lines}")
    DEVICE_ID = found[0][1]
    return found[0][0]


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
    d = cmd(path, 0x00, length=88)
    proto, = struct.unpack_from("<H", d, 4)
    build = d[8:24].split(b"\0")[0].decode()
    pid = d[40:56].split(b"\0")[0].decode()
    board = f"{board_name(d)}, " if len(d) >= 88 else ""
    return f"{board}usb {usb_location(path) or '?'} protocol {proto} pid {pid} build {build} flash-id {d[24:40].hex()}"


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
    if version != 1:
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
                      ("external/bluekitchen-btstack/", "btstack"), ("/usb/", "usb (sdk)"),
                      ("crypto_toolbox", "crypto (sdk)"), ("/FreeRTOS", "os"), ("/lib/system/", "os"),
                      ("/lib/driver/", "drivers (sdk)"), ("external/jieli-fw-AC63_BT_SDK/", "sdk app code")):
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


UPDATE_ERRORS = ["", "bad size", "out of order", "not begun", "verify failed (read back differs)",
                 "incomplete", "bad header", "CRC mismatch", "no chip key fits"]


def is_updater(path):
    return cmd(path, 0x00, length=88)[7] == 1


def update_info(path):
    d = cmd(path, 0x41, length=32)
    version, state, error, valid, at, room, written, begun, key = struct.unpack_from("<BBBBIIIII", d)
    return dict(state=state, error=error, valid=valid, at=at, room=room, written=written, begun=begun, key=key)


def update_step(path, what):
    i = update_info(path)
    if i["error"]:
        sys.exit(f"padtool: update {what}: {UPDATE_ERRORS[i['error']] if i['error'] < len(UPDATE_ERRORS) else i['error']}"
                 f" ({i['written']} of {i['begun']} bytes written; the pad stays in its updater)")
    return i


def update(path, image):
    """Header and app as jlnewfw --main-out writes them, through the updater
    (src/updater/updater.c): begin, data in order, commit, reboot."""
    if image[:4] != b"MAIN":
        sys.exit("padtool: not an update image (update.bin from a -split build)")
    if not is_updater(path):
        print("restarting into the updater")
        cmd(path, 0x40)
        time.sleep(1)
        path = wait_for_device(15)
        if not path or not is_updater(path):
            sys.exit("padtool: the pad did not come back in its updater (an app without one reboots)")
    i = update_info(path)
    print(f"updater: app at flash {i['at']:#x}, room {i['room']} bytes, "
          f"{'an app is there' if i['valid'] else 'no app'}")
    if len(image) > i["room"]:
        sys.exit(f"padtool: the image ({len(image)} bytes) does not fit ({i['room']})")
    start = time.monotonic()
    cmd(path, 0x42, len(image))
    print(f"chip key {update_step(path, 'begin')['key']:#06x}")
    chunk = 512
    for offset in range(0, len(image), chunk):
        cmd(path, 0x43, offset, out=image[offset:offset + chunk])
        if offset // chunk % 64 == 63:
            update_step(path, f"at {offset:#x}")
            print(f"\r{offset + chunk} of {len(image)} bytes", end="", flush=True)
    i = update_step(path, "data")
    print(f"\r{i['written']} of {len(image)} bytes in {time.monotonic() - start:.1f} s")
    scsi(path, [OP, 0x44, 0, 0, 0, 0], 0, 10000)
    i = update_step(path, "commit")
    if i["state"] != 2:
        sys.exit("padtool: the updater did not commit")
    print("committed; restarting into the app")
    cmd(path, 0x10)
    time.sleep(1)
    path = wait_for_device(15)
    if not path:
        sys.exit("padtool: the pad did not come back")
    print(identify(path))


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
SLOT_TARGETS = ["slot-" + n.lower() for n in SLOT_NAMES]
REMAP_NAMES = ["remap-" + n.lower() for n in SLOT_NAMES]
TARGETS = SLOT_TARGETS + ["palette", "calibration"] + REMAP_NAMES + ["remap-base"]
REMAP_TARGET = 10
BASE_TARGET = 18            # the base remap: what the remap slots inherit
PROTOCOL = 2                # the vendor commands this padtool speaks (IDENTIFY)
KINDS = ["slot", "palette", "calibration", "remap"]
T_ENUM, T_NUMBER, T_RGB, T_BYTES, T_TEXT, T_ENTRY, T_LIST = range(7)
# T_LIST: size numbers of a byte each, each at most max, shown and written "15,15,15,15,15"
RESULTS = ["ok", "unknown field", "bad length", "out of range", "too many changes at once",
           "ephemeral mode: only the remap in use can change"]
JOURNAL = os.path.expanduser("~/.cache/padtool/ephemeral.json")
PAIRING = ("address", "bond")   # kept by a whole-slot reset


def slot_number(name):
    """A slot by its key (A, up) or target name (slot-a, slot-up); None if neither."""
    name = name.lower()
    for i, n in enumerate(SLOT_NAMES):
        if name in (n.lower(), SLOT_TARGETS[i]):
            return i
    return None


def target_index(name):
    for i, t in enumerate(TARGETS):
        if t.lower() == name.lower():
            return i
    slot = slot_number(name)              # A, up: what slots were called before slot-a
    if slot is not None:
        return slot
    sys.exit(f"padtool: no target {name!r}; one of {' '.join(TARGETS)}")


def kind_of(t):
    return 0 if t < 8 else 3 if t >= REMAP_TARGET else t - 7


class FieldTable(list):
    """fields(): the table, and the firmware's protocol."""
    protocol = PROTOCOL


def targets(table):
    """The targets the table describes: remap slots only where it has their fields,
    the base remap listed before them."""
    if not any(f["kind"] == 3 for f in table):
        return list(range(REMAP_TARGET))
    return list(range(REMAP_TARGET)) + [BASE_TARGET] + list(range(REMAP_TARGET, BASE_TARGET))


def remap_field(f, name):
    return f["kind"] == 3 and f["name"] == name


def fields(path):
    """The field table, as the firmware describes it."""
    out, n = FieldTable(), 0
    out.protocol = int.from_bytes(cmd(path, 0, length=64)[4:6], "little")
    if out.protocol != PROTOCOL:
        sys.exit(f"padtool: the firmware speaks protocol {out.protocol}, this padtool {PROTOCOL}; "
                 "use the padtool from the same build")
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
    if t == T_ENTRY:
        return str(raw[0])
    if t == T_RGB:
        return "#" + raw.hex()
    if t == T_LIST:
        return ",".join(str(b) for b in raw)
    if f["name"] in ("left_curve_points", "right_curve_points"):
        return ",".join(str(b) for b in raw)
    if f["name"] == "address":
        return "derived" if not any(raw) else ":".join(f"{b:02x}" for b in reversed(raw))
    return "none" if not any(raw) else raw.hex()


STICK_FULL = 32767         # CAL_STICK_MAX: a stick's full deflection, and the deadzone scale


def describe_calibration(raw):
    """struct set_cal (src/core/pad.h) in words: sensor readings and the measured
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
            return padremap.parse_rules(text, f["size"] // padremap.RULE_SIZE)
        if t == T_ENUM:
            if text in f["names"]:
                return bytes([f["names"].index(text)])
            sys.exit(f"padtool: {f['name']}: one of {' '.join(f['names'])}")
        if t == T_NUMBER:
            return int(text, 0).to_bytes(f["size"], "little")
        if t == T_ENTRY:
            return bytes([int(text)])
        if t == T_LIST:
            numbers = [int(x) for x in text.replace(",", " ").split()]
            if len(numbers) != f["size"]:
                sys.exit(f"padtool: {f['name']}: {f['size']} numbers, not {len(numbers)}")
            if any(not 0 <= n <= f["max"] for n in numbers):
                sys.exit(f"padtool: {f['name']}: each 0-{f['max']}")
            return bytes(numbers)
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


CMD_LED_DEMO = 0x19
LED_DEMO_REFRESH = 10        # s; the pad ends a demo 30 s after the last request


def led_demo_names(path):
    names = []
    for i in range(255):
        name = cmd(path, CMD_LED_DEMO, i, 128).split(b"\0")[0].decode()
        if not name:
            break
        names.append(name)
    return names


def led_demo(path, names, first, seconds):
    """Show the pad's LED demos from `first`: one after another every `seconds`,
    or (seconds None) Enter for the next, a number for that one, b back, q quit."""
    lock, done, current = threading.Lock(), threading.Event(), [first]

    def show(i):
        with lock:
            cmd(path, CMD_LED_DEMO, i | 1 << 8, 128)

    def keep():                          # the pad lets a demo go without these
        while not done.wait(LED_DEMO_REFRESH):
            show(current[0])

    def announce(i):
        current[0] = i
        show(i)
        print(f"{i + 1}/{len(names)}  {names[i]}")

    threading.Thread(target=keep, daemon=True).start()
    try:
        if seconds is not None:
            for i in range(first, len(names)):
                announce(i)
                time.sleep(seconds)
            return
        print("Enter: next   b: back   number: that demo   q: quit")
        announce(first)
        while True:
            answer = input().strip().lower()
            if answer == "q":
                return
            i = current[0] - 1 if answer == "b" else int(answer) - 1 if answer.isdigit() else current[0] + 1
            if 0 <= i < len(names):
                announce(i)
            elif not answer and i == len(names):
                return
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        done.set()
        with lock:
            cmd(path, CMD_LED_DEMO, 0xff)


CMD_RUMBLE = 0x1a
RUMBLE_MAX_MS = 10000


def rumble(path, left, right, ms):
    """Motors at left/right percent of full PWM duty for ms; the slot's rumble
    percent is not applied.  0 ms stops."""
    duty = [round(max(0, min(100, v)) * 255 / 100) for v in (left, right)]
    cmd(path, CMD_RUMBLE, duty[0] | duty[1] << 8 | min(int(ms), RUMBLE_MAX_MS) << 16)
    return duty


def ramp(left, right, ms, steps=40):
    return [(left * i / steps, right * i / steps, ms / steps) for i in range(1, steps + 1)]


def pulses(ms, level=100, count=4, gap=400):
    return [(level, level, ms), (0, 0, gap)] * count


def sweep(left, right, low=8, high=40, step=2, ms=1000):
    """Steps from low to high percent on the chosen sides, each one printed."""
    return [(v if left else 0, v if right else 0, ms, f"  {v}%") for v in range(low, high + 1, step)]


def kick(level, ms=1500, kick_ms=30, kick_level=100):
    """Full power for kick_ms to get the motors spinning, then level."""
    return [(kick_level, kick_level, kick_ms), (level, level, ms)]


def texture(high, low, hz, ms=2000):
    """Alternate high and low percent hz times a second."""
    half = 500 / hz
    return [(high, high, half), (low, low, half)] * round(ms / (2 * half))


RUMBLE_TESTS = (
    [(f"both {v}%", [(v, v, 1500)]) for v in (5, 10, 15, 20, 25, 30, 40, 50, 60, 70, 80, 90, 100)] +
    [(f"{side} only {v}%", [(v if side == "left" else 0, v if side == "right" else 0, 1500)])
     for side in ("left", "right") for v in (30, 60, 100)] +
    [("ramp both 0-100% over 4 s", ramp(100, 100, 4000)),
     ("ramp left 0-100% over 4 s", ramp(100, 0, 4000)),
     ("ramp right 0-100% over 4 s", ramp(0, 100, 4000)),
     ("ramp both 100-0% over 4 s", ramp(100, 100, 4000)[::-1]),
     ("left then right, 60%, alternating", [(60, 0, 300), (0, 60, 300)] * 4)] +
    [(f"pulses: {ms} ms at 100%", pulses(ms)) for ms in (20, 40, 80, 150)] +
    [(f"pulses: 80 ms at {v}%", pulses(80, v)) for v in (30, 60)] +
    # Where does the rattle start and stop, and which motor makes it?
    [("find the rattle: both 8-40% in 2% steps, 1 s each", sweep(True, True)),
     ("find the rattle: left only 8-40%", sweep(True, False)),
     ("find the rattle: right only 8-40%", sweep(False, True))] +
    # A stopped motor needs more to start than to keep turning: kick it first.
    [(f"kick 30 ms at 100%, then both {v}%", kick(v)) for v in (6, 10, 15, 20)] +
    [("kick 60 ms at 100%, then both 10%", kick(10, kick_ms=60)),
     ("soft kick 100 ms at 40%, then both 12%", kick(12, kick_ms=100, kick_level=40))] +
    # Gentle without running slowly: short bursts at a higher level.
    [("bursts: 50% 40 ms on, 160 ms off (5/s)", [(50, 50, 40), (0, 0, 160)] * 10),
     ("bursts: 40% 30 ms on, 70 ms off (10/s)", [(40, 40, 30), (0, 0, 70)] * 20),
     ("bursts: 35% 20 ms on, 20 ms off (25/s)", texture(35, 0, 25))] +
    # Textures: two levels alternating.
    [("purr: 50% / 20% at 25/s", texture(50, 20, 25)),
     ("engine idle: 60% / 25% at 12/s", texture(60, 25, 12)),
     ("engine rev: 90% / 50% at 20/s", texture(90, 50, 20)),
     ("heartbeat", [(80, 80, 70), (0, 0, 130), (55, 55, 70), (0, 0, 600)] * 3),
     ("crisp tap: kick 25 ms at 100%, then 50% for 40 ms", [(100, 100, 25), (50, 50, 40), (0, 0, 500)] * 4),
     ("left 30% + right 10% (blend)", [(30, 10, 2000)]),
     ("left 10% + right 30% (blend)", [(10, 30, 2000)])]
)


def play_rumble(path, steps):
    """Each step runs a little past its time; the next one takes over."""
    for left, right, ms, *label in steps:
        start = time.monotonic()
        if label:
            print(label[0], flush=True)
        if left or right:
            rumble(path, left, right, ms + 100)
        else:
            rumble(path, 0, 0, 0)
        time.sleep(max(0, ms / 1000 - (time.monotonic() - start)))
    rumble(path, 0, 0, 0)


CMD_INPUT = 0x04


def read_input(path):
    """The controls as the pad reads them: calibrated, before Home gestures and
    the remap, and raw.  None if the pad has none."""
    d = cmd(path, CMD_INPUT, length=64)
    if len(d) < 36:
        return None
    _, flags, _, buttons, lx, ly, rx, ry, l2, r2 = struct.unpack_from("<BBHI4h2H", d)
    return {"buttons": buttons, "keys": [k for i, k in enumerate(padremap.KEYS) if k and buttons >> i & 1],
            "left": (lx, ly), "right": (rx, ry), "l2": l2, "r2": r2, "sticks_read": bool(flags & 1),
            "raw_sticks": struct.unpack_from("<4H", d, 20), "raw_triggers": struct.unpack_from("<2H", d, 28),
            "ms": struct.unpack_from("<I", d, 32)[0]}


def input_line(i):
    return (f"L {i['left'][0]:6} {i['left'][1]:6}  R {i['right'][0]:6} {i['right'][1]:6}  "
            f"L2 {i['l2']:5}  R2 {i['r2']:5}  raw {' '.join(f'{v:4}' for v in i['raw_sticks'] + i['raw_triggers'])}  "
            f"{' '.join(i['keys']) or '-'}")


def show_input(path, follow):
    i = read_input(path)
    if not i:
        sys.exit("padtool: the pad reports no input")
    if not follow:
        print(input_line(i))
        return
    try:
        while True:
            print("\r" + input_line(read_input(path)).ljust(110), end="", flush=True)
            time.sleep(0.03)
    except KeyboardInterrupt:
        print()


def balance_text(balance):
    """How much the other side is turned down."""
    pct = round(abs(balance) * 100)
    return "even" if not pct else f"{pct}% to {'right' if balance > 0 else 'left'}"


def balance_bar(balance, half=10):
    """L----|----R, the mark where the balance is."""
    cells = ["-"] * (2 * half + 1)
    cells[half] = "|"
    cells[round(half + balance * half)] = "O"
    return "L " + "".join(cells) + " R"


def strength_bar(strength, width=20):
    """0 ####------ 100"""
    full = round(strength * width / 100)
    return "0 " + "#" * full + "-" * (width - full) + " 100"


def nudge(value, push, dt, low, high, rate=0.4):
    """Move value by a stick's push (-1..1), rate of the range a second at full
    push; it stays where it is left.  The sticks rest a little off centre."""
    if abs(push) <= 0.15:
        return value
    return max(low, min(high, value + push * rate * (high - low) * dt))


RUMBLE_STEP_DEFAULT = 3         # the rumble setting's default step


def rumble_step_settings(path, step):
    """The base remap's ((left min, max), (right min, max), kick ms) for a rumble
    step, as the pad has them: each board has its own defaults."""
    values = read_target(path, fields(path), BASE_TARGET)
    at = lambda name: values[name][0][step - 1]
    return ((at("rumble_left_min"), at("rumble_left_max")),
            (at("rumble_right_min"), at("rumble_right_max")), at("rumble_kick_ms"))


def motor_percent(strength, low, high):
    """As the firmware spreads rumble: the lightest is low, full is high."""
    return 0.0 if strength <= 0 else low + (high - low) * strength / 100


def rumble_sticks(path, ranges, kick_ms):
    """Left stick up/down: one strength, spread over each motor's range as the
    firmware does, a kick when a motor starts; right stick left/right: the
    balance.  Each stays where it is left; L1 zeroes the strength, R1 evens
    the balance."""
    print(f"Left motor {ranges[0][0]}-{ranges[0][1]}%, right motor {ranges[1][0]}-{ranges[1][1]}%, "
          f"kick {kick_ms} ms.\n"
          "Left stick: push up for stronger, down for weaker; it stays when you let go.  L1: back to 0.\n"
          "Right stick: push left or right to move the balance that way; it stays too.  R1: back to even.\n"
          "Ctrl-C: stop.")
    strength, balance, last = 0.0, 0.0, time.monotonic()   # balance -1: left only, +1: right only
    gains = (1.0, 1.0)
    kick_until = [0.0, 0.0]
    try:
        while True:
            i = read_input(path)
            now = time.monotonic()
            dt, last = now - last, now
            strength = nudge(strength, -i["left"][1] / 32767, dt, 0.0, 100.0)    # y is down
            balance = nudge(balance, i["right"][0] / 32767, dt, -1.0, 1.0, rate=0.2)
            if "l1" in i["keys"]:
                strength = 0.0
            if "r1" in i["keys"]:
                balance = 0.0
            gains = (1 - max(0.0, balance), 1 + min(0.0, balance))
            want = [motor_percent(strength, *ranges[side]) * gains[side] for side in (0, 1)]
            for side in (0, 1):
                if not want[side]:
                    kick_until[side] = 0.0
                elif not kick_until[side]:
                    kick_until[side] = now + kick_ms / 1000
            left, right = (100.0 if want[side] and now < kick_until[side] else want[side] for side in (0, 1))
            if left or right:
                rumble(path, left, right, 200)
            else:
                rumble(path, 0, 0, 0)
            left, right = want
            print(f"\rstrength: {strength:3.0f}% {strength_bar(strength)}   balance: {balance_text(balance):<16} "
                  f"{balance_bar(balance)}   [left: {left:3.0f}%  right: {right:3.0f}%]  ", end="", flush=True)
            time.sleep(0.02)
    except KeyboardInterrupt:
        print()
    finally:
        rumble(path, 0, 0, 0)
    print(f"strength {strength:.0f}%, balance: {balance_text(balance)} (left x{gains[0]:.2f}, right x{gains[1]:.2f})")


def rumble_test(path, first, seconds):
    """Play RUMBLE_TESTS from `first`: all of them, `seconds` apart, or (seconds
    None) Enter for the next, r again, a number for that one, b back, q quit."""
    def announce(i):
        print(f"{i + 1}/{len(RUMBLE_TESTS)}  {RUMBLE_TESTS[i][0]}", flush=True)
        play_rumble(path, RUMBLE_TESTS[i][1])

    try:
        if seconds is not None:
            for i in range(first, len(RUMBLE_TESTS)):
                announce(i)
                time.sleep(seconds)
            return
        print("Enter: next   r: again   b: back   number: that test   q: quit")
        i = first
        announce(i)
        while True:
            answer = input().strip().lower()
            if answer == "q":
                return
            n = i if answer == "r" else i - 1 if answer == "b" else int(answer) - 1 if answer.isdigit() else i + 1
            if 0 <= n < len(RUMBLE_TESTS):
                i = n
                announce(i)
            elif not answer and n == len(RUMBLE_TESTS):
                return
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        rumble(path, 0, 0, 0)


def unpair(path, slot):
    """Firmware clears the bond and generates a new address in the same commit."""
    if not 0 <= slot < 8:
        raise ValueError("Unpair requires a pad slot")
    identity = cmd(path, 0, length=64)
    if len(identity) < 56 or identity[:4] != b"C2PD" or identity[6] != BOARD_IDS["pad"]:
        raise ValueError("Unpair requires a pad")
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
                  f"{f['size']} numbers 0-{f['max']}, N,N,..." if f["type"] == T_LIST else
                  "palette ENTRY 0-15" if f["type"] == T_ENTRY else
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
        path = wait_for_device(30)
        if not path or ephemeral_now(path):
            sys.exit("padtool: the pad did not come back in normal mode")
    for b in batches:
        send(path, [(t, fid, bytes.fromhex(v)) for t, fid, v in b], journal=False)
    os.remove(JOURNAL)
    print(f"kept {len(batches)} changes")


EVENT = struct.Struct("@llHHi")
EV_SYN, EV_KEY = 0, 1
EVIOCSCLOCKID = 0x400445A0
CLOCK_MONOTONIC = 1


def gamepad_events():
    """(path, name) of the evdev nodes with gamepad or joystick buttons."""
    found = []
    for path in sorted(glob.glob("/dev/bwrap/event*") + glob.glob("/dev/input/event*")):
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        except OSError:
            continue
        try:
            keys = bytearray(96)
            fcntl.ioctl(fd, 0x80604521, keys)              # EVIOCGBIT(EV_KEY, 96)
            name = bytearray(64)
            fcntl.ioctl(fd, 0x80404506, name)              # EVIOCGNAME(64)
            if any(keys[code // 8] >> (code % 8) & 1 for code in (0x120, 0x130)):
                found.append((path, name.split(b"\0")[0].decode(errors="replace")))
        except OSError:
            pass
        finally:
            os.close(fd)
    return found


def pad_clock(path):
    """(host monotonic ns at the middle of the round trip, pad us, round trip ns)."""
    t0 = time.monotonic_ns()
    us, = struct.unpack("<I", cmd(path, 0x05, length=4)[:4])
    t1 = time.monotonic_ns()
    return (t0 + t1) // 2, us, t1 - t0


def quantiles(values):
    v = sorted(values)
    pick = lambda q: v[min(len(v) - 1, int(q * len(v)))]
    return {"p50": round(pick(.5), 3), "p90": round(pick(.9), 3), "p99": round(pick(.99), 3),
            "max": round(v[-1], 3)}


def percentiles(values):
    q = quantiles(values)
    return f"p50 {q['p50']:6.2f}  p90 {q['p90']:6.2f}  p99 {q['p99']:6.2f}  max {q['max']:6.2f} ms"


def latency(path, seconds, period_ms, event, csv_path=None, throughput=False):
    if not event:
        nodes = gamepad_events()
        if len(nodes) != 1:
            listed = "".join(f"\n  {p}  {n}" for p, n in nodes) or " none"
            sys.exit(f"padtool: give the host's gamepad node with --event; found:{listed}")
        event = nodes[0][0]
    fd = os.open(event, os.O_RDONLY | os.O_NONBLOCK)
    fcntl.ioctl(fd, EVIOCSCLOCKID, struct.pack("i", CLOCK_MONOTONIC))
    if throughput:
        period_ms, edges = 0, int(seconds * 2000)     # more than the link can take: stopped by time
        print(f"A flips in every report the pad sends, for {seconds:g} s; host events from {event}")
    else:
        edges = max(2, int(seconds * 1000 / period_ms))
        print(f"{edges} A edges, one per {period_ms} ms at random times; host events from {event}")

    syncs, records, events = [], [], []
    def sync(n=8):                     # the tightest of n round trips
        syncs.append(min((pad_clock(path) for _ in range(n)), key=lambda s: s[2]))
    def drain():
        try:
            while True:
                data = os.read(fd, EVENT.size * 64)
                for i in range(0, len(data) - EVENT.size + 1, EVENT.size):
                    sec, usec, kind, code, value = EVENT.unpack_from(data, i)
                    events.append((sec * 10**9 + usec * 1000, kind, code, value))
        except BlockingIOError:
            pass
    def take():
        while True:
            d = scsi(path, bytes([OP, 0x1C, 0]), 12 + 12 * 16)
            count, lost, active = struct.unpack_from("<IIB", d)
            for i in range(count):
                records.append(struct.unpack_from("<IIB", d, 12 + 12 * i))
            if count < 16:
                return lost, active

    sync()
    drain()
    events.clear()
    scsi(path, bytes([OP, 0x1C, 1]) + struct.pack("<IH", edges, period_ms), 0)
    started = time.monotonic_ns()
    next_sync = time.monotonic() + 1
    try:
        while True:
            time.sleep(0.05)
            drain()
            lost, active = take()
            if time.monotonic() >= next_sync:
                sync()
                next_sync += 1
            if not active:
                break
            if throughput and time.monotonic_ns() - started >= seconds * 1e9:
                scsi(path, bytes([OP, 0x1C, 2]), 0)
                time.sleep(0.05)
                lost, active = take()
                break
    except KeyboardInterrupt:
        scsi(path, bytes([OP, 0x1C, 2]), 0)
        lost, _ = take()
    time.sleep(0.3)                    # the last edges reach the host
    drain()
    sync()
    ended = time.monotonic_ns()
    os.close(fd)

    # pad us -> host ns: the 32-bit clock unwrapped, a straight line through the syncs
    def unwrap(us, ref):
        return ref + ((us - ref) & 0xFFFFFFFF) - (1 << 32 if (us - ref) & 0x80000000 else 0)
    ref = syncs[0][1]
    xs = [unwrap(s[1], ref) for s in syncs]
    ys = [s[0] for s in syncs]
    mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx if sxx else 1000.0
    to_host = lambda us: my + slope * (unwrap(us, ref) - mx)
    residual = max(abs(y - to_host(s[1])) for y, s in zip(ys, syncs)) / 1e6
    rtt = min(s[2] for s in syncs) / 1e6

    keys = collections.Counter(code for _, kind, code, _ in events if kind == EV_KEY)
    if not keys:
        sys.exit("padtool: no key events reached the host (is the slot connected through this node?)")
    code = keys.most_common(1)[0][0]
    presses = [(t, v) for t, kind, c, v in events if kind == EV_KEY and c == code and v in (0, 1)]
    reports = sum(1 for t, kind, c, _ in events if kind == EV_SYN and c == 0 and started <= t <= ended)

    pad_ms, air_ms, total_ms, rows = [], [], [], []
    alone = [r for r in records if r[1]]
    # The host sees an edge only when it changes what the host last got: after
    # an edge undone in the pad, the next one is no change to it.
    expected, host_level = [], 0
    for r in alone:
        if r[2] != host_level:
            expected.append(r)
            host_level = r[2]
    j = 0
    for i, (edge_us, sent_us, level) in enumerate(expected):
        t_edge = to_host(edge_us)
        # The next edge of this level takes the events after its own time: an
        # edge whose event never came must not take that one.
        until = t_edge + 100e6
        if i + 2 < len(expected):
            until = min(until, to_host(expected[i + 2][0]))
        while j < len(presses) and presses[j][0] < t_edge - 2e6:
            j += 1
        k = j
        while k < len(presses) and presses[k][0] < until and presses[k][1] != level:
            k += 1
        if k < len(presses) and presses[k][0] < until:
            total = (presses[k][0] - t_edge) / 1e6
            pad = ((sent_us - edge_us) & 0xFFFFFFFF) / 1000
            pad_ms.append(pad)
            total_ms.append(total)
            air_ms.append(total - pad)
            rows.append(((t_edge - started) / 1e6, level, pad, total))
            j = k + 1
    if csv_path:
        with open(csv_path, "w") as f:
            f.write("edge_ms,level,pad_ms,total_ms\n")
            f.writelines(f"{e:.3f},{l},{p:.3f},{t:.3f}\n" for e, l, p, t in rows)
            f.write("# probe records: edge_ms,sent_ms,level\n")
            f.writelines(f"# {(to_host(e) - started) / 1e6:.3f},"
                         f"{(to_host(t) - started) / 1e6 if t else 0:.3f},{l}\n" for e, t, l in records)
            f.write("# host events: t_ms,code,value\n")
            f.writelines(f"# {(t - started) / 1e6:.3f},{c:#x},{v}\n" for t, k, c, v in events if k == EV_KEY)
    span = (ended - started) / 1e9
    print(f"clock: round trip {rtt:.2f} ms, fit within {residual:.2f} ms, pad clock {slope / 1000:.6f}x")
    print(f"edges: {len(records)} made, {len(alone)} sent in a report of their own "
          f"({len(records) - len(alone)} undone first), {len(total_ms)} of the {len(expected)} "
          f"changes it should see seen by the host"
          f"{f', {lost} records overwritten' if lost else ''}")
    if throughput:
        sent = sorted(to_host(r[1]) for r in alone)
        seen = [t for t, v in presses if started <= t <= ended]
        window = (sent[-1] - sent[0]) / 1e9 if len(sent) > 1 else span
        gaps = [(b - a) / 1e6 for a, b in zip(seen, seen[1:])]
        print(f"pad: {len(sent)} reports sent, {len(sent) / window:.0f}/s; "
              f"host: {len(seen)} A changes seen, {len(seen) / window:.0f}/s "
              f"({max(0, len(sent) - len(seen))} lost or merged)")
        if gaps:
            print(f"host gaps between reports            {percentiles(gaps)}")
        return {"mode": "throughput", "seconds": seconds, "event": event,
                "clock_rtt_ms": round(rtt, 3), "clock_fit_ms": round(residual, 3),
                "pad_reports": len(sent), "pad_rate": round(len(sent) / window, 1),
                "host_reports": len(seen), "host_rate": round(len(seen) / window, 1),
                "lost": max(0, len(sent) - len(seen)), "host_gap_ms": quantiles(gaps) if gaps else None}
    print(f"pad: {len(alone) / span:.0f} reports/s sent with a new edge; "
          f"host: {reports / span:.0f} reports/s with a change, key {code:#x}")
    if period_ms < 10:
        print("latency: not matched at a period this short (edges closer than the latency); "
              "use --period 10 or more")
    elif total_ms:
        print(f"pad    edge -> report sent    {percentiles(pad_ms)}")
        print(f"air    report -> host event   {percentiles(air_ms)}")
        print(f"total  edge -> host event     {percentiles(total_ms)}")
    matched = period_ms >= 10 and total_ms
    return {"mode": "latency", "seconds": seconds, "period_ms": period_ms, "event": event,
            "clock_rtt_ms": round(rtt, 3), "clock_fit_ms": round(residual, 3),
            "edges": len(records), "alone": len(alone), "expected": len(expected),
            "seen": len(total_ms), "pad_rate": round(len(alone) / span, 1),
            "host_rate": round(reports / span, 1),
            "pad_ms": quantiles(pad_ms) if matched else None,
            "air_ms": quantiles(air_ms) if matched else None,
            "total_ms": quantiles(total_ms) if matched else None}


def tool(paths):
    """Feature report 0xf5 "tool": a USB personality (src/core/pad.h) ends and the
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


def hub_pair(slot):
    """Pair a dongle slot: SCSI to the tool device, else feature report 0xf5
    "pair" SLOT to a dongle that is the hub (link.h, hub mode)."""
    if not 0 <= slot < 6:
        sys.exit("padtool: dongle slot must be 0-5")
    if DEVICE and is_path(DEVICE) or matching():
        cmd(need_device(), 0x17, slot)
        return
    report = b"\xf5pair" + bytes([slot])
    hidiocsfeature = 0xC0004806 | (len(report) << 16)
    for path in sorted(glob.glob("/dev/bwrap/hidraw*") + glob.glob("/dev/hidraw*")):
        uevent = f"/sys/class/hidraw/{os.path.basename(path)}/device/uevent"
        try:
            with open(uevent) as f:
                if "HID_ID=0003:00001209:00000001" not in f.read():
                    continue
            with open(path, "rb+", buffering=0) as f:
                fcntl.ioctl(f, hidiocsfeature, bytearray(report))
        except OSError:
            continue
        print(f"dongle hub {path}: slot {slot} pairing")
        return
    sys.exit("padtool: no dongle found, as the tool device or the hub")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command", choices=["tool-only", "hub", "find", "identify", "status", "log", "stats", "calibrate", "led-demo", "rumble", "rumble-test", "rumble-sticks", "input", "reboot", "loader", "update", "relink", "connect", "disconnect", "pair", "unpair", "profile", "peek", "trace",
                                       "fields", "get", "set", "reset", "backup-settings",
                                       "restore-settings", "factory-reset", "ephemeral", "keep", "tool", "latency"])
    p.add_argument("args", nargs="*")
    p.add_argument("--calibration", action="store_true", help="restore-settings: calibration too")
    p.add_argument("-f", "--follow", action="store_true")
    p.add_argument("-w", "--watch", type=float, metavar="SECONDS", help="stats: show and reset every N seconds")
    p.add_argument("--reset", action="store_true", help="stats: reset the counters after reading")
    p.add_argument("--hz", type=int, default=2000, help="profile: samples per second")
    p.add_argument("--seconds", type=float, help="led-demo: show each this long, then the next; rumble: how long (1);"
                   " rumble-test: play them all, this long apart")
    p.add_argument("-d", "--device", help="which pad or dongle, when several are connected: its path, "
                   "USB port (5-2.1), or part of its board name (nova) or flash ID (padtool find)")
    def percent_range(text):
        try:
            low, high = (int(v) for v in text.split("-"))
        except ValueError:
            raise argparse.ArgumentTypeError("MIN-MAX, percent, e.g. 25-60")
        if not 0 <= low <= high <= 100:
            raise argparse.ArgumentTypeError("MIN-MAX with 0 <= MIN <= MAX <= 100")
        return low, high
    p.add_argument("--step", type=int, choices=range(1, 6), default=RUMBLE_STEP_DEFAULT,
                   help="rumble-sticks: the pad's ranges and kick for this rumble step")
    p.add_argument("--left", type=percent_range, metavar="MIN-MAX",
                   help="rumble-sticks: left motor percent range")
    p.add_argument("--right", type=percent_range, metavar="MIN-MAX",
                   help="rumble-sticks: right motor percent range")
    p.add_argument("--kick", type=int, metavar="MS",
                   help="rumble-sticks: full power this long when a motor starts")
    p.add_argument("--full", action="store_true", help="rumble-sticks: both motors 0-100, no kick")
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
    p.add_argument("--period", type=int, default=20, help="latency: one A edge per this many ms")
    p.add_argument("--throughput", action="store_true",
                   help="latency: A flips in every report the pad sends: the most reports per second")
    p.add_argument("--json", help="latency: append the results, as one JSON line, to this file")
    p.add_argument("--label", help="latency --json: a note stored with the results (e.g. the dongle)")
    p.add_argument("--csv", help="latency: write each edge's times (and the host's key events) here")
    p.add_argument("--event", help="latency: the host's evdev node for the slot (default: the only gamepad)")
    a = p.parse_args()
    global DEV, BOARD, DEVICE
    DEV = a.dev
    DEVICE = a.device
    BOARD = a.board
    if a.elf is None:
        a.elf = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "gamesir-cyclone-2-" + BOARD, "app.elf")
    if BOARD == "dongle" and a.command in {
        "stats", "latency", "calibrate", "led-demo", "rumble", "rumble-test", "rumble-sticks", "input", "relink", "connect", "disconnect", "fields", "get", "set", "reset",
        "backup-settings", "restore-settings", "factory-reset", "keep",
    }:
        p.error(f"{a.command} is not supported by dongle firmware")
    if a.command == "tool":
        tool(a.args)
        return
    if a.command == "tool-only" and (BOARD != "dongle" or len(a.args) > 1 or a.args and a.args[0] not in ("on", "off")):
        p.error("tool-only requires --board dongle and on, off or nothing")
    if a.command == "hub" and (BOARD != "dongle" or len(a.args) > 1 or a.args and a.args[0] not in ("on", "off")):
        p.error("hub requires --board dongle and on, off or nothing")
    if a.command == "tool-only":
        mode = cmd(need_device(), 0x31, {"on": 1, "off": 0}.get(a.args[0] if a.args else "", 0xff), length=1)
        print(f"dongle: tool only {'on (until power loss)' if mode and mode[0] else 'off'}")
    elif a.command == "hub":
        mode = cmd(need_device(), 0x32, {"on": 1, "off": 0}.get(a.args[0] if a.args else "", 0xff), length=1)
        print(f"dongle: hub mode {'on' if mode and mode[0] else 'off'}")
    elif a.command == "find":
        for path, _ in devices():
            print(f"{BOARD} {path}  {identify(path)}  {status(path)}")
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
    elif a.command == "latency":
        path = need_device()
        result = latency(path, a.seconds or 10, a.period, a.event, a.csv, a.throughput)
        if a.json and result:
            result.update(time=time.strftime("%Y-%m-%dT%H:%M:%S"), pad=identify(path),
                          label=a.label or "")
            with open(a.json, "a") as f:
                f.write(json.dumps(result) + "\n")
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
        if BOARD != "pad" or len(a.args) != 1 or slot_number(a.args[0]) is None:
            p.error("unpair requires a pad slot: slot-a ... slot-right (or A B X Y up down left right)")
        try:
            unpair(need_device(), slot_number(a.args[0]))
        except ValueError as error:
            p.error(str(error))
    elif a.command == "pair":
        if BOARD == "dongle":
            if len(a.args) > 1 or a.args and not a.args[0].isdigit():
                p.error("pair: a dongle slot number (0: its own pad)")
            hub_pair(int(a.args[0]) if a.args else 0)
            return
        if len(a.args) == 1 and slot_number(a.args[0]) is not None:
            slot = slot_number(a.args[0])
        else:
            p.error("pair requires a slot: slot-a ... slot-right (or A B X Y up down left right)")
        cmd(need_device(), 0x17, slot)
    elif a.command == "connect":
        slot = slot_number(a.args[0]) if a.args else 0xfe
        if slot is None:
            p.error("connect: a slot: slot-a ... slot-right (or A B X Y up down left right)")
        cmd(need_device(), 0x13, slot)
    elif a.command == "disconnect":
        if a.args:
            p.error("disconnect takes no arguments")
        cmd(need_device(), 0x1b)
    elif a.command == "relink":
        cmd(need_device(), 0x12)
    elif a.command == "led-demo":
        path = need_device()
        names = led_demo_names(path)
        if a.args and a.args[0] == "list":
            for i, name in enumerate(names):
                print(f"{i + 1:3}  {name}")
            return
        if a.args and (not a.args[0].isdigit() or not 1 <= int(a.args[0]) <= len(names)):
            p.error(f"led-demo: a number 1-{len(names)}, or list")
        led_demo(path, names, int(a.args[0]) - 1 if a.args else 0, a.seconds)
    elif a.command == "rumble-test" and a.args[:1] == ["list"]:
        for i, (name, _) in enumerate(RUMBLE_TESTS):
            print(f"{i + 1:3}  {name}")
    elif a.command in ("input", "rumble-sticks"):
        path = need_device()
        if a.command == "input":
            show_input(path, a.follow)
        elif a.args:
            p.error("rumble-sticks takes no arguments, only options")
        else:
            if a.full:
                left, right, kick = (0, 100), (0, 100), 0
            else:
                left, right, kick = rumble_step_settings(path, a.step)
            rumble_sticks(path, (a.left or left, a.right or right), kick if a.kick is None else a.kick)
    elif a.command in ("rumble", "rumble-test"):
        path = need_device()
        if a.command == "rumble-test":
            if a.args and (not a.args[0].isdigit() or not 1 <= int(a.args[0]) <= len(RUMBLE_TESTS)):
                p.error(f"rumble-test: a number 1-{len(RUMBLE_TESTS)}, or list")
            rumble_test(path, int(a.args[0]) - 1 if a.args else 0, a.seconds)
            return
        try:
            levels = [float(v) for v in a.args]
        except ValueError:
            levels = []
        if not 1 <= len(levels) <= 2 or not all(0 <= v <= 100 for v in levels):
            p.error("rumble LEFT [RIGHT]: percent, 0-100")
        left, right = levels[0], levels[-1]
        ms = (a.seconds if a.seconds is not None else 1) * 1000
        if not left and not right:
            rumble(path, 0, 0, 0)
            return
        if not 0 < ms <= RUMBLE_MAX_MS:
            p.error("rumble: --seconds above 0, at most 10")
        duty = rumble(path, left, right, ms)
        print(f"left {duty[0]}/255  right {duty[1]}/255  for {ms / 1000:g} s")
    elif a.command == "calibrate":
        path = need_device()
        cmd(path, 0x18)
        print("Leave sticks and triggers untouched for 2.5 seconds; then circle both sticks and press both triggers fully. A or B saves; Home cancels.")
    elif a.command in ("reboot", "loader"):
        cmd(need_device(), 0x10 if a.command == "reboot" else 0x11)
    elif a.command == "update":
        if len(a.args) != 1:
            sys.exit("padtool update FILE")
        with open(a.args[0], "rb") as f:
            update(need_device(), f.read())
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
