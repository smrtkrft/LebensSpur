#!/usr/bin/env python3
"""LebensSpur — zorlama ve gerçek koşul testi (yalnız stdlib; reset bölümü IDF python + pyserial ister).

Kullanım: test_stress.py <cihaz-ip> [--port /dev/cu.usbmodemXXXX]

Amaç: cihaz uç değerlerde, yük altında ve kötü koşullarda (yarım bağlantılar, art arda yeniden başlatma,
yazma sırasında elektrik kesintisi benzeri reset) kararlı kalıyor mu. Her bölümden sonra cihazın
çökmediği (açılış sayacı / çökme kaydı), belleğin sızmadığı ve ayarların bozulmadığı denetlenir.
"""
import http.client, json, os, random, socket, subprocess, sys, threading, time

HOST = sys.argv[1]
PORT = sys.argv[sys.argv.index("--port") + 1] if "--port" in sys.argv else None
IDF_PY = os.path.expanduser("~/.espressif/python_env/idf5.5_py3.9_env/bin/python")
W = {"X-LS-Request": "1"}
fails = 0


def req(method, path, body=None, headers=None, raw=None, timeout=8):
    c = http.client.HTTPConnection(HOST, 80, timeout=timeout)
    h = {"Host": HOST}
    if body is not None:
        raw = json.dumps(body).encode()
        h.update({"Content-Type": "application/json", "X-LS-Request": "1"})
    if headers:
        h.update(headers)
    try:
        c.request(method, path, body=raw, headers=h)
        r = c.getresponse()
        data = r.read()
    except OSError as e:
        return 0, {"err": str(e)}
    finally:
        c.close()
    try:
        return r.status, json.loads(data)
    except Exception:
        return r.status, None


def check(name, cond, info=""):
    global fails
    print(("  OK   " if cond else "  HATA ") + name + ("" if cond else f"  -> {info}"), flush=True)
    if not cond:
        fails += 1


def data(path):
    s, j = req("GET", path)
    return j["data"] if s == 200 and j and "data" in j else {}


def device():
    return data("/api/device")


def wait_up(boot_before=None, limit=120):
    t0 = time.time()
    while time.time() - t0 < limit:
        d = device()
        if d and (boot_before is None or d.get("boot_count", -1) > boot_before):
            return d
        time.sleep(2)
    return {}


BASE = {}


def health(tag, boot_expect=None):
    """Bölüm sonu: cihaz ayakta, beklenmedik yeniden başlama yok, çökme kaydı aynı, bellek toparlandı."""
    time.sleep(2)
    d = wait_up(limit=60)
    check(f"[{tag}] cihaz ayakta", bool(d))
    if not d:
        return d
    be = BASE["boot_count"] if boot_expect is None else boot_expect
    check(f"[{tag}] beklenmedik yeniden başlama yok", d["boot_count"] == be, (be, d["boot_count"], d.get("last_cause")))
    check(f"[{tag}] yeni çökme kaydı yok", d.get("coredump") == BASE["coredump"], (BASE["coredump"], d.get("coredump")))
    check(f"[{tag}] bellek toparlandı (boş ≥ başlangıç − 12 KB)", d["heap_free"] >= BASE["heap_free"] - 12 * 1024,
          (BASE["heap_free"], d["heap_free"]))
    check(f"[{tag}] en düşük bellek güvenli (≥ 40 KB)", d["heap_min"] >= 40 * 1024, d["heap_min"])
    rl = data("/api/status").get("relay", {})
    check(f"[{tag}] röle boşta", rl == {"on": False, "remaining": 0}, rl)
    return d


def stop_timer():
    req("DELETE", "/api/timer/vacation", headers=W)
    req("POST", "/api/timer/cancel", headers=W)


print("== önkoşul")
if data("/api/auth").get("enabled"):
    print("cihazda parola var; test parolasız cihazda çalışır")
    sys.exit(2)
BASE.update(device())
check("cihaz erişilebilir", bool(BASE), BASE)
if not BASE:
    sys.exit(1)
print(f"   açılış {BASE['boot_count']}, boş bellek {BASE['heap_free']}, en düşük {BASE['heap_min']}, çökme kaydı {BASE.get('coredump')}")
stop_timer()
CFG0 = {s: data(f"/api/config/{s}") for s in ("relay", "timer", "actions", "auth", "device")}

# ---------------------------------------------------------------- 1
print("== 1) eşzamanlı istek seli (8 iş parçacığı, 30 sn)")
stats = {"ok": 0, "busy": 0, "bad": [], "err": 0}
lock = threading.Lock()
paths = ["/api/status", "/api/config/timer", "/api/config/relay", "/api/device", "/api/logs?n=20",
         "/api/wifi", "/api/groups", "/api/ota", "/api/backup", "/"]


def flood(deadline):
    while time.time() < deadline:
        p = random.choice(paths)
        s, _ = req("GET", p, timeout=10)
        with lock:
            if s == 200:
                stats["ok"] += 1
            elif s == 503:
                stats["busy"] += 1
            elif s == 0:
                stats["err"] += 1
            else:
                stats["bad"].append((p, s))


dl = time.time() + 30
th = [threading.Thread(target=flood, args=(dl,)) for _ in range(8)]
[t.start() for t in th]
# sel sürerken arayüzün asıl işi çalışıyor mu: check-in her 5 sn
probe = []
while time.time() < dl:
    t0 = time.time()
    s, _ = req("GET", "/api/status", timeout=15)
    probe.append((s, time.time() - t0))
    time.sleep(5)
[t.join() for t in th]
print(f"   200: {stats['ok']}, 503: {stats['busy']}, bağlantı hatası: {stats['err']}, diğer: {len(stats['bad'])}")
check("selde 4xx/5xx (503 dışı) yok", not stats["bad"], stats["bad"][:5])
check("selde yanıtların çoğu başarılı (≥ %90)", stats["ok"] >= 0.9 * (stats["ok"] + stats["busy"] + stats["err"]), stats)
check("sel sırasında durum sorgusu yanıt verdi (≤ 10 sn)", all(s == 200 and dt <= 10 for s, dt in probe), probe)
health("sel")

# ---------------------------------------------------------------- 2
print("== 2) canlı bağlantı (SSE) tükenmesi: 8 bağlantı, 4 yuva")
socks, codes = [], []
for i in range(8):
    try:
        so = socket.create_connection((HOST, 80), timeout=5)
        so.sendall(f"GET /api/events HTTP/1.1\r\nHost: {HOST}\r\nAccept: text/event-stream\r\n\r\n".encode())
        so.settimeout(5)
        head = so.recv(256).decode(errors="replace")
        codes.append(head.split(" ")[1] if " " in head else "?")
        socks.append(so)
    except OSError as e:
        codes.append("x:" + type(e).__name__)
print("   yanıt kodları:", codes)
check("en çok 4 canlı bağlantı kabul edildi, fazlası 503", codes.count("200") <= 4 and codes.count("503") >= 3, codes)
s, _ = req("GET", "/api/status", timeout=10)
check("yuvalar doluyken API yanıt veriyor", s == 200, s)
for so in socks:
    so.close()
time.sleep(4)
so = socket.create_connection((HOST, 80), timeout=5)
so.sendall(f"GET /api/events HTTP/1.1\r\nHost: {HOST}\r\n\r\n".encode())
so.settimeout(5)
head = so.recv(256).decode(errors="replace")
so.close()
check("bağlantılar kapanınca yuva yeniden açılıyor", " 200 " in head, head[:40])
health("sse")

# ---------------------------------------------------------------- 3
print("== 3) yarım kalan bağlantılar (10 soket, hiçbir şey göndermeden)")
idle = []
for _ in range(10):
    try:
        idle.append(socket.create_connection((HOST, 80), timeout=5))
    except OSError:
        break
print(f"   açılan: {len(idle)}")
t0 = time.time()
ok = False
while time.time() - t0 < 25:
    s, _ = req("GET", "/api/status", timeout=5)
    if s == 200:
        ok = True
        break
    time.sleep(1)
check("yarım bağlantılar varken API 25 sn içinde yanıt verdi", ok, time.time() - t0)
for so in idle:
    so.close()
health("yarim")

# ---------------------------------------------------------------- 4
print("== 4) ayar yazma fırtınası (60 kayıt art arda) + tutarlılık")
last = None
errs = []
for i in range(60):
    last = random.randint(0, 60)
    s, j = req("PUT", "/api/config/relay", {"delay_s": last})
    if s != 200:
        errs.append((i, s))
check("tüm kayıtlar kabul edildi", not errs, errs[:5])
check("son yazılan değer okunuyor", data("/api/config/relay").get("delay_s") == last, (last, data("/api/config/relay")))
time.sleep(3)
b = data("/api/backup")
check("yedek geçerli (birleştirilmiş yazım)", b.get("available") is True, b)
req("PUT", "/api/config/relay", {"delay_s": CFG0["relay"].get("delay_s", 0)})
health("yazma")

# ---------------------------------------------------------------- 5
print("== 5) sayaç uç değerleri")
bad = [({"unit": "min", "value": 0}, "değer 0"), ({"unit": "min", "value": 61}, "değer 61"),
       ({"unit": "year", "value": 1}, "birim year"), ({"warn_count": 0}, "uyarı 0"), ({"warn_count": 11}, "uyarı 11"),
       ({"warn_first_s": 5184001}, "ilk uyarı > 60 gün"), ({"value": -1}, "negatif"), ({"value": 1.5}, "kesirli"),
       ({"value": "7"}, "metin sayı"), ({"value": 1e30}, "çok büyük"),
       ({"unit": "min", "value": 1, "warn_count": 10, "warn_first_s": 0}, "1 dk'ya 10 uyarı sığmaz")]
for body, name in bad:
    s, j = req("PUT", "/api/timer", body)
    check(f"geçersiz sayaç reddedildi: {name}", s == 400, (s, j))
good = [({"unit": "min", "value": 1, "warn_count": 1, "warn_first_s": 0}, "1 dk, 1 uyarı (en kısa)"),
        ({"unit": "day", "value": 60, "warn_count": 10, "warn_first_s": 0}, "60 gün, 10 uyarı"),
        ({"unit": "day", "value": 60, "warn_count": 1, "warn_first_s": 5184000}, "ilk uyarı = süre"),
        ({"unit": "hour", "value": 1, "warn_count": 10, "warn_first_s": 3599}, "1 sa, ilk uyarı 3599 sn")]
for body, name in good:
    s, j = req("PUT", "/api/timer", body)
    if s != 200:
        check(f"uç geçerli sayaç: {name}", False, (s, j))
        continue
    req("POST", "/api/timer/checkin", headers=W)
    t = data("/api/status").get("timer", {})
    plan = t.get("warn_plan", [])
    sane = all(0 < x < t.get("total", 0) for x in plan) and plan == sorted(plan, reverse=True) and len(set(plan)) == len(plan)
    check(f"uç geçerli sayaç: {name} → plan tutarlı ({len(plan)} uyarı)", t.get("state") == "running" and sane, t)
    stop_timer()
req("PUT", "/api/timer", {k: CFG0["timer"][k] for k in ("unit", "value", "warn_count", "warn_first_s")})
health("sayac")

# ---------------------------------------------------------------- 6
print("== 6) check-in fırtınası (sayaç çalışırken 50 kez)")
req("PUT", "/api/timer", {"unit": "min", "value": 5, "warn_count": 3, "warn_first_s": 0})
req("POST", "/api/timer/checkin", headers=W)
codes = [req("POST", "/api/timer/checkin", headers=W)[0] for _ in range(50)]
check("tüm check-in'ler yanıtlandı (200/429)", all(c in (200, 429) for c in codes), set(codes))
t = data("/api/status").get("timer", {})
check("sayaç tam süreden çalışıyor", t.get("state") == "running" and t.get("remaining", 0) >= t.get("total", 1) - 5, t)
stop_timer()
req("PUT", "/api/timer", {k: CFG0["timer"][k] for k in ("unit", "value", "warn_count", "warn_first_s")})
health("checkin")

# ---------------------------------------------------------------- 7
print("== 7) ek dosya uç değerleri")
req("PUT", "/api/groups/0", {"name": "Zorlama", "subject": "s", "body": "b", "recipients": "a@b.ch"})


def upload(name, size):
    c = http.client.HTTPConnection(HOST, 80, timeout=180)
    try:
        c.request("PUT", f"/api/groups/0/files/{name}", body=b"\xA5" * size, headers={"X-LS-Request": "1", "Host": HOST})
        r = c.getresponse()
        r.read()
        return r.status
    except OSError:
        return 0
    finally:
        c.close()


s = upload("tam2mb.bin", 2 * 1024 * 1024)
check("tam 2 MB ek kabul", s == 200, s)
s = upload("fazla.bin", 2 * 1024 * 1024 + 1)
check("2 MB + 1 bayt reddedildi (413)", s == 413, s)
s = upload("ikinci.bin", 2 * 1024 * 1024)
s3 = upload("ucuncu.bin", 1024)
check("grup toplamı 4 MB'ı aşınca reddedildi", s == 200 and s3 in (409, 413), (s, s3))
fl = data("/api/groups/0/files")
check("ek listesi tutarlı", fl.get("total", 0) <= 4 * 1024 * 1024, fl)
for f in fl.get("files", []):
    req("DELETE", f"/api/groups/0/files/{f['name']}", headers=W)
req("DELETE", "/api/groups/0", headers=W)
health("ekler")

# ---------------------------------------------------------------- 8
print("== 8) art arda yeniden başlatma (3 kez, beklemeden)")
b0 = BASE["boot_count"]
for i in range(3):
    s, _ = req("POST", "/api/device/restart", headers=W)
    d = wait_up(b0 + i, 90)
    check(f"yeniden başlatma {i + 1}: cihaz döndü", bool(d), d)
    if d:
        check(f"yeniden başlatma {i + 1}: neden kullanıcı/yazılım (çökme değil)", d.get("last_cat") not in ("panic", "wdt"), d.get("last_cause"))
BASE["boot_count"] = device().get("boot_count", b0 + 3)
health("restart")

# ---------------------------------------------------------------- 9
if PORT and os.path.exists(IDF_PY):
    print("== 9) yazma sırasında elektrik kesintisi benzeri reset (3 kez)")
    RESET = ("import serial,time,sys;s=serial.Serial(sys.argv[1],115200);"
             "s.dtr=False;s.rts=True;time.sleep(0.1);s.rts=False;s.close()")
    for k in range(3):
        b0 = device().get("boot_count", 0)
        stop = time.time() + 4
        def writer():
            while time.time() < stop:
                req("PUT", "/api/config/relay", {"delay_s": random.randint(0, 60)}, timeout=3)
                req("PUT", "/api/config/device", {"name": f"LS-Zorlama-{random.randint(0, 999)}"}, timeout=3)
        wt = threading.Thread(target=writer)
        wt.start()
        time.sleep(random.uniform(0.5, 3.0))
        subprocess.run([IDF_PY, "-c", RESET, PORT], capture_output=True, timeout=20)
        wt.join()
        d = wait_up(b0, 90)
        check(f"reset {k + 1}: cihaz döndü", bool(d), d)
        allok = all(req("GET", f"/api/config/{sec}")[0] == 200 for sec in ("relay", "device", "timer", "actions", "smtp", "wifi_p"))
        check(f"reset {k + 1}: tüm ayar bölümleri okunuyor", allok)
        check(f"reset {k + 1}: ağ ayarı korundu (WiFi bağlı)", data("/api/status").get("wifi", {}).get("state") == "connected")
        b = data("/api/backup")
        check(f"reset {k + 1}: yedek geçerli", b.get("available") is True, b)
    req("PUT", "/api/config/relay", {"delay_s": CFG0["relay"].get("delay_s", 0)})
    req("PUT", "/api/config/device", {"name": CFG0["device"].get("name", "")})
    BASE["boot_count"] = device().get("boot_count", 0)
    health("reset")
else:
    print("== 9) reset bölümü atlandı (--port verilmedi ya da IDF python yok)")

print("== son: ayarlar başlangıçtaki gibi")
for sec, v in CFG0.items():
    now = data(f"/api/config/{sec}")
    check(f"{sec} başlangıçtaki gibi", now == v, (v, now))
print(f"== sonuç: {fails} hata")
sys.exit(1 if fails else 0)
