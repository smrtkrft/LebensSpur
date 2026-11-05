# 02 — Araştırma: TLS ve kanallar (SMTP, Telegram, webhook, MQTT)

Tarih: 2026-11-05 · Hedef: ESP32-C6, ESP-IDF v5.5.2 · Plan maddeleri: `02-plan-ilk.md` §4.6–4.7, §5.5, §8 soru 2 ve 13.

Kaynak kısaltmaları: `esp_tls.c` = `~/esp/esp-idf-v5.5.2/components/esp-tls/esp_tls.c`, `mbed.c` = `.../esp-tls/esp_tls_mbedtls.c`, `http.c` = `.../esp_http_client/esp_http_client.c`, `tssl.c` = `.../tcp_transport/transport_ssl.c`, `mqtt.c` = `.../mqtt/esp-mqtt/mqtt_client.c`, `smtp.c` = `ALTE/HW/components/smtp_client/smtp_client.c`. Satır numaraları v5.5.2 kaynağındandır. "Canlı ölçüm" = bu belge yazılırken Mac'ten `openssl s_client` ile alınan sonuçlar (ESP değil; sertifika zinciri ve EHLO yetenekleri cihazdan bağımsızdır, süreler değildir).

Doğrulanan tasarım kararı (özet): **Tek `net_tls` yardımcısı + SMTP'de esp-tls ile STARTTLS mümkün ve destekli API ile yapılıyor; CA paketi daima; zaman aşımı modeli "soket 10 s + toplam son tarih"; teslimat seri.** Düzeltme gereken noktalar: "doğrulama kapalı" seçeneği sdkconfig gerektirir (bunun yerine "sertifika yapıştır" önerilir), esp_http_client `timeout_ms` işlem başınadır (toplam değil), esp-mqtt görev yaratır (hafif istemci önerilir), Outlook.com kişisel hesaplar parola ile artık çalışmaz.

---

## 1. SMTP STARTTLS esp-tls ile

### Bulgu

1. **Durum makinesi dışarıdan verilen soketle devam edebiliyor.** `esp_tls_low_level_conn` (`esp_tls.c:462-542`) `tls->conn_state`'e göre `switch` eder. `ESP_TLS_CONNECTING` dalında (`:492-528`) `cfg->non_block == false` ise `select()` atlanır ve doğrudan `create_ssl_handle(hostname, …)` + `ESP_TLS_HANDSHAKE` dalına düşer (`:519-532`). `create_ssl_handle` → `esp_create_mbedtls_handle` **`tls->server_fd.fd = tls->sockfd`** yapar (`mbed.c:129`) ve `mbedtls_ssl_set_bio(&tls->ssl, &tls->server_fd, mbedtls_net_send, mbedtls_net_recv, NULL)` (`mbed.c:198`). Yani el sıkışma, `tls->sockfd`'ye koyduğumuz **mevcut** soket üzerinde yapılır.
2. **Soketi ve durumu dışarıdan koymak için resmi API var:** `esp_tls_set_conn_sockfd` (`esp_tls.c:765-773`, başlık `esp_tls.h:586`) ve `esp_tls_set_conn_state` (`esp_tls.c:785-793`, `esp_tls.h:610`); durum aralığı `ESP_TLS_INIT..ESP_TLS_DONE` denetlenir (`:787`). Enum sırası `INIT=0, CONNECTING, HANDSHAKE, FAIL, DONE` (`esp_tls.h:35-39`); `esp_tls_init()` `calloc` ile `role = ESP_TLS_CLIENT (=0)` bırakır (`esp_tls.c:177`, `esp_tls.h:43`).
3. **Düz TCP tarafı da esp-tls'ten alınır:** `esp_tls_plain_tcp_connect` (`esp_tls.c:547-553`) → `tcp_connect` (`:364-460`): `getaddrinfo`, `SO_RCVTIMEO/SO_SNDTIMEO = timeout_ms` (`:275-292`), **non-blocking `connect()` + `select(timeout_ms)`** (`:383-424`), sonra soket yeniden blocking (`:446-452`). ALTE'nin "`connect()` zaman aşımı yok" bulgusu (harita §2.8/3) böylece kapanır.
4. **`is_tls` bayrağı yalnız `ESP_TLS_INIT` dalında `true` olur** (`esp_tls.c:471-474`). Yükseltme yolunda `false` kalır. Etkisi: `esp_mbedtls_conn_delete` (`mbed.c:455-466`) `mbedtls_net_free` çağırmaz; ama `esp_tls_conn_destroy` (`esp_tls.c:154-173`) `tls->sockfd >= 0` ise `close()` yapar (`:159-161`) ve `esp_mbedtls_cleanup` (ssl/conf/entropy/ctr_drbg/x509 serbest bırakma) her durumda çağrılır (`mbed.c:457`). Sonuç: tek `close`, sızıntı yok. **Olumsuz yan etki yok (koddan çıkarım; cihazda doğrulanmalı).**
5. `esp_tls_conn_new_sync` dönüşü: `1` başarı, `-1` hata, `0` zaman aşımı (`esp_tls.c:555-579`). `0`'da da `esp_tls_conn_destroy` gerekir (tampon ve soket sahipliği `tls`'te).
6. SNI/CN: `set_client_config` `hostname`'i `mbedtls_ssl_set_hostname`'e verir (`mbed.c:879-901`); yükseltme çağrısında da `hostname` geçtiğimiz için SMTP sunucu adı doğrulanır.
7. IDF ağacında bu iki `set_conn_*` API'sini kullanan başka bileşen yok (`grep` boş; yalnız `esp_tls.c/h`). IDF'in `examples/protocols/smtp_client` örneği esp-tls değil, elle mbedTLS kullanır (`smtp_client_example_main.c:21-30, 300-330`) ve ALTE'nin aynı kalıbıdır (verify sonucu yalnız loglanır, `perform_tls_handshake` `:233-240`).
8. Sertifika doğrulama esp-tls'te **ya REQUIRED ya hata**: `crt_bundle_attach` → `MBEDTLS_SSL_VERIFY_REQUIRED` + `esp_crt_bundle_attach(&conf)` (`mbed.c:945-949`); doğrulama seçeneği verilmezse `ESP_ERR_MBEDTLS_SSL_SETUP_FAILED` + "No server verification option set" (`mbed.c:987-988`). ALTE'nin `VERIFY_OPTIONAL` + okunmayan `verify_result` hatası (`smtp.c:322,601`) esp-tls'e geçince yapısal olarak kapanır.

### Öneri — akış ve iskelet

Akış: `esp_tls_init` → `esp_tls_plain_tcp_connect` (DNS+TCP, 10 s) → düz metin `220` / `EHLO` / `STARTTLS` → `220` → `set_conn_sockfd` + `set_conn_state(CONNECTING)` → `esp_tls_conn_new_sync` (aynı soket, el sıkışma bütçesi) → `EHLO` tekrar (RFC 3207 §4.2: önceki EHLO bilgisi atılır) → `AUTH` → … → `QUIT`/`221` → `esp_tls_conn_destroy`. Port 465: `esp_tls_conn_new_sync` doğrudan `INIT`'ten (standart yol).

```c
/* net_tls.c — STARTTLS: düz TCP → aynı soket üzerinde TLS (IDF 5.5.2) */
esp_err_t net_tls_open_starttls(net_tls_t *c, const char *host, uint16_t port,
                                const net_tls_policy_t *pol, int64_t deadline_us)
{
    esp_tls_cfg_t cfg = net_tls_cfg(pol, 10000);          /* bundle|cacert, non_block=false, timeout 10 s */
    c->tls = esp_tls_init();                               /* esp_tls.c:175 */
    esp_tls_error_handle_t eh; esp_tls_get_error_handle(c->tls, &eh);
    int fd = -1;
    esp_err_t err = esp_tls_plain_tcp_connect(host, strlen(host), port, &cfg, eh, &fd); /* :547 */
    if (err != ESP_OK) return err;                         /* DNS + connect(select 10 s) */
    c->fd = fd; c->plain = true;                           /* recv/send ile düz metin */
    if (smtp_expect(c, 220, deadline_us) || smtp_cmd(c, "EHLO [%s]", 250, deadline_us)
        || smtp_cmd(c, "STARTTLS", 220, deadline_us)) return ESP_FAIL;   /* RFC 3207 §4 */
    esp_tls_set_conn_sockfd(c->tls, fd);                   /* esp_tls.c:765 */
    esp_tls_set_conn_state(c->tls, ESP_TLS_CONNECTING);   /* esp_tls.c:785 → :492 dalı */
    cfg.timeout_ms = 15000;                                /* el sıkışma toplam bütçesi (:569-575) */
    if (esp_tls_conn_new_sync(host, strlen(host), port, &cfg, c->tls) != 1) {
        err = net_tls_capture_error(c);                    /* esp_tls_get_and_clear_last_error */
        esp_tls_conn_destroy(c->tls); c->tls = NULL;       /* soketi de kapatır (:159) */
        return err;
    }
    c->plain = false;                                      /* artık esp_tls_conn_read/write */
    return smtp_cmd(c, "EHLO [%s]", 250, deadline_us);     /* RFC 3207 §4.2: EHLO tekrar */
}
```

Düz metin aşamasında yanıtlar **satır satır, CRLF'e kadar** okunmalı; `220` STARTTLS yanıtından sonra sunucu ClientHello gelene dek hiçbir şey göndermez (RFC 3207 §4), dolayısıyla fazla okuma riski yalnız kendi tamponlamamızdan gelir: düz aşamada bayt bayt ya da `MSG_PEEK` ile okuyun, TLS aşamasında tamponlu okuma serbest (mbedTLS kaydı zaten tamponlar).

### Risk

- Yükseltme yolu IDF'in kendi kodunca sınanmıyor; **cihazda Gmail 587 ile ilk adımda doğrulanmalı** (`is_tls=false` yan etkileri, `esp_tls_conn_destroy` sonrası `lsof`-benzeri soket sayımı: `CONFIG_LWIP_MAX_SOCKETS` tüketimi).
- IDF küçük sürümlerinde `esp_tls_low_level_conn` iç yapısı değişebilir; `net_tls` içinde tek yerde kalsın, derleme hedefinde IDF sürümü sabit (`5.5.2`).
- Soket zaman aşımı düz bağlantıda ayarlanır (10 s); el sıkışma bütçesi 15 s `conn_new_sync` döngüsünde sayılır → en kötü ≈ 10 s okuma bloklaması + 15 s kontrolü = **~25 s** üst sınır. İstenirse yükseltmeden önce `setsockopt(SO_RCVTIMEO)` ile 15 s'ye eşitlenir.

---

## 2. esp-tls zaman aşımları, WANT_READ, hata okuma, "insecure"

### Bulgu

1. **`esp_tls_cfg_t.timeout_ms` üç yerde kullanılır** (`esp_tls.h:195-198` yalnız "network timeout" der):
   - Soket `SO_RCVTIMEO`/`SO_SNDTIMEO` (`esp_tls.c:279-292`); 0 ise 10 s (`:113, :282`).
   - `connect()` için `select` süresi (`:409-415`); `non_block=true` ise `EINPROGRESS`'te hemen döner (`:403-407`).
   - `esp_tls_conn_new_sync` toplam süre denetimi: `low_level_conn` 0 döndükçe geçen süre `timeout_ms` ile karşılaştırılır (`:560-577`, `ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT`).
   - **DNS kapsanmaz:** `getaddrinfo` bloklar (`:221`). lwIP DNS: `DNS_MAX_RETRIES=4` (`lwip/opt.h:1171`), `DNS_TMR_INTERVAL` 1 s, yeniden deneme aralığı `tmr = retries` (`dns.c:1183-1211`) → sunucu başına ≈ 1+1+2+3 = **7 s**, 3 DNS sunucusunda ≈ **21 s** (koddan çıkarım). Toplam "bağlantı" bütçesi = DNS (≤ ~21 s) + TCP (10 s).
2. **El sıkışma döngüsü nasıl biter:** blocking soket + `SO_RCVTIMEO` dolunca lwIP `ERR_TIMEOUT → EWOULDBLOCK` (`lwip/api/err.c:52`); `mbedtls_net_recv` bunu `MBEDTLS_ERR_SSL_WANT_READ`'e çevirir (`net_sockets.c`, `net_would_block`); `esp_mbedtls_handshake` WANT_READ/WRITE'ta `0` döner (`mbed.c:323-326`); `conn_new_sync` geçen süreye bakar (`esp_tls.c:569-575`). ALTE'nin sonsuz `while(mbedtls_ssl_handshake)` döngüsü (`smtp.c:389-396`) bu yolla **doğal olarak sınırlanır**.
3. **`esp_tls_conn_read/write` WANT_* döndürür, kendi başına döngü kurmaz:** `esp_mbedtls_read` WANT_READ/WRITE'ı olduğu gibi döndürür (`mbed.c:404-418`), `0` = karşı tarafın `close_notify`'ı (`:405-407`). `esp_mbedtls_write` tüm veri yazılana kadar döner, WANT_*'ta "yazılan kadar ya da hata" döner (`mbed.c:422-452`); 4096'dan (`MBEDTLS_SSL_OUT_CONTENT_LEN`) büyük yazımları kendisi parçalar (`:427-429`). Dolayısıyla **`timeout_ms` dolduğunda okuma `WANT_READ` (-0x6900) olarak görünür**; sonsuz döngüden kaçınmanın tek yolu çağıranın son tarihi (deadline) tutmasıdır. `tcp_transport` da aynı şeyi yapar: `select` sonrası `WANT_READ`/`SSL_TIMEOUT` → `ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT` (`tssl.c:260-294`).
4. **`non_block + esp_tls_conn_new_async`** (`esp_tls.c:584-590`): her çağrı bir adım ilerletir (`CONNECTING`'de `select(timeout_ms)` `:493-517`, el sıkışma WANT_*'ta `0`). Toplam süre denetimi tamamen çağıranda. `tcp_transport` bunu `esp_tls_connect_async` ile kullanır (`tssl.c:61-91`). Seri teslimat görevinde senkron yol + deadline yeterli; async yalnız "iptal edilebilir bekleme" gerekirse.
5. **Hata okuma:** `esp_tls_get_error_handle` + `esp_tls_get_and_clear_last_error(h, &code, &flags)` (`esp_tls.c:795-819`): dönüş = son `esp_err_t` (ör. `ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED`, `esp_tls_errors.h:47`), `code` = mbedTLS `-ret`, `flags` = X.509 doğrulama bayrakları. Bayraklar yalnız el sıkışma **başarısız** olup CA yapılandırılmışsa ve eş sertifika alınmışsa yakalanır: `esp_mbedtls_verify_certificate` → `mbedtls_ssl_get_verify_result` → `ESP_TLS_ERR_TYPE_MBEDTLS_CERT_FLAGS` (`mbed.c:312-318, 468-476`). `VERIFY_REQUIRED` olduğundan doğrulama hatası = el sıkışma hatası (`MBEDTLS_ERR_X509_CERT_VERIFY_FAILED -0x2700`); `mbedtls_x509_crt_verify_info(flags)` ile kullanıcıya okunur neden üretilebilir ("sertifika süresi dolmuş", "CN uyuşmuyor"). Başarılı el sıkışmadan sonra `esp_tls_get_ssl_context` + `mbedtls_ssl_get_verify_result` her zaman 0'dır (aksi halde el sıkışma düşerdi).
6. **"insecure":** `set_client_config` sıralaması `mbed.c:945-989`: bundle → global store → `cacert_buf` → PSK → **hiçbiri yoksa** yalnız `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` ile `VERIFY_NONE` (`:984-985`); bu seçenek `CONFIG_ESP_TLS_INSECURE`'a bağlıdır (`esp-tls/Kconfig`). ALTE'de ikisi de kapalı (`esp_bin/HW/sdkconfig:808`). `skip_common_name` **doğrulamayı kapatmaz**, yalnız SNI/CN karşılaştırmasını atlar (`mbed.c:879-901`); zincir yine CA'ya karşı doğrulanır. Sonuç: planın "SMTP için kullanıcı isterse doğrulama kapalı" seçeneği **`CONFIG_ESP_TLS_INSECURE=y` + `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`** gerektirir ve çalışma anında `crt_bundle_attach=NULL, cacert_buf=NULL` verilerek seçilir.
   - Güvenlik etkisi: derleme ayarı yalnız "doğrulama seçeneği verilmeyen" bağlantının varsayılanını değiştirir; Telegram/webhook/OTA `crt_bundle_attach` verdiği sürece REQUIRED kalır. Asıl risk, bir çağrıda `crt_bundle_attach`'ın unutulmasının **sessizce** doğrulamasız bağlantıya dönmesidir (bugün bu hata ile derleme/çalışma anında yakalanır: `mqtts://` bulgusu tam da bu mekanizmayla düştü).
   - Alternatif, sdkconfig değişmeden: `cacert_buf` ile kullanıcının **sunucu/CA sertifikasını PEM olarak yapıştırması** (`mbed.c:959-963`). mbedTLS 3.x güven deposundaki kendinden imzalı bir sunucu sertifikasını "yerel olarak güvenilir uç sertifika" olarak kabul eder (`x509_crt_verify_chain` → `x509_crt_check_ee_locally_trusted`), yani kullanıcı Proton Bridge/kendi sunucusunun sertifikasını yapıştırınca doğrulama **açık kalır**. IP ile adreslenen sunucuda ek olarak `skip_common_name=true` gerekir (CN/SAN IP değilse).

### Öneri

- `net_tls_cfg(policy, timeout_ms)` tek üretici: `policy ∈ {BUNDLE, PINNED_PEM, (opsiyonel) NONE}`; `NONE` yalnız `#if CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY` altında derlenir, aksi halde `ESP_ERR_NOT_SUPPORTED`. Birim testi: `BUNDLE` → `crt_bundle_attach != NULL`.
- **Ürün kararı önerisi:** SMTP "sertifika doğrulama" seçeneği üç değerli: *Paket (varsayılan)* / *Özel sertifika (PEM yapıştır)* / *Kapalı*. "Kapalı" ilk sürümde **çıkarılabilir**: Proton Bridge ve özel sunucu senaryosu PEM ile çözülür, sdkconfig güvenli kalır. Kalacaksa GUI'de kırmızı uyarı + evlog'a "TLS doğrulaması kapalı" satırı.
- Zaman aşımı modeli: `timeout_ms = 10 000` (soket + connect), el sıkışma `conn_new_sync` 15 000; her okuma/yazma çağrısı `deadline_us` alır; WANT_READ/WRITE = "10 s'de veri yok" → deadline kontrolü → tekrar. İş başına deadline: `t0 + 10 s (DNS/TCP) + 15 s (TLS) + 120 s + 60 s × ek` (plan §4.6) ve DNS için ayrı tolerans (≤ 21 s; yoksa DNS hatası "TCP zaman aşımı" sanılır).
- Hata haritalama `channel_send` `err[96]` için: `ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME` → "DNS çözümlenemedi", `..._CONNECTION_TIMEOUT` → "bağlantı zaman aşımı", `ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED` + `flags != 0` → "sertifika doğrulanamadı: <verify_info>", `flags == 0` → "TLS el sıkışma hatası -0x%04x".

```c
/* net_tls_read_line: WANT_READ = "timeout_ms içinde veri yok" → deadline'a kadar tekrar */
static int net_tls_read_line(net_tls_t *c, char *buf, size_t n, int64_t deadline_us)
{
    size_t len = 0;
    while (len + 1 < n) {
        if (esp_timer_get_time() > deadline_us) return NET_ERR_TIMEOUT;
        char ch; ssize_t r = c->plain ? recv(c->fd, &ch, 1, 0) : esp_tls_conn_read(c->tls, &ch, 1);
        if (r == 1) { buf[len++] = ch; if (ch == '\n') break; continue; }
        if (r == 0) return NET_ERR_CLOSED;                            /* close_notify / FIN */
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE ||
            (c->plain && (errno == EWOULDBLOCK || errno == EAGAIN))) continue;   /* soket 10 s doldu */
        return NET_ERR_IO;                                             /* gerçek hata; last_error oku */
    }
    buf[len] = 0;
    return (int)len;
}
```

### Risk

- `is_async`/`non_block` yolları IDF'te daha az kullanılır; senkron + deadline tercih edilmeli.
- `timeout_ms` 10 s ile komut başına bloklama 10 s'yi aşamaz, ama çok adımlı SMTP diyaloğunda toplam süre adım sayısıyla çarpılır; deadline olmadan "120 s" garantisi yoktur.

---

## 3. SMTP doğruluğu

### Bulgu — RFC 5321 akışı ve kodlar

| Adım | Beklenen | Geçici (yeniden dene) | Kalıcı | Kaynak |
|---|---|---|---|---|
| Bağlantı selamı | `220` | `421` (servis kapalı) | `554` (reddedildi) | RFC 5321 §3.1, §4.2.1 |
| `EHLO` | `250` (çok satırlı) | `421` | `500/501/502/504` → `HELO` dene | §4.1.1.1 |
| `STARTTLS` | `220` | `454` (TLS şimdi yok) | `501`, `502` (STARTTLS yok) | RFC 3207 §4 |
| `AUTH PLAIN/LOGIN` | `334` ara, `235` başarı | `454` (geçici kimlik hatası) | `535` (kimlik), `534` (mekanizma/uyg. parolası gerek), `530` (önce STARTTLS) | RFC 4954 §4, §6 |
| `MAIL FROM` | `250` | `451/452/421` | `550/553/552/503/555` | §4.1.1.2 |
| `RCPT TO` | `250` ya da `251` | `450/451/452` | `550/551/553/552` | §4.1.1.3 |
| `DATA` | `354` | `451/421` | `503` (RCPT yok), `554` | §4.1.1.4 |
| `<CRLF>.<CRLF>` | `250` | `451/452/421` | `552/554` | §4.1.1.4, §4.2.3 |
| `QUIT` | `221` | — | — | §4.1.1.10 |

- Sınıf kuralı (§4.2.1): yalnız ilk haneye göre karar ver (2xx tamam, 3xx ara, 4xx geçici, 5xx kalıcı); bilinmeyen kodlara da ilk haneyle davran.
- Çok satırlı yanıt (§4.2.1): `250-metin` devam, `250 metin` son satır; satır `^[2-5][0-9]{2}[ -]`. ALTE tek `recv` ile okur (`smtp.c:118,177`) → Gmail'in 10 satırlık EHLO yanıtı iki TCP parçasında gelirse sonraki komut yanıtı kayar (harita §2.8/6). Yanıt satırı ≤ 512 bayt (§4.5.3.1.5).
- Sunucu tarafı asgari zaman aşımları (§4.5.3.2): `DATA` sonu için **10 dk** — büyük ekli iletide son `250` anti-spam taramasından dolayı onlarca saniye gecikebilir; son nokta için okuma bütçesi ≥ 60 s olmalı (plan 120 s uygun).
- Noktalama (§4.5.2): gövdede `.` ile başlayan her satıra bir `.` eklenir. ALTE yapmıyor (`smtp.c:497-505`): kullanıcı gövdesinde yalnız `.` olan satır iletiyi **erken bitirir**, kalan satırlar komut sanılır (`500`), "başarılı" loglanır. Base64 alfabesinde `.` yok → ek gövdeleri güvenli.
- Satır uzunluğu: metin satırı ≤ 1000 bayt CRLF dahil (RFC 5321 §4.5.3.1.6); RFC 5322 §2.1.1 998 zorunlu / 78 önerilir. Base64: RFC 2045 §6.8 **≤ 76 karakter**. ALTE 570 B ham → 760 karakterlik satır (`smtp.c:710-726`): SMTP sınırının altında, MIME kuralının 10 katı üstünde; çoğu alıcı çözer, bazı geçitler kırar.
- Çıplak LF (§2.3.8) yasak; ALTE gövde JSON'dan `\n` ile gelir ve olduğu gibi gider. Gövde ve başlıklar CRLF'e normalize edilmeli (Exchange Online'ın çıplak LF'i reddettiği bildiriliyor; doğrulanmadı).
- `8BITMIME` (RFC 6152): EHLO'da ilan edilirse `MAIL FROM:<…> BODY=8BITMIME` ile UTF-8 gövde `Content-Transfer-Encoding: 8bit` olarak gidebilir. **Canlı EHLO (2026-11-05):** Gmail (465/587), Outlook, iCloud hepsi `8BITMIME`, `SMTPUTF8`, `PIPELINING`, `SIZE` ilan ediyor. ALTE pazarlık yapmadan 8-bit gönderiyor (harita §2.8/8).
- `SIZE` (RFC 1870): Gmail `35882577`, Outlook `157286400`, iCloud `28319744` (canlı). ALTE'nin 5 × 2 MB eki ≈ 13,7 MB base64 → sığar; ama sınır EHLO'dan okunup tahmini boyutla **önceden** karşılaştırılırsa 13 MB gönderip sonda `552` almak önlenir.
- Başlıklar (RFC 5322 §3.6): zorunlu yalnız `From` ve `Date`; `Message-ID` SHOULD. Eksik `Date`/`Message-ID` spam puanlamasında olumsuz (SpamAssassin `MISSING_DATE`, `MISSING_MID`); MSA'lar (RFC 6409 §8) ekleyebilir ama garanti değil. ALTE: `Date`, `Message-ID`, `MIME-Version` (düz mailde) yok (harita §2.4).
- Konu UTF-8: RFC 2047 encoded-word `=?UTF-8?B?…?=`, her encoded-word ≤ 75 karakter (§2); ham 45 baytlık parçalar (UTF-8 sınırında kesilerek) 72 karakter verir; parçalar `CRLF SP` ile katlanır. ALTE ham 8-bit konu gönderir.
- `EHLO` argümanı FQDN ya da adres literali `[a.b.c.d]` (§4.1.1.1). ALTE `EHLO lebensspur` (`smtp.c:371`) — Gmail kabul eder, katı sunucular `501` verebilir.
- Dosya adı: `Content-Disposition: attachment; filename="…"`; ASCII dışı için RFC 2231 gerekir; plan zaten dosya adını beyaz listeyle ASCII'ye sınırlıyor (§5.7) → kodlama gereksiz. `"` ve CR/LF'i reddet.
- MIME sınırı: base64 alfabesinde `-` ve `_` yok, `=` yalnız satır sonunda; `=_LS_<rastgele hex>` biçimli sınır ek gövdeleriyle çakışamaz.

### Bulgu — AUTH ve sağlayıcılar (2026-11-05 canlı EHLO + web)

| Sağlayıcı | Sunucu/port | AUTH (EHLO) | Parola durumu | Zincir kökü (CMN paketinde?) |
|---|---|---|---|---|
| Gmail | `smtp.gmail.com` 465 TLS / 587 STARTTLS | `LOGIN PLAIN XOAUTH2 …` | Uygulama parolası **çalışır**; 2 Adımlı Doğrulama şart; Google "önerilmez, çoğu durumda gereksiz" der; yalnız güvenlik anahtarlı 2SV, kurum hesabı, Gelişmiş Koruma'da yok ([Google](https://support.google.com/accounts/answer/185833)). "Less secure apps" bitti (tüketici 2022, Workspace Mayıs 2025, [Workspace blog](https://workspaceupdates.googleblog.com/2023/09/winding-down-google-sync-and-less-secure-apps-support.html)). | leaf ← GTS WE2 (ECDSA P-256) ← **GTS Root R4 (ECC P-384)** ← GlobalSign Root CA; R4 ve GlobalSign Root CA `cmn_crt_authorities.csv`'de ✔ |
| Outlook.com (kişisel) | `smtp-mail.outlook.com` 587 | `LOGIN XOAUTH2` (**PLAIN yok**) | Temel kimlik doğrulama kişisel hesaplarda **kapatıldı** (16 Eylül 2024 bildiriliyor; Microsoft sayfası "yalnız OAuth'a geçiyoruz" der, tarih vermez — [MS](https://support.microsoft.com/en-us/office/f4202ebf-89c6-4a8a-bec3-3d60cf7deaef)); `535 5.7.139 basic authentication is disabled` raporları. **ESP için fiilen kullanılamaz** (XOAUTH2 = Azure uygulama kaydı + tarayıcı akışı). | DigiCert Global G2 ← DigiCert Global Root G2 ✔ |
| Microsoft 365 (iş) | `smtp.office365.com` 587 | aynı | SMTP AUTH temel: **Aralık 2026 sonuna kadar** açık, sonra engel (yönetici geri açabilir), kesin kaldırma 2027'de duyurulacak; Ocak 2027'den sonra açılan kiracılarda yok ([office365itpros 2026-01-29](https://office365itpros.com/2026/01/29/smtp-auth-basic-retirement/)). | aynı ✔ |
| iCloud | `smtp.mail.me.com` 587 STARTTLS | `LOGIN PLAIN …` | Uygulamaya özel parola ([Apple 102525](https://support.apple.com/en-me/102525)) | Apple Public Server RSA CA 11 ← USERTrust RSA ✔ |
| Proton Bridge | `127.0.0.1:1025` STARTTLS | LOGIN/PLAIN | Bridge parolası; **yalnız localhost dinler**, LAN'dan erişim için ana makinede `socat` benzeri yönlendirme gerekir; sertifika kendinden imzalı ([Discourse](https://meta.discourse.org/t/smtp-config-to-use-localhost-or-172-17-0-1-is-not-working-how-to-debug/229056)). | yok → PEM yapıştır |
| Telegram API (karşılaştırma) | `api.telegram.org:443` | — | — | Go Daddy Secure CA G2 ← **Go Daddy Root CA G2** ✔ |

Hepsi TLS 1.2'yi de kabul ediyor (canlı `-tls1_2`: Telegram `ECDHE-RSA-AES128-GCM-SHA256`, Gmail `ECDHE-ECDSA-CHACHA20-POLY1305`, iCloud/Outlook `ECDHE-RSA-AES256-GCM-SHA384`).

- `AUTH PLAIN` tek tur: `AUTH PLAIN base64("\0user\0pass")` (RFC 4616, RFC 4954 §4 ilk yanıt). `AUTH LOGIN`: `334 VXNlcm5hbWU6` → base64(user) → `334 UGFzc3dvcmQ6` → base64(pass) → `235`. Outlook yalnız LOGIN ilan ettiği için **ikisi de şart**; EHLO ayrıştırılıp PLAIN varsa PLAIN, yoksa LOGIN.
- ALTE parolanın base64'ünü `ESP_LOGI` ile basıyor (`smtp.c:191,462`): yeni kodda komut logu AUTH satırlarını maskeler.

### Öneri — ek parçası ve ileti düzeni

- **Parça boyutu: 2964 B ham → 52 satır × (76 + CRLF) = 4056 B base64** (57 × 52 = 2964). Tek TLS kaydına sığar (`MBEDTLS_SSL_OUT_CONTENT_LEN=4096`); 3072 B ham 57'nin katı değildir (ara dolgu `=` ek gövdesini bozar). Bellek darsa 1482 B → 2028 B (26 satır). Tamponlar (≈ 7 KB) yığında değil heap'te ya da statik.
- İleti başlıkları: `Date` (saat SNTP ile geçerliyse; `time(NULL) > 2024-01-01` değilse atla ve evlog'a not — yanlış tarih eksik tarihten kötü), `From: LebensSpur <user>`, `To`, `Subject` (RFC 2047), `Message-ID: <epoch.sayac@ls-xxxx.local>`, `MIME-Version: 1.0`, `Content-Type: multipart/mixed` ya da `text/plain; charset=UTF-8`, gövde `8bit` (+`BODY=8BITMIME`) ya da 8BITMIME ilan edilmemişse `base64`.
- Gövde normalizasyonu: `\n` → `\r\n`, dot-stuffing, satır 998'de kır, CR/LF enjeksiyonunu konu/alıcıda reddet (ALTE'de web katmanında var, harita §2.4).
- Grup başına **tek oturum, çoklu `RCPT TO`**: 10 MB eki alıcı başına yeniden göndermek (ALTE `main.c:302-328`) süre ve enerji israfı; `RCPT` yanıtı alıcı başına izlenir (`250/251` kabul, `5xx` o alıcı kalıcı, `4xx` o alıcı yeniden), en az bir kabul varsa `DATA`. Gizlilik için `To: undisclosed-recipients:;` ya da grup üyeleri `To`'da — ürün kararı.
- Sonuç sınıfları `channel_send` için: `OK` / `RETRY` (DNS, TCP, TLS zaman aşımı, 4xx, 421) / `PERM` (5xx MAIL/RCPT/DATA) / `CONFIG` (`535/534/530`, sertifika doğrulama hatası) — `CONFIG` sınıfında deneme harcanmaz, GUI'ye "SMTP ayarı hatalı: …" düşer.
- GUI yardım metni: "Outlook.com kişisel hesaplar parola ile çalışmaz (Microsoft 2024); Gmail/iCloud uygulama parolası; Microsoft 365 iş hesapları 2026 sonuna kadar". Proton Bridge: "ana makinede yönlendirme + sertifika yapıştır".

```c
/* RFC 5321 §4.2.1: "250-…" devam, "250 …" son; dönüş = üç haneli kod ya da NET_ERR_* */
static int smtp_read_reply(net_tls_t *c, char *line, size_t n, int64_t deadline_us)
{
    int code;
    for (;;) {
        int r = net_tls_read_line(c, line, n, deadline_us);
        if (r < 0) return r;
        if (r < 4 || !isdigit((unsigned char)line[0]) || !isdigit((unsigned char)line[1])
            || !isdigit((unsigned char)line[2])) return NET_ERR_PROTO;
        code = (line[0]-'0')*100 + (line[1]-'0')*10 + (line[2]-'0');
        if (line[3] == ' ' || line[3] == '\r') return code;          /* son satır */
        if (line[3] != '-') return NET_ERR_PROTO;                     /* devam satırı değil */
        if (code == 250 && strncasecmp(line + 4, "AUTH ", 5) == 0) smtp_note_auth(c, line + 9);
        if (code == 250 && strncasecmp(line + 4, "SIZE ", 5) == 0) c->size_limit = strtoul(line + 9, NULL, 10);
        if (code == 250 && strncasecmp(line + 4, "8BITMIME", 8) == 0) c->has_8bitmime = true;
    }
}
```

### Risk

- Gmail uygulama parolası "önerilmez" statüsünde; Google kaldırırsa tek çıkış XOAUTH2 (cihazda tarayıcı akışı yok). `yapilacaklar.md`'ye "XOAUTH2/Device Code araştırması" eklenmeli.
- Outlook.com kişisel hesaplar şimdiden dışarıda; README/GUI buna göre güncellenmeli.
- Çoklu RCPT tek oturum kararı alıcı gizliliği gerektirir; ürün kararı bekler.

---

## 4. Telegram Bot API

### Bulgu

- `sendMessage`: `chat_id` "Integer or String, Yes"; `text` "String, Yes — Text of the message to be sent, **1-4096 characters after entities parsing**"; `parse_mode` "String, **Optional** — Mode for parsing entities in the message text" (core.telegram.org/bots/api, 2026-11-05 indirilen sayfa). `parse_mode` verilmezse metin **düz** işlenir: kullanıcı metnindeki `_ * [ ] ( )` gibi karakterler sorun çıkarmaz. MarkdownV2 verilirse `_ * [ ] ( ) ~ \` > # + - = | { } . !` ters bölüyle kaçırılmalı; eski `Markdown` "legacy mode, retained for backward compatibility" ve dengesiz `_`/`*` → `400 Bad Request: can't parse entities`.
- Yanıt biçimi: JSON `{"ok": bool, "result"|"description", "error_code", "parameters"?}`; `parameters.retry_after` "In case of exceeding flood control, the number of seconds left to wait before the request can be repeated". HTTP durum kodu `error_code` ile aynıdır (uygulamada: `400` geçersiz istek/chat yok/metin uzun, `401` token geçersiz, `403` bot engellenmiş ya da sohbet başlatılmamış, `404` token biçimi bozuk, `429` flood). Sınırlar ([FAQ](https://core.telegram.org/bots/faq)): aynı sohbete ~1 ileti/s, toplu ~30/s, grupta 20/dk; aşımda `429`.
- İstek: `POST https://api.telegram.org/bot<token>/sendMessage`, `Content-Type: application/json`, gövde `{"chat_id":"…","text":"…"}` (ALTE `telegram_client.c:94-124` doğru; metin 512 ile sınırlı, 4096 altında). Token URL'de gider → URL logda maskelenmeli.
- Sertifika zinciri (canlı): `api.telegram.org` ← Go Daddy Secure CA G2 ← **Go Daddy Root CA G2**, RSA 2048, `cmn_crt_authorities.csv` satırı "GoDaddy,Go Daddy Root Certificate Authority - G2" → **CMN paketinde var**. TLS 1.3 varsayılan, TLS 1.2 kabul.
- esp_http_client zaman aşımı işlem başınadır (bkz. §5); Telegram yanıtı küçük (≈ 300 B), tipik süre TLS el sıkışması + 1 RTT.

### Öneri

- `parse_mode` **gönderme**; metni olduğu gibi JSON-kaçışla (`\" \\ \n` ve `< 0x20`), UTF-8 koru; 4096'yı aşan metni 4000'de kes ve "…" ekle.
- Yanıt gövdesini (≤ 512 B) oku; `ok=false` ise `description`'ı `err[96]`'ya yaz ("chat not found", "bot was blocked by the user" GUI için anlamlı). `429` → `retry_after` saniyesini outbox'ın sonraki deneme zamanına ekle (EARLY_WARN'un 1 dk aralığı genelde yeter). `401/404` → `CONFIG`, `403/400 chat not found` → `CONFIG`, `5xx`/ağ → `RETRY`.
- `disable_notification` ve `link_preview_options.is_disabled` isteğe bağlı; gerekmiyor.

### Risk

- Telegram kök CA'yı değiştirirse (GoDaddy → Let's Encrypt/DigiCert) CMN paketi yine kapsar (ISRG X1/X2, DigiCert var); daha egzotik bir kök seçilirse OTA ile paket güncellemesi gerekir. `yapilacaklar`: yılda bir canlı zincir denetimi.

---

## 5. esp_http_client (webhook + Telegram)

### Bulgu

- **`timeout_ms` işlem başınadır, toplam değildir:** `esp_transport_connect(…, timeout_ms)` (`http.c:1606`) → SSL için `esp_tls_cfg_t.timeout_ms` (`tssl.c:108`) → soket + connect + el sıkışma (§2); ardından **her** `esp_transport_read(…, timeout_ms)` (`http.c:1276,1339,1546`) ve `esp_transport_write(…, timeout_ms)` (`:1729,1808`) ayrı ayrı `select` bekler. Varsayılan 5 s (`:169,571-572`). En kötü toplam ≈ DNS (≤ 21 s) + connect (T) + el sıkışma (≤ 2T) + yazma (T) + başlık okuma (T) + gövde okumaları (T × n) + yönlendirme başına tekrar.
- **Yönlendirme:** varsayılan `max_redirection_count=10` (`:167,523`); `esp_http_check_response` 301/302/303/307/308'de URL'yi değiştirip `process_again` ile döner (`:1085-1120`); `disable_auto_redirect=true` ile yalnız `HTTP_EVENT_REDIRECT` yayınlanır. ALTE 3 (`api_client.c:162`).
- **Başka görevden `esp_http_client_close`:** istemcide kilit yok; `close` → `esp_transport_close` → `esp_tls_conn_destroy` (`http.c:1820-1829`, `tssl.c:335`) çalışırken diğer görev `esp_transport_read` içinde aynı `tls`'i kullanır → **use-after-free riski, güvenli değil**. Kesinti istenirse yalnız HTTPS'te `is_async=true` + `esp_http_client_perform` `ESP_ERR_HTTP_EAGAIN` döngüsü (`:1392-1405, 1611-1621`; düz HTTP'de async yok `:1614-1617`) ve döngüde deadline/iptal bayrağı; sonra `cleanup`.
- **`keep_alive_enable`** TCP keepalive'dır (varsayılan kapalı, `:729-734`); HTTP kalıcı bağlantı: yanıt `keep-alive` ise soket `cleanup`'a kadar açık kalır (`:1516-1518, 2032`), `esp_http_client_cleanup` → `close` (`:1020-1025`). Tek atış kullanımda `cleanup` hemen çağrılır; `Connection: close` başlığı sunucu tarafı için temiz.
- **TLS:** `.crt_bundle_attach = esp_crt_bundle_attach` (`:827-829`); `http://` şemasında yok sayılır (düz). `skip_cert_common_name_check` ve `common_name` alanları esp-tls'e geçer (`esp_http_client.h:207-208`).
- **TX tamponu:** istek satırı + başlıklar `buffer_size_tx`'e (varsayılan 512, `DEFAULT_HTTP_BUF_SIZE`) sığmalı (`:1656-1676`); webhook URL (255) + özel başlıklar (255) + `Host/Content-Type/Content-Length` 512'yi aşabilir → `buffer_size_tx = 1024`.

### Öneri

- Senkron yol + `timeout_ms = 10 000` + hesaplanmış üst sınır: Telegram/webhook için toplam ≈ ≤ 60 s (DNS 21 + connect 10 + TLS 20 + yazma 10 + okuma 10 + küçük gövde). Outbox politikasında (plan §4.6) bu değerleri kullan; async yol yalnız "iptal" ürün gereksinimi olursa.
- Webhook: `max_redirection_count = 2`, `buffer_size_tx = 1024`, `Connection: close`; 2xx = `OK`, 3xx aşımı/4xx = `PERM` (`408/429` → `RETRY`), 5xx/ağ = `RETRY`. `http://` izinli kalsın ama GUI'de "sırlar düz metin gider" uyarısı (harita §4.3).
- Telegram: `buffer_size` 512 (yanıt küçük), yanıt gövdesi olay işleyicide toplanır.
- 10 s işlem başına **yeterli**; Telegram yanıtı tipik < 1 s, webhook hedefi ev otomasyonu (LAN) ya da bulut.

```c
/* HTTPS isteği toplam süre denetimiyle (yalnız https; http.c:1611-1621). Senkron yol: aynı cfg, is_async=false, döngü yok. */
esp_http_client_config_t hc = { .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 10000,
    .crt_bundle_attach = esp_crt_bundle_attach, .is_async = true, .max_redirection_count = 2,
    .buffer_size_tx = 1024, .event_handler = rx_collect, .user_data = &rx };   /* rx: ≤512 B gövde */
esp_http_client_handle_t h = esp_http_client_init(&hc);
esp_http_client_set_header(h, "Content-Type", "application/json");
esp_http_client_set_header(h, "Connection", "close");
esp_http_client_set_post_field(h, body, strlen(body));
esp_err_t err;
do {
    err = esp_http_client_perform(h);
    if (err == ESP_ERR_HTTP_EAGAIN) {
        if (esp_timer_get_time() > deadline_us || job_cancelled(job)) { err = ESP_ERR_TIMEOUT; break; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
} while (err == ESP_ERR_HTTP_EAGAIN);
int status = esp_http_client_get_status_code(h);
esp_http_client_cleanup(h);                                   /* close + free (http.c:1020-1025) */
```

### Risk

- Async mod IDF'te daha az sınanmış (yalnız HTTPS, yönlendirme + chunked birleşimleri); ilk sürümde senkron.
- `timeout_ms` küçültülerek "toplam" taklit edilemez: 10 s altı değerler yavaş TLS el sıkışmasını (bkz. §7) keser.

---

## 6. esp-mqtt tek yayın kalıbı ve "görev yaratılmaz" kuralı

### Bulgu

- **Yaşam döngüsü ve maliyet:** `esp_mqtt_client_init` → yapılandırma kopyası, giriş/çıkış tamponu `CONFIG_MQTT_BUFFER_SIZE` (1024 + 1024, `mqtt.c:413-427`), outbox, olay döngüsü. `esp_mqtt_client_start` → **`xTaskCreate("mqtt_task", task_stack, prio)`** (`:1849-1855`); yığın `config->task.stack_size` yoksa `CONFIG_MQTT_TASK_STACK_SIZE = 6144`, öncelik `CONFIG_MQTT_TASK_PRIORITY = 5` (`:436-441`, `Kconfig`). TLS el sıkışması bu görevde yapılır (6 KB yığın Espressif varsayılanı; mbedTLS bağlamları heap'te).
- **Adım zaman aşımları:** `network.timeout_ms` (0 → `MQTT_NETWORK_TIMEOUT_MS` 10 000, `:525-527`, `mqtt_config.h:52`) hem `esp_transport_connect` (`:1693-1696`) hem CONNACK bekleme (`esp_mqtt_connect` `:780-783`) hem yazma içindir. `MQTT_EVENT_CONNECTED` `:1708`; `MQTT_EVENT_PUBLISHED` PUBACK'te (`:1463,1514`); hata → `esp_mqtt_abort_connection` → `MQTT_STATE_WAIT_RECONNECT` + `MQTT_EVENT_DISCONNECTED` (`:836-848`). `disable_auto_reconnect=true` ile görev `WAIT_RECONNECT`'te `wait_timeout_ms/2` (= 5 s) adımlarla bekler (`:1793-1812`).
- **`stop`/`destroy` süresi:** `esp_mqtt_client_stop` bağlıysa DISCONNECT gönderir (çağıran görevden), `run=false`, `STOPPED_BIT`'i **`portMAX_DELAY`** ile bekler (`:1916-1946`). Görev döngüsü `CONNECTED`'da `esp_transport_poll_read(MQTT_POLL_READ_TIMEOUT_MS=1000)` (`:1819`) → stop ≤ ~1 s; `WAIT_RECONNECT`'te ≤ ~5 s; `INIT`'te bloklayan `esp_transport_connect`/CONNACK beklemesi bitene dek (≤ ~20 s). `destroy` stop'tan sonra serbest bırakma (`:912-935`), mikrosaniyeler. ALTE bunu MQTT olay bitleriyle doğru kuruyor (`api_client.c:235-269`), ama `destroy` toplam süresi hesaplanmamış (≤ 8 + 5 + 20 s).
- **`publish` vs `enqueue`:** `esp_mqtt_client_publish` outbox'a koyar ve bağlıysa **çağıran görevden** hemen yazar (`:2148-2250`); `esp_mqtt_client_enqueue(…, store=true)` yalnız outbox'a koyar, mqtt görevi `CONNECTED` döngüsünde `QUEUED` öğeleri gönderir (`:1738-1746`, `mqtt_resend_queued :1546`). Dolayısıyla "start'tan önce enqueue → start → PUBLISHED bekle" tek olay bekleyişiyle çalışır. Gönderilmemiş QoS1 iletiler `CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS` (30 s) sonra silinir.
- **`mqtts://`:** `broker.verification.crt_bundle_attach = esp_crt_bundle_attach` (`:580` → `:144-147`), ya da `broker.verification.certificate` (PEM), `skip_cert_common_name_check` — esp-tls ile aynı politika. ALTE'de eksik (harita §4.8/1), bu yüzden `mqtts://` esp-tls'in "No server verification option set" hatasıyla düşer.
- **Kural çatışması:** esp-mqtt her `start`'ta görev yaratır, `stop`'ta siler → planın "çalışma anında görev yaratılmaz" kuralını (§2) ihlal eder. Kalıcı başlatılmış istemci (reconnect açık) kuralı korur ama sürekli TLS oturumu + 6 KB yığın + bağlantı tutar; "bağlan-gönder-kapat" kararıyla çelişir.
- **Boyut (ALTE `esp_bin/HW/build/lebensspur.map`, `esp_idf_size --archives`):** `libmqtt.a` **14 044 B** flash; karşılaştırma: `libesp_http_client.a` 10 287, `libesp-tls.a` 5 916, `libtcp_transport.a` 4 152 (HTTP istemcisi için kalır), `libmbedtls.a` 38 560, `libmbedcrypto.a` 87 472.

### Öneri

**Hafif MQTT 3.1.1 yayıncısı `net_tls` üzerinde** (CONNECT → CONNACK → PUBLISH QoS1 → PUBACK → DISCONNECT; OASIS MQTT 3.1.1 §3.1, §3.2, §3.3, §3.4, §3.14), teslimat görevinde senkron, görev yok, `mqtt://` için `esp_tls_plain_tcp_connect`, `mqtts://` için `conn_new_sync` + paket. Kazanç: −14 KB flash, çalışma anında −6 KB yığın −~3 KB heap, kural ihlali yok, tek zaman aşımı modeli. ~120–150 satır. `retain` yapılandırılabilir (Home Assistant için genelde istenir, harita §4.10), `client_id = LS-<mac>`, keepalive 30 s, clean session. esp-mqtt IDF'te hazır durur; hafif istemci sorun çıkarırsa geri dönüş maliyeti düşüktür (aynı `channel_send` arayüzü).

```c
/* MQTT 3.1.1 tek yayın: fixed header = tip | kalan uzunluk (varint); strings = 2 bayt uzunluk + UTF-8 */
static size_t put_str(uint8_t *p, const char *s) { size_t n = strlen(s); p[0] = n >> 8; p[1] = n; memcpy(p + 2, s, n); return n + 2; }
esp_err_t mqtt_publish_once(net_tls_t *c, const mqtt_cfg_t *m, const char *topic,
                            const char *payload, bool retain, int64_t dl)
{
    uint8_t vh[320]; size_t n = 0;                              /* CONNECT: §3.1 */
    n += put_str(vh + n, "MQTT"); vh[n++] = 4;                  /* protocol level 4 = 3.1.1 */
    vh[n++] = 0x02 | (m->user[0] ? 0x80 : 0) | (m->pass[0] ? 0x40 : 0);   /* clean session, user, pass */
    vh[n++] = 0; vh[n++] = 30;                                  /* keepalive 30 s */
    n += put_str(vh + n, m->client_id);
    if (m->user[0]) n += put_str(vh + n, m->user);
    if (m->pass[0]) n += put_str(vh + n, m->pass);
    if (mq_send(c, 0x10, vh, n, dl) || mq_expect(c, 0x20, dl) || c->rx[3] != 0)
        return ESP_ERR_INVALID_RESPONSE;                        /* CONNACK rc != 0 → kimlik/yetki */
    size_t pn = 0; uint8_t *pub = c->txbuf;                     /* PUBLISH QoS1: 0x32 | retain */
    pn += put_str(pub + pn, topic); pub[pn++] = 0; pub[pn++] = 1;        /* packet id 1 */
    size_t pl = strlen(payload); memcpy(pub + pn, payload, pl); pn += pl;
    if (mq_send(c, 0x32 | (retain ? 1 : 0), pub, pn, dl)) return ESP_FAIL;
    if (mq_expect(c, 0x40, dl) || c->rx[2] != 0 || c->rx[3] != 1) return ESP_ERR_TIMEOUT;   /* PUBACK id 1 */
    static const uint8_t disc[2] = { 0xE0, 0x00 };              /* DISCONNECT, yanıt yok */
    net_tls_write(c, disc, 2, dl);
    return ESP_OK;
}
```

`mq_expect` sabit başlığı + kalan uzunluğu + gövdeyi `c->rx`'e okur ve tipi denetler; CONNACK dönüş kodları (§3.2.2.3): 1 protokol, 2 client id, 3 sunucu yok, 4 kullanıcı/parola, 5 yetki yok → `CONFIG` sınıfı; zaman aşımı/TCP → `RETRY`.

Alternatif (kabul edilebilir): esp-mqtt'yi "kural istisnası" olarak tutmak; o zaman `enqueue(store) → start → PUBLISHED|ERROR tek bekleme (≤ 15 s) → stop → destroy`, `network.timeout_ms = 8000`, `task.stack_size = 6144`, `crt_bundle_attach` zorunlu, toplam süre ≤ ~30 s diye belgelenir.

### Risk

- Kendi protokol kodu: broker uyumluluğu (Mosquitto, HA, EMQX, HiveMQ) sınanmalı; MQTT 5 ve WebSocket yok (ALTE'de de yok).
- Hafif istemcide yeniden gönderim yok; outbox katmanı zaten işi tekrar dener (QoS1 çift teslim olası, idempotent yük).

---

## 7. Seri teslimat ve güç

### Bulgu

- **Bağlantı başına süre (ESP32-C6, bileşenler):** DNS 50 ms–1 s (lwIP önbelleği ile sonraki sorgular anlık), TCP 20–200 ms, TLS 1.2 el sıkışması: doğrudan C6+mbedTLS ölçümü web'de yok. Dolaylı veriler: wolfSSL C6 ECDHE P-256 ≈ 320 ms/işlem (donanım fark etmemiş, [wolfSSL](https://www.wolfssl.com/espressif-risc-v-hardware-accelerated-cryptographic-functions-up-to-1000-faster-than-software/)); ESP32 forumlarında tam el sıkışma 1–4 s, ECC ağırlıklı ([esp32.com](https://esp32.com/viewtopic.php?p=17797)). C6'da mbedTLS donanım ECC yalnız **SECP192R1/SECP256R1** nokta çarpımı ve doğrulaması (`mbedtls/Kconfig` `MBEDTLS_HARDWARE_ECC` yardımı; `port/ecc/ecc_alt.c:57,88`), diğer eğriler yazılıma düşer (`MBEDTLS_ECC_OTHER_CURVES_SOFT_FALLBACK=y`); RSA/MPI donanımda (`soc_caps.h:55`). Sonuç: zincir biçimi süreyi belirler — Telegram/Outlook/iCloud tamamen RSA-2048 (hızlı); **Gmail'de WE2 → GTS Root R4 imzası ECDSA P-384 (yazılım)** → ölçülmeli; X25519 anahtar değişimi de yazılımdadır (ALTE `CURVE25519` açık, `sdkconfig:1922`). Kaba bütçe: **1–3 s** / bağlantı, Gmail +1–2 s olası. **Ölçüm planı:** `esp_timer_get_time()` ile `plain_tcp_connect` ve `conn_new_sync` çevresi, her sağlayıcıda, `CURVE25519` açık/kapalı iki yapı.
- **TLS 1.3 vs 1.2:** ALTE yalnız 1.2 (`sdkconfig:1810,1873`). 1.3 `MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` zorunlu, HKDF ve PSA crypto başlatma (`mbed.c:121-128`), post-handshake `NewSessionTicket` işleme döngüsü (`mbed.c:333-400`), daha büyük kod. Kazanç: 1 RTT daha az (≈ 50–150 ms WAN) — kripto süresinin yanında önemsiz. Beş uç nokta da TLS 1.2'yi kabul ediyor (canlı). **Öneri: 1.2'de kal**; boyut/heap etkisi ölçülmeden açılmasın.
- **Tamponlar:** `MBEDTLS_SSL_IN_CONTENT_LEN=16384`, `OUT=4096` (`sdkconfig:1801-1803`); `mbedtls_ssl_setup` giriş ≈ 16,4 KB + çıkış ≈ 4,1 KB'yi heap'ten alır (`ssl_tls.c:1380-1400`, `MBEDTLS_SSL_IN_BUFFER_LEN = 13 + yük`). Giriş 16 KB altına inmek **interop riski**: sunucu 16 KB'lik kayıt gönderebilir (RFC 8446 §5.1; MFL/record_size_limit uzantısına çoğu sunucu uymaz), el sıkışma `Certificate` iletisi tek kayıtta 3–6 KB (ölçülen zincirler 2–3 sertifika). 8 KB çoğu durumda çalışır ama "özel SMTP sunucusu, 4 sertifikalık RSA-4096 zinciri" senaryosunda düşer. Çıkış 4 KB ek parçası (4056 B) için yeterli. Heap kazancı istenirse `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` (+`DYNAMIC_FREE_CONFIG_DATA/CA_CERT`): IDF ölçümü `https_request` örneği **42 196 B → 22 013 B** (`docs/en/api-reference/protocols/mbedtls.rst:82-110`); `KEEP_PEER_CERTIFICATE=n` tek başına → 38 533 B.
- **Heap/yığın maliyeti:** IDF tablosu (`docs/.../esp_tls.rst:120-140`, `https_request`, IN 16384/OUT 4096): mbedTLS **toplam heap ≈ 37 KB, görev yığını kullanımı ≈ 3,6 KB**. esp-tls bağlamları `esp_tls_t` içinde **heap'te** (`esp_tls_init` calloc, `esp_tls.c:177`; `esp_tls_private.h:40-60`), ALTE'de ise yığında (`smtp.c:289-292`, ≈ 2–3 KB). Seri teslimat ile tepe heap = 1 TLS (~40 KB) + teslimat tamponları (~8 KB) + HTTP istemci (~2 KB) ≈ **50 KB**; ALTE'nin 3 eşzamanlı oturumu (~120 KB + 26 KB yığın) ortadan kalkar.
- **Güç:** TLS el sıkışması CPU'yu 160 MHz'de 1–3 s meşgul eder; WiFi TX ekli gönderimde sürekli (10 MB ≈ 10–30 s, WiFi/SPI-flash/TLS verimi ~0,5–1 MB/s tahmin). Seri teslimat tepe akımı (bir radyo + bir kripto patlaması) düşürür; ALTE'nin brownout endişesi (`SL_EV_MAIL_SEND` işareti, harita §2.6) için olumlu. Modem uyku kapalı olduğundan (harita §1.6) ortalama tüketimde fark küçük.

### Öneri

- `delivery` görevi **10 KB** yığın yeterli: TLS ≈ 3,6 KB + log/snprintf ≈ 1,5 KB + SMTP satır tamponu 1 KB; **ek parça tamponları (≈ 7 KB) heap'te/statik**; sınamada `uxTaskGetStackHighWaterMark` ≥ 2 KB koşulu. Gmail P-384 yolu yazılım ECC'si yığın kullanımını artırabilir → aynı ölçümde izlenir.
- Tamponlar 16384/4096 kalsın; `heap_caps_get_minimum_free_size` ekli teslimat sırasında < 60 KB'ye inerse `MBEDTLS_DYNAMIC_BUFFER` denensin (OTA'nın `esp_https_ota` oturumu da aynı kuraldan yararlanır).
- İş başına süre bütçesi: `10 (DNS/TCP) + 15 (TLS) + 30 × komut (SMTP ≤ 8 komut) + 120 (DATA sonu) + 60 × ek`; outbox "bir anda tek iş" ile ağ meşguliyeti üst sınırı hesaplanabilir (10 grup × 1 oturum).
- Tek `net_tls` modülü ölçümleri evlog'a yazsın: `dns_ms, tcp_ms, tls_ms, cipher, tls_ver` (GUI "son teslimat" özeti; alan sorunlarında teşhis).

### Risk

- Süre değerleri tahmindir; cihaz ölçümü yapılmadan outbox zaman aşımı sabitleri kesinleşmemeli.
- DNS'in `timeout_ms` dışında kalması (≤ 21 s) ve lwIP SYN yeniden denemelerinin `select` ile aşılması (`select` olmadan ≈ 12 × 1,5 s RTO ≈ 18–20 s, `tcp.c:1238,1295-1299`, `sdkconfig:1699,1715`) belgede kalsın; `getaddrinfo` için ayrı bütçe.

---

## 8. Plana etkisi

1. **§4.7 SMTP / §5.5 `net_tls`:** STARTTLS esp-tls ile **onaylandı** (`plain_tcp_connect` → `set_conn_sockfd` → `set_conn_state(CONNECTING)` → `conn_new_sync`); ilk cihaz sınaması Gmail 587 + 465. Elle mbedTLS sarmalayıcı **gerekmez**.
2. **§5.5 "insecure":** "doğrulama kapalı" seçeneği `CONFIG_ESP_TLS_INSECURE` + `SKIP_SERVER_CERT_VERIFY` ister; öneri: seçenek **"CA paketi / özel sertifika (PEM) / (kapalı: çıkar ya da kırmızı uyarı)"**. Ürün kararı 23 olarak eklenmeli. Proton Bridge yalnız PEM + ana makinede yönlendirme ile.
3. **§4.6 zaman aşımları:** "bağlantı 10 s" = soket + connect; DNS ayrı (≤ 21 s); el sıkışma 15 s `conn_new_sync`; her okuma/yazma `deadline`; SMTP son nokta 120 s; HTTP `timeout_ms` işlem başına (toplam ≤ ~60 s). Outbox sonuç sınıfları **`OK / RETRY / PERM / CONFIG`** — `CONFIG` deneme harcamaz, GUI'ye düşer.
4. **§4.7 SMTP içerik:** EHLO ayrıştırma (AUTH PLAIN/LOGIN seçimi, SIZE ön denetimi, 8BITMIME), çok satırlı yanıt, kod tablosu, dot-stuffing, CRLF normalizasyonu, RFC 2047 konu, `Date`/`Message-ID`, base64 76/satır, **parça 2964 B ham → 4056 B**, `EHLO [ip]`, grup başına tek oturum + çoklu RCPT (ürün kararı: alıcı görünürlüğü). AUTH satırları logda maskeli.
5. **Sağlayıcı gerçeği (GUI/README):** Gmail ve iCloud uygulama parolası ile çalışır; **Outlook.com kişisel çalışmaz**; Microsoft 365 iş 2026 sonuna kadar; `yapilacaklar`: XOAUTH2/Device Code araştırması.
6. **§4.7 MQTT:** esp-mqtt görev yaratır → **hafif MQTT 3.1.1 yayıncısı `net_tls` üzerinde** (−14 KB flash, kural ihlali yok); `retain` ayarı; esp-mqtt yedek plan. `mqtts://` için her iki yolda da CA paketi.
7. **§4.7 Telegram/webhook:** `parse_mode` yok, yanıt gövdesi okunur (`description`, `retry_after`), `buffer_size_tx=1024`, `max_redirection_count=2`, `Connection: close`, `esp_http_client_close` başka görevden **çağrılmaz**.
8. **§3.2 görevler:** `delivery` 10 KB yığın + heap'te parça tamponları; tek TLS oturumu/anda → tepe heap ≈ 50 KB; TLS 1.2 kalır, tamponlar 16384/4096 kalır, `DYNAMIC_BUFFER` ölçüme bağlı.
9. **§6.5 sınama:** cihazda ölçüm listesi — `dns/tcp/tls ms` beş uç için, `CURVE25519` açık/kapalı, heap min, yığın su izi; yıllık CA zinciri denetimi (`openssl s_client`), `cmn_crt_authorities.csv` kapsamı (GoDaddy G2, GTS R1–R4, GlobalSign, DigiCert G2, USERTrust, ISRG).
