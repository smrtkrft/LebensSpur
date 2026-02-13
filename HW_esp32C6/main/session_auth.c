/**
 * Session Auth - Token Bazlı Oturum Yönetimi
 *
 * Bearer token birincil kimlik doğrulama yöntemi.
 * Login -> token al -> her istekte "Authorization: Bearer <token>" gönder.
 * Cookie fallback olarak desteklenir.
 *
 * Bağımlılık: config_manager (Katman 2)
 * Katman: 2 (Yapılandırma)
 */

#include "session_auth.h"
#include "config_manager.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "SESSION";

static session_t s_sessions[SESSION_MAX_COUNT];
static bool s_initialized = false;
static auth_config_t s_auth;
static uint32_t s_timeout_sec = 60 * 60;   // Varsayılan 60 dk

// ============================================================================
// Yardımcı
// ============================================================================

static uint32_t uptime_sec(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static void generate_token(char *out)
{
    uint8_t bytes[SESSION_TOKEN_LEN / 2];   // 16 byte
    esp_fill_random(bytes, sizeof(bytes));

    for (int i = 0; i < (int)sizeof(bytes); i++) {
        sprintf(&out[i * 2], "%02x", bytes[i]);
    }
    out[SESSION_TOKEN_LEN] = '\0';
}

static session_t *find_by_token(const char *token)
{
    if (!token || token[0] == '\0') return NULL;

    for (int i = 0; i < SESSION_MAX_COUNT; i++) {
        if (s_sessions[i].valid && strcmp(s_sessions[i].token, token) == 0) {
            return &s_sessions[i];
        }
    }
    return NULL;
}

static void cleanup_expired(void)
{
    uint32_t now = uptime_sec();

    for (int i = 0; i < SESSION_MAX_COUNT; i++) {
        if (s_sessions[i].valid && (now - s_sessions[i].last_access) > s_timeout_sec) {
            ESP_LOGD(TAG, "Timeout: slot %d", i);
            memset(&s_sessions[i], 0, sizeof(session_t));
        }
    }
}

static int find_free_slot(void)
{
    cleanup_expired();

    // Boş slot ara
    for (int i = 0; i < SESSION_MAX_COUNT; i++) {
        if (!s_sessions[i].valid) return i;
    }

    // Yer yok - en eski oturumu sil
    int oldest = 0;
    uint32_t oldest_time = s_sessions[0].last_access;

    for (int i = 1; i < SESSION_MAX_COUNT; i++) {
        if (s_sessions[i].last_access < oldest_time) {
            oldest_time = s_sessions[i].last_access;
            oldest = i;
        }
    }

    ESP_LOGW(TAG, "Oturum limiti, en eski siliniyor: slot %d", oldest);
    memset(&s_sessions[oldest], 0, sizeof(session_t));
    return oldest;
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t session_auth_init(void)
{
    if (s_initialized) return ESP_OK;

    memset(s_sessions, 0, sizeof(s_sessions));

    // Auth config yükle
    esp_err_t ret = config_load_auth(&s_auth);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Auth config yuklenemedi, varsayilan");
    }

    // Timeout hesapla (config'de dakika, biz saniye tutuyoruz)
    s_timeout_sec = s_auth.session_timeout_min * 60;
    if (s_timeout_sec == 0) {
        s_timeout_sec = 3600;   // Minimum 1 saat
    }

    s_initialized = true;
    ESP_LOGI(TAG, "OK - timeout=%lu dk, sifre=%s",
             s_auth.session_timeout_min,
             s_auth.password[0] ? "ayarli" : "YOK");
    return ESP_OK;
}

bool session_check_password(const char *password)
{
    if (!s_initialized || !password) return false;

    if (s_auth.password[0] == '\0') {
        ESP_LOGW(TAG, "Sifre henuz ayarlanmamis");
        return false;
    }

    if (strcmp(password, s_auth.password) == 0) {
        ESP_LOGI(TAG, "Giris basarili");
        return true;
    }

    ESP_LOGW(TAG, "Giris basarisiz");
    return false;
}

bool session_has_password(void)
{
    return s_initialized && s_auth.password[0] != '\0';
}

esp_err_t session_create(char *token_out)
{
    if (!token_out) return ESP_ERR_INVALID_ARG;

    int slot = find_free_slot();
    session_t *s = &s_sessions[slot];
    uint32_t now = uptime_sec();

    generate_token(s->token);
    s->created_at = now;
    s->last_access = now;
    s->valid = true;

    strcpy(token_out, s->token);

    ESP_LOGI(TAG, "Oturum olusturuldu: slot %d", slot);
    return ESP_OK;
}

bool session_validate(const char *token)
{
    if (!token || token[0] == '\0') return false;

    session_t *s = find_by_token(token);
    if (!s) return false;

    uint32_t now = uptime_sec();
    if ((now - s->last_access) > s_timeout_sec) {
        ESP_LOGD(TAG, "Oturum timeout");
        s->valid = false;
        return false;
    }

    // Erişim zamanını güncelle (sliding window)
    s->last_access = now;
    return true;
}

void session_destroy(const char *token)
{
    session_t *s = find_by_token(token);
    if (s) {
        memset(s, 0, sizeof(session_t));
        ESP_LOGI(TAG, "Oturum sonlandirildi");
    }
}

void session_destroy_all(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
    ESP_LOGW(TAG, "Tum oturumlar temizlendi");
}

int session_get_active_count(void)
{
    cleanup_expired();
    int count = 0;
    for (int i = 0; i < SESSION_MAX_COUNT; i++) {
        if (s_sessions[i].valid) count++;
    }
    return count;
}

// ============================================================================
// Token Çıkarma (HTTP Header Parsing)
// ============================================================================

bool session_extract_bearer_token(const char *auth_header, char *token_out)
{
    if (!auth_header || !token_out) return false;
    token_out[0] = '\0';

    // "Bearer " prefix kontrolü (7 karakter)
    if (strncmp(auth_header, "Bearer ", 7) != 0) return false;

    const char *tok = auth_header + 7;

    // Boşlukları atla
    while (*tok == ' ') tok++;

    // Token uzunluğu kontrolü
    size_t len = strlen(tok);
    if (len != SESSION_TOKEN_LEN) return false;

    // Hex karakter kontrolü
    for (size_t i = 0; i < len; i++) {
        char c = tok[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }

    memcpy(token_out, tok, SESSION_TOKEN_LEN);
    token_out[SESSION_TOKEN_LEN] = '\0';
    return true;
}

bool session_extract_cookie_token(const char *cookie_header, char *token_out)
{
    if (!cookie_header || !token_out) return false;
    token_out[0] = '\0';

    // "ls_token=" ara
    const char *search = SESSION_COOKIE_NAME "=";
    const char *pos = strstr(cookie_header, search);
    if (!pos) return false;

    pos += strlen(search);

    // Token'ı kopyala (';' veya string sonuna kadar)
    int i = 0;
    while (*pos && *pos != ';' && *pos != ' ' && i < SESSION_TOKEN_LEN) {
        token_out[i++] = *pos++;
    }
    token_out[i] = '\0';

    if (i != SESSION_TOKEN_LEN) {
        token_out[0] = '\0';
        return false;
    }
    return true;
}

bool session_extract_token(const char *auth_header, const char *cookie_header, char *token_out)
{
    if (!token_out) return false;

    // Öncelik 1: Bearer token
    if (auth_header && session_extract_bearer_token(auth_header, token_out)) {
        return true;
    }

    // Öncelik 2: Cookie fallback
    if (cookie_header && session_extract_cookie_token(cookie_header, token_out)) {
        return true;
    }

    token_out[0] = '\0';
    return false;
}

// ============================================================================
// Cookie Formatları (Login/Logout Response)
// ============================================================================

int session_format_cookie(const char *token, char *buffer, size_t size)
{
    if (!token || !buffer || size < 80) return 0;

    return snprintf(buffer, size,
        "%s=%s; Path=/; Max-Age=%lu; HttpOnly; SameSite=Strict",
        SESSION_COOKIE_NAME, token, s_timeout_sec);
}

int session_format_logout_cookie(char *buffer, size_t size)
{
    if (!buffer || size < 60) return 0;

    return snprintf(buffer, size,
        "%s=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict",
        SESSION_COOKIE_NAME);
}

// ============================================================================
// Şifre Yönetimi
// ============================================================================

esp_err_t session_change_password(const char *current, const char *new_pass)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!current || !new_pass) return ESP_ERR_INVALID_ARG;

    // Mevcut şifreyi doğrula
    if (!session_check_password(current)) {
        ESP_LOGW(TAG, "Sifre degistirme: mevcut sifre yanlis");
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(new_pass) < SESSION_MIN_PASSWORD) {
        ESP_LOGW(TAG, "Sifre degistirme: yeni sifre cok kisa (min %d)", SESSION_MIN_PASSWORD);
        return ESP_ERR_INVALID_SIZE;
    }

    strncpy(s_auth.password, new_pass, sizeof(s_auth.password) - 1);
    s_auth.password[sizeof(s_auth.password) - 1] = '\0';

    esp_err_t ret = config_save_auth(&s_auth);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Sifre degistirildi");
    }
    return ret;
}

esp_err_t session_set_initial_password(const char *password)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!password) return ESP_ERR_INVALID_ARG;

    // Setup tamamlanmissa ve sifre varsa reddet
    // Setup tamamlanmamissa (ilk kurulum) sifre degistirilebilir
    if (s_auth.password[0] != '\0' && config_is_setup_completed()) {
        ESP_LOGW(TAG, "Sifre zaten ayarli, set_initial reddedildi");
        return ESP_ERR_INVALID_STATE;
    }

    if (strlen(password) < SESSION_MIN_PASSWORD) {
        return ESP_ERR_INVALID_SIZE;
    }

    strncpy(s_auth.password, password, sizeof(s_auth.password) - 1);
    s_auth.password[sizeof(s_auth.password) - 1] = '\0';

    esp_err_t ret = config_save_auth(&s_auth);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Ilk sifre ayarlandi");
    }
    return ret;
}

uint32_t session_get_timeout_sec(void)
{
    return s_timeout_sec;
}
