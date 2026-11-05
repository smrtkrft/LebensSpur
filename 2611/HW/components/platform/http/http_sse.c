/*
 * http_sse — GET /api/events. httpd_req_async_handler_begin ile soket httpd döngüsünden ayrılır
 * (LRU temizliği kapatmaz); push httpd_queue_work ile httpd görevinde yapılır → yuvalar kilitsiz.
 * Kopma: gönderim hatası ya da recv(MSG_PEEK) == 0. 60 sn'de bir kalp atışı tam durumu taşır.
 */
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include "esp_timer.h"
#include "http_priv.h"

#define SLOTS 4
#define HEARTBEAT_US (60LL * 1000000LL)

static httpd_handle_t s_h;
static httpd_req_t *s_slot[SLOTS];          /* yalnız httpd görevinde erişilir */
static http_sse_source_t s_source;
static atomic_bool s_pending;
static esp_timer_handle_t s_hb;
static char s_buf[1536];

void http_sse_set_source(http_sse_source_t fn) { s_source = fn; }

static void drop(int i)
{
    httpd_req_async_handler_complete(s_slot[i]);
    s_slot[i] = NULL;
}

static bool alive(httpd_req_t *r)
{
    char c;
    int n = recv(httpd_req_to_sockfd(r), &c, 1, MSG_PEEK | MSG_DONTWAIT);
    return n != 0;                                  /* 0 = karşı taraf kapattı; -1/EAGAIN = canlı */
}

static void send_all(void *arg)
{
    (void)arg;
    atomic_store(&s_pending, false);
    if (!s_source) return;
    cJSON *o = s_source();
    if (!o) return;
    memcpy(s_buf, "data: ", 6);
    bool ok = cJSON_PrintPreallocated(o, s_buf + 6, (int)sizeof s_buf - 9, false);
    cJSON_Delete(o);
    if (!ok) return;
    strcat(s_buf, "\n\n");
    for (int i = 0; i < SLOTS; i++) {
        if (!s_slot[i]) continue;
        if (!alive(s_slot[i]) || httpd_resp_send_chunk(s_slot[i], s_buf, (ssize_t)strlen(s_buf)) != ESP_OK) drop(i);
    }
}

void http_sse_broadcast(void)
{
    if (s_h && !atomic_exchange(&s_pending, true)) {
        if (httpd_queue_work(s_h, send_all, NULL) != ESP_OK) atomic_store(&s_pending, false);
    }
}

static void hb_cb(void *arg) { (void)arg; http_sse_broadcast(); }

void http_sse_init(httpd_handle_t h)
{
    s_h = h;
    const esp_timer_create_args_t a = { .callback = hb_cb, .name = "sse_hb" };
    if (esp_timer_create(&a, &s_hb) == ESP_OK) esp_timer_start_periodic(s_hb, HEARTBEAT_US);
}

esp_err_t http_sse_handler(http_req_t *r)
{
    int free_i = -1;
    for (int i = 0; i < SLOTS; i++) {
        if (s_slot[i] && !alive(s_slot[i])) drop(i);
        if (!s_slot[i] && free_i < 0) free_i = i;
    }
    if (free_i < 0) return http_fail(r, 503, "busy", "en cok 4 canli baglanti", NULL);
    httpd_req_t *async = NULL;
    if (httpd_req_async_handler_begin(r->req, &async) != ESP_OK) return http_fail(r, 500, "internal", "", NULL);
    httpd_resp_set_type(async, "text/event-stream");
    httpd_resp_set_hdr(async, "Cache-Control", "no-store");
    httpd_resp_set_hdr(async, "Connection", "keep-alive");
    if (httpd_resp_send_chunk(async, ": ok\n\n", 6) != ESP_OK) {
        httpd_req_async_handler_complete(async);
        return ESP_OK;
    }
    s_slot[free_i] = async;
    http_sse_broadcast();                           /* ilk durum hemen */
    return ESP_OK;
}
