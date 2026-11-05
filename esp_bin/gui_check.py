#!/usr/bin/env python3
"""
GUI denetimi: headless Chrome'u DevTools protokolüyle sürer (yalnız Python stdlib).

  python3 esp_bin/gui_check.py http://192.168.176.50 OUT_DIR [--width 420 --height 860] [--steps steps.json]

Varsayılan adımlar: ana ekran, ayarlar penceresinin 5 sekmesi, alt sayfalar, açık tema.
Her adımda ekran görüntüsü (OUT_DIR/NN-ad.png) alınır; yakalanmamış JS hataları ve console.error
satırları toplanır. Hata varsa çıkış kodu 1.
"""
import argparse
import base64
import json
import os
import re
import socket
import struct
import subprocess
import sys
import time
import urllib.request

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"


class WS:
    def __init__(self, url):
        assert url.startswith("ws://")
        hostport, path = url[5:].split("/", 1)
        host, port = hostport.split(":")
        self.s = socket.create_connection((host, int(port)), timeout=30)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET /{path} HTTP/1.1\r\nHost: {hostport}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            resp += self.s.recv(1)
        if b" 101 " not in resp.split(b"\r\n")[0]:
            raise RuntimeError(resp)
        self.buf = b""
        self.s.settimeout(600)                    # uzun denetim adımları

    def send(self, text):
        data = text.encode()
        mask = os.urandom(4)
        n = len(data)
        hdr = b"\x81" + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n) if n < 65536
                         else bytes([0x80 | 127]) + struct.pack(">Q", n))
        self.s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        msg = b""
        while True:
            b0, b1 = self._read(2)
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            msg += self._read(n)
            if b0 & 0x80:
                return msg.decode()


class Page:
    def __init__(self, ws):
        self.ws, self.id, self.events = ws, 0, []

    def call(self, method, **params):
        self.id += 1
        self.ws.send(json.dumps({"id": self.id, "method": method, "params": params}))
        while True:
            m = json.loads(self.ws.recv())
            if m.get("id") == self.id:
                if "error" in m:
                    raise RuntimeError(f"{method}: {m['error']}")
                return m.get("result", {})
            self.events.append(m)

    def js(self, expr):
        r = self.call("Runtime.evaluate", expression=expr, awaitPromise=True, returnByValue=True)
        if "exceptionDetails" in r:
            return {"__error": r["exceptionDetails"].get("exception", {}).get("description", "?")}
        return r.get("result", {}).get("value")

    def wait(self, sec):
        end = time.time() + sec
        while time.time() < end:
            self.js("1")
            time.sleep(0.2)

    def shot(self, path):
        data = self.call("Page.captureScreenshot", format="png")["data"]
        with open(path, "wb") as fh:
            fh.write(base64.b64decode(data))

    def errors(self):
        out = []
        for e in self.events:
            if e.get("method") == "Runtime.exceptionThrown":
                d = e["params"]["exceptionDetails"]
                out.append("JS: " + d.get("exception", {}).get("description", d.get("text", "?")))
            elif e.get("method") == "Log.entryAdded" and e["params"]["entry"]["level"] == "error":
                en = e["params"]["entry"]
                out.append(f"{en.get('source')}: {en.get('text', '')[:160]} {en.get('url', '')}")
            elif e.get("method") == "Runtime.consoleAPICalled" and e["params"]["type"] == "error":
                out.append("console: " + " ".join(str(a.get("value", a.get("description", ""))) for a in e["params"]["args"]))
        self.events = []
        return out


# ad, çalıştırılacak JS (tıklama vb.), bekleme (s)
DEFAULT_STEPS = [
    ["home", "", 3],
    ["timer", "document.getElementById('settingsBtn').click()", 2],
    ["conn", "document.querySelector('[data-index=\"1\"]').click()", 3],
    ["conn-open", "document.querySelectorAll('.accordion').forEach(a=>a.classList.add('open'))", 1],
    ["actions", "document.querySelector('[data-index=\"2\"]').click()", 2],
    ["relay", "document.querySelector('[data-config=\"relay\"]').click()", 2],
    ["api", "document.getElementById('footerCancelBtn').click();document.querySelector('[data-config=\"early-api\"]').click()", 2],
    ["groups", "document.getElementById('footerCancelBtn').click();document.querySelector('[data-config=\"trigger-mail\"]').click()", 2],
    ["system", "document.getElementById('footerCancelBtn').click();document.querySelector('[data-index=\"3\"]').click()", 1],
    ["security", "document.getElementById('securityCardBtn').click()", 2],
    ["info", "document.getElementById('footerCancelBtn').click();document.querySelector('[data-index=\"4\"]').click()", 1],
    ["device", "document.querySelector('[data-subpage=\"deviceInfoSubpage\"]').click()", 3],
    ["logs", "document.getElementById('footerCancelBtn').click();document.querySelector('[data-subpage=\"logsSubpage\"]').click()", 3],
    ["light", "document.getElementById('footerCancelBtn').click();document.getElementById('footerCancelBtn').click();"
              "document.body.classList.add('light-theme')", 1],
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("url")
    ap.add_argument("out")
    ap.add_argument("--width", type=int, default=420)
    ap.add_argument("--height", type=int, default=860)
    ap.add_argument("--steps")
    ap.add_argument("--port", type=int, default=9333)
    ap.add_argument("--allow-net", default="", help="beklenen ağ hatası deseni (regex), sayılmaz")
    a = ap.parse_args()
    steps = json.load(open(a.steps)) if a.steps else DEFAULT_STEPS
    os.makedirs(a.out, exist_ok=True)
    prof = os.path.join(a.out, ".profile")
    proc = subprocess.Popen([CHROME, "--headless=new", "--disable-gpu", f"--remote-debugging-port={a.port}",
                             f"--user-data-dir={prof}", f"--window-size={a.width},{a.height}", "about:blank"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{a.port}/json"))
                ws_url = next(t["webSocketDebuggerUrl"] for t in tabs if t["type"] == "page")
                break
            except Exception:
                time.sleep(0.2)
        else:
            sys.exit("Chrome DevTools açılmadı")
        p = Page(WS(ws_url))
        p.call("Runtime.enable")
        p.call("Page.enable")
        p.call("Log.enable")                      # CSP ihlali, ağ hatası (404/500) burada görünür
        p.call("Page.navigate", url=a.url)
        for _ in range(100):                      # sayfa + uygulama hazır
            time.sleep(0.2)
            if p.js("document.readyState==='complete' && !!document.body.dataset.hash"):
                break
        p.wait(1)
        bad = 0
        for i, (name, expr, wait) in enumerate(steps):
            if expr:
                try:
                    r = p.js(expr if expr.startswith("(async") else "(()=>{" + expr + "})()")
                except RuntimeError as e:          # sayfa yenilendi (ör. yeniden başlatma sonrası)
                    if "navigated" not in str(e):
                        raise
                    r = f"sayfa yenilendi ({name})"
                    time.sleep(3)
                if isinstance(r, dict) and "__error" in r:
                    print(f"[{name}] adım JS hatası: {r['__error']}")
                    bad += 1
                elif isinstance(r, list):              # denetim adımı: başarısız kontrollerin listesi
                    for f in r:
                        print(f"[{name}] HATA {f}")
                    bad += len(r)
                elif isinstance(r, str):
                    print(f"[{name}] {r}")
            p.wait(wait)
            p.shot(os.path.join(a.out, f"{i:02d}-{name}.png"))
            for e in p.errors():
                if a.allow_net and e.startswith("network:") and re.search(a.allow_net, e):
                    print(f"[{name}] (beklenen) {e}")
                    continue
                print(f"[{name}] {e}")
                bad += 1
        print(f"gui_check: {len(steps)} adım, {bad} hata")
        sys.exit(1 if bad else 0)
    finally:
        proc.terminate()


if __name__ == "__main__":
    main()
