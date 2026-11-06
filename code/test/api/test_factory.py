#!/usr/bin/env python3
"""LebensSpur — fabrika ayarı ve yedek önerisi cihaz testi (yalnız stdlib).

Kullanım: test_factory.py <cihaz-ip>

Fabrika ayarı WiFi bilgisini siler. Bu yüzden test, geliştirme kancalı derlemeyle çalışır: cihaz her açılışta
birincil WiFi'yi derleme ortamından yeniden yazar (LS_DEV_WIFI_SSID/PASS). Parola kaynakta ya da git'te tutulmaz;
test bitince cihaz kancasız derlemeyle yeniden yüklenmelidir.

Denetlenen akış:
  1) yedek var → fabrika ayarı (kademe 1) → ayarlar varsayılan, arayüz geri yükleme önerisi alır
  2) karar verilmeden yapılan ayar değişikliği eski yedeğin üzerine YAZMAZ
  3) geri yükle → eski ayarlar döner, öneri kalkar
  4) fabrika ayarı → "yeni başla" → öneri kalkar, güncel ayarlar yeni yedek olur
  5) kademe 2 (harici flash da silinir) → öneri yok, sonraki değişiklik yeni yedek yazar
"""
import http.client, json, sys, time

HOST = sys.argv[1]
W = {"X-LS-Request": "1"}
fails = 0


def req(method, path, body=None, headers=None, timeout=8):
    c = http.client.HTTPConnection(HOST, 80, timeout=timeout)
    h = {"Host": HOST}
    raw = None
    if body is not None:
        raw = json.dumps(body).encode()
        h.update({"Content-Type": "application/json", "X-LS-Request": "1"})
    if headers:
        h.update(headers)
    try:
        c.request(method, path, body=raw, headers=h)
        r = c.getresponse()
        data = r.read()
    except OSError:
        return 0, None
    try:
        return r.status, json.loads(data)
    except Exception:
        return r.status, None


def check(name, cond, info=""):
    global fails
    print(("  OK   " if cond else "  HATA ") + name + ("" if cond else f"  -> {info}"))
    if not cond:
        fails += 1


def data(path):
    s, j = req("GET", path)
    return j["data"] if s == 200 and j else {}


def boots():
    return data("/api/device").get("boot_count", -1)


def wait_reboot(boot_before, limit=120):
    """Cihaz yeniden başlayıp ağa dönene kadar bekler (açılış sayacı artar).
    Çalışma süresine bakılmaz: art arda yeniden başlatmalarda yanıltıcıdır."""
    t0 = time.time()
    time.sleep(5)
    while time.time() - t0 < limit:
        b = boots()
        if b > boot_before:
            time.sleep(2)
            return True
        time.sleep(2)
    return False


def factory(level):
    b = boots()
    s, j = req("POST", "/api/device/factory-reset", {"level": level})
    check(f"fabrika ayarı kademe {level} başladı", s == 200, (s, j))
    check(f"kademe {level} sonrası cihaz geri geldi", wait_reboot(b, 180))


def backup_seq_settled():
    time.sleep(2.5)                         # yedek işi 300 ms birleştirme + yazma
    return data("/api/backup")


print("== önkoşul")
a = data("/api/auth")
if a.get("enabled"):
    print("cihazda parola var; test parolasız cihazda çalışır")
    sys.exit(2)
dev = data("/api/device")
check("cihaz erişilebilir", bool(dev), dev)

print("== 1) yedek var → fabrika ayarı (kademe 1)")
req("PUT", "/api/config/relay", {"delay_s": 7})
b0 = backup_seq_settled()
check("işaretli ayarın yedeği yazıldı", b0.get("available") and not b0.get("offer"), b0)
factory(1)
check("ayarlar varsayılana döndü (röle gecikmesi 0)", data("/api/config/relay").get("delay_s") == 0, data("/api/config/relay"))
b1 = data("/api/backup")
check("geri yükleme önerisi var, yedek aynı", b1.get("offer") is True and b1.get("seq") == b0.get("seq"), (b0, b1))

print("== 2) karar verilmeden yapılan değişiklik eski yedeği korur")
req("PUT", "/api/config/relay", {"delay_s": 3})
b2 = backup_seq_settled()
check("yedek üzerine yazılmadı", b2.get("seq") == b0.get("seq") and b2.get("offer") is True, (b0, b2))

print("== 3) geri yükle")
b = boots()
s, j = req("POST", "/api/backup/restore", headers=W)
check("geri yükleme başladı", s == 200, (s, j))
check("geri yükleme sonrası cihaz geri geldi", wait_reboot(b))
check("eski ayar döndü (röle gecikmesi 7)", data("/api/config/relay").get("delay_s") == 7, data("/api/config/relay"))
check("öneri kalktı", data("/api/backup").get("offer") is False, data("/api/backup"))

print("== 4) fabrika ayarı → yeni başla")
factory(1)
check("öneri var", data("/api/backup").get("offer") is True, data("/api/backup"))
s, j = req("POST", "/api/backup/dismiss", headers=W)
check("yeni başla kabul", s == 200, (s, j))
b4 = backup_seq_settled()
check("öneri kalktı, güncel ayarlar yedeklendi", b4.get("offer") is False and b4.get("available"), b4)
req("PUT", "/api/config/relay", {"delay_s": 2})
b4b = backup_seq_settled()
check("sonraki değişiklik yedeğe yazılıyor", b4b.get("seq", 0) > b4.get("seq", 0), (b4, b4b))

print("== 5) kademe 2 (harici flash da silinir)")
factory(2)
b5 = data("/api/backup")
check("kademe 2 sonrası öneri yok", b5.get("offer") is False, b5)
req("PUT", "/api/config/relay", {"delay_s": 0})
b5b = backup_seq_settled()
check("ilk değişiklik yeni yedek yazdı", b5b.get("available") is True and b5b.get("offer") is False, b5b)

print("== son durum")
check("röle gecikmesi varsayılan", data("/api/config/relay").get("delay_s") == 0, data("/api/config/relay"))
print(f"== sonuç: {fails} hata")
sys.exit(1 if fails else 0)
