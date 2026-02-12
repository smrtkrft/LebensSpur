/**
 * @file session_auth.c
 * @brief Session Authentication Implementation
 */

#include "session_auth.h"
#include "config_manager.h"
#include "log_manager.h"
#include "time_manager.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "session";

/* ============================================
 * SESSION STORAGE
 * ============================================ */
static session_t s_sessions[SESSION_MAX_ACTIVE];
static int s_session_timeout_min = 30;  // Default

/* ============================================
 * PRIVATE FUNCTIONS
 * ============================================ */

static void generate_token(char *token)
{
    uint8_t random_bytes[SESSION_TOKEN_LEN / 2];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    
    for (int i = 0; i < sizeof(random_bytes); i++) {
        sprintf(&token[i * 2], "%02x", random_bytes[i]);
    }
    token[SESSION_TOKEN_LEN] = '\0';
}

static session_t* find_session(const char *token)
{
    if (!token || strlen(token) != SESSION_TOKEN_LEN) {
        return NULL;
    }
    
    for (int i = 0; i < SESSION_MAX_ACTIVE; i++) {
        if (s_sessions[i].valid && 
            strcmp(s_sessions[i].token, token) == 0) {
            return &s_sessions[i];
        }
    }
    return NULL;
}

static session_t* find_empty_slot(void)
{
    // First, look for invalid sessions
    for (int i = 0; i < SESSION_MAX_ACTIVE; i++) {
        if (!s_sessions[i].valid) {
            return &s_sessions[i];
        }
    }
    
    // If all slots used, find oldest session
    session_t *oldest = &s_sessions[0];
    for (int i = 1; i < SESSION_MAX_ACTIVE; i++) {
        if (s_sessions[i].last_access < oldest->last_access) {
            oldest = &s_sessions[i];
        }
    }
    
    ESP_LOGW(TAG, "Evicting old session from %s", oldest->ip_address);
    return oldest;
}

static bool is_session_expired(const session_t *session)
{
    // Use monotonic uptime to avoid NTP time-jump issues
    int64_t now = esp_timer_get_time() / 1000; // microseconds -> milliseconds
    return now >= session->expires_at;
}

/* ============================================
 * INITIALIZATION
 * ============================================ */

esp_err_t session_auth_init(void)
{
    ESP_LOGI(TAG, "Initializing session auth...");
    
    // Clear all sessions
    memset(s_sessions, 0, sizeof(s_sessions));
    
    // Load auth config
    auth_config_t auth;
    config_load_auth(&auth);
    s_session_timeout_min = auth.session_timeout_min;
    
    ESP_LOGI(TAG, "Session timeout: %d min, max sessions: %d", 
             s_session_timeout_min, SESSION_MAX_ACTIVE);
    
    return ESP_OK;
}

void session_auth_deinit(void)
{
    session_auth_logout_all();
}

/* ============================================
 * AUTHENTICATION
 * ============================================ */

esp_err_t session_auth_login(const char *password, 
                              const char *ip_address,
                              const char *user_agent,
                              char *out_token)
{
    if (!password || !out_token) {
        return ESP_ERR_INVALID_ARG;
    }
    
    const char *ip = ip_address ? ip_address : "unknown";
    
    // Check lockout
    if (session_auth_lockout_remaining() > 0) {
        LOG_SECURITY(LOG_LEVEL_WARN, ip, "Login blocked - account locked");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Verify password
    if (!config_verify_password(password)) {
        LOG_SECURITY(LOG_LEVEL_WARN, ip, "Login failed - wrong password");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Find slot and create session
    session_t *session = find_empty_slot();
    if (!session) {
        return ESP_ERR_NO_MEM;
    }
    
    // Generate token
    generate_token(session->token);
    
    // Use monotonic uptime for session timing (immune to NTP jumps)
    int64_t now = esp_timer_get_time() / 1000;
    session->created_at = now;
    session->last_access = now;
    session->expires_at = now + ((int64_t)s_session_timeout_min * 60 * 1000);
    session->valid = true;
    
    // Store client info
    strncpy(session->ip_address, ip, sizeof(session->ip_address) - 1);
    session->ip_address[sizeof(session->ip_address) - 1] = '\0';
    
    if (user_agent) {
        strncpy(session->user_agent, user_agent, sizeof(session->user_agent) - 1);
        session->user_agent[sizeof(session->user_agent) - 1] = '\0';
    } else {
        session->user_agent[0] = '\0';
    }
    
    // Copy token to output
    strcpy(out_token, session->token);
    
    LOG_SECURITY(LOG_LEVEL_INFO, ip, "Login successful");
    ESP_LOGI(TAG, "New session created for %s", ip);
    
    return ESP_OK;
}

esp_err_t session_auth_logout(const char *token)
{
    session_t *session = find_session(token);
    if (!session) {
        return ESP_ERR_NOT_FOUND;
    }
    
    LOG_SECURITY(LOG_LEVEL_INFO, session->ip_address, "Logout");
    
    memset(session, 0, sizeof(session_t));
    return ESP_OK;
}

esp_err_t session_auth_logout_all(void)
{
    ESP_LOGI(TAG, "Logging out all sessions");
    
    for (int i = 0; i < SESSION_MAX_ACTIVE; i++) {
        if (s_sessions[i].valid) {
            memset(&s_sessions[i], 0, sizeof(session_t));
        }
    }
    
    return ESP_OK;
}

/* ============================================
 * SESSION VALIDATION
 * ============================================ */

bool session_auth_validate(const char *token)
{
    session_t *session = find_session(token);
    if (!session) {
        return false;
    }
    
    if (is_session_expired(session)) {
        ESP_LOGD(TAG, "Session expired for %s", session->ip_address);
        session->valid = false;
        return false;
    }
    
    return true;
}

esp_err_t session_auth_refresh(const char *token)
{
    session_t *session = find_session(token);
    if (!session) {
        return ESP_ERR_NOT_FOUND;
    }
    
    if (is_session_expired(session)) {
        session->valid = false;
        return ESP_ERR_INVALID_STATE;
    }
    
    // Use monotonic uptime
    int64_t now = esp_timer_get_time() / 1000;
    session->last_access = now;
    session->expires_at = now + ((int64_t)s_session_timeout_min * 60 * 1000);
    
    return ESP_OK;
}

esp_err_t session_auth_get_session(const char *token, session_t *session)
{
    if (!session) return ESP_ERR_INVALID_ARG;
    
    session_t *found = find_session(token);
    if (!found) {
        return ESP_ERR_NOT_FOUND;
    }
    
    *session = *found;
    return ESP_OK;
}

/* ============================================
 * SESSION MANAGEMENT
 * ============================================ */

int session_auth_count(void)
{
    int count = 0;
    for (int i = 0; i < SESSION_MAX_ACTIVE; i++) {
        if (s_sessions[i].valid && !is_session_expired(&s_sessions[i])) {
            count++;
        }
    }
    return count;
}

void session_auth_cleanup(void)
{
    for (int i = 0; i < SESSION_MAX_ACTIVE; i++) {
        if (s_sessions[i].valid && is_session_expired(&s_sessions[i])) {
            ESP_LOGD(TAG, "Cleaning up expired session from %s", 
                     s_sessions[i].ip_address);
            memset(&s_sessions[i], 0, sizeof(session_t));
        }
    }
}

int session_auth_remaining_attempts(void)
{
    auth_config_t auth;
    config_load_auth(&auth);
    
    if (config_is_locked_out()) {
        return 0;
    }
    
    int remaining = auth.max_login_attempts - auth.failed_attempts;
    return (remaining > 0) ? remaining : 0;
}

int session_auth_lockout_remaining(void)
{
    auth_config_t auth;
    config_load_auth(&auth);
    
    if (auth.lockout_until == 0) {
        return 0;
    }
    
    int64_t now = time_manager_get_timestamp_ms();
    if (now >= auth.lockout_until) {
        return 0;
    }
    
    return (int)((auth.lockout_until - now) / 1000);
}

/* ============================================
 * COOKIE HELPERS
 * ============================================ */

void session_auth_cookie_header(const char *token, char *buffer, size_t buffer_size)
{
    if (!token || !buffer || buffer_size == 0) return;
    
    // Set cookie with HttpOnly, SameSite=Lax, Path=/
    snprintf(buffer, buffer_size,
             "%s=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d",
             SESSION_COOKIE_NAME,
             token,
             s_session_timeout_min * 60);
}

esp_err_t session_auth_extract_token(const char *cookie_header, char *out_token)
{
    if (!cookie_header || !out_token) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Look for "LS_SID="
    char search[32];
    snprintf(search, sizeof(search), "%s=", SESSION_COOKIE_NAME);
    
    const char *start = strstr(cookie_header, search);
    if (!start) {
        return ESP_ERR_NOT_FOUND;
    }
    
    start += strlen(search);
    
    // Copy token until ; or end of string
    int i = 0;
    while (*start && *start != ';' && *start != ' ' && i < SESSION_TOKEN_LEN) {
        out_token[i++] = *start++;
    }
    out_token[i] = '\0';
    
    if (i != SESSION_TOKEN_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    
    return ESP_OK;
}

void session_auth_logout_cookie(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) return;
    
    // Expired cookie to clear
    snprintf(buffer, buffer_size,
             "%s=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0; Expires=Thu, 01 Jan 1970 00:00:00 GMT",
             SESSION_COOKIE_NAME);
}
