# LebensSpur v1 — Auth Reference

SKAPP ↔ LS arasındaki tüm authentication, pairing, handshake ve HMAC mekanizması **sk_core**'da yaşar. Bu dosya LS-özgü hiçbir ekleme yapmaz; sadece SKAPP geliştiricisinin doğru yere bakmasını kolaylaştırır.

## Referans dosyalar

| Konu | Dosya | Public API |
|---|---|---|
| ECDH eşleşme + Mutual C-R | `sk_core/src/sk_auth_ecdh.c` + `sk_auth_handshake.c` | `sk_auth_init()` |
| Per-message HMAC envelope | `sk_core/src/sk_auth_hmac.c` | (internal, transport tarafından kullanılır) |
| Confirm token (critical commands) | `sk_core/src/sk_auth_confirm.c` | `sk_cli_confirm_token(ctx)` |
| Pairing button gate | `sk_core/src/sk_button.c` (SHORT_PRESS → BLE adv on) | `sk_button_init()` |
| Bond storage (max 8 peer) | `sk_core/src/sk_auth.c` (BOND_SLOT_COUNT) | `sk_auth_*` family |
| Secure session glue | `sk_core/src/sk_secure_session.c` | `sk_cli_dispatch_line_authenticated()` |

## Akış özeti (SKAPP ile LS eşleşmesi)

1. Kullanıcı LS butonuna **kısa basar** → BLE advertising açılır (60 sn).
2. SKAPP BLE tarama yapar, `LS-AXXXXXXXX` advertising'ini görür.
3. SKAPP ECDH public key gönderir; LS kendi public key'ini cevaplar.
4. Karşılıklı C-R: SKAPP nonce → LS HMAC(shared_secret, nonce) → SKAPP verify.
5. Mutual handshake tamam → LS bond slot'una SKAPP UUID + HMAC key kaydedilir.
6. Sonraki tüm komutlar **NDJSON envelope** içinde HMAC ile imzalanır.
7. `requires_auth = true` olan komutlar (örn. `api.*` outbound HTTP setleri) yalnız authenticated transport'tan kabul edilir; USB CLI bu rastla `ERR_NOT_AUTHENTICATED` döner.

## LS-özgü dikkat noktaları

- **`/api/reset?key=...`** endpoint'i (ls_reset_api) sk_core auth zincirinin DIŞINDADIR. API key tek faktör; LAN içinde herkes erişebilir. SKAPP-cihaz hattının yedek erişim mekanizması olarak konumlandırılmıştır.
- **`ls_smtp` ve `ls_mail_groups`** komutları (smtp.*, mail.*) `requires_auth = false` — USB CLI'dan da konfigüre edilebilir (cihaz testi sırasında pratik). Token (api_key) NVS'te plaintext; cihaz NVS encryption ile korunmazsa fiziksel erişim sızıntı riski.
- **`timer.reset --by api`** sadece event etiketi; gerçek auth ls_reset_api endpoint'inde doğrulanır.

## TLS / sertifika konuları

- **Outbound TLS** (sk_api, ls_smtp): ESP-IDF cert bundle. ls_smtp Faz 1.3 ilk implementasyonunda `skip_common_name = true` (hostname doğrulama yok) → Faz 1.6 hardening'inde kapatılacak.
- **Inbound TLS** (ls_reset_api): YOK. HTTP plain text. Production'da opsiyonel TLS / IP allowlist Faz 1.6'da değerlendirilecek.

## Versiyonlama

Protokol versiyonu **1.0.0** — yeni komut/event/error eklemek MINOR bump. Var olan field'ın type/anlam değiştirilmesi MAJOR bump. `sk_capabilities`'in runtime sürümü ile bu doküman birlikte güncellenir.
