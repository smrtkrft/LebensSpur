// =====================================================================
// LebensSpur — main.c
// =====================================================================
// SmartKraft LebensSpur countdown cihazı. Boot sırası:
//   1) NVS init
//   2) sk_core (identity, CLI, event bus, errors, capabilities)
//   3) USB CLI transport (Serial/JTAG)
//   4) sk_auth + sk_passphrase (pairing/handshake/HMAC/confirm tokens)
//   5) sk_button (BLE-on / restart / factory-reset gestures)
//   6) WiFi STA + mDNS + BLE GATT + TCP NDJSON
//   7) sk_ota (HW firmware OTA; manifest URL boş → runtime'da disabled)
//   8) sk_api (outbound HTTP — Telegram, IFTTT, generic webhook)
//   9) Cihaza özgü ls_* component init'leri (timer, relay, smtp, mail
//      groups, reset_api) — Faz 1.1+ alt fazlarında eklenecek
//
// EDIT bloklarındaki sabitler dışında bu dosyada cihazlar arası ortak
// init mantığı yaşar. Cihaza özgü her şey "Device-specific code" işaretine
// kadar olan bölümün altına yazılır.
//
// Faz 1.0 — sadece kimlik + ortak altyapı. Component'lar Faz 1.1'den itibaren.
// =====================================================================

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "sk_core.h"   // umbrella — pulls every public sk_core API (incl. sk_ota)
#include "sk_api.h"    // outbound HTTP (Telegram, IFTTT, generic webhook)
#include "sk_log.h"    // structured event log baseline (boot reason etc.)
#include "ls_timer_engine.h"  // Faz 1.1 — countdown core
#include "ls_relay.h"         // Faz 1.2 — relay output
#include "ls_smtp.h"          // Faz 1.3 — SMTPS client
#include "ls_mail_groups.h"   // Faz 1.3 — mail group manager
#include "ls_reminder.h"      // Faz 1.7 — early-warning reminder mail (timer.alarm)
#include "ls_reset_api.h"     // Faz 1.4 — inbound HTTP reset endpoint

// === EDIT [Identity] =================================================
//
// 2-char uppercase ASCII (A-Z) device type code. "LS" = LebensSpur.
#define SK_DEVICE_TYPE_PREFIX   "LS"

// Single uppercase letter (A-Z) for hardware revision. 'A' = ilk PCB.
#define SK_HW_REV               'A'

// Human-readable product name shown in CLI banner / help header:
//   LS-A06TMFSQT - SmartKraft LebensSpur v0.1.0 (...)
// Brand "SmartKraft" is fixed in sk_core; this is the per-project part.
#define SK_PRODUCT_NAME         "LebensSpur"

// Firmware version — semver. Bump on every release.
#define SK_FW_VERSION           "0.1.0"

// Optional build tag (git sha, CI build number). NULL = none.
#define SK_BUILD_INFO           NULL
//
// === EDIT [Control button] ===========================================
//
// Tek donanım butonu sk_button gesture'ları sürer (BF ile aynı kalıp):
//   SHORT_PRESS  → BLE advertising aç (eşleşme penceresi)
//   LONG_PRESS   → device.restart (3 sn basılı tut, sk_button default)
//   MULTI_TAP    → factory reset (5× hızlı tap, sk_button default)
//
// NOT: LS PCB rev-A pin map'i netleşmemiş. Şimdilik ESP32-C6 DevKitC-1
// boot button (GPIO 9) varsayılan; PCB yerleşimi belirlenince güncellenir.
#define SK_BUTTON_GPIO          9
#define SK_BUTTON_ACTIVE_LOW    1
//
// === EDIT [Relay] ====================================================
//
// LS röle çıkış GPIO'su. NOT: LS PCB rev-A pin map'i netleşmemiş;
// şimdilik eski LS PCB'sindeki D8 (GPIO 19) varsayılan. PCB son
// halini alınca güncellenir. Kullanıcı `relay gpio <N>` ile
// runtime'da değiştirebilir (NVS'e yazılır).
#define LS_RELAY_DEFAULT_GPIO   19
//
// === EDIT [Wireless] =================================================
//
// TCP NDJSON port — also announced via mDNS as `_skapp._tcp`.
#define SK_TCP_PORT             8080
//
// === EDIT [Optional features] ========================================
#define SK_API_ENABLE           1   // Telegram, IFTTT, generic webhook
//
// === EDIT [sk_ota] ===================================================
//
// Manifest-driven OTA. Boş URL → sk_ota_init no-op (runtime'da disabled).
// GitHub Releases pattern:
//   https://github.com/<owner>/<repo>/releases/latest/download/manifest.json
#define SK_OTA_ENABLE           1
#define SK_OTA_MANIFEST_URL     ""
// =====================================================================

// Compile-time guards — typos in EDIT block are caught now, not at first boot.
_Static_assert(sizeof(SK_DEVICE_TYPE_PREFIX) == 3,
               "SK_DEVICE_TYPE_PREFIX must be exactly 2 ASCII characters");
_Static_assert(SK_HW_REV >= 'A' && SK_HW_REV <= 'Z',
               "SK_HW_REV must be an uppercase letter 'A' through 'Z'");
_Static_assert(SK_BUTTON_GPIO >= 0,
               "SK_BUTTON_GPIO must be a valid GPIO number");
_Static_assert(SK_TCP_PORT > 0 && SK_TCP_PORT < 65536,
               "SK_TCP_PORT must be a valid TCP port (1-65535)");

static const char *TAG = "main";

// CLI banner status line — fills the parenthesized portion of:
//   LS-A06TMFSQT - SmartKraft LebensSpur v0.1.0 (timer: countdown, wifi: connected)
static size_t ls_status_line(char *out, size_t cap)
{
    size_t off = 0;
    off += (size_t)snprintf(out + off, cap - off,
                            "timer: %s", ls_timer_engine_state_str());

    sk_wifi_status_t w;
    sk_wifi_status(&w);
    if (off < cap) {
        off += (size_t)snprintf(out + off, cap - off, ", wifi: %s",
                                w.connected ? "connected" : "off");
    }
    return off;
}

void app_main(void)
{
    // ESP-IDF stack INFO chatter'ı sustur — monitor'da gerçek olaylar
    // boğulmasın. WARN/ERROR gerçek sorunları yakalar.
    esp_log_level_set("NimBLE",    ESP_LOG_WARN);
    esp_log_level_set("wifi",      ESP_LOG_ERROR);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("BLE_INIT",  ESP_LOG_WARN);
    esp_log_level_set("pp",        ESP_LOG_WARN);
    esp_log_level_set("net80211",  ESP_LOG_WARN);
    esp_log_level_set("phy",       ESP_LOG_WARN);
    esp_log_level_set("phy_init",  ESP_LOG_WARN);

    // NVS bootstrap — almost every sk_* library uses NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // sk_core: identity, CLI, event bus, errors, capabilities.
    ESP_ERROR_CHECK(sk_core_init(&(sk_core_cfg_t){
        .device_type_prefix = SK_DEVICE_TYPE_PREFIX,
        .hw_rev             = SK_HW_REV,
        .fw_version         = SK_FW_VERSION,
        .build_info         = SK_BUILD_INFO,
    }));

    // Per-project banner identity: brand line and dynamic status.
    sk_core_set_product(SK_PRODUCT_NAME, NULL);  // version falls back to fw_version
    sk_core_set_status_provider(ls_status_line);

    // Boot reason event — sk_log queue is up after sk_core_init (which
    // calls sk_baseline_init → sk_log_init). POWERON/EXT/SW are clean
    // boots; PANIC/WDT/BROWNOUT mean the previous run died unexpectedly
    // and the user (or SKAPP) should see it in logs.get.
    {
        esp_reset_reason_t rr = esp_reset_reason();
        const char *rr_name = "UNKNOWN";
        bool       unclean  = false;
        switch (rr) {
            case ESP_RST_POWERON:    rr_name = "POWERON";    break;
            case ESP_RST_EXT:        rr_name = "EXT";        break;
            case ESP_RST_SW:         rr_name = "SW";         break;
            case ESP_RST_DEEPSLEEP:  rr_name = "DEEPSLEEP";  break;
            case ESP_RST_PANIC:      rr_name = "PANIC";      unclean = true; break;
            case ESP_RST_INT_WDT:    rr_name = "INT_WDT";    unclean = true; break;
            case ESP_RST_TASK_WDT:   rr_name = "TASK_WDT";   unclean = true; break;
            case ESP_RST_WDT:        rr_name = "WDT";        unclean = true; break;
            case ESP_RST_BROWNOUT:   rr_name = "BROWNOUT";   unclean = true; break;
            case ESP_RST_SDIO:       rr_name = "SDIO";       break;
            // ESP-IDF v5.x ek reset türleri (#ifdef korumalı).
#ifdef ESP_RST_USB
            case ESP_RST_USB:        rr_name = "USB";        break;
#endif
#ifdef ESP_RST_JTAG
            case ESP_RST_JTAG:       rr_name = "JTAG";       break;
#endif
#ifdef ESP_RST_EFUSE
            case ESP_RST_EFUSE:      rr_name = "EFUSE";      unclean = true; break;
#endif
#ifdef ESP_RST_PWR_GLITCH
            case ESP_RST_PWR_GLITCH: rr_name = "PWR_GLITCH"; unclean = true; break;
#endif
#ifdef ESP_RST_CPU_LOCKUP
            case ESP_RST_CPU_LOCKUP: rr_name = "CPU_LOCKUP"; unclean = true; break;
#endif
            default: break;
        }
        if (unclean) {
            SK_LOG_W("boot", "unclean", "reset=%s fw=%s", rr_name, SK_FW_VERSION);
        } else {
            SK_LOG_I("boot", "up", "reset=%s fw=%s", rr_name, SK_FW_VERSION);
        }
    }

    // Topic catalog — registered ONCE here, in the order categories should
    // appear in `help`. Kategoriler:
    //   TIMER         LS'in ana iş mantığı (countdown + vacation)
    //   SYSTEM        Cihaz altyapısı (ağ, firmware, kimlik, dış reset, log)
    //   SKAPP         Eşleşmiş telefon yönetimi (pairing, passphrase, bond)
    //   ALARM OUTPUT  Erken uyarı katmanı (timer.alarm event'inde)
    //   LS OUTPUT     Son tetikleme aksiyonları (timer.triggered event'i)

    // TIMER
    sk_cli_register_topic("timer",     "Countdown timer (set / start / stop / reset / status)", "TIMER");
    sk_cli_register_topic("vacation",  "Vacation mode: pause countdown for N days",           "TIMER");

    // SYSTEM
    sk_cli_register_topic("wifi",      "Network connection (primary + backup)",               "SYSTEM");
    sk_cli_register_topic("ble",       "Bluetooth transport",                                 "SYSTEM");
    sk_cli_register_topic("ota",       "Firmware updates (check / install / rollback)",       "SYSTEM");
    sk_cli_register_topic("device",    "Identity, restart, factory reset",                    "SYSTEM");
    sk_cli_register_topic("reset_api", "Inbound HTTP /api/reset endpoint",                    "SYSTEM");
    sk_cli_register_topic("logs",      "Log entries (ring buffer)",                           "SYSTEM");

    // SKAPP
    sk_cli_register_topic("pairing",   "SKAPP pairing window (open / status / close)",        "SKAPP");
    sk_cli_register_topic("auth",      "SKAPP connection passphrase (set / change / mode)",   "SKAPP");
    sk_cli_register_topic("bond",      "Paired SKAPP installs (list / remove)",               "SKAPP");

    // ALARM OUTPUT — erken uyarı servisleri (timer.alarm event'inde otomatik)
    sk_cli_register_topic("smtp",      "SMTPS mail server (host, port, sender, api_key)",     "ALARM OUTPUT");
    sk_cli_register_topic("reminder",  "Early-warning reminder mail (fires on timer.alarm)",  "ALARM OUTPUT");
    sk_cli_register_topic("api",       "Outbound webhook presets (Telegram, IFTTT, custom)",  "ALARM OUTPUT");

    // LS OUTPUT — son tetikleme aksiyonları (timer.triggered subscriber'ları aktif)
    sk_cli_register_topic("mail",      "Mail groups (recipients + subject + body)",           "LS OUTPUT");
    sk_cli_register_topic("relay",     "Output relay (GPIO + delay + duration + pulse)",      "LS OUTPUT");

    ESP_ERROR_CHECK(sk_transport_usb_init(NULL));

    // Auth subscriptions (control.short-press, factory-reset) registered first
    // so a button press during boot is captured rather than dropped — the
    // button polling task starts only after sk_button_init below.
    ESP_ERROR_CHECK(sk_auth_init());
    ESP_ERROR_CHECK(sk_passphrase_init());

    // Control button — drives pairing/BLE-on (short), restart (long),
    // factory reset (very long).
    ESP_ERROR_CHECK(sk_button_init(&(sk_button_cfg_t){
        .gpio_num        = SK_BUTTON_GPIO,
        .active_low      = SK_BUTTON_ACTIVE_LOW,
        .pullup_internal = true,
    }, NULL, NULL));

    // Wireless stack — every SmartKraft device is BLE + WiFi.
    ESP_ERROR_CHECK(sk_wifi_init());
    ESP_ERROR_CHECK(sk_mdns_init(SK_TCP_PORT, SK_FW_VERSION));
    ESP_ERROR_CHECK(sk_transport_ble_init(NULL));
    ESP_ERROR_CHECK(sk_transport_tcp_init(&(sk_transport_tcp_cfg_t){
        .port = SK_TCP_PORT,
    }));

#if SK_OTA_ENABLE
    ESP_ERROR_CHECK(sk_ota_init(&(sk_ota_cfg_t){
        .fw_version   = SK_FW_VERSION,
        .manifest_url = SK_OTA_MANIFEST_URL[0] ? SK_OTA_MANIFEST_URL : NULL,
    }));
#endif

#if SK_API_ENABLE
    ESP_ERROR_CHECK(sk_api_init());
#endif

    // ------------------------------------------------------------------
    // Device-specific (LebensSpur) — ls_* component init.
    //
    // Glue: her ls_* component'i sk_event_bus'a publish/subscribe olur,
    // doğrudan birbirini çağırmaz. Event şeması (kritik):
    //   timer.alarm     → ls_reminder (reminder mail), sk_api (alarm-class webhook)
    //   timer.triggered → ls_mail_groups, ls_relay, sk_api (trigger-class webhook)
    // ls_reminder ve sk_api'nin timer.alarm/triggered abonelikleri cihaza
    // otonomi verir: SKAPP kapalıyken bile uyarı + tetikleme zinciri çalışır.
    // ------------------------------------------------------------------

    ESP_ERROR_CHECK(ls_timer_engine_init());                // Faz 1.1 — countdown core
    ESP_ERROR_CHECK(ls_relay_init(LS_RELAY_DEFAULT_GPIO));   // Faz 1.2 — relay output
    ESP_ERROR_CHECK(ls_smtp_init());                        // Faz 1.3 — SMTPS client
    ESP_ERROR_CHECK(ls_reminder_init());                    // Faz 1.7 — reminder (timer.alarm)
    ESP_ERROR_CHECK(ls_mail_groups_init());                 // Faz 1.3 — mail groups
    ESP_ERROR_CHECK(ls_reset_api_init());                   // Faz 1.4 — inbound HTTP

    ESP_LOGI(TAG, "LebensSpur Faz 1.7 up — id=%s", sk_identity_get());
}
