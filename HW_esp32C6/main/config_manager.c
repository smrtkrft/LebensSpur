/**
 * @file config_manager.c
 * @brief Configuration Manager - JSON Implementation
 * 
 * Uses cJSON library for JSON parsing/serialization.
 * All config files stored on external flash.
 */

#include "config_manager.h"
#include "file_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "config_mgr";

/* ============================================
 * HELPER FUNCTIONS
 * ============================================ */

static cJSON* read_json_file(const char *filepath)
{
    if (!file_manager_exists(filepath)) {
        ESP_LOGD(TAG, "File not found: %s", filepath);
        return NULL;
    }
    
    int32_t file_size = file_manager_get_size(filepath);
    if (file_size <= 0 || file_size > 16384) {  // Max 16KB
        ESP_LOGW(TAG, "Invalid file size: %s (%ld)", filepath, file_size);
        return NULL;
    }
    
    char *buffer = malloc(file_size + 1);
    if (!buffer) {
        ESP_LOGE(TAG, "Memory allocation failed");
        return NULL;
    }
    
    size_t read_bytes = 0;
    esp_err_t ret = file_manager_read(filepath, buffer, file_size, &read_bytes);
    if (ret != ESP_OK) {
        free(buffer);
        return NULL;
    }
    buffer[read_bytes] = '\0';
    
    cJSON *json = cJSON_Parse(buffer);
    free(buffer);
    
    if (!json) {
        ESP_LOGE(TAG, "JSON parse error: %s", filepath);
    }
    
    return json;
}

static esp_err_t write_json_file(const char *filepath, cJSON *json)
{
    char *json_str = cJSON_PrintUnformatted(json);
    if (!json_str) {
        return ESP_FAIL;
    }
    
    esp_err_t ret = file_manager_write(filepath, json_str, strlen(json_str));
    free(json_str);
    
    return ret;
}

static void safe_strcpy(char *dest, const char *src, size_t dest_size)
{
    if (src && dest_size > 0) {
        strncpy(dest, src, dest_size - 1);
        dest[dest_size - 1] = '\0';
    } else if (dest_size > 0) {
        dest[0] = '\0';
    }
}

static void json_get_string(cJSON *json, const char *key, char *dest, size_t dest_size)
{
    cJSON *item = cJSON_GetObjectItem(json, key);
    if (item && cJSON_IsString(item) && item->valuestring) {
        safe_strcpy(dest, item->valuestring, dest_size);
    }
}

static int json_get_int(cJSON *json, const char *key, int default_val)
{
    cJSON *item = cJSON_GetObjectItem(json, key);
    if (item && cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return default_val;
}

static bool json_get_bool(cJSON *json, const char *key, bool default_val)
{
    cJSON *item = cJSON_GetObjectItem(json, key);
    if (item && cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return default_val;
}

static int64_t json_get_int64(cJSON *json, const char *key, int64_t default_val)
{
    cJSON *item = cJSON_GetObjectItem(json, key);
    if (item && cJSON_IsNumber(item)) {
        return (int64_t)item->valuedouble;
    }
    return default_val;
}

static int64_t get_current_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ============================================
 * INIT
 * ============================================ */

esp_err_t config_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing config manager...");
    
    if (!file_manager_is_mounted()) {
        ESP_LOGE(TAG, "File manager not mounted!");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Create config directory
    file_manager_mkdir(CONFIG_BASE_PATH);
    
    ESP_LOGI(TAG, "Config manager ready");
    return ESP_OK;
}

bool config_directory_exists(void)
{
    return file_manager_exists(CONFIG_BASE_PATH);
}

/* ============================================
 * TIMER CONFIG
 * ============================================ */

esp_err_t config_load_timer(timer_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    timer_config_t defaults = TIMER_CONFIG_DEFAULT();
    *config = defaults;
    
    cJSON *json = read_json_file(CONFIG_TIMER_FILE);
    if (!json) return ESP_OK;  // Use defaults
    
    config->enabled = json_get_bool(json, "enabled", defaults.enabled);
    config->interval_hours = json_get_int(json, "intervalHours", defaults.interval_hours);
    config->warning_minutes = json_get_int(json, "warningMinutes", defaults.warning_minutes);
    config->alarm_count = json_get_int(json, "alarmCount", defaults.alarm_count);
    json_get_string(json, "checkStart", config->check_start, sizeof(config->check_start));
    json_get_string(json, "checkEnd", config->check_end, sizeof(config->check_end));
    config->relay_trigger = json_get_bool(json, "relayTrigger", defaults.relay_trigger);
    config->vacation_enabled = json_get_bool(json, "vacationEnabled", defaults.vacation_enabled);
    config->vacation_days = json_get_int(json, "vacationDays", defaults.vacation_days);
    config->vacation_start = json_get_int64(json, "vacationStart", defaults.vacation_start);
    
    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t config_save_timer(const timer_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    
    cJSON_AddBoolToObject(json, "enabled", config->enabled);
    cJSON_AddNumberToObject(json, "intervalHours", config->interval_hours);
    cJSON_AddNumberToObject(json, "warningMinutes", config->warning_minutes);
    cJSON_AddNumberToObject(json, "alarmCount", config->alarm_count);
    cJSON_AddStringToObject(json, "checkStart", config->check_start);
    cJSON_AddStringToObject(json, "checkEnd", config->check_end);
    cJSON_AddBoolToObject(json, "relayTrigger", config->relay_trigger);
    cJSON_AddBoolToObject(json, "vacationEnabled", config->vacation_enabled);
    cJSON_AddNumberToObject(json, "vacationDays", config->vacation_days);
    cJSON_AddNumberToObject(json, "vacationStart", (double)config->vacation_start);
    
    esp_err_t ret = write_json_file(CONFIG_TIMER_FILE, json);
    cJSON_Delete(json);
    return ret;
}

/* ============================================
 * TIMER RUNTIME
 * ============================================ */

esp_err_t config_load_runtime(timer_runtime_t *runtime)
{
    if (!runtime) return ESP_ERR_INVALID_ARG;
    
    timer_runtime_t defaults = TIMER_RUNTIME_DEFAULT();
    *runtime = defaults;
    
    cJSON *json = read_json_file(CONFIG_RUNTIME_FILE);
    if (!json) return ESP_OK;
    
    runtime->triggered = json_get_bool(json, "triggered", defaults.triggered);
    runtime->last_reset = json_get_int64(json, "lastReset", defaults.last_reset);
    runtime->next_deadline = json_get_int64(json, "nextDeadline", defaults.next_deadline);
    runtime->reset_count = json_get_int(json, "resetCount", defaults.reset_count);
    runtime->trigger_count = json_get_int(json, "triggerCount", defaults.trigger_count);
    runtime->warnings_sent = json_get_int(json, "warningsSent", defaults.warnings_sent);
    
    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t config_save_runtime(const timer_runtime_t *runtime)
{
    if (!runtime) return ESP_ERR_INVALID_ARG;
    
    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    
    cJSON_AddBoolToObject(json, "triggered", runtime->triggered);
    cJSON_AddNumberToObject(json, "lastReset", (double)runtime->last_reset);
    cJSON_AddNumberToObject(json, "nextDeadline", (double)runtime->next_deadline);
    cJSON_AddNumberToObject(json, "resetCount", runtime->reset_count);
    cJSON_AddNumberToObject(json, "triggerCount", runtime->trigger_count);
    cJSON_AddNumberToObject(json, "warningsSent", runtime->warnings_sent);
    
    esp_err_t ret = write_json_file(CONFIG_RUNTIME_FILE, json);
    cJSON_Delete(json);
    return ret;
}

/* ============================================
 * WIFI CONFIG
 * ============================================ */

esp_err_t config_load_wifi(app_wifi_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    app_wifi_config_t defaults = WIFI_CONFIG_DEFAULT();
    *config = defaults;
    
    cJSON *json = read_json_file(CONFIG_WIFI_FILE);
    if (!json) return ESP_OK;
    
    json_get_string(json, "ssid", config->ssid, sizeof(config->ssid));
    json_get_string(json, "password", config->password, sizeof(config->password));
    config->configured = json_get_bool(json, "configured", defaults.configured);
    config->static_ip_enabled = json_get_bool(json, "staticIpEnabled", defaults.static_ip_enabled);
    json_get_string(json, "staticIp", config->static_ip, sizeof(config->static_ip));
    json_get_string(json, "gateway", config->gateway, sizeof(config->gateway));
    json_get_string(json, "subnet", config->subnet, sizeof(config->subnet));
    json_get_string(json, "dns", config->dns, sizeof(config->dns));
    
    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t config_save_wifi(const app_wifi_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    
    cJSON_AddStringToObject(json, "ssid", config->ssid);
    cJSON_AddStringToObject(json, "password", config->password);
    cJSON_AddBoolToObject(json, "configured", config->configured);
    cJSON_AddBoolToObject(json, "staticIpEnabled", config->static_ip_enabled);
    cJSON_AddStringToObject(json, "staticIp", config->static_ip);
    cJSON_AddStringToObject(json, "gateway", config->gateway);
    cJSON_AddStringToObject(json, "subnet", config->subnet);
    cJSON_AddStringToObject(json, "dns", config->dns);
    
    esp_err_t ret = write_json_file(CONFIG_WIFI_FILE, json);
    cJSON_Delete(json);
    return ret;
}

/* ============================================
 * AUTH CONFIG
 * ============================================ */

esp_err_t config_load_auth(auth_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    auth_config_t defaults = AUTH_CONFIG_DEFAULT();
    *config = defaults;
    
    cJSON *json = read_json_file(CONFIG_AUTH_FILE);
    if (!json) return ESP_OK;
    
    json_get_string(json, "password", config->password, sizeof(config->password));
    config->session_timeout_min = json_get_int(json, "sessionTimeoutMin", defaults.session_timeout_min);
    config->max_login_attempts = json_get_int(json, "maxLoginAttempts", defaults.max_login_attempts);
    config->lockout_duration_min = json_get_int(json, "lockoutDurationMin", defaults.lockout_duration_min);
    config->lockout_until = json_get_int64(json, "lockoutUntil", defaults.lockout_until);
    config->failed_attempts = json_get_int(json, "failedAttempts", defaults.failed_attempts);
    
    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t config_save_auth(const auth_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    
    cJSON_AddStringToObject(json, "password", config->password);
    cJSON_AddNumberToObject(json, "sessionTimeoutMin", config->session_timeout_min);
    cJSON_AddNumberToObject(json, "maxLoginAttempts", config->max_login_attempts);
    cJSON_AddNumberToObject(json, "lockoutDurationMin", config->lockout_duration_min);
    cJSON_AddNumberToObject(json, "lockoutUntil", (double)config->lockout_until);
    cJSON_AddNumberToObject(json, "failedAttempts", config->failed_attempts);
    
    esp_err_t ret = write_json_file(CONFIG_AUTH_FILE, json);
    cJSON_Delete(json);
    return ret;
}

/* ============================================
 * MAIL CONFIG (simplified - no groups for now)
 * ============================================ */

esp_err_t config_load_mail(mail_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    mail_config_t defaults = MAIL_CONFIG_DEFAULT();
    *config = defaults;
    
    cJSON *json = read_json_file(CONFIG_MAIL_FILE);
    if (!json) return ESP_OK;
    
    json_get_string(json, "smtpServer", config->smtp_server, sizeof(config->smtp_server));
    config->smtp_port = json_get_int(json, "smtpPort", defaults.smtp_port);
    json_get_string(json, "smtpUsername", config->smtp_username, sizeof(config->smtp_username));
    json_get_string(json, "smtpPassword", config->smtp_password, sizeof(config->smtp_password));
    json_get_string(json, "senderName", config->sender_name, sizeof(config->sender_name));
    config->use_tls = json_get_bool(json, "useTls", defaults.use_tls);
    
    // Load groups array
    cJSON *groups = cJSON_GetObjectItem(json, "groups");
    if (groups && cJSON_IsArray(groups)) {
        config->group_count = 0;
        cJSON *group_item;
        cJSON_ArrayForEach(group_item, groups) {
            if (config->group_count >= MAX_MAIL_GROUPS) break;
            
            mail_group_t *g = &config->groups[config->group_count];
            json_get_string(group_item, "name", g->name, sizeof(g->name));
            g->enabled = json_get_bool(group_item, "enabled", false);
            json_get_string(group_item, "subject", g->subject, sizeof(g->subject));
            json_get_string(group_item, "body", g->body, sizeof(g->body));
            
            // Load recipients
            cJSON *recipients = cJSON_GetObjectItem(group_item, "recipients");
            g->recipient_count = 0;
            if (recipients && cJSON_IsArray(recipients)) {
                cJSON *rcpt;
                cJSON_ArrayForEach(rcpt, recipients) {
                    if (g->recipient_count >= MAX_RECIPIENTS) break;
                    if (cJSON_IsString(rcpt) && rcpt->valuestring) {
                        safe_strcpy(g->recipients[g->recipient_count], rcpt->valuestring, MAX_EMAIL_LEN + 1);
                        g->recipient_count++;
                    }
                }
            }
            config->group_count++;
        }
    }
    
    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t config_save_mail(const mail_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    
    cJSON_AddStringToObject(json, "smtpServer", config->smtp_server);
    cJSON_AddNumberToObject(json, "smtpPort", config->smtp_port);
    cJSON_AddStringToObject(json, "smtpUsername", config->smtp_username);
    cJSON_AddStringToObject(json, "smtpPassword", config->smtp_password);
    cJSON_AddStringToObject(json, "senderName", config->sender_name);
    cJSON_AddBoolToObject(json, "useTls", config->use_tls);
    
    // Save groups
    cJSON *groups = cJSON_AddArrayToObject(json, "groups");
    for (int i = 0; i < config->group_count; i++) {
        const mail_group_t *g = &config->groups[i];
        cJSON *group_obj = cJSON_CreateObject();
        
        cJSON_AddStringToObject(group_obj, "name", g->name);
        cJSON_AddBoolToObject(group_obj, "enabled", g->enabled);
        cJSON_AddStringToObject(group_obj, "subject", g->subject);
        cJSON_AddStringToObject(group_obj, "body", g->body);
        
        cJSON *recipients = cJSON_AddArrayToObject(group_obj, "recipients");
        for (int j = 0; j < g->recipient_count; j++) {
            cJSON_AddItemToArray(recipients, cJSON_CreateString(g->recipients[j]));
        }
        
        cJSON_AddItemToArray(groups, group_obj);
    }
    
    esp_err_t ret = write_json_file(CONFIG_MAIL_FILE, json);
    cJSON_Delete(json);
    return ret;
}

/* ============================================
 * RELAY CONFIG
 * ============================================ */

esp_err_t config_load_relay(relay_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    relay_config_t defaults = RELAY_CONFIG_DEFAULT();
    *config = defaults;
    
    cJSON *json = read_json_file(CONFIG_RELAY_FILE);
    if (!json) return ESP_OK;
    
    config->inverted = json_get_bool(json, "inverted", defaults.inverted);
    config->on_delay_ms = json_get_int(json, "onDelayMs", defaults.on_delay_ms);
    config->off_delay_ms = json_get_int(json, "offDelayMs", defaults.off_delay_ms);
    config->pulse_duration_ms = json_get_int(json, "pulseDurationMs", defaults.pulse_duration_ms);
    config->pulse_mode = json_get_bool(json, "pulseMode", defaults.pulse_mode);
    config->pulse_interval_ms = json_get_int(json, "pulseIntervalMs", defaults.pulse_interval_ms);
    config->pulse_count = json_get_int(json, "pulseCount", defaults.pulse_count);
    
    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t config_save_relay(const relay_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    
    cJSON_AddBoolToObject(json, "inverted", config->inverted);
    cJSON_AddNumberToObject(json, "onDelayMs", config->on_delay_ms);
    cJSON_AddNumberToObject(json, "offDelayMs", config->off_delay_ms);
    cJSON_AddNumberToObject(json, "pulseDurationMs", config->pulse_duration_ms);
    cJSON_AddBoolToObject(json, "pulseMode", config->pulse_mode);
    cJSON_AddNumberToObject(json, "pulseIntervalMs", config->pulse_interval_ms);
    cJSON_AddNumberToObject(json, "pulseCount", config->pulse_count);
    
    esp_err_t ret = write_json_file(CONFIG_RELAY_FILE, json);
    cJSON_Delete(json);
    return ret;
}

/* ============================================
 * SETUP CONFIG
 * ============================================ */

esp_err_t config_load_setup(setup_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    setup_config_t defaults = SETUP_CONFIG_DEFAULT();
    *config = defaults;
    
    cJSON *json = read_json_file(CONFIG_SETUP_FILE);
    if (!json) return ESP_OK;
    
    config->setup_completed = json_get_bool(json, "setupCompleted", defaults.setup_completed);
    config->first_setup_time = json_get_int64(json, "firstSetupTime", defaults.first_setup_time);
    json_get_string(json, "deviceName", config->device_name, sizeof(config->device_name));
    config->boot_count = json_get_int(json, "bootCount", defaults.boot_count);
    
    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t config_save_setup(const setup_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    
    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    
    cJSON_AddBoolToObject(json, "setupCompleted", config->setup_completed);
    cJSON_AddNumberToObject(json, "firstSetupTime", (double)config->first_setup_time);
    cJSON_AddStringToObject(json, "deviceName", config->device_name);
    cJSON_AddNumberToObject(json, "bootCount", config->boot_count);
    
    esp_err_t ret = write_json_file(CONFIG_SETUP_FILE, json);
    cJSON_Delete(json);
    return ret;
}

/* ============================================
 * SETUP HELPERS
 * ============================================ */

bool config_is_setup_completed(void)
{
    setup_config_t config;
    config_load_setup(&config);
    return config.setup_completed;
}

esp_err_t config_mark_setup_completed(void)
{
    setup_config_t config;
    config_load_setup(&config);
    config.setup_completed = true;
    config.first_setup_time = get_current_time_ms();
    return config_save_setup(&config);
}

uint32_t config_increment_boot_count(void)
{
    setup_config_t config;
    config_load_setup(&config);
    config.boot_count++;
    config_save_setup(&config);
    ESP_LOGI(TAG, "Boot count: %lu", config.boot_count);
    return config.boot_count;
}

/* ============================================
 * SECURITY HELPERS
 * ============================================ */

bool config_verify_password(const char *password)
{
    if (!password) return false;
    
    // Check lockout
    if (config_is_locked_out()) {
        ESP_LOGW(TAG, "Account locked out");
        return false;
    }
    
    auth_config_t auth;
    config_load_auth(&auth);
    
    // Constant-time comparison to prevent timing attacks
    size_t pw_len = strlen(password);
    size_t stored_len = strlen(auth.password);
    
    volatile uint8_t result = (pw_len == stored_len) ? 0 : 1;
    size_t min_len = (pw_len < stored_len) ? pw_len : stored_len;
    
    for (size_t i = 0; i < min_len; i++) {
        result |= (password[i] ^ auth.password[i]);
    }
    
    if (result == 0) {
        config_clear_failed_attempts();
        return true;
    } else {
        config_record_failed_login();
        return false;
    }
}

void config_record_failed_login(void)
{
    auth_config_t auth;
    config_load_auth(&auth);
    
    auth.failed_attempts++;
    ESP_LOGW(TAG, "Failed login attempt: %lu/%lu", 
             auth.failed_attempts, auth.max_login_attempts);
    
    if (auth.failed_attempts >= auth.max_login_attempts) {
        auth.lockout_until = get_current_time_ms() + 
                            (auth.lockout_duration_min * 60 * 1000);
        ESP_LOGW(TAG, "Account locked for %lu minutes", auth.lockout_duration_min);
    }
    
    config_save_auth(&auth);
}

void config_clear_failed_attempts(void)
{
    auth_config_t auth;
    config_load_auth(&auth);
    auth.failed_attempts = 0;
    auth.lockout_until = 0;
    config_save_auth(&auth);
}

bool config_is_locked_out(void)
{
    auth_config_t auth;
    config_load_auth(&auth);
    
    if (auth.lockout_until == 0) {
        return false;
    }
    
    int64_t now = get_current_time_ms();
    if (now >= auth.lockout_until) {
        // Lockout expired, clear it
        auth.lockout_until = 0;
        auth.failed_attempts = 0;
        config_save_auth(&auth);
        return false;
    }
    
    return true;
}

/* ============================================
 * FACTORY RESET
 * ============================================ */

esp_err_t config_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset - deleting all config files");
    
    file_manager_delete(CONFIG_TIMER_FILE);
    file_manager_delete(CONFIG_WIFI_FILE);
    file_manager_delete(CONFIG_MAIL_FILE);
    file_manager_delete(CONFIG_AUTH_FILE);
    file_manager_delete(CONFIG_RUNTIME_FILE);
    file_manager_delete(CONFIG_RELAY_FILE);
    file_manager_delete(CONFIG_SETUP_FILE);
    
    ESP_LOGI(TAG, "Factory reset complete");
    return ESP_OK;
}
