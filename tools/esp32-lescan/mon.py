import sys, time, serial
s = serial.Serial("/dev/bwrap/ttyUSB-esp0", 115200, timeout=0.2)
if len(sys.argv) > 2 and sys.argv[2] == "reset":
    s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
end = time.time() + float(sys.argv[1])
buf = b""
while time.time() < end:
    buf += s.read(4096)
    *lines, buf = buf.split(b"\n")
    for l in lines:
        t = l.decode("utf-8", "replace").rstrip()
        if t: print(time.strftime("%H:%M:%S"), t, flush=True)
