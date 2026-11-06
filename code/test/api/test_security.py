#!/usr/bin/env python3
"""
Güvenlik testi — tarayıcı parolası YOKKEN ve VARKEN aynı kontroller, sonuçlar yan yana.

  python3 code/test/api/test_security.py <cihaz-ip>

Kaynak: OWASP API Top 10 (API2 Broken Authentication, API5 BFLA, API8 Misconfiguration), kimlik doğrulama
zafiyet testi becerisinin bu cihaza uyan bölümleri (JWT yok: sunucu tarafı oturum çerezi `ls_sess`).
Uç listesi kaynak koddaki rota tablolarından okunur → yeni uç eklenince test kendiliğinden kapsar.

Bilerek tasarım gereği olanlar (bulgu sayılmaz, raporda "tasarım" yazar):
  - Parola yokken arayüz/API ağdaki herkese açık (ALTE davranışı; GUI kalıcı uyarı gösterir).
  - İlk parola, parola yokken herkes tarafından konabilir (kurulum).
  - GET /api/reset?key= oturumsuz çalışır (dış otomasyon; anahtar + hız sınırı korur).
Sonda cihaz eski haline döner (parola kaldırılır, auth/remote ayarları geri yüklenir).
"""
import http.client
import json
import os
import re
import secrets
import socket
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.11.101"
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PW = "Sec-Test-" + secrets.token_hex(4)            # her koşuda farklı test parolası
rows = []                                          # (kontrol, mod, sonuç, ayrıntı)
fails = 0


def req(method, path, body=None, headers=None, cookie=None, raw=None, host=None, timeout=10):
    c = http.client.HTTPConnection(HOST, 80, timeout=timeout)
    h = {"Host": host or HOST}
    if headers:
        h.update(headers)
    if cookie:
        h["Cookie"] = cookie
    data = raw
    if body is not None:
        data = json.dumps(body)
        h.setdefault("Content-Type", "application/json")
    try:
        c.request(method, path, body=data, headers=h)
        r = c.getresponse()
        txt = r.read()
        try:
            j = json.loads(txt)
        except Exception:
            j = None
        return r.status, j, r, txt
    except Exception as e:
        return 0, None, None, str(e).encode()
    finally:
        c.close()


W = {"X-LS-Request": "1"}


def check(name, mode, ok, info="", design=False):
    global fails
    res = "TASARIM" if (design and not ok) else ("OK" if ok else "HATA")
    if res == "HATA":
        fails += 1
    rows.append((name, mode, res, str(info)[:140]))
    print(f"  {res:7s} [{mode}] {name}" + ("" if ok else f"  -> {str(info)[:140]}"))


def routes():
    """rota tablolarını kaynaktan oku: (yol, yöntem, bayraklar)"""
    out = []
    for f in ("api.c", "api_groups.c", "api_sys.c"):
        src = open(os.path.join(ROOT, "components/app/api", f), encoding="utf-8").read()
        for m in re.finditer(r'\{\s*"(/api/[^"]*)",\s*HTTP_(GET|POST|PUT|DELETE),\s*([^,]+),', src):
            out.append((m.group(1), m.group(2), m.group(3).strip()))
    return out


SAMPLE = {"/api/config/{}": "timer", "/api/wifi/{}": "b", "/api/groups/{}": "9", "/api/test/{}": "relay",
          "/api/jobs/{}": "1", "/api/groups/{}/files": "9", "/api/groups/{}/files/{}": ("9", "x.txt")}


def fill(path):
    v = SAMPLE.get(path)
    if isinstance(v, tuple):
        a, b = v
        return path.replace("{}", a, 1).replace("{}", b, 1)
    return path.replace("{}", v or "x")


PUBLIC = {("/api/auth", "GET"), ("/api/auth/login", "POST"), ("/api/reset", "GET"),
          ("/api/auth/logout", "POST")}       # çıkış oturumsuz da zararsız (çerezi siler)


def login(pw):
    s, j, r, _ = req("POST", "/api/auth/login", {"password": pw}, W)
    sc = (r.getheader("Set-Cookie") if r else "") or ""
    return s, sc, sc.split(";")[0] if sc else None


def status_state():
    s, j, _, _ = req("GET", "/api/status", cookie=CK.get("c"))
    return j["data"]["timer"]["state"] if s == 200 else None


CK = {}


def common(mode):
    """her iki modda aynı yapılan kontroller"""
    ck = CK.get("c")
    # --- CSRF / DNS rebinding (oturum olsa bile) ---
    s, *_ = req("POST", "/api/timer/cancel", cookie=ck)
    check("CSRF: X-LS-Request yoksa yazma reddedilir", mode, s == 403, s)
    s, *_ = req("POST", "/api/timer/cancel", headers={**W, "Origin": "http://evil.example"}, cookie=ck)
    check("CSRF: yabancı Origin reddedilir", mode, s == 403, s)
    s, *_ = req("POST", "/api/timer/cancel", headers=W, cookie=ck, host="evil.example")
    check("DNS rebinding: yabancı Host reddedilir", mode, s == 403, s)
    s, *_ = req("PUT", "/api/config/relay", raw='{"delay_s":1}', headers={**W, "Content-Type": "text/plain"}, cookie=ck)
    check("JSON uçları text/plain gövdeyi reddeder (form CSRF)", mode, s in (400, 415), s)
    s, *_ = req("POST", "/api/auth/login", {"password": "x"})
    check("giriş de CSRF korumalı (login CSRF)", mode, s == 403, s)
    # --- güvenlik başlıkları ---
    s, _, r, _ = req("GET", "/")
    h = {k.lower(): v for k, v in (r.getheaders() if r else [])}
    xfo = h.get("x-frame-options", "").upper() == "DENY" or "frame-ancestors 'none'" in h.get("content-security-policy", "")
    check("arayüz başka siteye gömülemez (clickjacking: XFO/CSP frame-ancestors)", mode, xfo, h.get("x-frame-options"))
    check("X-Content-Type-Options: nosniff", mode, h.get("x-content-type-options") == "nosniff", h.get("x-content-type-options"))
    check("Referrer-Policy: no-referrer", mode, h.get("referrer-policy") == "no-referrer", h.get("referrer-policy"))
    csp = h.get("content-security-policy", "")
    check("CSP: yalnız kendi betikleri (script-src 'self')", mode, "script-src 'self'" in csp or "default-src 'self'" in csp, csp)
    s, _, r, _ = req("GET", "/api/auth")
    check("API yanıtları önbelleğe alınmaz (no-store)", mode, r is not None and "no-store" in (r.getheader("Cache-Control") or ""),
          r.getheader("Cache-Control") if r else None)
    # --- bilgi sızıntısı ---
    s, j, _, txt = req("GET", "/api/auth")
    check("/api/auth parola özeti sızdırmaz", mode, b"hash" not in txt, txt[:80])
    for sec in ("smtp", "wifi_p", "wifi_b", "device", "telegram", "mqtt", "remote", "auth"):
        s, j, _, txt = req("GET", "/api/config/" + sec, cookie=ck)
        if s != 200:
            continue
        d = j["data"]
        leak = [k for k in ("pass", "ap_pass", "token", "key") if k in d and d[k] not in ("", "***")]
        check(f"config/{sec}: gizli alanlar maskeli", mode, not leak and "hash" not in d, leak or d.get("hash"))
    s, j, _, txt = req("GET", "/api/logs", cookie=ck)
    if s == 200:
        check("günlükte test parolası geçmez", mode, PW.encode() not in txt, "parola günlükte")
    s, j, _, txt = req("GET", "/api/yok-boyle-bir-uc")
    check("bilinmeyen uç 404, iç ayrıntı yok", mode, s in (401, 404) and b"/home" not in txt and b"esp_" not in txt, (s, txt[:80]))
    # --- yol geçişi / dosya erişimi ---
    for p in ("/../data/config/config.jsonl", "/data/config/config.jsonl", "/s/../../data/config/config.jsonl",
              "/s/%2e%2e/%2e%2e/data/config/config.jsonl", "/log/events.jsonl", "/.ls2611"):
        s, j, _, txt = req("GET", p, cookie=ck)
        check(f"dosya sistemine yol yok: {p}", mode, b"\"schema\"" not in txt and b"ssid" not in txt and b"\"type\":\"boot\"" not in txt, (s, txt[:60]))
    for name in ("..%2F..%2Fconfig%2Fconfig.jsonl", "%2Fdata%2Fx", "a%00b.txt", "..", "a%2Fb"):
        s, *_ = req("PUT", f"/api/groups/0/files/{name}", raw=b"x", headers={**W, "Content-Type": "application/octet-stream"}, cookie=ck)
        check(f"ek adı yol geçişi reddedilir: {name}", mode, s in (400, 401, 404, 409), s)
    # --- bozuk/aşırı girdi ---
    s, *_ = req("PUT", "/api/config/relay", raw="{" * 40 + "}" * 40, headers=W, cookie=ck)
    check("derin iç içe JSON reddedilir", mode, s in (400, 401), s)
    s, *_ = req("PUT", "/api/config/relay", raw="x" * 5000, headers=W, cookie=ck)
    check("2 KB üstü gövde 413", mode, s in (413, 401), s)
    s, *_ = req("PUT", "/api/config/relay", raw="{bozuk", headers=W, cookie=ck)
    check("bozuk JSON 400", mode, s in (400, 401), s)
    s, *_ = req("GET", "/api/status?" + "a" * 600, cookie=ck)
    check("aşırı uzun URI düzgün reddedilir", mode, s in (414, 400, 404, 431), s)
    s, *_ = req("GET", "/api/status", headers={"X-Long": "b" * 3000}, cookie=ck)
    check("aşırı uzun başlık düzgün reddedilir", mode, s in (431, 400), s)
    s, *_ = req("POST", "/api/auth/login", {"password": "p" * 1500}, W)
    ok = s in (400, 401, 413, 429) or (mode == "parolasız" and s == 200)   # parolasızken giriş zaten açık
    check("çok uzun parola denemesi çökertmez", mode, ok, s)
    s, *_ = req("GET", "/api/auth")
    check("bozuk girdilerden sonra cihaz yanıt veriyor", mode, s == 200, s)
    # --- bağlantı tüketme (hafif slowloris) ---
    socks = []
    for _ in range(12):
        try:
            so = socket.create_connection((HOST, 80), timeout=3)
            so.sendall(b"GET /api/auth HTTP/1.1\r\nHost: " + HOST.encode() + b"\r\n")   # başlık tamamlanmaz
            socks.append(so)
        except Exception:
            break
    time.sleep(1)
    up0 = None
    t0 = time.time()
    s, *_ = req("GET", "/api/auth", timeout=30)
    dt = time.time() - t0
    for so in socks:
        so.close()
    # httpd tek görev: her yarım istek recv_wait_timeout (2 s) kadar bekletir; ≤ 10 soket → en çok ~20 s
    check("12 yarım bağlantıda arayüz ≤ 25 s içinde yanıt veriyor", mode, s == 200 and dt <= 25, f"{s}, {dt:.1f} s")
    time.sleep(2)
    s, j, *_ = req("GET", "/api/status", cookie=ck)
    check("yarım bağlantılar sonrası çekirdek çalışıyor", mode, s == 200 and j["data"]["uptime"] > 0, s)
    time.sleep(2)
    # --- uzaktan sıfırlama anahtarı ---
    req("PUT", "/api/config/remote", {"enabled": True}, W, cookie=ck)
    s, j, *_ = req("POST", "/api/remote/key", headers=W, cookie=ck)
    key = j["data"]["key"] if s == 200 else ""
    check("uzaktan anahtar 26 kr base32", mode, bool(re.fullmatch(r"[A-Z2-7]{26}", key)), key)
    keys = set()
    for _ in range(3):
        s, j, *_ = req("POST", "/api/remote/key", headers=W, cookie=ck)
        keys.add(j["data"]["key"] if s == 200 else "")
    check("anahtarlar her seferinde farklı (rastgele)", mode, len(keys) == 3, keys)
    s, j, *_ = req("GET", "/api/remote/key", cookie=ck)
    key = j["data"]["key"] if s == 200 else ""
    codes = []
    for i in range(6):
        s, *_ = req("GET", "/api/reset?key=" + ("A" * 25 + str(i + 2)))
        codes.append(s)
    check("yanlış anahtar 5 kez → kilit (429)", mode, codes[-1] == 429, codes)
    s, *_ = req("GET", "/api/reset?key=" + key)
    check("kilit sürerken doğru anahtar da reddedilir", mode, s == 429, s)
    s, j, _, txt = req("GET", "/api/logs", cookie=ck)
    check("günlükte uzaktan anahtar geçmez", mode, key.encode() not in txt, "anahtar günlükte")
    req("PUT", "/api/config/remote", {"enabled": False}, W, cookie=ck)


def main():
    global fails
    s, j, *_ = req("GET", "/api/auth")
    if s != 200:
        sys.exit(f"cihaz yanıt vermiyor: {s}")
    if j["data"]["enabled"]:
        sys.exit("cihazda parola var; test parolasız başlamalı")
    s, j, *_ = req("GET", "/api/config/auth")
    auth0 = j["data"]
    req("POST", "/api/timer/cancel", headers=W)
    rlist = routes()
    print(f"== {len(rlist)} uç kaynaktan okundu")

    # ================= MOD A: parola yok =================
    mode = "parolasız"
    print(f"== {mode}")
    CK.clear()
    for path, meth, flags in rlist:
        if meth != "GET" or path == "/api/events":
            continue
        s, *_ = req("GET", fill(path))
        if (path, meth) in PUBLIC:
            continue
        check(f"GET {path} oturumsuz", mode, s == 401, f"açık ({s})", design=True)   # parolasız: tasarım gereği açık
    common(mode)

    # ================= geçiş: parola konur =================
    s, j, *_ = req("PUT", "/api/auth/password", {"current": "", "next": PW}, W)
    if s != 200:
        sys.exit(f"parola konamadı: {s} {j}")
    check("ilk parola oturumsuz konur (kurulum)", "geçiş", False, "parolasızken herkes koyabilir", design=True)

    # ================= MOD B: parola var =================
    mode = "parolalı"
    print(f"== {mode}")
    CK.clear()
    # --- tüm uçlar oturumsuz 401 (yazma uçları etkisiz) ---
    before = None
    s0, sc, ck = login(PW)
    s, j, *_ = req("GET", "/api/status", cookie=ck)
    before = j["data"]["timer"]["state"]
    for path, meth, flags in rlist:
        if (path, meth) in PUBLIC:
            continue
        body = {} if meth in ("POST", "PUT") else None
        s, *_ = req(meth, fill(path), body, W)
        check(f"{meth} {path} oturumsuz → 401", mode, s == 401, s)
    s, j, *_ = req("GET", "/api/status", cookie=ck)
    check("reddedilen yazma istekleri durumu değiştirmedi", mode, j["data"]["timer"]["state"] == before, j["data"]["timer"]["state"])
    s, *_ = req("GET", "/")
    check("arayüz dosyaları (giriş ekranı) oturumsuz yüklenir", mode, s == 200, s)
    # --- oturum çerezi ---
    check("giriş başarılı", mode, s0 == 200, s0)
    check("çerez HttpOnly + SameSite=Strict + Path=/", mode, "HttpOnly" in sc and "SameSite=Strict" in sc and "Path=/" in sc, sc)
    tok = ck.split("=", 1)[1] if ck else ""
    check("oturum anahtarı 128 bit (32 hex)", mode, bool(re.fullmatch(r"[0-9a-f]{32}", tok)), tok)
    toks = {login(PW)[2] for _ in range(3)}
    check("her girişte farklı anahtar", mode, len(toks) == 3, len(toks))
    for bad in ("ls_sess=" + secrets.token_hex(16), "ls_sess=" + tok[:-1] + ("0" if tok[-1] != "0" else "1"),
                "ls_sess=", "ls_sess=" + "f" * 300, "ls_sess=" + tok + "x", "ls_sess=%00" + tok[3:]):
        s, *_ = req("GET", "/api/status", cookie=bad)
        check(f"sahte/bozuk çerez reddedilir ({bad[:20]}…)", mode, s == 401, s)
    s, *_ = req("GET", "/api/status", headers={"Authorization": "Bearer " + tok})
    check("anahtar başlıkta/URL'de kabul edilmez (yalnız çerez)", mode, s == 401, s)
    s, *_ = req("GET", "/api/status?ls_sess=" + tok)
    check("anahtar URL parametresiyle kabul edilmez", mode, s == 401, s)
    # --- SSE ---
    s, *_ = req("GET", "/api/events", timeout=3)
    check("SSE oturumsuz → 401", mode, s == 401, s)
    # --- parola değişimi ---
    s, *_ = req("PUT", "/api/auth/password", {"current": PW, "next": PW + "2"}, W)
    check("oturumsuz parola değişimi → 401", mode, s == 401, s)
    s, *_ = req("PUT", "/api/auth/password", {"current": "yanlis", "next": PW + "2"}, W, cookie=ck)
    check("yanlış mevcut parolayla değişim → 403", mode, s == 403, s)
    s, *_ = req("PUT", "/api/auth/password", {"current": PW, "next": "abc"}, W, cookie=ck)
    check("kısa yeni parola reddedilir", mode, s == 400, s)
    # --- çıkış iptali (sunucu tarafı) ---
    _, _, ck2 = login(PW)
    req("POST", "/api/auth/logout", headers=W, cookie=ck2)
    s, *_ = req("GET", "/api/status", cookie=ck2)
    check("çıkıştan sonra eski çerez geçersiz", mode, s == 401, s)
    # --- yanlış parola: aynı yanıt, kilit ---
    s1, j1, *_ = req("POST", "/api/auth/login", {"password": "yanlis1"}, W)
    s2, j2, *_ = req("POST", "/api/auth/login", {"password": ""}, W)
    check("yanlış parola yanıtları ayırt edilemez", mode, s1 == s2 == 401 and j1 == j2, (s1, s2))
    CK["c"] = ck
    common(mode)
    req("PUT", "/api/config/auth", {"lock_min": 1, "protect": True}, W, cookie=ck)
    codes = []
    for i in range(6):
        s, *_ = req("POST", "/api/auth/login", {"password": f"yanlis{i}"}, W)
        codes.append(s)
    check("5 yanlış giriş → kilit (429)", mode, 429 in codes, codes)
    s, *_ = req("POST", "/api/auth/login", {"password": PW}, W)
    check("kilitliyken doğru parola da reddedilir", mode, s == 429, s)
    s, *_ = req("POST", "/api/auth/login", {"password": PW}, {**W, "X-Forwarded-For": "10.9.8.7"})
    check("X-Forwarded-For ile kilit atlatılamaz", mode, s == 429, s)
    s, j, *_ = req("GET", "/api/status", cookie=ck)
    check("kilit mevcut oturumu düşürmez", mode, s == 200, s)
    # --- oturum süresi dolması ---
    req("PUT", "/api/config/auth", {"ttl_min": 1}, W, cookie=ck)
    print("  … oturum süresi testi (65 s bekleniyor)")
    time.sleep(65)
    s, *_ = req("GET", "/api/status", cookie=ck)
    check("1 dk hareketsizlikten sonra oturum düşer", mode, s == 401, s)
    print("  … giriş kilidinin bitmesi bekleniyor")
    for _ in range(40):
        s, sc, ck = login(PW)
        if s == 200:
            break
        time.sleep(15)
    check("kilit süresi bitince giriş açılır", mode, s == 200, s)
    # --- parola değişince tüm oturumlar düşer ---
    _, _, ck3 = login(PW)
    s, *_ = req("PUT", "/api/auth/password", {"current": PW, "next": PW + "2"}, W, cookie=ck)
    check("parola değişti", mode, s == 200, s)
    s1, *_ = req("GET", "/api/status", cookie=ck)
    s2, *_ = req("GET", "/api/status", cookie=ck3)
    check("parola değişince tüm eski oturumlar geçersiz", mode, s1 == 401 and s2 == 401, (s1, s2))

    # ================= temizlik =================
    _, _, ck = login(PW + "2")
    req("PUT", "/api/config/auth", {k: auth0[k] for k in ("ttl_min", "protect", "lock_min")}, W, cookie=ck)
    s, *_ = req("PUT", "/api/auth/password", {"current": PW + "2", "next": ""}, W, cookie=ck)
    s, j, *_ = req("GET", "/api/auth")
    check("temizlik: parola kaldırıldı", "temizlik", s == 200 and not j["data"]["enabled"], j)

    # ================= karşılaştırma tablosu =================
    print("\n== karşılaştırma (aynı kontrol iki modda)")
    by = {}
    for name, mode, res, info in rows:
        by.setdefault(name, {})[mode] = res
    for name, m in by.items():
        if "parolasız" in m and "parolalı" in m:
            print(f"  {m['parolasız']:7s} | {m['parolalı']:7s} | {name}")
    design = sum(1 for r in rows if r[2] == "TASARIM")
    print(f"== sonuç: {fails} hata, {design} tasarım gereği açık, {len(rows)} kontrol")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
