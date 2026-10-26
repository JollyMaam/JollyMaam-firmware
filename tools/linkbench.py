#!/usr/bin/env python3
"""Benchmark the pad-to-PC link through our dongle: latency and throughput.

With one pad (on USB, for its clock and commands) and one of our dongles
plugged in, this puts both in ephemeral mode, sets a pad slot to the
dongle in RAM, pairs the two, runs `padtool latency` and
`padtool latency --throughput` RUNS times each, appends every result as a
JSON line (with both devices' identities) to OUT, prints a summary and
reboots both, so neither keeps anything from the test.

    tools/linkbench.py                       # 3 runs of 10 s each way
    tools/linkbench.py --runs 5 --seconds 20 --label "desk, 1 m"
    tools/linkbench.py --summary             # the results in OUT so far

The pad's slot (default up) is changed only in RAM; the dongle's bond too.
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PADTOOL = [sys.executable, os.path.join(HERE, "..", "padtool", "padtool.py")]
OUT = os.path.join(HERE, "..", "build", "bench", "linkbench.jsonl")


def padtool(*args, check=True, timeout=120):
    r = subprocess.run(PADTOOL + list(args), capture_output=True, text=True, timeout=timeout)
    if check and r.returncode:
        sys.exit(f"linkbench: padtool {' '.join(args)}: {(r.stderr or r.stdout).strip()}")
    return r.stdout.strip()


def state(*board):
    out = padtool(*board, "status", check=False)
    return out.split()[1] if out.startswith("state ") else None


def dongle_tool_device():
    """The dongle as its tool device: while a pad is linked it is that pad's type on USB."""
    if state("--dongle"):
        return True
    padtool("disconnect", check=False)
    for _ in range(3):
        time.sleep(2)
        if state("--dongle"):
            return True
        padtool("tool", check=False)
    return bool(state("--dongle"))


def wait_connected(seconds=30):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if state() == "connected":
            return True
        time.sleep(0.5)
    return False


def summary(path, since=None):
    rows = []
    with open(path) as f:
        for line in f:
            r = json.loads(line)
            if since and r.get("time", "") < since:
                continue
            rows.append(r)
    if not rows:
        print("no results")
        return
    print(f"{'time':19}  {'pad':12} {'dongle':18} {'mode':10} {'rate':>11}  {'p50':>6} {'p90':>6} {'p99':>6} {'max':>6}  missed")
    for r in rows:
        pad = r["pad"].split(",")[0]
        dongle = r.get("dongle", "").split(",")[0]
        if r["mode"] == "throughput":
            g = r["host_gap_ms"] or {}
            print(f"{r['time']:19}  {pad:12} {dongle:18} throughput {r['host_rate']:5.0f}/{r['pad_rate']:<5.0f}"
                  f"  {g.get('p50', 0):6.2f} {g.get('p90', 0):6.2f} {g.get('p99', 0):6.2f} {g.get('max', 0):6.2f}"
                  f"  {r['lost']}  (gaps)")
        else:
            t = r["total_ms"] or {}
            print(f"{r['time']:19}  {pad:12} {dongle:18} latency    {'':11}  {t.get('p50', 0):6.2f} {t.get('p90', 0):6.2f}"
                  f" {t.get('p99', 0):6.2f} {t.get('max', 0):6.2f}  {r['expected'] - r['seen']}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--runs", type=int, default=3)
    p.add_argument("--seconds", type=float, default=10)
    p.add_argument("--slot", default="up", help="the pad slot to use, in RAM only (default up)")
    p.add_argument("--type", default="generic", help="what the dongle is on USB (a HID type: generic, "
                   "dualsense, dualshock4, switch, switch2; xbox has no evdev node here)")
    p.add_argument("--label", default="", help="a note stored with each result")
    p.add_argument("--out", default=OUT)
    p.add_argument("--summary", action="store_true", help="print the results in --out and stop")
    a = p.parse_args()
    if a.summary:
        summary(a.out)
        return
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)

    if not state():
        sys.exit("linkbench: no pad running our firmware on USB")
    if not dongle_tool_device():
        sys.exit("linkbench: no dongle running our firmware (a pad still linked to it on battery?)")
    pad_id, dongle_id = padtool("identify"), padtool("--dongle", "identify")
    print(f"pad:    {pad_id}\ndongle: {dongle_id}")
    started = time.strftime("%Y-%m-%dT%H:%M:%S")
    try:
        padtool("ephemeral")
        padtool("--dongle", "ephemeral")
        padtool("set", a.slot, "type", a.type, "transport", "dongle")
        padtool("pair", a.slot)
        time.sleep(1)
        padtool("--dongle", "pair")
        if not wait_connected():
            sys.exit("linkbench: the pad did not connect through the dongle")
        time.sleep(2)                    # the link settles (QoS, the host's driver)
        for i in range(a.runs):
            for extra in ([], ["--throughput"]):
                print(f"run {i + 1}/{a.runs} {'throughput' if extra else 'latency'}", flush=True)
                tmp = a.out + ".run"
                if os.path.exists(tmp):
                    os.remove(tmp)
                out = padtool("latency", "--seconds", str(a.seconds), "--json", tmp, "--label", a.label,
                              *extra, timeout=a.seconds * 4 + 60)
                print("  " + out.replace("\n", "\n  "))
                with open(tmp) as f:
                    r = json.loads(f.read())
                os.remove(tmp)
                r["dongle"] = dongle_id
                with open(a.out, "a") as f:
                    f.write(json.dumps(r) + "\n")
    finally:
        padtool("disconnect", check=False)
        time.sleep(2)
        padtool("reboot", check=False)
        if dongle_tool_device():
            padtool("--dongle", "reboot", check=False)
        else:
            print("linkbench: could not reach the dongle to reboot it; unplug it to end its ephemeral mode")
    print()
    summary(a.out, since=started)


if __name__ == "__main__":
    main()
