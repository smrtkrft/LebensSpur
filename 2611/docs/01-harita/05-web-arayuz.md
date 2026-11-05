# 05 — Web arayüzü (ALTE/GUI) haritası

Kaynaklar: `ALTE/GUI/index.html` (1741 satır), `app.js` (3251), `style.css` (3103), `config.json`, `lang/{en,de,es,fr,it,tr}.json` (465 anahtar), `pic/logo.png`, `pic/darklogo.png`. Sürüm 0.0.70 (`config.json`, `index.html:7`, `index.html:1739`). Dosyalar derleme anında gzip'lenip firmware'e gömülüyor; sunucu tarafı (`/ext/*`, `/api/*`) başka belgede. Satır referansları `dosya:satır`. Bu belge dosyaların bütününün okunmasına ve `grep`/Python sayımlarına dayanır; varsayım içeren yerler açıkça işaretlidir.

---

## 1. Genel yapı

| Konu | Durum |
|---|---|
| Mimari | Tek sayfa uygulama (SPA). Tek `index.html`; vanilla JS (ES5 üslubu: `var`, `function`, `.then`), **dış kütüphane yok**, derleme/paketleme adımı yok, modül yok. |
| Yükleme | `/ext/style.css?v=0.0.70` (`index.html:7`), `/ext/app.js?v=0.0.70` (`index.html:1739`, body sonunda, `defer` yok). Dil dosyası `/ext/lang/<dil>.json?v=<Date.now()>` (`app.js:59`, önbellek kırıcı → her yüklemede yeniden indirilir). Logolar `/ext/pic/logo.png`, `/ext/pic/darklogo.png`. |
| Ana ekran | Başlık "LEBENSSPUR / SMARTKRAFT" (`index.html:11-14`), ortada ya logo ya da geri sayım halkası (SVG `r=90`, çevre 565.48; `index.html:18-32`, `app.js:531`), halkanın altında turuncu tatil göstergesi (`index.html:33-38`). Alt eylem çubuğunda 4 yuvarlak, yalnız ikonlu düğme: Ayarlar, Durdur, Yeniden başlat (check-in), Çıkış (`index.html:42-68`). |
| Ayarlar | Modal (`#popup`) + karartma (`#overlay`), `display:flex/none` ile (`style.css:147-166`). Üstte 5 sekmeli **carousel** gezinme: `panel-timer`, `panel-conn`, `panel-actions`, `panel-system`, `panel-info` (`index.html:84-88`); sol/sağ oklar sonsuz döngü (`app.js:224-236`). Sekme seçimi `data-index` ile yapılır; `data-tab` özniteliği JS'te **hiç okunmuyor** (`grep` sonucu: 0). Aktif sekme `translateX` ile ortalanır (`app.js:209-220`). |
| Alt sayfalar | Üç ayrı mekanizma aynı işi yapar: Info → `.subpage` + `data-subpage` (`app.js:1250-1307`); System → `.subpage` + `openSystemSubpage()` (`app.js:1637-1690`); Actions → `.action-detail-view` + `openActionSubpage()` (`app.js:2158-2197`) ve onun içinde ikinci bir seviye (mail grubu listesi → grup detayı, `app.js:2802-2846`). Alt sayfa açıkken carousel gizlenir (`style.display='none'`), footer "İptal" → "Geri" olur (`app.js:129-136`). Sekme değişince alt sayfalar kapansın diye `updateCarousel` iki kez **monkey-patch** edilir (`app.js:1724-1730`, `app.js:2227-2233`). |
| Footer | "İptal/Geri" + "Kaydet" (`index.html:1722-1725`). Kaydet'in ne yaptığı bağlama göre değişir (`app.js:158-172`): aksiyon alt sayfasındaysa ilgili `save*Config`, güvenlik alt sayfasındaysa `saveSecuritySettings`, **diğer her durumda** (Timer, Connections, Actions ana, System ana, Info, HW/GUI OTA ve Reset API alt sayfaları dahil) `saveAllSettings()`. |
| Giriş (login) | `#loginOverlay` (`index.html:1729-1737`, z-index 2000). İki tetikleyici: (a) `window.fetch` sarmalanır; `/api/` ile başlayan ve `/api/auth/login` olmayan bir istek 401 dönerse `showLogin()` (`app.js:29-39`); (b) açılışta `/api/system/info` → `auth_enabled` ise bir kez `/api/auth/status` → `enabled && !logged_in` ise `showLogin()` (`app.js:320-330`). Giriş: `POST /api/auth/login {password}`; `ok` → `location.reload()`; `locked` → `auth.locked {s}` (`app.js:2596-2623`); Enter tuşu desteklenir. Arka plandaki sayfa ve ana ekran yüklenmeye devam eder; **SSE (`EventSource`) sarmalayıcıdan geçmez**, 401'de `onerror` → 3 s'de bir sonsuz yeniden bağlanma (`app.js:887-890`). |
| Tema | `body.light-theme` sınıfı; `localStorage.ls_theme` (`app.js:1592-1618`); açık temada logo `darklogo.png`'ye döner (`app.js:1595-1600`, `app.js:698-700`). Varsayılan koyu. Cihazda saklanmaz (tarayıcıya özel). |
| Dil | `localStorage.ls_lang`, varsayılan `en` (`app.js:44`). Cihazda saklanmaz. |
| Responsive | Tek `@media (max-width:480px)` (`style.css:1112-1174`): halka 240 px, düğmeler 48 px, modal %95 × 80vh, carousel 180 px, `.setting-group` dikey. `body{overflow:hidden}` (`style.css:12`), `.main{height:100dvh}` (`style.css:21`). Modal sabit 70vh/80vh; içerik `.modal-body` içinde kayar. |
| Tipografi | Monospace yığını `'Consolas','SF Mono','Fira Code',monospace` (`style.css:8`); sistemde yoksa genel monospace. |
| Eksikler | Favicon `<link>` yok, `theme-color` yok, `<form>` yok (Enter ile gönderim yalnız login'de), `aria-*`/`role`/`<label for>` sıfır (sayım), 32 yalnız-ikon düğme; ana ekrandaki 4 düğmenin `title`'ı bile yok. |

---

## 2. Ekranlar ve sekmeler (eksiksiz)

### 2.0 Ana ekran (`index.html:10-69`)

| Öğe | Davranış |
|---|---|
| Logo / halka | `timerEnabled && remaining>0` veya tatil aktifse halka, aksi halde logo (`app.js:682-719`). |
| Halka | `stroke-dashoffset = 565.48 × (1 − kalan/toplam)` (`app.js:644-651`); tatilde gri (`.timer-ring.vacation`, `app.js:606`). `.timer-ring.warning` (`style.css:1058`) JS'te hiç set edilmiyor. |
| Sayaç metni | 3 satır (`#timeDays/#timeHours/#timeMinutes`); parçalar `g/s/d` ve <60 dk ise saniye (`app.js:618-642`). Etiket `REMAINING` / tatilde `WAITING`. Yerel 1 s `setInterval` ile azalır (`app.js:775-812`). |
| Tatil göstergesi | `#vacationIndicator.active`; metin `timer_display.vacation_left` **boş `{time}` ile** (`app.js:709`, `{time:''}`); kalan süre `#vacationRemaining`'e yazılmak istenir ama o id HTML'de **yok** (`app.js:686`). |
| Ayarlar düğmesi | `openPopup()`: modal aç + `fetchTimerState()` (`app.js:101-106`). |
| Durdur | Tatil aktifse `POST /api/timer/set {action:'vacation_cancel'}`, değilse `{action:'cancel'}`; **onay sorulmaz** (`app.js:898-932`). |
| Yeniden başlat | `POST /api/timer/set {action:'restart'}` → yanıttaki `remaining_seconds/total_seconds` ile yerel sayaç; onay yok (`app.js:937-958`). Bu, "check-in" eylemidir. |
| Çıkış | `window.location.href='about:blank'` (`app.js:963-968`). Oturumu kapatmaz. |

### 2.1 Sekme: Timer (`#panel-timer`, `index.html:102-186`)

| Alan | id | Tip / sınır / doğrulama | Not |
|---|---|---|---|
| Birim | `.unit-btn[data-unit]` | gün / saat / dakika (`gun/saat/dakika`), varsayılan saat | `selectedUnit` (`app.js:536-546`) |
| Süre | `timerValue` | number 1–60, +/- düğmeleri, klavye girişi 1–60'a kırpılır (`app.js:555-579`) | Sınır birimden bağımsız: 60 dk / 60 sa / 60 gün |
| Durum | `timerStatus` | metin: pasif / aktif {time} / tatil {time} (`app.js:724-738`) | |
| Alarm sayısı | `alarmCount` | range 0–10 (HTML), JS `max = floor(süre/2)` (`app.js:976-990`) | Değer `alarmCountValue` |
| Alarm bilgisi | `alarmInfo` | "x. y. z. saatte alarm çalar" (`app.js:992-1028`) | HTML varsayılanı Türkçe ve sabit (`index.html:136`) |
| Tatil modu | `vacationMode` | toggle; `vacationToggleDirty` ile SSE'nin ezmesi engellenir (`app.js:1046-1055`) | |
| Tatil süresi | `vacationDays` | number 1–60 (+/- kırpar; **klavye girişi kırpılmaz**, `app.js:1057-1069`) | Tatil aktifken gizli, `vacationActiveText` gösterilir |

Kaydet (footer): `saveAllSettings()` → bkz. §4. Timer zinciri sırayla: `{unit,value}` → `{action:'set_alarms',count}` → tatil (`vacation_set {days}` veya `vacation_cancel`) (`app.js:1183-1230`).

### 2.2 Sekme: Connections (`#panel-conn`, `index.html:189-531`) — 5 akordeon

Akordeon aç/kapa `.accordion.open` (`app.js:253-263`); `configured` sınıfı başlığı beyaz yapar.

**Mail (SMTP)** (`index.html:193-240`) — yüklenir `GET /api/smtp/get` (`app.js:398-416`, `configured` ise doldurur).

| Alan | id | Tip | Not |
|---|---|---|---|
| SMTP sunucu | `smtpServer` | text | |
| Port | `smtpPort` | number, varsayılan 465 | sınır yok |
| Gönderen | `smtpUser` | email | |
| API anahtarı / uygulama parolası | `smtpApiKey` | **text** (parola tipi değil, görünür) | Sunucudan geri gelmez; her Save'de boş gönderilebilir |
| Test gönder | `smtpTestBtn` | `POST /api/smtp/test {}` → `smtpStatus` (`app.js:483-510`) | **Kayıtlı** ayarı test eder, formdakini değil |

Kaydet: yalnızca `smtpServer` ve `smtpUser` doluysa `POST /api/smtp/save {server,port,user,api_key}` (`app.js:1083-1099`).

**WiFi** (`index.html:245-324`) — yüklenir `GET /api/wifi/config` (`app.js:421-480`); SSID yalnız alan boşsa doldurulur (`app.js:429`).

| Alan | id | Not |
|---|---|---|
| Durum | `st` | "AP Mode Active" / "Connected: {ssid} ({ip})" (`app.js:338-347`, 15 s'de bir) |
| SSID | `ssid` | text, `required` (form olmadığı için etkisiz) |
| Parola | `pass` | password; sunucudan gelmez |
| Statik IP | `wifiStaticIpEnabled` | toggle → `wifiStaticIpFields` (`app.js:268-276`) |
| IP / ağ geçidi / maske / DNS | `sip`, `wifiGateway`, `wifiSubnet`, `wifiDns` | text, placeholder'lar; **biçim doğrulaması yok** |
| mDNS | `wifiMdns` | `/api/system/info.mdns` |

Kaydet: her zaman `POST /api/wifi/connect {ssid,password,static_ip,gateway,subnet,dns}` (`app.js:1101-1122`); SSID boşsa NVS temizlenir (yorum `app.js:1101`).

**WiFi (Yedek)** (`index.html:329-406`) — aynı alan seti: `wifiBackupSsid`, `wifiBackupPass`, `wifiBackupStaticIpEnabled`, `wifiBackupStaticIp`, `wifiBackupGateway`, `wifiBackupSubnet`, `wifiBackupDns`, `wifiBackupMdns`. Kaydet: `POST /api/wifi/backup/save` (`app.js:1124-1144`).

**AP Modu** (`index.html:410-476`)

| Alan | id | Not |
|---|---|---|
| AP aktif | `apModeEnabled` | toggle, kapatınca `apModeWarning` uyarı bandı (`app.js:291-304`) |
| SSID | `apSsid` | readonly, `/api/system/info.ap_ssid` |
| Parola | `apPass` | readonly; **JS hiç doldurmuyor → daima "-"** (`index.html:457`; `grep apPass app.js` = 0) |
| mDNS | `apMdns` | readonly, `.mdns` |

Kaydet: `POST /api/wifi/ap/set {enabled:'true'|'false'}` (`app.js:1146-1156`).

**WiFi Gücü** (`index.html:480-529`)

| Alan | id | Not |
|---|---|---|
| Azami TX | `txPowerMax` | range 8–20 dBm, adım 1, varsayılan 17; `txPowerMaxValue` |
| Uyarlamalı | `txPowerAdaptive` | toggle |
| Şu anki | `txPowerNow` | `cfg.tx_now` |

Kaydet: `POST /api/wifi/txpower {max_dbm, adaptive:'true'|'false'}` (`app.js:1158-1169`).

### 2.3 Sekme: Actions (`#panel-actions`, `index.html:534-1015`)

**Kartlar** (`index.html:547-662`): 6 kart, `data-action` / `data-active`. Kartın gövdesine tıklama aktif/pasif çevirir ve **anında** `POST /api/actions/save` (`app.js:2214-2222`, `3143-3161`); dişli düğmesi alt sayfa açar (`app.js:2200-2211`). Açılışta `GET /api/actions/get` (`app.js:3120-3141`).

| Kart | data-action | Alt sayfa | Yükleyici |
|---|---|---|---|
| Telegram (erken) | `early-telegram` | `telegramConfigView` | `loadTelegramConfig` |
| Hatırlatma maili (erken) | `early-mail` | `earlyMailConfigView` | `loadEarlyMailConfig` |
| Alarm API (erken) | `early-api` | `apiConfigView` | `loadApiConfig` |
| Mail grupları (tetik) | `trigger-mail` | `triggerMailConfigView` | `loadTriggerMailGroups` |
| LS API (tetik) | `trigger-api` | `apiConfigView` (aynı görünüm) | `loadApiConfig` |
| Röle (tetik) | `trigger-relay` | `relayConfigView` | **yok** — `loadRelayConfig` hiç çağrılmıyor (`app.js:2205`, tanım `app.js:2332`) |

**Webhook / MQTT** (`apiConfigView`, `index.html:668-740`) — iki kart aynı formu paylaşır; "hangi olayda gönderileceği" kartların aktifliğiyle belirlenir (`api.desc`).

| Alan | id | Tip / doğrulama |
|---|---|---|
| URL | `webhookUrl` | url |
| Yöntem | `webhookMethod` | select POST/GET/PUT |
| Başlıklar | `webhookHeaders` | textarea; kaydederken `JSON.parse` ile doğrulanır (`app.js:2463-2465`) |
| Gövde | `webhookBody` | textarea; değişkenler `{status}`, `{time}`, `{device}` |
| Test | `testWebhookBtn` | önce kaydet, sonra `POST /api/api-config/test {target:'webhook'}` → `webhookStatus` (`app.js:2483-2510`) |
| Broker | `mqttBroker` | text (`mqtt://host:port`) |
| Konu | `mqttTopic` | text |
| Kullanıcı / parola | `mqttUser`, `mqttPass` | parola boş gelir; `has_pass` ise placeholder "kayıtlı" (`app.js:2443-2444`) |
| Mesaj | `mqttPayload` | textarea |
| Test | `testMqttBtn` | `{target:'mqtt'}` → `mqttStatus` |

Kaydet (footer): `POST /api/api-config {wh_url,wh_method,wh_headers,wh_body,mq_url,mq_topic,mq_user,mq_pass,mq_payload}` → kapat (`app.js:2451-2481`).

**Röle** (`relayConfigView`, `index.html:743-857`)

| Alan | id | Tip / sınır |
|---|---|---|
| Pin bilgisi | — | sabit metin `relay.pin_info` "Relay: D8 (GPIO 19) \| Button: D10 (GPIO 18)" |
| Ters çevir | `relayInverted` | toggle; `relayIdleState/relayActiveState` metinleri değişir (`app.js:2249-2259`) |
| Başlangıç gecikmesi | `relayDelay` | select 0/1/2/3/5/10 s |
| Aktif süre | `relayDurationValue` + `relayDurationUnit` | number 1–60 + sn/dk; ipucu "sn 1–59, dk 1–60" ama **JS kırpmıyor** |
| Darbe modu | `relayPulseEnabled` | toggle → `relayPulseFields` |
| Darbe süresi | `relayPulseDuration` | select 1/3/5/10 s |
| Döngü bilgisi | `relayCycleText` | `relay.cycle_text` (`app.js:2272-2289`) — çift `{pulse}` hatası, bkz. §9 |
| Test | `testRelayBtn` | önce `POST /api/relay/set`, sonra `POST /api/relay/test {}` (`app.js:2392-2429`) |

Kaydet: `buildRelaySaveData()` → `{duration, pulse, delay, inverted:'true'|'false'}`; darbe kapalıysa `duration=0, pulse=0` ("sürekli") (`app.js:2302-2329`). Aynı veri **her `saveAllSettings`'te de** gönderilir (`app.js:1171-1181`).

**Telegram** (`telegramConfigView`, `index.html:860-891`): `telegramToken` (text; kayıtlıysa boş + placeholder "token kayıtlı"), `telegramChat`, `telegramMessage`; test düğmesi önce kaydeder sonra `POST /api/telegram/test` (gövdesiz, `app.js:2669-2704`). Kaydet: `POST /api/telegram/save {token,chat_id,message}`.

**Erken uyarı maili** (`earlyMailConfigView`, `index.html:894-922`): `earlyMailRecipient` (email), `earlyMailSubject`, `earlyReminderMessage`. Kaydet: `POST /api/early-mail/save {email,subject,body}`.

**Mail grupları** (`triggerMailConfigView`, `index.html:925-938`): `mailGroupList` dinamik (`renderMailGroupList`, `app.js:2767-2800`: ad + "{count} recipients" + Görüntüle), `createMailGroup` → yeni grup (`id=-1`). Açıklama "en çok 10 grup" der; **istemci tarafı sınır yok**.

**Grup detayı** (`mailGroupDetailView`, `index.html:941-1013`)

| Alan | id | Not |
|---|---|---|
| Ad | `mailGroupName` | zorunlu (`alert.group_name_required`) |
| Konu | `mailGroupSubject` | |
| İçerik | `mailGroupContent` | textarea |
| Dosya ekleri | `mailGroupFile` (gizli input, `multiple`, `accept='image/*,audio/*,video/*,.txt,.pdf,.doc,.docx'`), `mailGroupFileBtn`, `mailGroupFileList` | Grup kaydedilmeden yüklenemez (`alert.save_first`); >2 MB reddedilir (`app.js:3045`); `POST /api/trigger-mail/upload` ham gövde + `X-Group-Id`, `X-Filename` başlıkları (`app.js:3049-3056`); silme `POST /api/trigger-mail/file-delete {group,name}` |
| Alıcılar | `mailGroupRecipients`, `newRecipientEmail`, `addRecipientBtn` | regex ile e-posta doğrulaması (`app.js:2889`); liste CSV olarak kaydedilir |
| Düğmeler | `mailGroupCancelBtn`, `mailGroupSaveBtn`, `mailGroupDeleteBtn` | Kaydet: `POST /api/trigger-mail/save {id,name,subject,body,recipients}`; Sil: onay + `POST /api/trigger-mail/delete {id}` |

Grup kimliği: `currentMailGroupId` **dizideki indeks** olarak kullanılır (`mailGroupData[groupId]`, `app.js:2817-2819`) ama görüntüle düğmesine sunucunun `g.id`'si yazılır (`app.js:2787`). Sunucu id'leri 0..n−1 ardışık değilse yanlış grup açılır.

### 2.4 Sekme: System (`#panel-system`, `index.html:1018-1543`)

**Ana içerik** (`index.html:1019-1122`)

| Öğe | id | Davranış |
|---|---|---|
| Tema | `themeSelect` | anında uygulanır, `localStorage` |
| Dil | `languageSelect` | `de/en/es/fr/it/tr`; anında `applyLanguage` |
| Tarayıcı parolası | `securityCardBtn` | → `securitySubpage` |
| Uzaktan sıfırlama | `resetApiCardBtn` | → `resetApiSubpage` |
| HW güncelleme | `otaFirmwareBtn` | → `hwOtaSubpage` ("HW" metni çevrilmez, `index.html:1084`) |
| GUI güncelleme | `otaGuiBtn` | → `guiOtaSubpage` (2611'de kalkıyor, `00-surec.md`) |
| Yeniden başlat | `rebootBtn` | onay → `POST /api/device/restart {}` → alert → 5 s sonra reload (`app.js:2119-2134`) |
| Fabrika ayarları | `factoryResetBtn` | **iki** onay → `POST /api/device/factory-reset {}` → 10 s sonra reload (`app.js:2136-2152`) |

**Güvenlik** (`securitySubpage`, `index.html:1125-1225`) — yükle `GET /api/auth/status` (`app.js:2519-2538`).

| Alan | id | Not |
|---|---|---|
| Durum kutusu | `securityStatusText` | açık/kapalı; kapalıysa `info-box warning` |
| Mevcut parola | `currentPassword` | auth kapalıysa `disabled` |
| Yeni / doğrula | `newPassword`, `confirmPassword` | eşleşme, mevcut zorunlu (auth açıkken), **min 4 karakter** (`app.js:2566`; açıklama "en az 8 önerilir") ; boş yeni parola = parolayı kaldır (onaylı) |
| Parolayı değiştir | `changePasswordBtn` | `POST /api/auth/set {current,new}` → alert → yeniden yükle |
| Oturumu kapat | `logoutBtn` | yalnız auth açıkken görünür; `POST /api/auth/logout {}` → reload |
| Otomatik çıkış | `autoLogoutTime` | number 1–60 dk, varsayılan 10 (`ttl_min`) |
| Giriş koruması | `loginProtection` | toggle (`protect`); açıklama "5 denemede kilit" sabit metin |
| Kilit süresi | `lockoutTime` | number 5–60 dk, varsayılan 15 (`lock_min`) |

Kaydet (footer): `POST /api/auth/set {ttl_min, lock_min, protect:'true'|'false'}` → alt sayfayı kapat (`app.js:2540-2555`). Parola bu yoldan **gönderilmez**.

**Uzaktan sıfırlama API** (`resetApiSubpage`, `index.html:1228-1298`) — yükle `GET /api/reset/config` (`app.js:3166-3179`).

| Öğe | id | Not |
|---|---|---|
| Etkin | `resetApiEnabled` | toggle → **anında** `POST /api/reset/config {enabled}` (`app.js:3190-3199`) |
| Anahtar | `resetApiKey` | readonly; `has_key` ise `masked_key`, değilse "henüz üretilmedi" |
| Kopyala | `copyApiKeyBtn` | `navigator.clipboard` / `execCommand('copy')` (maskeli değeri kopyalar) |
| Yeni anahtar | `refreshApiKeyBtn` | onay → `POST /api/reset/generate {}` → `api_key` gösterilir, toggle açılır, panoya kopyalanır, alert |
| Kullanım | `.code-block` ×2 | `GET /api/reset?key=…` ve `curl http://<host>/api/reset?key=…`; `window.location.host` ile güncellenir (`app.js:3181-3187`) |

Footer Kaydet bu sayfada `saveAllSettings()` çalıştırır (ilgisiz).

**HW OTA** (`hwOtaSubpage`, `index.html:1301-1422`) — yükle `GET /api/fw-ota/info` (`app.js:1771-1792`).

| Öğe | id | Not |
|---|---|---|
| Sürüm / bölüm / uzak sürüm | `currentFirmwareVersion`, `runningPartition`, `remoteFirmwareVersion` (`fwRemoteVersionRow` gizli) | |
| İlerleme | `fwOtaProgressSection`, `fwOtaProgressBar`, `fwOtaProgressText` | 2 s'de bir `GET /api/fw-ota/status` (`app.js:1794-1867`) |
| Kaynak | radio `hwOtaSource` official/custom | custom → `hwOtaWarning` + `hwOtaCustomSection` (`otaServerUrl`, url) |
| Denetle | `checkFirmwareUpdateBtn` | onay → `POST /api/fw-ota/check {source, url?}` → polling |
| İndir-kur | `startFirmwareUpdateBtn` (`fwStartSection`) | onay → `POST /api/fw-ota/start {}` → polling |
| Yeniden başlat | `fwOtaRestartBtn` (`fwRestartSection`) | onay → `POST /api/device/restart` → 5 s reload |
| Geri al | `fwRollbackBtn` (`fwRollbackSection`, `can_rollback` ise) | onay → `POST /api/fw-ota/rollback {}` → alert → onaylı restart |

Durum makinesi (`d.state`): 1 CHECKING, 2 UPDATE_AVAILABLE, 3 NO_UPDATE (alert), 4 DOWNLOADING (`progress_pct`), 5 DONE (alert), 6 ERROR (alert + `message`).

**GUI OTA** (`guiOtaSubpage`, `index.html:1425-1541`) — yükle `GET /api/gui-ota/info` (`app.js:1984-2004`): `currentGuiVersion`, `activeGuiSlot` ("Slot A/B"), `inactiveSlotVersion`, `rollbackSection` (`inactive_has_data`). Kaynak radio + `guiRepoUrl` + `guiBranch` (select main/develop/release). `updateGuiBtn` → onay → `POST /api/gui-ota/start {source, repo_url?, branch?}` → 2 s polling `GET /api/gui-ota/status` (`state` 5 DONE → 2 s sonra reload, 6 ERROR, 7 NO_UPDATE). `rollbackGuiBtn` → `POST /api/gui-ota/rollback {}` → 1 s sonra reload. (2611'de bu alt sayfa kalkıyor.)

### 2.5 Sekme: Info (`#panel-info`, `index.html:1546-1718`)

**Ana içerik**: iki kart (`data-subpage='deviceInfoSubpage'`, `'logsSubpage'`) + "Yol haritası" 3 sabit not (`notes.*`).

**Cihaz bilgisi** (`deviceInfoSubpage`, `index.html:1607-1672`) — `GET /api/system/detail` (`app.js:1336-1407`) + 15 s'lik `/api/system/info` bazı satırları da yazar (`app.js:355-375`).

| Bölüm | Satırlar (id) |
|---|---|
| Donanım | `sysDeviceId` (info), `sysChip`, `sysCores`, `sysCpuFreq` (MHz), `sysFlashSize` (MB) |
| RAM | `sysRamTotal`, `sysRamFree`, `sysRamMinFree`, `ramBarFill` (%60 sarı, %80 kırmızı), `ramUsagePercent` |
| Dahili flash | `sysIntFlashTotal`, `sysIntFlashUsed`, `sysIntFlashFree` |
| Harici flash | `sysExtFlashSize` "(16 MB)" / "(Unavailable)", `sysSlotA` "kullanılan KB / toplam KB", `sysSlotB` "{size} KB (reserved)", `sysUserData` "{free} KB free / {total} KB" |
| Bağlantı | `sysWifiStatus` (yeşil/kırmızı), `sysWifiSsid`, `sysRssi` (dBm + Excellent/Good/Fair/Weak eşikleri −50/−60/−70), `sysIp`, `sysMdns`, `sysApStatus` (**sabit "Active"**), `sysApIp` (**sabit "192.168.4.1"**), `sysApClients`, `sysTxPower` "x dBm (max y dBm)" |
| Çalışma | `sysUptime` (`formatUptime`), `sysBootCount` "#n", `sysResetReason` (`causeLabel(last_cause,last_cat)` yoksa `reset_reason`), `sysPrevUptime`, `sysFirmware` "vX" |

**Günlükler** (`logsSubpage`, `index.html:1675-1716`) — `GET /api/logs` (`app.js:1463-1486`).

| Öğe | Davranış |
|---|---|
| Özet | `logBootCount`, `logLastCause`, `logPrevUptime`, `logShortBoots` |
| Filtre | `all / boot / timer / msg / wifi / err` (`logMatchesFilter`, `app.js:1452-1461`): `timer`=cat timer+relay, `msg`=mail+tg+api, `err`=lvl ERR/WARN + açılış kayıtlarında cat power/software |
| Liste | `renderLogs` (`app.js:1529-1559`): açılış kayıtları (`type:'boot'`) kart olarak (`renderBootRecord`: zaman, "Boot #n", kategori rozeti, neden, ipucu `causeHint`, "Ran for", son olay `ev`+`ev_dt`, WiFi/istemci/RSSI/TX/heap satırı, kısa açılış notu); olay satırları (`renderEventRecord`: zaman, seviye rozeti, kategori rozeti, mesaj); açılış numarası değişince ayırıcı başlık |
| Zaman | `ts > 1700000000` ise `GG.AA.YYYY SS:DD`, değilse `+çalışma süresi` (`app.js:1436-1445`) |
| Brownout ipucu | son olay `LOAD_PEAK_EVENTS` içindeyse ve `ev_dt ≤ 30` → `hint.brownout_load`, değilse `hint.brownout_other` (`app.js:1424-1434`) |
| Düğmeler | `logRefreshBtn` → `loadLogs`; `logClearBtn` → onay → `POST /api/logs/clear {}` |

---

## 3. API çağrıları tablosu

Tüm çağrılar `fetch` ile, JSON gövdeli POST'larda `Content-Type: application/json` (39 kez elle yazılmış). Hiçbir yerde `r.ok`/HTTP durumu denetlenmez; `r.json()` başarısızsa `catch`. GET'lerin tamamına yakını `.catch(function(){})` ile **sessizce** yutulur; POST'lar `alert()` ile bildirir (57 `alert`, 14 `confirm`).

| Yol | Yöntem | Ne zaman | Gönderilen gövde | Beklenen yanıt alanları | Hata işleme | Satır |
|---|---|---|---|---|---|---|
| `/ext/lang/<l>.json?v=<ts>` | GET | açılış (en son), dil değişince | — | düz sözlük | sessiz | `app.js:59` |
| `/api/system/info` | GET | açılış + **her 15 s** | — | `auth_enabled, ap_ssid, mdns, connected, sta_ssid, ip, device_name, ext_flash, slot_a_total, slot_a_free, user_data_free, user_data_total` | sessiz | `app.js:322`, `393` |
| `/api/auth/status` | GET | açılışta `auth_enabled` ise 1 kez; güvenlik alt sayfası açılınca | — | `enabled, logged_in, ttl_min, protect, lock_min` | sessiz | `app.js:328`, `2520` |
| `/api/smtp/get` | GET | açılış, her `saveAllSettings` sonrası | — | `configured, server, port, user` | sessiz | `app.js:399` |
| `/api/wifi/config` | GET | açılış, her `saveAllSettings` sonrası | — | `sta_ssid, backup_ssid, static{ip,gateway,subnet,dns}, backup_static{…}, tx_max, tx_adaptive, tx_now, ap_enabled` | sessiz | `app.js:422` |
| `/api/smtp/test` | POST | düğme | `{}` | `status, message` | `mail.test_fail` | `app.js:491` |
| `/api/timer/get` | GET | açılış, popup her açılışta, **her 10 s**, Save sonrası | — | `enabled, remaining_seconds, total_seconds, vacation_enabled, vacation_remaining, vacation_total, alarm_count` | sessiz | `app.js:864`, `871`, `105` |
| `/api/events` | SSE | açılışta; hata → 3 s sonra yeniden | — | `"connected"` ya da `/api/timer/get` ile aynı JSON | `onerror` → kapat + yeniden | `app.js:876-893` |
| `/api/timer/set` | POST | Durdur | `{action:'cancel'}` / `{action:'vacation_cancel'}` | `status` | sessiz | `app.js:902` |
| `/api/timer/set` | POST | Yeniden başlat | `{action:'restart'}` | `status, remaining_seconds, total_seconds` | sessiz | `app.js:940` |
| `/api/timer/set` | POST | Save (zincir 1) | `{unit:'gun'\|'saat'\|'dakika', value}` | `status` | sessiz | `app.js:1190` |
| `/api/timer/set` | POST | Save (zincir 2) | `{action:'set_alarms', count}` | `status` | sessiz | `app.js:1201` |
| `/api/timer/set` | POST | Save (zincir 3, koşullu) | `{action:'vacation_set', days}` / `{action:'vacation_cancel'}` | `status` | sessiz | `app.js:1215`, `1223` |
| `/api/smtp/save` | POST | Save (sunucu+gönderen doluysa) | `{server, port, user, api_key}` | — | sessiz | `app.js:1093` |
| `/api/wifi/connect` | POST | **her Save** | `{ssid, password, static_ip, gateway, subnet, dns}` | — | sessiz | `app.js:1116` |
| `/api/wifi/backup/save` | POST | **her Save** | aynı şema | — | sessiz | `app.js:1138` |
| `/api/wifi/ap/set` | POST | **her Save** | `{enabled:'true'\|'false'}` | — | sessiz | `app.js:1150` |
| `/api/wifi/txpower` | POST | **her Save** | `{max_dbm, adaptive:'true'\|'false'}` | — | sessiz | `app.js:1163` |
| `/api/relay/set` | POST | **her Save**, röle Kaydet, röle Test öncesi | `{duration, pulse, delay, inverted:'true'\|'false'}` | `status` | `alert.relay_save_err` | `app.js:1175`, `2372`, `2399` |
| `/api/relay/get` | GET | **hiç** | — | `inverted, delay, pulse, duration` | — | `app.js:2333` |
| `/api/relay/test` | POST | düğme | `{}` | — | `alert.relay_test_err` | `app.js:2412` |
| `/api/system/detail` | GET | cihaz bilgisi alt sayfası açılınca | — | `chip, cores, cpu_mhz, flash_kb, heap_total, heap_free, heap_min, flash_used_kb, slot_a_total, slot_a_free, slot_b_kb, user_total, user_free, mdns, rssi, ext_flash_mb, ap_clients, tx_now, tx_max, uptime, boot_count, last_cause, last_cat, reset_reason, prev_uptime, fw_version` | sessiz | `app.js:1337` |
| `/api/logs` | GET | günlük alt sayfası, Yenile, Temizle sonrası | — | `logs[], boot_count, last_cause, last_cat, prev_uptime, short_boots`; kayıt: `type, n, ts, up, cat, cause, ev, ev_dt, wifi, cl, rssi, tx, heap_free, heap_min, short, approx, lvl, msg` | `logs.error` metni | `app.js:1467` |
| `/api/logs/clear` | POST | onaylı düğme | `{}` | `status, message` | alert | `app.js:1579` |
| `/api/fw-ota/info` | GET | HW OTA açılınca | — | `current_version, running_partition, can_rollback, rollback_version` | sessiz | `app.js:1772` |
| `/api/fw-ota/status` | GET | **2 s** polling (check/start sonrası, uç durumda durur) | — | `state 1-6, message, progress_pct, remote_version` | catch → polling durur | `app.js:1797` |
| `/api/fw-ota/check` | POST | onaylı düğme | `{source, url?}` | `status, message` | alert | `app.js:1890` |
| `/api/fw-ota/start` | POST | onaylı düğme | `{}` | `status, message` | alert | `app.js:1915` |
| `/api/fw-ota/rollback` | POST | onaylı düğme | `{}` | `status, message` | alert | `app.js:1953` |
| `/api/device/restart` | POST | OTA sonrası / Sistem | `{}` ya da gövdesiz | — | alert | `app.js:1939`, `1963`, `2123` |
| `/api/device/factory-reset` | POST | iki onay | `{}` | — | alert | `app.js:2141` |
| `/api/gui-ota/info` | GET | GUI OTA açılınca | — | `active_version, active_slot, inactive_version, inactive_has_data` | sessiz | `app.js:1985` |
| `/api/gui-ota/status` | GET | **2 s** polling | — | `state 0-7, progress_pct, message` | sessiz | `app.js:2013` |
| `/api/gui-ota/start` | POST | onaylı düğme | `{source, repo_url?, branch?}` | `status, message` | alert | `app.js:2070` |
| `/api/gui-ota/rollback` | POST | onaylı düğme | `{}` | `status, message` | alert | `app.js:2096` |
| `/api/api-config` | GET | Alarm/LS API alt sayfası | — | `webhook{url,method,headers,body}, mqtt{url,topic,user,payload,has_pass}` | sessiz | `app.js:2435` |
| `/api/api-config` | POST | Kaydet, test öncesi | `{wh_url, wh_method, wh_headers, wh_body, mq_url, mq_topic, mq_user, mq_pass, mq_payload}` | `status, message` | alert | `app.js:2466` |
| `/api/api-config/test` | POST | test düğmeleri | `{target:'webhook'\|'mqtt'}` | `status, message` | durum satırı | `app.js:2492` |
| `/api/auth/set` | POST | güvenlik Kaydet | `{ttl_min, lock_min, protect:'true'\|'false'}` | `status, message` | alert | `app.js:2544` |
| `/api/auth/set` | POST | parola değiştir | `{current, new}` | `status, message` | alert | `app.js:2567` |
| `/api/auth/logout` | POST | düğme | `{}` | — | her durumda reload | `app.js:2588` |
| `/api/auth/login` | POST | giriş | `{password}` | `status:'ok'\|'locked', retry_s` | `auth.wrong/locked` | `app.js:2600` |
| `/api/telegram/get` | GET | Telegram alt sayfası | — | `configured, chat_id, message` | sessiz | `app.js:2630` |
| `/api/telegram/save` | POST | Kaydet, test öncesi | `{token, chat_id, message}` | `status, message` | alert | `app.js:2652`, `2680` |
| `/api/telegram/test` | POST | test | **gövdesiz, başlıksız** | `status, message` | alert | `app.js:2692` |
| `/api/early-mail/get` | GET | alt sayfa | — | `email, subject, body` | sessiz | `app.js:2711` |
| `/api/early-mail/save` | POST | Kaydet | `{email, subject, body}` | `status, message` | alert | `app.js:2730` |
| `/api/trigger-mail/get` | GET | **açılış**, alt sayfa, kaydet/sil sonrası | — | `groups[{id, name, subject, body, recipients(csv)}]` | sessiz | `app.js:2758`, `3248` |
| `/api/trigger-mail/save` | POST | grup Kaydet | `{id:'-1'\|'n', name, subject, body, recipients}` | `status, message` | alert | `app.js:2930` |
| `/api/trigger-mail/delete` | POST | onaylı Sil | `{id}` | `status, message` | alert | `app.js:2957` |
| `/api/trigger-mail/files?group=n` | GET | grup detayı açılınca | — | `files[{name, size}]` | liste boşalır | `app.js:2991` |
| `/api/trigger-mail/upload` | POST | dosya seçilince (her dosya ayrı) | ham dosya gövdesi; başlık `X-Group-Id`, `X-Filename` | `status, message` | alert | `app.js:3049` |
| `/api/trigger-mail/file-delete` | POST | sil ikonu (onaysız) | `{group, name}` | `status` | sessiz | `app.js:3072` |
| `/api/actions/get` | GET | **açılış** | — | `early_telegram, early_mail, early_api, trigger_mail, trigger_api, trigger_relay` (bool) | sessiz | `app.js:3121` |
| `/api/actions/save` | POST | kart tıklanınca anında | `{early_telegram:'true'\|'false', …}` (6 anahtar) | — | sessiz | `app.js:3156` |
| `/api/reset/config` | GET | alt sayfa | — | `enabled, has_key, masked_key` | sessiz | `app.js:3167` |
| `/api/reset/config` | POST | toggle anında | `{enabled:'true'\|'false'}` | — | sessiz | `app.js:3193` |
| `/api/reset/generate` | POST | onaylı düğme | `{}` | `status, api_key` | sessiz | `app.js:3221` |
| `/api/reset?key=…` | GET | GUI çağırmaz; kullanıcıya belgelenen dış uç | — | — | — | `index.html:1285` |

**Polling / sürekli trafik (güç ve WiFi TX açısından)**

| Kaynak | Aralık | Satır | Not |
|---|---|---|---|
| Yerel sayaç tik | 1 s | `app.js:777` | ağ yok |
| `GET /api/timer/get` | **10 s** | `app.js:871` | SSE varken bile ("SSE koparsa yedek"); sayfa arka plandayken de sürer |
| `GET /api/system/info` | **15 s** | `app.js:393` | yalnızca WiFi durum satırı ve AP bilgisi için |
| SSE `/api/events` | sürekli açık bağlantı | `app.js:877` | kopunca 3 s'de bir yeniden; 401'de sonsuz döngü |
| `GET /api/fw-ota/status`, `/api/gui-ota/status` | 2 s | `app.js:1796`, `2012` | yalnız OTA sırasında; alt sayfa kapanınca temizlenir (`app.js:1671-1672`) |
| Dil dosyası | her sayfa yüklemesi | `app.js:59` | `?v=Date.now()` önbelleği kırar (7–8 KB gz) |

Açılışta istek yükü: 5 statik (`index.html`, `style.css`, `app.js`, logo, lang) + 8 API (`system/info`, [`auth/status`], `smtp/get`, `wifi/config`, `timer/get`, `events`, `actions/get`, `trigger-mail/get`) + tarayıcının kendiliğinden istediği `/favicon.ico` (link yok). Sürekli: 15 s'de ~1,7 istek + açık SSE soketi.

---

## 4. Durum yönetimi

- **Global değişkenler** (114 üst düzey `var`, hepsi `window` kapsamında, modül/kapsülleme yok). Başlıcaları: sayaç `timerRemaining/timerTotal/timerEnabled/timerInterval/selectedUnit` (`app.js:516-520`), tatil `vacationEnabled/vacationRemaining/vacationTotal/vacationToggleDirty` (`523-525`, `744`), `alarmCount` (`528`), gezinme `currentTab/infoSubpageActive/systemSubpageActive/currentSystemSubpage/actionSubpageActive/currentActionView` (`177`, `1248`, `1634-1635`, `125-126`), günlük `logData/logFilter` (`1412-1413`), mail grupları `mailGroupData/currentMailGroupId/currentRecipients/currentGroupFiles` (`2753-2755`, `2981`), kimlik `authEnabled/authChecked/loginVisible` (`2517`, `320`, `15`), OTA `fwOtaPollingTimer/guiOtaPollingTimer`, dil `currentLang/langData`.
- **Kalıcı tarayıcı durumu**: yalnız `localStorage.ls_lang` ve `localStorage.ls_theme`. `sessionStorage` ve `document.cookie` kullanılmaz; oturum çerezi (varsa) sunucu tarafından yönetilir, JS görmez.
- **Sunucudan doldurma**: tek büyük yapılandırma nesnesi **yok**; 17 ayrı yükleyici fonksiyon, her biri kendi ucunu çağırır ve `getElementById` ile alanları yazar (`loadInfo, loadSmtpConfig, loadWifiConfig, fetchTimerState, loadSystemDetail, loadLogs, loadFwOtaInfo, loadGuiOtaInfo, loadRelayConfig(çağrılmıyor), loadApiConfig, loadSecurityConfig, loadTelegramConfig, loadEarlyMailConfig, loadTriggerMailGroups, loadGroupFiles, loadActionStates, loadResetApiConfig`). Bazıları alanı yalnız boşsa doldurur (`!el.value`, `app.js:429, 438, 451`), bazıları her seferinde ezer.
- **Kaydetme yolları** (dört farklı model yan yana):
  1. **Toplu** `saveAllSettings()` (`app.js:1074-1243`): SMTP (koşullu) + WiFi + Yedek WiFi + AP + TX gücü + Röle paralel; Timer → alarm → tatil sırayla; `Promise.all` → 300 ms bekle → `fetchTimerState, loadSmtpConfig, loadWifiConfig` → `closePopup`. Hangi sekmede olunduğuna bakmaz; **her Save'de 7–10 POST**.
  2. **Alt sayfa Kaydet**: `saveTelegramConfig`, `saveEarlyMailConfig`, `saveApiConfig`, `saveRelayConfig`, `saveSecuritySettings` — her biri kendi ucu, başarıda alt sayfayı kapatır.
  3. **Anında** (Save gerekmez): aksiyon kartı aktif/pasif (`app.js:2220`), uzaktan sıfırlama toggle (`3192`), tema/dil (yalnız localStorage).
  4. **Kendi düğmesiyle**: parola değiştir, mail grubu kaydet/sil, dosya yükle/sil, API anahtarı üret, günlük temizle, OTA.
- **Kaydedilmemiş değişiklik takibi**: yok. "İptal" formları geri almaz; girilen değerler bir sonraki açılışta hâlâ durur (yükleyiciler dolu alanı ezmediği için). Tek istisna `vacationToggleDirty`: popup açıkken SSE'nin tatil toggle'ını ezmesini engeller (`app.js:834-849`), popup açılınca sıfırlanır (`app.js:104`).
- **Gizli alanlar**: WiFi parolaları, SMTP api_key, MQTT parolası, Telegram token sunucudan **asla geri gelmez**; alanlar boş kalır ve Save'de boş string gönderilir. Sunucunun "boş = mevcut değeri koru" yapıp yapmadığı sunucu haritasında doğrulanmalı (§11).
- **Boolean'lar string olarak** gönderilir: `'true'/'false'` (`ap/set`, `txpower`, `relay/set`, `actions/save`, `auth/set`, `reset/config`); sayılar bazen string (`timerVal`, `alarmEl.value`, `id:String(...)`), bazen `parseInt`. Sözleşme tutarsız.
- **SSE ile yerel sayaç uzlaşması**: `applyTimerState()` (`app.js:822-861`) sunucu değerini alır, yerel 1 s tik `startLocalCountdown()` ile sürer; her 10 s'de `/api/timer/get` ile yeniden hizalanır.

---

## 5. i18n

- **Dosya yapısı**: 6 dil, her biri **düz** (iç içe değil) sözlük, 465 anahtar, `onek.ad` biçiminde 40 önek (`ota` 48, `device` 37, `security` 27, `logs` 25, `relay/mail_group` 23, `cause/alert` 22, …). Anahtar kümeleri 6 dilde **birebir aynı** (eksik/fazla yok). `{param}` yer tutucu kümeleri tutarlı. `tr` ve `de` diyakritikleri tam (ç ğ ı ö ş ü / ä ö ü ß).
- **Mekanizma** (`app.js:47-92`): `t(key, params)` — anahtar yoksa anahtarın kendisi döner; parametre değişimi `String.replace('{k}', v)` ile → **yalnız ilk geçtiği yer** değişir. `applyLanguage()` dosyayı `fetch` eder, sonra `[data-i18n]` → `textContent`, `[data-i18n-ph]` → `placeholder`, `[data-i18n-title]` → `title` (322 öznitelik, 290 benzersiz anahtar), `<html lang>` günceller ve 5 dinamik metni yeniler (`updateFooterButtons, updateTimerStatus, updateAlarmInfo, updateVacationUI, updateRelayCycleInfo`). JS'te `t()` ile 140 anahtar + dinamik önekler `cause.*`, `cat.*`, `hint.*`, `ev.*`, `logcat.*` (sunucu kodlarından üretilir; `hint.*` yalnız 8 neden için var, diğerleri boş döner — tasarım gereği).
- **Yükleme sırası**: `applyLanguage(currentLang)` `app.js`'in **son satırı** (`3251`); fetch dönene kadar HTML'deki İngilizce görünür (dil parıldaması). Dil değişince sayfa yenilenmez, DOM yerinde güncellenir.
- **HTML'de gömülü sabit metinler** (dil gelmeden görünen ya da hiç çevrilmeyen): `index.html:136` Türkçe `"17. 18. 19. saatte alarm calar"` (JS değiştirir; ama varsayılan Türkçe + diyakritiksiz); `:254` `WiFi`; `:1084` `HW`, `:1092` `GUI`; `:321`, `:402` `mDNS:`; `:504` `dBm`; `:791-795`, `:835-838` `1 sec…10 sec` seçenekleri; `:847` `"5 sec on - 5 sec off x 6 cycles = 60 seconds"` (JS değiştirir); `:1656` `192.168.4.1` sabit; `:1259` `ls_xxxxxxxxxxxx`; `:1285`, `:1294` kod blokları (`cihaz.local`, JS değiştirir); `:1035-1040` dil adları (doğal). Login kartı başlığı `LebensSpur` (`:1731`).
- **HTML metni ≠ en.json metni** (dil yüklenince HTML'deki yazı değişir, yani HTML'deki İngilizce "kaynak" değil, bayat kopya):
  - `ota.server_url`: HTML "Firmware Binary URL" (`:1383`), en "OTA Server URL".
  - `ota.custom_desc`: HW kartı HTML "Direct URL to firmware binary" (`:1360`), GUI kartı "Update from your own GitHub repo" (`:1487`) — **tek anahtar**; dil yüklenince HW kartı da "GitHub repo" der (yanlış).
  - `device.restart_reason`: HTML "Last Shutdown Cause" (`:1667`), en "Restart Reason".
  - `logs.filter_err`: HTML "Errors" (`:1693`), en "Warnings".
  - `logs.clear`: HTML "Clear" (`:1712`), en "Clear logs".
  - `security.new_pw_desc` "At least 8 characters recommended" vs JS sınırı 4 (`app.js:2566`); `security.too_short` "at least 4".
- **Kullanılmayan anahtar**: `time.sec` (tek; `time.sec_abbr` kullanılıyor).
- **Adlandırma tutarsızlığı**: `configure` öneksiz tek anahtar (`index.html:558` vd.); `common.*` yalnız 2 anahtar; `status.*` 3.
- **6 dilde aynı kalan 7 anahtar** (özel ad/kısaltma, kabul edilebilir): `action.telegram, api.webhook, api.url, api.mqtt_title, device.slot_a, device.ssid, device.mdns`. İngilizceyle aynı kalanlar: de 22, es 19, fr 26, it 27, tr 19 — çoğu "Port", "Chat ID", "Test", "h/m/s" gibi meşru; `tr`'de İngilizce kalmış göze çarpanlar: `action.alarm_api` "Alarm API", `action.ls_api` "LS API", `telegram.bot_token` "Bot Token", `api.broker_url` "Broker URL", `alarm.unit` "alarm", `device.ap_ip` "AP IP".
- **`\n` içeren metinler**: `confirm.factory1`, `alert.key_copied` (yalnız `alert/confirm` ile uyumlu; HTML'e taşınırsa kırılır).
- **Çift yer tutucu hatası**: `relay.cycle_text` 6 dilde de `{pulse}` iki kez; `t()` ilkini değiştirir → ekranda "5 sn açık - {pulse} sn kapalı …" görünür.

---

## 6. Görseller

| Öğe | Sayı / boyut | Not |
|---|---|---|
| Inline SVG (`index.html`) | **68** adet, 24,9 KB ham; **36** benzersiz gövde | Feather/Lucide tarzı 24×24 `stroke='currentColor' stroke-width='2'` |
| Tekrar edenler | dişli ×7 (`settingsBtn` + 6 kart, 6,6 KB), chevron-down ×5 (akordeon), refresh ×5, info-circle ×4, zarf ×3, uyarı üçgeni ×3, çip ×3, kilit ×3; ×2: geri-ok, çıkış, wifi, zincir, artı, kalkan, github, indir | `<symbol>/<use>` sprite ile ~18 KB ham → ~1 KB gz kazanç |
| JS içinde SVG | 2 (X ikonu, string olarak `renderRecipients`/`renderGroupFiles`, `app.js:2862`, `3022`) | |
| CSS içinde SVG | select oku data-URI ×2 (`style.css:1572`, `2427`) | |
| `pic/logo.png` | 400×400, 8-bit paletli, 11,0 KB (gz 10,8 KB — kazanç yok) | koyu temada |
| `pic/darklogo.png` | 400×400, 8-bit paletli, 10,2 KB | açık temada (`app.js:1598`) |
| Gösterim boyutu | `min(160px, 40vw, 40vh)` (`style.css:67`) | 400 px kaynak, 2× retina için 320 px yeterli |
| Favicon | **yok** | tarayıcı `/favicon.ico` ister; sunucunun ne döndürdüğü sunucu haritasında |
| Diğer | yok (font dosyası, arka plan görseli yok) | |

---

## 7. CSS

- **Yapı**: 3103 satır, 542 üst düzey blok, 242 sınıf, 1 `@media`. **CSS değişkeni yok** (0 `--x`); 101 farklı hex renk, 625 kullanım (en sık `#1a1a1a` ×74, `#e0e0e0` ×63, `#888` ×45, `#e8e8e8` ×42, `#666` ×37, `#2a2a2a` ×34, `#999` ×29). Gri tonları `#888/#999/#666/#aaa/#e0e0e0/#e8e8e8` arasında gelişigüzel.
- **Tema**: açık tema `body.light-theme .x` önekiyle **186 blok** (%34) — koyu kuralların elle yeniden yazılması; değişkenli tek tema ile ~2900 satırlık dosyanın yaklaşık 900 satırı gider.
- **`!important`**: 43 adet, çoğu `.action-btn` (`style.css:84-107`, `:1134-1146`) ve `.hidden` (`:1242`).
- **Bileşen sınıfları**: `.setting-section/.section-title/.section-desc`, `.setting-group/.setting-header/.setting-control`, `.toggle`, `.input/.select/.input-sm/.input-readonly`, `.btn-sys/.btn-primary/.btn-sys-danger/.btn-cancel/.btn-footer-*`, `.accordion`, `.info-box/.warning-banner`, `.action-card`, `.action-detail-view`, `.subpage`, `.system-info/.system-row`, `.log-*`, `.ota-status-card/.source-card`, `.timer-ring`, `.login-*`.
- **Ölü kurallar** (HTML'de ve JS'te, dinamik `'cat-'+`, `'log-'+`, `'type-'+`, `'lvl-'+` üretimleri de hesaba katılarak, hiç geçmeyen 19 sınıf → **25 blok**): `.modal-back` (`:178`, `:191`), `.modal-header-right` (`:196`), `.btn-start` (`:581`, `:596`, `:970`, açık tema `:3039`, `:3044`), `.status-inactive` (`:1290`), `.storage-bar-success` (`:1334`), eski günlük kategori adları `.log-system/.log-smtp/.log-error/.log-restart/.log-hw` (`:1370-1398`) ve `.type-system/.type-smtp/.type-error/.type-restart/.type-hw` (`:1435-1442`, açık tema `:2813-2817`, `:2835-2836`), `.checkbox-list/.checkbox-item` (`:1792-1823`, açık tema `:2717-2727`), `.view-footer-left/-right` (`:2017-2025`). Ek olarak işlevsel ölü: `.timer-ring.warning` (`:1058`, JS hiç set etmiyor), `.info-grid/.lbl` (`:1011-1020`, yalnız HTML'de olmayan `#sysInfo` için). Toplam ≈ **28 blok / 542 (%5)**; açık tema tekrarlarıyla ~35 blok.
- **Yinelenen seçiciler**: 16 (çoğu `@media` içinde meşru); gerçek tekrarlar: `body.light-theme .input-label` (`:2418` ve `:3071`), `.vacation-days-group` (`:2577`, `:3075`), `.pulse-duration-fields` (`:2965`, `:3066`).
- **Gereksiz**: genel `.hidden{display:none !important}` (`:1241`) varken `.static-ip-fields.hidden`, `.warning-banner.hidden`, `.action-detail-view.hidden`, `.info-main-content.hidden`, `.subpage.hidden`, `.pulse-duration-fields.hidden`, `.login-overlay.hidden`, `.login-error.hidden` (8 blok).
- **Inline style**: HTML'de 33 `style='…'` (boşluk `div`'leri `:539`, `:602`; `width:auto;margin-top:0` düğmeler; `display:none` başlangıç durumları).

---

## 8. Boyut

Ölçüm: `wc -c` ve `gzip -9 -c | wc -c`.

| Dosya | Ham (B) | gzip -9 (B) | Not |
|---|---|---|---|
| `index.html` | 99 856 | 13 874 | 25 KB'i SVG |
| `app.js` | 115 528 | 22 642 | |
| `style.css` | 57 243 | 9 229 | |
| `config.json` | 67 | 96 | gzip büyütüyor; ham gömülmeli |
| `lang/en.json` | 22 312 | 7 184 | |
| `lang/de.json` | 23 613 | 7 803 | |
| `lang/es.json` | 24 503 | 7 804 | |
| `lang/fr.json` | 25 071 | 7 910 | |
| `lang/it.json` | 24 112 | 7 617 | |
| `lang/tr.json` | 23 752 | 7 893 | |
| `pic/logo.png` | 11 003 | 10 845 | gzip'lenmemeli |
| `pic/darklogo.png` | 10 158 | 10 059 | gzip'lenmemeli |
| **Toplam** | **437 218** | **112 846** | flash'ta ≈ 110 KB |

Dağılım (gz): dil dosyaları 46,2 KB (%41), JS 22,6 (%20), PNG 20,9 (%19), HTML 13,9 (%12), CSS 9,2 (%8).

**Küçültme potansiyeli** (bu oturumda Python ile kaba ölçüm; `terser/csso` kurulu değil):

| Dosya | Kaba minify (yorum+boşluk) gz | Gerçek minifier tahmini gz | Kazanç |
|---|---|---|---|
| `index.html` | 12 210 | ~11,5 KB (+ SVG sprite ile ~10,5) | 2–3 KB |
| `app.js` | 17 694 | ~13–14 KB (isim kısaltma, `fetch` sarmalayıcısı) | 8–9 KB |
| `style.css` | 7 683 | ~6 KB (değişkenli tek tema ile ~5) | 3–4 KB |
| lang ×6 | — | anahtar kısaltma %10–15; yalnız ihtiyaç duyulan dilin indirilmesi zaten böyle | 5–7 KB |
| PNG | — | 320 px'e küçültme + `pngquant`/`oxipng` | 8–12 KB |

Toplamda ~110 KB → ~80 KB mümkün görünüyor; en büyük tek kalem **dil dosyaları** ve **PNG'ler**, kod değil.

---

## 9. Görülen sorunlar

Önem: **K** kritik (yanlış davranış/veri kaybı), **O** orta, **D** düşük.

| # | Önem | Sorun | Yer |
|---|---|---|---|
| 1 | **K** | `loadRelayConfig()` hiç çağrılmıyor; röle alt sayfası daima HTML varsayılanlarını gösterir (ters=hayır, gecikme=0, darbe kapalı, süre 1 dk). Daha kötüsü `saveAllSettings()` bu **varsayılanları** her Save'de `POST /api/relay/set` ile gönderir → kullanıcının röle ayarı, Timer sekmesinde Save'e basıldığında sessizce sıfırlanır. | `app.js:2205` (çağrı yok), `2332` (tanım), `1171-1181` |
| 2 | **K** | Footer "Kaydet", bağlam ne olursa olsun `saveAllSettings()` çalıştırır: Info sekmesinde, Reset API / HW OTA / GUI OTA alt sayfalarında, Connections'ta tek bir alanı değiştirince bile WiFi bağlan, yedek WiFi, AP, TX gücü, röle, timer, alarm, tatil hepsi yeniden gönderilir. `/api/wifi/connect` boş parolayla gider (parola alanı sunucudan dolmaz). Sunucu bunu "yeniden bağlan" sayıyorsa her Save WiFi'ı düşürebilir; timer `{unit,value}` her Save'de gidiyorsa sayaç yeniden başlıyor olabilir (sunucu haritasında doğrulanmalı). | `app.js:158-172`, `1074-1243`, `1101-1122` |
| 3 | **K** | Mail grubu detayında (`mailGroupDetailView`) footer "Kaydet" → `currentActionView` hâlâ `'triggerMailConfigView'` → `else closeActionSubpage()` → tüm düzenleme **kaydedilmeden** kapanır. Kullanıcı yalnız görünüm içindeki küçük "Save" düğmesini kullanmalı. | `app.js:160-165`, `2802-2836` |
| 4 | **O** | `t()` yer tutucuyu yalnız ilk geçtiği yerde değiştirir; `relay.cycle_text` 6 dilde de `{pulse}` iki kez → ekranda ham "{pulse}" kalır. | `app.js:52`, `lang/*.json relay.cycle_text` |
| 5 | **O** | Giriş ekranı açıkken arka planda `/api/timer/get` (10 s), `/api/system/info` (15 s) ve SSE (`EventSource` 401 → 3 s'de bir yeniden bağlanma) sürer; sarmalayıcı yalnız `fetch`'i kapsar. Parola korumalı cihazda, açık kalan sekme sürekli reddedilen istek üretir. | `app.js:29-39`, `871`, `393`, `887-890` |
| 6 | **O** | SSE varken 10 s'lik `/api/timer/get` ve 15 s'lik `/api/system/info` polling'i sürüyor; `system/info` yalnızca WiFi durum satırı için. Dakikada ~10 istek, sekme arka plandayken de (`visibilitychange` yok). Pille/zayıf besleme ile çalışan cihazda gereksiz TX. | `app.js:393`, `871` |
| 7 | **O** | HTML'de olmayan id'lere yazan ölü kod: `#sysInfo` bloğu (`innerHTML` dahil), `#vacationRemaining`, `#vacationStatusText`; boş fonksiyon `updateActionButtons()`. Tatil göstergesinin metni `{time:''}` ile üretildiği için kalan süre **hiç gösterilmez**. | `app.js:378-387`, `686`, `710-712`, `750`, `757`, `675-677`, `709` |
| 8 | **O** | `apPass` alanı JS tarafından hiç doldurulmuyor → AP parolası daima "-". `sysApStatus` ("Active") ve `sysApIp` ("192.168.4.1") sabit; AP kapalıyken yanlış bilgi. | `index.html:457`, `:1655-1656` |
| 9 | **O** | Overlay'e tıklayarak kapatma (`closePopup`) yalnız Info alt sayfasını kapatır; System/Actions alt sayfası açıkken kapatıp yeniden açınca carousel gizli, alt sayfa açık kalır. | `app.js:108-115` |
| 10 | **O** | Mail grubu kimliği dizi indeksi varsayılıyor (`mailGroupData[groupId]`) ama düğmeye sunucunun `g.id`'si yazılıyor; id'ler ardışık değilse yanlış grup açılır/silinir. | `app.js:2787`, `2796`, `2817-2819`, `2923`, `2960` |
| 11 | **O** | Doğrulama boşlukları: IP/maske/DNS biçimi denetlenmez; `vacationDays` klavyeden 1–60 dışına çıkabilir; röle süresi "sn 1–59" ipucu var ama kırpılmaz; "en çok 10 grup" uygulanmaz; SMTP port sınırı yok; webhook URL `type=url` ama `<form>` olmadığından tarayıcı doğrulaması çalışmaz. | `index.html:292-310`, `app.js:1057-1069`, `2302-2329`, `2927` |
| 12 | **O** | Tutarsız kaydetme modeli: tema/dil anında (yalnız tarayıcıda), aksiyon kartı ve uzaktan-sıfırlama toggle'ı anında (cihazda), parola kendi düğmesiyle, oturum ayarları footer ile, diğer her şey toplu. Kullanıcı neyin ne zaman kaydedildiğini bilemez; "İptal" hiçbir şeyi geri almaz. | §4 |
| 13 | **O** | SMTP "Test gönder" formdaki değil **kayıtlı** ayarı test eder (Telegram/webhook/röle testleri ise önce kaydeder) — davranış farkı. SMTP api_key alanı düz `text` tipinde (ekranda görünür). | `app.js:483-510`, `index.html:232` |
| 14 | **O** | HTML'deki İngilizce metinler ile `en.json` farklılaşmış (6 örnek, §5); `ota.custom_desc` tek anahtar olduğu için HW OTA özel kaynak kartı dil yüklenince yanlış açıklama ("GitHub repo") gösterir. | `index.html:1360`, `1487`; `en.json:186` |
| 15 | **O** | Erişilebilirlik: `aria-*`, `role`, `<label for>` yok; 32 yalnız-ikon düğme; ana ekrandaki 4 düğmede `title` bile yok; `<form>` olmadığından Enter çalışmaz (login hariç); odak yönetimi yok (modal açılınca odak taşınmaz, Esc kapatmaz). | `index.html:43-67`, sayımlar |
| 16 | **D** | XSS: sunucu verisi `innerHTML`'e girdiği 12 yerde `esc()` kullanılmış (iyi); kalanlar: `g.id` kaçışsız özniteliğe (`app.js:2787`), `t()` çıktısı ve `{n}` parametreleri kaçışsız (`app.js:1506`, `1545`), dil dosyası ve sunucu güvenilir kabul ediliyor. Kabul edilebilir ama yeni GUI'de `textContent`/DOM API tercih edilmeli. | `app.js:1488-1527`, `2782-2787` |
| 17 | **D** | Tekrarlayan kod: 39× elle `'Content-Type': 'application/json'`, 46× `return r.json()`, 4× "önce kaydet sonra test" kalıbı (röle, Telegram, webhook, MQTT), 2× özdeş statik-IP toggle (`app.js:268-286`), 2× özdeş OTA kaynak radio (`1735-1763`), 3× özdeş alt sayfa aç/kapa, 2× `updateCarousel` monkey-patch, 5× `POST /api/timer/set` çağrısı. | |
| 18 | **D** | Ölü/bayat CSS ≈ 28 blok (§7); `data-tab` ve `has-settings` öznitelik/sınıfları JS'te kullanılmıyor; `.timer-ring.warning` hiç set edilmiyor (uyarı rengi işlevi kayıp). | `style.css`, `index.html:84-88` |
| 19 | **D** | Dil dosyası `?v=Date.now()` ile her yüklemede indiriliyor; CSS/JS `?v=0.0.70` sürüm 3 yerde elle tutuluyor. | `app.js:59`, `index.html:7`, `:1739` |
| 20 | **D** | "Çıkış" düğmesi `about:blank`'e gider; oturumu kapatmaz, mobilde anlamsız. Durdur/yeniden başlat onaysız (ölü adam anahtarında "durdur" tetiklenmeyi iptal eder; bilinçli karar olmalı). | `app.js:963-968`, `898-958` |
| 21 | **D** | `document.execCommand('copy')` kullanımdan kalkmış; `confirm/alert` tabanlı diyaloglar (14 + 57) tarayıcı tarafından engellenebilir, çevrilemez başlık taşır. | `app.js:3210-3211` |
| 22 | **D** | `#timerValue` sınırı birimden bağımsız 1–60 → "60 dakika" ile "60 gün" aynı kutu; `timer.min_max` metni de sabit. Alarm `range max=10` HTML'de, JS `floor(süre/2)` ile yeniden yazar. | `index.html:116`, `app.js:976-990` |

---

## 10. Korunması gereken davranışlar (yeni GUI gereksinim listesi)

Test edilebilir cümleler; "cihaz" = ESP32, "kullanıcı" = tarayıcı. 2611'de kalkacak olanlar işaretli.

**Ana ekran ve sayaç**
1. Sayaç çalışmıyorsa ana ekranda logo, çalışıyorsa kalan süreyi gösteren dairesel ilerleme halkası görünür; halka kalan/toplam oranıyla dolar.
2. Kalan süre gün/saat/dakika (ve 1 saatin altında saniye) olarak, dil kısaltmalarıyla gösterilir ve her saniye yerel olarak azalır.
3. Sunucudan SSE ile gelen her sayaç durumu (düğme basımı, süre dolması, yeniden başlatma) ekrana anında yansır; SSE koparsa otomatik yeniden bağlanılır ve periyodik `timer/get` ile hizalanır.
4. "Yeniden başlat" düğmesi sayaçı onaysız tam süreye sıfırlar (check-in) ve ekran anında güncellenir.
5. "Durdur" düğmesi çalışan sayaçı; tatil aktifse tatili iptal eder (tatil iptalinde sayaç tam süreden yeniden başlar).
6. Tatil modu aktifken halka gri "WAITING" durumuna geçer, altta turuncu tatil göstergesi kalan tatil süresiyle görünür; tatil bitince sayaç kendiliğinden başlar.
7. Ayarlar düğmesi modalı açar; overlay'e tıklama ya da İptal kapatır.

**Giriş ve güvenlik**
8. Cihazda tarayıcı parolası ayarlıysa ve oturum yoksa açılışta giriş ekranı gelir; herhangi bir `/api/*` isteğinin 401 dönmesi de giriş ekranını getirir.
9. Yanlış parolada hata metni, kilitli hesapta "x saniye sonra tekrar" metni gösterilir; Enter ile giriş yapılır; başarılı girişte sayfa yeniden yüklenir.
10. Güvenlik sayfasında: parola değiştir (mevcut + yeni + doğrula; eşleşme ve asgari uzunluk denetimi), yeni parola boş bırakılarak parolayı kaldır (onaylı), oturum kapat, otomatik çıkış süresi (dk), giriş koruması aç/kapa, kilit süresi (dk); durum kutusu parolanın açık/kapalı olduğunu söyler.

**Timer sekmesi**
11. Birim (gün/saat/dakika) ve 1–60 değer seçilir; +/- ve klavye ile girilir, sınır dışı değer kırpılır.
12. Alarm sayısı 0..⌊süre/2⌋ kaydırıcısıyla seçilir; hangi birimlerde alarm çalacağı metinle önizlenir ("17. 18. 19. saatte…"); 0 = alarm kapalı.
13. Tatil modu toggle + 1–60 gün; kaydedilince cihazda tatil başlar; tatil aktifken kalan süre gösterilir ve toggle kapatılıp kaydedilince tatil iptal olur.
14. Kaydet sırası korunur: önce süre/birim, sonra alarm sayısı, sonra tatil (sunucu alarm sayısını süreye göre kırpar).

**Bağlantılar**
15. SMTP: sunucu, port (varsayılan 465), gönderen adres, uygulama parolası kaydedilir; parola sunucudan geri gösterilmez; "Test gönder" sonucu satır içinde yeşil/kırmızı gösterilir.
16. Birincil ve yedek WiFi: SSID, parola, isteğe bağlı statik IP (IP, ağ geçidi, maske, DNS); SSID boş kaydedilirse ayar silinir; bağlı SSID/IP durum satırında görünür; mDNS adı gösterilir.
17. AP modu açılıp kapatılabilir; kapatırken erişim kaybı uyarısı gösterilir; AP SSID/mDNS salt okunur gösterilir (parola da gösterilmeli — ALTE'de eksik).
18. WiFi TX gücü 8–20 dBm kaydırıcısı ve uyarlamalı güç toggle'ı; mevcut TX gücü gösterilir.
19. Yapılandırılmış akordeonlar görsel olarak ayırt edilir (başlık beyaz).

**Aksiyonlar**
20. Altı aksiyon kartı (erken: Telegram, hatırlatma maili, Alarm API; tetik: mail grupları, LS API, röle) tıklanarak aktif/pasif yapılır ve bu durum cihazda kalıcıdır; açılışta cihazdan okunur.
21. Her kartın dişlisi ilgili yapılandırmayı açar; Alarm API ve LS API aynı webhook/MQTT yapılandırmasını paylaşır.
22. Webhook: URL, yöntem (POST/GET/PUT), JSON başlıklar (geçersiz JSON reddedilir), gövde (`{status}`, `{time}`, `{device}` değişkenleri); MQTT: broker URL, konu, kullanıcı/parola (parola geri gösterilmez, "kayıtlı" ipucu), mesaj; her ikisi için "önce kaydet, sonra test" düğmesi ve satır içi sonuç.
23. Röle: ters çevirme (boşta/tetikte enerji durumu metinle gösterilir), başlangıç gecikmesi (0/1/2/3/5/10 s), aktif süre (sn/dk), darbe modu ve darbe süresi (1/3/5/10 s), döngü özeti ("x sn açık – x sn kapalı × n döngü = toplam"), "Test" (önce kaydet, sonra tetikle). Kayıtlı değerler açılışta **yüklenmeli** (ALTE'de yüklenmiyor).
24. Telegram: bot token (geri gösterilmez, "kayıtlı" ipucu), chat id, mesaj; "test mesajı gönder" önce kaydeder.
25. Erken uyarı maili: alıcı, konu, gövde.
26. Mail grupları: liste (ad + alıcı sayısı), yeni grup, grup detayı (ad zorunlu, konu, içerik, alıcı listesi ekle/çıkar ile e-posta doğrulaması, dosya ekleri yükle/sil — grup kaydedilmeden yükleme yok, 2 MB üst sınır), kaydet/iptal/sil (onaylı).

**Sistem**
27. Tema (koyu/açık) ve dil (de/en/es/fr/it/tr) anında uygulanır ve tarayıcıda hatırlanır; açık temada logo değişir.
28. Uzaktan sıfırlama API'si: aç/kapa, maskeli anahtar, kopyala, yeni anahtar üret (onaylı; üretince gösterilir, panoya kopyalanır, toggle açılır), `GET /api/reset?key=` kullanım örneği mevcut ana makine adıyla.
29. Firmware OTA: mevcut sürüm ve bölüm; kaynak resmi/özel URL (özel kaynakta uyarı); "güncelleme denetle" → varsa uzak sürüm + "indir-kur" → ilerleme çubuğu → "yeniden başlat"; geri alma varsa önceki sürümle "geri al". Tüm tehlikeli adımlar onaylı.
30. ~~GUI OTA (slot A/B, GitHub repo/branch, geri alma)~~ — 2611'de kalkıyor (`00-surec.md`).
31. Cihazı yeniden başlat (onaylı; sonra sayfa yeniden yüklenir) ve fabrika ayarları (iki onay; sonra yeniden yüklenir).

**Bilgi**
32. Cihaz bilgisi: kimlik, çip, çekirdek, MHz, flash; RAM toplam/boş/asgari boş + yüzde çubuğu (sarı/kırmızı eşikleri); dahili flash toplam/kullanılan/boş; harici flash boyutu ve kullanıcı verisi boş/toplam (~~slot A/B~~ 2611'de anlamsız); WiFi durumu/SSID/RSSI (sözel kalite)/IP/mDNS/AP durumu/AP istemci sayısı/TX gücü; çalışma süresi, açılış sayısı, son kapanış nedeni (çevrilmiş + kategori), önceki çalışma süresi, firmware sürümü.
33. Günlükler: özet (açılış sayısı, son kapanış, önceki çalışma, kısa açılışlar); filtreler (tümü/açılış/sayaç/mesaj/WiFi/hata); açılış kayıtları kategori rengiyle (güç/yazılım/kullanıcı/diğer), neden metni, brownout yorumu, "x süre çalıştı", son olay ve kaç saniye önce, WiFi/istemci/RSSI/TX/heap satırı; olay satırları zaman (SNTP varsa tarih, yoksa +çalışma süresi), seviye ve kategori rozeti, mesaj; açılış numarası değişince ayırıcı; yenile; temizle (onaylı).
34. Yol haritası notları (3 madde) — bilgi amaçlı; korunup korunmayacağı ürün kararı.

**Genel**
35. Altı dil; tüm görünür metinler, placeholder'lar ve `title`'lar çevrilir; dil değişince sayfa yenilenmeden güncellenir.
36. Tek sayfa, dış bağımlılık yok, çevrimdışı (yalnız cihaza bağlıyken) çalışır.
37. Mobil (≤480 px) düzen: tek sütun ayar grupları, küçük halka ve düğmeler, tam genişlik modal.
38. Sunucudan gelen her metin (SSID, günlük mesajı, dosya adı, grup adı) HTML'e kaçışlanarak yazılır.

---

## 11. Açık sorular

1. **Boş gizli alan semantiği**: `/api/wifi/connect {password:''}`, `/api/smtp/save {api_key:''}`, `/api/api-config {mq_pass:''}`, `/api/telegram/save {token:''}` geldiğinde sunucu mevcut değeri koruyor mu, siliyor mu? (Sunucu haritası.) Yanıt, §9 #2'nin ciddiyetini belirler.
2. `/api/timer/set {unit,value}` her Save'de gönderiliyor: sunucu bunu yalnız yapılandırma değişikliği mi sayıyor, yoksa çalışan sayaçı yeniden mi başlatıyor? `/api/wifi/connect` aynı SSID ile gelince yeniden bağlanıyor mu?
3. `/api/relay/set` her Save'de varsayılanlarla geliyor (#1): sahadaki cihazlarda röle ayarı gerçekten sıfırlanıyor mu, yoksa sunucu bir koruma yapıyor mu?
4. Mail grubu `id`: sunucu id'leri her zaman 0..n−1 ardışık mı (silme sonrası yeniden numaralanıyor mu)?
5. `/api/system/info` AP parolasını (`ap_pass`) döndürüyor mu? Döndürmüyorsa `apPass` alanı niye var?
6. SSE `/api/events` 401 durumunda ne yapıyor; oturum çerezi `EventSource` ile gidiyor mu?
7. Günlük kategorileri: sunucu hâlâ `system/smtp/error/restart/hw` adlarını üretiyor mu, yoksa bunlar tamamen eski mi (CSS'teki çift şema)?
8. Sunucu `/favicon.ico` için ne döndürüyor (404 maliyeti)?
9. Yeni GUI'de dil ve tema cihazda mı saklanmalı (her tarayıcıda aynı), yoksa ALTE gibi tarayıcıda mı?
10. "Çıkış" düğmesinin amacı ne (about:blank)? Yeni GUI'de "oturumu kapat" mı olmalı, kaldırılmalı mı?
11. Alarm sayısı üst sınırı ⌊süre/2⌋ ve "en çok 10 grup", "2 MB dosya" sınırları sunucuda da var mı, yoksa yalnız istemci mi?
12. Timer alt/üst sınırı birim başına ayrı mı olmalı (README: 1 dk – 60 gün)?
