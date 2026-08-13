# Güvenlik Bulgusu — LS webhook komutlarında `requires_auth` eksik

**Tarih:** 2026-08-03
**Bileşen:** LebensSpur firmware · `HW/components/ls_cli/ls_webhook_cmds.c`
**Önem:** YÜKSEK (fiziksel erişim → kalıcı uzaktan yetki yükselmesi)
**Durum:** DÜZELTİLDİ (2026-08-13) — `Neue/ls_reset_api/src/ls_reset_api.c`
**Not:** Bulgu eski ağaçtaki (`ALTE/HW`) `ls_webhook_cmds.c` üzerinden yazılmıştı;
aynı kusur güncel `Neue` ağacında `reset_api.*` adlarıyla duruyordu ve orada
kapatıldı. `reset_api.enable/key/regen/get` artık `requires_auth=true`;
`reset_api.status` bilinçli olarak açık (yalnız enabled/bound boolean'ları,
sır sızdırmaz). Derleme yeşil; §5 doğrulama adımları donanımda BEKLİYOR.
**Bulan:** SKAPP USB CLI güvenlik denetimi (Faz 2)

---

## 1. Özet

LS webhook CLI komutları `requires_auth` bayrağı taşımıyor. USB CLI imzasız
(unauthenticated) dispatch ettiği için, cihaza **birkaç saniyelik fiziksel
erişimi** olan biri kalıcı bir uzaktan API anahtarı üretebiliyor ve
sonrasında **ağ üzerinden ölü-adam anahtarı alarmını tekrar tekrar
bastırabiliyor**.

Diğer yıkıcı komutlardan (factory-reset, pairing) farkı: bunların cihaz
üstünde buton karşılığı var, yani fiziksel erişimi olan zaten yapabilir.
Bu komutun buton karşılığı **yok** — kısa süreli fiziksel temas, kalıcı ve
uzaktan kullanılabilir bir yetkiye dönüşüyor. LebensSpur bir ölü-adam
anahtarı ürünü olduğu için etki doğrudan ürünün varlık sebebine iniyor.

---

## 2. Kanıt zinciri (hepsi kodda doğrulandı)

| # | Halka | Kanıt |
|---|---|---|
| 1 | USB CLI satırları **imzasız** dispatch edilir | `skapp-library/sk_core/src/sk_cli.c:714` → `dispatch_common(line, writer, user, /*authenticated=*/false)` |
| 2 | Auth kapısı yalnız bayrak taşıyan komutlara uygulanır | `sk_cli.c:501` → `if (!cmd->requires_auth \|\| ctx->authenticated) return false;` |
| 3 | Webhook komutlarında bayrak **yok** | `ls_webhook_cmds.c:54-58` (komut tablosu, aşağıda) |
| 4 | `key.generate` taze anahtar üretir **ve webhook'u açık bildirir** | `ls_webhook_cmds.c:34-42` → cevap: `{"api_key":"...","enabled":true}` |
| 5 | Anahtar `/api/reset` uç noktasını açar | `HW/main/main.c:350` → `sk_webhook_register("/api/reset", HTTP_GET, "ls_reset", ...)` |
| 6 | Uç nokta sayacı sıfırlar | `HW/main/main.c:276` → `sk_event_bus_publish("timer.restart", "{\"source\":\"webhook\"}")` |

### Mevcut komut tablosu (`ls_webhook_cmds.c:54-58`)

```c
static const sk_cli_command_t s_cmds[] = {
    { .name="webhook.reset.config.get",  ... .handler=cmd_config_get },
    { .name="webhook.reset.config.set",  ... .handler=cmd_config_set },
    { .name="webhook.reset.key.generate",... .handler=cmd_key_generate },
    { .name="webhook.reset.key.clear",   ... .critical=true, .handler=cmd_key_clear },
};
```

Dördünde de `.requires_auth` yok.

---

## 3. Saldırı senaryosu

1. Saldırgan cihaza USB kablosu takar (SKAPP → Ayarlar → Geliştirici modu →
   USB Console, ya da düpedüz PuTTY — firmware ham metin de kabul eder).
2. `webhook.reset.key.generate` yazar.
   Cihaz: `{"api_key":"ls_xxxxxxxx","enabled":true}` — taze anahtar, üstelik
   sahibi kapatmış olsa bile webhook açık raporlanıyor.
3. Saldırgan kabloyu çıkarır, gider. Toplam süre: ~10 saniye.
4. Sonrasında cihazın erişilebildiği herhangi bir yerden:
   `GET http://<cihaz>/api/reset?key=ls_xxxxxxxx`
   → `timer.restart` → geri sayım sıfırlanır.
5. Bunu istediği sıklıkta tekrarlar: **alarm hiç tetiklenmez.**

Sahibin bunu fark etmesi için webhook durumuna bakması gerekir; anahtar
sessizce üretildiği ve `config.get` yalnız maskeli önek gösterdiği için
tespit kolay değildir.

### Yan etkiler

- `webhook.reset.config.set` de korumasız → saldırgan webhook'u **açabilir**
  (sahibi kapatmış olsa bile).
- `webhook.reset.key.clear` `.critical=true` taşıyor ama `requires_auth`
  taşımıyor. `.critical` bir yetki kontrolü **değildir**: `device.confirm-token`
  komutu da korumasız olduğu için token her taşıyıcıdan istenerek alınabiliyor
  ve token komuta bağlı değil. Yani saldırgan sahibin meşru anahtarını da
  silebilir (entegrasyonu bozar).
- `webhook.reset.config.get` maskeli anahtar döndürüyor; sabit uzunlukta maske
  yerine önek sızdırdığı için ayrıca gözden geçirilmeli.

---

## 4. Düzeltme

```c
static const sk_cli_command_t s_cmds[] = {
    { .name="webhook.reset.config.get",  ..., .requires_auth=true, .handler=cmd_config_get },
    { .name="webhook.reset.config.set",  ..., .requires_auth=true, .handler=cmd_config_set },
    { .name="webhook.reset.key.generate",..., .requires_auth=true, .handler=cmd_key_generate },
    { .name="webhook.reset.key.clear",   ..., .requires_auth=true, .critical=true, .handler=cmd_key_clear },
};
```

**Neden dördü birden:** `api.*` komutlarının tamamı (`sk_api.c`) zaten
`requires_auth` taşıyor — gerekçe tam olarak bu: uzaktan erişim üreten ya da
uzaktan erişimi etkileyen hiçbir şey imzasız kanaldan yapılmamalı. Webhook
komutları aynı sınıfa girer.

**Etkisi:** Bu komutlar USB CLI'dan `ERR_NOT_AUTHENTICATED` dönecek; SKAPP
konsolu bunu zaten açıklayıcı bir kartla gösteriyor ("bu komut bonded BLE/WiFi
oturumu gerektiriyor"). Meşru kullanım eşleşmiş SKAPP oturumundan devam eder.

---

## 5. Doğrulama adımları (düzeltmeden sonra)

1. USB CLI'dan `webhook.reset.key.generate` → `ERR_NOT_AUTHENTICATED` dönmeli.
2. USB CLI'dan `webhook.reset.config.set --enabled 1` → `ERR_NOT_AUTHENTICATED`.
3. Eşleşmiş SKAPP (BLE/WiFi bonded oturum) üstünden aynı komutlar → çalışmalı.
4. `/api/reset?key=<eski anahtar>` → hâlâ çalışmalı (mevcut entegrasyonlar
   bozulmamalı; değişiklik yalnız anahtar ÜRETİMİNİ kapatıyor).

---

## 6. Kapsam notu

Bu bulgu SKAPP uygulaması tarafında **değil**, cihaz firmware'inde. SKAPP
tarafı ayrı bir denetimden geçti ve orada bulunan tek gerçek açık
(yıkıcı komut onay diyaloğunun cihazın gönderdiği metinle etiketlenmesi)
kapatıldı — artık diyalog kullanıcının yazdığı komutu gösteriyor, cihazın
yankısı yalnız tutarsızlık logu için kullanılıyor.

Ayrıca SKAPP'taki üç kaynak dosyanın yorumları (`usb_cli_transport.dart:13-15`,
`usb_console_providers.dart:19-20`, `console_message_view.dart:240-244`)
"`userdata.*` / `secure.*` requires_auth olduğu için USB'den engellidir"
diyor. Bu tree'de öyle komutlar **yok**; gerçek imzasız USB yüzeyi çok daha
geniş (`device.factory-reset`, `device.restart`, `wifi.connect/forget`,
`pairing.start`, `auth.passphrase.*`, `auth.token.rotate`, `ota.*` ve tüm LS
cihaz komutları). Çoğunun buton karşılığı olduğu için yetki yükselmesi
değiller — ama bu yorumların yüzeyi olduğundan dar göstermesi, bu bulgunun
uzun süre görünmez kalmasının sebebi. Yorumlar da düzeltilmeli.
