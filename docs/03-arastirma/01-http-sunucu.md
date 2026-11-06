# 03.01 — HTTP sunucu araştırması: SSE, httpd ayarları, istemci IP'si, CSRF, cJSON, başlık ömrü, statik dosya

Plan: `02-plan-ilk.md` §6.1 ve §8 soru 1, 9, 12. Eski uygulama: `ALTE/HW/components/web_server/web_server.c`.
Kaynaklar: ESP-IDF v5.5.2 (`~/esp/esp-idf-v5.5.2`, kısaltma `IDF/`), `components/esp_http_server/src/*.c`
(kısaltma `httpd_*.c`), `components/json/cJSON` 1.7.19, lwIP port, `riscv32-esp-elf-gcc 14.2.0` ile
`-Os -fstack-usage` ölçümü, ALTE bağlantı haritası (`esp_bin/HW/build/lebensspur.map`), web (Fetch/Fetch-Metadata
belirtimleri, MDN). Satır numaraları IDF v5.5.2 kaynağına aittir.

Tasarım kararı (girdi): tek httpd görevi; handler'lar ağ istemcisi çalıştırmaz; SSE `/api/events` için
`httpd_req_async_handler_begin` ile soket ayrılır, `app_core` durum değişiminde `httpd_queue_work` ile push;
60 s kalp atışı; tek JSON zarfı; cJSON; IP başına hız sınırı ve giriş kilidi. **Sonuç: karar doğrulandı**;
aşağıda her soru için kanıt, iyileştirme ve riskler var.

---

## 1. SSE için async handler (`httpd_req_async_handler_begin/complete` + `httpd_queue_work`)

### Bulgu

**Tam semantik (5.5.2):**

- `httpd_req_async_handler_begin(r, &out)` — `httpd_txrx.c:632-687`: `httpd_req_t` kopyası (`malloc`, `memcpy`),
  `httpd_req_aux` kopyası, **scratch (ham istek başlıkları) kopyası** (`:658-668`, boyut = fiilen alınan başlık
  uzunluğu), `resp_hdrs` dizisi (`calloc(max_resp_headers)`, `:670-676`). Sonra `r_aux->remaining_len = 0`
  (`:679`, ana döngü gövdenin kalanını okumaz) ve `sd->for_async_req = true` (`:682`). Kopya üzerinde
  `httpd_req_get_hdr_value_str`, `httpd_resp_set_hdr`, `httpd_resp_send_chunk` çalışır (scratch ve resp_hdrs kopyalı).
- `httpd_req_async_handler_complete(r)` — `:689-723`: `sd->for_async_req = false`, scratch/resp_hdrs/aux/req
  serbest, kontrol soketine `HTTPD_CTRL_MAX` kukla mesaj (`:714-715`) → `select()` uyanır, fd yeniden izlenir.
- **`for_async_req = true` olan soket üç yerde ana döngüden çıkarılır:**
  - `select()` fd kümesine eklenmez — `httpd_sess.c:90-96` (`HTTPD_TASK_SET_DESCRIPTOR`).
  - Oturum işleme atlanır — `httpd_main.c:248-251`.
  - **LRU purge adayı olmaz** — `httpd_sess.c:111-118` (`FIND_LOWEST_LRU`: "Only close sockets that are not in use").
    Yani `lru_purge_enable=true` SSE soketini **kapatmaz**; `httpd_sess_update_lru_counter` çağırmaya gerek yok
    (ALTE `web_server.c:1416` bunu ham fd yaklaşımı için yapıyordu).
- **Async istek sayısı sınırı yok**; tek sınır `max_open_sockets`. Örnek `async_handlers/main/main.c:285-290`:
  "max_open_sockets > MAX_ASYNC_REQUESTS" — en az bir soket normal isteklere kalmalı. 2611'de SSE slot sayısını
  **biz** sınırlarız (öneri: 4).
- **Doğrulama:** `httpd_valid_req()` varsayılan derlemede `true`'dur (`esp_httpd_priv.h:361-365`; `CONFIG_HTTPD_VALIDATE_REQ`
  Kconfig'de yok). Açık olsaydı yalnız httpd görevinden çağrıyı kabul ederdi (`httpd_parse.c:866-879`). Bizim
  tasarımda tüm SSE gönderimleri zaten httpd görevinde (`httpd_queue_work` içinden) yapılır → iki durumda da güvenli.

**`httpd_queue_work` — `httpd_main.c:136-166`:**

- Loopback UDP soketine `sendto` (`ctrl_sock.c:71-95`, 127.0.0.1:`ctrl_port`); **herhangi bir görevden** çağrılabilir
  (app_core, esp_timer görevi, delivery…), **ISR'den çağrılamaz** (soket API'si). `app_core`'dan çağrı güvenli.
- Mesaj = `{hc_msg, fonksiyon, arg}` (12 B, `esp_httpd_priv.h:43-51`); iş fonksiyonu **httpd görevinde**
  `httpd_process_ctrl_msg` içinde çalışır (`httpd_main.c:199-235`).
- **Kuyruk kapasitesi = lwIP UDP posta kutusu** `CONFIG_LWIP_UDP_RECVMBOX_SIZE` (varsayılan 6, `lwip/Kconfig:831-836`;
  ALTE sdkconfig:1722 = 6). Dolunca `sendto` hata verir → `ESP_FAIL` ("failed to queue work"). Varsayılan
  **bloke etmez**; `CONFIG_HTTPD_QUEUE_WORK_BLOCKING=y` sayaç semaforla bekler (`httpd_main.c:148-165, 502-513`).
- Gereksinim: `CONFIG_LWIP_NETIF_LOOPBACK=y` (`ctrl_sock.c:32-35`; ALTE sdkconfig:1689 zaten `y`).
- Aynı mekanizma `httpd_sess_trigger_close` tarafından da kullanılır (`httpd_sess.c:476-482`) → kuyruğu paylaşırlar.

**Soket kapanışı nasıl anlaşılır:**

- Async soket `select()`'te olmadığından **istemcinin FIN'i ana döngüce fark edilmez**. Fark etme yolları:
  1. `httpd_resp_send_chunk` / `httpd_socket_send` hatası: `send()` → `httpd_sock_err` (`httpd_txrx.c:740-760`):
     `EAGAIN/EINTR → HTTPD_SOCK_ERR_TIMEOUT`, diğerleri (`ECONNRESET`, `EPIPE`, `ENOTCONN`) → `HTTPD_SOCK_ERR_FAIL`.
     İstemci kapattıysa genelde **ikinci** gönderimde hata gelir (ilki tampona girer, RST döner).
  2. `recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT)`: `0` → karşı taraf kapattı (FIN); `-1 && errno==EWOULDBLOCK` → açık.
     Her push/kalp atışında bir satır; anında ve ucuz. SSE istemcisi veri göndermez, `>0` dönerse anormal → kapat.
  3. Kaybolan eş (FIN yok, telefon menzil dışı): keepalive açılmadıysa lwIP yeniden iletim ile koparır:
     `TCP_MAXRTX=12` (sdkconfig:1698; `tcp.c:1241`), geri çekilme `{1,2,3,4,5,6,7,7,7,7,7,7,7}×RTO` (`tcp.c:163-164`)
     → toplam ≈ 63×RTO ≈ **1,5–3 dk** sonra `send()` `ECONNABORTED` döner. `keep_alive_enable=true`
     (`httpd_main.c:93-116`, varsayılan 5/5/3 s) bunu idle sokette ~20 s'ye indirir; her sokete uygulanır.
- `close_fn` (`esp_http_server.h:255-270`): her oturum silinişinde çağrılır; verildiyse `close(fd)` **bize** düşer.
- `httpd_sess_set_ctx` / `req->sess_ctx + free_ctx` (`httpd_sess.c:279-310`, `esp_http_server.h:385-423`):
  handler'da `req->sess_ctx=…; req->free_ctx=…` atanır, handler dönünce `httpd_req_cleanup` sokete kopyalar
  (`httpd_parse.c:738-741`); soket **hangi yoldan kapanırsa kapansın** (`httpd_sess_delete` → `httpd_sess_clear_ctx`,
  `httpd_sess.c:355-392`; `httpd_stop` → `close_all`) `free_ctx(ctx)` httpd görevinde çağrılır. SSE slotunu
  temizlemek için `close_fn`'den daha hedefli: soketi kapatma işi IDF'de kalır.
- **Dikkat — `httpd_sess_close` yarış koruması** (`httpd_sess.c:143-146`): `lru_counter==0 && !lru_socket` ise kapatma
  **atlanır**. İlk tamamlanmış istekten sonra sayaç ≥1 olur (`httpd_sess.c:436`); SSE handler'ı döndükten sonra
  `trigger_close` güvenli. Handler **içinden** aynı sokete `trigger_close` atmayın.

**Chunked kodlama ve SSE:** `httpd_resp_send_chunk` ilk çağrıda `HTTP/1.1 <durum>\r\nContent-Type: …\r\n
Transfer-Encoding: chunked\r\n` + `set_hdr` başlıkları yazar, sonra her parçayı `hex-uzunluk\r\n…\r\n` çerçeveler
(`httpd_txrx.c:336-410`). Tarayıcı `EventSource` chunked akışı sorunsuz okur. ALTE'nin sorunu (`web_server.c:1351-1354`)
chunked başlıkla ham `data:` satırlarını **karıştırmasıydı**; tüm gönderimler `send_chunk` ile yapılırsa çerçeve tutarlıdır
(`async_handlers` örneği de bunu yapar: `main.c:142-170`, 60 s boyunca `httpd_resp_sendstr_chunk`).

**Bellek (SSE istemcisi başına, async kopya):** `sizeof(httpd_req_t)` ≈ 4+4+(`CONFIG_HTTPD_MAX_URI_LEN`+1)+… ≈ 545 B
(URI 512) / 289 B (URI 256) (`esp_http_server.h:373-424`) + aux ≈ 80 B + scratch = gelen başlık boyutu (tarayıcı
400–900 B) + `resp_hdrs` 8×8 B → **≈ 1–1,5 KB**. 4 istemci ≈ 5 KB. Kabul edilebilir.

**ALTE ham fd yaklaşımıyla karşılaştırma** (`web_server.c:1330-1440`, `:2610-2616`):

| | ALTE (ham fd + `close_fn` + ayrı görev) | 2611 (async + `queue_work`) |
|---|---|---|
| Kapanış algılama | Bedava: soket `select()`'te, FIN → parse hatası → `sess_delete` → `close_fn` | Gönderim hatası / `MSG_PEEK` / RTO; kalp atışıyla ≤60 s |
| LRU purge | Tehlike: her gönderimde `update_lru_counter` zorunlu (`:1416`) | Purge dışı (`for_async_req`) |
| İstemci veri gönderirse | Sunucu SSE soketinde **yeni istek** ayrıştırıp akışa HTTP yanıtı yazar (bozulma) | Soket izlenmez; `MSG_PEEK` ile kapatılır |
| Başlıklar | Ham `HTTP/1.1` yazımı, `Transfer-Encoding` yok (`:1355-1362`) | IDF yazar; chunked |
| Görev / kilit | Ek görev 3 KB yığın, `xTaskNotify`, `s_sse_pending` | **Ek görev yok**; tüm erişim httpd görevinde → **mutex gerekmez** |
| `close_fn` | Zorunlu, `close(fd)` bizde (`:1373-1385`) | Gerekmez; `free_ctx` yeter |
| Kod | ~110 satır + görev | ~90 satır, görev yok |

Sağlamlık: 2611 yaklaşımı daha sağlam (LRU ve istek-karışması tuzakları yok), daha az kod ve daha az RAM (ek görev yok).
Tek bedeli FIN'in anında görülmemesi; `MSG_PEEK` denetimiyle kapanır.

### Öneri — **plan aynen, şu iyileştirmelerle**

1. **Mutex'li istemci listesi → kilitsiz liste.** Handler, `free_ctx`, push ve kalp atışı işleri hepsi httpd görevinde
   çalışır. Görevler arası tek paylaşılan şey `pending` bayrağı (atomik `bool`); `app_core` bayrak temizse
   `httpd_queue_work` çağırır (ALTE'nin `s_sse_pending` deseni, görev yok). Kuyruk dolu dönerse bayrağı geri temizle.
2. **Kalp atışı durum anlık görüntüsünü taşısın** (`event: state` ile aynı yük, 60 s). Kaçan bir push kendini
   ≤60 s'de onarır; `: hb` yorumuna göre maliyet ~150 B/dk/istemci.
3. **Slot sayısı 4** (`SSE_MAX`), 5. istemci → en eski slot kapatılır (ALTE gibi) — ya da 503; en eskiyi kapatmak
   tarayıcı yeniden bağlanmalarında (eski soket henüz ölmediyse) daha iyi.
4. **Canlılık:** her push/kalp atışında önce `recv(MSG_PEEK|MSG_DONTWAIT)`, sonra `send_chunk`; hata → `complete` +
   `trigger_close`. `keep_alive_enable` **kapalı** kalsın (her idle sokete 5 s'de bir prob = güç/radyo).
5. **Slot temizliği `free_ctx` ile** (soket IDF tarafından hangi yoldan kapatılırsa kapatılsın slot boşalır; `close_fn` ve
   elle `close()` yok).
6. **Kimlik:** handler'da çerez doğrulanır; yoksa JSON `401` (zarf) ve normal dönüş. Çıkışta (`/api/auth/logout`)
   o oturumun SSE slotları kapatılır (slotta oturum kimliği tutulur).
7. **Kuyruk:** `CONFIG_LWIP_UDP_RECVMBOX_SIZE=6` yeter (push 1 + kalp atışı 1 + ≤4 `trigger_close`); `BLOCKING`
   seçeneği **açılmasın** (app_core bloke olmamalı).

### Risk / dikkat

- `httpd_queue_work` ISR'den çağrılamaz; `esp_timer` callback'i görev bağlamıdır (sorun yok), GPIO ISR değil.
- `send()` httpd görevinde bloklar: `SO_SNDTIMEO = send_wait_timeout` (5 s) — tamponu dolu ölü bir istemci, bir push'u
  en fazla 5 s bekletir; `MSG_PEEK` denetimi ve küçük yükler (≤ 300 B, tampon 5760 B) bunu pratikte önler.
- İlk tamamlanmış istekten önce `trigger_close` atlanır (`httpd_sess.c:143`); SSE handler'ı içinde değil, iş
  fonksiyonunda kapat.
- `httpd_stop` sırasında `complete` içindeki kukla mesaj `msg_fd` kapalı olduğundan uyarı loglar (`httpd_main.c:337-339`);
  zararsız. Sunucu durdurulurken slot tablosunu `free_ctx` zaten temizler.

### Kod iskeleti (≤30 satır)

```c
typedef struct { httpd_req_t *req; int fd; uint32_t sess_id; } sse_slot_t;
static sse_slot_t s_sse[4];                     /* yalnız httpd görevinde dokunulur */
static atomic_bool s_sse_pending;

static void sse_slot_free(void *ctx) {          /* free_ctx: soket IDF'ce kapanınca */
    sse_slot_t *s = ctx; if (s->req) httpd_req_async_handler_complete(s->req);
    s->req = NULL; s->fd = -1;
}
static esp_err_t sse_handler(httpd_req_t *req) {   /* GET /api/events */
    sse_slot_t *s = sse_slot_take(req);            /* boş yoksa en eskiyi kapat */
    req->sess_ctx = s; req->free_ctx = sse_slot_free;
    if (httpd_req_async_handler_begin(req, &s->req) != ESP_OK) return http_err(req, 503);
    s->fd = httpd_req_to_sockfd(req);
    httpd_resp_set_type(s->req, "text/event-stream");
    httpd_resp_set_hdr(s->req, "Cache-Control", "no-cache");       /* sabit dize */
    return httpd_resp_send_chunk(s->req, "retry: 3000\n\n", HTTPD_RESP_USE_STRLEN);
}
static void sse_push_work(void *arg) {            /* httpd görevinde */
    atomic_store(&s_sse_pending, false);
    int n = status_to_sse(s_buf, sizeof s_buf);    /* "event: state\ndata: {...}\n\n" */
    for (int i = 0; i < 4; i++) { sse_slot_t *s = &s_sse[i]; char c; if (!s->req) continue;
        int r = recv(s->fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
        bool dead = (r == 0) || (r < 0 && errno != EWOULDBLOCK) || (r > 0);
        if (dead || httpd_resp_send_chunk(s->req, s_buf, n) != ESP_OK) {
            httpd_req_async_handler_complete(s->req); s->req = NULL;
            httpd_sess_trigger_close(s_server, s->fd); }
    }
}
void http_notify_state(void) {                    /* app_core'dan; ISR değil */
    if (s_server && !atomic_exchange(&s_sse_pending, true))
        if (httpd_queue_work(s_server, sse_push_work, NULL) != ESP_OK) atomic_store(&s_sse_pending, false);
}
```

---

## 2. httpd yapılandırma değerleri

### Bulgu

- Varsayılanlar `HTTPD_DEFAULT_CONFIG()` — `esp_http_server.h:53-81`: öncelik `tskIDLE_PRIORITY+5`, yığın 4096,
  `max_open_sockets` 7, `max_uri_handlers` 8, `max_resp_headers` 8, `backlog_conn` 5, `lru_purge_enable` false,
  `recv/send_wait_timeout` 5 s, `max_req_hdr_len=CONFIG_HTTPD_MAX_REQ_HDR_LEN` (1024), `max_uri_len=CONFIG_HTTPD_MAX_URI_LEN` (512).
- **Soket bütçesi:** `httpd_start` denetimi `CONFIG_LWIP_MAX_SOCKETS ≥ max_open_sockets + 3` (`httpd_main.c:480-495`:
  dinleme + 2 UDP kontrol soketi). `LWIP_MAX_SOCKETS` aralığı 1..253 (`lwip/Kconfig:128-134`); boş slot maliyeti
  statik `lwip_sock` dizisinde ~40–50 B. ALTE: 16 (sdkconfig:1649) ile `max_open_sockets=7` (`web_server.c:2614`,
  yorumdaki "13" ile çelişir; 7+3=10 kullanılıyordu).
  2611 eşzamanlı soket tahmini: httpd 10+3 = 13; `delivery` (SMTP/webhook/Telegram, tek seferde bir) 1; MQTT kalıcı 1;
  OTA (`sys_worker`) 1; mDNS 1–2 (IPv6 kapalı) → **18**. SNTP/DNS/DHCP ham pcb kullanır, soket saymaz.
- **Tek istek bir anda:** `httpd_data` içinde **tek** `hd_req` / `hd_req_aux` var (`esp_httpd_priv.h:140-141`);
  scratch dinamik, bloklar hâlinde `realloc` (`httpd_parse.c:493-513`), sınırı aşınca `431` (`:565`); URI sınırı
  `414` (`:560`). Yani `max_req_hdr_len`'i büyütmek **kullanılmadıkça** RAM harcamaz; async kopyaları hariç.
- **Yığın (ölçüm, `-Os`, rv32imac):** cJSON `parse_value` **80 B/seviye** (array/object inline), `parse_string` 64 B,
  `print_value` **192 B/seviye**, `print_string_ptr` 64 B. Derinlik 16 sınırıyla parse ≤ 1,3 KB, yazdırma ≤ 3 KB (üst
  üste değil). lwIP `send()` yolu ~1 KB, `ESP_LOG`/`snprintf` ~0,5 KB. Gövde (2 KB) ve yanıt (4 KB) tamponları
  **yığında olmazsa** 8 KB rahat; olursa dar.
- `httpd_resp_send_chunk` ilk çağrıda başlık için `malloc` (`httpd_txrx.c:349`), küçük/geçici.
- Zaman aşımları `accept` sonrası `SO_RCVTIMEO/SO_SNDTIMEO` olarak sokete yazılır (`httpd_main.c:76-91`); `recv` parse
  döngüsünde bloklar → yavaş istemci (slow-loris) tek görevli sunucuyu çağrı başına ≤ `recv_wait_timeout` tutar.
- `backlog_conn` → `listen()` (`httpd_main.c:388`); lwIP `TCP_LISTEN_BACKLOG=1` (`lwipopts.h:633`) → değer etkili.
- `core_id`: C6 tek çekirdek, `tskNO_AFFINITY`.
- ALTE `max_uri_handlers = rota+4` (`:2612`) ve `httpd_uri_match_wildcard` (`:2611`) ile ~45 kayıt; IDF eşleştirici
  yol parametresi (`/api/groups/<i>`) desteklemez; 404/405 metin gövdeli IDF hatası döner (`httpd_uri.c:305-316`).

### Öneri — **planı şöyle değiştir: rota kaydı + değer tablosu**

**Rota kaydı:** IDF'ye yöntem başına **tek** joker kayıt (`/*` × GET/POST/PUT/DELETE, `uri_match_fn=httpd_uri_match_wildcard`),
eşleştirme + parametre çıkarımı + ön işleyici (oturum → CSRF → gövde sınırı → cJSON) **kendi tablomuzda** (plan §6.1'deki
`{yol, yöntem, auth, csrf, max_body, handler}`). 404/405 de JSON zarfla döner. `max_uri_handlers = 8`.

| Alan | Değer | Gerekçe |
|---|---|---|
| `task_priority` | 5 | app_core 8 > delivery 5 ≥ httpd 5 > sys_worker 3 (plan §3.2). TLS yapan `delivery` ile eşit; httpd'yi 6'ya almak GUI'yi SMTP sırasında daha akıcı tutar (isteğe bağlı) |
| `stack_size` | 8192 | Ölçüme göre yeter **ancak** gövde/yanıt tamponları statik (bkz. §5/§6); geliştirmede `uxTaskGetStackHighWaterMark` ile doğrula |
| `core_id` | `tskNO_AFFINITY` | tek çekirdek |
| `max_open_sockets` | 10 | 4 SSE + 6 normal (tarayıcı başına ≤6 keep-alive); LRU purge SSE'yi atlamaz sorunu yok |
| `CONFIG_LWIP_MAX_SOCKETS` | **20** | 13 + delivery 1 + MQTT 1 + OTA 1 + mDNS 2 = 18, pay 2 |
| `lru_purge_enable` | true | boşta keep-alive soketleri gerektiğinde kapanır; async hariç |
| `max_uri_handlers` | 8 | yöntem başına tek joker |
| `max_resp_headers` | 10 | Cache-Control, Content-Encoding, ETag, Set-Cookie, X-Content-Type-Options, Referrer-Policy… |
| `CONFIG_HTTPD_MAX_REQ_HDR_LEN` / `max_req_hdr_len` | **2048** | Chrome: UA+sec-ch-ua+Accept*+Cookie+Origin+Sec-Fetch ≈ 800–1000 B; 1024 sınırda; büyütmek kullanılmadıkça bedava |
| `CONFIG_HTTPD_MAX_URI_LEN` / `max_uri_len` | **256** | en uzun yol `/api/groups/<i>/files/<ad>?…` < 128; `httpd_req_t.uri[]` küçülür (statik + async kopya başına 256 B kazanç) |
| `recv_wait_timeout` / `send_wait_timeout` | 5 / 5 | LAN; daha kısa = yavaş telefonlarda 408 riski |
| `backlog_conn` | 5 | ≤3 tarayıcı |
| `keep_alive_enable` | false | güç; SSE canlılığı §1 ile |
| `uri_match_fn` | `httpd_uri_match_wildcard` | tek joker kayıt |
| `CONFIG_HTTPD_QUEUE_WORK_BLOCKING` | n | app_core bloke olmasın |
| `CONFIG_HTTPD_WS_SUPPORT` | n | WebSocket yok (kod boyutu) |

### Risk / dikkat

- `max_open_sockets+3 > LWIP_MAX_SOCKETS` ise `httpd_start` `ESP_ERR_INVALID_ARG` ile başlamaz (`httpd_main.c:490-495`);
  `sdkconfig.defaults` değişince `sdkconfig` silinmeli (bellek notu).
- Slow-loris tek görevli tasarımın doğasıdır; LAN cihazında kabul; `recv_wait_timeout` 5 s üst sınır verir.
- SSE 4 slot + 6 normal soket toplamı dolunca LRU purge boşta keep-alive soketleri kapatır; hepsi aktifse yeni bağlantı
  `accept` edilmeden bekler (`httpd_main.c:270-275`).

---

## 3. İstemci IP'si (hız sınırı / giriş kilidi)

### Bulgu

- `httpd_req_to_sockfd(req)` → `ra->sd->fd` (`httpd_txrx.c:725-738`); yalnız handler bağlamında (async kopyada da) geçerli.
- `getpeername` lwIP'de var: `lwip_getpeername` → `lwip_getaddrname(s, name, namelen, 0)` (`lwip/src/api/sockets.c:2829-2831`).
- `CONFIG_LWIP_IPV6=n` (ALTE sdkconfig:1687) iken httpd dinleyicisi `PF_INET` + `sockaddr_in` ile `INADDR_ANY`'e bağlanır
  (`httpd_main.c:347-371`); peer adresi **`struct sockaddr_in`** gelir, IPv4-mapped (`::ffff:`) karmaşası yok.
  `INADDR_ANY` → AP (`192.168.4.x`) ve STA arayüzlerinden gelen bağlantılar aynı dinleyicide; `getpeername` her ikisinde
  gerçek istemci IPv4'ünü döner.
- ALTE'de IP yoktu; **global** kilit vardı (`web_server.c:146, 332-354`): bir saldırgan meşru kullanıcıyı 15 dk kilitler
  (DoS). IP başına kilit bunu giderir.

### Öneri — **plan aynen** (ayrıntı)

- Dispatcher başında bir kez: `getpeername` → `uint32_t ip = sin_addr.s_addr` → istek bağlamına yaz (µs mertebesi).
- Tablo: 8 giriş `{ip, fails, lock_until_s, last_seen_s}`; arama doğrusal; yer yoksa `last_seen` en eski atılır (LRU).
  Zaman kaynağı `esp_timer_get_time()/1e6` (uptime; duvar saati sıçramalarından bağımsız).
- Kural: 5 ardışık hatalı giriş → o IP 15 dk kilit (`lock_min` ayarlanabilir, ALTE'deki gibi); başarılı giriş sayacı sıfırlar.
- **Global arka durdurucu**: toplam giriş denemesi ≤ 10/dk (parola türetme CPU'su için); tablo taşması (8'den çok IP
  saldırıyor) tek görevli sunucuda zaten `max_open_sockets` ile sınırlıdır.
- Hız sınırı (genel `/api`): aynı tablo `{ip, token-bucket}` ile; 2611'de GUI SSE varken polling yapmaz (plan §6.3),
  bu yüzden 10 istek/s/IP cömert ve yeterli.

```c
typedef struct { uint32_t ip, last_s, lock_until_s; uint8_t fails; } ip_slot_t;
static ip_slot_t s_ip[8];
static uint32_t req_peer_ip(httpd_req_t *req) {
    struct sockaddr_in a; socklen_t l = sizeof a;
    int fd = httpd_req_to_sockfd(req);
    return (fd >= 0 && getpeername(fd, (struct sockaddr *)&a, &l) == 0) ? a.sin_addr.s_addr : 0;
}
static ip_slot_t *ip_slot(uint32_t ip, uint32_t now) {   /* bul ya da LRU ile aç */
    ip_slot_t *old = &s_ip[0];
    for (int i = 0; i < 8; i++) { if (s_ip[i].ip == ip) return &s_ip[i];
                                  if (s_ip[i].last_s < old->last_s) old = &s_ip[i]; }
    *old = (ip_slot_t){ .ip = ip, .last_s = now }; return old;
}
```

### Risk / dikkat

- `getpeername` 0 dönemezse (`ip=0`) kilidi **global** uygula (asla "sınırsız" düşme).
- Yönlendirici NAT'ı arkasından (port yönlendirme) tüm istekler tek IP görünür → tek kilit; ürün LAN hedefli, kabul.
- `CONFIG_LWIP_IPV6` ileride açılırsa dinleyici `PF_INET6` olur (`httpd_main.c:347-362`) ve `sockaddr_storage` +
  IPv4-mapped çözümü gerekir; bu dosyaya not düşüldü.

---

## 4. CSRF: `Origin`/`Host`, `Sec-Fetch-Site`, özel başlık

### Bulgu

- Başlık okuma: `httpd_req_get_hdr_value_str(req, "Origin", buf, n)` — alan adı karşılaştırması **büyük/küçük harf
  duyarsız** (`strncasecmp`, `httpd_parse.c:1086-1087`); uzunluk için `httpd_req_get_hdr_value_len` (`:1004-1056`).
  Başlıklar scratch tamponunda, yanıt gönderimine kadar canlı; `send_chunk` ilk çağrıda `req_hdrs_count=0` yapar
  (`httpd_txrx.c:341`) → **başlıkları gönderimden önce oku**.
- **`Origin` ne zaman gönderilir** (MDN *Origin*): "same-origin requests except for GET or HEAD requests (i.e., they are
  added to same-origin POST, OPTIONS, PUT, PATCH, and DELETE requests)" ve tüm cross-origin isteklerde. Yani same-origin
  `fetch()` POST'ta **gelir**; same-origin **GET'te gelmez**.
- **`null` olma koşulları** (MDN *Referrer-Policy*, "Effect on the Origin header"): "Requests using GET or HEAD, or made
  in `cors`, `websocket`, or `webtransport` mode, are never affected… For other requests — such as HTML form submissions
  or `fetch()` calls using `mode: "same-origin"` or `"no-cors"` — the user agent sets `Origin` to `null` when the
  referrer policy is `no-referrer`…". Not: "Because `fetch()` defaults to `mode: "cors"`, a same-origin `fetch()` POST
  always sends its real `Origin`, even under `Referrer-Policy: no-referrer`." → GUI `fetch()` kullandığı sürece
  `Referrer-Policy` seçimi `Origin` denetimini bozmaz. Form gönderimi kullanılmayacak.
- **`Sec-Fetch-Site` plain HTTP'de gelmez.** Fetch Metadata belirtimi §3 "append the Fetch metadata headers" adım 1:
  "If r's current URL is not an potentially trustworthy URL, return." (https://w3c.github.io/webappsec-fetch-metadata/).
  MDN: "The header is only included in requests to potentially trustworthy URLs." Cihaz `http://192.168.x.x` sunduğundan
  (TLS sunucu yok, `CONFIG_MBEDTLS_TLS_CLIENT_ONLY=y`) **bu başlık hiç gelmez** → alternatif değil.
- **Özel başlık ve preflight** (MDN *CORS*, "Simple requests"): basit istek = GET/HEAD/POST + yalnız güvenli listedeki
  başlıklar + `Content-Type` ∈ {`application/x-www-form-urlencoded`, `multipart/form-data`, `text/plain`}. `X-LS-Request`
  ya da `Content-Type: application/json` isteği basit olmaktan çıkarır → **cross-origin** çağrı `OPTIONS` preflight ister;
  sunucu CORS başlığı dönmez (OPTIONS kayıtlı değil → IDF `405`) → tarayıcı asıl isteği **hiç göndermez**. CORS/preflight
  yalnız cross-origin için vardır; **same-origin isteklerde preflight olmaz** (MDN: "browsers restrict cross-origin HTTP
  requests initiated from scripts"); GUI aynı origin'den çalışır → sıfır ek tur.
- HTML `<form>` özel başlık koyamaz; `<img>/<script>` yalnız GET → özel başlık tek başına klasik CSRF'yi kapatır.
- **DNS rebinding** LAN cihazları için asıl tehdittir: saldırgan sayfası `evil.com`'u cihaz IP'sine çözdürür; tarayıcı
  için same-origin olur, `Origin: http://evil.com`, `Host: evil.com`. Çerez `evil.com`'a ait olmadığından gönderilmez
  (oturumlu uçlar güvende) ama oturumsuz uçlar (`/api/auth` durumu, giriş POST'u) ve özel başlıklı istek (same-origin
  sayıldığı için preflight yok!) ulaşır. **`Host` allowlist** (cihaz IP'leri, `hostname`, `hostname.local`, `:80` ile/siz)
  bunu keser.

### Öneri — **planı şöyle değiştir: kombinasyon**

Durum değiştiren tüm uçlar (POST/PUT/DELETE) için sırayla:

1. `Host` ∈ {AP IP, STA IP, hostname, hostname.local} (port eki serbest) — değilse `403 bad_request` (**tüm** `/api/*` için,
   GET dahil; DNS rebinding).
2. `X-LS-Request: 1` zorunlu — yoksa `403`. GUI `api.js` her çağrıda ekler.
3. `Origin` varsa `== "http://" + Host` olmalı; `null` ya da farklı → `403`. Yoksa (curl gibi tarayıcı dışı istemci)
   2. madde zaten koruyor → kabul.
4. Gövdeli isteklerde `Content-Type: application/json` zorunlu (basit istek dışına ikinci çıkış).
5. Çerez: `ls_sess=…; Path=/; HttpOnly; SameSite=Strict` (`Secure` konulamaz, HTTP). Strict, cross-site navigasyonda
   çerezi taşımaz; SPA ilk yükten sonra `fetch` ile same-site çağırdığı için sorun olmaz.
6. `Sec-Fetch-Site` **kullanılmaz**.
7. `GET /api/reset?key=` tek durum değiştiren GET: yetki = anahtar; CSRF denetimi yok, IP hız sınırı var (§3).
8. Yanıt başlıkları (bir kez, statik): `X-Content-Type-Options: nosniff`, `Referrer-Policy: same-origin`,
   `Content-Security-Policy: default-src 'self'` (inline script yoksa) — `max_resp_headers` 10 bunu karşılar.

### Risk / dikkat

- Tarayıcıların "Private/Local Network Access" korumaları (Chrome) ek katmandır, ama Firefox/Safari'de yok → güvenmeyin.
- `Host` listesi IP değişince (DHCP) güncellenmeli: `IP_EVENT_STA_GOT_IP` olayından `http` modülüne bildirim.
- `Origin` karşılaştırması tam dize eşitliği (sonda `/` yok); büyük/küçük harf: host kısmı küçük harfle normalize edilir.

---

## 5. cJSON

### Bulgu

- IDF `json` bileşeni = cJSON **1.7.19** (`components/json/cJSON/cJSON.h:82-84`); `cJSON.c` + `cJSON_Utils.c`
  derlenir (`json/CMakeLists.txt`), Utils kullanılmazsa bağlayıcı atar.
- **Boyut:** ALTE haritasında `libjson.a` `.text` **9 480 B** + `.rodata` 70 B; bağımsız derleme (`-Os`, rv32imac)
  `cJSON.o` text 9 752 B. ALTE zaten cJSON bağlıyor (`ls_util`, `api_client`; `web_server.c` kendi mini ayrıştırıcısını
  kullanıyordu, 0 cJSON çağrısı). 2611'de **ek flash maliyeti ≈ 0**; aksine mini ayrıştırıcı kalkar.
- API: `cJSON_ParseWithLength(buf, len)` (`cJSON.h:155`, `cJSON.c:1142`) — NUL sonlandırma gerekmez, `httpd_req_recv`
  tamponu doğrudan verilir; dizgeler kopyalanır (`parse_string`, `cJSON.c:819`), tampon parse sonrası yeniden kullanılabilir.
- **Parse belleği:** düğüm = `sizeof(cJSON)` 36 B → heap bloğu ~48 B; anahtar `strdup`; 2 KB'lık ~40–60 alanlı gövde →
  ~4–5 KB, ~120 küçük blok; `cJSON_Delete` ile hepsi geri verilir. Parçalanma, tahsisler arasına başka uzun ömürlü
  tahsis girmezse (handler ağ çağrısı yapmaz, plan) pratikte birleşir.
- **İç içe sınırı tehlikesi:** `CJSON_NESTING_LIMIT` 1000 (`cJSON.h:136-137`; denetim `cJSON.c:1497, 1657`), ölçülen
  `parse_value` çerçevesi **80 B/seviye** → sınır tetiklenmeden **80 KB yığın** gerekir; 2 KB gövde (`[[[[…`) 2048 seviye
  taşır → 8 KB httpd yığını **taşar** (stack overflow panik). Gövde sınırı tek başına korumaz.
- **`cJSON_PrintPreallocated(item, buf, len, format)`** (`cJSON.c:1343-1360`): `noalloc=true`; sığmazsa `ensure` `NULL`
  döner (`cJSON.c:513-515`) → `false`, taşma yok; cJSON notu (`cJSON.h:168`): tahmin 5 B'ye kadar şaşabilir, tamponu +5 B tut. Sayılar
  `double` → `%1.15g`/`%1.17g`; tam sayılar 2^53'e kadar kayıpsız; `valueint` `INT_MAX`'ta doyar → aralık denetimi.
- `cJSON_Print` (heap) 256 B'den başlayıp ikiye katlayarak `realloc` eder (`cJSON.c:1234-1283, 532`) → parçalanma
  kaynağı; **kullanılmaz**.
- `cJSON_Minify`: sunucu tarafında gereksiz (ayrıştırıcı boşluğu kabul eder; GUI zaten sıkışık gönderir).
- Alternatifler: `jsmn`/`espressif/json_parser` (+`json_generator`) ~3–4 KB flash kazandırır ama DOM API'siz
  ~45 uç için handler kodu şişer; cJSON zaten bağlı → anlamsız.

### Öneri — **plan aynen, şu eklerle**

1. **`CJSON_NESTING_LIMIT=16`** proje `CMakeLists.txt`'te: `idf_build_set_property(COMPILE_DEFINITIONS
   "-DCJSON_NESTING_LIMIT=16" APPEND)` (makro `#ifndef` ile korunur; en derin belgemiz 5). Ek güvenlik: ön işleyici gövdede
   dize dışı `[`/`{` sayısını 32'de keser (10 satır).
2. **Statik tamponlar, tek istek bir anda:** gövde 2 KB + yanıt 4 KB (+5) `http` modülünde statik → yığın ve heap
   değişmez; `cJSON_PrintPreallocated(root, s_resp, sizeof s_resp - 5, false)`.
3. Yanıt üretiminde kopyasız kurucular: `cJSON_AddItemToObjectCS` (sabit anahtar), `cJSON_CreateStringReference`
   (sabit dize) → tahsis sayısı yarıya iner.
4. Büyük çıktı (`/api/config/export`, `/api/logs`): bölüm başına ayrı `cJSON` ağacı → 4 KB tampona yaz → `send_chunk`;
   zarf `{"ok":true,"data":{` … `}}` elle. Tek ağaç + tek tampon **kurulmaz**.
5. `cJSON_Delete` handler çıkışında hemen; ağaç yapı dışına sızmaz (dispatcher sahipliği).
6. İsteğe bağlı (ölçüm gerektirirse): `cJSON_InitHooks` ile 8 KB statik bump-arena (free no-op, istek sonunda sıfırla) →
   sıfır heap; şart: cJSON yalnız httpd görevinde kullanılır (delivery payload'ları `snprintf` şablonu).

### Risk / dikkat

- Heap parçalanması ölçülmeden iddia edilemez; sınama listesine `heap_caps_get_largest_free_block` 1000 istek
  öncesi/sonrası farkı (plan §6.5).
- `cJSON_GetNumberValue` `double` döner; `uint32` alanlar (saniye, gün) `0 ≤ v ≤ sınır` ve `v == floor(v)` denetimiyle alınır.
- `CONFIG_NEWLIB_NANO_FORMAT` kapalı (sdkconfig:2409); açılırsa `%g` sayı yazımı bozulur — kapalı tutulmalı.

---

## 6. Yanıt başlığı değerlerinin ömrü (`httpd_resp_set_hdr` kopyalamıyor)

### Bulgu — **W1 doğrulandı**

- `httpd_resp_set_hdr` yalnız işaretçi saklar: `ra->resp_hdrs[n].field = field; .value = value;`
  (`httpd_txrx.c:189-192`). Yazım anı: `httpd_resp_send` → `:282-296`; `httpd_resp_send_chunk` ilk parça → `:368-385`.
- Aynı kural `httpd_resp_set_status` (`ra->status = status`, `:213`) ve `httpd_resp_set_type` (`ra->content_type = type`, `:232`) için.
- Async kopyada `resp_hdrs` dizisi kopyalanır ama değer işaretçileri aynıdır (`:676`) → SSE için yalnız sabit dizeler.
- Tek `hd_req_aux` olduğundan (`esp_httpd_priv.h:141`) bir anda tek senkron istek işlenir.

### Öneri — **plan aynen**: istek bağlamı yapısı

```c
typedef struct {            /* http modülünde STATİK tek örnek: bir anda tek istek */
    uint32_t  ip;           /* §3 */
    char      set_cookie[160];
    char      etag[24];
    char      body[2048];   /* §5 */
    char      resp[4096 + 5];
    route_t  *route; uint8_t params[4]; …
} http_ctx_t;
static http_ctx_t s_ctx;

static esp_err_t dispatch(httpd_req_t *req) {       /* IDF joker handler */
    memset(&s_ctx, 0, offsetof(http_ctx_t, body));  /* büyük tamponları silme */
    …
    if (need_cookie) {
        snprintf(s_ctx.set_cookie, sizeof s_ctx.set_cookie,
                 "ls_sess=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%u", tok, ttl);
        httpd_resp_set_hdr(req, "Set-Cookie", s_ctx.set_cookie);   /* send'e kadar canlı */
    }
    return httpd_resp_send(req, s_ctx.resp, n);     /* başlıklar burada yazılır */
}
```

Kural (denetim listesi maddesi): `httpd_resp_set_hdr/status/type`'a verilen her işaretçi ya **dize sabiti** ya da
`s_ctx` alanıdır; handler yerel dizisi **yasak**. `ESP_LOG`/`snprintf` araya girse de `s_ctx` bozulmaz.

### Risk / dikkat

- `s_ctx` yalnız httpd görevinde; `httpd_queue_work` iş fonksiyonları da httpd görevinde çalıştığından bir handler'ın
  ortasında **giremez** (işler `select()` döngüsünde, handler bittikten sonra işlenir) → paylaşım güvenli.
- Async (SSE) yanıtında `s_ctx` **kullanılmaz** (handler dönünce başka istek üzerine yazar); sabit dizeler.

---

## 7. Statik gömülü gzip dosyaları

### Bulgu

- lwIP (ALTE sdkconfig): `TCP_MSS=1440` (:1700), `TCP_SND_BUF=5760` = 4×MSS (:1704), `TCP_WND=5760` (:1705),
  `TCP_OVERSIZE=MSS` (:1712, `lwipopts.h:640`). Gönderim pbuf'ları talep üzerine ayrılır; `SND_BUF` üst sınırdır.
- `httpd_resp_send(req, buf, len)` → `httpd_send_all` döngüsü (`httpd_txrx.c:68-84`); `send()` tampon dolunca bloklar,
  `SO_SNDTIMEO` 5 s/çağrı, kısmi dönüş döngüyle tamamlanır. `Content-Length` IDF tarafından yazılır.
- `EMBED_FILES` ile gömülü `.gz` **flash'ta bellek eşlemeli sabit veri**: tek `httpd_resp_send(req, start, end-start)`
  yeter; parça/chunked gerekmez; lwIP veriyi MSS'lik segmentlere kendisi böler. Chunked kodlama (`send_chunk`) her
  parçaya ~8 B çerçeve ve ilk çağrıda `malloc` ekler (`:349`).
- Güç: radyo süresi ≈ toplam bayt (gzip belirleyici); uygulama parça boyutu segment sayısını **değiştirmez** (lwIP
  birleştirir); çok küçük parçalar yalnız CPU/bağlam değişimi artırır. Büyük ve az parça doğru yön; en büyüğü = tek çağrı.
- ALTE: 4 KB `malloc` parça + `send_chunk` (`web_server.c:555-587`); `immutable`/`no-cache` ve `Content-Encoding: gzip`
  (`:609-611`); ETag yok.
- `immutable` ile hash'li adlar: tarayıcı yeniden doğrulamaz (Firefox `immutable`'ı uygular; Chrome yeniden yüklemede
  alt kaynakları zaten doğrulamaz) → **ETag gereksiz**. `index.html` `no-cache` → her açılışta ~11 KB; `ETag` + 304 bunu
  ~200 B'ye indirir.

### Öneri — **planı şöyle değiştir**

1. Gömülü dosyalar: **tek `httpd_resp_send`**, chunked yok. Başlıklar (sabit dizeler): `Content-Encoding: gzip`,
   `Cache-Control: public, max-age=31536000, immutable` (hash'li), `Content-Type` uzantıdan, `X-Content-Type-Options: nosniff`.
2. LittleFS'ten okunan dosyalar (grup ekleri, yedek indirme): `s_ctx.resp` (4 KB) ile `send_chunk`; ayrı `malloc` yok.
   4 KB ≥ 2×MSS ve LittleFS sektör katı; daha küçük parça fayda getirmez.
3. **`index.html` için `ETag: "<sürüm>"`** (statik dize, derlemede) + `If-None-Match` eşleşirse
   `httpd_resp_set_status(req, "304 Not Modified"); httpd_resp_send(req, NULL, 0)`. Diğer dosyalara ETag yok.
   `Cache-Control: no-cache` kalır.
4. `Vary: Accept-Encoding` gereksiz (gzip'siz varyant yok). `Accept-Encoding` denetlenmez (tüm tarayıcılar gzip; `curl`
   için `--compressed` notu README'ye).
5. İsteğe bağlı, ölçüme bağlı: sayfa açılışı yavaşsa `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=11520` (8×MSS; RAM yalnız
   kullanımda) — varsayılan kalsın, sınamada ölçülsün.

### Risk / dikkat

- `httpd_resp_send` uzun gövdede httpd görevini gönderim süresince tutar (33 KB dil dosyası ≈ LAN'da <100 ms; SSE
  push'ları beklemede kalır) — kabul.
- `Content-Length` olan yanıtta `send_chunk` ile karıştırma yapılmaz (ALTE'nin SSE hatasının statik versiyonu).
- 304 yanıtında `Content-Type`/`Content-Encoding` başlıkları gönderilmemeli (IDF `Content-Type` yazar; `text/html`
  zararsız).

---

## Plana etkisi (özet)

1. **§6.1 SSE:** karar doğrulandı. Değişiklikler: "mutex'li istemci listesi" → **kilitsiz** (tüm erişim httpd görevinde;
   tek paylaşım atomik `pending`); 4 slot; slot temizliği `req->sess_ctx + free_ctx`; canlılık `recv(MSG_PEEK)` + gönderim
   hatası; kalp atışı **durum anlık görüntüsü** taşır (kaçan push 60 s'de onarılır); `close_fn`, ek görev,
   `update_lru_counter`, ham başlık yazımı **yok**; tüm gönderim `httpd_resp_send_chunk`.
2. **§6.1 rota tablosu:** IDF'ye yöntem başına tek `/*` kaydı; eşleştirme/parametre/404/405 bizim tabloda (JSON zarf).
3. **sdkconfig.defaults (2611):** `CONFIG_HTTPD_MAX_REQ_HDR_LEN=2048`, `CONFIG_HTTPD_MAX_URI_LEN=256`,
   `CONFIG_LWIP_MAX_SOCKETS=20`, `CONFIG_HTTPD_WS_SUPPORT=n`, `CONFIG_HTTPD_QUEUE_WORK_BLOCKING=n`, `LWIP_NETIF_LOOPBACK=y`
   (zaten), `CONFIG_NEWLIB_NANO_FORMAT=n` (zaten). CMake: `-DCJSON_NESTING_LIMIT=16`.
4. **httpd_config:** öncelik 5 (6 düşünülebilir), yığın 8 KB, `max_open_sockets` 10, `lru_purge` açık, `max_uri_handlers`
   8, `max_resp_headers` 10, zaman aşımı 5/5 s, `backlog` 5, keepalive kapalı, joker eşleştirici.
5. **Tampon stratejisi (yeni madde §6.1):** tek istek bir anda → `http` modülünde **statik istek bağlamı** (gövde 2 KB,
   yanıt 4 KB+5, çerez/ETag dizileri, IP); başlık değerleri yalnız sabit dize ya da bağlam alanı (W1 kapanır); heap
   yalnız cJSON ağacı, handler sonunda geri.
6. **CSRF (§5.7 / §6.1):** `Host` allowlist (DNS rebinding) + `X-LS-Request: 1` zorunlu + `Origin == http://Host`
   (varsa; `null` red) + `Content-Type: application/json` + çerez `HttpOnly; SameSite=Strict`. `Sec-Fetch-Site` plain
   HTTP'de gelmediğinden **kullanılmaz**. `Referrer-Policy` serbest (fetch cors modunda `Origin` gerçek kalır).
7. **Giriş kilidi / hız sınırı (§5.7):** IP başına 8 girişli LRU tablo (`getpeername`, `sockaddr_in`, IPv6 kapalı) + global
   arka durdurucu; ALTE'nin global kilidi kalkar (DoS).
8. **cJSON (§8 s.12):** kalır (≈0 ek flash); `ParseWithLength`, `PrintPreallocated`, CS kurucular, bölüm başına
   chunked export; iç içe sınırı 16 — **zorunlu** (8 KB yığın taşması).
9. **Statik (§6.1):** gömülü dosyalar tek `httpd_resp_send`; chunked yalnız LittleFS; `ETag`/304 yalnız `index.html`.
10. **Sınama listesi (§6.5) ekleri:** `uxTaskGetStackHighWaterMark(httpd)` derin JSON ve en büyük yanıtla; heap en büyük
    blok 1000 istek öncesi/sonrası; SSE: istemci kapatınca ≤60 s'de slot boşalıyor mu; 5. SSE istemcisi; `[[[[…` 2 KB
    gövde → 400 ve çökme yok; cross-origin POST tarayıcıda preflight ile reddediliyor mu; `Host: evil.com` → 403.
11. **§8 soru 1, 9, 12:** cevaplandı; ileride IPv6 açılırsa `getpeername` ve dinleyici notu (§3 risk).
