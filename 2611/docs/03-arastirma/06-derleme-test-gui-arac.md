# 06 — Derleme, sınama ve GUI araç zinciri (araştırma)

Kapsam: `02-plan-ilk.md` §6.3–6.5 ve §8 soru 10–11'in doğrulanması. Sekiz soru; her biri **Bulgu** (kaynak: dosya/satır, URL ya da bu oturumda yapılan ölçüm), **Öneri**, **Risk** ve gerekli yerde ≤40 satır iskelet.

Ortam (2026-11-05): macOS 27.2, Apple clang 21 (`ld-1267`), CMake 4.3.3, Node v24.17 / npm 11.13, Python 3.15 (`/opt/homebrew/bin/python3`), IDF 5.5.2 Python ortamı 3.9.6 (`~/.espressif/python_env/idf5.5_py3.9_env`), `brotli` 1.2.0 CLI; `zopfli`, `pngquant`, `oxipng`, `svgo`, `potrace`, `cppcheck`, `clang-tidy` **yok**. Ölçümler `ALTE/GUI` dosyaları üzerinde, gzip = Python `gzip.compress(level=9, mtime=0)`.

---

## 1. esbuild via npx — yeniden üretilebilir, çevrimdışı

**Bulgu**

- npm önbelleğinde esbuild 0.25.12, 0.27.0, 0.27.7, 0.28.1, 0.28.2 tar'ları ve `@esbuild/darwin-arm64` ikilileri var (`npm cache ls`); `~/.npm/_npx/` altında 0.28.1/0.28.2 kurulumları mevcut. Kayıt defterindeki güncel sürüm de **0.28.2** (`npm view esbuild version`, 2026-11-05).
- `npx --yes --offline esbuild@0.28.2 --version` → `0.28.2`, ağ erişimi olmadan çalışır. Önbellekte olmayan sürüm (`esbuild@0.26.0`) `--offline` ile **0,24 s içinde** `npm error code ENOTCACHED` verir (asılı kalmaz).
- `package.json` (`"esbuild": "0.28.2"`) ile `npm install --package-lock-only --offline` ve `npm ci --offline` sorunsuz; kilit dosyasında 27 `resolved` girdisi = esbuild + 26 platform ikilisi. esbuild'in **geçişli bağımlılığı yok**; sabit sürüm = kilit dosyası ile aynı belirleyicilik.
- `--bundle --minify --target=es2020 --format=iife`: ES modülleri tek IIFE'ye çevrildi (deneme: `import {t} from "./i18n.js"` → `(()=>{var o=t=>t;console.log(o("a"));})();`).
- ALTE üzerinde gerçek ölçüm (minify, bundle yok):
  - `app.js` 115 528 → 60 158 B ham; gz **22 635 → 14 123 B** (bütçe 14 KB — ALTE kodu tam sınırda).
  - `style.css` 57 243 → 42 293 B; gz **9 219 → 7 601 B** (bütçe 6 KB; ALTE iki temayı kopyalıyor, değişkenli tek tema ile 5–6 KB beklenir — 05 haritası §8 ile uyumlu).
- Kaynak haritası: `--sourcemap=external` → `app.min.js.map` 187 627 B ayrı dosya; JS içinde `sourceMappingURL` yorumu **yok** (0 eşleşme) — gömülü boyutu etkilemez.
- HTML minify (esbuild yapmaz): basit regex (yorum sil, `>\s+<` → `><`, çoklu boşluk → tek) 13 863 → **12 200 B gz**; Rust tabanlı `minify-html` 0.18.1 (py3.9 tekerleği var, **py3.15 için tekerlek yok**) → 12 016 B gz. Fark **184 B (%1,5)**. `htmlmin` 0.1.12 (2016) py3.15'te kurulamıyor (`Failed to build`).
- Node'suz Python alternatifi (`rjsmin`/`rcssmin` 1.3.0): isim kısaltma ve modül paketleme yok; 05 haritasının kaba ölçümü (yorum+boşluk) JS'te 17,7 KB gz verdi → esbuild'e göre **~3,5 KB** daha büyük; ayrıca ES modüllerini elle birleştirmek gerekir.

**Öneri**

- Tek sabit: `build.py` içinde `ESBUILD = "esbuild@0.28.2"`; çağrı `npx --yes --offline esbuild@0.28.2 …`; `ENOTCACHED` görülürse `--offline`'sız bir kez daha dene (ilk kurulum, ağ ister), yine olmazsa anlaşılır hata ("Node.js + ağ gerekli; `brew install node`"). Depoda ne `package.json` ne `node_modules` olur; önbellek `~/.npm/_npx/<hash>/` altındadır (hash paket belirtimine bağlı → belirleyici). `npm ci` + kilit istenirse `esp_bin/gui_tools/package.json` (commit) + `npm ci --prefix esp_bin/gui_tools` (`node_modules` git dışı); kazancı yok, karmaşıklık ekler — önerilmez.
- JS: `--bundle --minify --target=es2020 --format=iife --sourcemap=external --legal-comments=none --charset=utf8`. `.map` yalnız `<build>/gui/debug/` altında kalır, gömülmez.
- CSS: esbuild ile `--minify` (`--loader:.css=css`); tek dosya `src/css/app.css` içinden `@import` ile parçalar.
- HTML: Python regex yeter; güvenli kural: yalnız **satır sonu içeren** etiket arası boşluğu sil (`>\s*\n\s*<`), aynı satırdaki tek boşluğa dokunma; `<pre>`/`<textarea>` kullanma (2611 HTML'inde metin yok, i18n dolduruyor — risk pratikte sıfır).
- Node yalnız GUI kaynakları değiştiğinde çalışır (bkz. §2 bağımlılık zinciri); firmware-only değişiklikte npx hiç çağrılmaz.

**Risk**

- İlk derleme yeni bir makinede ağ ister (esbuild Go ikilisi ~10 MB). Belgelenir; `--offline` denemesi sayesinde ağ kopukken önbellekli sürüm çalışmaya devam eder.
- esbuild sürüm yükseltmesi çıktı baytlarını değiştirir (hash'li adlar değişir) — sürüm sabiti commit'le değişir, sorun değil.
- JS bütçesi 14 KB ALTE kodunda tam sınırda; 2611'in modüler yazımı küçülmeyi hedefler ama ölçülmeden kesin değil (bkz. §7).

---

## 2. CMake entegrasyonu — üretilen dosya ve `EMBED_FILES`

**Bulgu**

- `target_add_binary_data` (`tools/cmake/utilities.cmake:79-109`): `add_custom_command(OUTPUT <build>/<ad>.S … MAIN_DEPENDENCY "${embed_file}" DEPENDS … ${__DEPENDS})` + `target_sources`. `idf_component_register(EMBED_FILES …)` bunu `DEPENDS` vermeden çağırır (`component.cmake:512-514`). Dosyanın yapılandırma anında **var olması denetlenmez**; `MAIN_DEPENDENCY` üretilen dosya olunca Ninja zinciri kurar. IDF belgesi bunu açıkça örnekler (`build-system.rst:992-1003` "It is also possible to embed a generated file").
- Deney (sade CMake, Ninja): `add_custom_command(OUTPUT gui/app.js.gz DEPENDS ${GLOB_RECURSE src/*} build.py)` → `.S` (`MAIN_DEPENDENCY`) → `OBJECT` kütüphane. Sonuç: ilk derlemede üretim çalıştı; ikinci derleme **"ninja: no work to do"**; `src/app.js` düzenlenince 3 adım (üret → .S → derle) yeniden koştu; `src/` altına **yeni dosya** eklenince `file(GLOB_RECURSE … CONFIGURE_DEPENDS)` yeniden yapılandırmayı tetikledi ve üretim yeniden koştu.
- Python: IDF `PYTHON` özelliği = `~/.espressif/python_env/idf5.5_py3.9_env/bin/python` (`esp_bin/HW/build/CMakeCache.txt`); `export.sh` sonrası `python3` de aynı ortam (**3.9.6**). Yani `build.py` CMake'ten çağrıldığında **Python 3.9** ile çalışır; sistemdeki 3.15 kullanılmaz.
- `PROJECT_VER` sarmalayıcıda `project()` öncesi set edilir (ALTE kalıbı); bileşen CMakeLists'leri alt dizin kapsamında çalıştığı için `${PROJECT_VER}` oradan okunabilir.
- Hash'li adlar: `add_custom_command` çıktıları yapılandırma anında bilinmek zorunda → dosya **adına** hash gömülemez. Çözüm: sabit gömülü adlar (`app.js.gz`), hash yalnız **URL'de** (`/s/<hash>/app.js`), hash değeri `gui_hash.txt` olarak `EMBED_TXTFILES` ile gömülür (başlık üretip `OBJECT_DEPENDS` kurmaya gerek kalmaz).

**Öneri** — `2611/HW/components/platform/http/CMakeLists.txt` (iskelet):

```cmake
# GUI: 2611/GUI/src -> build.py -> <build>/gui/*  (yalnız GUI kaynağı değişince çalışır)
get_filename_component(GUI_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../GUI" ABSOLUTE)
set(GUI_OUT "${CMAKE_CURRENT_BINARY_DIR}/gui")
set(GUI_GZ index.html.gz app.js.gz app.css.gz keys.json.gz favicon.svg.gz
    lang/en.json.gz lang/de.json.gz lang/es.json.gz lang/fr.json.gz lang/it.json.gz lang/tr.json.gz)
list(TRANSFORM GUI_GZ PREPEND "${GUI_OUT}/" OUTPUT_VARIABLE GUI_OUTPUTS)
file(GLOB_RECURSE GUI_SRC CONFIGURE_DEPENDS "${GUI_DIR}/src/*")   # yeni dosya -> yeniden yapılandır
idf_build_get_property(_py PYTHON)                                 # IDF ortamı (py3.9)
add_custom_command(
    OUTPUT ${GUI_OUTPUTS} "${GUI_OUT}/favicon.png" "${GUI_OUT}/gui_hash.txt"
    COMMAND ${_py} "${GUI_DIR}/build.py" --src "${GUI_DIR}/src" --out "${GUI_OUT}"
            --ver "${PROJECT_VER}" --budget "${GUI_DIR}/budget.json"
    DEPENDS ${GUI_SRC} "${GUI_DIR}/build.py" "${GUI_DIR}/budget.json"
    COMMENT "GUI: esbuild + i18n + sprite + gzip + bütçe" VERBATIM)
idf_component_register(SRCS "http.c" "http_auth.c" "http_static.c" "http_sse.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server esp_timer nvs_flash
    EMBED_FILES    ${GUI_OUTPUTS} "${GUI_OUT}/favicon.png"
    EMBED_TXTFILES "${GUI_OUT}/gui_hash.txt")
```

- `build.py` hata verince Ninja adımı durur ve stderr görünür; ek `FATAL_ERROR` gerekmez. `build.py` çıktıya **önce** `*.tmp` yazıp sonunda yeniden adlandırmalı (yarım dosya kalmasın) ve hata halinde mevcut çıktıları silmeli (aksi halde ikinci `ninja` eski çıktıyla devam eder).
- `build.py` **Python 3.9 uyumlu** ve yalnız stdlib (`gzip, json, hashlib, re, subprocess, pathlib`): `match` yok, `X | None` yok (`from __future__ import annotations` ile olur), `tomllib` yok. Başında `assert sys.version_info >= (3, 9)`.
- Yapılandırma anında `execute_process` (ALTE kalıbı) seçilirse: daha basit ama her GUI düzenlemesi tam IDF yeniden yapılandırması demek; önerilmez. Ancak **hız/bütçe tablosunu** `message(STATUS)` ile görmek istenirse `build.py` çıktısı Ninja'da da görünür.
- HTTP tarafı: `/s/*` joker rota (`httpd_uri_match_wildcard`), `/s/<8hex>/<ad>` → `<ad>` tablosundan gömülü blob; `Cache-Control: public, max-age=31536000, immutable`; `index.html` `no-cache`; hash uyuşmasa da sunulur (eski sekme kırılmasın).

**Risk**

- `GLOB_RECURSE … CONFIGURE_DEPENDS` her `ninja` çağrısında dizini tarar (ms düzeyi, ihmal edilir).
- `PROJECT_VER` değişince `.gz`'ler yeniden üretilmez (DEPENDS'te yok) → `2611/HW/CMakeLists.txt`'yi de `DEPENDS`'e ekle; sarmalayıcı zaten `CMAKE_CONFIGURE_DEPENDS` ile izliyor.

---

## 3. gzip / brotli

**Bulgu**

- **Brotli yalnız HTTPS'te.** Chromium `net/http/http_request_headers.cc` `SetAcceptEncodingIfMissing`: `const bool can_use_advanced_encodings = (url.SchemeIsCryptographic() || IsLocalhost(url));` — `br` ve `zstd` yalnız bu doğruysa `Accept-Encoding`'e eklenir ("Advertise "br" encoding only if transferred data is opaque to proxy"). Firefox: Bugzilla 1670675, Valentin Gosu (Mozilla ağ ekibi): "You are correct that brotli is HTTPS only"; yalnız `network.http.accept-encoding` tercihine elle `br` eklenerek aşılır (varsayılan değil). Cihaz LAN'da düz HTTP sunduğu için (plan §6.1) **brotli kullanılamaz**; ölçülen kazanç (%13,9) kâğıt üstünde kalır.
- zopfli (pip `zopfli` 0.4.3, py3.15'e kuruldu) ölçümü, 15 yineleme:

| Dosya | gzip -9 | zopfli | kazanç | brotli 11 |
|---|---|---|---|---|
| index.html | 13 863 | 13 348 | %3,7 | 11 480 |
| app.js | 22 635 | 21 737 | %4,0 | 19 492 |
| style.css | 9 219 | 8 746 | %5,1 | 8 012 |
| lang/en.json | 7 176 | 6 901 | %3,8 | 6 181 |
| lang/tr.json | 7 885 | 7 612 | %3,5 | 7 141 |
| **Toplam** | **60 778** | **58 344** | **%4,0** | **52 306** |

  Beklenen %5–8 değil, **%4** (≈ 80 KB bütçede ~3 KB). zopfli ve gzip çıktıları iki çağrıda bayt bayt aynı (belirleyici).
- Belirleyici gzip: Python `gzip.compress(data, 9, mtime=0)` iki çağrıda aynı bayt; CLI karşılığı `gzip -9 -n`. Python başlıkta OS baytını 255 (bilinmiyor) yazar → platformdan bağımsız.

**Öneri**

- gzip -9, stdlib, `mtime=0`. zopfli **yok**: `build.py` IDF'in py3.9 ortamında koşar, oraya pip paketi eklemek kurulum adımı ve "makineye göre farklı bayt" demek; %4 için değmez. Bütçe aşımı %4'ün altında kalırsa yeniden değerlendir.
- Yanıt başlıkları: `Content-Encoding: gzip`, `Content-Type` doğru, `Vary: Accept-Encoding` (ara önbellek için; LAN'da zararsız). İstemci `Accept-Encoding` göndermese de gzip sun (1999'dan beri tüm tarayıcılar gönderir; `curl --compressed` yeterli) — kod yolu tek kalır.
- `config.json` benzeri 100 B altı dosyalar gzip'lenmez (ALTE'de 67 → 96 B büyüdü); 2611'de böyle dosya yok (`gui_hash.txt` metin gömülür).

**Risk**

- İleride HTTPS sunucu eklenirse brotli tekrar gündeme gelir; `build.py`'de biçim seçimi tek fonksiyon olsun.

---

## 4. i18n biçimi: `keys.json` + değer dizisi

**Bulgu** (ALTE `lang/*.json`, Python ile)

- 6 dilde **465 anahtar**, hepsi düz (`nesting depth 1`), tüm değerler `str`; anahtar kümeleri **eşit**; yer tutucu kümeleri anahtar başına **tutarlı (0 uyumsuzluk)**; boş değer **0**; değerlerde HTML etiketi **0**.
- Yer tutucular: `{time}` 42, `{n}` 18, `{count}`/`{status}`/`{device}`/`{pulse}`/`{total}`/`{msg}` 12'şer, diğerleri 6'şar (tek anahtar × 6 dil). Hepsi `{ad}` biçimi; **çoğul/ICU yok**, gerek de yok. Dikkat: bir yardım metninde `{"Authorization":"Bearer x"}` sabit JSON örneği var — doğrulayıcı yer tutucu desenini `\{[a-z_]+\}` ile sınırlamalı.
- İngilizceyle birebir aynı değer sayısı: de 29, es 26, fr 33, it 34, tr 26 (çoğu özel ad: Telegram, MQTT, SSID…) → "çevrilmemiş" kuralı gürültü üretir, eklenmemeli.
- ALTE HTML'inde `data-i18n` **hiç yok**; `app.js`'de 128 farklı sabit `t('…')` ve 2 dinamik (`t(k)`, `t(p)`) çağrı var. 2611'de HTML'de metin olmayacağı için (`data-i18n`) doğrulama kuralı HTML'yi de taramalı.
- Boyut (gz, 6 dil):

| Biçim | Toplam gz |
|---|---|
| A) olduğu gibi (girintili) | 46 163 |
| B) minify JSON (aynı yapı) | 45 554 |
| D) `keys.json` (2 448) + 6 değer dizisi (30 328; dil başına 4 724–5 277) | **32 776** |
| E) satır-ayrımlı düz metin değerler + keys | 32 200 |

  D = **−%29 (13,4 KB)**; 05 haritasının "~33 KB" tahmini doğru. E'nin ek kazancı 0,6 KB — JSON dizisi kalsın (ayrıştırma bedava).
- Plan bütçesi "dil ×6 ≤33 KB": ALTE'nin 465 dizgesiyle **32,8 KB — sınırda**. 2611 yeni ekranlar (tatil, gruplar, OTA onayı) ekler.

**Öneri**

- Biçim: `lang/keys.json` = sıralı anahtar dizisi; `lang/<xx>.json` = aynı sırada değer dizisi. Kaynakta (`src/lang/<xx>.json`) **sözlük** tutulur (okunabilir, diff'lenebilir); diziye `build.py` çevirir; anahtarlar sıralanır (belirleyici çıktı).
- Bütçe aşarsa 2. kademe: `build.py` HTML'deki `data-i18n="key"` ve JS'teki `t("key")` sabitlerini **indekse** çevirir (`t(123)`), `keys.json` cihaza hiç gömülmez (−2,4 KB) ve `keys.json` yalnız `debug/` çıktısı olur. İlk sürümde `keys.json` kalsın (geliştirici araçlarında anahtar görünür).
- Doğrulama kuralları (hata = derleme durur): (1) 6 dilde anahtar kümesi eşit; (2) anahtar başına `{ad}` kümesi eşit; (3) boş/yalnız-boşluk değer yok; (4) tüm değerler `str`, UTF-8, JSON geçerli; (5) HTML `data-i18n*="…"` ve JS `t("…")`/`t('…')` sabitlerinin tamamı anahtar kümesinde; (6) hiçbir anahtar kullanılmıyor olmasın — dinamik çağrılar için `src/lang/dynamic.json` ön-ek listesi (`"err."`, `"status."`), listedeki ön-ekler kullanılmış sayılır; (7) değerde `<` varsa hata (textContent politikası) — istisna listesi yok; (8) anahtar adı `^[a-z0-9_.]+$`.
- Tarayıcı: `fetch('/s/<hash>/lang/tr.json')` + `keys.json` aynı hash'le, `immutable`; dil seçimi `localStorage`; ilk yükleme `navigator.language` → desteklenen 6'dan biri yoksa `en`. `replaceAll('{n}', …)` yerine tek `replace(/\{([a-z_]+)\}/g, (_, k) => args[k])` (yer tutucu adı değer içinde tekrar etse de doğru).

**Risk**

- 33 KB bütçesi 2611'de aşılabilir; indeks modu hazır tutulmalı. Alternatif: bütçeyi 36 KB'ye çekip toplamı 80'de tutmak için PNG kazancını (−21 KB, §5) kullanmak — toplam zaten rahat.

---

## 5. SVG: sprite, favicon, logo

**Bulgu**

- Sprite ölçümü (ALTE `index.html`'deki 68 satır-içi SVG, 46 benzersiz): `<symbol>` sprite ham 10 219 B, **gz 1 772 B**; 68 adet `<svg><use href="#id"/></svg>` gz **82 B**. HTML'den tüm SVG'ler çıkınca gz 12 200 → **9 896 B** (−2,3 KB). Yani sprite satır-içi gömülürse HTML ≈ 11,7 KB; dış `icons.svg` ise HTML 9,9 + sprite 1,8 (immutable, bir kez iner).
- `<use href="#id">` (xlink'siz) ve `currentColor` ile stroke/fill: tüm güncel tarayıcılar, Safari/iOS dahil. ALTE ikonları zaten `stroke='currentColor'`.
- **SVG favicon** (caniuse "link-icon-svg", 2026-11-05 çekildi): Chrome 80+, Edge 80+, Firefox 41+, **Safari macOS 26.0+ ve iOS 26.0+** ("Not supported in 3.1–18.7"). WebKit bug 309949 (Mart 2026, Safari 26.3): SVG favicon içindeki `prefers-color-scheme` medya sorgusu dikkate alınmıyor. Yedek: `sips -z 32 32 logo.png` → **916 B** PNG (48×48: 1 514 B).
- Logo: `logo.png`/`darklogo.png` 400×400, 8-bit **paletli** PNG, 11 003 / 10 158 B (gz işe yaramaz; toplam gz bütçesinin %19'u). Görsel: üç vuruş — `<`, zikzak yıldırım, `>`; aynı şeklin iki renk sürümü. Elle çizilen SVG (aşağıda) **280 B ham, 200 B gz**; `qlmanage` ile 400 px render edildi, orijinalle örtüşüyor (vuruş kalınlığı/yuvarlak uçlar aynı):

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 400" fill="none" stroke="currentColor"
 stroke-width="14" stroke-linecap="round" stroke-linejoin="round">
<path d="M120 115 60 200l60 85M280 115l60 85-60 85M240 60l-22 70 36 24-78 3 50 34-78 3 50 34-78 3 36 24-22 70"/>
</svg>
```

  PNG + `filter: invert()` seçeneği 11 KB'de kalır; potrace/inkscape kurulu değil ve gerek yok.

**Öneri**

- İkon sprite'ı `index.html` içinde satır-içi (`<svg style="display:none">…`) — tek istek, Safari dış-`use` ayrıntıları yok; ikonlar yalnız `<path>` + 24×24 `viewBox`, `build.py` `src/icons/*.svg`'den birleştirir, `id` = dosya adı, kullanılmayan ikon hata.
- Logo: yukarıdaki SVG sprite'a `#logo` sembolü olarak girer (`currentColor` → tema otomatik); favicon için aynı yol `stroke-width="28"` ile `favicon.svg` (16 px'te okunur) + `favicon.png` 32×32 (Safari <26). HTML: `<link rel="icon" href="/favicon.svg" type="image/svg+xml"><link rel="icon" href="/favicon.png" sizes="32x32">`. Dark-mode medya sorgusu favicon SVG'ye konmaz (WebKit 309949), tek renk (koyu gri) kullanılır.
- Bütçe satırı "sprite+logo ≤5 KB" → gerçek ~2,9 KB (sprite 1,8 HTML içinde + favicon.svg ~0,2 + favicon.png 0,9); HTML bütçesi 11 KB sprite dahil tutulur.

**Risk**

- Satır-içi sprite her `index.html` isteğinde iner (no-cache; ~1,8 KB). SSE'li tek sayfa uygulamada sayfa yeniden yüklemesi seyrek — kabul.
- Safari 18 ve altı kullanıcıları PNG favicon görür (işlev kaybı yok).

---

## 6. Masaüstü birim testleri (Unity + CMake/ctest)

**Bulgu**

- IDF `components/unity/unity/src/{unity.c, unity.h, unity_internals.h}` = üst kaynak ThrowTheSwitch **Unity 2.6.0**, MIT, 4 341 satır. `-std=c11 -Wall -Wextra -Werror` ile Apple clang'da **0 uyarı** derlenir ve `UNITY_INCLUDE_CONFIG_H` tanımlanmazsa IDF katmanına ihtiyaç duymaz. IDF'in `include/unity_config.h` yolu `esp_err.h`, `sdkconfig.h` ve port işlevleri (`unity_putc`, `unity_flush`, `unity_exec_time_*`, `unity_port_linux.c`) ister → host'ta gereksiz sürtünme.
- IDF "linux hedefi" (`idf.py --preview set-target linux`, `docs/en/api-guides/host-apps.rst`): "experimental"; POSIX simülatörü "currently also works on macOS"; `set(COMPONENTS main)` zorunlu; mock'lar CMock + **Ruby** ister; tüm IDF derleme sistemi döner. Saf C modüller için ağır; kullanılmaz.
- Shim: gerçek `components/esp_common/include/esp_err.h` host'ta yalnız boş bir `sdkconfig.h` ile derlenir (`esp_err_to_name.c` dahil) → **shim gerekmez**. `esp_log.h` ise `esp_rom_sys.h` ister → **shim şart** (`ESP_LOGE/W` → `fprintf(stderr)`, `I/D/V` → `((void)0)`). `esp_check.h` gerçek başlığı shim `esp_log.h` önde olunca çalışır.
- **Kritik, makineye özgü:** `/usr/bin/cc` sysroot olarak `/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk` → **27.2 SDK** seçiyor; Xcode'un `ld-1267`'si bu SDK'nın `libSystem.tbd`'sini okuyamıyor: `ld: tapi error: malformed file … unknown architecture arm64e.x1-macos`. Sonuç: hiçbir host C programı bağlanmıyor, CMake'in derleyici sınaması (`CMakeTestCCompiler`) de düşüyor. Doğrulanan çözüm: `SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"` (→ Xcode'un MacOSX26.5.sdk'sı) ile `cc` bağlar, CMake + Ninja + `ctest` **"100% tests passed"**. `-Wl,-ld_classic` çözmüyor (aynı hata). `xcode-select -p` zaten Xcode.app; ortamda `SDKROOT` yok.
- CMake `-DCMAKE_OSX_SYSROOT=…` de çalışır; `SDKROOT` ortam değişkeni CMake tarafından otomatik alınır (deneyle doğrulandı).

**Öneri** — `2611/HW/test/host/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(lebensspur_host_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT DEFINED ENV{IDF_PATH})
  message(FATAL_ERROR "IDF_PATH tanımsız (Unity: \$IDF_PATH/components/unity/unity/src)")
endif()
set(HW "${CMAKE_CURRENT_LIST_DIR}/../..")
# Unity çekirdeği (ThrowTheSwitch 2.6.0, MIT) — IDF'in unity_config.h katmanı kullanılmaz
add_library(unity STATIC "$ENV{IDF_PATH}/components/unity/unity/src/unity.c")
target_include_directories(unity PUBLIC "$ENV{IDF_PATH}/components/unity/unity/src")
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE UNITY_SUPPORT_64)
# Host shim: shim/esp_log.h (printf), shim/sdkconfig.h (boş); esp_err.h gerçek IDF başlığı
add_library(shim INTERFACE)
target_include_directories(shim INTERFACE "${CMAKE_CURRENT_LIST_DIR}/shim"
    "$ENV{IDF_PATH}/components/esp_common/include")
option(SAN "ASan + UBSan" OFF)
enable_testing()
function(host_test name)                     # host_test(test_timer_sm test_timer_sm.c <modül.c> ...)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE "${HW}/components/app/core/include"
      "${HW}/components/platform/cfg/include")
  target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(${name} PRIVATE unity shim)
  if(SAN)
    target_compile_options(${name} PRIVATE -fsanitize=address,undefined -g)
    target_link_options(${name} PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()
host_test(test_timer_sm test_timer_sm.c "${HW}/components/app/core/timer_sm.c")
host_test(test_semver   test_semver.c   "${HW}/components/platform/ota/semver.c")
```

Örnek test `test_timer_sm.c`:

```c
#include "unity.h"
#include "timer_sm.h"
void setUp(void) {}
void tearDown(void) {}
static void test_checkin_resets_deadline(void) {
    timer_sm_t sm;
    timer_sm_init(&sm, 72 * 3600);
    timer_sm_checkin(&sm, 1000);
    TEST_ASSERT_EQUAL_UINT32(1000 + 72 * 3600, timer_sm_deadline(&sm));
}
int main(void) { UNITY_BEGIN(); RUN_TEST(test_checkin_resets_deadline); return UNITY_END(); }
```

Sürücü `esp_bin/test.sh` (derleme dizini `esp_bin/host_test/`, kaynak ağacına yazmaz):

```sh
#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# macOS: /usr/bin/cc CLT'nin MacOSX.sdk'sını (27.2) seçiyor, Xcode ld'si okuyamıyor;
# xcrun'ın verdiği SDK çalışıyor. SDKROOT verilmemişse xcrun'dan al.
[ "$(uname)" = Darwin ] && export SDKROOT="${SDKROOT:-$(xcrun --sdk macosx --show-sdk-path)}"
export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-v5.5.2}"
cmake -S "$ROOT/2611/HW/test/host" -B "$ROOT/esp_bin/host_test" -G Ninja "$@" >/dev/null
cmake --build "$ROOT/esp_bin/host_test"
ctest --test-dir "$ROOT/esp_bin/host_test" --output-on-failure
```

- Modül kuralı (planla uyumlu): `timer_sm`, `relay_sm`, `warn_sched`, `checkin`, `outbox`, `cfg` doğrulama/JSON, `semver`, reset sınıflandırıcı **yalnız** `esp_err.h`, `esp_log.h`, stdlib ve cJSON içerir (cJSON host'ta `$IDF_PATH/components/json/cJSON/cJSON.c` ile derlenir, 1 dosya). Zaman/NVS/GPIO çağrıları işlev işaretçisi ya da "port" başlığıyla dışarıda kalır.

**Risk**

- SDK sorunu bu Mac'in CLT/Xcode uyumsuzluğu; CLT güncellenince kendiliğinden gider, `test.sh` satırı zararsız kalır. Linux CI'da `SDKROOT` satırı atlanır.
- Unity'yi `IDF_PATH`'ten almak host testlerini IDF kurulumuna bağlar; projede IDF zaten zorunlu. Tam bağımsızlık istenirse 3 dosya `test/host/vendor/unity/` altına kopyalanır (MIT, lisans dosyasıyla).

---

## 7. Boyut bütçesi ve kalite kapıları

**Bulgu**

- IDF imajı bölüme sığmazsa derleme zaten durur: `components/partition_table/check_sizes.py` `raise SystemExit('Error: …')` (satır 43/100); doğal sınır ALTE tablosunda `ota_0/ota_1 = 0x1F0000` = **1 984 KiB**. Plan eşiği 1,5 MiB bunun altında → ayrı denetim gerekir.
- `esp_idf_size` 1.7.1 IDF ortamında kurulu; `python -m esp_idf_size --format json build/lebensspur.map` çalışıyor; `idf.py size --format json` de var. ALTE güncel: `lebensspur.bin` **1 268 896 B**; JSON `total_size` 1 268 796, `flash_code` 813 176, `flash_rodata` 337 440, `used_diram` 145 932 / 452 112 (%32).
- `-Werror` **zaten küresel**: `tools/cmake/build.cmake:132-144` tüm bileşenlere `-Wall -Werror -Wextra` uygular; gevşetmeler: `-Wno-error=unused-function/-unused-variable/-unused-but-set-variable/-deprecated-declarations`, `-Wno-error=extra`, `-Wno-unused-parameter`, `-Wno-sign-compare`, `-Wno-enum-conversion`. Yani plan maddesi "proje bileşenlerinde -Werror" fiilen var; eksik olan gevşetmelerin **proje bileşenlerinde geri alınması**.
- Statik analiz: `cppcheck`, `clang-tidy` yok. IDF ortamında `pyclang 0.6.3` (`idf.py clang-check`) kurulu ama `esp-clang` araç zinciri `~/.espressif/tools` altında **yok** (`idf_tools.py install esp-clang` gerekir, büyük indirme).

**Öneri**

- GUI bütçesi `2611/GUI/budget.json` (`{"index.html.gz": 11264, "app.js.gz": 14336, "app.css.gz": 6144, "lang_total": 33792, "total": 81920}` — bayt, KiB tabanlı; "sprite+logo" satırı HTML ve favicon içine katlanır) → `build.py` gzip sonrası tabloyu basar, aşımda `exit 1` (Ninja durur).
- Firmware eşiği `build.sh` içinde (bkz. §8): `stat -f %z build/lebensspur.bin` ≤ 1 572 864 değilse hata; ek olarak `esp_idf_size --format json` çıktısını `esp_bin/logs/size_v<VER>.json`'a yaz (eğilim takibi, 05-denetim'e kanıt). Bölüm tablosunu 1,5 MiB yapıp (`0x180000`) sınırı doğallaştırmak §5.8'in kararı; iki yol birbirini dışlamaz.
- Uyarılar: `2611/HW/cmake/warnings.cmake` tek dosya, her proje bileşeni `idf_component_register` sonrası `include(...)`:

```cmake
# Proje bileşenleri: IDF'in gevşettiği uyarıları hataya çevir (IDF/yönetilen bileşenler etkilenmez)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Werror=extra -Werror=unused-function -Werror=unused-variable
    -Werror=unused-but-set-variable -Wunused-parameter -Wsign-compare
    -Wshadow -Wundef -Wformat=2 -Wimplicit-fallthrough -Wvla)
```

  `-Wconversion` IDF başlıklarıyla gürültülü, dışarıda kalır. Alternatif (tek yer): sarmalayıcıda `project()` sonrası proje bileşen adları üzerinde döngü + `idf_component_get_property(lib ${c} COMPONENT_LIB)`; bileşen içi `include` daha görünür.
- Statik analiz "denetim" adımı, derleme kapısı değil: `brew install cppcheck` → `cppcheck --enable=warning,style,performance --std=c17 --project=esp_bin/2611/build/compile_commands.json -i managed_components`; clang-tidy için `idf_tools.py install esp-clang` + `idf.py clang-check` ileride (yapilacaklar).

**Risk**

- Bütçeler ALTE ölçümünden türetildi; 2611 adım 5'te ilk gerçek ölçümle **bir kez** kalibre edilir, sonra sabitlenir (bütçeyi sık oynatmak kapıyı anlamsızlaştırır).

---

## 8. `build.sh`: iki ağaç, tek betik

**Bulgu**

- `idf.py -C/--project-dir` (`tools/idf.py:721`) ve `-B/--build-dir` (`idf_py_actions/core_ext.py:418`) var. Her sarmalayıcının kendi dizini olunca (`esp_bin/2611/`, `esp_bin/HW/`) derleme dizini varsayılan `<proj>/build` → `-B` gerekmez.
- `sdkconfig.esp_bin`'deki bölüm yolu proje dizinine göre çözülür; `esp_bin/2611/` aynı derinlikte → `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="../../2611/HW/partitions.csv"`.
- `project(lebensspur)` iki ağaçta da aynı kalabilir (`lebensspur.bin`; OTA sürüm dosyası adı sabit olmalı). Sürüm uzayları ayrı tutulmalı (ALTE 0.0.x, 2611 ≥ 0.1.0), yoksa `esp_bin/firmware/lebensspur_vX.bin` çakışır.

**Öneri** — `esp_bin/build.sh` çekirdeği (mevcut IDF ortam bloğu korunur):

```sh
LINE=2611; TAG=""
if [ "${1:-}" = "--alte" ]; then LINE=alte; TAG="alte_"; shift; fi
case "$LINE" in
  2611) PROJ="$ROOT/esp_bin/2611"; SRC="$ROOT/2611/HW" ;;
  alte) PROJ="$ROOT/esp_bin/HW";   SRC="$ROOT/ALTE/HW" ;;
esac
if [ "${1:-}" = "test" ]; then exec "$ROOT/esp_bin/test.sh" "${@:2}"; fi   # host birim testleri
ARGS=("$@"); [ ${#ARGS[@]} -eq 0 ] && ARGS=(build)
idf.py -C "$PROJ" "${ARGS[@]}"
if [[ " ${ARGS[*]} " == *" build "* ]] && [ -f "$PROJ/build/lebensspur.bin" ]; then
  VER=$(sed -nE 's/.*set\(PROJECT_VER "([^"]+)"\).*/\1/p' "$SRC/CMakeLists.txt")
  BIN="$PROJ/build/lebensspur.bin"; SIZE=$(stat -f %z "$BIN"); MAX=$((1536*1024))
  mkdir -p "$ROOT/esp_bin/firmware" "$ROOT/esp_bin/logs"
  python -m esp_idf_size --format json "$PROJ/build/lebensspur.map" > "$ROOT/esp_bin/logs/size_${TAG}v${VER}.json"
  if [ "$SIZE" -gt "$MAX" ]; then echo "HATA: imaj $SIZE B > $MAX B (1,5 MiB)" >&2; exit 1; fi
  cp "$BIN" "$ROOT/esp_bin/firmware/${TAG}lebensspur_v${VER}.bin"
  cp "$BIN" "$ROOT/esp_bin/firmware/${TAG}lebensspur_latest.bin"
  echo "Firmware: ${TAG}lebensspur_v${VER}.bin ($SIZE B, sınır $MAX B)"
fi
```

- `esp_bin/2611/CMakeLists.txt` ALTE sarmalayıcısının kopyası; yalnız `SRC_DIR` → `../../2611/HW`. `esp_bin/2611/sdkconfig.esp_bin` yukarıdaki yol. `.gitignore`'a `esp_bin/2611/build/`, `esp_bin/2611/sdkconfig`, `esp_bin/2611/managed_components/`, `esp_bin/host_test/` eklenir; `dependencies.lock` git'te.
- `esp_bin/README.md` tablo güncellenir (iki ağaç, `test`, `--alte`).

**Risk**

- `--alte` ilk argüman olmak zorunda (basitlik); yanlış sırada verilirse `idf.py` bilinmeyen seçenek hatası verir — kabul edilebilir.
- `exec test.sh` satırı IDF `export.sh` sonrası çalışır; `python` IDF venv'idir (esp_idf_size oradadır) — `test.sh` için önemi yok.

---

## Plana etkisi (02-plan-ilk.md'ye işlenecekler)

1. **§6.4 / §3.1 — `build.py` Python 3.9 + stdlib.** CMake'ten IDF'in py3.9 ortamıyla çağrılır; `match`, `X | Y`, `tomllib` yok; pip bağımlılığı **yok** (zopfli −%4, minify-html py3.15 tekerleksiz, htmlmin ölü). esbuild `npx --yes --offline esbuild@0.28.2`, `ENOTCACHED` → ağlı yeniden deneme → anlaşılır hata. Depoda `package.json`/`node_modules` yok.
2. **§6.4 — `execute_process` yerine `add_custom_command`** + `file(GLOB_RECURSE … CONFIGURE_DEPENDS)`; sabit gömülü adlar, hash yalnız URL'de (`/s/<hash>/…`), hash `gui_hash.txt` olarak `EMBED_TXTFILES`. Çıktı dizini `<build>/esp-idf/http/gui/`. Kanıt: ikinci derleme "no work to do", kaynak düzenlemesi/yeni dosya yeniden üretim tetikliyor.
3. **§6.1 — brotli yok**, gzip -9 `mtime=0`; başlıklar `Content-Encoding: gzip`, `Vary: Accept-Encoding`; `Accept-Encoding` denetimi yapılmaz. Chromium/Firefox `br`'ı düz HTTP'de reklam etmiyor.
4. **§6.3 i18n** — `keys.json` + dizi: ölçülen −%29 (46,2 → 32,8 KB); bütçe 33 KB **sınırda** → indeks modu (`t(123)`, keys gömülmez, −2,4 KB) yedek plan. Sekiz doğrulama kuralı; yer tutucu deseni `\{[a-z_]+\}`; ICU/çoğul gerekmez.
5. **§6.3 görsel** — Logo elle çizilmiş SVG (280 B, `currentColor`), PNG'ler kalkar (**−21 KB gz**, bütçenin en büyük tek kazancı); sprite satır-içi (1,8 KB); favicon SVG (Safari 26+) + 32×32 PNG 916 B (Safari ≤18); "sprite+logo ≤5" satırı HTML'e katlanır, favicon ~1,1 KB ayrı.
6. **§6.5 host testleri** — Unity 2.6.0 `IDF_PATH`'ten (vendor yok), IDF `unity_config.h`/linux hedefi/CMock **kullanılmaz**; shim yalnız `esp_log.h` (+boş `sdkconfig.h`), `esp_err.h` gerçek; `esp_bin/test.sh` `SDKROOT`'u `xcrun`'dan alır (bu Mac'te CLT 27.2 SDK + Xcode ld uyumsuzluğu aksi halde tüm host bağlamasını kırıyor); `ctest --output-on-failure`; isteğe bağlı `-DSAN=ON`.
7. **§6.5 kapılar** — `-Werror` IDF'te zaten küresel; proje bileşenlerine `warnings.cmake` ile gevşetmeleri geri alan set. İmaj eşiği `build.sh`'de `.bin` boyutu (1,5 MiB) + `esp_idf_size` JSON günlüğü; bölüm sınırı (1 984 KiB) IDF tarafından zaten zorlanıyor. cppcheck `brew` ile denetim adımı; clang-tidy (`esp-clang`) yapilacaklar.
8. **§6.4 build.sh** — `--alte` ilk bayrak, `test` alt komutu, `idf.py -C`, `esp_bin/2611/` sarmalayıcı + `sdkconfig.esp_bin`; firmware adı `alte_` ön-ekiyle ayrışır; sürüm uzayları ayrı.
9. **§7 adım 0** iskeletine ekle: `esp_bin/2611/`, `esp_bin/test.sh`, `2611/HW/cmake/warnings.cmake`, `2611/HW/test/host/{CMakeLists.txt,shim/}`, `2611/GUI/{build.py,budget.json}`; adım 5'te bütçeler bir kez kalibre edilip sabitlenir.
