/**
 * Config Manager - JSON Ayar Yönetimi
 *
 * Tüm ayarlar harici flash'ta /ext/config/ altında JSON olarak saklanır.
 * cJSON ile serialization/deserialization.
 *
 * Bağımlılık: file_manager (Katman 1)
 * Katman: 2 (Yapılandırma)
 */

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================
// DOSYA YOLLARI
// ============================================
#define CONFIG_BASE_PATH        "/ext/config"
#define CONFIG_TIMER_FILE       CONFIG_BASE_PATH "/timer.json"
#define CONFIG_WIFI_FILE        CONFIG_BASE_PATH "/wifi.json"
#define CONFIG_MAIL_FILE        CONFIG_BASE_PATH "/mail.json"
#define CONFIG_API_FILE         CONFIG_BASE_PATH "/api.json"
#define CONFIG_AUTH_FILE        CONFIG_BASE_PATH "/auth.json"
#define CONFIG_RUNTIME_FILE     CONFIG_BASE_PATH "/runtime.json"
#define CONFIG_RELAY_FILE       CONFIG_BASE_PATH "/relay.json"
#define CONFIG_SETUP_FILE       CONFIG_BASE_PATH "/setup.json"

// ============================================
// SABİTLER
// ============================================
#define MAX_SSID_LEN            32
#define MAX_PASSWORD_LEN        64
#define MAX_EMAIL_LEN           64
#define MAX_SUBJECT_LEN         128
#define MAX_URL_LEN             256
#define MAX_TOKEN_LEN           64
#define MAX_HOSTNAME_LEN        32
#define MAX_RECIPIENTS          10
#define MAX_MAIL_GROUPS         10
#define MAX_GROUP_NAME_LEN      32

// ============================================
// TIMER AYARLARI (Dead Man's Switch)
// ============================================
typedef struct {
    bool enabled;
    uint32_t interval_hours;        // Sıfırlama aralığı (saat)
    uint32_t warning_minutes;       // Uyarı süresi (dakika)
    char check_start[8];            // Aktif saat başlangıç "HH:MM"
    char check_end[8];              // Aktif saat bitiş "HH:MM"
    char relay_action[16];          // "none", "on", "off", "pulse"
} timer_config_t;

#define TIMER_CONFIG_DEFAULT() {    \
    .enabled = false,               \
    .interval_hours = 24,           \
    .warning_minutes = 30,          \
    .check_start = "08:00",         \
    .check_end = "22:00",           \
    .relay_action = "none"          \
}

// ============================================
// TIMER RUNTIME DURUMU
// ============================================
typedef struct {
    bool triggered;
    int64_t last_reset;             // Son sıfırlama (epoch ms)
    int64_t next_deadline;          // Sonraki deadline (epoch ms)
    uint32_t reset_count;
    uint32_t trigger_count;
} timer_runtime_t;

#define TIMER_RUNTIME_DEFAULT() {   \
    .triggered = false,             \
    .last_reset = 0,                \
    .next_deadline = 0,             \
    .reset_count = 0,               \
    .trigger_count = 0              \
}

// ============================================
// WIFI AYARLARI
// ============================================
typedef struct {
    char ip[16];
    char gateway[16];
    char subnet[16];
    char dns[16];
} static_ip_config_t;

typedef struct {
    char primary_ssid[MAX_SSID_LEN];
    char primary_password[MAX_PASSWORD_LEN];
    bool primary_static_enabled;
    static_ip_config_t primary_static;
    char primary_mdns[MAX_HOSTNAME_LEN];

    char secondary_ssid[MAX_SSID_LEN];
    char secondary_password[MAX_PASSWORD_LEN];
    bool secondary_static_enabled;
    static_ip_config_t secondary_static;
    char secondary_mdns[MAX_HOSTNAME_LEN];

    bool ap_mode_enabled;
    bool allow_open_networks;
} ls_wifi_config_t;

#define LS_WIFI_CONFIG_DEFAULT() {          \
    .primary_ssid = "",                     \
    .primary_password = "",                 \
    .primary_static_enabled = false,        \
    .primary_static = {{0}},                \
    .primary_mdns = "",                     \
    .secondary_ssid = "",                   \
    .secondary_password = "",               \
    .secondary_static_enabled = false,      \
    .secondary_static = {{0}},              \
    .secondary_mdns = "",                   \
    .ap_mode_enabled = true,                \
    .allow_open_networks = false            \
}

// ============================================
// MAIL AYARLARI
// ============================================
typedef struct {
    char name[MAX_GROUP_NAME_LEN];
    bool enabled;
    char recipients[MAX_RECIPIENTS][MAX_EMAIL_LEN];
    int recipient_count;
} mail_group_t;

#define MAIL_GROUP_DEFAULT() {  \
    .name = "",                 \
    .enabled = false,           \
    .recipients = {{0}},        \
    .recipient_count = 0        \
}

typedef struct {
    char server[MAX_HOSTNAME_LEN];
    int port;
    char username[MAX_EMAIL_LEN];
    char password[MAX_PASSWORD_LEN];
    char sender_name[MAX_GROUP_NAME_LEN];
} mail_config_t;

#define MAIL_CONFIG_DEFAULT() {     \
    .server = "smtp.gmail.com",     \
    .port = 465,                    \
    .username = "",                 \
    .password = "",                 \
    .sender_name = "LebensSpur"     \
}

// ============================================
// API AYARLARI
// ============================================
typedef struct {
    bool enabled;
    char endpoint[MAX_HOSTNAME_LEN];
    bool require_token;
    char token[MAX_TOKEN_LEN];
} api_config_t;

#define API_CONFIG_DEFAULT() {  \
    .enabled = true,            \
    .endpoint = "trigger",      \
    .require_token = false,     \
    .token = ""                 \
}

// ============================================
// RELAY AYARLARI
// relay_config_t ile uyumlu (relay_manager.h)
// ============================================
typedef struct {
    bool inverted;
    uint32_t delay_seconds;         // Tetikleme öncesi bekleme
    uint32_t duration_seconds;      // Açık kalma süresi (0=süresiz)
    bool pulse_enabled;
    uint32_t pulse_on_ms;
    uint32_t pulse_off_ms;
} ls_relay_config_t;

#define LS_RELAY_CONFIG_DEFAULT() { \
    .inverted = false,              \
    .delay_seconds = 0,             \
    .duration_seconds = 0,          \
    .pulse_enabled = false,         \
    .pulse_on_ms = 500,             \
    .pulse_off_ms = 500             \
}

// ============================================
// SETUP DURUMU
// ============================================
typedef struct {
    bool setup_completed;
    int64_t first_setup_time;       // Epoch ms
    char device_name[MAX_HOSTNAME_LEN];
} setup_config_t;

#define SETUP_CONFIG_DEFAULT() {    \
    .setup_completed = false,       \
    .first_setup_time = 0,          \
    .device_name = "LebensSpur"     \
}

// ============================================
// AUTH AYARLARI
// ============================================
typedef struct {
    char password[64];              // Sadece sifre (username yok)
    uint32_t session_timeout_min;   // Oturum timeout (dakika)
    bool password_enabled;          // Sifre aktif mi (false = dogrudan erisim)
} auth_config_t;

#define AUTH_CONFIG_DEFAULT() { \
    .password = "",             \
    .session_timeout_min = 60,  \
    .password_enabled = false   \
}

// ============================================
// FONKSİYON PROTOTİPLERİ
// ============================================

esp_err_t config_manager_init(void);

// Timer
esp_err_t config_load_timer(timer_config_t *config);
esp_err_t config_save_timer(const timer_config_t *config);

// Timer Runtime
esp_err_t config_load_runtime(timer_runtime_t *runtime);
esp_err_t config_save_runtime(const timer_runtime_t *runtime);

// WiFi
esp_err_t config_load_wifi(ls_wifi_config_t *config);
esp_err_t config_save_wifi(const ls_wifi_config_t *config);

// Mail
esp_err_t config_load_mail(mail_config_t *config);
esp_err_t config_save_mail(const mail_config_t *config);
esp_err_t config_load_mail_group(int index, mail_group_t *group);
esp_err_t config_save_mail_group(int index, const mail_group_t *group);

// API
esp_err_t config_load_api(api_config_t *config);
esp_err_t config_save_api(const api_config_t *config);

// Auth
esp_err_t config_load_auth(auth_config_t *config);
esp_err_t config_save_auth(const auth_config_t *config);

// Relay
esp_err_t config_load_relay(ls_relay_config_t *config);
esp_err_t config_save_relay(const ls_relay_config_t *config);

// Setup
esp_err_t config_load_setup(setup_config_t *config);
esp_err_t config_save_setup(const setup_config_t *config);
bool config_is_setup_completed(void);
esp_err_t config_mark_setup_completed(void);

// Factory Reset
esp_err_t config_factory_reset(void);
bool config_directory_exists(void);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_MANAGER_H
