# LebensSpur (LS) firmware

SmartKraft **LebensSpur** geri sayım cihazının ESP32-C6 firmware'i. Eski `esp32/LebensSpur/` web tabanlı kodu DONDURULMUŞ; yeni CLI + BLE + WiFi mimarisi sıfırdan bu klasörde inşa ediliyor.

İlgili belgeler:
- Faz 1 kapsam kararları: memory `ls-faz1-decisions`
- Genel dönüşüm planı: memory `LS CLI dönüşüm planı`
- Eski Web UI referansı: `<repo-root>/ls.html`
- Eski tam kod (sadece okuma, kod değişikliği YASAK): `esp32/LebensSpur/`

## Mimari özet

LebensSpur, kullanıcının önceden tanımladığı süre boyunca herhangi bir reset gelmezse tetiklenir ve N alarm + mail/telegram/webhook/röle çıkışı dizisini yürütür. Cihaz tamamen CLI üzerinden yapılandırılır; eski sistemdeki tarayıcı arayüzü kaldırıldı, yerine SKAPP (Flutter) GUI gelecek (Faz 2).

```
        ┌─────────────────────────────────────────────────────┐
        │ ls_timer_engine     (countdown + alarm + vacation) │
        │ ls_relay            (GPIO output, pulse, delay)    │
        │ ls_smtp             (SMTP client, STARTTLS/SSL)    │
        │ ls_mail_groups      (max 10 group → recipients)    │
        │ ls_reset_api        (inbound HTTP /api/reset)      │
        └────────────────────┬────────────────────────────────┘
                             │ event bus + CLI
        ┌────────────────────┴────────────────────────────────┐
        │ sk_core (zorunlu): identity + CLI + BLE GATT       │
        │ + WiFi STA + mDNS + TCP NDJSON + auth (ECDH+HMAC)  │
        │ + button + LED + sk_ota (HW firmware)              │
        └────────────────────┬────────────────────────────────┘
                             │
                             ▼
        sk_api (opsiyonel): outbound HTTP (Telegram, IFTTT, webhook)
```

## Klasör yapısı

```
ls/
├── README.md                 (bu dosya)
├── CMakeLists.txt            (ESP-IDF proje root)
├── sdkconfig.defaults
├── main/
│   └── main.c                (boot init + LS component wire-up)
├── sk_core/                  (sk_core kopyası — skapp-library ile aynı sürüm)
├── sk_api/                   (sk_api kopyası — outbound HTTP)
├── ls_timer_engine/          (Faz 1.1 — countdown core)
├── ls_relay/                 (Faz 1.2 — relay controller)
├── ls_smtp/                  (Faz 1.3 — SMTP client)
├── ls_mail_groups/           (Faz 1.3 — group manager)
├── ls_reset_api/             (Faz 1.4 — inbound HTTP)
├── protocol/
│   └── v1/                   (Faz 1.5 — SKAPP-LS sözleşmesi)
└── examples/
    └── minimal/              (skapp-library template, dokunulmaz)
```

`sk_core/` ve `sk_api/` skapp-library'den kopya. Geliştirme sırasında ikisi senkron tutulur; sk_core'da bir düzeltme olunca ls/ ve BF/ kopyaları güncellenir.

## Faz 1 alt fazları (CLI-only standalone)

| Alt-faz | Durum | İçerik |
|---|---|---|
| 1.0 | ✓ tamam | main.c LS'leştir + README |
| 1.1 | ✓ tamam | `ls_timer_engine` |
| 1.2 | ✓ tamam | `ls_relay` |
| 1.3 | ✓ tamam | `ls_smtp` + `ls_mail_groups` (attachment'sız) |
| 1.4 | ✓ tamam | `ls_reset_api` (inbound HTTP) |
| 1.5 | ✓ tamam | `protocol/v1/` (commands.json + events.json + errors.json + auth.md) |
| 1.6 | ✓ tamam | sk_core ts_unix replay fix + SMTP cert bundle + JSON escape |

## Build (ilk doğrulama)

```bash
. $IDF_PATH/export.sh      # ESP-IDF v5.x toolchain
cd esp32/ls
idf.py set-target esp32c6
idf.py build               # statik kontrol; warning ≤ 0 hedefli
idf.py -p COM5 flash monitor   # gerçek cihazda dene
```

Beklenen ilk boot çıktısı:
```
I (xxx) main: LebensSpur Faz 1 (1.6) up — id=LS-AXXXXXXXX
```

Sonra CLI üzerinde: `help` ile tüm topic'ler, `timer.status`, `relay.status`,
`smtp.get`, `mail.group.list`, `reset_api.status` gezilebilir.

Faz 1 bittiğinde cihaz USB CLI + BLE + TCP üzerinden tek başına tam işlevsel; SKAPP olmadan da kullanılabilir.

## Faz 2 (sonraki aşama)

SKAPP CLI-GUI entegrasyonu. LS ekranları `app/lib/features/devices/lebensspur/` altında iskelet halinde mevcut; Faz 1 sonrası bunlar CLI komutlarına bağlanır.

## Kapsamdan ÇIKARILAN eski özellikler

- MQTT (Home Assistant entegrasyonu için webhook yeterli)
- Public WiFi captive portal (kalıcı kurulum cihazı için niş)
- Browser password / auto logout (SKAPP-cihaz ECDH+HMAC zaten güvenli)
- Theme / Language seçici (SKAPP tarafına)
- GUI OTA / web interface A/B slot (Web UI yok)
- Mail attachments (Faz 1'de yok, sonra eklenebilir)

Detaylı gerekçe: memory `ls-faz1-decisions`.

## Lisans

SmartKraft açık kaynak firmware. Lisans detayı (AGPL-3.0 muhtemel) ürün çıkışında netleşecek.
