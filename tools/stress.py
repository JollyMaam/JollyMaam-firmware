#!/usr/bin/env python3
"""Stress pads' links to this PC: connect/disconnect cycles, relinks, host
drops, reboots, latency and long runs, then write a report.

    tools/stress.py PLAN.json
    tools/stress.py --target pad=5-2.2,link=bluetooth,slot=south,type=generic \\
                    --test connect:count=20 --test latency:seconds=30
    tools/stress.py --example        # an example plan to start from

A target is one way a pad reaches the PC:
    name     a label for the report (default: pad-link)
    pad      USB port (5-2.2, from `padtool find`) or a part of the pad's name
    link     bluetooth (this PC's adapter), dongle (our dongle on this PC's USB)
             or usb (the pad's own cable, a USB personality)
    slot     the pad slot (south, east, ...; default south)
    type     the slot's radio_type (bluetooth, dongle) or usb_type (usb);
             default: what the slot has
    dongle   the dongle's USB port or name (link dongle; default: the only one)
    set      more slot settings, NAME=VALUE;NAME=VALUE (developer ones too)

A test is a name and its parameters (all optional):
    connect     count=10 hold=3 gap=2 timeout=20   connect, take reports for
                HOLD s, disconnect, wait GAP s; times the host's first report
    relink      count=10 timeout=20               drop and remake the link
                while connected (padtool relink); times the outage (over
                the dongle: as the pad's state shows it)
    host-drop   count=5 timeout=20                bluetooth: the PC ends the
                link; what the pad does, then a reconnect
    reboot      count=3 timeout=30                restart the pad, set it up
                again, connect once
    latency     seconds=30 period=20 max_p99=     end-to-end latency with the
                pad's probe (pads whose build has it; not over link usb)
    throughput  seconds=10                        the most reports/s the link takes
    soak        seconds=300 stall=100 drive=auto period=20   connected for
                SECONDS: report rate, gaps longer than STALL ms, links lost
                (reconnected).  drive=probe: the latency probe makes an A edge
                every PERIOD ms, a steady stream (and its latency over the
                whole run); idle: nothing pressed (a profile that sends only
                on change then has gaps of its own, so only lost links fail);
                auto: probe where the pad has it

Every pad and dongle used is put in ephemeral mode first, its slot changed in
RAM only, and rebooted at the end, so nothing is kept.  Bluetooth targets
need a bond with this PC: when the slot has none, the pad is paired with
BlueZ (bluetoothctl), and the PC forgets that pairing at the end.
Instrumentation is kept low: while a test runs only the host side is
watched (the hidraw node: every report and its time); the pad is asked over
USB only between cycles, on a failure, and by the latency probe.

Results go to build/stress/<time>/: events.jsonl (one line per cycle),
report.md (the summary, also printed) and each pad's log at the end.
"""
import argparse
import glob
import json
import os
import re
import select
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
PADTOOL = [sys.executable, os.path.join(HERE, "padtool", "padtool.py")]
DEV_ROOT = os.environ.get("DEV_ROOT", "")
TOOL_ID = "000016C0:000005DF"          # the tool device (pad.h); not a host node

EXAMPLE = {
    "targets": [
        {"name": "c2-bt", "pad": "Cyclone 2", "link": "bluetooth", "slot": "south", "type": "generic"},
        {"name": "c2-dongle", "pad": "Cyclone 2", "link": "dongle", "slot": "west", "type": "generic"},
        {"name": "c2-usb", "pad": "Cyclone 2", "link": "usb", "slot": "south", "type": "generic"},
        {"name": "u2cw-bt", "pad": "Ultimate 2C Wireless", "link": "bluetooth", "slot": "east"},
        {"name": "u2cb-bt", "pad": "Ultimate 2C Bluetooth", "link": "bluetooth", "slot": "east", "type": "generic"},
        {"name": "u2cb-usb", "pad": "Ultimate 2C Bluetooth", "link": "usb", "slot": "east", "type": "generic"},
    ],
    "tests": [
        {"test": "connect", "count": 10, "hold": 3},
        {"test": "relink", "count": 5},
        {"test": "host-drop", "count": 3},
        {"test": "latency", "seconds": 20},
        {"test": "throughput", "seconds": 10},
        {"test": "soak", "seconds": 120},
    ],
}
DEFAULTS = {
    "connect": {"count": 10, "hold": 3, "gap": 2, "timeout": 20},
    "relink": {"count": 10, "timeout": 20},
    "host-drop": {"count": 5, "timeout": 20},
    "reboot": {"count": 3, "timeout": 30},
    "latency": {"seconds": 30, "period": 20, "max_p99": None},
    "throughput": {"seconds": 10},
    "soak": {"seconds": 300, "stall": 100, "drive": "auto", "period": 20},
}
LINKS = ("bluetooth", "dongle", "usb")


class Skip(Exception):
    """A test that does not apply to a target."""


class Fail(Exception):
    """A target that cannot go on (its pad is gone, its set-up failed)."""


def now():
    return time.monotonic()


def say(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


# ---- padtool and bluetoothctl ------------------------------------------------

def padtool(*args, timeout=60):
    """(return code, output) of a padtool command; never raises."""
    try:
        r = subprocess.run(PADTOOL + [str(a) for a in args], capture_output=True, text=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr).strip()
    except subprocess.TimeoutExpired:
        return -1, f"padtool {' '.join(map(str, args))}: timed out"


def must(*args, timeout=60):
    rc, out = padtool(*args, timeout=timeout)
    if rc:
        raise Fail(f"padtool {' '.join(map(str, args))}: {out.splitlines()[-1] if out else rc}")
    return out


def find_devices():
    """{port: {"board": pad|dongle, "name", "build", "line"}} of everything running our firmware."""
    found = {}
    for board in ("pad", "dongle"):
        rc, out = padtool("--board", board, "find")
        for line in out.splitlines():
            m = re.match(r"(\w+) (\S+)\s+(.+?), usb (\S+) .* build (\S+)", line)
            if m:
                found[m.group(4)] = {"board": m.group(1), "name": m.group(3), "build": m.group(5),
                                     "line": line}
    return found


def status(dev):
    """{"state", "slot", "uptime"} of a pad or dongle on its tool device, or None."""
    rc, out = padtool(*dev.args, "status", timeout=15)
    m = re.search(r"state (\S+) slot (\S+).*uptime ([0-9.]+)", out)
    if rc or not m:
        return None
    return {"state": m.group(1), "slot": m.group(2), "uptime": float(m.group(3))}


def wait_status(dev, want, timeout):
    """Poll until the state is one of WANT (any state, for None); the last status."""
    end, s = now() + timeout, None
    while now() < end:
        s = status(dev)
        if s and (want is None or s["state"] in want):
            return s
        time.sleep(0.3)
    return s


def bluetoothctl(*args, timeout=15):
    try:
        r = subprocess.run(["bluetoothctl", *args], capture_output=True, text=True, timeout=timeout)
        return re.sub(r"\x1b\[[0-9;]*[A-Za-z]|\r", "", r.stdout)
    except (subprocess.TimeoutExpired, FileNotFoundError) as e:
        return str(e)


def bluez_paired(addr):
    return "Paired: yes" in bluetoothctl("info", addr)


def pc_address():
    m = re.search(r"Controller (\S+)", bluetoothctl("show"))
    return m.group(1).lower() if m else None


def bluez_pair(addr, le, timeout=30):
    """Scan for ADDR and pair it with a NoInputNoOutput agent; True when it paired."""
    p = subprocess.Popen(["bluetoothctl"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, bufsize=1)
    out = []
    threading.Thread(target=lambda: out.extend(p.stdout), daemon=True).start()
    send = lambda line: (p.stdin.write(line + "\n"), p.stdin.flush())
    seen = lambda pat: re.search(pat, re.sub(r"\x1b\[[0-9;]*[A-Za-z]|\r", "", "".join(out)), re.I)
    ok = False
    try:
        send("agent NoInputNoOutput")
        send("default-agent")
        send("scan " + ("le" if le else "bredr"))
        end = now() + timeout
        while now() < end and not seen(rf"Device {addr}") and "Name" not in bluetoothctl("info", addr):
            time.sleep(0.5)
        send("scan off")
        time.sleep(0.5)
        send(f"pair {addr}")
        end = now() + timeout
        while now() < end:
            if seen(r"Pairing successful"):
                ok = True
                break
            if seen(r"Failed to pair|AuthenticationFailed|ConnectionAttemptFailed|not available"):
                break
            time.sleep(0.3)
        send(f"trust {addr}")
        time.sleep(0.5)
        send("quit")
        p.wait(timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        p.kill()
    return ok


# ---- the host side -----------------------------------------------------------

def dev_node(name):
    return os.path.join(DEV_ROOT or "/dev", name)


def hid_nodes():
    """Every hidraw node: {"name", "sys" (resolved), "id", "uniq"}."""
    nodes = []
    for sys_path in glob.glob("/sys/class/hidraw/hidraw*"):
        try:
            with open(sys_path + "/device/uevent") as f:
                ev = dict(line.rstrip("\n").split("=", 1) for line in f if "=" in line)
        except OSError:
            continue
        nodes.append({"name": os.path.basename(sys_path), "sys": os.path.realpath(sys_path + "/device"),
                      "id": ev.get("HID_ID", ""), "uniq": ev.get("HID_UNIQ", "").lower()})
    return nodes


def event_node(node):
    """The evdev node beside a hidraw node (padtool latency reads it)."""
    for path in sorted(glob.glob(node["sys"] + "/input/input*/event*")):
        return dev_node(os.path.basename(path))
    return None


class Host:
    """Every report the PC gets from one target, across links: a thread finds
    its hidraw node (again after it went away) and stamps each report."""

    def __init__(self, match):
        self.match = match
        self.times = []                  # monotonic time of every report
        self.node = None                 # the node being read
        self.attached = []               # (time, node name) each time one was found
        self.lost = []                   # times a node went away
        self.lock = threading.Lock()
        self.running = True
        threading.Thread(target=self.run, daemon=True).start()

    def find(self):
        for n in hid_nodes():
            if self.match(n):
                return n
        return None

    def run(self):
        fd = None
        while self.running:
            if fd is None:
                n = self.find()
                if not n:
                    time.sleep(0.02)
                    continue
                try:
                    fd = os.open(dev_node(n["name"]), os.O_RDONLY | os.O_NONBLOCK)
                except OSError:
                    time.sleep(0.05)
                    continue
                with self.lock:
                    self.node = n
                    self.attached.append((now(), n["name"]))
            select.select([fd], [], [], 0.05)
            try:
                while True:
                    os.read(fd, 1024)
                    t = now()
                    with self.lock:
                        self.times.append(t)
            except BlockingIOError:
                pass
            except OSError:              # the node went away
                os.close(fd)
                fd = None
                with self.lock:
                    self.node = None
                    self.lost.append(now())
        if fd is not None:
            os.close(fd)

    def stop(self):
        self.running = False

    def since(self, t):
        with self.lock:
            return [x for x in self.times if x >= t]

    def first_after(self, t, timeout, attach=False):
        """Time of the first report after T, waiting up to TIMEOUT s from now; None.
        ATTACH: or of the node appearing (see attached_after)."""
        end = now() + timeout
        while now() < end:
            with self.lock:
                after = [x for x in self.times[-200:] if x >= t]
            if attach:
                after = sorted(after + self.attached_after(t))
            if after:
                return after[0]
            time.sleep(0.01)
        return None

    def attached_after(self, t):
        """Times its node appeared after T.  Over Bluetooth a profile that
        sends only on change sends its first report as the host's HID link
        opens, before this reader can open the node BlueZ makes then, and an
        idle pad sends no other: the node's appearance is the connection."""
        with self.lock:
            return [x for x, _ in self.attached if x >= t]

    def quiet(self, t, quiet_s, timeout):
        """Wait until its node is gone, or no report came for QUIET_S s; the time or None."""
        end = now() + timeout
        while now() < end:
            with self.lock:
                last = self.times[-1] if self.times else 0
                gone = self.node is None
            if gone or now() - max(last, t) >= quiet_s:
                return now()
            time.sleep(0.05)
        return None


def gaps_ms(times):
    return [(b - a) * 1000 for a, b in zip(times, times[1:])]


def quantiles(values):
    if not values:
        return None
    v = sorted(values)
    pick = lambda q: v[min(len(v) - 1, int(q * len(v)))]
    return {"p50": round(pick(.5), 2), "p90": round(pick(.9), 2), "p99": round(pick(.99), 2),
            "max": round(v[-1], 2), "n": len(v)}


def stream_stats(times, span):
    g = gaps_ms(times)
    return {"rate": round(len(times) / span, 1) if span > 0 else 0,
            "gap_ms": quantiles(g)}


# ---- devices and targets -----------------------------------------------------

class Device:
    """A pad or dongle running our firmware, by its USB port."""

    def __init__(self, port, info):
        self.port, self.info = port, info
        self.dongle = info["board"] == "dongle"
        self.args = (["--dongle"] if self.dongle else []) + ["-d", port]
        self.ephemeral = False

    def __str__(self):
        return f"{self.info['name']} [{self.port}]"


class Target:
    def __init__(self, spec, devices):
        self.spec = spec
        self.link = spec.get("link", "bluetooth")
        if self.link not in LINKS:
            raise SystemExit(f"stress: link must be one of {', '.join(LINKS)}: {spec}")
        self.pad = pick(devices, spec.get("pad"), "pad")
        self.dongle = pick(devices, spec.get("dongle"), "dongle") if self.link == "dongle" else None
        self.slot = spec.get("slot", "south")
        self.type = spec.get("type")
        self.set = dict(kv.split("=", 1) for kv in str(spec.get("set", "")).split(";") if "=" in kv)
        self.name = spec.get("name") or f"{self.pad.info['name'].lower().replace(' ', '')}-{self.link}"
        self.address = None
        self.host = None
        self.paired_here = set()         # addresses this run paired with BlueZ

    def match(self, node):
        if TOOL_ID in node["id"]:
            return False
        if self.link == "bluetooth":
            return bool(self.address) and node["uniq"] == self.address
        port = (self.dongle or self.pad).port
        return f"/{port}:" in node["sys"]


def pick(devices, want, board):
    found = [d for d in devices.values() if d.info["board"] == board
             and (not want or want == d.port or want.lower() in d.info["name"].lower())]
    if len(found) != 1:
        listed = ", ".join(str(d) for d in devices.values() if d.info["board"] == board) or "none"
        raise SystemExit(f"stress: {board} {want or ''}: {'no match' if not found else 'more than one'} "
                         f"(found: {listed})")
    return found[0]


def slot_values(t):
    out = must(*t.pad.args, "get", t.slot)
    values = {}
    for line in out.splitlines():
        m = re.match(r"\S+\s+(\S+)\s+\*?\s*(.*)$", line)
        if m:
            values[m.group(1)] = m.group(2).strip()
    return values


def ephemeral(dev):
    if not dev.ephemeral:
        out = must(*dev.args, "ephemeral")
        if "ephemeral" not in out:
            raise Fail(f"{dev}: ephemeral mode not confirmed: {out}")
        dev.ephemeral = True


def dongle_tool(t, timeout=10):
    """The dongle back as its tool device (it is the pad's type while one is linked)."""
    if status(t.dongle):
        return True
    padtool(*t.pad.args, "disconnect")
    end = now() + timeout
    while now() < end:
        if status(t.dongle):
            return True
        time.sleep(1)
    return False


def setup(t, log):
    """Ephemeral mode, the slot as the target wants it (RAM), the bond it needs; idle."""
    if not status(t.pad):
        raise Fail(f"{t.pad}: no answer on USB")
    ephemeral(t.pad)
    want = dict(t.set)
    if t.link == "usb":
        if t.type:
            want["usb_type"] = t.type
    else:
        want["radio"] = "bluetooth" if t.link == "bluetooth" else "dongle"
        want["usb_type"] = "none"        # the cable must not win over the radio
        if t.type:
            want["radio_type"] = t.type
    have = slot_values(t)
    changes = [x for k, v in want.items() if have.get(k) != v for x in (k, v)]
    if changes:
        must(*t.pad.args, "set", t.slot, *changes)
        have = slot_values(t)
    if t.link == "usb" and have.get("usb_type", "none") == "none":
        raise Fail(f"{t.name}: slot {t.slot} has no usb_type; give type")
    t.address = have.get("address", "").lower()
    if t.link == "bluetooth":
        pc = (pc_address() or "").replace(":", "")
        pc_rev = "".join(reversed([pc[i:i + 2] for i in range(0, len(pc), 2)]))
        bonded = pc_rev and pc_rev in have.get("bond", "").lower()
        if bonded and bluez_paired(t.address):
            bluetoothctl("trust", t.address)
        else:
            rtype = have.get("radio_type", "")
            le = rtype.endswith("-ble") or rtype == "switch2"
            log("pair", f"{t.name}: pairing {t.address} ({'LE' if le else 'Classic'}) with this PC")
            bluetoothctl("remove", t.address)
            must(*t.pad.args, "pair", t.slot)
            if not bluez_pair(t.address, le):
                raise Fail(f"{t.name}: BlueZ did not pair with {t.address}")
            t.paired_here.add(t.address)
            time.sleep(2)
            padtool(*t.pad.args, "disconnect")
    elif t.link == "dongle":
        if not dongle_tool(t):
            raise Fail(f"{t.dongle}: no answer on USB")
        ephemeral(t.dongle)
        log("pair", f"{t.name}: pairing slot {t.slot} with {t.dongle}")
        # The dongle first: a pad made pairable while the dongle still waits on
        # an old bond links on that bond, and the dongle's pair then ends it.
        must(*t.dongle.args, "pair")
        time.sleep(1)
        t0 = now()
        must(*t.pad.args, "pair", t.slot)
        if not t.host.first_after(t0, 30):
            raise Fail(f"{t.name}: the pad did not link to the dongle ({status(t.pad)})")
        time.sleep(1)
        padtool(*t.pad.args, "disconnect")
        dongle_tool(t)
    idle(t)


def idle(t, timeout=15):
    """The pad on its tool device and not in a session."""
    if t.link == "usb" and not status(t.pad):
        tool_back(t)
    s = status(t.pad)
    if s and s["state"] != "idle":
        padtool(*t.pad.args, "disconnect")
        s = wait_status(t.pad, ("idle",), timeout)
    if not s or s["state"] != "idle":
        raise Fail(f"{t.name}: the pad is not idle: {s}")
    if t.dongle:
        dongle_tool(t)
    return s


def tool_back(t, timeout=10):
    """A pad in a USB session back to its tool device (feature report via its hidraw)."""
    end = now() + timeout
    while now() < end:
        nodes = [n for n in hid_nodes() if t.match(n)]
        if nodes:
            padtool("tool", *[dev_node(n["name"]) for n in nodes])
        if wait_status(t.pad, None, 3):
            return True
    return False


# ---- one connect, one disconnect ---------------------------------------------

def connect(t, timeout):
    """Start the slot's session; (seconds to the host's first report, or None, padtool output)."""
    t0 = now()
    rc, out = padtool(*t.pad.args, "connect", t.slot)
    if rc and t.link != "usb":       # a USB session leaves USB before padtool hears back
        return None, out
    first = t.host.first_after(t0, timeout, attach=t.link == "bluetooth")
    return (first - t0 if first else None), out


def disconnect(t, timeout=10):
    """End the session; seconds until the host stopped hearing it and the pad is idle, or None."""
    t0 = now()
    if t.link == "usb":
        if not tool_back(t, timeout):
            return None
    else:
        padtool(*t.pad.args, "disconnect")
    if t.host.quiet(t0, 0.5, timeout) is None:
        return None
    s = wait_status(t.pad, ("idle",), timeout)
    if not s or s["state"] != "idle":
        return None
    return now() - t0


def diagnose(t):
    """What the pad says after a failure: its state and last log lines."""
    s = status(t.pad)
    rc, log = padtool(*t.pad.args, "log", timeout=20)
    return {"status": s, "log": log.splitlines()[-12:] if not rc else []}


def recover(t, log):
    try:
        idle(t, timeout=20)
    except Fail as e:
        log("recover", f"{t.name}: {e}; setting it up again")
        setup(t, log)


# ---- the tests -----------------------------------------------------------------

def test_connect(t, p, rec, log):
    for i in range(p["count"]):
        secs, out = connect(t, p["timeout"])
        if secs is None:
            rec(cycle=i, ok=False, why="no report from the host", padtool=out[-200:], **diagnose(t))
            recover(t, log)
            continue
        t1 = now()
        time.sleep(p["hold"])
        stream = stream_stats(t.host.since(t1), now() - t1)
        node = t.host.node and t.host.node["name"]
        down = disconnect(t)
        ok = down is not None
        rec(cycle=i, ok=ok, connect_s=round(secs, 3), disconnect_s=round(down, 3) if ok else None,
            node=node, **stream,
            **({} if ok else {"why": "did not end"} | diagnose(t)))
        if not ok:
            recover(t, log)
        time.sleep(p["gap"])


def connected(t, timeout=20):
    secs, out = connect(t, timeout)
    if secs is None:
        raise Fail(f"{t.name}: could not connect for the test: {out[-200:]}")
    return secs


def outage(t, t0, timeout):
    """After T0 (a link dropped on purpose): (seconds without reports, from the
    last report before the drop to the first after it) or None, and whether a
    drop was seen at all (its node went away, or 250 ms without a report)."""
    end = now() + timeout
    dropped = None
    while now() < end:
        times = t.host.since(t0 - 1)
        with t.host.lock:
            lost = [x for x in t.host.lost if x >= t0]
        before = [x for x in times if x < (lost[0] if lost else now())]
        if dropped is None:
            if lost:
                dropped = lost[0]
            else:
                g = [(b - a, a) for a, b in zip(times, times[1:]) if b >= t0]
                quiet = now() - times[-1] if times else 0
                if quiet >= 0.25:
                    dropped = times[-1]
                elif g and max(g)[0] >= 0.25:
                    dropped = max(g)[1]
            if dropped is None and now() - t0 > 5:
                return None, False
        if dropped is not None:
            last = max([x for x in before if x <= dropped] or [t0])
            after = [x for x in times if x > dropped + 0.001]
            if t.link == "bluetooth":
                after = sorted(after + [x for x in t.host.attached_after(dropped + 0.001)])
            if after:
                return after[0] - last, True
        time.sleep(0.05)
    return None, dropped is not None


def outage_pad(t, t0, timeout):
    """outage() as the pad sees it: the dongle keeps its USB device and repeats
    the last report for a while after the pad went, so the host cannot tell."""
    end, down = now() + timeout, None
    while now() < end:
        s = status(t.pad)
        state = s and s["state"]
        if down is None and state != "connected":
            down = now()
        elif down is not None and state == "connected":
            return now() - down, True
        if down is None and now() - t0 > 5:
            return None, False
        time.sleep(0.1)
    return None, down is not None


def test_relink(t, p, rec, log):
    if t.link == "usb":
        raise Skip("no relink on a USB session")
    if "Ultimate 2C" in t.pad.info["name"]:
        raise Skip("relink is not on this board (outside config mode)")
    connected(t)
    time.sleep(1)
    for i in range(p["count"]):
        t0 = now()
        rc, out = padtool(*t.pad.args, "relink")
        if rc:
            rec(cycle=i, ok=False, why="relink refused", padtool=out[-200:])
            continue
        if t.link == "dongle":
            o, dropped = outage_pad(t, t0, p["timeout"])
        else:
            o, dropped = outage(t, t0, p["timeout"])
        if not dropped:
            rec(cycle=i, ok=False, why="no drop seen (relink ignored?)")
        else:
            rec(cycle=i, ok=o is not None, outage_ms=round(o * 1000) if o is not None else None,
                **({} if o is not None else {"why": f"no reports for {p['timeout']} s after the drop"}
                   | diagnose(t)))
        if o is None:
            recover(t, log)
            connected(t)
        time.sleep(2)
    disconnect(t)


def test_host_drop(t, p, rec, log):
    if t.link != "bluetooth":
        raise Skip("the PC can end only a Bluetooth link")
    for i in range(p["count"]):
        connected(t)
        time.sleep(2)
        t0 = now()
        bluetoothctl("disconnect", t.address)
        noticed = t.host.quiet(t0, 0.5, p["timeout"])
        time.sleep(3)
        after = status(t.pad)
        state = after and after["state"]
        # A pad that went back to connecting or connected is fine too; end it and reconnect.
        padtool(*t.pad.args, "disconnect")
        wait_status(t.pad, ("idle",), 10)
        secs, out = connect(t, p["timeout"])
        rec(cycle=i, ok=secs is not None, host_saw_drop_s=round(noticed - t0, 3) if noticed else None,
            pad_state_after=state, reconnect_s=round(secs, 3) if secs else None,
            **({} if secs else diagnose(t)))
        if secs is None:
            recover(t, log)
        else:
            disconnect(t)
        time.sleep(2)


def test_reboot(t, p, rec, log):
    for i in range(p["count"]):
        before = status(t.pad)
        t0 = now()
        padtool(*t.pad.args, "reboot")
        t.pad.ephemeral = False
        time.sleep(1)
        s = wait_status(t.pad, None, p["timeout"])
        boot = now() - t0
        if not s or (before and s["uptime"] > before["uptime"]):
            rec(cycle=i, ok=False, why="did not come back on USB" if not s else "did not restart")
            raise Fail(f"{t.pad}: gone after reboot")
        setup(t, log)
        secs, out = connect(t, 30)
        rec(cycle=i, ok=secs is not None, boot_s=round(boot, 2), connect_s=round(secs, 3) if secs else None,
            **({} if secs else diagnose(t)))
        if secs is None:
            recover(t, log)
        else:
            disconnect(t)


def run_latency(t, p, throughput):
    if t.link == "usb":
        raise Skip("the latency probe needs the tool device, which a USB session replaces")
    if not has_probe(t):
        raise Skip("no latency probe in this build (the 8BitDo's: a U2CW_PROBE_EDGES test build)")
    connected(t)
    time.sleep(2)
    ev = t.host.node and event_node(t.host.node)
    if not ev:
        disconnect(t)
        raise Fail(f"{t.name}: no evdev node beside {t.host.node}")
    tmp = os.path.join(OUT_DIR, f".{t.name}.latency.json")
    args = ["latency", "--seconds", p["seconds"], "--event", ev, "--json", tmp]
    args += ["--throughput"] if throughput else ["--period", p["period"]]
    rc, out = padtool(*t.pad.args, *args, timeout=p["seconds"] * 4 + 60)
    disconnect(t)
    if rc or not os.path.exists(tmp):
        if "PROBE" in out or "not supported" in out or "no key events" in out:
            raise Skip(f"no latency probe: {out.splitlines()[-1]}")
        raise Fail(f"{t.name}: padtool latency: {out[-300:]}")
    with open(tmp) as f:
        r = json.load(f)
    os.remove(tmp)
    return r, out


def test_latency(t, p, rec, log):
    r, out = run_latency(t, p, False)
    total = r.get("total_ms") or {}
    merged = r.get("missed_short", 0)  # pulses under ~2 link events: newest wins, by design
    missed = r["expected"] - r["seen"] - merged
    ok = bool(total) and missed == 0 and (p["max_p99"] is None or total["p99"] <= p["max_p99"])
    rec(ok=ok, total_ms=total, pad_ms=r.get("pad_ms"), air_ms=r.get("air_ms"), missed=missed, merged=merged,
        expected=r["expected"], host_rate=r["host_rate"], clock_fit_ms=r["clock_fit_ms"])


def test_throughput(t, p, rec, log):
    r, out = run_latency(t, p, True)
    rec(ok=r["host_reports"] > 0, pad_rate=r["pad_rate"], host_rate=r["host_rate"], lost=r["lost"],
        host_gap_ms=r["host_gap_ms"])


def has_probe(t):
    """The latency probe: in the Jieli pads' builds, not the 8BitDo ones (test builds only)."""
    return t.link != "usb" and "Ultimate 2C" not in t.pad.info["name"]


def test_soak(t, p, rec, log):
    drive = p["drive"]
    if drive == "auto":
        drive = "probe" if has_probe(t) else "idle"
    if drive == "probe" and not has_probe(t):
        raise Skip("drive=probe: this target has no latency probe")
    connected(t)
    time.sleep(1)
    probe, tmp = None, os.path.join(OUT_DIR, f".{t.name}.soak.json")
    if drive == "probe":         # A every PERIOD ms: a steady stream, and its latency
        ev = t.host.node and event_node(t.host.node)
        if os.path.exists(tmp):
            os.remove(tmp)
        probe = subprocess.Popen(PADTOOL + [*t.pad.args, "latency", "--seconds", str(p["seconds"]),
                                            "--period", str(p["period"]), "--event", ev, "--json", tmp],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    t0 = now()
    end = t0 + p["seconds"]
    drops, window = 0, 10
    while now() < end:
        w0 = now()
        while now() < min(end, w0 + window):
            time.sleep(0.2)
            times = t.host.since(w0)
            if (times and now() - times[-1] > 2) or (not times and now() - w0 > 2):
                drops += 1             # no report for 2 s: the link is lost
                rec(event="lost", at_s=round(now() - t0, 1), **diagnose(t))
                log("lost", f"{t.name}: no reports for 2 s at {now() - t0:.0f} s; reconnecting")
                padtool(*t.pad.args, "disconnect")
                wait_status(t.pad, ("idle",), 10)
                secs, _ = connect(t, 30)
                rec(event="reconnect", ok=secs is not None, connect_s=secs and round(secs, 3))
                if secs is None:
                    recover(t, log)
                    connected(t)
                break
        times = t.host.since(w0)
        g = gaps_ms(times)
        rec(event="window", at_s=round(w0 - t0, 1), rate=round(len(times) / max(0.1, now() - w0), 1),
            max_gap_ms=round(max(g), 1) if g else None,
            stalls=sum(1 for x in g if x > p["stall"]))
    # The probe's first edge comes a moment after it starts: no gap before it.
    start = t0 + 1 if probe else t0
    times = t.host.since(start)
    span = now() - start
    lat = {}
    if probe:
        try:
            out, _ = probe.communicate(timeout=60)
        except subprocess.TimeoutExpired:
            probe.kill()
            out = "timed out"
        if os.path.exists(tmp):
            with open(tmp) as f:
                r = json.load(f)
            os.remove(tmp)
            merged = r.get("missed_short", 0)
            lat = {"total_ms": r.get("total_ms"), "missed": r["expected"] - r["seen"] - merged,
                   "merged": merged, "expected": r["expected"]}
        else:
            lat = {"probe_error": out.strip().splitlines()[-1] if out.strip() else "no result"}
    g = gaps_ms(times)
    stalls = [round(x) for x in g if x > p["stall"]]
    # Undriven, a profile that sends only on change has gaps of its own: then only lost links count.
    ok = drops == 0 and (drive == "idle" or (not stalls and not lat.get("probe_error") and not lat.get("missed")))
    rec(ok=ok, seconds=p["seconds"], drive=drive, drops=drops, stalls=len(stalls),
        worst_stalls_ms=sorted(stalls)[-5:], **stream_stats(times, span), **lat)
    disconnect(t)


TESTS = {"connect": test_connect, "relink": test_relink, "host-drop": test_host_drop,
         "reboot": test_reboot, "latency": test_latency, "throughput": test_throughput, "soak": test_soak}


# ---- the report ----------------------------------------------------------------

def fmt_q(q, key="p50"):
    return "-" if not q else f"{q['p50']:.1f}/{q['p99']:.1f}/{q['max']:.1f}"


def merged_text(r):
    return f" (+{r['merged']} pulses under 8 ms merged)" if r.get("merged") else ""


def summarize(target, test, rows):
    """One line of the report: (verdict, text)."""
    if any(r.get("skipped") for r in rows):
        return "skip", rows[0]["skipped"]
    if any(r.get("error") for r in rows):
        return "FAIL", rows[-1]["error"]
    cycles = [r for r in rows if "ok" in r and r.get("event") in (None, "reconnect")]
    bad = [r for r in cycles if not r["ok"]]
    verdict = "FAIL" if bad else "ok"
    text = f"{len(cycles) - len(bad)}/{len(cycles)} ok"
    num = lambda k: [r[k] for r in cycles if r.get(k) is not None]
    if test == "connect":
        c, d = quantiles(num("connect_s")), quantiles(num("disconnect_s"))
        gaps = [r["gap_ms"]["max"] for r in cycles if r.get("gap_ms")]
        rates = num("rate")
        text += (f"; connect p50/p99/max {fmt_q(c)} s; disconnect p50 {d['p50'] if d else '-'} s"
                 f"; report rate {min(rates) if rates else 0:.0f}-{max(rates) if rates else 0:.0f}/s"
                 f", worst gap {max(gaps) if gaps else 0:.0f} ms")
    elif test == "relink":
        text += f"; outage p50/p99/max {fmt_q(quantiles(num('outage_ms')))} ms"
    elif test == "host-drop":
        states = sorted(set(str(r.get("pad_state_after")) for r in cycles))
        text += (f"; pad after the drop: {', '.join(states)}"
                 f"; reconnect p50/p99/max {fmt_q(quantiles(num('reconnect_s')))} s")
    elif test == "reboot":
        text += (f"; boot p50 {fmt_q(quantiles(num('boot_s')))} s"
                 f"; connect p50/p99/max {fmt_q(quantiles(num('connect_s')))} s")
    elif test == "latency" and cycles:
        r = cycles[-1]
        text = (f"total p50/p99/max {fmt_q(r['total_ms'])} ms (pad {fmt_q(r['pad_ms'])}, air {fmt_q(r['air_ms'])})"
                f"; missed {r['missed']}/{r['expected']}{merged_text(r)}; host {r['host_rate']:.0f} reports/s")
    elif test == "throughput" and cycles:
        r = cycles[-1]
        text = (f"pad {r['pad_rate']:.0f}/s, host {r['host_rate']:.0f}/s, lost or merged {r['lost']}"
                f"; host gaps p50/p99/max {fmt_q(r['host_gap_ms'])} ms")
    elif test == "soak":
        final = [r for r in rows if "drops" in r]
        if final:
            r = final[-1]
            verdict = "ok" if r["ok"] else "FAIL"
            text = (f"{r['seconds']} s {r['drive']}: {r['rate']:.0f} reports/s, gaps p50/p99/max "
                    f"{fmt_q(r['gap_ms'])} ms; over {DEFAULTS['soak']['stall']} ms: {r['stalls']} "
                    f"{r['worst_stalls_ms'] or ''}; links lost {r['drops']}")
            if r.get("total_ms"):
                text += f"; latency p50/p99/max {fmt_q(r['total_ms'])} ms, missed {r['missed']}/{r['expected']}{merged_text(r)}"
            if r.get("probe_error"):
                text += f"; probe: {r['probe_error']}"
    return verdict, text


def write_report(path, header, results):
    lines = ["# Stress test", "", *header, "", "| target | test | result | details |", "|---|---|---|---|"]
    for (target, test), rows in results.items():
        verdict, text = summarize(target, test, rows)
        lines.append(f"| {target} | {test} | {verdict} | {text} |")
    fails = [(k, r) for k, rows in results.items() for r in rows if r.get("ok") is False or r.get("error")]
    if fails:
        lines += ["", "## Failures", ""]
        for (target, test), r in fails:
            what = r.get("error") or r.get("why") or "failed"
            lines.append(f"- {target} {test} cycle {r.get('cycle', '-')}: {what}"
                         + (f"; pad {r['status']['state']}" if r.get("status") else ""))
            for line in r.get("log", [])[-6:]:
                lines.append(f"    {line}")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return "\n".join(lines)


# ---- the plan ------------------------------------------------------------------

def parse_kv(text):
    """name:k=v,k=v (a test) or k=v,k=v (a target) as a dict."""
    out = {}
    for part in text.split(","):
        if "=" in part:
            k, v = part.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def typed(p, defaults):
    out = dict(defaults)
    for k, v in p.items():
        if k == "test":
            continue
        if k not in defaults:
            raise SystemExit(f"stress: unknown parameter {k} (known: {', '.join(defaults)})")
        out[k] = float(v) if v is not None and str(v).replace(".", "", 1).isdigit() else v
        if isinstance(out[k], float) and out[k].is_integer() and k != "hold":
            out[k] = int(out[k])
    return out


OUT_DIR = None


def main():
    global OUT_DIR
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("plan", nargs="?", help="a JSON plan: {\"targets\": [...], \"tests\": [...], \"rounds\": N}")
    ap.add_argument("--target", action="append", default=[], metavar="K=V,...", help="a target (see above)")
    ap.add_argument("--test", action="append", default=[], metavar="NAME[:K=V,...]", help="a test (see above)")
    ap.add_argument("--rounds", type=int, help="run the whole test list this many times (default 1)")
    ap.add_argument("--out", help="the results directory (default build/stress/<time>)")
    ap.add_argument("--only", metavar="NAME,...", help="only these of the plan's targets (by name)")
    ap.add_argument("--example", action="store_true", help="print an example plan")
    ap.add_argument("--keep-pairing", action="store_true",
                    help="leave the PC paired with pads this run paired (their bond is gone after the reboot)")
    a = ap.parse_args()
    if a.example:
        print(json.dumps(EXAMPLE, indent=2))
        return
    plan = {}
    if a.plan:
        with open(a.plan) as f:
            plan = json.load(f)
    targets = plan.get("targets", []) + [parse_kv(x) for x in a.target]
    tests = plan.get("tests", []) + [{"test": x.split(":", 1)[0], **parse_kv(x.split(":", 1)[1] if ":" in x else "")}
                                     for x in a.test]
    if a.only:
        names = a.only.split(",")
        unknown = [n for n in names if n not in {t.get("name") for t in targets}]
        if unknown:
            raise SystemExit(f"stress: no target {', '.join(unknown)} in the plan")
        targets = [t for t in targets if t.get("name") in names]
    rounds = a.rounds or plan.get("rounds", 1)
    if not targets or not tests:
        ap.error("give a plan, or --target and --test")
    for x in tests:
        if x.get("test") not in TESTS:
            raise SystemExit(f"stress: unknown test {x.get('test')} (known: {', '.join(TESTS)})")
    tests = [(x["test"], typed(x, DEFAULTS[x["test"]])) for x in tests]

    devices = {port: Device(port, info) for port, info in find_devices().items()}
    targets = [Target(s, devices) for s in targets]
    OUT_DIR = a.out or os.path.join(REPO, "build", "stress", time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(OUT_DIR, exist_ok=True)
    events = open(os.path.join(OUT_DIR, "events.jsonl"), "a")
    used = sorted({d for t in targets for d in (t.pad, t.dongle) if d}, key=lambda d: d.port)
    header = [f"Started {time.strftime('%Y-%m-%d %H:%M:%S')}, {rounds} round(s); results in {OUT_DIR}", ""]
    header += [f"- {d.info['name']} [{d.port}] build {d.info['build']}" for d in used]
    header += [f"- target {t.name}: {t.pad.info['name']} slot {t.slot} over {t.link}"
               + (f" ({t.type})" if t.type else "") + (f" via {t.dongle}" if t.dongle else "") for t in targets]
    print("\n".join(header), flush=True)

    results = {}
    def log(kind, text):
        say(text)
        events.write(json.dumps({"time": time.time(), "kind": kind, "text": text}) + "\n")
        events.flush()

    try:
        for rnd in range(rounds):
            for t in targets:
                t.host = Host(t.match)
                try:
                    log("setup", f"{t.name}: setting up (round {rnd + 1}/{rounds})")
                    setup(t, log)
                except Fail as e:
                    log("error", f"{t.name}: set-up failed: {e}")
                    for test, _ in tests:
                        results.setdefault((t.name, test), []).append({"error": f"set-up: {e}"})
                    t.host.stop()
                    continue
                for test, p in tests:
                    rows = results.setdefault((t.name, test), [])
                    def rec(**r):
                        r = {"round": rnd, **r}
                        rows.append(r)
                        events.write(json.dumps({"time": time.time(), "target": t.name, "test": test, **r}) + "\n")
                        events.flush()
                        if "ok" in r and r.get("event") != "window":
                            brief = {k: v for k, v in r.items() if k not in ("log", "round", "status")}
                            say(f"{t.name} {test}: {json.dumps(brief)}")
                    log("test", f"{t.name}: {test} {p}")
                    try:
                        TESTS[test](t, p, rec, log)
                    except Skip as e:
                        rec(skipped=str(e))
                        log("skip", f"{t.name}: {test} skipped: {e}")
                    except Fail as e:
                        rec(error=str(e), **diagnose(t))
                        log("error", f"{t.name}: {test}: {e}")
                        try:
                            recover(t, log)
                        except Fail as e2:
                            log("error", f"{t.name}: giving up on this target: {e2}")
                            break
                try:
                    idle(t)
                except Fail as e:
                    log("error", str(e))
                t.host.stop()
    except KeyboardInterrupt:
        log("stop", "interrupted")
    finally:
        say("cleaning up: every device used rebooted, out of ephemeral mode")
        for d in used:
            rc, logtext = padtool(*d.args, "log", timeout=20)
            with open(os.path.join(OUT_DIR, f"log-{d.port}.txt"), "w") as f:
                f.write(logtext + "\n")
        for t in targets:
            if t.link == "usb" and not status(t.pad):
                tool_back(t)
            elif t.dongle and not status(t.dongle):
                dongle_tool(t)
            padtool(*t.pad.args, "disconnect")
        time.sleep(1)
        for d in used:
            if d.ephemeral:
                rc, out = padtool(*d.args, "reboot")
                if rc:
                    say(f"{d}: reboot failed ({out}); unplug it to leave ephemeral mode")
        if not a.keep_pairing:
            for t in targets:
                for addr in t.paired_here:
                    bluetoothctl("remove", addr)
        events.close()
    report = write_report(os.path.join(OUT_DIR, "report.md"), header, results)
    print()
    print(report)


if __name__ == "__main__":
    main()
