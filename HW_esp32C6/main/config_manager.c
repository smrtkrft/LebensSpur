/**
 * Config Manager - JSON Ayar Yönetimi
 *
 * cJSON ile JSON serialization/deserialization.
 * Tüm config dosyaları /ext/config/ altında.
 *
 * Bağımlılık: file_manager (Katman 1)
 * Katman: 2 (Yapılandırma)
 */

#include "config_manager.h"
#include "file_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "CONFIG";

// ============================================================================
// Yardımcı Fonksiyonlar
// ============================================================================

static cJSON *read_json_file(const char *filepath)
{
    if (!file_manager_exists(filepath)) {
        ESP_LOGD(TAG, "Dosya yok: %s", filepath);
        return NULL;
    }

    int32_t file_size = file_manager_get_size(filepath);
    if (file_size <= 0 || file_size > 8192) {
        ESP_LOGW(TAG, "Gecersiz dosya boyutu: %s (%ld)", filepath, file_size);
        return NULL;
    }

    char *buffer = malloc(file_size + 1);
    if (!buffer) {
        ESP_LOGE(TAG, "Bellek ayrilamadi: %ld byte", file_size + 1);
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
        ESP_LOGE(TAG, "JSON parse hatasi: %s", filepath);
    }
    return json;
}

static esp_err_t write_json_file(const char *filepath, cJSON *json)
{
    char *str = cJSON_PrintUnformatted(json);
    if (!str) {
        ESP_LOGE(TAG, "JSON serialize hatasi");
        return ESP_FAIL;
    }

    esp_err_t ret = file_manager_write(filepath, str, strlen(str));
    free(str);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Yazilamadi: %s", filepath);
    }
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

static void json_get_str(cJSON *json, const char *key, char *dest, size_t size)
{
    cJSON *item = cJSON_GetObjectItem(json, key);
    if (item && cJSON_IsString(item) && item->valuestring) {
        safe_strcpy(dest, item->valuestring, size);
    }
}

static int json_get_int(cJSON *json, const char *key, int def)
{
    cJSON *item = cJSON_GetObjectItem(json, key);
    return (item && cJSON_IsNumber(item)) ? item->valueint : def;
}

static bool json_get_bool(cJSON *json, const char *key, bool def)
{
    cJSON *item = cJSON_GetObjectItem(json, key);
    return (item && cJSON_IsBool(item)) ? cJSON_IsTrue(item) : def;
}

static double json_get_double(cJSON *json, const char *key, double def)
{
    cJSON *item = cJSON_GetObjectItem(json, key);
    return (item && cJSON_IsNumber(item)) ? item->valuedouble : def;
}

// ============================================================================
// Init
// ============================================================================

esp_err_t config_manager_init(void)
{
    file_manager_mkdir(CONFIG_BASE_PATH);

    ESP_LOGI(TAG, "OK - %s", CONFIG_BASE_PATH);
    return ESP_OK;
}

bool config_directory_exists(void)
{
    return file_manager_exists(CONFIG_BASE_PATH);
}

// ============================================================================
// Timer Config
// ============================================================================

esp_err_t config_load_timer(timer_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    timer_config_t def = TIMER_CONFIG_DEFAULT();
    *config = def;

    cJSON *json = read_json_file(CONFIG_TIMER_FILE);
    if (!json) return ESP_OK;

    config->enabled = json_get_bool(json, "enabled", def.enabled);
    config->interval_hours = json_get_int(json, "intervalHours", def.interval_hours);
    config->warning_minutes = json_get_int(json, "warningMinutes", def.warning_minutes);
    json_get_str(json, "checkStart", config->check_start, sizeof(config->check_start));
    json_get_str(json, "checkEnd", config->check_end, sizeof(config->check_end));
    json_get_str(json, "relayAction", config->relay_action, sizeof(config->relay_action));

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
    cJSON_AddStringToObject(json, "checkStart", config->check_start);
    cJSON_AddStringToObject(json, "checkEnd", config->check_end);
    cJSON_AddStringToObject(json, "relayAction", config->relay_action);

    esp_err_t ret = write_json_file(CONFIG_TIMER_FILE, json);
    cJSON_Delete(json);
    return ret;
}

// ============================================================================
// Timer Runtime
// ============================================================================

esp_err_t config_load_runtime(timer_runtime_t *runtime)
{
    if (!runtime) return ESP_ERR_INVALID_ARG;

    timer_runtime_t def = TIMER_RUNTIME_DEFAULT();
    *runtime = def;

    cJSON *json = read_json_file(CONFIG_RUNTIME_FILE);
    if (!json) return ESP_OK;

    runtime->triggered = json_get_bool(json, "triggered", false);
    runtime->last_reset = (int64_t)json_get_double(json, "lastReset", 0);
    runtime->next_deadline = (int64_t)json_get_double(json, "nextDeadline", 0);
    runtime->reset_count = json_get_int(json, "resetCount", 0);
    runtime->trigger_count = json_get_int(json, "triggerCount", 0);

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

    esp_err_t ret = write_json_file(CONFIG_RUNTIME_FILE, json);
    cJSON_Delete(json);
    return ret;
}

// ============================================================================
// WiFi Config
// ============================================================================

static void load_static_ip(cJSON *parent, const char *key, static_ip_config_t *ip)
{
    cJSON *obj = cJSON_GetObjectItem(parent, key);
    if (!obj) return;

    json_get_str(obj, "ip", ip->ip, sizeof(ip->ip));
    json_get_str(obj, "gateway", ip->gateway, sizeof(ip->gateway));
    json_get_str(obj, "subnet", ip->subnet, sizeof(ip->subnet));
    json_get_str(obj, "dns", ip->dns, sizeof(ip->dns));
}

static void save_static_ip(cJSON *parent, const char *key, const static_ip_config_t *ip)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "ip", ip->ip);
    cJSON_AddStringToObject(obj, "gateway", ip->gateway);
    cJSON_AddStringToObject(obj, "subnet", ip->subnet);
    cJSON_AddStringToObject(obj, "dns", ip->dns);
    cJSON_AddItemToObject(parent, key, obj);
}

esp_err_t config_load_wifi(ls_wifi_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    ls_wifi_config_t def = LS_WIFI_CONFIG_DEFAULT();
    *config = def;

    cJSON *json = read_json_file(CONFIG_WIFI_FILE);
    if (!json) return ESP_OK;

    // Primary
    json_get_str(json, "primarySSID", config->primary_ssid, MAX_SSID_LEN);
    json_get_str(json, "primaryPassword", config->primary_password, MAX_PASSWORD_LEN);
    config->primary_static_enabled = json_get_bool(json, "primaryStaticEnabled", false);
    json_get_str(json, "primaryMDNS", config->primary_mdns, MAX_HOSTNAME_LEN);
    load_static_ip(json, "primaryStatic", &config->primary_static);

    // Secondary
    json_get_str(json, "secondarySSID", config->secondary_ssid, MAX_SSID_LEN);
    json_get_str(json, "secondaryPassword", config->secondary_password, MAX_PASSWORD_LEN);
    config->secondary_static_enabled = json_get_bool(json, "secondaryStaticEnabled", false);
    json_get_str(json, "secondaryMDNS", config->secondary_mdns, MAX_HOSTNAME_LEN);
    load_static_ip(json, "secondaryStatic", &config->secondary_static);

    // Genel
    config->ap_mode_enabled = json_get_bool(json, "apModeEnabled", true);
    config->allow_open_networks = json_get_bool(json, "allowOpenNetworks", false);

    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t config_save_wifi(const ls_wifi_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;

    // Primary
    cJSON_AddStringToObject(json, "primarySSID", config->primary_ssid);
    cJSON_AddStringToObject(json, "primaryPassword", config->primary_password);
    cJSON_AddBoolToObject(json, "primaryStaticEnabled", config->primary_static_enabled);
    cJSON_AddStringToObject(json, "primaryMDNS", config->primary_mdns);
    save_static_ip(json, "primaryStatic", &config->primary_static);

    // Secondary
    cJSON_AddStringToObject(json, "secondarySSID", config->secondary_ssid);
    cJSON_AddStringToObject(json, "secondaryPassword", config->secondary_password);
    cJSON_AddBoolToObject(json, "secondaryStaticEnabled", config->secondary_static_enabled);
    cJSON_AddStringToObject(json, "secondaryMDNS", config->secondary_mdns);
    save_static_ip(json, "secondaryStatic", &config->secondary_static);

    // Genel
    cJSON_AddBoolToObject(json, "apModeEnabled", config->ap_mode_enabled);
    cJSON_AddBoolToObject(json, "allowOpenNetworks", config->allow_open_networks);

    esp_err_t ret = write_json_file(CONFIG_WIFI_FILE, json);
    cJSON_Delete(json);
    return ret;
}

// ============================================================================
// Mail Config
// ============================================================================

esp_err_t config_load_mail(mail_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    mail_config_t def = MAIL_CONFIG_DEFAULT();
    *config = def;

    cJSON *json = read_json_file(CONFIG_MAIL_FILE);
    if (!json) return ESP_OK;

    json_get_str(json, "server", config->server, MAX_HOSTNAME_LEN);
    config->port = json_get_int(json, "port", def.port);
    json_get_str(json, "username", config->username, MAX_EMAIL_LEN);
    json_get_str(json, "password", config->password, MAX_PASSWORD_LEN);
    json_get_str(json, "senderName", config->sender_name, MAX_GROUP_NAME_LEN);

    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t config_save_mail(const mail_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(json, "server", config->server);
    cJSON_AddNumberToObject(json, "port", config->port);
    cJSON_AddStringToObject(json, "username", config->username);
    cJSON_AddStringToObject(json, "password", config->password);
    cJSON_AddStringToObject(json, "senderName", config->sender_name);

    esp_err_t ret = write_json_file(CONFIG_MAIL_FILE, json);
    cJSON_Delete(json);
    return ret;
}

// ============================================================================
// Mail Group Config
// ============================================================================

#define MAIL_GROUP_FILE_FMT  CONFIG_BASE_PATH "/mail_group_%d.json"

esp_err_t config_load_mail_group(int index, mail_group_t *group)
{
    if (!group || index < 0 || index >= MAX_MAIL_GROUPS) {
        return ESP_ERR_INVALID_ARG;
    }

    mail_group_t def = MAIL_GROUP_DEFAULT();
    *group = def;

    char path[64];
    snprintf(path, sizeof(path), MAIL_GROUP_FILE_FMT, index);

    cJSON *json = read_json_file(path);
    if (!json) return ESP_ERR_NOT_FOUND;

    json_get_str(json, "name", group->name, MAX_GROUP_NAME_LEN);
    group->enabled = json_get_bool(json, "enabled", false);

    cJSON *arr = cJSON_GetObjectItem(json, "recipients");
    if (arr && cJSON_IsArray(arr)) {
        int count = cJSON_GetArraySize(arr);
        group->recipient_count = 0;
        for (int i = 0; i < count && i < MAX_RECIPIENTS; i++) {
            cJSON *item = cJSON_GetArrayItem(arr, i);
            if (item && cJSON_IsString(item) && item->valuestring) {
                safe_strcpy(group->recipients[i], item->valuestring, MAX_EMAIL_LEN);
                group->recipient_count++;
            }
        }
    }

    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t config_save_mail_group(int index, const mail_group_t *group)
{
    if (!group || index < 0 || index >= MAX_MAIL_GROUPS) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[64];
    snprintf(path, sizeof(path), MAIL_GROUP_FILE_FMT, index);

    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(json, "name", group->name);
    cJSON_AddBoolToObject(json, "enabled", group->enabled);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < group->recipient_count && i < MAX_RECIPIENTS; i++) {
        if (group->recipients[i][0] != '\0') {
            cJSON_AddItemToArray(arr, cJSON_CreateString(group->recipients[i]));
        }
    }
    cJSON_AddItemToObject(json, "recipients", arr);

    esp_err_t ret = write_json_file(path, json);
    cJSON_Delete(json);
    return ret;
}

// ============================================================================
// API Config
// ============================================================================

esp_err_t config_load_api(api_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    api_config_t def = API_CONFIG_DEFAULT();
    *config = def;

    cJSON *json = read_json_file(CONFIG_API_FILE);
    if (!json) return ESP_OK;

    config->enabled = json_get_bool(json, "enabled", def.enabled);
    json_get_str(json, "endpoint", config->endpoint, MAX_HOSTNAME_LEN);
    config->require_token = json_get_bool(json, "requireToken", def.require_token);
    json_get_str(json, "token", config->token, MAX_TOKEN_LEN);

    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t config_save_api(const api_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;

    cJSON_AddBoolToObject(json, "enabled", config->enabled);
    cJSON_AddStringToObject(json, "endpoint", config->endpoint);
    cJSON_AddBoolToObject(json, "requireToken", config->require_token);
    cJSON_AddStringToObject(json, "token", config->token);

    esp_err_t ret = write_json_file(CONFIG_API_FILE, json);
    cJSON_Delete(json);
    return ret;
}

// ============================================================================
// Auth Config
// ============================================================================

esp_err_t config_load_auth(auth_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    auth_config_t def = AUTH_CONFIG_DEFAULT();
    *config = def;

    cJSON *json = read_json_file(CONFIG_AUTH_FILE);
    if (!json) return ESP_OK;

    json_get_str(json, "password", config->password, sizeof(config->password));
    config->session_timeout_min = json_get_int(json, "sessionTimeout", def.session_timeout_min);
    cJSON *pe = cJSON_GetObjectItem(json, "passwordEnabled");
    if (cJSON_IsBool(pe)) config->password_enabled = cJSON_IsTrue(pe);

    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t config_save_auth(const auth_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;

    cJSON_AddStringToObject(json, "password", config->password);
    cJSON_AddNumberToObject(json, "sessionTimeout", config->session_timeout_min);
    cJSON_AddBoolToObject(json, "passwordEnabled", config->password_enabled);

    esp_err_t ret = write_json_file(CONFIG_AUTH_FILE, json);
    cJSON_Delete(json);
    return ret;
}

// ============================================================================
// Relay Config (ls_relay_config_t - relay_config_t ile uyumlu)
// ============================================================================

esp_err_t config_load_relay(ls_relay_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    ls_relay_config_t def = LS_RELAY_CONFIG_DEFAULT();
    *config = def;

    cJSON *json = read_json_file(CONFIG_RELAY_FILE);
    if (!json) return ESP_OK;

    config->inverted = json_get_bool(json, "inverted", def.inverted);
    config->delay_seconds = json_get_int(json, "delaySeconds", def.delay_seconds);
    config->duration_seconds = json_get_int(json, "durationSeconds", def.duration_seconds);
    config->pulse_enabled = json_get_bool(json, "pulseEnabled", def.pulse_enabled);
    config->pulse_on_ms = json_get_int(json, "pulseOnMs", def.pulse_on_ms);
    config->pulse_off_ms = json_get_int(json, "pulseOffMs", def.pulse_off_ms);

    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t config_save_relay(const ls_relay_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;

    cJSON_AddBoolToObject(json, "inverted", config->inverted);
    cJSON_AddNumberToObject(json, "delaySeconds", config->delay_seconds);
    cJSON_AddNumberToObject(json, "durationSeconds", config->duration_seconds);
    cJSON_AddBoolToObject(json, "pulseEnabled", config->pulse_enabled);
    cJSON_AddNumberToObject(json, "pulseOnMs", config->pulse_on_ms);
    cJSON_AddNumberToObject(json, "pulseOffMs", config->pulse_off_ms);

    esp_err_t ret = write_json_file(CONFIG_RELAY_FILE, json);
    cJSON_Delete(json);
    return ret;
}

// ============================================================================
// Setup Config
// ============================================================================

esp_err_t config_load_setup(setup_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    setup_config_t def = SETUP_CONFIG_DEFAULT();
    *config = def;

    cJSON *json = read_json_file(CONFIG_SETUP_FILE);
    if (!json) return ESP_OK;

    config->setup_completed = json_get_bool(json, "setupCompleted", false);
    config->first_setup_time = (int64_t)json_get_double(json, "firstSetupTime", 0);
    json_get_str(json, "deviceName", config->device_name, MAX_HOSTNAME_LEN);

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

    esp_err_t ret = write_json_file(CONFIG_SETUP_FILE, json);
    cJSON_Delete(json);
    return ret;
}

bool config_is_setup_completed(void)
{
    setup_config_t cfg = SETUP_CONFIG_DEFAULT();
    config_load_setup(&cfg);
    return cfg.setup_completed;
}

esp_err_t config_mark_setup_completed(void)
{
    setup_config_t cfg = SETUP_CONFIG_DEFAULT();
    config_load_setup(&cfg);

    cfg.setup_completed = true;
    if (cfg.first_setup_time == 0) {
        cfg.first_setup_time = esp_timer_get_time() / 1000;
    }

    return config_save_setup(&cfg);
}

// ============================================================================
// Factory Reset
// ============================================================================

esp_err_t config_factory_reset(void)
{
    ESP_LOGW(TAG, "FABRIKA AYARLARINA DONULUYOR!");

    const char *files[] = {
        CONFIG_TIMER_FILE,
        CONFIG_WIFI_FILE,
        CONFIG_MAIL_FILE,
        CONFIG_API_FILE,
        CONFIG_AUTH_FILE,
        CONFIG_RUNTIME_FILE,
        CONFIG_RELAY_FILE,
        CONFIG_SETUP_FILE,
    };

    int deleted = 0;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (file_manager_exists(files[i])) {
            file_manager_delete(files[i]);
            deleted++;
        }
    }

    // Mail group dosyalarını da sil
    for (int i = 0; i < MAX_MAIL_GROUPS; i++) {
        char path[64];
        snprintf(path, sizeof(path), MAIL_GROUP_FILE_FMT, i);
        if (file_manager_exists(path)) {
            file_manager_delete(path);
            deleted++;
        }
    }

    ESP_LOGI(TAG, "Fabrika ayarlari tamamlandi (%d dosya silindi)", deleted);
    return ESP_OK;
}
