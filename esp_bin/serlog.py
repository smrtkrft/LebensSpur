#!/usr/bin/env python3
"""Cihazi resetleyip N saniye seri log yakalar; USB yeniden sayilirsa yeniden baglanir.
Kullanim: serlog.py PORT SANIYE [--noreset]"""
import os, sys, time, serial
port, dur = sys.argv[1], float(sys.argv[2])
reset = '--noreset' not in sys.argv
end = time.time() + dur
buf = b''
s = None
while time.time() < end:
    try:
        if s is None:
            if not os.path.exists(port):
                time.sleep(0.1); continue
            s = serial.Serial(port, 115200, timeout=0.2)
            if reset:
                s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
                reset = False
        buf += s.read(4096)
    except (serial.SerialException, OSError):
        buf += b'\n[serlog: port koptu, yeniden baglaniliyor]\n'
        try: s.close()
        except Exception: pass
        s = None
        time.sleep(0.2)
sys.stdout.write(buf.decode(errors='replace'))
