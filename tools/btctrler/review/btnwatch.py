#!/usr/bin/env python3
"""Fetch host btmon log for N seconds; print Switch 0x30 button changes and rate."""
import re, subprocess, sys, time
H = ["python3", "/work/cyclone2-esp32/hostbt/hostbt.py"]
RX = re.compile(r"> ACL Data RX: Handle (\d+) flags 0x\w+ dlen (\d+)\s+#\d+ \[hci\d+\] ([\d.]+)")
t = float(sys.argv[1]) if len(sys.argv) > 1 else 10
s = int(subprocess.run(H + ["seq"], capture_output=True, text=True).stdout.split()[0])
time.sleep(t)
lines = subprocess.run(H + ["since", str(s)], capture_output=True, text=True).stdout.splitlines()
prev = None; n = 0; first = last = None; zl = 0
for i, l in enumerate(lines):
    m = RX.search(l)
    if not m or i + 2 >= len(lines) or "PSM 19" not in lines[i + 1]: continue
    h = lines[i + 2].split(" M ")[1].split()[:16]
    if h[1] != "30": continue
    ts = float(m.group(3)); n += 1; first = first or ts; last = ts
    btn = " ".join(h[4:7]); zl += int(h[6], 16) >> 7
    if btn != prev: print(f"{ts:.3f} buttons {btn}"); prev = btn
for l in lines:
    if "Mode Change" in l or "Interval:" in l or "Disconnect" in l: print(l.split(" M ")[-1].strip())
if n > 1: print(f"{n} reports, {(n-1)/(last-first):.0f}/s, ZL set in {zl}")
