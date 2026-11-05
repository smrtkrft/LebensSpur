/*
 * api_sys — sistem uçları: cihaz bilgisi, günlükler, yeniden başlatma, fabrika ayarı (1/2),
 * uzaktan sıfırlama anahtarı ve GET /api/reset?key= (sözleşme: docs/07-adim6-sozlesme.md).
 * Uzun işler (yeniden başlatma, /data tam silme) sys_worker'da; yanıt önce gider.
 */
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "cfg.h"
#include "core.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "evlog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jobs.h"
#include "net_wifi.h"
#include "storage.h"
#include "sys_worker.h"
#include "api_priv.h"

/* ---------- cihaz bilgisi ---------- */

static void add_vol(cJSON *o, const char *name, const char *mount)
{
    cJSON *v = cJSON_AddObjectToObject(o, name);
    storage_state_t st = storage_state(mount);
    cJSON_AddStringToObject(v, "state", storage_state_name(st));
    size_t total = 0, used = 0;
    if (st == STG_MOUNTED && storage_info(mount, &total, &used) == ESP_OK) {
        cJSON_AddNumberToObject(v, "total", (double)total);
        cJSON_AddNumberToObject(v, "used", (double)used);
    }
}

static esp_err_t h_device(http_req_t *r)
{
    cJSON *o = cJSON_CreateObject();
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    cJSON_AddStringToObject(o, "chip", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(o, "rev", ci.revision);              /* çok*100 + az: 2 → v0.2 */
    cJSON_AddNumberToObject(o, "cores", ci.cores);
    cJSON_AddNumberToObject(o, "cpu_mhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    uint32_t fsz = 0;
    esp_flash_get_size(NULL, &fsz);
    cJSON_AddNumberToObject(o, "flash_kb", fsz / 1024);
    const esp_partition_t *run = esp_ota_get_running_partition();
    cJSON_AddNumberToObject(o, "slot_kb", run ? run->size / 1024 : 0);
    esp_image_metadata_t md = { 0 };
    const esp_partition_pos_t pos = { .offset = run ? run->address : 0, .size = run ? run->size : 0 };
    uint32_t app = run && esp_image_get_metadata(&pos, &md) == ESP_OK ? md.image_len : 0;
    cJSON_AddNumberToObject(o, "app_kb", (app + 1023) / 1024);
    cJSON_AddNumberToObject(o, "heap_total", (double)heap_caps_get_total_size(MALLOC_CAP_DEFAULT));
    cJSON_AddNumberToObject(o, "heap_free", (double)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    cJSON_AddNumberToObject(o, "heap_min", (double)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
    cJSON_AddNumberToObject(o, "ext_mb", storage_chip_size() >> 20);
    add_vol(o, "log", STG_LOG);
    add_vol(o, "data", STG_DATA);
    wifi_status_t w;
    net_wifi_get_status(&w);
    cfg_wifi_tx_t tx;
    cfg_copy_section(CFG_SEC_WIFI_TX, &tx);
    cJSON_AddNumberToObject(o, "tx_q", w.tx_q);
    cJSON_AddNumberToObject(o, "tx_max_q", tx.max_q);
    evlog_boot_info_t b;
    evlog_boot_info(&b);
    cJSON_AddNumberToObject(o, "boot_count", b.boot_count);
    cJSON_AddStringToObject(o, "last_cause", b.cause ? b.cause : "unknown");
    cJSON_AddStringToObject(o, "last_cat", b.cat ? b.cat : "unknown");
    cJSON_AddNumberToObject(o, "prev_uptime", b.prev_uptime_s);
    return http_ok(r, o);
}

/* ---------- güncelleme bilgisi (indirme/kurulum adım 6'da) ---------- */

static esp_err_t h_ota(http_req_t *r)
{
    cJSON *o = cJSON_CreateObject();
    const esp_partition_t *run = esp_ota_get_running_partition();
    cJSON_AddStringToObject(o, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "partition", run ? run->label : "?");
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t d;
    if (other && esp_ota_get_partition_description(other, &d) == ESP_OK &&
        strcmp(d.project_name, esp_app_get_description()->project_name) == 0) {
        cJSON_AddStringToObject(o, "prev_version", d.version);
    }
    cJSON_AddStringToObject(o, "state", "idle");
    return http_ok(r, o);
}

/* ---------- günlükler ---------- */

#define LOG_MAX_LINES 150

static bool add_line(const char *line, void *ctx)
{
    if (line[0] != '{') return true;                       /* bozuk/yarım satır atlanır */
    cJSON_AddItemToArray((cJSON *)ctx, cJSON_CreateRaw(line));
    return true;
}

static esp_err_t h_logs(http_req_t *r)
{
    char buf[16];
    uint32_t cursor = 0;
    if (http_query(r, "cursor", buf, sizeof buf)) cursor = (uint32_t)strtoul(buf, NULL, 10);
    int n = LOG_MAX_LINES;
    if (http_query(r, "n", buf, sizeof buf)) {
        n = atoi(buf);
        if (n < 1 || n > LOG_MAX_LINES) n = LOG_MAX_LINES;
    }
    cJSON *o = cJSON_CreateObject();
    evlog_boot_info_t b;
    evlog_boot_info(&b);
    cJSON_AddNumberToObject(o, "boot_count", b.boot_count);
    cJSON_AddStringToObject(o, "last_cause", b.cause ? b.cause : "unknown");
    cJSON_AddStringToObject(o, "last_cat", b.cat ? b.cat : "unknown");
    cJSON_AddNumberToObject(o, "prev_uptime", b.prev_uptime_s);
    cJSON_AddNumberToObject(o, "short_boots", b.short_boots);
    cJSON_AddNumberToObject(o, "dropped", evlog_dropped());
    cJSON *arr = cJSON_AddArrayToObject(o, "logs");
    uint32_t next = 0;
    esp_err_t e = evlog_read(cursor, n, add_line, arr, &next);
    if (e != ESP_OK) {
        cJSON_Delete(o);
        return http_fail(r, 409, "storage_absent", "", NULL);
    }
    cJSON_AddNumberToObject(o, "next", next);
    return http_ok(r, o);
}

static esp_err_t h_logs_clear(http_req_t *r)
{
    if (evlog_clear() != ESP_OK) return http_fail(r, 409, "storage_absent", "", NULL);
    evlog_write(EVL_INFO, "user", "gunluk temizlendi");
    return http_ok(r, NULL);
}

/* ---------- yeniden başlatma / fabrika ayarı ---------- */

static job_id_t s_factory_job;

static void job_restart(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(400));                         /* HTTP yanıtı gitsin */
    sys_restart(RST_USER_WEB);
}

static void erase_progress(int pct)
{
    jobs_update(s_factory_job, JOB_RUNNING, pct > 99 ? 99 : pct, "data");
    sys_worker_feed();
}

static void job_factory(void *arg)
{
    uint8_t level = (uint8_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(400));
    if (level == 2) {
        evlog_write(EVL_WARN, "user", "fabrika sifirlama kademe 2: /data tamamen siliniyor");
        esp_err_t e = storage_erase_data_full(erase_progress);
        if (e != ESP_OK) evlog_write(EVL_ERR, "user", "/data silinemedi: %s", esp_err_to_name(e));
    }
    jobs_update(s_factory_job, JOB_DONE, 100, "restart");
    vTaskDelay(pdMS_TO_TICKS(1500));                        /* arayüz "bitti"yi görsün */
    core_post(&(core_event_t){ .type = EV_FACTORY_RESET, .u.factory.level = level });
}

static esp_err_t h_restart(http_req_t *r)
{
    evlog_write(EVL_INFO, "user", "web arayuzunden yeniden baslatma");
    if (sys_worker_post(job_restart, NULL) != ESP_OK) return http_fail(r, 503, "busy", "", NULL);
    return http_ok(r, NULL);
}

static esp_err_t h_factory(http_req_t *r)
{
    const cJSON *l = cJSON_GetObjectItem(http_body(r), "level");
    if (!cJSON_IsNumber(l) || (l->valueint != 1 && l->valueint != 2)) {
        return http_fail(r, 400, "validation", "1|2", "level");
    }
    if (l->valueint == 2 && storage_state(STG_DATA) == STG_ABSENT) return http_fail(r, 409, "storage_absent", "", NULL);
    s_factory_job = jobs_create("factory");
    if (sys_worker_post(job_factory, (void *)(uintptr_t)l->valueint) != ESP_OK) {
        jobs_update(s_factory_job, JOB_FAILED, 0, "busy");
        return http_fail(r, 503, "busy", "", NULL);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "job_id", s_factory_job);
    return http_ok(r, o);
}

/* ---------- uzaktan sıfırlama ---------- */

#define KEY_LEN 26                                          /* base32, 130 bit */

static esp_err_t h_key_get(http_req_t *r)
{
    cfg_remote_t rm;
    cfg_copy_section(CFG_SEC_REMOTE, &rm);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "key", rm.key);
    memset(&rm, 0, sizeof rm);
    return http_ok(r, o);
}

static esp_err_t h_key_new(http_req_t *r)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    uint8_t rnd[KEY_LEN];
    esp_fill_random(rnd, sizeof rnd);
    char key[KEY_LEN + 1];
    for (int i = 0; i < KEY_LEN; i++) key[i] = abc[rnd[i] & 31];
    key[KEY_LEN] = 0;
    cJSON *in = cJSON_CreateObject();
    cJSON_AddStringToObject(in, "key", key);
    esp_err_t e = cfg_set_section_json(CFG_SEC_REMOTE, in, NULL, 0, NULL);
    cJSON_Delete(in);
    if (e != ESP_OK) return http_fail(r, 500, "internal", "", NULL);
    evlog_write(EVL_INFO, "user", "uzaktan sifirlama anahtari yenilendi");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "key", key);
    return http_ok(r, o);
}

/* hız sınırı: 1 dk içinde 5 hatalı → 1 dk kilit (tüm istemciler; anahtar 130 bit, kaba kuvvet zaten imkânsız) */
static uint32_t s_fail_win, s_fail_n, s_lock_until;

static bool ct_eq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned d = (unsigned)(la ^ lb);
    for (size_t i = 0; i < KEY_LEN + 1; i++) d |= (unsigned)((i < la ? a[i] : 0) ^ (i < lb ? b[i] : 0));
    return d == 0;
}

static esp_err_t h_reset(http_req_t *r)
{
    uint32_t now = evlog_uptime_s();
    if (now < s_lock_until) return http_fail(r, 429, "locked", "", NULL);
    cfg_remote_t rm;
    cfg_copy_section(CFG_SEC_REMOTE, &rm);
    char key[KEY_LEN + 8] = "";
    http_query(r, "key", key, sizeof key);
    bool ok = rm.enabled && rm.key[0] && ct_eq(key, rm.key);
    memset(&rm, 0, sizeof rm);
    if (!ok) {
        if (now - s_fail_win > 60) { s_fail_win = now; s_fail_n = 0; }
        if (++s_fail_n >= 5) {
            s_lock_until = now + 60;
            evlog_write(EVL_WARN, "api", "uzaktan sifirlama: cok fazla hatali anahtar, 1 dk kilit (%s)", http_client_ip(r));
        }
        return http_fail(r, 403, "forbidden", "", "key");
    }
    app_status_t st;
    core_get_status(&st);
    if (st.total_s == 0) return http_fail(r, 409, "no_timer", "", NULL);
    core_event_t ev = { .type = EV_CHECKIN, .u.checkin.src = CHK_API_KEY };
    return core_post(&ev) == ESP_OK ? http_ok(r, NULL) : http_fail(r, 503, "busy", "", NULL);
}

const http_route_t api_sys_routes[] = {
    { "/api/device", HTTP_GET, HR_AUTH, h_device },
    { "/api/ota", HTTP_GET, HR_AUTH, h_ota },
    { "/api/device/restart", HTTP_POST, HR_AUTH | HR_CSRF, h_restart },
    { "/api/device/factory-reset", HTTP_POST, HR_WRITE, h_factory },
    { "/api/logs", HTTP_GET, HR_AUTH, h_logs },
    { "/api/logs", HTTP_DELETE, HR_AUTH | HR_CSRF, h_logs_clear },
    { "/api/remote/key", HTTP_GET, HR_AUTH, h_key_get },
    { "/api/remote/key", HTTP_POST, HR_AUTH | HR_CSRF, h_key_new },
    { "/api/reset", HTTP_GET, 0, h_reset },
};
const size_t api_sys_routes_n = sizeof api_sys_routes / sizeof api_sys_routes[0];
