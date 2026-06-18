#pragma once

// Two-step OTA — version-aware, manual user trigger.
//
//   1. sk_ota_check()      fetch manifest.json, parse version + URL + sha256,
//                          compare with current via semver. State transitions
//                          to UPDATE_AVAILABLE or NO_UPDATE; the URL + hash
//                          are cached internally for the next step.
//   2. sk_ota_start()      use the cached URL, download via esp_https_ota,
//                          write A/B partition, optionally verify sha256,
//                          set boot partition, reboot.
//
// Manifest format (JSON, hosted on GitHub Releases or any HTTPS server):
//   {
//     "version": "1.2.3",
//     "url":     "https://.../firmware.bin",
//     "sha256":  "abc...64hex"      (optional — verified if present)
//   }
//
// Recommended URL pattern (GitHub Releases):
//   https://github.com/<owner>/<repo>/releases/latest/download/manifest.json
// GitHub auto-redirects "latest" so the URL stays stable across releases —
// the manifest itself points to the actual firmware.bin download.

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SK_OTA_IDLE,
    SK_OTA_CHECKING,
    SK_OTA_UPDATE_AVAILABLE,
    SK_OTA_NO_UPDATE,
    SK_OTA_DOWNLOADING,
    SK_OTA_DONE,
    SK_OTA_ERROR,
} sk_ota_state_t;

typedef struct {
    sk_ota_state_t state;
    int            progress_pct;
    char           message[128];
    char           current_version[16];
    char           remote_version[16];
    char           remote_url[192];      // cached from last successful check
    bool           has_sha256;            // true if manifest provided sha256
    char           remote_sha256[65];     // lowercase hex, NUL-terminated
} sk_ota_status_t;

typedef struct {
    const char *fw_version;        // semver "X.Y.Z" — running firmware
    const char *manifest_url;      // HTTPS URL to manifest.json (NULL = OTA disabled)
} sk_ota_cfg_t;

esp_err_t sk_ota_init(const sk_ota_cfg_t *cfg);

// Async manifest fetch + version compare. Result delivered as state change
// + `ota.fw.state` event. Pass NULL to use the configured manifest_url.
esp_err_t sk_ota_check(const char *custom_manifest_url);

// Async download + flash + reboot.
//   custom_url == NULL  → use cached URL from last check; requires
//                         state == SK_OTA_UPDATE_AVAILABLE (else rejected).
//   custom_url != NULL  → override cached URL (manifest-less APP-driven flow).
//                         sha256 verification is skipped in this case.
esp_err_t sk_ota_start(const char *custom_url);

esp_err_t sk_ota_rollback(void);
bool      sk_ota_can_rollback(void);
bool      sk_ota_is_busy(void);
void      sk_ota_get_status(sk_ota_status_t *out);
const char *sk_ota_running_label(void);

// String form of the state enum — useful for `ota.fw.status` JSON output
// and logs. Returns "?" for out-of-range values.
const char *sk_ota_state_str(sk_ota_state_t s);

#ifdef __cplusplus
}
#endif
