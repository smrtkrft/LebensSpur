/*
 * smtp_client.c – SMTP client (STARTTLS port 587 + Implicit TLS port 465)
 * lwip soketleri + mbedtls SSL ile kesin calisan cozum
 */
#include "smtp_client.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/base64.h"

static const char *TAG = "smtp";

#define NVS_NAMESPACE   "ls_smtp"
#define SMTP_BUF_SIZE   1024

static smtp_config_t s_config;
static bool s_configured = false;

/* ── NVS ─────────────────────────────────────────────── */

static esp_err_t load_from_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t len;

    len = sizeof(s_config.server);
    err = nvs_get_str(h, "server", s_config.server, &len);
    if (err != ESP_OK) { nvs_close(h); return err; }

    err = nvs_get_u16(h, "port", &s_config.port);
    if (err != ESP_OK) { nvs_close(h); return err; }

    len = sizeof(s_config.user);
    err = nvs_get_str(h, "user", s_config.user, &len);
    if (err != ESP_OK) { nvs_close(h); return err; }

    len = sizeof(s_config.api_key);
    err = nvs_get_str(h, "api_key", s_config.api_key, &len);
    if (err != ESP_OK) { nvs_close(h); return err; }

    nvs_close(h);
    s_configured = true;
    return ESP_OK;
}

static esp_err_t save_to_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, "server", s_config.server);
    nvs_set_u16(h, "port", s_config.port);
    nvs_set_str(h, "user", s_config.user);
    nvs_set_str(h, "api_key", s_config.api_key);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────── */

esp_err_t smtp_client_init(void)
{
    memset(&s_config, 0, sizeof(s_config));
    s_configured = false;
    if (load_from_nvs() == ESP_OK) {
        ESP_LOGI(TAG, "SMTP config yuklendi: %s:%d user=%s",
                 s_config.server, s_config.port, s_config.user);
    } else {
        ESP_LOGI(TAG, "SMTP config yok, yapilandirilmadi");
    }
    return ESP_OK;
}

esp_err_t smtp_client_save_config(const smtp_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memcpy(&s_config, cfg, sizeof(s_config));
    s_configured = true;
    ESP_LOGI(TAG, "SMTP config kaydedildi: %s:%d", s_config.server, s_config.port);
    return save_to_nvs();
}

esp_err_t smtp_client_get_config(smtp_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memcpy(cfg, &s_config, sizeof(s_config));
    return ESP_OK;
}

bool smtp_client_has_config(void)
{
    return s_configured;
}

/* ── lwip soket yardimcilari ─────────────────────────── */

static int sock_read(int fd, char *buf, int bufsz)
{
    int len = recv(fd, buf, bufsz - 1, 0);
    if (len > 0) {
        buf[len] = '\0';
        ESP_LOGI(TAG, "<<< %s", buf);
    } else {
        ESP_LOGE(TAG, "<<< soket okuma hatasi: errno=%d", errno);
    }
    return len;
}

static int sock_write(int fd, const char *data, int len)
{
    ESP_LOGI(TAG, ">>> %.*s", len > 64 ? 64 : len, data);
    int sent = 0;
    while (sent < len) {
        int ret = send(fd, data + sent, len - sent, 0);
        if (ret < 0) {
            ESP_LOGE(TAG, "soket yazma hatasi: errno=%d", errno);
            return ret;
        }
        sent += ret;
    }
    return sent;
}

static int sock_command(int fd, const char *cmd, char *buf, int bufsz)
{
    if (sock_write(fd, cmd, strlen(cmd)) < 0) return -1;
    return sock_read(fd, buf, bufsz);
}

/* ── mbedtls SSL yardimcilari ────────────────────────── */

static int ssl_send_cb(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *((int *)ctx);
    int ret = send(fd, buf, len, 0);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        return -1;
    }
    return ret;
}

static int ssl_recv_cb(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *((int *)ctx);
    int ret = recv(fd, buf, len, 0);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return MBEDTLS_ERR_SSL_WANT_READ;
        return -1;
    }
    return ret;
}

static int ssl_read_response(mbedtls_ssl_context *ssl, char *buf, int bufsz)
{
    int len = mbedtls_ssl_read(ssl, (unsigned char *)buf, bufsz - 1);
    if (len > 0) {
        buf[len] = '\0';
        ESP_LOGI(TAG, "<<< %s", buf);
    } else if (len == MBEDTLS_ERR_SSL_WANT_READ || len == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    } else {
        ESP_LOGE(TAG, "<<< SSL okuma hatasi: -0x%x", -len);
    }
    return len;
}

static int ssl_write_data(mbedtls_ssl_context *ssl, const char *data, int len)
{
    ESP_LOGI(TAG, ">>> %.*s", len > 64 ? 64 : len, data);
    int written = 0;
    while (written < len) {
        int ret = mbedtls_ssl_write(ssl, (const unsigned char *)(data + written), len - written);
        if (ret < 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                ESP_LOGE(TAG, "SSL yazma hatasi: -0x%x", -ret);
                return ret;
            }
            continue;
        }
        written += ret;
    }
    return written;
}

static int ssl_command(mbedtls_ssl_context *ssl, const char *cmd, char *buf, int bufsz)
{
    if (ssl_write_data(ssl, cmd, strlen(cmd)) < 0) return -1;
    return ssl_read_response(ssl, buf, bufsz);
}

/* ── Base64 ──────────────────────────────────────────── */

static esp_err_t base64_encode_str(const char *input, char *output, size_t out_size)
{
    size_t olen = 0;
    int ret = mbedtls_base64_encode((unsigned char *)output, out_size, &olen,
                                     (const unsigned char *)input, strlen(input));
    if (ret != 0) return ESP_FAIL;
    output[olen] = '\0';
    return ESP_OK;
}

/* ── TCP baglanti ────────────────────────────────────── */

static int smtp_tcp_connect(const char *host, uint16_t port)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    ESP_LOGI(TAG, "DNS cozumleniyor: %s", host);

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *addr_list = NULL;
    int ret = getaddrinfo(host, port_str, &hints, &addr_list);
    if (ret != 0 || !addr_list) {
        ESP_LOGE(TAG, "DNS cozumleme hatasi: %d (sunucu: %s)", ret, host);
        return -1;
    }

    char ip_str[16];
    struct sockaddr_in *addr4 = (struct sockaddr_in *)addr_list->ai_addr;
    inet_ntoa_r(addr4->sin_addr, ip_str, sizeof(ip_str));
    ESP_LOGI(TAG, "DNS basarili: %s -> %s", host, ip_str);

    int fd = socket(addr_list->ai_family, addr_list->ai_socktype, addr_list->ai_protocol);
    if (fd < 0) {
        ESP_LOGE(TAG, "Soket olusturulamadi: errno=%d", errno);
        freeaddrinfo(addr_list);
        return -1;
    }

    // Timeout ayarla
    struct timeval tv;
    tv.tv_sec = 15;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "TCP baglaniyor: %s:%d", ip_str, port);
    ret = connect(fd, addr_list->ai_addr, addr_list->ai_addrlen);
    freeaddrinfo(addr_list);

    if (ret != 0) {
        ESP_LOGE(TAG, "TCP baglanti hatasi: errno=%d", errno);
        close(fd);
        return -1;
    }

    ESP_LOGI(TAG, "TCP baglanti basarili");
    return fd;
}

/* ── SMTP gonderim ───────────────────────────────────── */

esp_err_t smtp_client_send(const char *to, const char *subject, const char *body)
{
    if (!s_configured) {
        ESP_LOGW(TAG, "SMTP yapilandirilmamis");
        return ESP_ERR_INVALID_STATE;
    }

    bool use_starttls = (s_config.port != 465);

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    char *buf = NULL;
    char b64[128];
    esp_err_t err = ESP_OK;
    int ret;
    int sockfd = -1;

    /* Seed RNG */
    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "RNG seed hatasi: -0x%x", -ret);
        err = ESP_FAIL; goto cleanup;
    }

    /* SSL config */
    ret = mbedtls_ssl_config_defaults(&conf,
                                       MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        ESP_LOGE(TAG, "SSL config hatasi: -0x%x", -ret);
        err = ESP_FAIL; goto cleanup;
    }

    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

    /* Cert bundle (ESP-IDF dahili sertifikalar) */
    esp_crt_bundle_attach(&conf);

    ret = mbedtls_ssl_set_hostname(&ssl, s_config.server);
    if (ret != 0) {
        ESP_LOGE(TAG, "SSL hostname hatasi: -0x%x", -ret);
        err = ESP_FAIL; goto cleanup;
    }

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) {
        ESP_LOGE(TAG, "SSL setup hatasi: -0x%x", -ret);
        err = ESP_FAIL; goto cleanup;
    }

    /* Buffer */
    buf = (char *)calloc(1, SMTP_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Bellek yetersiz");
        err = ESP_ERR_NO_MEM; goto cleanup;
    }

    /* TCP baglanti (lwip soketiyle) */
    ESP_LOGI(TAG, "=== SMTP Baslaniyor (%s) ===", use_starttls ? "STARTTLS" : "Implicit TLS");

    sockfd = smtp_tcp_connect(s_config.server, s_config.port);
    if (sockfd < 0) {
        ESP_LOGE(TAG, "TCP baglanti kurulamadi");
        err = ESP_FAIL; goto cleanup;
    }

    /* SSL BIO: lwip soketini SSL'e bagla */
    mbedtls_ssl_set_bio(&ssl, &sockfd, ssl_send_cb, ssl_recv_cb, NULL);

    if (use_starttls) {
        /* ── STARTTLS modu (port 587) ──────────────────── */

        /* Greeting (plain TCP) */
        ESP_LOGI(TAG, "[1/10] Greeting bekleniyor...");
        if (sock_read(sockfd, buf, SMTP_BUF_SIZE) <= 0) {
            ESP_LOGE(TAG, "Greeting alinamadi");
            err = ESP_FAIL; goto cleanup;
        }

        /* EHLO (plain TCP) */
        ESP_LOGI(TAG, "[2/10] EHLO gonderiliyor (plain)...");
        if (sock_command(sockfd, "EHLO lebensspur\r\n", buf, SMTP_BUF_SIZE) <= 0) {
            ESP_LOGE(TAG, "EHLO basarisiz");
            err = ESP_FAIL; goto cleanup;
        }

        /* STARTTLS komutu */
        ESP_LOGI(TAG, "[3/10] STARTTLS gonderiliyor...");
        if (sock_command(sockfd, "STARTTLS\r\n", buf, SMTP_BUF_SIZE) <= 0) {
            ESP_LOGE(TAG, "STARTTLS komutu basarisiz");
            err = ESP_FAIL; goto cleanup;
        }
        if (strncmp(buf, "220", 3) != 0) {
            ESP_LOGE(TAG, "STARTTLS reddedildi: %s", buf);
            err = ESP_FAIL; goto cleanup;
        }

        /* TLS handshake */
        ESP_LOGI(TAG, "[4/10] TLS handshake...");
        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                char errbuf[128];
                mbedtls_strerror(ret, errbuf, sizeof(errbuf));
                ESP_LOGE(TAG, "TLS handshake hatasi: -0x%x (%s)", -ret, errbuf);
                err = ESP_FAIL; goto cleanup;
            }
        }
        ESP_LOGI(TAG, "TLS basarili! Cipher: %s", mbedtls_ssl_get_ciphersuite(&ssl));

        /* EHLO (TLS uzerinden tekrar) */
        ESP_LOGI(TAG, "[5/10] EHLO gonderiliyor (TLS)...");
        if (ssl_command(&ssl, "EHLO lebensspur\r\n", buf, SMTP_BUF_SIZE) <= 0) {
            ESP_LOGE(TAG, "EHLO (TLS) basarisiz");
            err = ESP_FAIL; goto cleanup;
        }

    } else {
        /* ── Implicit TLS modu (port 465) ──────────────── */

        /* TLS handshake (hemen) */
        ESP_LOGI(TAG, "[1/8] TLS handshake...");
        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                char errbuf[128];
                mbedtls_strerror(ret, errbuf, sizeof(errbuf));
                ESP_LOGE(TAG, "TLS handshake hatasi: -0x%x (%s)", -ret, errbuf);
                err = ESP_FAIL; goto cleanup;
            }
        }
        ESP_LOGI(TAG, "TLS basarili! Cipher: %s", mbedtls_ssl_get_ciphersuite(&ssl));

        /* Greeting (TLS) */
        ESP_LOGI(TAG, "[2/8] Greeting bekleniyor...");
        if (ssl_read_response(&ssl, buf, SMTP_BUF_SIZE) <= 0) {
            ESP_LOGE(TAG, "Greeting alinamadi");
            err = ESP_FAIL; goto cleanup;
        }

        /* EHLO (TLS) */
        ESP_LOGI(TAG, "[3/8] EHLO gonderiliyor...");
        if (ssl_command(&ssl, "EHLO lebensspur\r\n", buf, SMTP_BUF_SIZE) <= 0) {
            ESP_LOGE(TAG, "EHLO basarisiz");
            err = ESP_FAIL; goto cleanup;
        }
    }

    /* ── AUTH LOGIN (her iki modda da TLS uzerinden) ── */
    ESP_LOGI(TAG, "AUTH LOGIN...");
    if (ssl_command(&ssl, "AUTH LOGIN\r\n", buf, SMTP_BUF_SIZE) <= 0) {
        ESP_LOGE(TAG, "AUTH LOGIN basarisiz");
        err = ESP_FAIL; goto cleanup;
    }

    /* Username (base64) */
    ESP_LOGI(TAG, "Username gonderiliyor...");
    if (base64_encode_str(s_config.user, b64, sizeof(b64)) != ESP_OK) {
        ESP_LOGE(TAG, "Username base64 encode hatasi");
        err = ESP_FAIL; goto cleanup;
    }
    strcat(b64, "\r\n");
    if (ssl_command(&ssl, b64, buf, SMTP_BUF_SIZE) <= 0) {
        ESP_LOGE(TAG, "Username gonderme hatasi");
        err = ESP_FAIL; goto cleanup;
    }

    /* Password (base64) */
    ESP_LOGI(TAG, "Password gonderiliyor...");
    if (base64_encode_str(s_config.api_key, b64, sizeof(b64)) != ESP_OK) {
        ESP_LOGE(TAG, "Password base64 encode hatasi");
        err = ESP_FAIL; goto cleanup;
    }
    strcat(b64, "\r\n");
    if (ssl_command(&ssl, b64, buf, SMTP_BUF_SIZE) <= 0) {
        ESP_LOGE(TAG, "Password gonderme hatasi");
        err = ESP_FAIL; goto cleanup;
    }
    if (strncmp(buf, "235", 3) != 0) {
        ESP_LOGE(TAG, "Auth basarisiz: %s", buf);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    ESP_LOGI(TAG, "Auth basarili!");

    /* MAIL FROM */
    ESP_LOGI(TAG, "MAIL FROM...");
    snprintf(buf, SMTP_BUF_SIZE, "MAIL FROM:<%s>\r\n", s_config.user);
    if (ssl_command(&ssl, buf, buf, SMTP_BUF_SIZE) <= 0) {
        ESP_LOGE(TAG, "MAIL FROM basarisiz");
        err = ESP_FAIL; goto cleanup;
    }

    /* RCPT TO */
    ESP_LOGI(TAG, "RCPT TO: %s", to);
    snprintf(buf, SMTP_BUF_SIZE, "RCPT TO:<%s>\r\n", to);
    if (ssl_command(&ssl, buf, buf, SMTP_BUF_SIZE) <= 0) {
        ESP_LOGE(TAG, "RCPT TO basarisiz");
        err = ESP_FAIL; goto cleanup;
    }

    /* DATA */
    ESP_LOGI(TAG, "DATA...");
    if (ssl_command(&ssl, "DATA\r\n", buf, SMTP_BUF_SIZE) <= 0) {
        ESP_LOGE(TAG, "DATA basarisiz");
        err = ESP_FAIL; goto cleanup;
    }

    /* Headers + Body */
    snprintf(buf, SMTP_BUF_SIZE,
             "From: LebensSpur <%s>\r\n"
             "To: <%s>\r\n"
             "Subject: %s\r\n"
             "Content-Type: text/plain; charset=UTF-8\r\n"
             "\r\n"
             "%s\r\n"
             ".\r\n",
             s_config.user, to, subject, body);
    if (ssl_command(&ssl, buf, buf, SMTP_BUF_SIZE) <= 0) {
        ESP_LOGE(TAG, "Mail icerigi gonderme hatasi");
        err = ESP_FAIL; goto cleanup;
    }
    if (strncmp(buf, "250", 3) != 0) {
        ESP_LOGW(TAG, "Mail gonderim sonucu: %s", buf);
    }

    /* QUIT */
    ssl_command(&ssl, "QUIT\r\n", buf, SMTP_BUF_SIZE);

    ESP_LOGI(TAG, "=== Mail basariyla gonderildi: %s -> %s ===", s_config.user, to);

cleanup:
    if (buf) free(buf);
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    if (sockfd >= 0) close(sockfd);
    return err;
}

/* ── MIME Content-Type belirleme ──────────────────────── */

static const char *get_mime_type(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";
    ext++; // noktayi atla
    if (strcasecmp(ext, "pdf") == 0) return "application/pdf";
    if (strcasecmp(ext, "txt") == 0) return "text/plain";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "gif") == 0) return "image/gif";
    if (strcasecmp(ext, "mp3") == 0) return "audio/mpeg";
    if (strcasecmp(ext, "wav") == 0) return "audio/wav";
    if (strcasecmp(ext, "mp4") == 0) return "video/mp4";
    if (strcasecmp(ext, "doc") == 0) return "application/msword";
    if (strcasecmp(ext, "docx") == 0) return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    return "application/octet-stream";
}

/* Dosya adindan sadece dosya ismini al (path'siz) */
static const char *get_basename(const char *path)
{
    const char *last = strrchr(path, '/');
    return last ? last + 1 : path;
}

/* ── Dosya ekli SMTP gonderim (MIME multipart) ──────── */

esp_err_t smtp_client_send_with_attachments(const char *to, const char *subject,
                                             const char *body,
                                             const char **file_paths, int file_count)
{
    // Ek yoksa normal gonder
    if (file_count <= 0 || !file_paths) {
        return smtp_client_send(to, subject, body);
    }

    if (!s_configured) {
        ESP_LOGW(TAG, "SMTP yapilandirilmamis");
        return ESP_ERR_INVALID_STATE;
    }

    bool use_starttls = (s_config.port != 465);

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    char *buf = NULL;
    char b64[128];
    esp_err_t err = ESP_OK;
    int ret;
    int sockfd = -1;

    /* ── Baglanti + Auth (smtp_client_send ile ayni) ── */

    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);
    if (ret != 0) { err = ESP_FAIL; goto cleanup; }

    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) { err = ESP_FAIL; goto cleanup; }

    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    esp_crt_bundle_attach(&conf);

    ret = mbedtls_ssl_set_hostname(&ssl, s_config.server);
    if (ret != 0) { err = ESP_FAIL; goto cleanup; }

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) { err = ESP_FAIL; goto cleanup; }

    buf = (char *)calloc(1, SMTP_BUF_SIZE);
    if (!buf) { err = ESP_ERR_NO_MEM; goto cleanup; }

    ESP_LOGI(TAG, "=== SMTP+MIME Baslaniyor (%s) ===", use_starttls ? "STARTTLS" : "Implicit TLS");

    sockfd = smtp_tcp_connect(s_config.server, s_config.port);
    if (sockfd < 0) { err = ESP_FAIL; goto cleanup; }

    mbedtls_ssl_set_bio(&ssl, &sockfd, ssl_send_cb, ssl_recv_cb, NULL);

    if (use_starttls) {
        if (sock_read(sockfd, buf, SMTP_BUF_SIZE) <= 0) { err = ESP_FAIL; goto cleanup; }
        if (sock_command(sockfd, "EHLO lebensspur\r\n", buf, SMTP_BUF_SIZE) <= 0) { err = ESP_FAIL; goto cleanup; }
        if (sock_command(sockfd, "STARTTLS\r\n", buf, SMTP_BUF_SIZE) <= 0) { err = ESP_FAIL; goto cleanup; }
        if (strncmp(buf, "220", 3) != 0) { err = ESP_FAIL; goto cleanup; }
        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            { err = ESP_FAIL; goto cleanup; }
        }
        if (ssl_command(&ssl, "EHLO lebensspur\r\n", buf, SMTP_BUF_SIZE) <= 0) { err = ESP_FAIL; goto cleanup; }
    } else {
        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            { err = ESP_FAIL; goto cleanup; }
        }
        if (ssl_read_response(&ssl, buf, SMTP_BUF_SIZE) <= 0) { err = ESP_FAIL; goto cleanup; }
        if (ssl_command(&ssl, "EHLO lebensspur\r\n", buf, SMTP_BUF_SIZE) <= 0) { err = ESP_FAIL; goto cleanup; }
    }

    /* AUTH LOGIN */
    if (ssl_command(&ssl, "AUTH LOGIN\r\n", buf, SMTP_BUF_SIZE) <= 0) { err = ESP_FAIL; goto cleanup; }
    if (base64_encode_str(s_config.user, b64, sizeof(b64)) != ESP_OK) { err = ESP_FAIL; goto cleanup; }
    strcat(b64, "\r\n");
    if (ssl_command(&ssl, b64, buf, SMTP_BUF_SIZE) <= 0) { err = ESP_FAIL; goto cleanup; }
    if (base64_encode_str(s_config.api_key, b64, sizeof(b64)) != ESP_OK) { err = ESP_FAIL; goto cleanup; }
    strcat(b64, "\r\n");
    if (ssl_command(&ssl, b64, buf, SMTP_BUF_SIZE) <= 0) { err = ESP_FAIL; goto cleanup; }
    if (strncmp(buf, "235", 3) != 0) { err = ESP_ERR_INVALID_ARG; goto cleanup; }

    /* MAIL FROM + RCPT TO + DATA */
    snprintf(buf, SMTP_BUF_SIZE, "MAIL FROM:<%s>\r\n", s_config.user);
    if (ssl_command(&ssl, buf, buf, SMTP_BUF_SIZE) <= 0) { err = ESP_FAIL; goto cleanup; }
    snprintf(buf, SMTP_BUF_SIZE, "RCPT TO:<%s>\r\n", to);
    if (ssl_command(&ssl, buf, buf, SMTP_BUF_SIZE) <= 0) { err = ESP_FAIL; goto cleanup; }
    if (ssl_command(&ssl, "DATA\r\n", buf, SMTP_BUF_SIZE) <= 0) { err = ESP_FAIL; goto cleanup; }

    /* ── MIME Multipart mesaj ────────────────────────── */

    #define MIME_BOUNDARY "----LS-MIME-BOUNDARY-2026"

    /* Mail header'lari */
    snprintf(buf, SMTP_BUF_SIZE,
             "From: LebensSpur <%s>\r\n"
             "To: <%s>\r\n"
             "Subject: %s\r\n"
             "MIME-Version: 1.0\r\n"
             "Content-Type: multipart/mixed; boundary=\"" MIME_BOUNDARY "\"\r\n"
             "\r\n"
             "--" MIME_BOUNDARY "\r\n"
             "Content-Type: text/plain; charset=\"UTF-8\"\r\n"
             "\r\n"
             "%s\r\n",
             s_config.user, to, subject, body);
    if (ssl_write_data(&ssl, buf, strlen(buf)) < 0) { err = ESP_FAIL; goto cleanup; }

    /* Her dosya icin MIME attachment part'i */
    for (int fi = 0; fi < file_count; fi++) {
        const char *fpath = file_paths[fi];
        const char *fname = get_basename(fpath);
        const char *mime = get_mime_type(fname);

        FILE *fp = fopen(fpath, "rb");
        if (!fp) {
            ESP_LOGW(TAG, "Dosya acilamadi: %s", fpath);
            continue;
        }

        /* Dosya boyutunu al */
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        ESP_LOGI(TAG, "Ek gonderiliyor: %s (%ld byte, %s)", fname, fsize, mime);

        /* MIME part header */
        snprintf(buf, SMTP_BUF_SIZE,
                 "\r\n--" MIME_BOUNDARY "\r\n"
                 "Content-Type: %s; name=\"%s\"\r\n"
                 "Content-Transfer-Encoding: base64\r\n"
                 "Content-Disposition: attachment; filename=\"%s\"\r\n"
                 "\r\n",
                 mime, fname, fname);
        if (ssl_write_data(&ssl, buf, strlen(buf)) < 0) {
            fclose(fp);
            err = ESP_FAIL; goto cleanup;
        }

        /* Dosyayi chunk'lar halinde oku, base64 encode et, gonder */
        /* 57 byte raw = 76 base64 char (1 satir). 570 byte = 10 satir */
        unsigned char raw_chunk[570];
        char b64_chunk[800]; // 570 * 4/3 + satir sonlari + bosluk
        size_t b64_len;
        size_t bytes_read;

        while ((bytes_read = fread(raw_chunk, 1, sizeof(raw_chunk), fp)) > 0) {
            ret = mbedtls_base64_encode((unsigned char *)b64_chunk, sizeof(b64_chunk) - 2,
                                         &b64_len, raw_chunk, bytes_read);
            if (ret != 0) {
                ESP_LOGE(TAG, "Base64 encode hatasi");
                break;
            }
            /* Satir sonlari ekle (her 76 karakterde) */
            /* mbedtls_base64_encode zaten \0-terminated uretir */
            b64_chunk[b64_len] = '\r';
            b64_chunk[b64_len + 1] = '\n';
            b64_len += 2;
            if (ssl_write_data(&ssl, b64_chunk, b64_len) < 0) {
                fclose(fp);
                err = ESP_FAIL; goto cleanup;
            }
        }

        fclose(fp);
    }

    /* Son boundary + mesaj sonu */
    snprintf(buf, SMTP_BUF_SIZE, "\r\n--" MIME_BOUNDARY "--\r\n.\r\n");
    if (ssl_command(&ssl, buf, buf, SMTP_BUF_SIZE) <= 0) {
        err = ESP_FAIL; goto cleanup;
    }
    if (strncmp(buf, "250", 3) != 0) {
        ESP_LOGW(TAG, "MIME mail gonderim sonucu: %s", buf);
    }

    ssl_command(&ssl, "QUIT\r\n", buf, SMTP_BUF_SIZE);
    ESP_LOGI(TAG, "=== MIME mail gonderildi: %s -> %s (%d ek) ===", s_config.user, to, file_count);

cleanup:
    if (buf) free(buf);
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    if (sockfd >= 0) close(sockfd);
    return err;
}

esp_err_t smtp_client_send_test(void)
{
    if (!s_configured) return ESP_ERR_INVALID_STATE;

    char body[384];
    snprintf(body, sizeof(body),
             "LebensSpur SMTP Test\n"
             "====================\n\n"
             "Bu mail LebensSpur cihazindan otomatik gonderilmistir.\n"
             "SMTP baglantisi basariyla kuruldu.\n\n"
             "Sunucu: %s\n"
             "Port: %d\n"
             "Gonderen: %s\n\n"
             "Bu maili aliyorsaniz mail sistemi dogru calisiyor.",
             s_config.server, s_config.port, s_config.user);

    return smtp_client_send(s_config.user,
                            "LebensSpur - SMTP Test Basarili",
                            body);
}
