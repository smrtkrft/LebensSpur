#!/usr/bin/env python3
"""LebensSpur 2611 — HTTP API sözleşme testi (yalnız stdlib). Kullanım: test_api.py <cihaz-ip>
Cihazın durumunu değiştirir (sayaç kurar, iptal eder); tezgâh cihazında çalıştırılır."""
import http.client, json, sys

HOST = sys.argv[1]
fails = 0
cookie = None


def req(method, path, body=None, headers=None, raw=None, host=None):
    c = http.client.HTTPConnection(HOST, 80, timeout=8)
    h = {"Host": host or HOST}
    if cookie:
        h["Cookie"] = cookie
    if body is not None:
        raw = json.dumps(body).encode()
    if raw is not None:
        h["Content-Type"] = "application/json"
        h["X-LS-Request"] = "1"
    if headers:
        h.update(headers)
    c.request(method, path, body=raw, headers=h)
    r = c.getresponse()
    data = r.read()
    try:
        j = json.loads(data)
    except Exception:
        j = None
    return r.status, j, r


def check(name, cond, info=""):
    global fails
    print(("  OK   " if cond else "  HATA ") + name + ("" if cond else f"  -> {info}"))
    if not cond:
        fails += 1


print("== temel")
# önkoşul: GUI denemelerinden kalan durumdan bağımsız başla (ilk uyarı otomatik, sayaç durmuş, tatil yok)
req("PUT", "/api/timer", {"unit": "min", "value": 4, "warn_count": 2, "warn_first_s": 0}, headers={"X-LS-Request": "1"})
req("DELETE", "/api/timer/vacation", headers={"X-LS-Request": "1"})
req("POST", "/api/timer/cancel", headers={"X-LS-Request": "1"})
s, j, _ = req("GET", "/api/auth"); check("auth durumu", s == 200 and j["ok"], (s, j))
s, j, _ = req("GET", "/api/status"); check("status", s == 200 and "timer" in j["data"], (s, j))
s, j, _ = req("GET", "/api/yok"); check("bilinmeyen uç 404", s == 404 and j["error"]["code"] == "not_found", (s, j))
s, j, _ = req("DELETE", "/api/status", headers={"X-LS-Request": "1"}); check("yanlış yöntem 405", s == 405, (s, j))
s, j, _ = req("GET", "/api/config/yok"); check("bilinmeyen bölüm 404", s == 404, (s, j))

print("== CSRF / gövde")
s, j, _ = req("PUT", "/api/timer", {"unit": "hour", "value": 5, "warn_count": 2})
s, j, _ = req("POST", "/api/timer/cancel"); check("X-LS-Request yok → 403 (durdur)", s == 403, (s, j))
s, j, _ = req("POST", "/api/timer/cancel", headers={"X-LS-Request": "1"}, host="evil.com")
s, j, _ = req("POST", "/api/timer/cancel", headers={"X-LS-Request": "1", "Origin": "http://evil.com"})
s, j, _ = req("GET", "/api/status")
check("reddedilen istekler durumu DEĞİŞTİRMEDİ", j["data"]["timer"]["state"] == "running", j["data"]["timer"])
s, j, _ = req("PUT", "/api/timer", raw=b'{"unit":"min","value":', headers={"X-LS-Request": "1"})
s, j, _ = req("GET", "/api/status")
check("bozuk gövde işleyiciye ulaşmadı", j["data"]["timer"]["total"] == 18000, j["data"]["timer"])
s, j, _ = req("POST", "/api/timer/checkin"); check("X-LS-Request yok → 403", s == 403, (s, j))
s, j, _ = req("POST", "/api/timer/checkin", headers={"X-LS-Request": "1"}, host="evil.com")
check("yabancı Host → 403", s == 403, (s, j))
s, j, _ = req("POST", "/api/timer/checkin", headers={"X-LS-Request": "1", "Origin": "http://evil.com"})
check("yabancı Origin → 403", s == 403, (s, j))
s, j, _ = req("PUT", "/api/timer", raw=b"{bozuk"); check("bozuk JSON → 400", s == 400, (s, j))
s, j, _ = req("PUT", "/api/timer", raw=b"[" * 2000 + b"]" * 2000)
check("2 KB üstü → 413", s == 413, (s, j))
s, j, _ = req("PUT", "/api/timer", raw=b"[" * 1000 + b"]" * 1000)
check("derin iç içe JSON → 400, çökme yok", s == 400, (s, j))
s, j, _ = req("PUT", "/api/timer", raw=b"{}", headers={"Content-Type": "text/plain"})
check("Content-Type text/plain → 400", s == 400, (s, j))

print("== sayaç")
s, j, _ = req("PUT", "/api/timer", {"unit": "day", "value": 2, "warn_count": 3})
check("2 gün / 3 uyarı", s == 200 and j["data"]["warn_plan"][0] == 43200 and j["data"]["warn_plan"][-1] == 7200, (s, j))
s, j, _ = req("PUT", "/api/timer", {"unit": "min", "value": 10, "warn_count": 10})
check("10 dk / 10 uyarı → 400 + en çok", s == 400 and j["error"]["field"] == "warn_count", (s, j))
s, j, _ = req("PUT", "/api/timer", {"unit": "week", "value": 1})
check("geçersiz birim → 400", s == 400 and j["error"]["field"] == "unit", (s, j))
s, j, _ = req("PUT", "/api/timer", {"unit": "hour", "value": 24, "warn_count": 4})
check("24 sa / 4", s == 200 and j["data"]["warn_plan"] == [21600, 11887, 6542, 3600], (s, j))
s, j, _ = req("GET", "/api/status")
check("çalışıyor", j["data"]["timer"]["state"] == "running" and j["data"]["timer"]["total"] == 86400, j)
s, j, _ = req("POST", "/api/timer/vacation", {"days": 2}); check("tatil", s == 200, (s, j))
s, j, _ = req("POST", "/api/timer/vacation", {"days": 61}); check("tatil 61 → 400", s == 400, (s, j))
s, j, _ = req("GET", "/api/status"); check("durum tatil", j["data"]["timer"]["state"] == "vacation", j)
s, j, _ = req("DELETE", "/api/timer/vacation", headers={"X-LS-Request": "1"}); check("tatil iptal", s == 200, (s, j))
s, j, _ = req("POST", "/api/timer/checkin", headers={"X-LS-Request": "1"}); check("erteleme", s == 200, (s, j))
s, j, _ = req("POST", "/api/timer/cancel", headers={"X-LS-Request": "1"}); check("durdur", s == 200, (s, j))
s, j, _ = req("GET", "/api/status"); check("durum durdu", j["data"]["timer"]["state"] == "stopped", j)

print("== yapılandırma")
s, j, _ = req("PUT", "/api/config/smtp", {"server": "smtp.gmail.com", "port": 465, "pass": "gizli"})
check("smtp kaydet", s == 200 and j["data"]["pass"] == "***", (s, j))
s, j, _ = req("GET", "/api/config/smtp"); check("parola maskeli", j["data"]["pass"] == "***", j)
s, j, _ = req("PUT", "/api/config/smtp", {"port": 70000}); check("port 70000 → 400", s == 400 and j["error"]["field"] == "port", (s, j))
s, j, _ = req("PUT", "/api/config/smtp", {"nope": 1}); check("bilinmeyen alan → 400", s == 400, (s, j))
s, j, _ = req("PUT", "/api/config/auth", {"hash": "x"}); check("internal alan → 400", s == 400, (s, j))
s, j, _ = req("GET", "/api/config/auth"); check("hash görünmez", "hash" not in j["data"], j)
s, j, _ = req("PUT", "/api/config/early_mail", {"subject": "a\r\nBcc: x@y"}); check("başlık enjeksiyonu → 400", s == 400, (s, j))
s, j, _ = req("GET", "/api/wifi"); check("wifi", s == 200 and j["data"]["state"] == "connected", j)

print("== kimlik")
s, j, _ = req("PUT", "/api/auth/password", {"current": "", "next": "abc"}); check("kısa parola → 400", s == 400, (s, j))
import socket, time as _t
sse = socket.create_connection((HOST, 80), timeout=5)
sse.sendall(f"GET /api/events HTTP/1.1\r\nHost: {HOST}\r\n\r\n".encode())
first = sse.recv(4096)
check("SSE akışı açıldı (parolasız)", b"200" in first.split(b"\r\n")[0], first[:60])
s, j, _ = req("PUT", "/api/auth/password", {"current": "", "next": "test1234"}); check("parola koy", s == 200, (s, j))
closed = False
t0 = _t.time()
while _t.time() - t0 < 10:
    try:
        d = sse.recv(4096)
        if not d:
            closed = True
            break
    except socket.timeout:
        pass
    except OSError:
        closed = True
        break
sse.close()
check("parola konunca açık SSE akışı kapandı", closed, f"{_t.time() - t0:.1f} s")
s, j, _ = req("GET", "/api/status"); check("oturumsuz → 401", s == 401 and j["error"]["code"] == "auth_required", (s, j))
s, j, _ = req("POST", "/api/auth/login", {"password": "yanlis"}); check("yanlış parola → 401", s == 401, (s, j))
s, j, r = req("POST", "/api/auth/login", {"password": "test1234"})
sc = r.getheader("Set-Cookie") or ""
check("giriş + çerez", s == 200 and sc.startswith("ls_sess=") and "HttpOnly" in sc and "SameSite=Strict" in sc, (s, sc))
cookie = sc.split(";")[0]
s, j, _ = req("GET", "/api/status"); check("oturumla status", s == 200, (s, j))
s, j, _ = req("PUT", "/api/auth/password", {"current": "test1234", "next": ""}); check("parola kaldır", s == 200, (s, j))
cookie = None
s, j, _ = req("GET", "/api/status"); check("parolasız tekrar açık", s == 200, (s, j))

print("== WiFi kaydı yanıtı")
t0 = _t.time()
s, j, _ = req("PUT", "/api/config/wifi_b", {"hidden": False})
check("WiFi bölümü kaydı yanıt döndürdü (yeniden bağlanmadan önce)", s == 200, (s, j, f"{_t.time() - t0:.1f} s"))
_t.sleep(10)
t0 = _t.time()
s, j, _ = req("PUT", "/api/config/device", {"ap_enabled": True})
check("AP bölümü kaydı yanıt döndürdü", s == 200, (s, j, f"{_t.time() - t0:.1f} s"))
_t.sleep(5)

print("== mail grupları ve ekler")
import time
s, j, _ = req("PUT", "/api/groups/0", {"name": "Aile", "subject": "Acil", "body": "Mesaj", "recipients": "a@b.ch, c@d.ch"})
check("grup kaydet", s == 200 and j["data"]["name"] == "Aile", (s, j))
s, j, _ = req("GET", "/api/groups"); check("grup listesi", s == 200 and j["data"][0]["recipients"] == 2, j)
s, j, _ = req("PUT", "/api/groups/10", {"name": "x"}); check("grup 10 → 404", s == 404, (s, j))
def upload(g, name, data):
    c = http.client.HTTPConnection(HOST, 80, timeout=20)
    c.request("PUT", f"/api/groups/{g}/files/{name}", body=data, headers={"X-LS-Request": "1", "Host": HOST})
    r = c.getresponse(); b = r.read()
    return r.status, json.loads(b) if b else None
s, j = upload(0, "not%20a.txt", b"merhaba\n" * 1000); check("ek yükle (URL kodlu ad)", s == 200, (s, j))
s, j = upload(0, "..%2Fka%C3%A7%C4%B1%C5%9F", b"x"); check("geçersiz ad → 400", s == 400, (s, j))
s, j = upload(5, "a.txt", b"x"); check("kaydedilmemiş gruba → 409", s == 409, (s, j))
s, j, _ = req("GET", "/api/groups/0/files"); check("ek listesi", s == 200 and j["data"]["files"][0]["name"] == "not a.txt" and j["data"]["files"][0]["size"] == 8000, j)
s, j, _ = req("DELETE", "/api/groups/0/files/not%20a.txt", headers={"X-LS-Request": "1"}); check("ek sil", s == 200, (s, j))

print("== kanal testleri (gerçek ağ)")
def run_test(ch, wait=40):
    s, j, _ = req("POST", f"/api/test/{ch}", headers={"X-LS-Request": "1"})
    if s != 200: return s, j
    jid = j["data"]["job_id"]
    for _ in range(wait):
        time.sleep(1)
        s, j, _ = req("GET", f"/api/jobs/{jid}")
        if j["data"]["state"] in ("done", "failed"): return s, j["data"]
    return s, j["data"]
s, d = run_test("smtp"); check("smtp ayarsız → ayar hatası", d["state"] == "failed", d)
req("PUT", "/api/config/webhook", {"url": "https://httpbin.org/post", "method": "POST"})
s, d = run_test("webhook"); check("webhook https (CA paketi)", d["state"] == "done", d)
req("PUT", "/api/config/mqtt", {"url": "mqtt://broker.hivemq.com", "topic": "lebensspur/2611/test"})
s, d = run_test("mqtt"); check("mqtt broker.hivemq.com (düz)", d["state"] == "done", d)
req("PUT", "/api/config/mqtt", {"url": "mqtts://broker.hivemq.com:8883"})
s, d = run_test("mqtt"); check("mqtts broker.hivemq.com:8883 (TLS)", d["state"] == "done", d)
s, j, _ = req("GET", "/api/config/actions")
check("testi geçen kanallar otomatik açıldı", j["data"]["early_webhook"] and j["data"]["trig_mqtt"], j["data"])
s, j, _ = req("DELETE", "/api/groups/0", headers={"X-LS-Request": "1"}); check("grup sil", s == 200, (s, j))
s, j, _ = req("GET", "/api/jobs/999"); check("bilinmeyen iş → 404", s == 404, (s, j))

print(f"== sonuç: {fails} hata")
sys.exit(1 if fails else 0)
