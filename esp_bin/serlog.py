#!/usr/bin/env python3
"""Cihazi resetleyip N saniye seri log yakalar. Kullanim: serlog.py PORT SANIYE"""
import sys, time, serial
s = serial.Serial(sys.argv[1], 115200, timeout=0.2)
s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
end = time.time() + float(sys.argv[2]); buf = b''
while time.time() < end:
    buf += s.read(4096)
sys.stdout.write(buf.decode(errors='replace'))
