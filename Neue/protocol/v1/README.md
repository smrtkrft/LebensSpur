# LebensSpur Protocol v1

SKAPP ↔ LS arasındaki **LS-özgü** sözleşme. sk_core'un sağladığı zorunlu komut/event/error katmanı her SmartKraft cihazda aynıdır ve bu klasörde tekrarlanmaz.

## Dosyalar

| Dosya | İçerik |
|---|---|
| `commands.json` | LS CLI komutları: timer, vacation, relay, smtp, mail.group, reset_api |
| `events.json`   | LS event şemaları: timer.*, relay.*, smtp.*, mail_groups.fire |
| `errors.json`   | LS yüzeyinden dönebilen ERR_* kodlarının özeti (tam katalog sk_core'da) |
| `auth.md`       | Pairing + handshake + HMAC akışı; sk_core'a referans |

## Kullanım

SKAPP'ın LS ekran setini (`app/lib/features/devices/lebensspur/`) implement ederken bu dosyalar single source of truth'tur. Bir komut imzasını veya event payload'ını değiştirmeden önce burayı güncelle, sonra Dart tarafını uygula.

## Versiyon

**1.0.0** (2026-05-20) — Faz 1.5'te ilk taslak.

Her sürüm semver: yeni komut/event/error eklenmesi MINOR; var olan field'ın type veya anlamı değişirse MAJOR.
