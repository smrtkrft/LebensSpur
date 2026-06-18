#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// =====================================================================
// ls_reset_api — inbound HTTP /api/reset?key=ls_xxx
//
// Tek endpoint, GET /api/reset. URL query'sinde `key` parametresi NVS'te
// saklı api_key ile eşleşmek zorunda. Match olunca cihaz timer'ı
// sıfırlanır (timer.reset.requested event'i `{"by":"api"}` ile yayılır;
// ls_timer_engine subscribe eder). Diğer tüm path'ler 404.
//
// Faz 1.4 kapsamı: yalnız port 80 HTTP (TLS yok). Bu LAN içinde ortak
// pattern; gerçek production'da SKAPP-cihaz hattı zaten BLE+ECDH+HMAC
// ile güvenli, bu endpoint zayıf-erişim API key fallback. Hardening
// Faz 1.6'da değerlendirilecek (TLS opsiyonel toggle, IP allowlist).
//
// CLI komutları (human / dotted canonical):
//   reset_api enable <on|off>     / reset_api.enable
//   reset_api key <ls_xxx...>     / reset_api.key
//   reset_api regen               / reset_api.regen
//   reset_api get                 / reset_api.get
//   reset_api status              / reset_api.status
// =====================================================================

#define LS_RESET_API_KEY_LEN  16   // "ls_" + 12 hex char + NUL

esp_err_t ls_reset_api_init(void);

bool ls_reset_api_is_enabled(void);

#ifdef __cplusplus
}
#endif
