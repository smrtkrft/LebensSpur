/**
 * @file session_auth.h
 * @brief Session-based Authentication
 * 
 * Provides:
 * - Session token generation
 * - Token validation
 * - Cookie management
 * - Rate limiting (via config_manager)
 */

#ifndef SESSION_AUTH_H
#define SESSION_AUTH_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * CONFIGURATION
 * ============================================ */
#define SESSION_TOKEN_LEN       32          // Length of session token (hex chars)
#define SESSION_MAX_ACTIVE      4           // Max concurrent sessions
#define SESSION_COOKIE_NAME     "LS_SID"    // Session cookie name

/* ============================================
 * SESSION STRUCTURE
 * ============================================ */
typedef struct {
    char token[SESSION_TOKEN_LEN + 1];
    int64_t created_at;         // Unix timestamp (ms)
    int64_t last_access;        // Last activity timestamp
    int64_t expires_at;         // Expiration timestamp
    char ip_address[16];        // Client IP
    char user_agent[64];        // User agent (truncated)
    bool valid;                 // Is session active
} session_t;

/* ============================================
 * INITIALIZATION
 * ============================================ */

/**
 * @brief Initialize session manager
 */
esp_err_t session_auth_init(void);

/**
 * @brief Deinitialize session manager
 */
void session_auth_deinit(void);

/* ============================================
 * AUTHENTICATION
 * ============================================ */

/**
 * @brief Attempt login with password
 * @param password User-provided password
 * @param ip_address Client IP (for logging)
 * @param user_agent Client user agent
 * @param out_token Output: session token if success
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for wrong password
 */
esp_err_t session_auth_login(const char *password, 
                              const char *ip_address,
                              const char *user_agent,
                              char *out_token);

/**
 * @brief Logout (invalidate session)
 * @param token Session token to invalidate
 */
esp_err_t session_auth_logout(const char *token);

/**
 * @brief Logout all sessions
 */
esp_err_t session_auth_logout_all(void);

/* ============================================
 * SESSION VALIDATION
 * ============================================ */

/**
 * @brief Validate session token
 * @param token Session token
 * @return true if valid and not expired
 */
bool session_auth_validate(const char *token);

/**
 * @brief Refresh session (update last access time)
 * @param token Session token
 * @return ESP_OK on success
 */
esp_err_t session_auth_refresh(const char *token);

/**
 * @brief Get session info
 * @param token Session token
 * @param session Output session info
 * @return ESP_OK if found
 */
esp_err_t session_auth_get_session(const char *token, session_t *session);

/* ============================================
 * SESSION MANAGEMENT
 * ============================================ */

/**
 * @brief Get number of active sessions
 */
int session_auth_count(void);

/**
 * @brief Cleanup expired sessions
 */
void session_auth_cleanup(void);

/**
 * @brief Get remaining login attempts before lockout
 * @return Number of attempts remaining, 0 if locked out
 */
int session_auth_remaining_attempts(void);

/**
 * @brief Get lockout remaining time in seconds
 * @return Seconds until lockout ends, 0 if not locked
 */
int session_auth_lockout_remaining(void);

/* ============================================
 * COOKIE HELPERS
 * ============================================ */

/**
 * @brief Generate Set-Cookie header value
 * @param token Session token
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 */
void session_auth_cookie_header(const char *token, char *buffer, size_t buffer_size);

/**
 * @brief Extract session token from Cookie header
 * @param cookie_header Full Cookie header string
 * @param out_token Output token buffer (SESSION_TOKEN_LEN + 1)
 * @return ESP_OK if found
 */
esp_err_t session_auth_extract_token(const char *cookie_header, char *out_token);

/**
 * @brief Generate logout cookie (expired)
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 */
void session_auth_logout_cookie(char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif // SESSION_AUTH_H
