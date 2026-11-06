# 03 — Depolama, kalıcı veri, günlük, OTA

Kaynaklar (tamamı okundu): `ALTE/HW/components/ext_flash/*`, `ALTE/HW/components/sys_log/*`, `ALTE/HW/components/fw_ota/*`, `ALTE/HW/components/gui_ota/*`, `ALTE/HW/partitions.csv`, `ALTE/HW/sdkconfig.defaults`, `ALTE/HW/CMakeLists.txt`, `ALTE/HW/main/main.c`, `ALTE/HW/components/web_server/web_server.c`, `esp_bin/build.sh`, `esp_bin/HW/CMakeLists.txt`, `esp_bin/HW/sdkconfig.esp_bin`, üretilen `esp_bin/HW/sdkconfig`. NVS envanteri için `ALTE/HW` altındaki tüm `.c` dosyaları grep'lendi ve ilgili bölümler okundu (`wifi_manager.c`, `smtp_client.c`, `telegram_client.c`, `api_client.c`, `timer_manager.c`, `output.c`, `system_info.c`). GUI tarafında yalnızca ilgili uçları çağıran `app.js` / `index.html` satırlarına bakıldı.

Satır referansları `dosya:satır` biçiminde; dosya adları aşağıdaki kısaltmalarla:

| Kısaltma | Tam yol |
|---|---|
| `ext_flash.c` / `.h` | `ALTE/HW/components/ext_flash/ext_flash.c` / `include/ext_flash.h` |
| `ext_flash/CMakeLists.txt`, `gzip_gui.py` | `ALTE/HW/components/ext_flash/…` |
| `sys_log.c` / `.h` | `ALTE/HW/components/sys_log/…` |
| `fw_ota.c` / `.h` | `ALTE/HW/components/fw_ota/…` |
| `gui_ota.c` / `.h` | `ALTE/HW/components/gui_ota/…` |
| `web_server.c` | `ALTE/HW/components/web_server/web_server.c` |
| `main.c` | `ALTE/HW/main/main.c` |
| `wifi_manager.c`, `smtp_client.c`, `telegram_client.c`, `api_client.c`, `timer_manager.c`, `output.c`, `system_info.c` | `ALTE/HW/components/<ad>/<ad>.c` |
| `sdkconfig` | üretilen `esp_bin/HW/sdkconfig` (git'te değil) |

Sürüm: ALTE v0.0.70 (commit 77d849f).

---

## 0. Bir bakışta

- **Dahili 4 MB flash tamamen dolu**: NVS 24 KB + otadata 8 KB + phy 4 KB + iki uygulama bölümü 1984 KB. Veri bölümü yok. Firmware 1 268 896 B (≈1239 KB), uygulama bölümünde ≈745 KB boş.
- **GUI dört kopya**: firmware'e gömülü (gzip, ≈113 KB; `ota_0` ve `ota_1`'de ayrı ayrı), açılışta harici flash Slot A'ya kopyalanan `.gz` dosyaları, GUI OTA ile GitHub'dan Slot B'ye inen **düz** dosyalar.
- Harici flash üç LittleFS: `/slot_a` (1 MB), `/slot_b` (1 MB), `/ext` (çip − 2 MB = 16 MB çipte 14 MB). `/ext` altında yalnızca iki şey yazılıyor: `logs/` (sys_log) ve `mail/g{0..9}/` (mail ekleri).
- Harici flash **olmadan da cihaz çalışıyor**: GUI gömülü kopyadan sunuluyor (`web_server.c:661-673`), sys_log yalnızca RTC belleğinde kalıyor. Yeni mimarinin "GUI yalnızca dahili" hedefi bugün zaten yedek yol olarak var.
- **sys_log**: RTC_NOINIT kayıt + `/ext/logs/events.log` (JSONL, 64 KB döner) + `/ext/logs/checkpoint.json` (10 dk). Açılışta önceki kapanış `esp_reset_reason()` + RTC geçerliliğiyle sınıflandırılıyor (power / software / user / other / unknown).
- **fw_ota**: GitHub Releases `latest` → `tag_name` eşitlik karşılaştırması → `esp_https_ota` → yeniden başlatma kullanıcıya bırakılır → yeni imaj 60 s sonra `esp_ota_mark_app_valid_cancel_rollback` (`main.c:38,55-74,500-510`). İmza doğrulaması **yok**; özel URL ile keyfi firmware yüklenebiliyor.
- **gui_ota**: GitHub raw `main` dalından 12 düz dosya → pasif slota → NVS `ls_gui_ota/active_slot` değişir. gzip commit'inden (baf262e) sonra **fiilen bozuk**: Slot B aktifken `.gz` araması Slot A'ya düşüyor ve eski gzip GUI sunuluyor (bkz. §8.1).
- Sürüm numarası **beş** yerde elle tutuluyor (README "üç" diyor): `ALTE/HW/CMakeLists.txt:7`, `ext_flash.c:570`, `ALTE/GUI/config.json`, `ALTE/GUI/index.html:7` ve `:1739` (`?v=` önbellek kırıcı).

---

## 1. Modüller

### 1.1 `ext_flash` — harici SPI flash, bölümler, LittleFS, GUI kopyası

**Amaç.** Harici W25Q çipini SPI2 üzerinden açmak, üç sanal bölüm kaydedip LittleFS olarak bağlamak, gömülü GUI dosyalarını Slot A'ya kopyalamak, aktif GUI slotunu NVS'de tutmak, dosya okuma yardımcıları sunmak.

**Donanım/bus** (`ext_flash.c:32-37, 112-147`): SPI2_HOST; CS=GPIO21 (D3), MISO=GPIO0 (D0), MOSI=GPIO22 (D4), SCLK=GPIO16 (D6); `max_transfer_sz` 64 KB, `SPI_DMA_CH_AUTO`; cihaz `SPI_FLASH_DIO`, 40 MHz, `cs_id` 0. Çip boyutu çalışma anında `esp_flash_get_size` ile okunur (`:150-163`); 3 MB'den küçükse `ESP_ERR_INVALID_SIZE` (`MIN_CHIP_SIZE`, `:55`; yorum "en az 5 MB" diyor, makro 3 MB — bayat yorum).

**Dış arayüz** (`ext_flash.h`):

| Fonksiyon | Sözleşme |
|---|---|
| `esp_err_t ext_flash_init(void)` | SPI + 3 bölüm + 3 mount + NVS aktif slot. Slot A **ve** user_data ikisi de bağlanamazsa `ESP_FAIL`; biri bağlıysa `ESP_OK` (`:262-264`). SPI/çip hatasında erken döner, hiçbir şey bağlanmaz. |
| `long ext_flash_slot_a_get_file_size(path)` | `/slot_a` altında `stat`; yoksa −1 (`:276-290`). |
| `ext_flash_slot_a_fopen/fread/fclose` | Tek `fopen` ile parça parça okuma; `ext_flash_file_t` = `{FILE*, long size}` heap'te (`:296-342`). |
| `ext_flash_active_fopen/fread/fclose/get_file_size` | Aktif slottan okur; **dosya aktif slotta yoksa Slot A'ya düşer** (`:400-404`, `:446-449`). |
| `char ext_flash_get_active_slot(void)` / `esp_err_t ext_flash_set_active_slot(char)` | `'a'`/`'b'`; set → NVS `ls_gui_ota/active_slot` yazar ve commit eder (`:353-371`). |
| `ext_flash_get_active_mount()` / `get_inactive_mount()` | `"/slot_a"` / `"/slot_b"` (`:373-381`). |
| `ext_flash_is_mounted()` | slot_a **veya** user_data bağlı (`:457-460`). |
| `ext_flash_get_chip_size()` | byte; 0 = çip yanıt vermedi. |
| `ext_flash_is_slot_a/b/user_data_mounted()` | ayrı bayraklar. |
| `ext_flash_get_info/get_slot_a_info/get_slot_b_info(total, free)` | `esp_littlefs_info` (`:482-534`). |
| `esp_err_t ext_flash_format_user_data(void)` | **Format değil**: `/ext` içeriğini özyinelemeli siler (`:684-695`). |
| `esp_err_t ext_flash_create_default_files(void)` | Sürüm uyuşmazsa Slot A'yı temizleyip gömülü GUI'yi yazar (`:697-742`). |

**Davranış — `ext_flash_init` adımları** (`:106-270`):

1. SPI bus → flash cihazı → `esp_flash_init` → boyut oku, 3 MB altı reddet.
2. `esp_partition_register_external` ×3: `slot_a` 0x000000/1 MB, `slot_b` 0x100000/1 MB, `user_data` 0x200000/(çip−2 MB); tip DATA, alt tip ANY (`:177-205`). Kayıt hatası loglanır, devam edilir.
3. Her bölüm için `esp_vfs_littlefs_register` ile `format_if_mount_failed = true` (`:213, 231, 249`) — mount edilemeyen/bozuk bölüm **sessizce biçimlendirilir**.
4. Her başarılı mount'tan sonra `verify_fs_size_or_format` (`:71-87`): `esp_littlefs_info` toplamı bölümün yarısından küçükse (eski 2 MB harita kalıntısı) `esp_littlefs_format`. Bilgi alınamazsa dokunulmaz, "bağlı" sayılır (`:74`).
5. `load_active_slot` (`:89-104`): NVS yoksa/okunamazsa `'a'`.

**Davranış — gömülü GUI'nin Slot A'ya kopyalanması** (`:536-742`): derleme anında gzip'lenmiş `index.html.gz`, `style.css.gz`, `app.js.gz`, 6 dil `.gz`, düz `config.json` (EMBED_TXTFILES, `strlen` ile uzunluk `:719`) ve iki PNG firmware'e gömülüdür (`:545-568`; CMake `:41-54`). `needs_update` (`:653-682`) `/slot_a/config.json`'un ilk 255 baytında `WEB_FILES_VERSION` ("0.0.70", `:570`) **alt dizesini** arar; bulamazsa Slot A **tamamen silinir** (`delete_dir_contents`, `:579-608`) ve 12 dosya yazılır; her dosya yazıldıktan sonra geri okunup boyutu doğrulanır (`:610-643`), hata yalnızca `ESP_LOGE`. Sonunda aktif slot `'b'` ise `'a'`'ya **zorlanır** (`:736-739`): her firmware sürüm değişiminde kullanıcının GUI OTA'sı geçersiz olur. `main.c:456`'dan, `sys_log_init`'ten sonra ve web sunucudan önce çağrılır.

**Bağımlılıklar**: `driver`, `spi_flash`, `littlefs` (joltwallet, `idf_component.yml` ^1.20.4, kilit 1.23.0), `vfs`, `nvs_flash`; `sdkconfig:2230 CONFIG_LITTLEFS_MAX_PARTITIONS=3` (tam olarak kullanılan sayı — dördüncü mount olmaz). Derleme: `ext_flash/CMakeLists.txt` GUI dizinini `../../../GUI` olarak çözer (`:2`), `gzip_gui.py`'yi **configure** aşamasında çalıştırır (`execute_process`, `:24-29`), kaynak değişince yeniden configure için `CMAKE_CONFIGURE_DEPENDS` (`:15-18`). `gzip_gui.py`: seviye 9, `mtime=0` (deterministik), hedef kaynaktan yeniyse atlar (`:29`). Gömülü sembol adları dosya **tabanından** üretilir (`_binary_en_json_gz_start`); aynı tabanlı iki dosya gömülemez.

### 1.2 `sys_log` — kalıcı olay günlüğü ve kapanış nedeni

**Amaç** (`sys_log.h:1-18`): cihazın neden yeniden başladığını kesin kaydetmek; güvenlik açısından kritik olayları (tetikleme, mail, giriş…) kalıcı tutmak; tam güç kaybında bile yaklaşık çalışma süresini bilmek.

**Dış arayüz** (`sys_log.h`):

| Fonksiyon | Sözleşme |
|---|---|
| `esp_err_t sys_log_init(void)` | `ext_flash_init` sonrası; user_data bağlı değilse yalnızca RTC analizi. Önceki çalışmayı sınıflandırır, "boot" satırı yazar, ilk checkpoint'i yazar, shutdown handler kaydeder, izleme görevini (stack 4096, öncelik 1) başlatır (`sys_log.c:525-552`). |
| `void sys_log_mark_event(ev, arg)` | Yalnızca RTC (ucuz). Son olay + uptime + arg (`:554-564`). |
| `void sys_log_set_wifi_ctx(mode, ap_clients, rssi, tx_dbm)` | RTC WiFi bağlamı (`:566-576`). |
| `void sys_log_set_restart_reason(reason)` | `esp_restart()` öncesi niyet (`:578-586`). |
| `void sys_log_time_synced(void)` | SNTP sonrası; `boot_epoch = now − uptime`; 1 700 000 000'dan küçük zaman reddedilir (`:588-598`). |
| `void sys_log_write(level, cat, fmt, …)` | Flash'a **hemen** JSONL satırı; tamponlar heap'te (256+320+420 B) (`:600-631`). |
| `char *sys_log_read_tail_json(max_entries)` | Dosyanın son 24 KB'sinden en yeni önce JSON dizisi; `malloc`, çağıran `free` eder (`:633-720`). |
| `esp_err_t sys_log_clear(void)` | `events.log` silinir, ardından "temizlendi" satırı yazılır (`:722-731`). |
| `sys_log_boot_count/short_boots/boot_epoch/last_cause/last_cause_cat/prev_uptime` | Bu açılışın özet alanları (`:733-738`). |

**Davranış** ayrıntısı §5'te. Bağımlılıklar: `ext_flash` (yalnızca `ext_flash_is_user_data_mounted`), `esp_timer`, `esp_system`, `heap`, `freertos`.

### 1.3 `fw_ota` — firmware OTA (GitHub Releases)

**Amaç**: GitHub Releases'tan `lebensspur.bin` indirip `esp_https_ota` ile pasif uygulama bölümüne yazmak; rollback bilgisi sunmak.

**Sabitler** (`fw_ota.c:24-32`): API `https://api.github.com/repos/smrtkrft/LebensSpur/releases/latest`; indirme `https://github.com/smrtkrft/LebensSpur/releases/download/<tag>/lebensspur.bin`; HTTP zaman aşımı 15 s, OTA 120 s; kontrol tamponu 4 KB; görev yığını 12 288 B, öncelik 5.

**Dış arayüz** (`fw_ota.h`): `fw_ota_init`, `fw_ota_check(custom_url|NULL)`, `fw_ota_start()` (yalnızca `UPDATE_AVAILABLE` durumunda), `fw_ota_get_status(*)` (mutex altında kopya), `fw_ota_rollback()`, `fw_ota_is_busy()`, `fw_ota_can_rollback()`, `fw_ota_get_running_label()`, `fw_ota_get_rollback_version()`. Durum makinesi: `IDLE(0) → CHECKING(1) → UPDATE_AVAILABLE(2) | NO_UPDATE(3) | ERROR(6)`; `UPDATE_AVAILABLE → DOWNLOADING(4) → DONE(5) | ERROR(6)`. `fw_ota_status_t`: state, `progress_pct`, `message[128]`, `remote_version[16]`, `current_version[16]`.

**Bağımlılıklar**: `esp_https_ota`, `esp_http_client`, `mbedtls` (sertifika paketi), `app_update`, `esp_partition`, `sys_log`. Akış §6.1'de.

### 1.4 `gui_ota` — GUI OTA (GitHub raw, slot değişimi)

**Amaç**: 12 GUI dosyasını GitHub raw'dan pasif slota indirip aktif slotu değiştirmek.

**Sabitler** (`gui_ota.c:25-47`): varsayılan kaynak `https://raw.githubusercontent.com/smrtkrft/LebensSpur/main/ALTE/GUI/` (**main dalı**, sürüm kapısı yok); parça 4 KB; zaman aşımı 15 s; dosya listesi `index.html, style.css, app.js, config.json, pic/logo.png, pic/darklogo.png, lang/{en,de,es,fr,it,tr}.json`.

**Dış arayüz** (`gui_ota.h`): `gui_ota_init`, `gui_ota_start(params)` (`use_custom_source`, `repo_url[128]`, `branch[32]`), `gui_ota_get_status`, `gui_ota_rollback`, `gui_ota_is_busy`. Durumlar: `IDLE(0) CHECKING(1) DOWNLOADING(2) VERIFYING(3) SWITCHING(4) DONE(5) ERROR(6) NO_UPDATE(7)`; `gui_ota_status_t`: state, `file_index`, `file_count`, `progress_pct`, `message[128]`, `remote_version[16]`, `current_version[16]`. Görev yığını 10 240, öncelik 5 (`:477`).

**Bağımlılıklar**: `ext_flash`, `esp_http_client`, `mbedtls`, `freertos`, `nvs_flash` (doğrudan kullanılmıyor), `sys_log`. Akış §6.2'de.

### 1.5 `main.c` ve `web_server.c`'deki depolama/OTA parçaları

- Açılış sırası (`main.c:426-528`): cihaz adı → `nvs_flash_init` (yer yok / yeni sürüm → **`nvs_flash_erase`**, `:433-439`) → `ext_flash_init` → `sys_log_init` → (harici OK ise) `ext_flash_create_default_files` → timer → output → smtp/telegram/api → `gui_ota_init`/`fw_ota_init` → WiFi AP → web sunucu → OTA onay zamanlayıcısı (60 s) → flash bilgisi logu → `SL_EV_BOOT` + "Sistem hazır" satırı → otomatik WiFi bağlantısı.
- OTA onayı (`main.c:36-74, 500-510`): `esp_timer` 60 s sonra ayrı görevde (`ota_ok`, 4096, öncelik 3) `ESP_OTA_IMG_PENDING_VERIFY` ise `esp_ota_mark_app_valid_cancel_rollback` + sys_log satırı. Zamanlayıcı oluşturulamazsa hemen onaylar.
- Fabrika sıfırlama (buton 5× `main.c:102-112`; web `web_server.c:1215-1232`): `SL_RST_FACTORY` → sys_log satırı → `nvs_flash_erase()` → `ext_flash_format_user_data()` (= `/ext` içeriği silinir: **günlük ve mail ekleri dahil**) → 200/500 ms → `esp_restart`.
- Yeniden başlatma (web `web_server.c:1203-1213`; buton 3× `output.c:126-131`): `SL_RST_USER_WEB` / `SL_RST_USER_BUTTON`.
- Statik dosya sunumu (`web_server.c:554-683`, rota `"/ext/*"` `:2596`, `"/"` `:2542`): beyaz liste (`static_path_allowed`, `:617-626`); istemci gzip kabul ediyorsa önce aktif slotta `<yol>.gz`, sonra `<yol>`, sonra gömülü tablo (`s_embedded_files`, `:89-102`); her sunumda `SL_EV_GUI_SERVE` (`:571, 668`). Önbellek: css/js/png/ico `public, max-age=31536000, immutable`, html/json `no-cache` (`:608-609`).
- API uçları (rota tablosu `:2541-2597`): `/api/logs` GET, `/api/logs/clear` POST; `/api/trigger-mail/upload|files|file-delete`; `/api/gui-ota/info|start|status|rollback`; `/api/fw-ota/info|check|start|status|rollback`; `/api/device/restart`, `/api/device/factory-reset`; `/api/system/info` ve `/api/system/detail` (slot/user_data doluluk, boot sayacı, kapanış nedeni). `/ext/*` ve `/` kimlik doğrulaması istemez; diğerleri parola ayarlıysa oturum ister.

---

## 2. Bölüm haritası

### 2.1 Dahili flash (4 MB) — `partitions.csv`

| Ad | Tip/alt tip | Offset | Boyut | Kullanım |
|---|---|---|---|---|
| (bootloader) | — | 0x0 | — | ESP-IDF; `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (`sdkconfig.defaults:17`), bootloader WDT 9 s (`sdkconfig:504-506`) |
| (partition table) | — | 0x8000 | 4 KB | `CONFIG_PARTITION_TABLE_OFFSET=0x8000`, MD5 açık |
| `nvs` | data/nvs | 0x9000 | 0x6000 (24 KB) | Tüm yapılandırma (§3); şifreleme kapalı (`sdkconfig.defaults:33`) |
| `otadata` | data/ota | 0xF000 | 0x2000 | Aktif uygulama + rollback durumu |
| `phy_init` | data/phy | 0x11000 | 0x1000 | RF kalibrasyonu (`CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=y`, `sdkconfig:1197`) |
| `ota_0` | app/ota_0 | 0x20000 | 0x1F0000 (1984 KB) | Firmware A |
| `ota_1` | app/ota_1 | 0x210000 | 0x1F0000 (1984 KB) | Firmware B; biter: 0x400000 = tam 4 MB |

Boş alan: **yok**. Firmware 1 268 896 B → uygulama bölümünde ≈745 KB (%37) boş. Gömülü GUI payı ≈113 KB (gz: app.js 22 642, index.html 13 874, style.css 9 229, diller 7 184–7 910 ×6 = 46 211; PNG 10 158 + 11 003; config.json 67).

`system_info.c:39-52` "flash kullanımı" olarak harici etiketler dışındaki **bölüm boyutlarını toplar** — gerçek doluluk değil, her zaman ≈4064 KB.

### 2.2 Harici flash (çip boyutu açılışta) — `ext_flash.c:42-55`

| Etiket | Offset | Boyut | Mount | İçerik |
|---|---|---|---|---|
| `slot_a` | 0x000000 | 1 MB | `/slot_a` | Gömülü GUI'nin kopyası (`.gz` + config.json + PNG) |
| `slot_b` | 0x100000 | 1 MB | `/slot_b` | GUI OTA hedefi (düz dosyalar) |
| `user_data` | 0x200000 | çip − 2 MB (16 MB → 14 MB; 32 MB → 30 MB) | `/ext` | `logs/`, `mail/` |

Bölümler `esp_partition_register_external` ile IDF bölüm tablosuna eklenir (DATA/ANY). LittleFS: sayfa 256, okuma/yazma 128, cache 512, lookahead 128, blok döngüsü 512, mtime açık (`sdkconfig:2231-2241`); blok boyutu bileşende 4096'ya sabit (`managed_components/joltwallet__littlefs/src/esp_littlefs.c:43`).

Üç mount'un hepsi `format_if_mount_failed = true`. Çip hiç yoksa `ext_flash_init` SPI aşamasında döner; üç bayrak da `false` kalır, program devam eder.

---

## 3. NVS envanteri

Kurallar: tüm anahtarlar ≤15 karakter (NVS sınırı), namespace'ler `ls_*` + `wifi_cfg`. "Boyut" = koddaki tampon; string için en fazla tampon−1 karakter. "Yazan/okuyan" sütunlarında web sunucu uçları da gösterilir çünkü birçok modülün NVS'ye tek yazma yolu web API'sidir.

| Namespace | Anahtar | Tip | Boyut / sınır | Varsayılan | Yazan | Okuyan | Satır |
|---|---|---|---|---|---|---|---|
| `wifi_cfg` | `p_ssid` | str | ≤32 | "" (kayıt yok = AP'de kal) | `wifi_manager_connect_sta` (`/api/wifi/connect`), `clear_primary` (boş → `nvs_erase_key`) | `load_credentials_from_nvs` | `wifi_manager.c:112-135, 144, 663-667, 777` |
| `wifi_cfg` | `p_pass` | str | ≤64 | "" | aynı | aynı | `:113, 130` |
| `wifi_cfg` | `p_ip`, `p_gw`, `p_mask`, `p_dns` | str | ≤15 | "" (boş = DHCP; gw/mask/dns boşsa türetilir) | aynı | aynı | `:114-117, 131-134` |
| `wifi_cfg` | `b_ssid`, `b_pass`, `b_ip`, `b_gw`, `b_mask`, `b_dns` | str | yukarıdaki gibi | "" | `save_backup` (`/api/wifi/backup/save`), `clear_backup` | `load_cred(b_)` | `:145, 721, 791` |
| `wifi_cfg` | `ap_enabled` | u8 | 0/1 | 1 | `set_ap_enabled` (`/api/wifi/ap/set`) | `wifi_manager_init` | `:762, 559` |
| `wifi_cfg` | `tx_max` | u8 | 8…20 dBm (aralık dışı yok sayılır) | 17 | `set_tx_power` (`/api/wifi/txpower`) | `init` | `:898, 562` |
| `wifi_cfg` | `tx_adapt` | u8 | 0/1 | 1 | aynı | aynı | `:899, 566` |
| `wifi_cfg` | `ssid`, `password`, `static_ip`, `backup_ssid`, `backup_pass`, `backup_ip` (**eski**) | str | — | — | bir kez `p_*`/`b_*`'ye taşınır ve silinir | `load_credentials_from_nvs` | `:147-166` |
| `ls_timer` | `t_enabled` | u8 | 0/1 | 0 | `save_to_nvs` | `load_from_nvs` | `timer_manager.c:93, 121` |
| `ls_timer` | `t_remain` | i32 | saniye; ≤0 → pasif | 0 | aynı (**sayım sürerken her 60 s**, `:189-191, 233-235`; ayrıca set/cancel/restart/vacation/alarm/expiry) | aynı | `:94, 122` |
| `ls_timer` | `t_total` | i32 | 60…5 184 000 s (1 dk–60 gün) | 0 | aynı | aynı | `:95, 123` |
| `ls_timer` | `t_alarms` | u8 | 0…10 (>10 → 3; set'te ≤ değer/2'ye kırpılır) | 3 | aynı | aynı | `:96, 124, 133, 349-354` |
| `ls_timer` | `v_enabled`, `v_remain`, `v_total` | u8, i32, i32 | tatil 1…60 gün | 0 | aynı | aynı | `:97-99, 125-127` |
| `ls_output` | `relay_inv` | u8 | 0/1 | 0 | `output_save_config` (`/api/relay/set`) | `output_load_config` | `output.c:447, 420`; `web_server.c:1273` |
| `ls_output` | `relay_dur` | u16 | 0…3600 s (web denetimi `:1262`) | 0 (süresiz) | aynı | aynı | `output.c:448, 424` |
| `ls_output` | `relay_pulse`, `relay_delay` | u8 | 0…60 s | 0 | aynı | aynı | `:449-450, 428-432` |
| `ls_smtp` | `server` | str | ≤63 | — (dördü birden yoksa "yapılandırılmamış") | `save_to_nvs` (`/api/smtp/save`; api_key boşsa eski korunur) | `load_from_nvs` | `smtp_client.c:69, 44`; `web_server.c:1084-1090` |
| `ls_smtp` | `port` | u16 | 0 → 465 | 465 | aynı | aynı | `:70, 47`; `web_server.c:1077` |
| `ls_smtp` | `user`, `api_key` | str | ≤63 | — | aynı | aynı | `:71-72, 51-55` |
| `ls_telegram` | `token` | str | ≤71 | "" (boş gelirse eski korunur) | `telegram_client_save_config` (`/api/telegram/save`) | `telegram_client_init` | `telegram_client.c:53, 32`; `web_server.c:1876-1881` |
| `ls_telegram` | `chat_id` | str | ≤31 (zorunlu) | "" | aynı | aynı | `:54, 34` |
| `ls_telegram` | `message` | str | ≤255 | "" (boşsa sabit metin, `main.c:189`) | aynı | aynı | `:55, 36` |
| `ls_api` | `wh_url` | str | ≤255; `http://`/`https://` | "" | `api_client_save_config` (`/api/api-config`) | `api_client_init` | `api_client.c:68, 36`; `web_server.c:499` |
| `ls_api` | `wh_method` | str | GET/POST/PUT | "POST" | aynı | aynı | `:69, 37, 47` |
| `ls_api` | `wh_headers` | str | ≤255, JSON nesnesi | "" | aynı | aynı | `:70, 38` |
| `ls_api` | `wh_body` | str | ≤511, şablon | "" | aynı | aynı | `:71, 39` |
| `ls_api` | `mq_url` | str | ≤127; `mqtt://`/`mqtts://` | "" | aynı | aynı | `:72, 40` |
| `ls_api` | `mq_topic`, `mq_user`, `mq_pass`, `mq_payload` | str | ≤127, ≤63, ≤63 (boşsa eski korunur), ≤255 | "" | aynı | aynı | `:73-76, 41-44`; `web_server.c:497` |
| `ls_ew_mail` | `email` | str | ≤63 (CR/LF temizlenir) | "" | `/api/early-mail/save` | `/api/early-mail/get`, `alarm_mail_task` | `web_server.c:1517, 1472`; `main.c:140` |
| `ls_ew_mail` | `subject`, `body` | str | ≤63, ≤255 | "" | aynı | aynı | `web_server.c:1518-1519`; `main.c:141-142` |
| `ls_tg_mail` | `count` | u8 | 0…10 | 0 | `/api/trigger-mail/save` (yeni), `/delete` (kaydırma) | `/get`, `trigger_mail_task` | `web_server.c:1632, 1722, 1549`; `main.c:256` |
| `ls_tg_mail` | `g{i}_name`, `g{i}_subj` | str | ≤63; i = 0…9 | "" | aynı (delete: `g{i+1}` → `g{i}` kopyalanır, son grup `nvs_erase_key`) | aynı | `web_server.c:1640-1642, 1686-1732`; `main.c:264-265` |
| `ls_tg_mail` | `g{i}_body`, `g{i}_rcpt` | str | ≤255; rcpt virgülle ayrılmış | "" | aynı | aynı | `web_server.c:1644-1646`; `main.c:267-271` |
| `ls_actions` | `early_tg`, `early_mail`, `early_api`, `trig_mail`, `trig_api` | u8 | 0/1 | 0 | `/api/actions/save` | `/api/actions/get`, `main.c` görevleri | `web_server.c:1820-1825, 1776-1780`; `main.c:122, 173, 222, 235, 361` |
| `ls_actions` | `trig_relay` | u8 | 0/1 | **1** | aynı | `on_timer_expired` | `web_server.c:1773, 1781`; `main.c:347` |
| `ls_reset` | `api_key` | str | `ls_` + 8 hex (11 kr.) | "" | `/api/reset/generate` | `/api/reset/config` GET, `/api/reset` | `web_server.c:2000, 1933, 2022` |
| `ls_reset` | `enabled` | u8 | 0/1 | 0 (generate → 1) | `/api/reset/config` POST, `/generate` | aynı | `:1976, 2002, 1945, 2020` |
| `ls_auth` | `hash` | str | 64 hex (SHA-256(MAC:parola)) | yok → koruma kapalı | `auth_store_password` (`/api/auth/set`; boş → `nvs_erase_key`) | `auth_load` | `web_server.c:205-208, 180` |
| `ls_auth` | `ttl_min` | u16 | 1…1440 dk | 10 | `auth_store_session_cfg` | `auth_load` | `:223, 184` |
| `ls_auth` | `protect` | u8 | 0/1 | 1 | aynı | aynı | `:224, 186` |
| `ls_auth` | `lock_min` | u8 | 1…60 dk | 15 | aynı | aynı | `:225, 189` |
| `ls_gui_ota` | `active_slot` | str | "a"/"b" | "a" | `ext_flash_set_active_slot` ← `gui_ota_task`, `gui_ota_rollback`, `create_default_files` | `load_active_slot` | `ext_flash.c:362, 96`; `gui_ota.c:424, 513`; `ext_flash.c:738` |
| (IDF) `nvs.net80211` | — | — | — | — | `esp_wifi` (`CONFIG_ESP_WIFI_NVS_ENABLED=y`, `sdkconfig:1357`) | aynı | — |

Toplu silme noktaları: `main.c:108` (buton 5×), `main.c:436` (NVS yer yok/sürüm), `web_server.c:1224` (web fabrika sıfırlama). Hiçbir modül NVS yazma hatasını kullanıcıya iletmez (çoğu `nvs_set_*` dönüşünü kontrol etmez; istisna `telegram_client.c:53-63`, `ext_flash.c:362-363`).

Kaba doluluk: tüm anahtarlar en uzun değerleriyle dolu olsa ≈10 KB veri + giriş başlıkları → 24 KB bölüme (5 kullanılabilir sayfa ≈ 20 KB) sığar; mail grupları (10 × 640 B) en büyük kalem.

---

## 4. Dosya sistemi envanteri (harici flash)

| Yol | İçerik | Biçim | Kim yazar | Kim okur | Döndürme / sınır |
|---|---|---|---|---|---|
| `/slot_a/index.html.gz`, `style.css.gz`, `app.js.gz` | gömülü GUI kopyası | gzip | `ext_flash_create_default_files` (sürüm uyuşmazsa; önce tüm slot silinir) | `web_server` (`serve_static_path` → `ext_flash_active_*`, Slot A'ya düşüş dahil) | 1 MB bölüm; her firmware sürümünde yeniden yazılır |
| `/slot_a/config.json` | `{"device","version","wifi_configured"}` | JSON, 67 B | aynı | `ext_flash.needs_update` (`:653-682`), `gui_ota.read_current_version` (`:305-321`), `web_server` `/api/gui-ota/info` (`:2248-2260`) | — |
| `/slot_a/pic/logo.png`, `darklogo.png` | logolar | PNG | aynı | web_server | — |
| `/slot_a/lang/{en,de,es,fr,it,tr}.json.gz` | çeviriler | gzip | aynı | web_server | — |
| `/slot_b/<12 dosya>` | GitHub raw'dan inen GUI (düz) | html/css/js/json/png | `gui_ota_task` (hedef pasif slot; önce temizlenir) | web_server (aktifse), `/api/gui-ota/info` (`inactive_version`), `gui_ota_rollback` (config.json var mı) | 1 MB; boyut denetimi **yok** |
| `/ext/logs/events.log` | olay günlüğü | JSONL, satır başına 1 kayıt | `sys_log` (`append_line_locked`) | `sys_log_read_tail_json` ← `/api/logs` | 64 KB aşılınca ilk yarı atılır (32 yazımda bir denetim) (`sys_log.c:45, 198-241`) |
| `/ext/logs/events.tmp` | döndürme geçici dosyası | JSONL | `rotate_if_needed_locked` | — | remove+rename arasında güç kesilirse yetim kalır, temizlenmez |
| `/ext/logs/checkpoint.json` | `{"boot","up","epoch","heap_min"}` | JSON ≤160 B | `sys_log` açılışta, 60. saniyede, sonra her 600 s | `read_checkpoint` (yalnızca RTC geçersizken) | üzerine yazılır |
| `/ext/mail/g{0..9}/<dosya>` | tetikleme maili ekleri | ham | `/api/trigger-mail/upload` (`X-Group-Id`, `X-Filename`; ≤2 MB/dosya; `/`, `\`, `..` reddedilir) | `trigger_mail_task` → `smtp_client_send_with_attachments` (**ilk 5 dosya**, `main.c:276-294`); `/api/trigger-mail/files` listeler | dosya sayısı/toplam boyut sınırı yok; grup silinince dizin silinir ve `g{i+1}` → `g{i}` `rename` (`web_server.c:1737-1758`) |

Fabrika sıfırlama `/ext` altındaki **her şeyi** siler (`ext_flash_format_user_data` → `delete_dir_contents("/ext")`), `/slot_a` ve `/slot_b`'ye dokunmaz.

Harici flash yokken: `/ext` yazan uçlar `ext_flash_is_user_data_mounted()` ile reddeder (`web_server.c:2094`), `sys_log` dosyaya yazmaz, `/api/logs` `[]` döner, `/api/logs/clear` hata verir.

---

## 5. sys_log biçimi

### 5.1 RTC kaydı (`sys_log.c:55-74`)

`RTC_NOINIT_ATTR rtc_rec_t s_rtc`: `magic` (0x4C534C47 'LSLG'), `boot_count`, `uptime_s` (izleme görevi her saniye), `boot_epoch` (SNTP sonrası), `short_boots`, `last_event`, `last_event_up_s`, `last_event_arg`, `heap_free`, `heap_min`, `restart_reason`, `wifi_mode`, `ap_clients`, `sta_rssi`, `tx_dbm`, `check` (FNV-1a, `check` hariç tüm alanlar). Her değişiklik `portENTER_CRITICAL` altında ve `rtc_commit` ile sağlama yenilenir. Brownout/panic/WDT/yazılım resetinde korunur; yalnızca gerçek güç kaybında silinir.

Son olay kodları (`sys_log.h:26-46`) ve kim işaretliyor: `BOOT` (`main.c:518` ve açılış analizinde ön değer `sys_log.c:408`), `AP_CLIENT_JOIN/LEAVE` (`wifi_manager.c:437, 455`), `STA_CONNECTING` (`:293`), `STA_GOT_IP` (`:531`, arg=RSSI), `STA_LOST` (`:466`, arg=reason), `GUI_SERVE` (`web_server.c:571, 668`, arg=byte), `MAIL_SEND` (`main.c:153, 307`), `TG_SEND` (`main.c:190`), `WEBHOOK` (`api_client.c:189` arg 0 webhook, `:249` arg 1 MQTT), `OTA_FW` (`fw_ota.c:259`), `OTA_GUI` (`gui_ota.c:358`), `TX_POWER` (`wifi_manager.c:892`), `RELAY` (`output.c:385`), `TIMER_EXPIRED` (`main.c:345`), `BUTTON` (`main.c:82`). `SL_EV_USER_RESTART` hiçbir yerde işaretlenmiyor (ölü değer).

Niyetli yeniden başlatma nedenleri (`sys_log.h:49-57`): `USER_WEB` (`web_server.c:1207`), `USER_BUTTON` (`output.c:128`), `FACTORY` (`main.c:106`, `web_server.c:1219`), `GENERIC` (shutdown handler, neden belirtilmemiş `esp_restart`, `sys_log.c:510-521`). **`OTA_FW` ve `OTA_GUI` hiçbir yerde set edilmiyor** — OTA sonrası yeniden başlatma `/api/device/restart` ile yapıldığı için günlükte "user_restart" görünür.

Yük piki sayılan olaylar (`:160-168`): BOOT, AP_CLIENT_JOIN, GUI_SERVE, STA_CONNECTING, STA_GOT_IP, MAIL_SEND, TG_SEND, WEBHOOK, OTA_FW, OTA_GUI, RELAY.

### 5.2 Kayıt satırları (JSONL)

**Boot kaydı** (`:444-462`, önceki çalışma için, bir kez):

```
{"type":"boot","n":<önceki açılış no>,"ts":<önceki çalışmanın bitiş epoch'u|0>,"up":<önceki uptime s>,
 "approx":true|false,"cat":"power|software|user|other|unknown","cause":"<kod>","msg":"<Türkçe açıklama>",
 "hint":"<yorum>","ev":"<son olay adı>","ev_dt":<olaydan resete geçen s|-1>,"ev_arg":<int>,
 "wifi":"OFF|AP|STA|APSTA|?","cl":<AP istemci>,"rssi":<dBm>,"tx":<dBm>,
 "heap_free":<B>,"heap_min":<B>,"short":<kısa açılış sayısı>}
```

**Olay kaydı** (`:616-620`):

```
{"type":"event","n":<bu açılış no>,"ts":<epoch|0>,"up":<uptime s>,"lvl":"INFO|WARN|ERR","cat":"<kategori>","msg":"<kaçışlı metin>"}
```

Seviyeler ve kategoriler serbest metindir; başlıkta önerilenler: `sys, power, wifi, timer, mail, tg, api, relay, ota, user` (`sys_log.h:89-90`). Aynı `cat` anahtarı boot kaydında **farklı sözlük** (power/software/…) taşır; GUI `type` ile ayırır. `ts` SNTP senkronundan önce 0'dır, geriye doldurulmaz. `msg` kaçışı yalnızca `"` `\` ve kontrol karakterleri (`:170-186`); `msg`/`hint` boot kaydında sabit, tırnaksız Türkçe (ASCII) metinlerdir. Kayıt uzunluğu: olay ≤420 B, boot ≤640 B.

### 5.3 Kapanış nedeni algoritması (`:284-368, 370-467`)

Girdi: `esp_reset_reason()`, RTC geçerli mi (`magic` + sağlama), geçersizse checkpoint'ten `boot/up/epoch/heap_min` (yaklaşık, `approx:true`).

| `esp_reset_reason` | RTC | `cat` / `cause` | Ek |
|---|---|---|---|
| BROWNOUT | — | power / `brownout` | son olay yük pikiyse ve ≤30 s önceyse hint "akım yetersiz", değilse "besleme kararsız" |
| PWR_GLITCH | — | power / `pwr_glitch` | |
| POWERON | geçersiz | power / `power_loss` | "RTC bellek silindi" |
| POWERON | geçerli | power / `poweron_rst` | EN/reset hattı ya da çok kısa kesinti |
| PANIC | — | software / `panic` | |
| INT_WDT / TASK_WDT / WDT / CPU_LOCKUP | — | software / `int_wdt` `task_wdt` `wdt` `cpu_lockup` | TWDT 10 s + PANIC açık (`sdkconfig.defaults:38-43`) sayesinde donma reset'e dönüşür |
| SW | `restart_reason` | user / `user_restart` `button_restart` `factory_reset` (`ota_fw`, `ota_gui` ölü) | kayıt yoksa software / `sw_restart` |
| DEEPSLEEP, EXT, SDIO, USB, JTAG, EFUSE | — | other / `deepsleep` `ext_pin` `sdio` `usb` `jtag` `efuse` | |
| diğer | — | unknown / `unknown` | |

Kısa çalışma kuralı (`:415-439`): önceki uptime < 5 s ise `short_boots++`, normalde loga yazılmaz; **power/software** kategorisindeyse 1., 10., 20., … kısa açılışta tek toplu satır yazılır (boot döngüsü görünür kalsın). İlk açılış (önceki boot no 0) loga girmez. Yeni RTC kaydı `memset` + `boot_count+1` ile kurulur; `last_event=BOOT`.

### 5.4 İzleme görevi ve checkpoint (`:471-508`)

Her 1 s: `uptime_s`, `heap_free`, `heap_min` RTC'ye. Checkpoint: 60. saniye, sonra her 600 s (`last_ckpt = 60 − 600` hilesi). Sağlık satırı her 12 saat (`INFO/sys`). Shutdown handler `esp_restart` çağrılarında `restart_reason` boşsa `GENERIC` yazar.

### 5.5 GUI'ye sunum

- `GET /api/logs` (`web_server.c:1288-1311`): `{"boot_count","short_boots","boot_epoch","last_cause","last_cat","prev_uptime","logs":[…]}`; `logs` en yeni önce, `sys_log_read_tail_json(150)` — ama dosyanın yalnızca **son 24 KB**'si okunur, pratikte ≈80–150 kayıt. Chunked gönderim; `Cache-Control: no-cache`.
- `POST /api/logs/clear` (`:1313-1322`).
- `GET /api/system/detail` (`:1127-1201`): `boot_count`, `short_boots`, `last_cause`, `last_cat`, `prev_uptime`, ayrıca IDF `reset_reason` metni.
- GUI (`app.js:1467-1500+`): `cat`/`cause`/`hint` çeviri anahtarlarıyla (`cat.*`, `causeLabel`, `causeHint`) gösterilir; `approx` işareti, `up` süresi; `type` ile boot/event ayrımı.

---

## 6. OTA akışları

### 6.1 Firmware OTA (`fw_ota.c`)

Kontrol (`fw_ota_check` `:374-401` → `fw_ota_check_task` `:126-250`):

1. `s_task_handle != NULL` ise reddet. Özel URL varsa sakla; durum sıfırla (mutex dışında).
2. Görevde: `current_version = esp_app_get_description()->version` (= `PROJECT_VER`).
3. Özel URL varsa: sürüm kontrolü **yok**, `remote_version = "custom"`, doğrudan `UPDATE_AVAILABLE` (`:138-149`).
4. Resmi: GitHub API GET (`User-Agent: LebensSpur-ESP32`, `Accept: application/vnd.github+json`, sertifika paketi, 15 s). HTTP ≠ 200 → `ERROR "GitHub API hatasi: HTTP n"` (anonim istek kotası 60/saat → 403 buraya düşer). Yanıtın ilk 4095 baytı okunur, `"tag_name"` naif ayrıştırıcıyla alınır (`:72-94`), `v` öneki atılır.
5. `strcmp(current, remote) == 0` → `NO_UPDATE`; **farklıysa** (daha eski olsa da) → indirme URL'si `…/download/<tag>/lebensspur.bin`, `UPDATE_AVAILABLE`.

İndirme (`fw_ota_start` `:403-431` → `fw_ota_download_task` `:256-339`):

1. Yalnızca `UPDATE_AVAILABLE` ve URL doluysa.
2. `SL_EV_OTA_FW` + sys_log "başlatıldı". `esp_https_ota_begin` (sertifika paketi, 120 s, keep-alive, ≤5 yönlendirme — GitHub indirmesi `objects.githubusercontent.com`'a yönlendirir). `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP` kapalı olsa da bu yalnızca "doğrulama seçeneği yok" durumunu reddeder (`esp_https_ota.c:288-316`, IDF 5.5.2); `crt_bundle_attach` ayarlı olduğundan **`http://` özel URL kabul edilir** ve `esp_http_client` şemaya göre düz TCP kullanır — `web_server.c:2429-2442` de şema denetlemez.
3. `esp_https_ota_perform` döngüsü; ilerleme yüzdesi, her %10'da mesaj.
4. `esp_https_ota_finish`: imaj başlığı/çip doğrulaması, otadata'ya pasif bölüm `NEW` olarak yazılır. Başarı → `DONE "yeniden baslatma gerekli"`, `update_rollback_info()` (bu noktada "diğer bölüm" = **yeni yazılan** imaj; `can_rollback`/`rollback_version` artık yeni sürümü gösterir `:105-120, 320`), sys_log. `ESP_ERR_OTA_VALIDATE_FAILED` → "dogrulama basarisiz".
5. Yeniden başlatma GUI'de ayrı düğme → `/api/device/restart` (`app.js:1935-1945`) → neden `USER_WEB`.

Rollback ve onay:

- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`: yeni imaj ilk açılışta `PENDING_VERIFY`; `main.c:500-510` 60 s sonra `esp_ota_mark_app_valid_cancel_rollback` (`:55-67`) ve "dogrulandi" satırı. 60 s dolmadan **herhangi bir** reset (brownout, kullanıcı yeniden başlatma, WDT) → bootloader imajı `ABORTED` işaretler ve eski imaja döner. Kod hiçbir yerde `esp_ota_mark_app_invalid_rollback_and_reboot` çağırmaz; tek geri dönüş yolu budur + elle rollback.
- `fw_ota_rollback` (`:441-470`): pasif bölümde geçerli `esp_app_desc` varsa `esp_ota_set_boot_partition(other)`; yeniden başlatma yine kullanıcıya bırakılır (`app.js:1947-1975`). Yeni açılış yine `PENDING_VERIFY` → 60 s onay.
- Anti-rollback (eFuse sürüm sayacı) kapalı (`sdkconfig:453`).

Güvenlik: TLS doğrulaması var (`esp_crt_bundle_attach`, yaygın CA paketi `sdkconfig.defaults:70`). **İmza doğrulaması yok** (`CONFIG_SECURE_BOOT` ve `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` kapalı, `sdkconfig:520-521`). Özel URL herhangi bir HTTPS sunucusu olabilir; `/api/fw-ota/check` yalnızca tarayıcı parolası ayarlıysa oturum ister; AP parolasız (`wifi_manager.c:617`).

### 6.2 GUI OTA (`gui_ota.c`)

`gui_ota_start` (`:457-484`) → `gui_ota_task` (`:327-438`):

1. Aktif slot `config.json` → `current_version` (yoksa boş).
2. `CHECKING`: kaynak `config.json` indir (`build_url` `:110-149`: özel kaynakta `github.com/<u>/<r>` → `raw.githubusercontent.com/<u>/<r>/<branch|main>/ALTE/GUI/`; yol sabit). `"version"` ayrıştır. Hata → `ERROR "Uzak sunucuya baglanilamadi"`.
3. Eşitse `NO_UPDATE`; değilse `SL_EV_OTA_GUI` + sys_log.
4. `DOWNLOADING`: **pasif slot tamamen silinir** (`:366`), `pic/` ve `lang/` oluşturulur, 12 dosya sırayla indirilir (`download_file_to_path` `:179-246`: 4 KB parça, `fwrite` dönüşü denetlenmez, 0 bayt → hata; aralarda 50 ms). Herhangi biri başarısız → `ERROR`, slot **yarım kalır**.
5. `VERIFYING`: yalnızca `config.json` ve `index.html` var ve ≥10 B mi (`:404-419`).
6. `SWITCHING`: `ext_flash_set_active_slot(diğer)` → NVS. `DONE`; GUI 2 s sonra `location.reload()` (`app.js:2020-2026`). Cihaz yeniden başlatılmaz.

`gui_ota_rollback` (`:494-519`): diğer slotta `config.json` varsa aktif slot değiştirilir; GUI 1 s sonra yeniler.

Güvenlik: TLS var; içerik doğrulaması/imza yok; özel kaynak keyfi HTML/JS'yi cihazın origin'inde çalıştırır (oturum çerezi dahil).

### 6.3 İki OTA'nın ortak / tekrar eden kodu

| Parça | fw_ota | gui_ota | Başka |
|---|---|---|---|
| Durum yapısı + mutex + `set_status`/`set_progress` + `s_task_handle` deseni | `:34-69` | `:49-79` | — |
| Naif JSON string ayrıştırıcı (`"key":"value"`) | `parse_json_str` `:72-94` | `parse_json_value` `:82-107` | `web_server` cJSON (`ls_util`), `ext_flash.needs_update` `strstr` |
| config.json'dan sürüm okuma | — | `read_current_version` `:305-321` | `ext_flash.c:653-682`, `web_server.c:2248-2275` (iki kopya) |
| Özyinelemeli dizin silme | — | `delete_dir_contents` `:152-176` | `ext_flash.c:579-608` (aynı kod) |
| GitHub özel kaynak alanı (`source:"custom"`) | `web_server.c:2429-2442` | `:2305-2319` | — |
| "busy" denetimi görev tutamacıyla, durum sıfırlama mutex dışında | `:376-392` | `:459-476` | — |

---

## 7. Derleme ve sürüm

**Akış** (`esp_bin/build.sh`): `ROOT`, `PROJ=esp_bin/HW`, `IDF_DIR` (varsayılan `~/esp/esp-idf-v5.5.2`) (`:14-16`); kurulu `idf5.5_py*_env` seçilir (`:25-32`); `export.sh` (`:35`); `cd esp_bin/HW && idf.py <args|build>` (`:37-41`); `build` içeriyorsa ve `build/lebensspur.bin` varsa `PROJECT_VER` `ALTE/HW/CMakeLists.txt`'den `sed` ile okunup `esp_bin/firmware/lebensspur_v<VER>.bin` ve `lebensspur_latest.bin` olarak kopyalanır (`:44-52`).

**Sarmalayıcı proje** (`esp_bin/HW/CMakeLists.txt`): `SRC_DIR=../../ALTE/HW` (`:14`); `SDKCONFIG_DEFAULTS = [ALTE/HW/sdkconfig.defaults, esp_bin/HW/sdkconfig.esp_bin]` (`:17-19`); `PROJECT_VER` kaynak CMakeLists'ten regex ile (`:22-24`) ve o dosya `CMAKE_CONFIGURE_DEPENDS` (`:26`); `EXTRA_COMPONENT_DIRS = ALTE/HW/main, ALTE/HW/components` (`:29`); `project(lebensspur)` → `lebensspur.bin` (fw_ota'nın beklediği release varlık adı). `sdkconfig.esp_bin:3` bölüm tablosu yolunu `../../ALTE/HW/partitions.csv` yapar. Üretilen `sdkconfig`, `managed_components/`, `build/` `esp_bin/HW` altında; `dependencies.lock` git'te (idf 5.5.2, littlefs 1.23.0, mdns 1.14.0). `ALTE/HW/CMakeLists.txt` tek başına da derlenebilir (`:4` kendi defaults, `:7` sürüm, `:9-10`).

**gzip adımı**: configure aşamasında `gzip_gui.py <GUI_DIR> <build>/esp-idf/ext_flash/gui_gz <9 dosya>`; çıktılar `EMBED_FILES`, `config.json` `EMBED_TXTFILES` (NUL sonlu), PNG'ler ham (`ext_flash/CMakeLists.txt:12-55`). Son derleme çıktıları: `gui_gz/` toplam ≈92 KB.

**Sürüm numarasının yerleri** (hepsi elle, derleme denetimi yok):

| Yer | Kullanım |
|---|---|
| `ALTE/HW/CMakeLists.txt:7` `PROJECT_VER "0.0.70"` | `esp_app_desc_t.version` → fw_ota karşılaştırması, `/api/system/detail.fw_version`, dosya adı |
| `ext_flash.c:570` `WEB_FILES_VERSION "0.0.70"` | Slot A yenileme kararı |
| `ALTE/GUI/config.json` `"version":"0.0.70"` | Slot A'ya yazılan/karşılaştırılan sürüm; gui_ota uzak sürüm; `/api/gui-ota/info` |
| `ALTE/GUI/index.html:7` `style.css?v=0.0.70`, `:1739` `app.js?v=0.0.70` | Tarayıcı önbellek kırıcı (css/js 1 yıl `immutable`) |

`esp_bin/README.md:29-30` "üç yer" diyor; gerçekte beş.

---

## 8. Gördüğüm sorunlar

Önem sırasına göre; her madde satır numaralı.

1. **GUI OTA fiilen bozuk (gzip sonrası)**. Slot B aktifken `serve_static_path` önce `<yol>.gz` arar (`web_server.c:650-653`); `ext_flash_active_get_file_size` B'de bulamayınca Slot A'ya düşer (`ext_flash.c:446-449`), A'da `.gz` olduğundan >0 döner; ardından `ext_flash_active_fopen` da aynı düşüşle A'nın gzip'ini açar (`:400-404`). Sonuç: GUI OTA "tamamlandı", `/api/gui-ota/info` yeni sürümü gösterir (config.json B'den), ama tarayıcıya **eski Slot A GUI'si** gider. `web_server.c:636-639`'daki yorum düşüşü hesaba katmıyor. baf262e ile ortaya çıktı.
2. **Firmware OTA'da imza doğrulaması yok + özel URL + parolasız AP**. `sdkconfig:520-521` imzalı uygulama kapalı; `fw_ota.c:138-149` özel URL'de sürüm/kaynak/şema denetimi yok — `http://` URL bile kabul edilir ve TLS'siz indirilir (§6.1 adım 2); `/api/fw-ota/check` ve `/start` yalnızca tarayıcı parolası ayarlıysa korunur (`web_server.c:2591-2592`, `:2532`). Parola yoksa AP menzilindeki herkes keyfi firmware yükleyebilir. Aynı şey gui_ota özel kaynak için (`gui_ota.c:112-145`): keyfi JS, cihaz origin'inde.
3. **Sırlar düz metin**: NVS şifrelemesi kapalı (`sdkconfig.defaults:33`), flash şifreleme kapalı (`sdkconfig:522`). WiFi parolası, SMTP uygulama parolası, Telegram token, MQTT parolası, webhook `Authorization` başlığı USB'den okunabilir.
4. **Sessiz veri kaybı yolları**: (a) `nvs_flash_init` yer yok / yeni sürüm → `nvs_flash_erase` (`main.c:433-439`) tüm yapılandırmayı siler, günlüğe düşmez (sys_log henüz yok); (b) LittleFS mount hatası → otomatik format (`ext_flash.c:213, 231, 249`), ekler ve günlük gider, kayıt yok; (c) `verify_fs_size_or_format` (`:71-87`) 16 MB çipte oluşmuş 14 MB `user_data`'yı 32 MB çipe taşınınca (beklenen 30 MB, yarısı 15 MB > 14 MB) **biçimlendirir**.
5. **Sürüm karşılaştırması eşitlik**: `fw_ota.c:233` ve `gui_ota.c:351` yalnızca `strcmp`; daha eski bir release "güncelleme" sayılır (düşürme), ön-sürüm etiketleri de. `ext_flash.c:674` `strstr` alt dize: "0.0.7" beklenirken dosyada "0.0.70" varsa eşleşmiş sayılır; ilk 255 bayt okunur.
6. **GUI OTA doğrulaması yetersiz**: `fwrite` dönüşü denetlenmiyor (`gui_ota.c:228`), slot dolarsa kesik dosya; doğrulama yalnızca iki dosyanın ≥10 B olması (`:404-419`); indirme başarısızsa pasif slot yarım kalır ve `config.json` inmişse rollback düğmesi yarım slota geçiş sunar (`web_server.c:2278, 2290`; `gui_ota.c:503-511` yalnız config.json'a bakar).
7. **OTA sonrası 60 s penceresi**: `main.c:38` — yeni firmware'in ilk 60 saniyesinde kullanıcı yeniden başlatma, brownout ya da WDT olursa bootloader eski sürüme döner; kullanıcıya bu açıklanmıyor; günlükte "user_restart" + sonra eski sürüm görünür. `SL_RST_OTA_FW/OTA_GUI` hiç set edilmediğinden (`sys_log.c:350-351` ölü) OTA kaynaklı yeniden başlatma günlükte ayırt edilemez.
8. **Tarayıcı önbelleği**: css/js/png `max-age=31536000, immutable` (`web_server.c:608-609`); tek kırıcı `index.html`'deki elle yazılan `?v=0.0.70` (`index.html:7, 1739`). Sürüm artırımında bu iki satır unutulursa kullanıcı bir yıl eski `app.js` görür. Beş yerde elle sürüm (§7).
9. **`fw_ota` rollback bilgisi yanıltıcı**: `DONE` sonrası `update_rollback_info` (`fw_ota.c:320`) "diğer bölüm"ü yani az önce yazılan yeni imajı okur; `/api/fw-ota/info` yeniden başlatmadan önce `can_rollback=true, rollback_version=<yeni>` döner.
10. **Esp_timer görevinde flash yazımı**: `timer_manager.c:182, 191, 224, 235` `save_to_nvs` ve `main.c:342-364` `on_timer_expired` (sys_log dosya yazımı, röle, görev oluşturma) `esp_timer` geri çağrısında çalışır; `main.c:72` aynı sebeple OTA onayını ayrı göreve taşımış — tutarsız. Diğer `esp_timer` geri çağrıları (TX güç değerlendirme, STA yeniden deneme, SSE tetiği) gecikir.
11. **NVS yazma sıklığı**: sayım sürerken her 60 s `t_remain` (`timer_manager.c:24, 233`); ayda ≈43 000 giriş yazımı, 24 KB bölümde sayfa silme ≈10 saatte bir → 100 k döngüyle onlarca yıl; **aşınma pratik sorun değil**, ama sayaç kalıcılığı 60 s çözünürlüklü (çökme/kesintide ≤60 s kayıp; brownout döngüsünde her açılış yeni yazım).
12. **Günlük büyümesi ve okuma**: 64 KB üst sınır 32 yazımda bir denetlenir → ≈77 KB'ye kadar çıkabilir (`sys_log.c:238`); döndürme `remove` + `rename` (`:221-222`) — arada kesinti `events.tmp` yetimi bırakır, `sys_log_init` temizlemez; `/api/logs` ≈50 KB geçici heap (24 KB ham + satır dizisi + ≈24 KB çıktı, `:660-703`); `max_entries=150` hedefi 24 KB sınırı yüzünden çoğu zaman tutmaz.
13. **Mail ekleri**: yükleme sayısı sınırsız, gönderimde yalnızca ilk 5 (`main.c:276, 285`) — fazlası sessizce atlanır; `files` yanıtı 2048 B'ye sığmayınca JSON bozulur (`web_server.c:2168-2197`); grup silmede `rmdir` başarısızsa (alt dizin) `rename` zinciri kayar (`:1740-1758`); ek gönderimi `fopen` hatasında eki atlar (`smtp_client.c:682-686`) ama mail "gönderildi" sayılır.
14. **Yarışlar (küçük)**: `fw_ota_check`/`gui_ota_start` durum `memset`'i mutex dışında (`fw_ota.c:389`, `gui_ota.c:472`); aktif slot B iken GUI OTA Slot A'yı silerken web sunucu düşüşle A'dan okuyor olabilir; `s_task_handle` denetimi atomik değil.
15. **Sihirli sayılar / bayat yorumlar**: `MIN_CHIP_SIZE` yorumu "5 MB" makro 3 MB (`ext_flash.c:55`); `main.c:3` "Harici 32MB", bench 16 MB; `/api/system/detail` `slot_b_kb:1024` sabit (`web_server.c:1160`); `system_info.c:39-52` "kullanılan flash" = bölüm toplamı; `active_slot` NVS'de string (`ext_flash.c:361`); `ext_flash_format_user_data` adı format değil silme.
16. **Tekrarlar**: §6.3 tablosu; ayrıca `/api/gui-ota/info` içinde aynı 12 satır iki kez (`web_server.c:2248-2275`).
17. **GitHub bağımlılığı**: gui_ota `main` dalını doğrudan çeker (`gui_ota.c:26`) — her push anında "güncelleme"; fw_ota anonim API kotası 60/saat.
18. **Hata iletimi**: NVS yazma hataları çoğu yerde yutuluyor (`web_server.c:1517-1520, 1640-1648, 1820-1827`; `timer_manager.c:93-100`; `output.c:447-451`); `ext_flash_create_default_files` yazma hataları yalnızca konsolda.

---

## 9. Korunması gereken davranışlar (kullanıcıya görünen, test edilebilir)

Depolama/günlük/OTA alanı için; yeni mimaride GUI slotları kalkınca bile geçerli olanlar.

1. Cihaz harici flash **olmadan** açılır, GUI sunulur, sayaç/ayarlar çalışır; yalnızca günlük dosyası ve ekler devre dışı kalır. (Test: SPI flash sökülü/bozuk → `/` 200 döner, `/api/logs` `[]`.)
2. Tüm yapılandırma yeniden başlatmada ve firmware OTA'sında korunur (NVS). (Test: ayar yap → OTA → ayarlar aynı.)
3. Geri sayım kalıcıdır: güç kesilip gelince kalan süre en fazla 60 s hatayla devam eder; süre dolmuşsa tetiklenir; tatil modu ve alarm sayısı da kalıcı.
4. Erken uyarı sayaçları yeniden başlatmada **tekrar ateşlenmez** (`resync_alarms_fired`, `timer_manager.c:74-82`).
5. Fabrika sıfırlama (web ya da buton 5×): tüm ayarlar, günlük ve mail ekleri silinir; cihaz AP modunda temiz açılır; silme işlemi günlüğe yazılmış olsa da dosya silindiğinden görünmez — bu kabul edilen davranış mı, §11'e not düşüldü.
6. Her açılışta önceki kapanış nedeni sınıflandırılır ve günlükte "boot" kaydı olarak görünür; brownout'ta son olay ve "yük piki" yorumu, tam güç kaybında "RTC silindi / yaklaşık süre"; 5 s altı açılışlar sayılır, boot döngüsü 1./10./20. tekrarda tek satır olarak görünür.
7. Günlükte en az şu olaylar: WiFi bağlandı/koptu/bağlanamadı, AP istemci geldi, AP aç/kapa, TX gücü değişti; sayaç ayarlandı/sıfırlandı/durduruldu/tatil; erken uyarı tetiklendi ve mail/Telegram/webhook/MQTT sonuçları; GERİ SAYIM DOLDU + röle; tetikleme mailleri alıcı başına ve özet; uzaktan sıfırlama API; web girişi başarılı/hatalı/kilit; parola ayarlandı/kaldırıldı; yeniden başlatma/fabrika sıfırlama istekleri; OTA başladı/bitti/hata/rollback/doğrulandı; 12 saatlik sağlık satırı; günlük temizlendi.
8. Günlük `ts` SNTP sonrası gerçek epoch, öncesinde 0 + `up`; GUI her iki durumu da gösterir.
9. Günlük sınırlıdır ve eski yarısı atılarak döner; GUI en yeni kayıtları önce görür; "temizle" düğmesi çalışır ve kendisi bir satır bırakır.
10. `/api/system/detail` ve `/api/logs` başlığı: `boot_count`, `short_boots`, `last_cause`, `last_cat`, `prev_uptime` alanları.
11. Firmware OTA: "Kontrol et" → sürüm karşılaştırması (resmi GitHub Releases, `lebensspur.bin` varlığı) ya da özel URL; ilerleme yüzdesi ve mesaj; "Yeniden başlat" ayrı adım; yeni sürüm 60 s stabil çalışmazsa otomatik eski sürüme dönüş; elle rollback düğmesi (diğer bölümde geçerli imaj varsa) + sürüm bilgisi.
12. Resmi OTA kaynağında (GitHub) TLS doğrulaması (sertifika paketi) yapılır. (Özel URL'de `http://` bugün kabul ediliyor — korunacak davranış değil, §8.2 / §11.3.)
13. Mail ekleri: grup başına dizin, ≤2 MB/dosya, listeleme ve silme; tetiklemede ek olarak gönderilir; grup silinince ekleri silinir, sonraki gruplar kayar.
14. GUI statik dosyaları gzip ile sunulur (`Content-Encoding: gzip`), css/js uzun süre önbelleklenir, html/json önbelleklenmez; `/ext/...` URL öneki GUI'nin sözleşmesidir (`index.html:7, 17, 1739`; `app.js:59, 700, 1598`).
15. Sürüm GUI'de görünür: firmware (`fw_version`) ve — slotlar kalkınca — tek sürüm.

---

## 10. Yeni mimari için notlar

GUI harici flash'tan kalkınca gereksizleşen kod / uç / anahtar:

| Alan | Kaldırılacak |
|---|---|
| `ext_flash.c` | Slot A/B bölüm kaydı ve mount (`:42-50, 177-195, 207-241`), slot dosya API'si (`:272-451`), aktif slot + NVS `ls_gui_ota` (`:63-65, 89-104, 344-381`), gömülü GUI sembolleri ve Slot A kopyası (`:536-742`), `WEB_FILES_VERSION`; `verify_fs_size_or_format` yalnızca tek bölüm için ya da hiç |
| `ext_flash/CMakeLists.txt` | GUI gömme bu bileşenden çıkar; gzip + `EMBED_FILES` web sunucu (ya da ayrı `gui` bileşeni) tarafına taşınır |
| `gui_ota/` | bileşen tamamen |
| `web_server.c` | `/api/gui-ota/*` rotaları ve işleyicileri (`:2237-2384, 2586-2589`); `send_active_slot_file_chunked` ve `serve_static_path`'in slot dalı (`:557-590, 648-659`); gömülü tablo kalır (`:52-103, 661-673`); `/api/system/info` ve `/detail`'deki `slot_a_*`, `slot_b_kb` (`:689-694, 709-710, 1133-1137, 1158-1160`) |
| `main.c` | `ext_flash_create_default_files` (`:456`), `log_flash_info` slot satırları (`:408-417`), "slot %c" (`:521-523`), `gui_ota_init` (`:483`) |
| `sys_log.h` | `SL_EV_OTA_GUI`, `SL_RST_OTA_GUI`, `SL_EV_USER_RESTART` (zaten ölü) |
| `system_info.c` | `slot_a/slot_b/user_data` etiket istisnaları (`:44-46`) |
| GUI | gui-ota ekranı (`app.js:1985-2115`), slot göstergeleri (`:372, 1374-1378`), `config.json` sürüm alanının firmware'le eşleştirilmesi; `index.html` `?v=` kırıcısı yerine derleme anında üretilen bir değer |
| sdkconfig | `CONFIG_LITTLEFS_MAX_PARTITIONS` 3 → 1 (ya da yedek bölümü varsa 2) |

Harici flash'ta yalnızca kullanıcı verisi kalınca basitleşenler:

- Tek `esp_partition_register_external` + tek mount (`/ext`), tek "bağlı mı" bayrağı; `ext_flash_is_mounted` anlam karmaşası (`:457-460`) biter.
- Fabrika sıfırlama: özyinelemeli silme yerine `esp_littlefs_format("user_data")` (atomik, hızlı).
- Dosya düşüş mantığı (`active → slot_a → embedded`) yerine yalnızca gömülü GUI; `serve_static_path` ≈20 satıra iner; tüm GUI dosyaları zaten gzip gömülü (`s_embedded_files`).
- Sürüm tek yerde (`PROJECT_VER`) → `esp_app_get_description()->version` hem GUI'ye hem önbellek kırıcıya kaynak olabilir.
- `delete_dir_contents` ve naif JSON ayrıştırıcıların kopyaları kalkar (gui_ota ile).
- GitHub raw bağımlılığı kalkar; tek OTA kanalı, tek sürüm.

Dikkat edilecekler:

- Dahili flash'ta veri bölümü **yok**; GUI gömülü kalacaksa bölüm tablosu değişmeyebilir. Ayrı bir dahili veri bölümü (ör. yapılandırma yedeği, GUI dosyaları) isteniyorsa iki uygulama bölümünden kırpmak gerekir (bugünkü firmware 1239 KB; 2 × 1536 KB → 896 KB kalır — rakam, karar değil).
- GUI firmware'e gömülü kalınca her GUI değişikliği bir firmware sürümü ve ≈1.2 MB OTA demek (eski GUI OTA ≈480 KB düz dosya indiriyordu). Bu kullanıcının bildiği bir sonuçtur.
- `/ext` eklerin yanı sıra "yapılandırma yedeği" taşıyacaksa NVS ↔ dosya senkron kuralı tanımlanmalı (hangisi kaynak, çakışmada ne olur).
- `CONFIG_LITTLEFS_*` ve `format_if_mount_failed` kararı: sessiz format yerine "bağlanamadı, kullanıcı onayıyla biçimlendir" davranışı düşünülebilir (sys_log'a düşsün).
- Harici flash SCLK = GPIO16 = UART0 TX; USB Serial/JTAG konsolu şart (`sdkconfig.defaults:49-54`).

---

## 11. Açık sorular

1. **NVS uyumluluğu**: 2611 firmware'i mevcut cihazlara OTA ile gidecek mi? Evetse §3'teki namespace/anahtar adları ve tipleri aynen korunmalı ya da bir taşıma adımı yazılmalı (wifi_manager'daki eski anahtar taşıma örneği gibi, `wifi_manager.c:147-166`). Hayırsa şema serbest.
2. Harici flash bölümlemesi: tek LittleFS mi, "yapılandırma yedeği" için ayrı küçük bölüm mü? Bölümler sıfır offsetten mi başlasın (eski Slot A/B alanı boşa çıkıyor)?
3. OTA imza doğrulaması (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`) ve/veya özel URL'nin kaldırılması isteniyor mu? Parolasız AP + parolasız web ile birleşince en büyük açık bu.
4. NVS/flash şifrelemesi: cihaz fiziksel erişime açık bir ürün; sırların düz metin kalması kabul mü?
5. Fabrika sıfırlamada günlüğün de silinmesi istenen davranış mı? (Arıza analizi için sıfırlama öncesi günlüğü korumak isteyebilirsiniz.)
6. Günlük üst sınırı 64 KB kalsın mı? 14 MB alan var; daha uzun geçmiş (ör. 1 MB) ve `/api/logs` için sayfalama düşünülebilir.
7. Mail eki sınırı: 5 dosya/grup kuralı yükleme tarafında da uygulansın mı, toplam boyut sınırı olsun mu?
8. Sürüm karşılaştırması eşitlik mi kalsın, "yalnızca daha yeni" (semver) mi? Düşürme bilinçli bir özellik mi?
9. OTA sonrası 60 s onay penceresi: kullanıcıya GUI'de "60 s bekleyin" uyarısı ve `SL_RST_OTA_FW` nedeninin set edilmesi isteniyor mu?
10. Tarayıcı önbelleği: `?v=` kırıcısı derleme anında üretilsin mi, yoksa `immutable` politikası gevşetilsin mi?
11. GitHub repo/varlık adı (`smrtkrft/LebensSpur`, `lebensspur.bin`) ve anonim API kotası (60/saat) ile yaşanacak mı; alternatif (örn. `releases/latest/download/...` yönlendirmesi ile API'siz) isteniyor mu?
12. Üretim çipi 16 MB mi, 32 MB mi? `verify_fs_size_or_format` kuralı çip değişiminde veriyi silebiliyor (§8.4c).
