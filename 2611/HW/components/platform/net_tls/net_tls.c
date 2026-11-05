#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "net_tls.h"

struct net_conn {
    esp_tls_t *tls;     /* düz bağlantıda da sahibi bu (esp_tls_plain_tcp_connect soketi) */
    int fd;
    bool secure;
    char rbuf[256];     /* readline tamponu */
    size_t rlen;
};

static void fill_cfg(esp_tls_cfg_t *cfg, const net_tls_opts_t *o, int timeout_ms)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->timeout_ms = timeout_ms;
    if (o->verify || o->cacert_pem) {
        if (o->cacert_pem) {
            cfg->cacert_pem_buf = (const unsigned char *)o->cacert_pem;
            cfg->cacert_pem_bytes = strlen(o->cacert_pem) + 1;
        } else {
            cfg->crt_bundle_attach = esp_crt_bundle_attach;
        }
    }
    /* verify=false ve PEM yok: CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY ile doğrulamasız (bilinçli seçim) */
}

static void set_err(char *err, size_t n, const char *what, esp_tls_t *t)
{
    if (!err) return;
    int code = 0, flags = 0;
    esp_tls_error_handle_t h = NULL;
    if (t && esp_tls_get_error_handle(t, &h) == ESP_OK && h) {
        esp_tls_get_and_clear_last_error(h, &code, &flags);
    }
    snprintf(err, n, "%s (tls %d, flags 0x%x)", what, code, flags);
}

net_conn_t *net_connect_tls(const char *host, uint16_t port, const net_tls_opts_t *o, char *err, size_t errlen)
{
    net_conn_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->tls = esp_tls_init();
    esp_tls_cfg_t cfg;
    fill_cfg(&cfg, o, (int)(o->connect_ms + o->handshake_ms));
    if (!c->tls || esp_tls_conn_new_sync(host, (int)strlen(host), port, &cfg, c->tls) != 1) {
        set_err(err, errlen, "tls baglantisi kurulamadi", c->tls);
        net_close(c);
        return NULL;
    }
    esp_tls_get_conn_sockfd(c->tls, &c->fd);
    c->secure = true;
    return c;
}

net_conn_t *net_connect_plain(const char *host, uint16_t port, const net_tls_opts_t *o, char *err, size_t errlen)
{
    net_conn_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->tls = esp_tls_init();
    esp_tls_cfg_t cfg;
    fill_cfg(&cfg, o, (int)o->connect_ms);
    esp_tls_error_handle_t eh = NULL;
    if (c->tls) esp_tls_get_error_handle(c->tls, &eh);
    if (!c->tls || esp_tls_plain_tcp_connect(host, (int)strlen(host), port, &cfg, eh, &c->fd) != ESP_OK) {
        set_err(err, errlen, "tcp baglantisi kurulamadi", c->tls);
        net_close(c);
        return NULL;
    }
    esp_tls_set_conn_sockfd(c->tls, c->fd);
    return c;
}

esp_err_t net_starttls(net_conn_t *c, const char *host, const net_tls_opts_t *o, char *err, size_t errlen)
{
    esp_tls_cfg_t cfg;
    fill_cfg(&cfg, o, (int)o->handshake_ms);
    c->rlen = 0;                               /* düz metin artığı TLS'e karışmasın */
    esp_tls_set_conn_state(c->tls, ESP_TLS_CONNECTING);
    if (esp_tls_conn_new_sync(host, (int)strlen(host), 0, &cfg, c->tls) != 1) {
        set_err(err, errlen, "starttls el sikismasi basarisiz", c->tls);
        return ESP_FAIL;
    }
    c->secure = true;
    return ESP_OK;
}

static int remaining_ms(int64_t deadline)
{
    int64_t d = (deadline - esp_timer_get_time()) / 1000;
    return d <= 0 ? 0 : d > 30000 ? 30000 : (int)d;
}

static void set_timeouts(int fd, int ms)
{
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

int net_read(net_conn_t *c, void *buf, size_t len, int64_t deadline)
{
    if (c->rlen) {                                     /* readline artığı önce */
        size_t n = c->rlen < len ? c->rlen : len;
        memcpy(buf, c->rbuf, n);
        memmove(c->rbuf, c->rbuf + n, c->rlen - n);
        c->rlen -= n;
        return (int)n;
    }
    for (;;) {
        int ms = remaining_ms(deadline);
        if (ms == 0) return -ETIMEDOUT;
        set_timeouts(c->fd, ms);
        int r = c->secure ? (int)esp_tls_conn_read(c->tls, buf, len) : (int)recv(c->fd, buf, len, 0);
        if (r >= 0) return r;
        if (c->secure && (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE)) continue;
        if (!c->secure && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return r;
    }
}

int net_write_all(net_conn_t *c, const void *buf, size_t len, int64_t deadline)
{
    const char *p = buf;
    size_t left = len;
    while (left) {
        int ms = remaining_ms(deadline);
        if (ms == 0) return -ETIMEDOUT;
        set_timeouts(c->fd, ms);
        int r = c->secure ? (int)esp_tls_conn_write(c->tls, p, left) : (int)send(c->fd, p, left, 0);
        if (r > 0) { p += r; left -= (size_t)r; continue; }
        if (c->secure && (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE)) continue;
        if (!c->secure && r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return r < 0 ? r : -EPIPE;
    }
    return (int)len;
}

int net_readline(net_conn_t *c, char *line, size_t cap, int64_t deadline)
{
    size_t n = 0;
    for (;;) {
        for (size_t i = 0; i < c->rlen; i++) {
            if (c->rbuf[i] == '\n') {
                size_t take = i + 1;
                size_t copy = i > 0 && c->rbuf[i - 1] == '\r' ? i - 1 : i;
                if (n + copy >= cap) copy = cap - 1 - n;
                memcpy(line + n, c->rbuf, copy);
                n += copy;
                line[n] = 0;
                memmove(c->rbuf, c->rbuf + take, c->rlen - take);
                c->rlen -= take;
                return (int)n;
            }
        }
        if (c->rlen == sizeof c->rbuf) {                 /* satır tampondan uzun: kırp, devam */
            size_t copy = c->rlen;
            if (n + copy >= cap) copy = cap - 1 - n;
            memcpy(line + n, c->rbuf, copy);
            n += copy;
            c->rlen = 0;
        }
        int ms = remaining_ms(deadline);
        if (ms == 0) return -ETIMEDOUT;
        set_timeouts(c->fd, ms);
        int r = c->secure ? (int)esp_tls_conn_read(c->tls, c->rbuf + c->rlen, sizeof c->rbuf - c->rlen)
                          : (int)recv(c->fd, c->rbuf + c->rlen, sizeof c->rbuf - c->rlen, 0);
        if (r > 0) { c->rlen += (size_t)r; continue; }
        if (r == 0) return -EPIPE;
        if (c->secure && (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE)) continue;
        if (!c->secure && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return r;
    }
}

bool net_is_tls(const net_conn_t *c) { return c->secure; }

void net_close(net_conn_t *c)
{
    if (!c) return;
    if (c->tls) esp_tls_conn_destroy(c->tls);      /* soketi de kapatır */
    free(c);
}
