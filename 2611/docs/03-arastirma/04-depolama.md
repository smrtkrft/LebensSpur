# 04 — Depolama araştırması: NVS, LittleFS, harici SPI flash

Tarih: 2026-11-05. Girdi: `02-plan-ilk.md` §3.4, §4.2, §5.1, §5.2, §5.3, §8 (soru 5, 6, 14); `01-harita/03-depolama-gunluk-ota.md` §1.1, §2.2, §3, §8; `ALTE/HW/components/ext_flash/ext_flash.c`. Kaynak kodu satır satır okundu; rakamlar koddan ve veri sayfalarından türetildi, ölçüm değil.

| Kısaltma | Tam yol |
|---|---|
| `nvs_api.cpp`, `nvs_storage.cpp`, `nvs_page.cpp`, `nvs_pagemanager.cpp` | `~/esp/esp-idf-v5.5.2/components/nvs_flash/src/…` |
| `nvs_constants.h` | `…/components/nvs_flash/private_include/nvs_constants.h` |
| `nvs.h`, `nvs_flash.h` | `…/components/nvs_flash/include/…` |
| `nvs_flash.rst`, `nvs_encryption.rst` | `…/docs/en/api-reference/storage/…` |
| `nvs/Kconfig`, `sec/Kconfig` | `…/components/nvs_flash/Kconfig`, `…/components/nvs_sec_provider/Kconfig` |
| `esp_littlefs.c`, `littlefs_esp_part.c`, `esp_littlefs.h`, `lfs/Kconfig` | `esp_bin/HW/managed_components/joltwallet__littlefs/{src,include,.}/…` |
| `lfs.c`, `lfs.h`, `README.md`, `DESIGN.md` | `…/joltwallet__littlefs/src/littlefs/…` |
| `esp_flash_api.c`, `chip_generic.c`, `chip_winbond.c`, `chip_drivers.c`, `os_func_app.c` | `…/components/spi_flash/{esp_flash_api,spi_flash_chip_generic,spi_flash_chip_winbond,spi_flash_chip_drivers,spi_flash_os_func_app}.c` |
| `spi_flash_hal.h`, `spi_pins.h`, `soc_caps.h` | `…/components/hal/include/hal/spi_flash_hal.h`, `…/components/soc/esp32c6/include/soc/{spi_pins,soc_caps}.h` |
| `partition.c`, `esp_partition.h` | `…/components/esp_partition/…` |
| `spi_flash/index.rst`, `spi_master.rst` | `…/docs/en/api-reference/peripherals/…` |
| `sdkconfig` | üretilen `esp_bin/HW/sdkconfig` (ALTE) |

---

## 0. Bir bakışta

1. **NVS blob güç kaybına karşı atomiktir** (eski ya da yeni, asla karışık): yeni sürüm tüm parçaları + dizin girişiyle yazılır, eski dizin ancak ondan sonra silinir; yarım kalan yazım açılışta temizlenir. `nvs_commit` no-op; `nvs_set_blob` döndüğünde veri flash'tadır.
2. **64 KB NVS yeterli**: planlanan blob'lar ≈1 100 giriş ≈ 8,8 sayfa; kapasite 15 sayfa (1 sayfa yedek). Tek grup blob'u (≈2,8 KB) ≈ 90 giriş, ≈10 ms, sayfa boşaltma denk gelirse 60–100 ms (en kötü ≈0,5 s, önbellek kapalı).
3. **`rt` blob 32 B tutulursa** 60 s'de bir yazım yılda ≈12,5 k sayfa silmesi → sektör başına ≈2 k → 100 k dayanımla onlarca yıl; aşınma sorun değil. Değişmemiş blob zaten yazılmaz (`cmpMultiPageBlob`).
4. **LittleFS 2.11 (joltwallet 1.23.0)**: `rename` tek metadata commit'i ile atomik; dosya içeriği `fsync`/`close` olmadan kalıcı değil; `lfs_format` yalnızca 2 bloğu siler → kademe 2 için gizli veri silme ayrıca `esp_partition_erase_range` ister (16 MB ≈ 40 s tipik, 64 KB blok silme ile).
5. **Harici flash sürücüsü**: ALTE `sdkconfig`'te Winbond sürücüsü kapalı; W25Q128 jenerik sürücüyle çalışır ama **W25Q256 (32 MB) 16 MB'a kırpılır** — `CONFIG_SPI_FLASH_SUPPORT_WINBOND_CHIP=y` şart. GPSPI flash yolu DMA kullanmaz (64 B işlem) → `SPI_DMA_DISABLED` yeterli. SPI2 işlemleri önbelleği kapatmaz: harici flash yazımı CPU'yu durdurmaz. Flash şifrelemesi/mmap harici çipe uygulanmaz (`index.rst:23,31`).
6. **`/data` + `/log` iki bölüm** öneriyorum: günlük bölümü bozulursa kendi kendine biçimlenir ve kademe-2'de hayatta kalır (ilke 11: her biçimleme kayıtlı); `/data` asla sessiz biçimlenmez.
7. **Yedek dosyası JSONL (bölüm başına satır)** olsun: yazma/okuma ≤8 KB tamponla akar; 40 KB cJSON ağacı (≈100 KB heap tepe) hiç kurulmaz. `GET export` aynı akışı HTTP'ye yazar; `import` GUI tarafında bölüm bölüm `PUT`.

---

## 1. NVS blob

### Bulgu

**Sayfa/giriş düzeni.** Sayfa = 4 096 B flash sektörü; 32 B başlık + 32 B durum bit haritası + 126 × 32 B giriş (`nvs_constants.h:21-24`; `nvs_flash.rst` "Structure of a Page"). Değişken uzunluklu öğe = 1 başlık girişi + ⌈n/32⌉ veri girişi (`nvs_page.cpp:186-192`). Tek parça en çok `CHUNK_MAX_SIZE = 32 × 125 = 4 000 B` (`nvs_constants.h:27`).

**Blob parçalama (v2 biçimi).** `nvs_set_blob` → `Storage::writeItem(BLOB)` → `writeMultiPageBlob` (`nvs_storage.cpp:273-376`):
- Blob, aktif sayfanın kalan yerine (`getVarDataTailroom`) sığdığı kadar `BLOB_DATA` parçalarına bölünür; her parça kendi başlık girişiyle yazılır (`:321-325`). İlk parça için kalan yer < 400 B ise sayfa `FULL` işaretlenip yeni sayfaya geçilir (`:296-316`).
- Tüm parçalar yazıldıktan **sonra** 1 girişlik `BLOB_IDX` (`dataSize, chunkCount, chunkStart`) yazılır (`:355-361`).
- Üst sınır: `(sayfa sayısı − 1) × 4 000 B` → 64 KB bölümde tek blob ≤ 60 000 B (`:282-290`); belgedeki "508 000 B ya da bölümün %97,6'sı − 4 000" ifadesi aynı kuraldır (`nvs_flash.rst` "Keys and Values").
- Giriş maliyeti: `E(blob) = Σ_parça (1 + ⌈boyut_parça/32⌉) + 1`.

**Güncelleme atomikliği.** `writeItem` önce mevcut `BLOB_IDX`'i bulur; içerik aynıysa **hiç yazmaz** (`:466-479`, `cmpMultiPageBlob`). Farklıysa sürüm ofsetini değiştirir (`VER_0 ↔ VER_1`, `:483-488`), yeni blob'u bütünüyle yazar, ardından eski dizini ve parçalarını siler (`:542-549`, `eraseMultiPageBlob :786+`). Güç kaybı senaryoları:
- Yeni parçalar yazıldı, dizin yazılmadı → açılışta `eraseOrphanDataBlobs` yetim parçaları siler (`:228-239`); eski blob geçerli kalır.
- Yeni dizin yazıldı, eski silinmedi → `PageManager::load` son sayfadaki son öğenin kopyasını önceki sayfalarda arayıp siler (`nvs_pagemanager.cpp:58-93`); `eraseMismatchedBlobIndexes` uyumsuz dizinleri ayıklar.
- Sayfa boşaltma sırasında (`FREEING`) kesinti → açılışta kopyalama tamamlanır (`:95-128`; `nvs_flash.rst` "Erasing… completed upon the next power-on").
- Belge: "power off at any point… no loss of data, except for the new key-value pair being written" (`nvs_flash.rst` "Security, Tampering, and Robustness").
- `nvs_commit` no-op (`nvs_api.cpp:411-421`, `nvs_handle_simple.cpp:101-106`); yazım `nvs_set_*` içinde flash'a gider.

**Fiziksel yazım adımları** (`nvs_page.cpp:99-151`): başlık girişi 32 B yazım + bit haritası 4 B yazım; veri girişleri **tek** `esp_partition_write` (`writeEntryData`, tüm parça bir seferde) + bit haritası; artık ≤31 B için bir giriş daha. Silme = bit haritasında durum değişimi (sektör silme yok). Sektör silme yalnızca `requestNewPage`'de: ≥2 boş sayfa varsa yalnız etkinleştirme; tek boş sayfa kalınca en çok silinmiş girişli sayfa seçilir, canlı girişleri yeni sayfaya kopyalanır, sektör silinir (`nvs_pagemanager.cpp:139-200`). Bozuk (`CORRUPT`) sayfa etkinleştirilirken silinir (`:202-217`).

**Okuma.** `nvs_get_blob(h, key, NULL, &len)` boyutu verir; tampon ≥ saklanan boyut olmalı, yoksa `ESP_ERR_NVS_INVALID_LENGTH` (`nvs.h:383`); `readMultiPageBlob` tüm parçaları sırayla tampona kopyalar, kısmi/ofsetli okuma yok (`nvs_storage.cpp:651-700`). Struct büyürse eski (küçük) blob büyük tampona okunur — uzunluk `len` ile ayırt edilir; struct küçülürse boyut sorgusu + geçici tampon gerekir.

**İstatistik.** `nvs_get_stats(NULL, &s)` → `used_entries, free_entries, available_entries (= free − 126), total_entries, namespace_count` (`nvs_pagemanager.cpp:222-245`; `nvs.h:604-618`). Byte = giriş × 32. `nvs_get_used_entry_count(handle)` ad alanı başına.

**RAM/zaman.** Bölüm başına ≈22 KB/MB → 64 KB için ≈1,4 KB + sayfa başına hash listesi 128–640 B; 1 000 anahtar ≈5,5 KB ve ≈0,5 s açılış (`nvs_flash.rst:29-31`). 2611'de ≈30 anahtar → ihmal edilir.

**Maliyet tablosu** (struct boyutları plan §5.1'deki tampon uzunluklarından; `+1` NUL):

| Blob | ≈bayt | giriş | Not |
|---|---|---|---|
| `device` | 66 | 5 | |
| `wifi_p`, `wifi_b` | 162 | 8 + 8 | |
| `wifi_tx`, `timer`, `relay`, `actions`, `remote`, `meta` | ≤32 | 3 × 6 = 18 | başlık + 1 veri + dizin |
| `smtp` | 196 | 9 | |
| `telegram` | 617 | 22 | |
| `webhook` | 1 025 | 35 | |
| `mqtt` | 640 | 22 | |
| `early_mail` | 2 242 | 73 | |
| `grp0..grp9` | 2 756 | 89–90 × 10 = 900 | çoğu zaman 2 parçaya bölünür |
| `auth` | ≈70 | 5 | |
| `rt` | ≤32 | 3 | 33–64 B olursa 4 |
| ad alanı girişi | — | 1 | tek `ls` ad alanı |
| **Toplam** | ≈35 KB | **≈1 105** | **≈8,8 sayfa** |

Kapasite: 16 sayfa − 1 yedek = 15 × 126 = **1 890 giriş (60,5 KB)**; boş ≈785 giriş (6,2 sayfa). Bir grup güncellemesi yeni kopya için 90 boş giriş ister (eski kopya sonra serbest kalır) → rahat sığar. `nvs_get_stats` ile GUI'ye "%58 dolu" gibi gösterilebilir.

**Süre tahmini** (dahili 4 MB flash, SPI1, önbellek kapalı; sayfa programlama tipik 0,4–0,7 ms/256 B, sektör silme tipik 45–60 ms, en kötü 300–400 ms):
- Grup blob'u: karşılaştırma okuması ≈1 ms + 2 784 B tek yazım ≈ 11 × 0,5 ms ≈ 6 ms + başlık/dizin/bit haritası ≈ 1 ms → **≈8–12 ms**; sayfa boşaltma denk gelirse + kopyalama (≤4 KB) + sektör silme → **≈60–100 ms**, en kötü ≈450 ms. `CONFIG_SPI_FLASH_YIELD_DURING_ERASE=y` (`sdkconfig:2054-2056`) silme sırasında 20 ms'de bir diğer görevlere yol verir.
- 500 B blob: 17 giriş, ≈2–3 ms. `rt` (32 B): 3 giriş, ≈1 ms; değişmemişse yalnız okuma.

**`rt` aşınma hesabı** (yalnız `RUNNING` iken 60 s'de bir, 32 B blob = 3 giriş): 525 600 yazım/yıl × 3 = 1,58 M giriş ÷ 126 = **≈12 500 sayfa dolumu/yıl** = o kadar sektör silme. Statik yapılandırma sayfaları (≈9) seçilmez (silinmiş girişi yok); silmeler ≈6 dönen sektöre yayılır → **≈2 100 silme/sektör/yıl** → 100 k dayanımla ≈48 yıl. `rt` tek `u64` olsaydı (1 giriş) ≈4 200 dolum/yıl → ≈140 yıl. ALTE'nin dakikada 1 girişi (`03-depolama…md` §8.11) ile aynı düzen.

**Alternatif: büyük metinler ayrı `nvs_set_str`?** Dize ≤4 000 B ve **tek sayfada bitişik** olmalı (`nvs_flash.rst` "at least one page capable of keeping the whole string in a contiguous row"; `nvs.h:294-296`); 2 049 B gövde = 66 bitişik giriş → parçalanma durumunda `ESP_ERR_NVS_NOT_ENOUGH_SPACE` (bir kez `requestNewPage` denenir, `nvs_storage.cpp:509-526`). Artı: yalnız değişen alan yazılır (alıcı listesi 17 giriş), boş gövde 2 giriş. Eksi: bölüm-içi atomiklik kaybolur (gövde yazıldı, alıcılar yazılmadı → karışık grup), 50+ anahtar, şema tablosu karmaşıklaşır. GUI formu = bölüm = tek `PUT` olduğundan tek blob'un "ya hep ya hiç" garantisi daha değerli.

### Öneri

1. **Bölüm başına blob** kalsın (atomik, şema tablosuyla uyumlu). Blob'un başına `{u16 schema; u16 len}` koy; `cfg_load_section` önce boyutu sorgulayıp (`nvs_get_blob(…, NULL, &len)`) en büyük bölüm boyutunda bir geçici tampona okusun, `schema`/`len`'e göre taşısın; `ESP_FAIL`/CRC hataları "yok" sayılıp varsayılan + günlük.
2. **Gruplar `cfg_t` dışında**: 10 × 2 756 B = 27,6 KB statik RAM yerine `cfg_group_get(i, grp_t*)` ile istek anında NVS'den oku (≈1 ms). `delivery` gönderimde 3 KB heap tampona alır; API `GET /api/groups/<i>` aynı yol. `cfg_t` ≈5 KB'ye iner.
3. **`rt` 32 B'ye sığsın** (`schema u8, state u8, warn_fired u8, flags u8, total_s u32, remaining_s u32, vac_total_s u32, vac_remaining_s u32, trigger_pending u8[5], seq u32` = 27 B). 60 s'de bir yazım `app_core`'da kalabilir: 3 giriş ≈1 ms; sayfa boşaltma denk gelirse ≈100 ms gecikme tek çekirdekte hangi görevin yazdığından bağımsızdır.
4. `nvs_get_stats` sonucunu `GET /api/storage`'a koy: `{nvs:{used,total,avail}}` giriş × 32.
5. Anahtar/ad alanı adları ≤15 karakter (`nvs.h:61-62`); tek ad alanı `ls`.

### Risk

- Yapılandırma dolu + `rt` çöp sayfaları: boşaltma sırasında kopyalama + silme en kötü ≈0,5 s önbellek kapalı; WiFi sürücüsü ve httpd o an bekler. Ürün açısından kabul; ölçümle doğrula (adım 2 denetimi).
- Struct küçültme/alan silme şema sürümü olmadan `INVALID_LENGTH` üretir; `schema` alanı zorunlu.
- Dahili flash dayanımı 100 k varsayımı; sektör başına yıllık ≈2 k silme ile güvenli, ama `rt` 60 s'den sık yazılmamalı.

```c
// cfg/nvs_blob.c — bölüm blob'u: başlık + gövde, şema geçişi
typedef struct { uint16_t schema; uint16_t len; } cfg_hdr_t;

esp_err_t cfg_blob_load(nvs_handle_t h, const char *key, void *dst, size_t dst_len,
                        uint16_t want_schema, cfg_migrate_fn migrate)
{
    size_t have = 0;
    esp_err_t err = nvs_get_blob(h, key, NULL, &have);
    if (err != ESP_OK) return err;                      // NOT_FOUND → varsayılan
    if (have < sizeof(cfg_hdr_t) || have > CFG_BLOB_MAX) return ESP_ERR_INVALID_SIZE;
    uint8_t *tmp = (have <= dst_len) ? dst : malloc(have);  // büyüdüyse yerinde oku
    if (!tmp) return ESP_ERR_NO_MEM;
    err = nvs_get_blob(h, key, tmp, &have);
    if (err == ESP_OK) {
        const cfg_hdr_t *hdr = (const cfg_hdr_t *)tmp;
        if (hdr->schema != want_schema) err = migrate(hdr->schema, tmp, have, dst, dst_len);
        else if (tmp != dst) memcpy(dst, tmp, dst_len);
    }
    if (tmp != dst) free(tmp);
    return err;
}
// Yazma: nvs_set_blob(h, key, &sec, sizeof sec);  nvs_commit no-op ama çağrılır.
```

---

## 2. NVS güvenilirlik

### Bulgu

**`nvs_flash_init` dönüşleri** (`nvs_flash.h:48-78`; kod `nvs_api.cpp:139-161`):

| Dönüş | Neden | Onarım |
|---|---|---|
| `ESP_ERR_NVS_NO_FREE_PAGES` | Tüm 16 sektörde geçerli sayfa başlığı var, boş sayfa yok (`nvs_pagemanager.cpp:131-133`). Normal işletimde olmaz (yazım yolu ≥1 boş sayfa korur); bölüm kesilince ya da yabancı veri (eski düzenden kalan NVS sayfaları) olunca. | `nvs_flash_erase()` + yeniden `init` kaçınılmaz (`nvs.h:42`). |
| `ESP_ERR_NVS_NEW_VERSION_FOUND` | Sayfa başlığındaki sürüm < 0xFE (`nvs_page.cpp:74-75`); 5.5.2 biçimi `NVS_CONST_NVS_VERSION 0xfe` (`nvs_constants.h:33`, "sürüm 2"). Yalnızca gelecekteki bir IDF yazdıysa. | Silme kaçınılmaz. |
| `ESP_ERR_NOT_FOUND` | Bölüm tablosunda `nvs` etiketi yok. | Derleme hatası; sıfırlamayla düzelmez. |
| `ESP_ERR_NO_MEM`, flash hataları | heap/sürücü. | Yeniden deneme; günlük. |

**Bozuk sayfa davranışı** (`nvs_page.cpp:27-98`): başlık `UNINITIALIZED` görünüyorsa tüm 4 KB taranır, tek bir 0xFF olmayan sözcük sayfayı `CORRUPT` yapar; başlık CRC uyuşmazsa `CORRUPT`. `CORRUPT` sayfa boş listeye girer ve ilk etkinleştirmede silinir (`nvs_pagemanager.cpp:202-217`) — içeriği **sessizce** yiter, `init` başarılı döner. Giriş CRC'si bozuk öğeler okunamaz (`ESP_ERR_NVS_NOT_FOUND`/`ESP_FAIL`), sayfa kalır.

**2611 bölüm tablosunda `nvs` 0x9000/0x10000** ALTE'nin `nvs` (24 KB) + `otadata` + `phy_init` + uygulama başlangıcını kapsar (`03-depolama…md` §2.1). Yalnız uygulama flaş edilirse: ALTE sayfaları (sürüm 0xFE) geçerli yüklenir ve ALTE ad alanları okunabilir kalır; öteki sektörler `CORRUPT` → boş liste. `init` başarılı olur; karar 10 ("eski NVS silinir") kodla sağlanmalı.

**`CONFIG_NVS_ASSERT_ERROR_CHECK`** (`nvs/Kconfig:23-27`): varsayılan `n` → iç tutarsızlıklar `ESP_FAIL` döndürür, `abort` yok (`NVS_ASSERT_OR_RETURN`). ALTE'de de kapalı (`sdkconfig:1972`).

**`nvs_flash_erase()`** bölüm açıksa önce kapatır (`nvs_api.cpp:182-195`), 16 sektörü siler (≈0,8–1 s, önbellek kapalı), ardından `nvs_flash_init` yeniden çağrılabilir. `nvs_erase_all(handle)` yalnız ad alanını bit haritasında siler (sektör silme yok, çöp kalır).

**Şifreleme hazırlığı.** `CONFIG_NVS_ENCRYPTION` `SECURE_FLASH_ENC_ENABLED || SOC_HMAC_SUPPORTED`'a bağlı (`nvs/Kconfig:3-11`); C6 HMAC destekler. HMAC şeması (`sec/Kconfig:22-29, 32-44`; `nvs_encryption.rst` "HMAC Peripheral-Based Scheme"): XTS anahtarları eFuse'daki HMAC anahtarından türetilir, **`nvs_keys` bölümü gerekmez**, flash şifrelemesi gerekmez; `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID` 0..5 seçilir (varsayılan −1 = derleme hatası), ilk açılışta anahtar yoksa **eFuse bloğu yakılır** (geri dönüşsüz, 6 bloktan biri). `nvs_flash_init()` bu durumda kendisi `nvs_flash_read_security_cfg_v2` → gerekirse `generate_keys_v2` → `nvs_flash_secure_init` çağırır (`nvs_api.cpp:139-161`); okuma/yazma API'si değişmez (`nvs_flash.h:64-65`). Şifreli bölümde sayfa başlıkları açık, girişler XTS-AES (`nvs_flash.rst` "Page header and entry state bitmap are always written unencrypted"). Şifreleme açıldıktan sonra eski düz NVS'nin girişleri CRC tutmayacağı için okunamaz → yine "meta yok → sil" yoluyla temizlenir.

### Öneri

1. Açılış sırası (plan §3.4 adım 1):
   ```
   err = nvs_flash_init();
   if (err == NO_FREE_PAGES || err == NEW_VERSION_FOUND || err == ESP_FAIL) { nvs_flash_erase(); err = nvs_flash_init(); neden = NVS_RESET_INIT_ERR; }
   if (!cfg_meta_exists()) { nvs_flash_erase(); nvs_flash_init(); neden = NVS_RESET_FRESH; }   // karar 10: ALTE kalıntısı
   evlog_boot_note(neden);   // evlog_init'ten önce RAM halkasına
   ```
   `meta` yokken bölümü silmek ALTE sayfalarının yerini kaplamasını da önler (6 sayfa = 756 giriş kaybı).
2. `CONFIG_NVS_ASSERT_ERROR_CHECK=n` kalsın; her `nvs_*` dönüşü kontrol edilip `ESP_FAIL` "bölüm bozuk" olarak günlüğe düşsün (ALTE'nin yutma alışkanlığı bitsin, §8.18).
3. Fabrika kademe 1 = `nvs_flash_erase()` + `init` + `meta` yaz (çöp bırakmaz; ≈1 s). WiFi sürücüsü NVS kullanmıyor (`ESP_WIFI_NVS_ENABLED=n`), `phy_init` ayrı bölüm.
4. Şifreleme için tasarımda tek kısıt: NVS'e yalnız `nvs_flash_init()`/`nvs_open()` ile girilsin (özel bölüm adı, `nvs_flash_init_partition` yok). İleride `sdkconfig`: `CONFIG_NVS_ENCRYPTION=y`, `CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC=y`, `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=<n>`; kod değişikliği sıfır. Yedek JSON için `cfg_backup` yazıcısına şimdi bir `cipher` kancası (varsayılan düz) bırakılsın (soru 14): aynı HMAC anahtarından `esp_hmac_calculate` ile türetilen AES-GCM anahtarı yedeği yalnız o cihazda açılır kılar.
5. `nvs_partition_gen`/host araçlarıyla okunabilirlik şifreleme sonrası kalkar; USB'den NVS dökümüne bağlı bir teşhis adımı tasarlanmasın.

### Risk

- `CORRUPT` sayfa sessizce silinir: bir bölüm (ör. `smtp`) yoksa "bozuldu" mu "hiç yazılmadı" mı ayırt edilemez → `meta.seq` ile tutarlılık: `meta` var ama bölüm yoksa "kayıp, yedekten geri yükle?" uyarısı.
- HMAC anahtarı eFuse'da yakıldıktan sonra o blok başka amaçla kullanılamaz; Secure Boot/Flash Encryption planı ile blok dağılımı birlikte kararlaştırılmalı (yapilacaklar).
- Şifreleme açıldığı anda tüm NVS yeniden yazılır (eski düz veri okunamaz) → yalnız üretim adımında, yedekle birlikte.

---

## 3. LittleFS

### Bulgu

**Sürümler.** joltwallet `esp_littlefs` 1.23.0 (`idf_component.yml`, commit 75e4cd4; ALTE manifesti `^1.20.4`); gömülü littlefs `LFS_VERSION 0x0002000b` = **v2.11**, disk biçimi 2.1 (`lfs.h:24,31`).

**Güç kaybı.** "All POSIX operations, such as remove and rename, are atomic, even in event of power-loss. Additionally, file updates are not actually committed to the filesystem until sync or close" (`README.md:112-114`). `lfs_rename_` tek `lfs_dir_commit` içinde `DELETE(hedef)+CREATE+MOVE` etiketlerini yazar (`lfs.c:4061-4070`); hedef varsa atomik üzerine yazma. Dosya verisi CoW: `lfs_file_sync` → `lfs_file_flush` + metadata commit (`lfs.c:3434+`); `fsync` olmadan kesintide dosya son commit'e döner. Dosya **içeriği** CRC'lenmez (yalnız metadata commit'leri); ekler/yedek için uygulama CRC'si gerekir.

**Ekleme (append) maliyeti.** Kapalı bir dosyayı açıp sonuna yazınca littlefs **son yarım bloğu yeni bloğa kopyalar** (`lfs_ctz_extend`, `lfs.c:2922-2975`: "just copy out the last block if it is incomplete") → her açma-ekleme-kapama oturumu 1 blok silme (≈45 ms tipik W25Q) + ≤4 KB kopya + metadata commit. Dosya açık tutulup `fsync` yapılırsa aynı blok içinde yerinde programlanır (kopya yok).

**esp_littlefs mount/format akışı** (`esp_littlefs.c`):
- Yapılandırma: `block_size` 4 096 sabit (`:43`), `read/prog 128`, `cache 512`, `lookahead 128`, `block_cycles 512`, `block_count 0 = süperbloktan oku` (`:1364-1368`, `sdkconfig:2231-2237`).
- `esp_vfs_littlefs_register`: önce `esp_littlefs_init` (mount), **başarılıysa** VFS kaydı (`:456-487`); mount hatasında bağlam serbest bırakılır (`:1570-1580`), geride kayıt kalmaz → aynı etiketle `esp_littlefs_format(label)` çağrılıp (`:571-603`: geçici `dont_mount` bağlamı kurar, `format_from_efs`, bağlamı siler) sonra `register` yeniden çağrılabilir.
- `format_if_mount_failed=true` → mount hatasında hemen format (`:1514-1535`), ALTE'nin sessiz biçimlemesi.
- `format_from_efs` (`:226-303`): bağlıysa `lfs_unmount` + **tüm açık fd'leri serbest bırakır** (`esp_littlefs_free_fds`), `lfs_format(block_count = bölüm/4096)`, bağlıysa yeniden mount. Eşzamanlı okuyucunun fd'si geçersiz pointer olur.
- `lfs_format_` (`lfs.c:4399-4460`): yalnız kök dizin çiftini (`lfs_dir_alloc` → 2 blok) siler ve süperbloğu yazar; **diğer blokları silmez** → eski veri fiziksel olarak kalır.
- `grow_on_mount` → `lfs_fs_grow(bölüm/4096)` (`:1544-1556`); yalnız büyütür (`lfs.c:5270-5273`). `block_count=0` ile mount süperbloktaki sayıyı alır; bölümden küçükse hata vermez (`lfs.c:4607-4616`) — ALTE'nin "yarı boyut" vakasının kökü (§8.4c).
- `read_only` → VFS salt okunur; `dont_mount` → yalnız bağlam (format için kullanılıyor) (`esp_littlefs.h:73-76`).
- `esp_littlefs_mounted(label)` = `cache_size > 0` (`:321-327`); `esp_littlefs_info` = `block_size × block_count` ve **`lfs_fs_size` tam ağaç gezisi** (`:305-315`) — 4 MB ek için ≈1 000 blok başı okuma ≈ 30–50 ms; her durum isteğinde çağrılmamalı.
- Kilit: dosya sistemi başına özyinelemeli mutex, her VFS çağrısı `sem_take/sem_give` ile sarılı (`:1588-1608`, `:1972-1986`, `:2245-2254`, `:2409-2444`). `rename` kaynak ya da hedef **açıksa `EBUSY`** (`:2411-2422`).
- `fsync` → `lfs_file_sync` (`:2238-2255`); `fflush` (newlib) flash'a yazmaz, `CONFIG_LITTLEFS_FLUSH_FILE_EVERY_WRITE` kapalı (`lfs/Kconfig`; `sdkconfig:2244`).
- Blok sürücüsü `esp_partition_read/write/erase_range(4096)` (`littlefs_esp_part.c:35-82`).

**Boyut/RAM/süre (16 MB = 4 096 blok).** RAM: `lfs_t` + okuma/program önbellekleri 2 × 512 + lookahead 128 + bağlam ≈ **1,5 KB** boşta, açık dosya başına 512 B önbellek + fd yapısı ≈ 0,6 KB. `lookahead_size` 128 B = 1 024 blok penceresi → 16 MB'de pencere 4 kez tazelenir; her tazeleme tam ağaç gezisi (`DESIGN.md:920-952`, O(n²) uyarısı). 512 B pencere tüm çipi kaplar (32 MB için 1 024 B). Mount: süperblok çiftini okur (≈8 KB, birkaç ms); ilk yazımda `lfs_fs_forceconsistency` tüm dizin çiftlerini gezer (≈15 dizin → ≈100 KB okuma, ≈20–40 ms, bir kez). Dizin başına 2 blok (8 KB) metadata; ≤512 B dosyalar dizin içinde satır içi. Blok sayısı 4 096 littlefs için küçüktür (32 bit `lfs_block_t`). `CONFIG_LITTLEFS_OBJ_NAME_LEN=64` → ek adı ≤63 karakter.

**Tek `/data` mı, günlük ayrı mı?** Atomiklik dosya düzeyinde olduğundan günlük yazımı ekleri **bozamaz**; tek bölüm teknik olarak yeterli. Ayrı bölümün kazandırdıkları: (a) günlük bölümü bozulursa düşük değerli veri olduğu için `format_if_mount_failed`-benzeri otomatik onarım uygulanabilir, `/data` asla sessiz biçimlenmez; (b) kademe-2 `/data` biçimlenirken **günlük hayatta kalır** → "biçimlendi" kaydı kaybolmaz (ilke 11 ile çelişkiyi çözer); (c) günlük taşkını ekleri aç bırakamaz; (d) `/data` tam silinirken (soru 7, ≈40 s) `esp_flash_erase_region` bloklar arasında bus kilidini bıraktığından (`esp_flash_api.c:700-706`) günlük yazımı sürer. Maliyet: ikinci mount ≈1,5 KB RAM, `CONFIG_LITTLEFS_MAX_PARTITIONS=2`, sabit 1 MB.

### Öneri

1. **İki bölüm**: `lslog` 0x000000/1 MB → `/log`; `lsdata` 0x100000/(çip − 1 MB) → `/data`. `/log`: mount hatasında (boş ya da bozuk) biçimle, nedenini RAM halkasına yaz, ilk satır olarak günlüğe düşür. `/data`: `format_if_mount_failed=false`; mount hatasında blok 0–1 (8 KB) `esp_partition_read` ile 0xFF mi bak: boşsa `esp_littlefs_format` + `register` + günlük; doluysa `STORAGE_ERROR`, GUI rozeti, kullanıcı onayıyla biçimleme.
2. **Düzen işareti**: mount sonrası `/data/.ls2611` (ve `/log/.ls2611`) yoksa "yabancı dosya sistemi" → biçimle + günlük. ALTE'nin 0x000000'daki 1 MB `slot_a` süperbloğu böyle temizlenir; `grow_on_mount` ve yarı-boyut kuralı gereksizleşir. İşaret varsa `esp_littlefs_info` toplamı ≠ bölüm boyutu → yalnız uyarı.
3. `sdkconfig.defaults`: `CONFIG_LITTLEFS_MAX_PARTITIONS=2`, `CONFIG_LITTLEFS_LOOKAHEAD_SIZE=512` (32 MB çip de hedefse 1 024), `CONFIG_LITTLEFS_CACHE_SIZE=512`, `CONFIG_LITTLEFS_USE_MTIME=y` kalsın (yedek dosyasında zaman), `CONFIG_LITTLEFS_FLUSH_FILE_EVERY_WRITE=n`, `CONFIG_LITTLEFS_SPIFFS_COMPAT=n`.
4. `esp_littlefs_info` sonucunu `storage` modülü önbelleğe alsın (yazım sonrası ve 5 dk'da bir tazele); `GET /api/storage` önbelleği okur.
5. `storage` durum makinesi: `ABSENT / MOUNTED / ERROR / FORMATTING`; format yalnız `sys_worker`'da ve açık fd yokken (bkz. §5 kilit).

### Risk

- Ayrı `/log` bölümü sabit 1 MB: 256 KB × 2 döndürme için fazlasıyla yeterli; ileride günlük büyürse yeniden bölümleme = biçimleme.
- `esp_littlefs_format` açık fd'leri sessizce boşaltır; eşzamanlı okuyucu varsa kullanım-sonrası-serbest. §5'teki okuma sayacı şart.
- `lfs_fs_size`/lookahead tazelemesi büyük eklerle onlarca ms; `sys_worker` dışında çağrılmasın.

```c
// storage/mount.c — sıkı /data bağlama politikası
static esp_err_t data_mount(const esp_partition_t *p)
{
    esp_vfs_littlefs_conf_t c = { .base_path = "/data", .partition_label = "lsdata",
                                  .format_if_mount_failed = false };
    esp_err_t err = esp_vfs_littlefs_register(&c);
    if (err == ESP_OK) return marker_check_or_format("/data", "lsdata", &c);
    if (region_is_blank(p, 2 * 4096)) {                 // blok 0-1 tamamen 0xFF
        evlog_note("data_fs_blank_format");
        if (esp_littlefs_format("lsdata") != ESP_OK) return ESP_FAIL;
        err = esp_vfs_littlefs_register(&c);
        if (err == ESP_OK) write_marker("/data/.ls2611");
        return err;
    }
    evlog_note("data_fs_mount_failed_keep");             // dolu + bozuk: dokunma
    return ESP_ERR_INVALID_STATE;                         // STORAGE_ERROR
}
static bool region_is_blank(const esp_partition_t *p, size_t n)
{
    uint8_t buf[256];
    for (size_t off = 0; off < n; off += sizeof buf) {
        if (esp_partition_read(p, off, buf, sizeof buf) != ESP_OK) return false;
        for (size_t i = 0; i < sizeof buf; i++) if (buf[i] != 0xFF) return false;
    }
    return true;
}
```

---

## 4. Harici SPI flash

### Bulgu

**Başlatma zinciri** (ALTE `ext_flash.c:112-163`): `spi_bus_initialize(SPI2_HOST, …, SPI_DMA_CH_AUTO)` → `spi_bus_add_flash_device(&chip, {.io_mode=SPI_FLASH_DIO, .freq_mhz=40, .cs_id=0})` (`esp_flash_spi_init.h:17-30`; `input_delay_ns` 0 = bilinmiyor) → `esp_flash_init(chip)` (`esp_flash_api.c:309-376`): kimlik okuma (10 deneme, `:323-327`), sürücü seçimi, boyut, `set_io_mode`. `esp_flash_get_size` → `chip->size` (`:611-625`).

**Çip yokken.** Host `read_id` tümü-1/tümü-0 görünce hata verir (`:458-491`, "Inner function fails if it sees all-ones or all-zeroes"); 10 deneme × birkaç µs → `esp_flash_init` **<1 ms** içinde `ESP_ERR_FLASH_NOT_INITIALISED` döner. MISO gürültülüyse iki ardışık okuma uyuşmaz → aynı hata. Kararlı rastgele kimlik (ör. sürekli çekili hat) jenerik sürücü tarafından kabul edilir ve `1 << (id & 0xFF)` boyutu üretilir (`chip_generic.c:92-114`) → mantıksız boyutlar olası.

**Sürücü listesi ve 32 MB.** C6'da varsayılan açık satıcılar yalnız XMC ve GD (`esp32c6/Kconfig.soc_caps.in`; ALTE `sdkconfig:2069-2072`: Winbond kapalı). Liste sırası ISSI, GD, MXIC, **Winbond (kapalı)**, …, sonda her kimliği kabul eden `esp_flash_chip_generic` (`chip_drivers.c:34-58`). W25Q128 (0xEF4018) jenerik sürücüyle sorunsuz (24 bit adres). **W25Q256 (0xEF4019)**: jenerik sürücü `SPI_FLASH_CHIP_CAP_32MB_SUPPORT` vermez (`chip_generic.c:667-678`, yalnız XMC 0x46) → `esp_flash_init` "Detected flash size > 16 MB, but access beyond 16 MB is not supported" uyarısıyla boyutu **16 MB'a kırpar** (`esp_flash_api.c:354-356`). Winbond sürücüsü 4 baytlık komutlarla (`chip_winbond.c:16-29, 175, 247-282`) 32 MB'ı destekler; `spi_flash_optional_feature.rst:152` 32 bit adreslemenin satıcıya özel olduğunu söyler. `CONFIG_SPI_FLASH_OVERRIDE_CHIP_DRIVER_LIST` (`Kconfig:370-378`) kendi listesini vermek içindir; gereksiz.

**DMA/önbellek/eşzamanlılık.** GPSPI flash host'u işlem başına 64 B veri tamponu kullanır, DMA yok (`spi_flash_hal.h:27-28`; `esp32c6 gpspi_flash_ll.h:53-56` DMA kapalı) → ALTE'nin `SPI_DMA_CH_AUTO` + `max_transfer_sz 64 KB` boşa (3 GDMA çiftinden biri + tanımlayıcılar; `soc_caps.h:176`). SPI2 işlemleri yalnız bus kilidini alır, **önbelleği kapatmaz** (`os_func_app.c:97-108, 300-311`): harici flash okuma/yazma/silme CPU'yu durdurmaz; bekleme `esp_rom_delay_us(1)` ile meşgul döngü ama her 20 ms'de `vTaskDelay(1)` yol verir (`chip_generic.c:424-480`; `os_func_app.c:214-230`; `sdkconfig:2054-2056`) → boşta görevi çalışır, görev WDT (10 s, `sdkconfig:1302-1304`) tetiklenmez. WiFi GPSPI2 kullanmaz; C6'da genel amaçlı tek SPI host SPI2'dir (`soc_caps.h:397`) ve tamamen harici flash'a ayrılır. Kripto donanımı (AES/SHA DMA) ayrı paylaşımlı GDMA çifti kullanır; SPI2 DMA kapalıyken çatışma yok.

**Pinler/hız.** XIAO C6: CS=GPIO21 (D3), MISO=GPIO0 (D0), MOSI=GPIO22 (D4), SCLK=GPIO16 (D6). GPSPI2 IOMUX pinleri MISO 2, HD 4, WP 5, CLK 6, MOSI 7, CS 16 (`spi_pins.h`) → kablolama GPIO matrisinden geçer; matris giriş gecikmesi azami okuma frekansını düşürür, HAL `input_delay_ns` + matris gecikmesinden ek dummy hesaplar (`spi_master.rst:699`; `spi_flash_hal.c:63-77, 125`). 40 MHz DIO ALTE'de çalışıyor; 80 MHz matris üzerinden önerilmez. 4 hatla yalnız tek/dual mod: DIO (adres+veri 2 hat) için çipin WP#/HOLD# modülde yukarı çekili olmalı; QIO/QOUT için IO2/IO3 bağlı değil. GPSPI2 SCLK = GPIO16 = UART0 TX → USB-Serial/JTAG konsolu şart (harita §10).

**Flash şifrelemesi.** "Encrypted flash operations are only supported with the main flash chip… Reading through cache is only supported on the main flash" (`spi_flash/index.rst:23,31`); `esp_partition_register_external` harici çip için `encrypted=false` sabitler (`partition.c:442`), `erase_size` 4 096 (`:441`), `offset+size > chip->size` → `ESP_ERR_INVALID_SIZE` (`:418-420`). Harici flash'taki yedek/ekler ileride flash şifrelemesi açılsa da düz kalır (soru 14).

**Süre (W25Q128JV veri sayfası tipik/azami).** Sayfa programlama 0,4/3 ms; 4 KB sektör silme 45/400 ms; 64 KB blok silme 150/2 000 ms; çip silme 40/200 s. W25Q256JV: sektör 50/400 ms, blok 150/2 000 ms, çip 80/400 s. IDF zaman aşımları: sektör 600 ms, blok 4 100 ms, çip 4 000 ms (`chip_generic.c:64-68`) — `esp_flash_erase_chip` çip-silme komutu **göndermez**, `erase_region(0,size)` yapar (`esp_flash_api.c:629-641`), 64 KB hizalıysa blok silme (`:727-733`) → 4 s çip zaman aşımı devreye girmez.

### Öneri

1. `sdkconfig.defaults`: `CONFIG_SPI_FLASH_SUPPORT_WINBOND_CHIP=y` (W25Q256 için zorunlu; W25Q128'de de doğru sürücü). GD varsayılan kalsın (dahili flash).
2. `spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED)`, `max_transfer_sz=0`; `io_mode=SPI_FLASH_DIO`, `freq_mhz=40`, `input_delay_ns=0`.
3. Çip doğrulama: `esp_flash_init` hatası → `ABSENT` (günlük: `esp_err_to_name`); başarılıysa `chip_id` logla, boyut `[4 MB, 32 MB]` dışındaysa `ABSENT` say ("tanınmayan çip"). Açılış maliyeti <5 ms; akış değişmez (plan §3.4 adım 4).
4. Yokken `spi_bus_remove_flash_device` + `spi_bus_free` gereksiz; pinler başka işe yaramaz. Yeniden deneme uç noktası (`POST /api/storage/retry`) istenirse `storage_init` yeniden çağrılabilir; ilk sürümde yok.
5. Harici flash "yalnız veri" olduğundan `esp_partition_register_external` tip `DATA`, alt tip `ESP_PARTITION_SUBTYPE_DATA_LITTLEFS` (0x83) verilsin; `esp_littlefs` etiketle bulur, alt tip belgeleme amaçlı.

### Risk

- 32 MB çip + Winbond sürücüsü kapalı = sessiz 16 MB; üst 16 MB'a yazım adres sarması yapmaz (sürücü sınırı) ama kullanıcı kapasiteyi yarım görür. Winbond dışı 32 MB çip (GD25Q256: GD sürücüsünde 32 MB desteği var, `chip_gd.c:35`) ayrıca doğrulanmalı.
- Bozuk/gürültülü MISO ile rastgele kimlik → yanlış boyut; boyut aralığı denetimi yeterli savunma.
- 40 MHz matris üzerinden sinyal bütünlüğü kablo uzunluğuna bağlı; üretim kartında kısa izler. Sorun çıkarsa 26 MHz'e inilir (`spi_flash_cal_clock` 80/n böler).

---

## 5. Dosya işlemleri `sys_worker`'da

### Bulgu

- VFS yolu: `open/write/fsync/close/rename` → `vfs_littlefs_*` → `lfs_*`, her çağrı dosya sistemi mutex'i altında (`esp_littlefs.c:1967-1986, 2150-2180, 2238-2255, 2405-2447`). stdio `fflush` yalnız newlib tamponunu boşaltır; kalıcılık için **`fsync(fileno(f))`** ya da doğrudan POSIX `write()+fsync()` gerekir.
- Döndürme: `rename("events.jsonl","events.1.jsonl")` atomik (littlefs tek commit; hedef varsa üzerine yazılır). Kaynak açıksa `EBUSY` (`:2411-2416`) → yazıcı dosyayı kapatmış olmalı; okuyucu açıksa da `EBUSY`.
- Sondan okuma: `lseek(fd, -4096·k, SEEK_END)` + `read` → `lfs_file_seek` CTZ atlamalı listede O(log n) blok; 4 KB okuma 40 MHz DIO + 64 B işlemlerle ≈1–2 ms. 50 kayıt (≈10 KB) ≈ 3 blok ≈ 5 ms.
- Eşzamanlı okuma/yazma: mutex işlem düzeyinde serileştirir; okuyucu yalnız son `fsync`'lenmiş durumu görür (CoW), yarım satır görmez (yazıcı tam satırları yazıp `fsync` ederse). Yazıcının 45 ms'lik blok silmesi okuyucuyu o kadar bekletir; önbellek kapanmadığı için httpd dışında kimse etkilenmez.
- `esp_littlefs_format` bağlı dosya sistemini unmount edip açık fd'leri boşaltır (`:226-241`): eşzamanlı okuyucuyla **güvenli değil**.
- Büyük yükleme (`PUT /api/groups/<i>/files/<ad>`, ≤2 MB): 512 blok × (silme 45 ms + 16 × 0,4 ms program) ≈ **25–30 s** tipik; RAM'e sığmaz → httpd görevinde gövde parçası geldikçe dosyaya yazılmalı (sys_worker'a aktarım kuyruğu karmaşıklık ekler). Bu sürede httpd görevi bloke; SSE `httpd_queue_work` işleri bekler.

### Öneri

1. **Günlük yazımı** (`sys_worker` boşaltma): `open(O_WRONLY|O_APPEND|O_CREAT)` → tam satırlar `write` → `fsync` → `close`. Oturum başına ≈1 blok silme (yarım blok kopyası) + metadata; 100 boşaltma/gün → 1 MB `/log`'da blok başına yılda <1 silme. Dosya açık tutulmaz → döndürme `EBUSY` görmez.
2. **Döndürme**: boyut ≥256 KB ise boşaltmadan önce `rename(events.jsonl → events.1.jsonl)` (eski `.1` üzerine yazılır); `EBUSY` gelirse (okuyucu açık) bu turu atla, sonraki boşaltmada dene; sınır yumuşak.
3. **Okuma** (`GET /api/logs?before&limit`): httpd görevinde doğrudan, **aç-oku-kapat** tek parça: `limit ≤ 100`, en çok 16 KB okuyup belleğe al, dosyayı kapat, sonra yanıtı gönder (fd soket üzerinden açık kalmasın). `before` = bayt ofseti + dosya kimliği (`events.jsonl` `st_mtime` ya da ilk satırdaki `boot` sayacı); döndürme sonrası ofset geçersizse `restart:true` ile yeniden başlat. Okumanın `sys_worker`'a taşınması OTA sırasında günlüğü dakikalarca kapatır; gerekmez.
4. **Depolama kilidi**: `storage_acquire(READ)` sayaç / `storage_acquire(EXCLUSIVE)` (format, unmount, tam silme). API işleyicileri ve `delivery` okuma için sayaç alır; `sys_worker` biçimleme öncesi `FORMATTING` durumuna geçip sayaç sıfırlanana kadar bekler (≤5 s, aksi halde `busy`). Yeni açmalar `FORMATTING`'de `storage_absent` ile reddedilir.
5. **Ek yükleme** httpd işleyicisinde akışla yazılır (plan §5.2 "dosya işleri sys_worker'da" kuralına açık istisna): `config.tmp` kalıbı gibi `<ad>.part` + `rename`; eksik gövde/aşım → `.part` silinir. Yükleme süresi GUI'de ilerleme ile; `max_body` 2 MB; soket `recv` zaman aşımı parça başına.
6. Her dosyada uygulama CRC32: günlük satırında gerekmez (satır bütünlüğü JSON ayrıştırmasıyla), yedek ve ekler için §6.

### Risk

- httpd tek görev: 30 s'lik yükleme sırasında diğer istekler ve SSE snapshot'ları bekler. Kabul edilir; GUI "yükleniyor" durumunda başka istek atmaz (tek kaydetme modeli).
- Okuyucu açıkken döndürme ertelenir; sürekli okuyan bir istemci (2 s'de bir `/api/logs`) döndürmeyi teorik olarak uzun süre erteleyebilir → döndürme boşaltma başında, okuma kısa (ms) → pratikte çakışma nadir.
- `fsync` unutulan her yazım güç kaybında kaybolur; yazıcı yardımcı fonksiyonu tek yerde (`storage_write_file_atomic`).

```c
// storage/file.c — tam satırları ekle, kalıcılığı garanti et (sys_worker)
int storage_append_lines(const char *path, const char *buf, size_t len)
{
    if (storage_acquire(STORAGE_READ, 0) != ESP_OK) return -1;   // FORMATTING ise reddet
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644), rc = -1;
    if (fd >= 0) {
        rc = (write(fd, buf, len) == (ssize_t)len && fsync(fd) == 0) ? 0 : -1;
        close(fd);
    }
    storage_release(STORAGE_READ);
    return rc;
}
// Döndürme: close edilmiş dosyada  rename(cur, old)  → EBUSY ise bir sonraki boşaltmada.
```

---

## 6. Yedek JSON

### Bulgu

- Boyut: 10 grup × (ad+konu+gövde+alıcılar ≈2,8 KB + JSON kaçışları) ≈ 29–32 KB + diğer bölümler ≈4 KB → **≈35–40 KB** (sırlar dahil).
- cJSON ağacı: düğüm ≈40 B + malloc payı, dize kopyaları ayrı → 40 KB belge için ağaç ≈60 KB; `cJSON_Print*` çıktı tamponu +40 KB → **≈100 KB heap tepe**. `cJSON_PrintPreallocated(item, buf, len, fmt)` sabit tampona yazar; "allocate 5 bytes more than you actually need" (`cJSON.h:168-169`); `cJSON_PrintBuffered` tahmini tamponla büyür (`:166`).
- Atomik değişim: `config.tmp` yaz + `fsync` + `close` → `rename(config.tmp, config.json)`; littlefs `rename` atomik ve hedefi üzerine yazar (§3). `config.prev.json` için iki `rename` gerekir; aradaki kesintide `config.json` eksik kalabilir, geri yükleme `prev`'e düşmeli. Dosya verisi CRC'siz (§3) → bit bozulması ayrıştırma hatası olarak görünür ama sessiz değer bozulması (dize içinde) yakalanmaz.

### Öneri

1. **Biçim: JSON Lines, bölüm başına satır** (`/data/config/config.jsonl`):
   ```
   {"t":"meta","schema":1,"seq":42,"ts":1762300000,"ver":"2611.0.1"}
   {"t":"device","d":{...}}
   {"t":"grp0","d":{...}}
   …
   {"t":"end","crc32":"9a3f0c1e","lines":21}
   ```
   `crc32` önceki tüm satırların baytları üzerinden; `lines` satır sayısı. Yazma: bölüm başına cJSON nesnesi (≤3 KB dize) → `cJSON_PrintPreallocated(obj, buf, 8192, false)` → `write` → `cJSON_Delete` → **≈12 KB heap tepe**. Okuma: 8 KB satır tamponu (2 048 B gövde kaçışlarla ≤6 KB), satır başına `cJSON_ParseWithLength`, aynı bölüm doğrulayıcısı (`cfg_validate_section`) + `nvs_set_blob`. Tüm dosya asla RAM'de değil.
2. **Aynı akış üç yerde**: `sys_worker` yedek yazımı; `GET /api/config/export` (chunked HTTP, satır satır); `POST /api/config/restore` (dosyadan). `POST /api/config/import` → GUI dosyayı satır satır okuyup bölüm bölüm `PUT /api/config/<bölüm>` yapar (cihazda toplu ayrıştırma yok); API yüzeyi küçülür.
3. **Geri yükleme doğrulaması**: önce tam geçiş (satır sayısı, `end.crc32`, `meta.schema ≤ desteklenen`, her satır JSON + bölüm adı tanınıyor + doğrulayıcı geçti) — hiçbir şey yazmadan; sonra ikinci geçişte yaz; `meta.seq` korunur, `meta.restored_from_seq` eklenir; evlog `config_restored(seq)`. `seq` kuralı: yedek `seq` < NVS `seq` ise "daha eski yedek" uyarısı, kullanıcı onayı.
4. **Döndürme**: `config.tmp` → (1) `unlink(config.prev.jsonl)`, (2) `rename(config.jsonl → config.prev.jsonl)`, (3) `rename(config.tmp → config.jsonl)`. Geri yükleme sırası: `config.jsonl` → `config.prev.jsonl` → `config.tmp` (yalnız `end` satırı ve CRC geçerliyse). Açılışta `.tmp` artığı temizlenir (`sys_worker` ilk iş).
5. Yedek yazımı bölüm `PUT`'undan sonra `sys_worker` kuyruğuna (plan §5.1) — ardışık 5 `PUT` tek yazıma birleştirilsin (300 ms bekleme), her kaydetmede 40 KB (≈10 blok silme + yazım ≈0,6 s) tekrarlanmasın.
6. `cipher` kancası (§2 öneri 4) satır düzeyinde: `{"t":"grp0","e":"<base64 AES-GCM>"}` — ileride açılınca biçim değişmez.

### Risk

- GUI-taraflı import bölüm başına istek → 20 istek; hız sınırı ve CSRF ön işleyicisi her birinde çalışır; GUI sıralı gönderir.
- JSONL insan tarafından düzenlenmek istenirse CRC bozulur → `restore` "CRC uyuşmuyor, yine de?" seçeneği yalnız GUI'den, günlüklü.
- Sırların düz yedeği harici çipte USB/sökmeyle okunabilir (harita §8.3); karar 15 bilinçli; kademe-2 tam silme (§7) bu yüzden önemli.

---

## 7. Fabrika sıfırlama kademe 2 (`/data` biçimleme)

### Bulgu

- `esp_littlefs_format` → `lfs_format` yalnız kök dizin çiftini siler (`lfs.c:4417-4446`: `lfs_dir_alloc` 2 blok, süperblok yaz); tüm eski dosya blokları **fiziksel olarak kalır** → ≈2 × 45 ms + metadata ≈ **0,1–0,2 s**, ama sökülen çipten ekler ve yedek (sırlar) okunabilir.
- Tam silme `esp_partition_erase_range(part, 0, size)` → `esp_flash_erase_region`: 64 KB hizalı bloklarda blok silme (`esp_flash_api.c:727-733`), bloklar arasında bus kilidi bırakılır (`:700-706`), her 20 ms'de yol verilir (`sdkconfig:2054-2056`). 16 MB = 256 blok × 150 ms tipik ≈ **38 s** (azami 2 s/blok → 8,5 dk kuramsal); 32 MB ≈ 77 s. 4 KB sektörlerle 4 096 × 45 ms ≈ 184 s — blok silme açıkça üstün (`CONFIG_SPI_FLASH_BYPASS_BLOCK_ERASE` kapalı kalmalı). Çip-silme komutu kullanılmaz (`:629-641`), 4 s zaman aşımı (`chip_generic.c:65`) devreye girmez.
- SPI2 önbelleği kapatmadığından silme boyunca WiFi/HTTP çalışır; `sys_worker` görevi meşgul (20 ms'de 1 tik yol verir, görev WDT güvende).
- NVS kademe 1: `nvs_flash_erase()` 16 sektör ≈0,8–1 s **önbellek kapalı** (dahili flash) — yeniden başlatma öncesi kabul edilir.

### Öneri

1. **Kademe 2 = tam silme + biçimleme**, hızlı biçimleme seçeneği yok: `/data` için `storage_acquire(EXCLUSIVE)` → `esp_vfs_littlefs_unregister("lsdata")` → 64 KB adımlarla `esp_partition_erase_range` (ilerleme `job` durumuna `%`: `GET /api/jobs/<id>`) → `esp_littlefs_format` → `register` → `.ls2611` + dizinler. `/log` korunur; kademe-2 başlangıcı ve bitişi `/log`'a yazılır (ilke 11). Ardından kademe 1 + `sys_restart(RST_FACTORY)`.
2. Güvenlik gerekçesi: yedekte WiFi/SMTP/MQTT parolaları, Telegram jetonu, API anahtarı; eklerde kişisel içerik. Cihaz el değiştirdiğinde ≈40 s bedel kabul edilir; GUI'de "yaklaşık 1 dk sürer, cihaz çalışmaya devam eder" uyarısı.
3. Silme sırasında güç kesilirse: yarı silinmiş `/data` → sonraki açılışta mount hatası + bölge boş değil → `ERROR`; `sys_worker` açılışta NVS'deki `factory2_pending` bayrağını görüp silmeyi **sürdürür** (bayrak kademe-2 başında yazılır, sonunda silinir).
4. `/log` bölümü için "günlüğü sil" (`DELETE /api/logs`) = `unlink` ×2 (fiziksel silme değil; günlük sır içermez, karar: ilke 11 gereği "günlük silindi" kaydı yeni dosyanın ilk satırı).

### Risk

- 38 s'lik işte httpd `POST /api/device/factory-reset {level:2}` hemen `{job_id}` dönmeli (plan §6.2 iş modeli); senkron tutulursa soket zaman aşımı.
- W25Q azami blok silme süresi 2 s: aşırı aşınmış/sıcak çipte dakikalar; ilerleme göstergesi bunu görünür kılar, zaman aşımı 4,1 s/blok (`chip_generic.c:67`) aşılırsa `ESP_ERR_TIMEOUT` → iş `failed`, bayrak kalır, yeniden denenir.
- Yarı-silinmiş durumda cihaz `/data`'sız çalışır (günlük RAM/`/log`'da), `factory2_pending` bayrağı yokmuş gibi davranılırsa kullanıcı bozuk görür → bayrak zorunlu.

```c
// sys_worker/job_factory2.c — tam silme + biçimleme, ilerleme ile
static esp_err_t job_factory2(job_t *j)
{
    const esp_partition_t *p = storage_data_partition();
    ESP_RETURN_ON_ERROR(storage_acquire(STORAGE_EXCLUSIVE, pdMS_TO_TICKS(5000)), TAG, "busy");
    cfg_flag_set("factory2_pending", 1);
    esp_vfs_littlefs_unregister("lsdata");
    for (size_t off = 0; off < p->size; off += 0x10000) {
        esp_err_t err = esp_partition_erase_range(p, off, 0x10000);   // 64 KB blok, ≈150 ms
        if (err != ESP_OK) { job_fail(j, err); goto out; }
        job_progress(j, (off + 0x10000) * 100 / p->size);
    }
    ESP_GOTO_ON_ERROR(esp_littlefs_format("lsdata"), out, TAG, "format");
    ESP_GOTO_ON_ERROR(storage_mount_data(), out, TAG, "mount");       // .ls2611 + log/ config/ mail/
    cfg_flag_set("factory2_pending", 0);
    evlog_write(EV_FACTORY2_DONE);
out:
    storage_release(STORAGE_EXCLUSIVE);
    return j->err;
}
```

---

## 8. Plana etkisi

| Plan maddesi | Değişiklik |
|---|---|
| §5.1 cfg | Blob başına `{schema,len}` başlığı; **gruplar `cfg_t` dışında** (`cfg_group_get`), `cfg_t` ≈5 KB; `rt` ≤32 B; `nvs_get_stats` → `/api/storage`. Yedek biçimi **JSONL bölüm başına satır** + CRC32 + `end`; `import` GUI'de bölüm bölüm `PUT`; `export/restore` akışlı. Yedek yazımı 300 ms birleştirme. `cipher` kancası (soru 14). |
| §3.4 açılış | Adım 1: `init` hatası **veya `meta` yok** → `nvs_flash_erase` + yeniden `init`, nedeni RAM halkasına (karar 10, ALTE kalıntısı). |
| §5.2 storage | **İki bölüm**: `lslog` 1 MB @0 → `/log` (otomatik onarım), `lsdata` kalan → `/data` (sessiz biçimleme yok; boş bölge 8 KB denetimi; `.ls2611` düzen işareti, yarı-boyut kuralı ve `grow_on_mount` yok). Durum makinesi `ABSENT/MOUNTED/ERROR/FORMATTING` + okuma sayacı / dışlayıcı kilit. `esp_littlefs_info` önbellekli. SPI2 `SPI_DMA_DISABLED`, DIO 40 MHz, boyut `[4,32] MB` denetimi. |
| §5.3 evlog | Yol `/log/events.jsonl`; boşaltma aç-yaz-`fsync`-kapat; döndürme `EBUSY`'de ertelenir; okuma httpd'de kısa aç-oku-kapat, `before` + dosya kimliği. |
| §5.8 sdkconfig | `CONFIG_SPI_FLASH_SUPPORT_WINBOND_CHIP=y`; `CONFIG_LITTLEFS_MAX_PARTITIONS=2`, `LOOKAHEAD_SIZE=512`; NVS varsayılanları (assert kapalı). Şifreleme için ileride yalnız 3 `CONFIG_NVS_*` satırı. |
| §6.2 API | `POST /api/config/import` → GUI-taraflı bölüm `PUT` döngüsü (uç kalkabilir); `factory-reset level 2` → `{job_id}` + ilerleme; `PUT …/files/<ad>` httpd'de akışla yazar (sys_worker kuralına istisna). |
| Karar 12 | Kademe 2 = **tam silme (≈40 s) + biçimleme**, `/log` korunur, `factory2_pending` ile kesintiye dayanıklı. |
| §7 adım 1 | Cihaz denetimine ekle: grup blob yazım süresi ölçümü (GC'li/GC'siz), `/data` mount hatası senaryosu (bozuk süperblok → `ERROR`, boş → format), çip yok senaryosu (<5 ms), 2 MB ek yükleme süresi. |
| yapilacaklar | NVS şifreleme + yedek şifreleme (HMAC türevli AES-GCM, eFuse blok planı); W25Q256 doğrulaması; `/log` bölüm boyutu. |
