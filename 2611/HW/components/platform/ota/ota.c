#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "evlog.h"
#include "jobs.h"
#include "net_wifi.h"
#include "sys_worker.h"
#include "ota.h"
#include "semver.h"

static const char *TAG = "ota";

#define REPO        "smrtkrft/LebensSpur-firmware"            /* K1: yalnız release deposu */
#define ASSET       "lebensspur.bin"
#define LATEST_URL  "https://github.com/" REPO "/releases/latest"
#define DL_FMT      "https://github.com/" REPO "/releases/download/%s/" ASSET
#define TIMEOUT_MS  10000
#define CONFIRM_US  (5LL * 1000000LL)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static ota_status_t s_st;
static atomic_bool s_busy, s_abort;
static char s_url[256];                  /* özel kaynak (boş = resmi) */
static uint16_t s_job;
static esp_timer_handle_t s_confirm_t;

/* ---------- durum ---------- */

const char *ota_state_name(ota_state_t s)
{
    static const char *const n[] = { "idle", "checking", "available", "no_update", "downloading", "ready", "error",
                                     "aborted", "rollback_ready" };
    return (unsigned)s < sizeof n / sizeof n[0] ? n[s] : "error";
}

static void set_state(ota_state_t st, int pct, const char *msg)
{
    portENTER_CRITICAL(&s_mux);
    s_st.state = st;
    if (pct >= 0) s_st.pct = pct;
    if (msg) strlcpy(s_st.message, msg, sizeof s_st.message);
    portEXIT_CRITICAL(&s_mux);
}

static void set_remote(const char *v)
{
    if (*v == 'v' || *v == 'V') v++;                    /* tag "v1.0.1.1" ile imaj "1.0.1.1" aynı biçimde */
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_st.remote, v, sizeof s_st.remote);
    portEXIT_CRITICAL(&s_mux);
}

void ota_get_status(ota_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

bool ota_restart_pending(void)
{
    ota_status_t s;
    ota_get_status(&s);
    return s.state == OTA_READY || s.state == OTA_ROLLBACK_READY;
}

static void finish_job(bool ok, const char *msg)
{
    if (s_job) jobs_update(s_job, ok ? JOB_DONE : JOB_FAILED, ok ? 100 : 0, msg);
    s_job = 0;
    atomic_store(&s_busy, false);
}

/* önceki bölüm: aynı ürünün geçerli imajı varsa geri alınabilir */
static void scan_rollback(void)
{
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t d;
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    bool ok = other && esp_ota_get_partition_description(other, &d) == ESP_OK &&
              strncmp(d.project_name, esp_app_get_description()->project_name, sizeof d.project_name) == 0 &&
              (esp_ota_get_state_partition(other, &st) != ESP_OK || st == ESP_OTA_IMG_VALID || st == ESP_OTA_IMG_UNDEFINED);
    portENTER_CRITICAL(&s_mux);
    s_st.rollback_available = ok;
    strlcpy(s_st.rollback, ok ? d.version : "", sizeof s_st.rollback);
    portEXIT_CRITICAL(&s_mux);
}

/* ---------- açılış onayı (koşullu) ---------- */

static void confirm_job(void *arg)
{
    (void)arg;
    wifi_status_t w;
    net_wifi_get_status(&w);
    uint32_t up = evlog_uptime_s();
    if (up < 30 || (w.state != WST_CONNECTED && up < 120)) return;     /* zamanlayıcı yeniden dener */
    esp_timer_stop(s_confirm_t);
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_st.pending_verify = false;
        portEXIT_CRITICAL(&s_mux);
        evlog_write(EVL_INFO, "ota", "yeni surum onaylandi: v%s", esp_app_get_description()->version);
        scan_rollback();
    }
}

static void confirm_tick(void *arg)
{
    (void)arg;
    sys_worker_post(confirm_job, NULL);                 /* flash yazımı timer görevinde değil */
}

esp_err_t ota_init(void)
{
    strlcpy(s_st.current, esp_app_get_description()->version, sizeof s_st.current);
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_st.pending_verify = true;
        const esp_timer_create_args_t a = { .callback = confirm_tick, .name = "ota_conf" };
        if (esp_timer_create(&a, &s_confirm_t) == ESP_OK) esp_timer_start_periodic(s_confirm_t, CONFIRM_US);
        evlog_write(EVL_INFO, "ota", "yeni surum ilk acilis: v%s, onay bekleniyor", s_st.current);
    }
    const esp_partition_t *inv = esp_ota_get_last_invalid_partition();
    esp_app_desc_t d;
    if (inv && esp_ota_get_partition_description(inv, &d) == ESP_OK) strlcpy(s_st.last_invalid, d.version, sizeof s_st.last_invalid);
    scan_rollback();
    return ESP_OK;
}

/* ---------- denetim ---------- */

static char s_loc[256];

static esp_err_t head_ev(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(e->header_key, "Location") == 0) {
        strlcpy(s_loc, e->header_value, sizeof s_loc);
    }
    return ESP_OK;
}

/* resmi: HEAD /releases/latest → Location .../releases/tag/<tag>; gövde inmez, API kotası yok */
static esp_err_t latest_tag(char *tag, size_t cap, char *err, size_t errlen)
{
    s_loc[0] = 0;
    esp_http_client_config_t c = { .url = LATEST_URL, .method = HTTP_METHOD_HEAD, .disable_auto_redirect = true,
                                   .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = TIMEOUT_MS,
                                   .buffer_size_tx = 1024, .event_handler = head_ev };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return (snprintf(err, errlen, "bellek yok"), ESP_ERR_NO_MEM);
    esp_err_t e = esp_http_client_perform(h);
    int code = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (e != ESP_OK) return (snprintf(err, errlen, "baglanti: %s", esp_err_to_name(e)), e);
    if (code == 404) return (snprintf(err, errlen, "yayin deposu bulunamadi (404)"), ESP_ERR_NOT_FOUND);
    const char *p = strstr(s_loc, "/releases/tag/");
    if (code / 100 != 3 || !p) return (snprintf(err, errlen, "beklenmeyen yanit %d", code), ESP_FAIL);
    p += strlen("/releases/tag/");
    size_t n = strcspn(p, "?#/");
    if (!n || n >= cap) return (snprintf(err, errlen, "tag okunamadi"), ESP_FAIL);
    memcpy(tag, p, n);
    tag[n] = 0;
    return ESP_OK;
}

static esp_http_client_config_t dl_cfg(const char *url)
{
    /* buffer_size_tx 2048: GitHub'ın imzalı indirme adresi ~900 B, varsayılan 512 sığmaz (araştırma §2) */
    return (esp_http_client_config_t){ .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = TIMEOUT_MS,
                                       .buffer_size = 2048, .buffer_size_tx = 2048, .keep_alive_enable = true };
}

/* indirme başlatır ve imaj tanımını okur (flash'a yazmaz); kapıları uygular */
static esp_err_t open_image(const char *url, esp_https_ota_handle_t *h, esp_app_desc_t *d, char *err, size_t errlen)
{
    esp_http_client_config_t hc = dl_cfg(url);
    esp_https_ota_config_t oc = { .http_config = &hc };
    esp_err_t e = esp_https_ota_begin(&oc, h);
    if (e != ESP_OK) return (snprintf(err, errlen, "indirme acilamadi: %s", esp_err_to_name(e)), e);
    e = esp_https_ota_get_img_desc(*h, d);
    if (e != ESP_OK) {
        esp_https_ota_abort(*h);
        return (snprintf(err, errlen, "imaj basligi okunamadi: %s", esp_err_to_name(e)), e);
    }
    const esp_app_desc_t *me = esp_app_get_description();
    if (strncmp(d->project_name, me->project_name, sizeof d->project_name) != 0) {
        esp_https_ota_abort(*h);
        snprintf(err, errlen, "baska urunun imaji: %.32s", d->project_name);
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

static void check_job(void *arg)
{
    (void)arg;
    char err[96] = "", ver[32] = "";
    esp_err_t e;
    if (s_url[0]) {
        esp_https_ota_handle_t h;
        esp_app_desc_t d;
        e = open_image(s_url, &h, &d, err, sizeof err);
        if (e == ESP_OK) {
            esp_https_ota_abort(h);
            strlcpy(ver, d.version, sizeof ver);
        }
    } else {
        e = latest_tag(ver, sizeof ver, err, sizeof err);
    }
    if (e != ESP_OK) {
        set_state(OTA_ERROR, 0, err);
        evlog_write(EVL_WARN, "ota", "denetim basarisiz: %s", err);
        finish_job(false, err);
        return;
    }
    set_remote(ver);
    bool newer = semver_cmp(ver, s_st.current) > 0;
    char msg[64];
    snprintf(msg, sizeof msg, "%s: %s", newer ? "yeni surum" : "guncel", ver);
    set_state(newer ? OTA_AVAILABLE : OTA_NO_UPDATE, 0, msg);
    evlog_write(EVL_INFO, "ota", "denetim: uzak %s, calisan %s", ver, s_st.current);
    finish_job(true, msg);
}

/* ---------- indirme ---------- */

static void start_job(void *arg)
{
    (void)arg;
    char err[96] = "", url[256];
    if (s_st.pending_verify) {
        set_state(OTA_ERROR, 0, "calisan surum henuz onaylanmadi");
        finish_job(false, "pending_verify");
        return;
    }
    if (s_url[0]) {
        strlcpy(url, s_url, sizeof url);
    } else {
        char tag[32];
        if (latest_tag(tag, sizeof tag, err, sizeof err) != ESP_OK) {
            set_state(OTA_ERROR, 0, err);
            finish_job(false, err);
            return;
        }
        set_remote(tag);
        snprintf(url, sizeof url, DL_FMT, tag);
    }
    set_state(OTA_DOWNLOADING, 0, "baglaniliyor");
    evlog_mark(EVM_OTA, 0);
    esp_https_ota_handle_t h;
    esp_app_desc_t d;
    esp_err_t e = open_image(url, &h, &d, err, sizeof err);
    if (e == ESP_OK && semver_cmp(d.version, s_st.current) <= 0) {
        esp_https_ota_abort(h);
        snprintf(err, sizeof err, "imaj surumu %.31s calisandan yeni degil", d.version);
        e = ESP_ERR_INVALID_VERSION;
    }
    if (e != ESP_OK) {
        set_state(OTA_ERROR, 0, err);
        evlog_write(EVL_WARN, "ota", "indirme reddedildi: %s", err);
        finish_job(false, err);
        return;
    }
    set_remote(d.version);
    evlog_write(EVL_INFO, "ota", "indirme basladi: v%s", d.version);
    int size = esp_https_ota_get_image_size(h), last = -1;
    while ((e = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (atomic_load(&s_abort)) break;
        int got = esp_https_ota_get_image_len_read(h);
        int pct = size > 0 ? (int)((int64_t)got * 99 / size) : 0;
        if (pct != last) {
            last = pct;
            set_state(OTA_DOWNLOADING, pct, NULL);
            if (s_job) jobs_update(s_job, JOB_RUNNING, pct, "indiriliyor");
        }
        sys_worker_feed();
    }
    if (atomic_load(&s_abort)) {
        esp_https_ota_abort(h);
        set_state(OTA_ABORTED, 0, "iptal edildi (tetikleme)");
        evlog_write(EVL_WARN, "ota", "indirme iptal edildi");
        scan_rollback();                                /* yarım yazım öbür bölümü bozdu: geri alma yok */
        finish_job(false, "aborted");
        return;
    }
    if (e == ESP_OK && !esp_https_ota_is_complete_data_received(h)) e = ESP_ERR_INVALID_SIZE;
    if (e == ESP_OK) {
        e = esp_https_ota_finish(h);
    } else {
        esp_https_ota_abort(h);
    }
    if (e != ESP_OK) {
        snprintf(err, sizeof err, "indirme/dogrulama: %s", esp_err_to_name(e));
        set_state(OTA_ERROR, 0, err);
        evlog_write(EVL_ERR, "ota", "%s", err);
        scan_rollback();
        finish_job(false, err);
        return;
    }
    char msg[64];
    snprintf(msg, sizeof msg, "v%.31s hazir, yeniden baslatin", d.version);
    set_state(OTA_READY, 100, msg);
    evlog_write(EVL_INFO, "ota", "indirme tamam: v%s, yeniden baslatma bekleniyor", d.version);
    finish_job(true, msg);
}

/* ---------- dış arayüz ---------- */

static esp_err_t submit(const char *url, uint16_t job, sys_job_fn_t fn, ota_state_t st)
{
    if (atomic_exchange(&s_busy, true)) return ESP_ERR_INVALID_STATE;
    strlcpy(s_url, url ? url : "", sizeof s_url);
    s_job = job;
    atomic_store(&s_abort, false);
    set_state(st, 0, "");
    if (sys_worker_post(fn, NULL) != ESP_OK) {
        atomic_store(&s_busy, false);
        set_state(OTA_ERROR, 0, "is kuyrugu dolu");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ota_check(const char *url, uint16_t job) { return submit(url, job, check_job, OTA_CHECKING); }
esp_err_t ota_start(const char *url, uint16_t job) { return submit(url, job, start_job, OTA_DOWNLOADING); }

void ota_abort(void)
{
    if (atomic_load(&s_busy)) atomic_store(&s_abort, true);
}

esp_err_t ota_rollback(void)
{
    if (atomic_load(&s_busy) || ota_restart_pending()) return ESP_ERR_INVALID_STATE;   /* hazır imaj varken değil */
    scan_rollback();
    if (!s_st.rollback_available) return ESP_ERR_NOT_FOUND;
    esp_err_t e = esp_ota_set_boot_partition(esp_ota_get_next_update_partition(NULL));
    if (e == ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof msg, "v%.31s'a donulecek, yeniden baslatin", s_st.rollback);
        set_state(OTA_ROLLBACK_READY, 0, msg);
        evlog_write(EVL_WARN, "ota", "geri alma: sonraki acilis v%s", s_st.rollback);
    }
    ESP_LOGI(TAG, "rollback: %s", esp_err_to_name(e));
    return e;
}
