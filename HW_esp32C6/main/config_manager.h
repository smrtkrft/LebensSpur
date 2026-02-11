/**
 * @file config_manager.h
 * @brief Configuration Manager - JSON on External Flash
 * 
 * All settings stored as JSON files on external flash.
 * Path: /ext/config/
 * 
 * Security Notes:
 * - Passwords stored in plain text (device physical security assumed)
 * - Session tokens use cryptographically secure random generation
 * - Rate limiting prevents brute force attacks
 */

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Config file paths (on external flash) */
#define CONFIG_BASE_PATH        "/ext/config"
#define CONFIG_TIMER_FILE       CONFIG_BASE_PATH "/timer.json"
#define CONFIG_WIFI_FILE        CONFIG_BASE_PATH "/wifi.json"
#define CONFIG_MAIL_FILE        CONFIG_BASE_PATH "/mail.json"
#define CONFIG_AUTH_FILE        CONFIG_BASE_PATH "/auth.json"
#define CONFIG_RUNTIME_FILE     CONFIG_BASE_PATH "/runtime.json"
#define CONFIG_RELAY_FILE       CONFIG_BASE_PATH "/relay.json"
#define CONFIG_SETUP_FILE       CONFIG_BASE_PATH "/setup.json"

/** Size limits */
#define MAX_SSID_LEN            32
#define MAX_PASSWORD_LEN        64
#define MAX_EMAIL_LEN           64
#define MAX_SUBJECT_LEN         128
#define MAX_BODY_LEN            1024
#define MAX_HOSTNAME_LEN        32
#define MAX_RECIPIENTS          10
#define MAX_MAIL_GROUPS         10
#define MAX_GROUP_NAME_LEN      32

/* ============================================
 * TIMER CONFIGURATION (Dead Man's Switch)
 * ============================================ */
typedef struct {
    bool enabled;                   // Timer active
    uint32_t interval_hours;        // Reset interval (hours, 1-720)
    uint32_t warning_minutes;       // Warning time before trigger (minutes)
    uint32_t alarm_count;           // Number of alarms before trigger (0-10)
    char check_start[8];            // Active hours start (HH:MM)
    char check_end[8];              // Active hours end (HH:MM)
    bool relay_trigger;             // Trigger relay on timeout
    bool vacation_enabled;          // Vacation mode active
    uint32_t vacation_days;         // Vacation duration (days)
    int64_t vacation_start;         // Vacation start timestamp
} timer_config_t;

#define TIMER_CONFIG_DEFAULT() {    \
    .enabled = false,               \
    .interval_hours = 24,           \
    .warning_minutes = 30,          \
    .alarm_count = 3,               \
    .check_start = "08:00",         \
    .check_end = "22:00",           \
    .relay_trigger = true,          \
    .vacation_enabled = false,      \
    .vacation_days = 7,             \
    .vacation_start = 0             \
}

/* ============================================
 * TIMER RUNTIME STATE (persisted)
 * ============================================ */
typedef struct {
    bool triggered;                 // Has been triggered
    int64_t last_reset;             // Last reset timestamp (epoch ms)
    int64_t next_deadline;          // Next deadline (epoch ms)
    uint32_t reset_count;           // Total reset count
    uint32_t trigger_count;         // Trigger count
    uint32_t warnings_sent;         // Warnings sent in current cycle
} timer_runtime_t;

#define TIMER_RUNTIME_DEFAULT() {   \
    .triggered = false,             \
    .last_reset = 0,                \
    .next_deadline = 0,             \
    .reset_count = 0,               \
    .trigger_count = 0,             \
    .warnings_sent = 0              \
}

/* ============================================
 * WIFI CONFIGURATION
 * ============================================ */
typedef struct {
    char ssid[MAX_SSID_LEN + 1];
    char password[MAX_PASSWORD_LEN + 1];
    bool configured;
    
    // Static IP (optional)
    bool static_ip_enabled;
    char static_ip[16];
    char gateway[16];
    char subnet[16];
    char dns[16];
} wifi_config_t;

#define WIFI_CONFIG_DEFAULT() {     \
    .ssid = "",                     \
    .password = "",                 \
    .configured = false,            \
    .static_ip_enabled = false,     \
    .static_ip = "",                \
    .gateway = "",                  \
    .subnet = "",                   \
    .dns = ""                       \
}

/* ============================================
 * MAIL GROUP
 * ============================================ */
typedef struct {
    char name[MAX_GROUP_NAME_LEN + 1];
    bool enabled;
    char subject[MAX_SUBJECT_LEN + 1];
    char body[MAX_BODY_LEN + 1];
    char recipients[MAX_RECIPIENTS][MAX_EMAIL_LEN + 1];
    int recipient_count;
} mail_group_t;

#define MAIL_GROUP_DEFAULT() {      \
    .name = "",                     \
    .enabled = false,               \
    .subject = "",                  \
    .body = "",                     \
    .recipients = {{0}},            \
    .recipient_count = 0            \
}

/* ============================================
 * MAIL/SMTP CONFIGURATION
 * ============================================ */
typedef struct {
    char smtp_server[MAX_HOSTNAME_LEN + 1];
    uint16_t smtp_port;
    char smtp_username[MAX_EMAIL_LEN + 1];
    char smtp_password[MAX_PASSWORD_LEN + 1];
    char sender_name[MAX_GROUP_NAME_LEN + 1];
    bool use_tls;
    
    mail_group_t groups[MAX_MAIL_GROUPS];
    int group_count;
} mail_config_t;

#define MAIL_CONFIG_DEFAULT() {     \
    .smtp_server = "smtp.gmail.com",\
    .smtp_port = 465,               \
    .smtp_username = "",            \
    .smtp_password = "",            \
    .sender_name = "LebensSpur",    \
    .use_tls = true,                \
    .groups = {{0}},                \
    .group_count = 0                \
}

/* ============================================
 * RELAY CONFIGURATION
 * ============================================ */
typedef struct {
    bool inverted;                  // Active LOW
    uint32_t on_delay_ms;
    uint32_t off_delay_ms;
    uint32_t pulse_duration_ms;
    bool pulse_mode;
    uint32_t pulse_interval_ms;
    uint32_t pulse_count;           // 0 = infinite
} relay_config_t;

#define RELAY_CONFIG_DEFAULT() {    \
    .inverted = false,              \
    .on_delay_ms = 0,               \
    .off_delay_ms = 0,              \
    .pulse_duration_ms = 500,       \
    .pulse_mode = false,            \
    .pulse_interval_ms = 500,       \
    .pulse_count = 0                \
}

/* ============================================
 * AUTHENTICATION CONFIGURATION
 * Security: Password only, rate limiting
 * ============================================ */
typedef struct {
    char password[MAX_PASSWORD_LEN + 1];
    uint32_t session_timeout_min;       // Session timeout (minutes)
    uint32_t max_login_attempts;        // Max failed attempts
    uint32_t lockout_duration_min;      // Lockout duration
    int64_t lockout_until;              // Lockout end timestamp
    uint32_t failed_attempts;           // Current failed count
} auth_config_t;

#define AUTH_CONFIG_DEFAULT() {     \
    .password = "",                 \
    .session_timeout_min = 60,      \
    .max_login_attempts = 5,        \
    .lockout_duration_min = 15,     \
    .lockout_until = 0,             \
    .failed_attempts = 0            \
}

/* ============================================
 * SETUP STATE
 * ============================================ */
typedef struct {
    bool setup_completed;
    int64_t first_setup_time;
    char device_name[MAX_HOSTNAME_LEN + 1];
    uint32_t boot_count;
} setup_config_t;

#define SETUP_CONFIG_DEFAULT() {    \
    .setup_completed = false,       \
    .first_setup_time = 0,          \
    .device_name = "LebensSpur",    \
    .boot_count = 0                 \
}

/* ============================================
 * FUNCTION PROTOTYPES
 * ============================================ */

/** Initialize config manager (call after file_manager_init) */
esp_err_t config_manager_init(void);

/** Timer config */
esp_err_t config_load_timer(timer_config_t *config);
esp_err_t config_save_timer(const timer_config_t *config);

/** Timer runtime */
esp_err_t config_load_runtime(timer_runtime_t *runtime);
esp_err_t config_save_runtime(const timer_runtime_t *runtime);

/** WiFi config */
esp_err_t config_load_wifi(wifi_config_t *config);
esp_err_t config_save_wifi(const wifi_config_t *config);

/** Mail config */
esp_err_t config_load_mail(mail_config_t *config);
esp_err_t config_save_mail(const mail_config_t *config);

/** Auth config */
esp_err_t config_load_auth(auth_config_t *config);
esp_err_t config_save_auth(const auth_config_t *config);

/** Relay config */
esp_err_t config_load_relay(relay_config_t *config);
esp_err_t config_save_relay(const relay_config_t *config);

/** Setup config */
esp_err_t config_load_setup(setup_config_t *config);
esp_err_t config_save_setup(const setup_config_t *config);

/** Setup status helpers */
bool config_is_setup_completed(void);
esp_err_t config_mark_setup_completed(void);

/** Factory reset */
esp_err_t config_factory_reset(void);

/** Check config directory */
bool config_directory_exists(void);

/** Increment boot count */
uint32_t config_increment_boot_count(void);

/** Verify password (with rate limiting) */
bool config_verify_password(const char *password);

/** Record failed login attempt */
void config_record_failed_login(void);

/** Clear failed attempts after successful login */
void config_clear_failed_attempts(void);

/** Check if locked out */
bool config_is_locked_out(void);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_MANAGER_H
