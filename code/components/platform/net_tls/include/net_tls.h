/*
 * net_tls — tüm TLS/TCP istemci bağlantılarının TEK geçidi (03-arastirma/02).
 * - CA paketi her zaman bağlıdır; doğrulama yalnız opts.verify=false ile (yalnız SMTP kullanıcı seçimi)
 *   kapatılabilir. Kimse esp_tls'i doğrudan yapılandırmaz (CONFIG_ESP_TLS_INSECURE açık olduğu için).
 * - Her işlem son tarihlidir (deadline_us, esp_timer zamanı); WANT_READ döngüleri sonsuz dönmez.
 * - STARTTLS: düz TCP üzerinde konuşup aynı soketi TLS'e yükseltir.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool verify;                /* false: sunucu sertifikası doğrulanmaz (uyarılı seçenek) */
    const char *cacert_pem;     /* NULL: ortak CA paketi */
    uint32_t connect_ms;        /* TCP bağlantı (DNS ayrı) */
    uint32_t handshake_ms;
} net_tls_opts_t;

typedef struct net_conn net_conn_t;

#define NET_TLS_OPTS_DEFAULT { .verify = true, .cacert_pem = NULL, .connect_ms = 10000, .handshake_ms = 15000 }

net_conn_t *net_connect_tls(const char *host, uint16_t port, const net_tls_opts_t *o, char *err, size_t errlen);
net_conn_t *net_connect_plain(const char *host, uint16_t port, const net_tls_opts_t *o, char *err, size_t errlen);
esp_err_t   net_starttls(net_conn_t *c, const char *host, const net_tls_opts_t *o, char *err, size_t errlen);
/* >0 bayt, 0 bağlantı kapandı, <0 hata ya da son tarih */
int  net_read(net_conn_t *c, void *buf, size_t len, int64_t deadline_us);
int  net_write_all(net_conn_t *c, const void *buf, size_t len, int64_t deadline_us);
/* CRLF'e kadar bir satır (CRLF atılır, NUL sonlu). Dönüş: uzunluk ya da <0 */
int  net_readline(net_conn_t *c, char *line, size_t cap, int64_t deadline_us);
bool net_is_tls(const net_conn_t *c);
void net_close(net_conn_t *c);
