# 03 — Araştırma: OTA, coredump, bölüm tablosu

Kapsam: `02-plan-ilk.md` §5.6 (OTA), §5.8 (bölüm tablosu), §8 soru 3, 4, 7. Kaynaklar: ESP-IDF v5.5.2 (`~/esp/esp-idf-v5.5.2`, kısaltma `IDF/`), ALTE `fw_ota.c`, harita `03-depolama-gunluk-ota.md` §6/§8, ve 2026-11-05 tarihli canlı GitHub ölçümleri (curl/openssl/gh). Satır numaraları IDF 5.5.2'ye aittir.

Kısa karar özeti (ayrıntılar aşağıda):

| Konu | Karar |
|---|---|
| Sürüm denetimi | GitHub API yok. `HEAD /releases/latest` → `Location` başlığından tag; indirmede `esp_https_ota_get_img_desc` ile ikinci kapı. |
| Depo paylaşımı | **Engel:** `smrtkrft/LebensSpur` deposunun `releases/latest`'i bugün `DMF_v1.0.6` / `SmartKraft_DMF.ino.bin`. Ayrı depo ya da ürün adı denetimi şart. |
| HTTP tampon | GitHub imzalı URL 927 B; `buffer_size_tx` varsayılan 512 → "Out of buffer". ALTE bugün GitHub'dan OTA **yapamaz**. 2048 ayarla. |
| Abort | Yazım başlamadan abort → flaş'a hiç dokunulmaz. Yazım sonrası abort → pasif slot yarım, **pasif otadata girdisi silinmiş** olur (begin'de). |
| Onay | `esp_ota_begin` PENDING_VERIFY'da OTA'yı reddeder; onay koşulu `sys_worker`'da, timer callback'inde değil. Anti-rollback kapalı kalsın. |
| Coredump | ELF + CRC32 + 128 KB yeter (tahmini 40–70 KB). RISC-V özeti backtrace değil ham yığın verir; çözümleme masaüstünde. |
| Bölüm tablosu | 4 MB'a tam oturuyor. `phy_init` gereksiz. Eski NVS verisi **silinmez**, görünür kalır → tek seferlik `erase-flash`/şema denetimi. |

---

## 1. GitHub API'siz sürüm denetimi

### Bulgu

**1a. Yönlendirme zinciri (canlı ölçüm, 2026-11-05).**

```
HEAD https://github.com/smrtkrft/LebensSpur/releases/latest
  → 302  location: https://github.com/smrtkrft/LebensSpur/releases/tag/v1.0.6
         cache-control: no-cache   (ETag yok, gövde yok)

HEAD https://github.com/smrtkrft/LebensSpur/releases/latest/download/SmartKraft_DMF.ino.bin
  → 302  location: https://github.com/smrtkrft/LebensSpur/releases/download/v1.0.6/SmartKraft_DMF.ino.bin
  → 302  location: https://release-assets.githubusercontent.com/github-production-release-asset/...?...&jwt=...
  → 200  accept-ranges: bytes, content-length: 1472608, etag: "0x8DE1C9DEE5ADB7E"
GET -r 0-1023 → 206, content-range: bytes 0-1023/1472608
```

- İkinci sıçramanın `Location` uzunluğu **927 B**, yol+sorgu **883 B**; ana makine artık `objects.githubusercontent.com` değil `release-assets.githubusercontent.com`. İmzalı URL'deki JWT'de `exp − nbf = 300 s` (5 dakika geçerlilik).
- Hiçbir adım `api.github.com`'a gitmez → 60/saat anonim kota ve JSON kesme sorunu **tamamen kalkar**.
- **Depo paylaşımı sorunu:** `gh release list` → tek release `DMF_v1.0.6` (tag `v1.0.6`), tek varlık `SmartKraft_DMF.ino.bin` (1 472 608 B). `releases/latest` GitHub'da depo başına tektir; LebensSpur 2611 bu depoyu DMF ile paylaştığı sürece `releases/latest` **DMF'yi** gösterir. Semver ile `1.0.6 > 0.0.70` → "güncelleme var". `esp_https_ota` yalnız chip-id/revizyon denetler (`IDF/components/esp_https_ota/src/esp_https_ota.c:646-667`), **`project_name` denetlemez**; varlık adı çakışsaydı DMF imajı (C6 için derlenmişse) cihaza yazılırdı.

**1b. `esp_https_ota` yönlendirme davranışı.**

- `_http_connect` (`esp_https_ota.c:155-209`): `open → fetch_headers → _http_handle_response_code → while(process_again(status))`. 301/302/303/307/308'de `esp_http_client_set_redirection` (`esp_http_client.c:1062-1077`) yalnızca `set_url(location)` yapıp `redirect_counter++` sayar; **`max_redirection_count` denetimi yoktur**. O denetim yalnız `esp_http_check_response` (`esp_http_client.c:1088-1096`) içindedir ve onu yalnız `esp_http_client_perform` çağırır. Sonuç: ALTE'nin `.max_redirection_count = 5` ayarı (`fw_ota.c:270`) normal OTA yolunda **etkisizdir**; sadece `partial_http_download` HEAD isteğinde (`esp_https_ota.c:387-400`, `perform` ile) geçerlidir. Sonsuz yönlendirme döngüsüne karşı koruma yok (GitHub için pratik risk düşük).
- `Location` başlığı istemcide özel alanda birikir (`esp_http_client.c:270-275`, `http_utils_append_string` ile realloc → uzunluk sınırı yok). `esp_http_client_get_header` **istek** başlıklarını okur (`:370-377`), yanıt başlığını değil; yanıt başlığına tek açık yol `HTTP_EVENT_ON_HEADER` olayıdır (`:242-256`, `event.header_key/header_value`).
- **İstek tamponu:** `http_client_prepare_first_line` ilk satırı (`"GET <yol>?<sorgu> HTTP/1.1"`) `buffer_size_tx` içine `snprintf` ile yazar; sığmazsa `"Out of buffer"` → −1 → istek başarısız (`esp_http_client.c`, `prepare_first_line`; varsayılan `DEFAULT_HTTP_BUF_SIZE` 512, `include/esp_http_client.h:19`). GitHub'ın 883 B yol+sorgusu 512'ye sığmaz. ALTE `buffer_size_tx` ayarlamıyor (`fw_ota.c:265-271`) → **ALTE bugünkü GitHub altyapısıyla ikinci sıçramada başarısız olur**. (RX tarafı sorun değil: parser akışlıdır, `Location` heap'te büyür.)

**1c. İmaj tanımını yazmadan önce okuma.**

- `esp_https_ota_begin` bağlantıyı kurar, yönlendirmeleri izler, `image_length = content-length`, pasif bölümü seçer, `ota_upgrade_buf = malloc(max(buffer_size, 1024))` ayırır; **`esp_ota_begin` çağırmaz** (`esp_https_ota.c:294-537`). Durum `ESP_HTTPS_OTA_BEGIN`.
- `esp_https_ota_get_img_desc` → `get_description_from_image` (`:574-634`): durum ≥ BEGIN ise `read_header` (`:539-572`) tam **1024 B** (`IMAGE_HEADER_SIZE`, `:22`) okur, `binary_file_len = 1024`; `esp_app_desc_t` ofseti `sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)` = 32 (`:604`), `magic_word` denetlenir (`:620`). Flaş'a **hiçbir şey yazılmaz, silinmez**.
- İlk `esp_https_ota_perform` (`:685-736`): `esp_ota_begin(staging, erase_size)` burada çağrılır; `binary_file_len` doluysa önceden okunan 1024 B yazılır (`:697-699, 736`). `bulk_flash_erase=false` → `OTA_WITH_SEQUENTIAL_WRITES` → sektörler yazıldıkça silinir (`esp_ota_ops.c:316-327`); `true` → `image_length` kadar alan ilk perform'da topluca silinir (`esp_ota_ops.c:187-199`). `partial_http_download` ile: begin'de HEAD + `Range: bytes=0-(max-1)`, ilk perform'da yine 1024 B'lik başlık (`max_http_request_size ≥ 1024` olmalı).
- Sürüm eski/aynıysa `esp_https_ota_abort` (`:888-923`): durum BEGIN'de `esp_ota_abort` çağrılmaz (handle yok); tampon `free`, `_http_cleanup` (close+cleanup), handle `free`. **Flaş değişmez.**

**1d. Varlık adı 404.** `releases/latest/download/lebensspur.bin` bugün 404 → `_http_handle_response_code` `"File not found(404)"` → `ESP_FAIL` (`esp_https_ota.c:127-129`) → begin başarısız; yani yanlış ürün yanlışlıkla **inmez**, ama kullanıcıya "güncelleme var" denmiş olur.

### Öneri — tam akış

1. **Ürün kararı (engel):** ya 2611 için ayrı GitHub deposu (önerilen), ya da `releases/latest` kullanılmaz; aynı depoda kalınacaksa sürüm bilgisi sabit yoldan küçük bir manifest ile verilir (`raw.githubusercontent.com/<o>/<r>/<dal>/release/lebensspur.json`, ≤300 B: `{"version":"0.1.0","url":"https://github.com/.../releases/download/ls-v0.1.0/lebensspur.bin"}`) — dal bağımlı, kota yok, ama "release" kavramından kopuk. Aşağıdaki akış ayrı depo varsayar.
2. **Denetim (`ota_check`, `sys_worker`):** `HEAD https://github.com/<o>/<r>/releases/latest`, `disable_auto_redirect=true`, `crt_bundle_attach`, `timeout_ms=10000`, `buffer_size_tx=1024`; `HTTP_EVENT_ON_HEADER`'da `Location` yakalanır, `/releases/tag/` sonrası tag alınır (`v` öneki atılır). `status != 302 || tag yok` → hata. `semver_cmp(tag, çalışan) > 0` → `UPDATE_AVAILABLE{tag}`, aksi `UP_TO_DATE`. Son görülen tag NVS'e yazılır (aynı tag için tekrar bildirim yok). Gövde inmez, kota yok.
3. **İndirme (`ota_start`):** resmi URL doğrudan `https://github.com/<o>/<r>/releases/download/<tag>/lebensspur.bin` (bir sıçrama az) ya da özel https URL. `esp_https_ota_begin` → `esp_https_ota_get_img_desc` → üç kapı: `project_name == "lebensspur"` (`esp_app_desc.c:70` 32 kr), `semver_cmp(yeni, çalışan) > 0` (özel URL'de `allow_same` seçeneği yoksa eşit de ret), (isteğe bağlı) `idf_ver` günlüğe. Ret → `esp_https_ota_abort` → `REJECTED_{PROJECT,OLDER,SAME}`. Geçerse perform döngüsü (§2), `finish`, `pending_version` kaydı, `sys_restart(RST_OTA)`.
4. `partial_http_download=false`, `bulk_flash_erase=false`, `buffer_size=2048`, **`buffer_size_tx=2048`**, `timeout_ms=10000`, `keep_alive_enable=true` (sıçramalar farklı ana makine; zararsız).

```c
/* ota_check: HEAD /releases/latest → Location → tag (gövde yok, API kotası yok) */
static esp_err_t on_http_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "Location")) {
        const char *p = strstr(e->header_value, "/releases/tag/");
        if (p) strlcpy((char *)e->user_data, p + 14, 32);       /* "v0.1.0" */
    }
    return ESP_OK;
}
esp_err_t ota_check_latest_tag(char tag[32])
{
    tag[0] = 0;
    esp_http_client_config_t c = {
        .url = OTA_LATEST_URL, .method = HTTP_METHOD_HEAD,
        .disable_auto_redirect = true, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000, .buffer_size_tx = 1024,
        .event_handler = on_http_evt, .user_data = tag,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_http_client_perform(h);               /* 302'de ESP_OK döner */
    int st = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (err != ESP_OK) return err;
    return (st == 302 && tag[0]) ? ESP_OK : ESP_ERR_NOT_FOUND;
}
```

### Risk

- **Yüksek:** depo paylaşımı (DMF). Ayrı depo olmadan hem "güncelleme var" yanlış pozitifi hem de ad çakışmasında yanlış imaj.
- **Yüksek:** `buffer_size_tx` ayarlanmazsa GitHub'dan indirme ikinci sıçramada ölür (ALTE'de böyle).
- Orta: `_http_connect` yönlendirme sayacı yok → kötü niyetli özel URL sonsuz döngüye sokabilir; `timeout_ms` her turda bağlanır ama tur sayısı sınırsız. Özel URL'de yalnızca https ve yönlendirme sayısını `HTTP_EVENT_ON_HEADER` ile saymak (≥5'te bayrak → begin sonrası abort) hafifletir.
- Düşük: GitHub `releases/latest` ön-sürümleri (pre-release) atlar; "latest" tanımı GitHub'ındır (yayın tarihi değil, en son oluşturulan non-prerelease).

---

## 2. `esp_https_ota_abort` semantiği

### Bulgu

- `esp_https_ota_abort` (`esp_https_ota.c:888-923`): `IN_PROGRESS/SUCCESS` → `esp_ota_abort(update_handle)`; her durumda `ota_upgrade_buf` free, `_http_cleanup` (soket + TLS bağlamı serbest, `:211-215`), handle free. `esp_ota_abort` (`esp_ota_ops.c:438-448`) yalnız handle'ı listeden çıkarır; **flaş/otadata'ya yazmaz**.
- Yazım başlamışsa bölümün durumu: ardışık silmede ilk yazımda sektör 0 silinir (`esp_ota_ops.c:316-322`) → pasif slottaki eski imajın `esp_app_desc` sihri yok olur → `esp_ota_get_partition_description(other)` `ESP_ERR_NOT_FOUND` (`esp_ota_ops.c:825-827`). Bir sonraki OTA `esp_ota_begin` ile baştan başlar; elle silme gerekmez.
- **Plan belgesindeki "otadata değişmez" ifadesi yanlış.** `esp_ota_begin`, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` açıkken `esp_ota_invalidate_inactive_ota_data_slot()` çağırır (`esp_ota_ops.c:198-202` → `:1094-1097` → `erase_last_boot_app_partition(skip_app_part_erase=true)` `:1041-1087`): **pasif otadata sektörü silinir** (aktif girdi dokunulmaz). Bu, yarım imaja önyükleme olasılığını kapatır; yan etkisi: abort sonrası `esp_ota_check_rollback_is_possible()` false döner ve GUI'de "geri dön" seçeneği (doğru olarak) kaybolur. `esp_ota_set_boot_partition(other)` zaten imajı `esp_image_verify` ile doğrular (`esp_ota_ops.c:599-641`), yarım imaja geçiş mümkün değil.
- `esp_ota_begin`, çalışan imaj `PENDING_VERIFY` ise `ESP_ERR_OTA_ROLLBACK_INVALID_STATE` döner (`esp_ota_ops.c:164-172`): **onaylanmamış imaj OTA başlatamaz** (§3'e bağlanır).
- **Perform bloklaması:** `IN_PROGRESS`'te tek `esp_http_client_read(…, ota_upgrade_buf_size)` (`esp_https_ota.c:748-750`; boyut `max(buffer_size,1024)`, `:501`). `esp_transport_read` `timeout_ms` ile sınırlı; zaman aşımında `-ESP_ERR_HTTP_EAGAIN` (`esp_http_client.c:1355-1361`) → perform `ESP_ERR_HTTPS_OTA_IN_PROGRESS` döner (`esp_https_ota.c:778-781`). Tek çağrı en kötü ≈ `timeout_ms` bloklar. `esp_https_ota_begin` ise DNS+TCP+TLS+3 sıçrama = sıçrama başına `timeout_ms`'e kadar (ALTE'nin 120 s'i ile dakikalarca). IDF örneği 5 s kullanır (`examples/system/ota/advanced_https_ota/main/Kconfig.projbuild:25-27`).
- Abort bayrağı: `_http_connect` begin içinde döner; oradan çıkış yok. Perform döngüsü arasında bayrak yoklamak tek kalıp (IDF örneği de böyle, `advanced_https_ota_example.c:296-305`).

### Öneri

- `timeout_ms = 10000` (begin ≤ ~40 s, perform turu ≤ 10 s). TWDT 10 s ise `sys_worker`'ı TWDT'ye **abone etme** ya da her turda `esp_task_wdt_reset`; `timeout_ms` < TWDT olmalı.
- İptal: `app_core` tetiklemede `atomic_bool ota_cancel` kurar (kuyruk değil; `sys_worker` döngüde). Döngü: `perform → cancel? abort : progress`. Abort sonrası durum `ABORTED{bytes}` ve evlog.
- GUI "geri dön" bilgisi `esp_ota_check_rollback_is_possible()` + pasif bölüm `esp_app_desc` okumasıyla **her istekte** üretilsin; ALTE'nin önbelleklenen `s_rollback_available` hatası (harita §8/9) böyle kapanır.

```c
/* sys_worker: OTA işi (begin + img_desc kapıları §1'de) */
while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    if (atomic_load(&s_ota_cancel)) { esp_https_ota_abort(h); return OTA_ABORTED; }
    int got = esp_https_ota_get_image_len_read(h), tot = esp_https_ota_get_image_size(h);
    ota_progress(tot > 0 ? got * 100 / tot : -1);           /* durum anlık görüntüsü → SSE */
    esp_task_wdt_reset();                                     /* abone ise */
}
if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) { esp_https_ota_abort(h); return OTA_FAILED; }
err = esp_https_ota_finish(h);                                /* doğrular + otadata NEW yazar */
```

### Risk

- Orta: begin sırasında iptal edilemez (≤ ~40 s). Tetikleme bu pencereye denk gelirse röle/outbox yine `app_core`'da çalışır; yalnız OTA biraz geç iptal olur. Kabul edilebilir; GUI'de "iptal ediliyor" göstergesi.
- Düşük: `esp_https_ota_finish` içindeki `esp_ota_end` → `esp_image_verify` tüm imajı okur (1.3 MB, ~1 s); bu sırada yoklama yok.

---

## 3. Onay ve rollback

### Bulgu

- Bootloader (`IDF/components/bootloader_support/src/bootloader_utility.c:396-404`): her açılışta `PENDING_VERIFY` olan her otadata girdisi **önce** `ABORTED` yapılır; sonra seçilen girdi `NEW` ise `PENDING_VERIFY` yazılır (`:446-451`). Yani yeni imajın **tek** açılış hakkı vardır; `esp_ota_mark_app_valid_cancel_rollback` çağrılmadan herhangi bir reset (brownout, WDT, kullanıcı) → bir sonraki açılış eski imaj.
- `esp_ota_mark_app_valid_cancel_rollback` → `esp_ota_current_ota_is_workable(true)` aktif girdiyi `VALID` yazar (`esp_ota_ops.c:904-947`; otadata sektör yazımı — timer callback'inde değil görevde). `esp_ota_mark_app_invalid_rollback_and_reboot` (`:954-961`) `esp_ota_check_rollback_is_possible()` (`:848-902`) ister: diğer otadata girdisi geçerli **ve** o slottaki imaj `esp_image_verify` ile sağlam olmalı; sonra `INVALID` + `esp_restart`.
- OTA sonrası otadata: begin'de pasif girdi silinir (§2), `finish` → `esp_ota_set_boot_partition(staging)` → `esp_rewrite_ota_data` yeni girdiyi diğer sektöre `seq+1, NEW` yazar (`esp_ota_ops.c:552-597`). Yeniden açılışta aktif = yeni (PENDING_VERIFY), pasif = eski (VALID) → rollback mümkün.
- `esp_ota_get_next_update_partition(NULL)` **çalışan** bölümden sonrakini döner (`esp_ota_ops.c:737-747`); OTA yazıldıktan sonra bu "az önce yazılan" imajdır → ALTE'nin `rollback_version=<yeni>` hatası (harita §8/9) buradan.
- Rollback olduğunu açılışta anlamak: `esp_ota_get_last_invalid_partition()` (`esp_ota_ops.c:982-1005`) `INVALID/ABORTED` girdili ve imajı sağlam slotu döner; `esp_ota_get_partition_description` ile o imajın sürümü okunur → evlog "ota_rollback from vX".
- Anti-rollback: `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` (`IDF/components/bootloader/Kconfig.app_rollback:15-21`; sorudaki `CONFIG_APP_ROLLBACK_ANTI_ROLLBACK` adı yok), `BOOTLOADER_APP_SECURE_VERSION` eFuse sayacı; `esp_ota_mark_app_valid` eFuse yakar (`esp_ota_ops.c:834-845, 922-926`); geri dönüş imkânsızlaşır, fabrika bölümü yasak (`bootloader_utility.c:431`), `check_rollback_is_possible` daha sıkı (`:870-895`). Secure boot yokken anlamı yok.

### Öneri

- **Koşullu onay yeri:** açılışta `esp_ota_get_state_partition(running) == PENDING_VERIFY` ise `sys_worker`'a tek seferlik "ota_confirm" işi; iş, `app_core` durum anlık görüntüsünden `http_up && (sta_connected || uptime ≥ 120 s)` koşulunu 1 s'de bir yoklar (ya da olaylardan tetiklenir), sağlanınca `esp_ota_mark_app_valid_cancel_rollback()` + evlog `ota_confirmed vX`. Timer callback'inde flaş yazımı yok (ALTE `main.c:72` bunu zaten ayrı göreve taşımıştı; tutarlı hale gelir).
- Onay beklerken `ota_start` ve "geri dön" reddedilir (`ESP_ERR_OTA_ROLLBACK_INVALID_STATE` zaten gelir; GUI'ye "ilk açılış onayı bekleniyor" mesajı).
- Açılışta rollback tespiti → evlog `boot: rollback, failed=vX, running=vY`; kullanıcıya GUI rozeti ("son güncelleme geri alındı").
- Sürüm raporu (her istekte hesaplanır):
  - `boot != running` → `pending_version = desc(boot)`, `rollback_available=false` (yeniden başlatma bekleniyor).
  - aksi → `rollback_available = esp_ota_check_rollback_is_possible()`, `rollback_version = desc(next_update_partition)` yalnız `rollback_available` ise.
  - `last_invalid = esp_ota_get_last_invalid_partition()` → `last_failed_version`.
- Anti-rollback **kapalı** kalsın: kullanıcı elle düşürme/geri dönme hakkını korumalı; sürüm politikası yazılımda (`semver_cmp`), `allow_downgrade` yalnız özel URL + açık onay ile.
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` kalır; `CONFIG_BOOTLOADER_WDT` 9 s kalır (ilk açılış takılırsa rollback).

### Risk

- Orta: 120 s içinde router yoksa ve http ayağa kalkmadıysa onay gelmez → sonraki reset'te geri dönüş; kullanıcı "güncelleme yapıldı ama eski sürüm" görür. evlog + GUI rozeti bunu görünür kılar. 120 s koşulu zaten STA olmadan da onaylar; http'nin AP modunda ayağa kalkması yeterli.
- Düşük: `esp_ota_mark_app_valid` otadata yazarken güç giderse girdi CRC'siz kalır → bootloader diğer girdiyi seçer → eski imaj. Tek seferlik, tekrar OTA ile çözülür.

---

## 4. Semver

### Bulgu

- `esp_app_desc_t.version` `char[32]` (`IDF/components/esp_app_format/include/esp_app_desc.h`), `PROJECT_VER`'den kopyalanır (`esp_app_desc.c:32`), `_Static_assert(sizeof(PROJECT_VER) <= 32)` (`:66`) → en çok 31 karakter. `project_name` de 32 (`:70`).
- `set(PROJECT_VER "0.0.70")` (`ALTE/HW/CMakeLists.txt:7`) `project()`'ten önce serbest biçimlidir; `tools/cmake/project.cmake:88`'deki `^[0-9]+(\.[0-9]+)*$` denetimi yalnız `project(ad VERSION x)` sözdizimine uygulanır. Dolayısıyla `0.1.0-rc1` kullanılabilir. Kaynak yoksa `version.txt` / `git describe` (`project.cmake:685-687`).
- GitHub tag'ı `v` önekli (`v1.0.6` gözlendi); `strip` gerekir. IDF örneği yalnız `memcmp` eşitliği yapar (`advanced_https_ota_example.c:186`) — plan "yalnız daha yeni" istiyor.

### Öneri — `semver_cmp` (≤40 satır) ve testler

Kurallar: `[vV]MAJOR.MINOR.PATCH[-pre[.pre…]][+meta]`; sayısal alanlar sayı olarak; ön-sürümsüz > ön-sürümlü; ön-sürüm tanıtıcıları noktayla bölünür, sayısal < alfasayısal, sayısal sayı olarak, aksi ASCII, kısa olan küçük; `+meta` yok sayılır; bozuk biçim `SEMVER_BAD`.

Doğrulama (2026-11-05): aşağıdaki kod 38 satır; macOS `cc -std=c11 -Wall -Wextra` ve `riscv32-esp-elf-gcc -march=rv32imac_zicsr_zifencei -Os -Wall -Wextra` ile uyarısız derlendi; aşağıdaki 11 test + 7 ek sınır durumu (NULL, `"1.0.0x"`, `"2.0.0"<"10.0.0"`, `rc1+meta` eşitliği, simetri) host'ta çalıştırıldı: 18/18 geçti.

```c
#define SEMVER_BAD INT_MIN
static const char *sv_core(const char *s, long out[3])
{
    if (!s) return NULL;
    if (*s == 'v' || *s == 'V') s++;
    for (int i = 0; i < 3; i++) {
        char *e;
        if (!isdigit((unsigned char)*s)) return NULL;
        out[i] = strtol(s, &e, 10); s = e;
        if (i < 2 && *s++ != '.') return NULL;
    }
    return (*s == 0 || *s == '-' || *s == '+') ? s : NULL;    /* "" | "-pre" | "+meta" */
}
static int sv_pre_cmp(const char *a, const char *b)
{
    int ha = (*a == '-'), hb = (*b == '-');
    if (ha != hb) return ha ? -1 : 1;                           /* ön-sürümsüz büyük */
    if (!ha) return 0;
    for (a++, b++;; a += (*a == '.'), b += (*b == '.')) {
        size_t la = strcspn(a, ".+"), lb = strcspn(b, ".+");
        if (!la || !lb) return la == lb ? 0 : (la ? 1 : -1);    /* uzun olan büyük */
        char *ea, *eb; long na = strtol(a, &ea, 10), nb = strtol(b, &eb, 10);
        int da = (size_t)(ea - a) == la, db = (size_t)(eb - b) == lb;
        if (da && db) { if (na != nb) return na < nb ? -1 : 1; }
        else if (da != db) return da ? -1 : 1;                  /* sayısal < alfasayısal */
        else { int r = strncmp(a, b, la < lb ? la : lb); if (r) return r < 0 ? -1 : 1;
               if (la != lb) return la < lb ? -1 : 1; }
        a += la; b += lb;
    }
}
int semver_cmp(const char *a, const char *b)                    /* <0 a<b, 0 eşit, >0 a>b */
{
    long va[3], vb[3];
    const char *ra = sv_core(a, va), *rb = sv_core(b, vb);
    if (!ra || !rb) return SEMVER_BAD;
    for (int i = 0; i < 3; i++) if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    return sv_pre_cmp(ra, rb);
}
```

Unity (masaüstü, `tests/`):

```c
TEST_ASSERT_EQUAL(0,  semver_cmp("0.0.70", "v0.0.70"));
TEST_ASSERT(semver_cmp("0.0.9",  "0.0.70") < 0);                 /* sayısal, sözlüksel değil */
TEST_ASSERT(semver_cmp("0.0.70", "0.1.0") < 0);
TEST_ASSERT(semver_cmp("1.0.0-rc1", "1.0.0") < 0);
TEST_ASSERT(semver_cmp("1.0.0-alpha", "1.0.0-alpha.1") < 0);
TEST_ASSERT(semver_cmp("1.0.0-alpha.1", "1.0.0-alpha.beta") < 0);
TEST_ASSERT(semver_cmp("1.0.0-beta.2", "1.0.0-beta.11") < 0);
TEST_ASSERT_EQUAL(0,  semver_cmp("1.0.0+b1", "1.0.0+b2"));
TEST_ASSERT_EQUAL(SEMVER_BAD, semver_cmp("1.0", "1.0.0"));
TEST_ASSERT_EQUAL(SEMVER_BAD, semver_cmp("custom", "1.0.0"));
TEST_ASSERT(semver_cmp("1.0.6", "0.0.70") > 0);                  /* DMF tag'ı: ürün adı kapısı şart */
```

### Risk

- Düşük: `PROJECT_VER` üç yerde elle (harita §7: CMakeLists, `config.json`, `index.html ?v=`); 2611'de tek kaynak + derleme zamanı üretim (plan §6.4) bu riski kaldırır. Bu belge bunu değiştirmez.

---

## 5. https zorunluluğu, TLS, tampon ve yığın

### Bulgu

- `is_server_verification_enabled` yalnız `cert_pem || use_global_ca_store || crt_bundle_attach` bakar (`esp_https_ota.c:288-292`); `esp_https_ota_begin` bunların hiçbiri yoksa ve `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP` kapalıysa `ESP_ERR_INVALID_ARG` döner (`:308-316`). **Şema denetimi yoktur**: `crt_bundle_attach` ayarlıyken `http://` URL kabul edilir, `esp_http_client` şemaya göre düz TCP açar (harita §6.1'i doğrular). Aynı şekilde `https → http` yönlendirmesi de izlenir (`set_redirection` → `set_url` şemayı değiştirir).
- `esp_http_client_get_transport_type()` (`esp_http_client.h:723`, `HTTP_TRANSPORT_OVER_SSL`) ve `esp_http_client_get_url()` (`:836`) mevcuttur; `http_client_init_cb` (`esp_https_ota.h:63`) istemci tutamacını verir → begin sonrası (yönlendirmeler bittikten sonra) son taşıma türü denetlenebilir.
- CA paketi: ALTE `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN` (`esp_bin/HW/sdkconfig:1823`), 44 kök (`IDF/components/mbedtls/esp_crt_bundle/cmn_crt_authorities.csv`); `github.com` → Sectigo / **USERTrust ECC** (satır 43), `release-assets.githubusercontent.com` → Let's Encrypt / **ISRG Root X1** (satır 35). Her ikisi pakette (openssl ile 2026-11-05 doğrulandı).
- `partial_http_download`: `CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD` sembolü 5.5.2'de **hiçbir Kconfig'de yok** → `esp_https_ota.c:167-178` kalıcı bağlantı bloğu ölü kod; her parça için `esp_http_client_close` + `_http_connect` (`:794-823`) = yeni TLS el sıkışması. 1.3 MB / 64 KB ≈ 20 el sıkışma; GitHub imzalı URL'si 300 s'de dolar → yavaş WiFi'de ortada 403. Tek kazanım mbedTLS RX tamponunu 16 KB→4 KB düşürmek (`docs/en/api-reference/system/esp_https_ota.rst:48`); C6'da 512 KB SRAM ile gereksiz. GitHub Range'i destekliyor (206 ölçüldü) ama kullanmaya değmez.
- Yığın: IDF örneği 8 KB (`advanced_https_ota_example.c:411`, `cert_pem` ile); ALTE 12 KB (`fw_ota.c:32`) ve çalıştı. Sertifika paketi doğrulaması + ECDSA (github.com ECC sertifika) + `ESP_LOG` biçimlendirme 8 KB'de dar. Plan §3.2 `sys_worker` 8 KB.
- Boyut referansı (ALTE map `esp_bin/HW/build/lebensspur.map`): `esp_https_ota.c.obj` 7 567 B, `esp_http_client.c.obj` 12 366 B, `esp_ota_ops.c.obj` 5 736 B (text+rodata).

### Öneri

- Uygulama katmanında: `strncasecmp(url, "https://", 8) == 0`, kullanıcı bilgisi (`@`) yok, ana makine boş değil; aksi 400. `http_client_init_cb` ile tutamaç saklanır; `esp_https_ota_begin` döndükten sonra `esp_http_client_get_transport_type(h) != HTTP_TRANSPORT_OVER_SSL` ise `abort` (yönlendirme ile http'ye düşmeyi kapatır). `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP` kapalı kalır (ikinci savunma: sertifika seçeneği hiç verilmezse ret).
- `esp_http_client_config_t`: `crt_bundle_attach`, `timeout_ms 10000`, `buffer_size 2048`, **`buffer_size_tx 2048`**, `keep_alive_enable`, `event_handler` (yönlendirme sayacı için `Location` sayımı, ≥5 → bayrak). `esp_https_ota_config_t`: `partial_http_download=false`, `bulk_flash_erase=false`, `http_client_init_cb`.
- `sys_worker` yığını **12 KB** (plan §3.2'deki 8 KB'yi büyüt); ilk canlı OTA'da `uxTaskGetStackHighWaterMark` ölçülüp 10 KB'ye çekilebilir. OTA sırasında heap: TLS ≈ 40–50 KB (16 KB in + 4 KB out + zincir) + 2 KB tampon; `MBEDTLS_SSL_IN_CONTENT_LEN=16384` kalır.
- Özel URL sunucusunun sertifikası 44 kök CA'dan birine zincirlenmeli; GUI yardım metni bunu söyler (kendi CA'lı sunucu desteklenmez — yapilacaklar: `cert_pem` yükleme).

### Risk

- Orta: `https → http` yönlendirmesi begin içinde izlenir; başlık inmiş olur ama imaj yazılmaz (abort begin sonrası). Kabul.
- Düşük: 44 köklük paket büyük ticari CA'ları kapsar; eksik kök → `ESP_ERR_ESP_TLS…` → GUI'de "sertifika doğrulanamadı".

---

## 6. Coredump

### Bulgu

- Kconfig (`IDF/components/espcoredump/Kconfig`): hedef seçimi `ESP_COREDUMP_ENABLE_TO_FLASH` (`:3-21`, varsayılan NONE — ALTE/Neue'de NONE, `sdkconfig:1411/1742`); biçim varsayılan **ELF** (`:25-33`); sağlama varsayılan **CRC32** (`:37-47`, SHA256 yalnız ELF, daha çok yığın ister); `ESP_COREDUMP_CHECK_BOOT=y` (`:61-64`); `ESP_COREDUMP_MAX_TASKS_NUM=64` (`:83-90`, çöken görev her zaman dahil, diğerleri öncelik sırasıyla); `ESP_COREDUMP_STACK_SIZE` C6'da varsayılan **0** = çöken görevin yığınında çalışır (`:118-130`; belge >1300 önerir, `core_dump.rst:62`); `ESP_COREDUMP_SUMMARY_STACKDUMP_SIZE=1024` yalnız RISC-V (`:132-139`); `ESP_COREDUMP_LOGS=y` ≈5 KB DRAM (`:75-81`); `ESP_COREDUMP_CAPTURE_DRAM` tüm .bss/.data/heap, ≥128 KB ve fazlası (`:50-59`); `ESP_COREDUMP_FLASH_NO_OVERWRITE` (`:100-109`).
- Boyut: belge formülü `20 + N × (12 + TCB + yığın)` (`docs/en/api-guides/core_dump.rst:109`). Uygulama yalnız **kullanılan** yığını yazar: `stack_start = pxTopOfStack`, `stack_end = pxEndOfStack` (`core_dump_common.c:240-241`; çöken görevde istisna çerçevesinden itibaren `:246`). 12 uygulama + ~6 IDF görevi (`esp_timer`, `tiT`, `sys_evt`, `wifi`, `httpd`, `IDLE`, `Tmr Svc`) ≈ 18 görev; kullanılan yığın toplamı 30–40 KB + TCB (~0,4 KB × 18) + ELF not/başlıkları ~2 KB → **tipik 40–50 KB, en kötü ≈70 KB**. Sığmazsa `esp_core_dump_flash_write_prepare` `"Not enough space"` → `ESP_ERR_NO_MEM`, **hiç yazılmaz** (`core_dump_flash.c:216-220`); yalnız gereken sektörler silinir (`:231-239`). 128 KB ≈ 2× pay; `CAPTURE_DRAM` açılmadıkça yeterli.
- Açılış: `ESP_SYSTEM_INIT_FN(init_coredump, SECONDARY, …, 130)` otomatik `esp_core_dump_init` (`core_dump_init.c:18-22`) → bölüm bulunur, `CHECK_BOOT` ile `esp_core_dump_image_check` (`core_dump_flash.c:312-325`; dumpı 32 B'lik okumalarla tarar, `:327-401`). Bölüm yoksa yalnız log (`:117-128`), panik yazımı `hw_init`'te reddedilir (`:99-103`) — çökme yok (eski ALTE tablosuyla çalışan cihaz için önemli).
- Dönüşler: `esp_core_dump_image_check/get`: `ESP_ERR_NOT_FOUND` (bölüm yok), **`ESP_ERR_INVALID_SIZE` (boş: ilk 4 bayt 0xFFFFFFFF, `:475-477`)**, `ESP_ERR_INVALID_CRC` (bozuk), `ESP_OK`. `esp_core_dump_image_erase` bölümü silip başa `BLANK_COREDUMP_SIZE = 0xFFFFFFFF` yazar (`:17, 403-443`) → sonraki `image_get` `INVALID_SIZE` döner (başlık yorumundaki "NOT_FOUND" ifadesi kodla uyuşmuyor, `esp_core_dump.h:117-124`). Uygulama ikisini de "dump yok" saymalı.
- Özet: `esp_core_dump_get_summary` (`core_dump_elf.c:1016-1065`) bölümü mmap'ler (sağlama **yeniden doğrulanmaz** → önce `image_check`), not bölümünden `exc_task[16]`, `exc_pc`, `exc_tcb`, `app_elf_sha256[]`, `core_dump_version`, RISC-V ek bilgisi `ex_info.{mstatus,mtvec,mcause,mtval,ra,sp,exc_a[8]}` (`include/port/riscv/esp_core_dump_summary_port.h`) ve `exc_bt_info = {stackdump[1024], dump_size}` — **RISC-V'de backtrace adres listesi yok, çöken görevin ham yığın kopyası var** (`core_dump_port.c:412-426`); çözümleme masaüstünde ELF ile. `esp_core_dump_get_panic_reason(buf,n)` panik metnini verir (`core_dump_elf.c:991-1014`; "Guru Meditation…", "Task watchdog…", `abort()`). İkisi de yalnız `TO_FLASH && ELF` ile derlenir (`esp_core_dump.h:126-175`). Özet yapısı ≈1,1 KB → heap (`esp_core_dump.h:163-170` örneği).
- Panik işleyici: `esp_core_dump_write` `PRINT_REBOOT`/`SILENT_REBOOT`/`HALT` seçiminden bağımsız çalışır (`esp_system/panic.c:417-427`); `CONFIG_ESP_TASK_WDT_PANIC=y` (ALTE) ile TWDT tetiklemeleri de dump üretir → "donma" vakaları teşhis edilebilir.
- Bölüm tanımı: `data` / `coredump` (alt tür 0x03, `partition-tables.rst:191`); flaş şifrelemesi varsa `encrypted` bayrağı (`core_dump.rst:102`).
- Firmware boyutu: mevcut iki build'de coredump kapalı → map'ten ölçülemedi. Kod hacmi kıyasıyla (`esp_https_ota.c` 997 satır → 7,6 KB) `core_dump_common+elf+flash+crc` ≈ 2 000 satır → **tahmini 12–18 KB flaş**, DRAM: loglar ~5 KB (kapat), isteğe bağlı ayrı yığın 1,8 KB. Açınca `idf.py size-components` ile ölç (yapilacaklar).

### Öneri

- `sdkconfig.defaults`: `ESP_COREDUMP_ENABLE_TO_FLASH=y`, `ESP_COREDUMP_DATA_FORMAT_ELF=y`, `ESP_COREDUMP_CHECKSUM_CRC32=y`, `ESP_COREDUMP_MAX_TASKS_NUM=24`, `ESP_COREDUMP_STACK_SIZE=1792` (yığın taşması çöküşlerinde dump'ın kendisi taşmasın), `ESP_COREDUMP_LOGS=n`, `ESP_COREDUMP_CAPTURE_DRAM=n`, `ESP_COREDUMP_FLASH_NO_OVERWRITE=n` (son çökme kazanır; kullanıcı indirince silinir), `ESP_COREDUMP_CHECK_BOOT=y`.
- Açılış (evlog'dan sonra, `sys_worker`): `image_check()==ESP_OK` → `malloc(summary)`, `get_summary` + `get_panic_reason` → evlog `boot: panic, task=…, pc=0x…, mcause=…, mtval=0x…, elf=<sha ilk 8>, reason="…"`, GUI rozeti "çökme kaydı var". Dump **silinmez**; kullanıcı indirir ya da siler.
- API: `GET /api/coredump` → `image_get(&addr,&size)` → `esp_partition_find_first(DATA, COREDUMP)` → 4 KB parçalarla `esp_partition_read` + `httpd_resp_send_chunk`, `Content-Disposition: attachment; filename="coredump-<ver>-<ts>.bin"`; `DELETE /api/coredump` → `image_erase`. Dosya bölüm imajıdır (4 B boyut + ELF + CRC) → `idf.py coredump-info -c dosya` / `espcoredump.py info_corefile -c dosya -t raw build/lebensspur.elf` (`core_dump.rst:103`).
- Her release'e `.elf`'i de varlık olarak ekle (özetteki `app_elf_sha256` ile eşleştirme); aksi halde dump çözümlenemez.

```c
/* açılışta çökme özeti → evlog */
if (esp_core_dump_image_check() == ESP_OK) {
    esp_core_dump_summary_t *s = malloc(sizeof *s);           /* ≈1,1 KB */
    char why[128] = "";
    if (s && esp_core_dump_get_summary(s) == ESP_OK) {
        esp_core_dump_get_panic_reason(why, sizeof why);
        evlog_boot_panic(s->exc_task, s->exc_pc, s->ex_info.mcause, s->ex_info.mtval,
                         s->app_elf_sha256, why);              /* dump silinmez */
    }
    free(s);
}
```

### Risk

- Orta: dump yazımı panik anında flaş'a 40–70 KB yazar (~0,5–1 s); `BOOTLOADER_WDT` ilgisiz (uygulama içinde), TWDT panik işleyicide beslenir (`panic.c:418`). Flaş şifrelemesi kapalı → dump düz metin (içinde WiFi parolası olabilir: RAM'de). GUI'de uyarı metni; `CAPTURE_DRAM` kapalı tutulması bu riski küçültür (yalnız yığınlar).
- Düşük: `MAX_TASKS_NUM=24` aşılırsa düşük öncelikli görevler atlanır (çöken görev her zaman dahil).

---

## 7. Bölüm tablosu

### Bulgu

- Hizalama kuralları (`IDF/components/partition_table/gen_esp32part.py:106-110, 558-573`): `app` ofseti 0x10000 katı, boyutu 0x1000 katı; `data` ofseti 0x1000 katı; ilk bölüm `PARTITION_TABLE_OFFSET + 0x1000` = 0x9000'den küçük olamaz (`:316-324`); toplam `verify_size_fits` ile flaş boyutuna (`:372`).
- Önerilen tablo kontrolü:

| Ad | Ofset | Boyut | Biter | Hizalama |
|---|---|---|---|---|
| nvs | 0x9000 | 0x10000 (64 KB) | 0x19000 | data ✓ |
| otadata | 0x19000 | 0x2000 | 0x1B000 | data ✓, 2 sektör zorunlu (`partition-tables.rst:163`, `bootloader_utility.c:402`) |
| phy_init | 0x1B000 | 0x1000 | 0x1C000 | data ✓ — **gereksiz** (aşağıda) |
| (boşluk) | 0x1C000 | 0x4000 (16 KB) | 0x20000 | — |
| ota_0 | 0x20000 | 0x1E0000 (1920 KB) | 0x200000 | app ✓ |
| ota_1 | 0x200000 | 0x1E0000 | 0x3E0000 | app ✓ |
| coredump | 0x3E0000 | 0x20000 (128 KB) | **0x400000** | data ✓ = tam 4 MB |

Firmware 1 268 896 B (harita §2.1) → 1 920 KB'de ≈ 697 KB (%35) boş.

- `phy_init`: yalnız `CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION=y` (varsayılan n, `esp_phy/Kconfig:23-33`) ya da `ESP_PHY_MULTIPLE_INIT_DATA_BIN` ile okunur (`phy_init.c:576-592, 1112`); kalibrasyon verisi `ESP_PHY_CALIBRATION_AND_DATA_STORAGE=y` ile NVS `phy` ad alanına yazılır (`phy_init.c:692-738`; ALTE `sdkconfig:1197`). Bölüm bugün de kullanılmıyor.
- NVS 64 KB = 16 sayfa; `PageManager::load` en az bir boş sayfa ister, yoksa `ESP_ERR_NVS_NO_FREE_PAGES` (`nvs_pagemanager.cpp:129-133`); sayfa 126 giriş × 32 B, blob parçası ≤ 4 000 B (`nvs_page.hpp:39-43`) → 2,8 KB `rt` blobu tek parça. Belge asgarisi 0x3000 (`partition-tables.rst:177`).
- **Eski cihazda tablo değişince NVS:** `idf.py flash` NVS alanına dokunmaz; `PageManager::load` 16 sektörü tek tek yükler (`nvs_pagemanager.cpp:26-30`): eski 6 sayfa geçerli (aynı IDF → `NVS_VERSION` aynı, `NEW_VERSION_FOUND` gelmez, `nvs_page.cpp:74-75`), eski otadata/phy sektörleri başlık CRC'si tutmadığından `CORRUPT` (`nvs_page.cpp:69-70, 91-92`), kalanı `UNINITIALIZED`; `CORRUPT`/`UNINITIALIZED` boş listeye girer ve ilk kullanımda silinir (`activatePage`, `:202-213`). Sonuç: `nvs_flash_init` **ESP_OK**, ALTE'nin tüm anahtarları (harita §3 ad alanları) **görünür kalır**; otomatik silme yok. `NO_FREE_PAGES`/`NEW_VERSION_FOUND` → `nvs_flash_erase` akışı bu durumda tetiklenmez. Belge de uyarır: tablo güncellemesi veriyi silmez, `idf.py erase-flash` gerekir (`partition-tables.rst:319`).
- `idf.py flash` hedefi: bootloader + bölüm tablosu + **boş `ota_data_initial.bin`** + uygulama (`app_update/CMakeLists.txt:16-31, 73`; `partition_table/CMakeLists.txt:172-174`). Her `flash` otadata'yı sıfırlar → ota_0 önyüklenir; ota_1'deki eski imaj kalır. `idf.py app-flash` yalnız uygulamayı ota_0'a yazar, otadata'ya dokunmaz → cihaz OTA ile ota_1'e geçmişse **eski imaj açılmaya devam eder** (tezgâhta kafa karıştırır).
- OTA ile bölüm tablosu değişmez (`esp_https_ota` yalnız uygulama bölümünü yazar; bootloader/tablo yazımı ayrı ve riskli özellik). Sahadaki ALTE tablolu cihaz USB ile yeniden yazılmadıkça `nvs 24 KB, coredump yok` ile kalır.

### Öneri

- Tablo (4 MB'a tam oturur, `CONFIG_ESPTOOLPY_FLASHSIZE_4MB`):

```
# Name,     Type, SubType,  Offset,   Size
nvs,        data, nvs,      0x9000,   0x10000
otadata,    data, ota,      0x19000,  0x2000
# 0x1B000-0x20000 (20 KB) ayrılmış: ileride nvs_keys (0x1000, encrypted) + yedek
ota_0,      app,  ota_0,    0x20000,  0x1E0000
ota_1,      app,  ota_1,    0x200000, 0x1E0000
coredump,   data, coredump, 0x3E0000, 0x20000
```

`phy_init` kaldırılır (`ESP_PHY_INIT_DATA_IN_PARTITION=n` kalır). 20 KB boşluk plan §8/14'teki NVS şifrelemesi için `nvs_keys`'e yeter.

- Temiz kurulum: tezgâhta tabloya geçişte **bir kez `idf.py erase-flash`**; firmware tarafında ilk açılışta kendi ad alanında `schema` anahtarı yoksa `nvs_flash_erase` + yeniden init + evlog "nvs_reset: schema yok" (ALTE kalıntılarını ve yarım durumları temizler; bilinçli ve kayıtlı). Sonraki tezgâh adımlarında `idf.py flash` (otadata sıfırlanır), `app-flash` **kullanılmaz** (iş süreci notu).
- Firmware eski tabloya **dayanıklı** olsun: coredump bölümü yoksa `image_check` `NOT_FOUND` → özellik kapalı, GUI rozeti "coredump bölümü yok"; NVS 24 KB ile de çalışır (64 KB varsayımı kodda sabit olmasın).

### Risk

- Orta: `erase-flash` unutulursa ALTE anahtarları görünür; 2611 yalnız kendi ad alanlarını okuyacağından işlevsel sorun yok, ama 24 KB'lik eski veri yer kaplar ve "temiz kurulum" sayılmaz; şema denetimi bunu kapatır.
- Düşük: `otadata` sıfırlanınca `PENDING_VERIFY` bekleyen bir imaj varsa durum kaybolur — tezgâh için önemsiz.

---

## Plana etkisi

1. **§5.6 OTA — engel:** `releases/latest` yalnız depo LebensSpur 2611'e ayrılmışsa kullanılabilir; bugün `smrtkrft/LebensSpur` `DMF_v1.0.6`'yı gösteriyor. Karar: ayrı depo (önerilen) ya da manifest dosyası. Her iki durumda `esp_app_desc.project_name` kapısı zorunlu.
2. **§5.6 OTA — mekanizma:** API yok; `HEAD /releases/latest` → `Location` tag (gövde/kota yok) + indirmede `esp_https_ota_get_img_desc` (1024 B, flaş'a dokunmadan) → `project_name`, `semver_cmp > 0`, (begin sonrası) `HTTP_TRANSPORT_OVER_SSL` kapıları. `buffer_size_tx = 2048` **zorunlu** (ALTE'nin GitHub OTA'sı bugünkü imzalı URL'lerle çalışmaz). `partial_http_download` kapalı, `timeout_ms` 10 s, `max_redirection_count` etkisiz (özel URL'de `Location` sayımı).
3. **§5.6 abort/onay:** "otadata değişmez" ifadesi düzeltilir: begin'de pasif otadata girdisi silinir; abort sonrası geri dönüş seçeneği doğru olarak kaybolur ve her istekte `esp_ota_check_rollback_is_possible()` ile hesaplanır. Onay işi `sys_worker`'da; `PENDING_VERIFY` iken OTA/geri dönüş reddedilir; açılışta `esp_ota_get_last_invalid_partition` ile rollback evlog'a düşer. Anti-rollback kapalı.
4. **§3.2 görevler:** `sys_worker` yığını 8 KB → **12 KB** (TLS + sertifika paketi); OTA sırasında heap ≈ 50 KB ek. `app_core` → `sys_worker` iptal yolu `atomic_bool` (kuyruk değil).
5. **§5.8 bölüm tablosu:** `phy_init` kalkar, 20 KB ayrılmış boşluk (`nvs_keys` için), coredump 128 KB, tam 4 MB. `sdkconfig.defaults`'a coredump seti (ELF, CRC32, `STACK_SIZE=1792`, `LOGS=n`, `MAX_TASKS_NUM=24`). Firmware boyutu +12–18 KB (tahmin; ölçülecek).
6. **§3.4 açılış sırası:** adım 3 (`evlog_init`) sonrasına "coredump özeti → evlog" eklenir; adım 1'e "şema yoksa `nvs_flash_erase`" eklenir. Eski tabloyla (coredump bölümü yok, NVS 24 KB) çalışmak desteklenir.
7. **§6.2 uçlar:** `/api/ota/check`, `/api/ota/start`, `/api/ota/cancel`, `/api/ota/status` (`pending_version`, `rollback_available`, `rollback_version`, `last_failed_version`), `/api/coredump` GET/DELETE.
8. **İş süreci / yapilacaklar:** release'e `.elf` ekle; tabloya geçişte tek seferlik `erase-flash`; tezgâhta `app-flash` değil `flash`; coredump boyutunu `size-components` ile ölç; özel CA (`cert_pem`) desteği ileride; `semver_cmp` + Unity testleri masaüstünde.
