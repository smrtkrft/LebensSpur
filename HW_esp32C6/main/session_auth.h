/**
 * Session Auth - Token Bazlı Oturum Yönetimi
 *
 * Bearer token ile kimlik doğrulama.
 * Login sonrası token üretilir, client her istekte
 * "Authorization: Bearer <token>" header'ı gönderir.
 * Cookie fallback desteklenir (eski tarayıcılar için).
 *
 * Session'lar RAM'de tutulur (restart'ta kaybolur).
 * Timeout config'den okunur (varsayılan 60 dk).
 *
 * Bağımlılık: config_manager (Katman 2)
 * Katman: 2 (Yapılandırma)
 */

#ifndef SESSION_AUTH_H
#define SESSION_AUTH_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================
// SABİTLER
// ============================================
#define SESSION_TOKEN_LEN       32      // 16 byte = 32 hex karakter
#define SESSION_MAX_COUNT       4       // Maksimum eşzamanlı oturum
#define SESSION_COOKIE_NAME     "ls_token"
#define SESSION_MIN_PASSWORD    1       // Minimum şifre uzunluğu

// ============================================
// SESSION YAPISI
// ============================================
typedef struct {
    char token[SESSION_TOKEN_LEN + 1];
    uint32_t created_at;        // Oluşturulma (uptime saniye)
    uint32_t last_access;       // Son erişim (uptime saniye)
    bool valid;
} session_t;

// ============================================
// FONKSİYON PROTOTİPLERİ
// ============================================

/**
 * Session sistemini başlat (config_manager_init sonrası)
 */
esp_err_t session_auth_init(void);

/**
 * Şifre doğrula
 * @return true: şifre doğru
 */
bool session_check_password(const char *password);

/**
 * Şifre ayarlanmış mı kontrol et
 * @return true: şifre mevcut (boş değil)
 */
bool session_has_password(void);

/**
 * Yeni session oluştur, token döndür
 * @param token_out min SESSION_TOKEN_LEN+1 boyutunda buffer
 * @return ESP_OK: başarılı
 */
esp_err_t session_create(char *token_out);

/**
 * Token geçerli mi kontrol et (timeout dahil)
 * Geçerliyse last_access güncellenir
 * @return true: geçerli oturum
 */
bool session_validate(const char *token);

/**
 * Oturumu sonlandır
 */
void session_destroy(const char *token);

/**
 * Tüm oturumları temizle
 */
void session_destroy_all(void);

/**
 * Aktif oturum sayısı
 */
int session_get_active_count(void);

/**
 * HTTP Authorization header'ından token çıkar
 * Format: "Bearer <token>"
 * @param auth_header Authorization header değeri
 * @param token_out Token buffer (min SESSION_TOKEN_LEN+1)
 * @return true: token bulundu
 */
bool session_extract_bearer_token(const char *auth_header, char *token_out);

/**
 * HTTP Cookie header'ından token çıkar (fallback)
 * Format: "ls_token=<token>; ..."
 * @param cookie_header Cookie header değeri
 * @param token_out Token buffer (min SESSION_TOKEN_LEN+1)
 * @return true: token bulundu
 */
bool session_extract_cookie_token(const char *cookie_header, char *token_out);

/**
 * HTTP request'ten token çıkar (önce Bearer, sonra Cookie)
 * web_server kolaylık fonksiyonu
 * @param auth_header Authorization header (NULL olabilir)
 * @param cookie_header Cookie header (NULL olabilir)
 * @param token_out Token buffer (min SESSION_TOKEN_LEN+1)
 * @return true: token bulundu
 */
bool session_extract_token(const char *auth_header, const char *cookie_header, char *token_out);

/**
 * Set-Cookie header oluştur (login response için)
 * @return yazılan karakter sayısı
 */
int session_format_cookie(const char *token, char *buffer, size_t size);

/**
 * Cookie silme header'ı oluştur (logout için)
 * @return yazılan karakter sayısı
 */
int session_format_logout_cookie(char *buffer, size_t size);

/**
 * Şifre değiştir
 * @param current Mevcut şifre (doğrulama)
 * @param new_pass Yeni şifre
 * @return ESP_OK, ESP_ERR_INVALID_ARG (yanlış şifre), ESP_ERR_INVALID_SIZE (çok kısa)
 */
esp_err_t session_change_password(const char *current, const char *new_pass);

/**
 * İlk kurulumda şifre ayarla (şifre henüz yoksa)
 * @return ESP_OK, ESP_ERR_INVALID_STATE (şifre zaten var)
 */
esp_err_t session_set_initial_password(const char *password);

/**
 * Session timeout değerini al (saniye)
 */
uint32_t session_get_timeout_sec(void);

#ifdef __cplusplus
}
#endif

#endif // SESSION_AUTH_H
