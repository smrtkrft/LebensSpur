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
#include "esp_core_dump.h"
#include "esp_partition.h"
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
#include "ota.h"
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
    size_t cd_addr = 0, cd_size = 0;
    bool cd = esp_core_dump_image_check() == ESP_OK && esp_core_dump_image_get(&cd_addr, &cd_size) == ESP_OK && cd_size;
    cJSON_AddNumberToObject(o, "coredump", cd ? (double)cd_size : 0);
    return http_ok(r, o);
}

/* ---------- güncelleme (OTA) ---------- */

static esp_err_t h_ota(http_req_t *r)
{
    ota_status_t st;
    ota_get_status(&st);
    const esp_partition_t *run = esp_ota_get_running_partition();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "version", st.current);
    cJSON_AddStringToObject(o, "partition", run ? run->label : "?");
    cJSON_AddStringToObject(o, "state", ota_state_name(st.state));
    cJSON_AddNumberToObject(o, "pct", st.pct);
    cJSON_AddStringToObject(o, "message", st.message);
    cJSON_AddStringToObject(o, "remote", st.remote);
    cJSON_AddBoolToObject(o, "available", st.state == OTA_AVAILABLE);
    if (st.rollback_available) cJSON_AddStringToObject(o, "prev_version", st.rollback);
    cJSON_AddBoolToObject(o, "pending_verify", st.pending_verify);
    if (st.last_invalid[0]) cJSON_AddStringToObject(o, "last_invalid", st.last_invalid);
    return http_ok(r, o);
}

/* gövde {source:"official"} | {source:"custom", url:"https://…"}; özel kaynak yalnız https */
static esp_err_t ota_req(http_req_t *r, bool start)
{
    const cJSON *src = cJSON_GetObjectItem(http_body(r), "source");
    const cJSON *url = cJSON_GetObjectItem(http_body(r), "url");
    const char *u = NULL;
    if (cJSON_IsString(src) && strcmp(src->valuestring, "custom") == 0) {
        if (!cJSON_IsString(url) || strncmp(url->valuestring, "https://", 8) != 0 || strlen(url->valuestring) > 250 ||
            strpbrk(url->valuestring, " \t\r\n")) {
            return http_fail(r, 400, "validation", "https:// ile baslayan adres", "url");
        }
        u = url->valuestring;
    } else if (!cJSON_IsString(src) || strcmp(src->valuestring, "official") != 0) {
        return http_fail(r, 400, "validation", "official|custom", "source");
    }
    job_id_t id = jobs_create(start ? "ota_start" : "ota_check");
    esp_err_t e = start ? ota_start(u, id) : ota_check(u, id);
    if (e != ESP_OK) {
        jobs_update(id, JOB_FAILED, 0, "busy");
        return http_fail(r, 409, "busy", "", NULL);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "job_id", id);
    return http_ok(r, o);
}

static esp_err_t h_ota_check(http_req_t *r) { return ota_req(r, false); }
static esp_err_t h_ota_start(http_req_t *r) { return ota_req(r, true); }

static esp_err_t h_ota_rollback(http_req_t *r)
{
    esp_err_t e = ota_rollback();
    if (e == ESP_ERR_NOT_FOUND) return http_fail(r, 409, "no_rollback", "", NULL);
    if (e != ESP_OK) return http_fail(r, 409, "busy", esp_err_to_name(e), NULL);
    return http_ok(r, NULL);
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
    sys_restart(ota_restart_pending() ? RST_OTA : RST_USER_WEB);   /* güncelleme/geri alma uygulanıyor */
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

/* ---------- çökme kaydı (coredump) ---------- */

static esp_err_t h_coredump(http_req_t *r)
{
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&addr, &size) != ESP_OK || !size) {
        return http_fail(r, 404, "not_found", "", NULL);
    }
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!p) return http_fail(r, 404, "not_found", "", NULL);
    httpd_req_t *raw = http_raw(r);
    httpd_resp_set_type(raw, "application/octet-stream");
    httpd_resp_set_hdr(raw, "Content-Disposition", "attachment; filename=\"lebensspur-coredump.elf\"");
    httpd_resp_set_hdr(raw, "Cache-Control", "no-store");
    httpd_resp_set_hdr(raw, "X-Content-Type-Options", "nosniff");
    static char buf[1024];
    size_t off = addr - p->address;                       /* görüntü bölüm başından itibaren */
    for (size_t done = 0; done < size;) {
        size_t n = size - done < sizeof buf ? size - done : sizeof buf;
        if (esp_partition_read(p, off + done, buf, n) != ESP_OK || httpd_resp_send_chunk(raw, buf, (ssize_t)n) != ESP_OK) {
            httpd_resp_send_chunk(raw, NULL, 0);
            return ESP_OK;
        }
        done += n;
    }
    httpd_resp_send_chunk(raw, NULL, 0);
    return ESP_OK;
}

static esp_err_t h_coredump_del(http_req_t *r)
{
    esp_err_t e = esp_core_dump_image_erase();
    if (e != ESP_OK) return http_fail(r, 500, "internal", esp_err_to_name(e), NULL);
    evlog_write(EVL_INFO, "user", "cokme kaydi silindi");
    return http_ok(r, NULL);
}

/* ---------- depolama ---------- */

static esp_err_t h_storage(http_req_t *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "chip_mb", storage_chip_size() >> 20);
    add_vol(o, "log", STG_LOG);
    add_vol(o, "data", STG_DATA);
    const char *n = storage_note();
    cJSON_AddStringToObject(o, "note", n ? n : "");
    return http_ok(r, o);
}

static job_id_t s_fmt_job;
static const char *s_fmt_mount;

static void job_format(void *arg)
{
    (void)arg;
    jobs_update(s_fmt_job, JOB_RUNNING, 10, s_fmt_mount);
    esp_err_t e = storage_format(s_fmt_mount);
    evlog_write(e == ESP_OK ? EVL_WARN : EVL_ERR, "user", "%s bicimlendi: %s", s_fmt_mount, esp_err_to_name(e));
    jobs_update(s_fmt_job, e == ESP_OK ? JOB_DONE : JOB_FAILED, 100, esp_err_to_name(e));
}

static esp_err_t h_storage_format(http_req_t *r)
{
    const cJSON *m = cJSON_GetObjectItem(http_body(r), "mount");
    const char *mount = !cJSON_IsString(m) ? NULL : strcmp(m->valuestring, STG_LOG) == 0 ? STG_LOG
                      : strcmp(m->valuestring, STG_DATA) == 0 ? STG_DATA : NULL;
    if (!mount) return http_fail(r, 400, "validation", "/log|/data", "mount");
    if (storage_state(mount) == STG_ABSENT) return http_fail(r, 409, "storage_absent", "", NULL);
    s_fmt_mount = mount;
    s_fmt_job = jobs_create("format");
    if (sys_worker_post(job_format, NULL) != ESP_OK) {
        jobs_update(s_fmt_job, JOB_FAILED, 0, "busy");
        return http_fail(r, 503, "busy", "", NULL);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "job_id", s_fmt_job);
    return http_ok(r, o);
}

/* ---------- yapılandırma yedeği / dışa aktarma ---------- */

static esp_err_t h_backup_info(http_req_t *r)
{
    uint32_t seq = 0;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "available", cfg_backup_info(&seq) == ESP_OK);
    cJSON_AddNumberToObject(o, "seq", seq);
    cJSON_AddBoolToObject(o, "offer", cfg_backup_offer());   /* fabrika ayarı sonrası: geri yükleme önerilsin */
    return http_ok(r, o);
}

/* "yeni başla": eski yedek bırakılır, güncel ayarlar yedek olur */
static esp_err_t h_backup_dismiss(http_req_t *r)
{
    esp_err_t e = cfg_backup_dismiss();
    if (e != ESP_OK) return http_fail(r, 500, "internal", esp_err_to_name(e), NULL);
    evlog_write(EVL_INFO, "user", "yedek onerisi reddedildi: guncel ayarlar yedeklenecek");
    return http_ok(r, NULL);
}

static job_id_t s_bk_job;

static void job_backup(void *arg)
{
    bool restore = arg != NULL;
    esp_err_t e = restore ? cfg_backup_restore() : cfg_backup_write();
    evlog_write(e == ESP_OK ? EVL_INFO : EVL_ERR, "user", "yapilandirma %s: %s", restore ? "geri yuklendi" : "yedeklendi",
                esp_err_to_name(e));
    jobs_update(s_bk_job, e == ESP_OK ? JOB_DONE : JOB_FAILED, 100, restore && e == ESP_OK ? "restart" : esp_err_to_name(e));
    if (restore && e == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        sys_restart(RST_GENERIC);                          /* geri yüklenen ayarlar her modülde temiz başlasın */
    }
}

static esp_err_t backup_req(http_req_t *r, bool restore)
{
    if (storage_state(STG_DATA) != STG_MOUNTED) return http_fail(r, 409, "storage_absent", "", NULL);
    if (restore && cfg_backup_info(NULL) != ESP_OK) return http_fail(r, 404, "not_found", "yedek yok", NULL);
    s_bk_job = jobs_create(restore ? "restore" : "backup");
    if (sys_worker_post(job_backup, restore ? (void *)1 : NULL) != ESP_OK) {
        jobs_update(s_bk_job, JOB_FAILED, 0, "busy");
        return http_fail(r, 503, "busy", "", NULL);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "job_id", s_bk_job);
    return http_ok(r, o);
}

static esp_err_t h_backup_write(http_req_t *r) { return backup_req(r, false); }
static esp_err_t h_backup_restore(http_req_t *r) { return backup_req(r, true); }

/* tüm bölümler + gruplar; gizli alanlar maskeli (indirilen dosyada parola yok) */
static esp_err_t h_export(http_req_t *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "device", net_wifi_device_name());
    cJSON_AddStringToObject(o, "version", esp_app_get_description()->version);
    cJSON *sec = cJSON_AddObjectToObject(o, "sections");
    for (int i = 0; i < CFG_SEC__COUNT; i++) {
        cJSON_AddItemToObject(sec, cfg_sections[i].name, cfg_get_section_json((cfg_section_t)i, CFG_EXPORT_API));
    }
    cJSON *gr = cJSON_AddArrayToObject(o, "groups");
    static cfg_group_t g;
    for (int i = 0; i < CFG_GROUP_COUNT; i++) {
        if (cfg_group_get(i, &g) != ESP_OK) continue;
        cJSON *j = cfg_fields_to_json(&cfg_group_desc, &g, CFG_EXPORT_API);
        cJSON_AddNumberToObject(j, "id", i);
        cJSON_AddItemToArray(gr, j);
    }
    http_set_header(r, "Content-Disposition", "attachment; filename=\"lebensspur-config.json\"");
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
    esp_err_t e = core_post_sync(&ev, 1000);
    return e == ESP_OK || e == ESP_ERR_TIMEOUT ? http_ok(r, NULL) : http_fail(r, 503, "busy", "", NULL);
}

const http_route_t api_sys_routes[] = {
    { "/api/device", HTTP_GET, HR_AUTH, h_device },
    { "/api/ota", HTTP_GET, HR_AUTH, h_ota },
    { "/api/ota/check", HTTP_POST, HR_WRITE, h_ota_check },
    { "/api/ota/start", HTTP_POST, HR_WRITE, h_ota_start },
    { "/api/ota/rollback", HTTP_POST, HR_AUTH | HR_CSRF, h_ota_rollback },
    { "/api/device/restart", HTTP_POST, HR_AUTH | HR_CSRF, h_restart },
    { "/api/device/factory-reset", HTTP_POST, HR_WRITE, h_factory },
    { "/api/logs", HTTP_GET, HR_AUTH, h_logs },
    { "/api/logs", HTTP_DELETE, HR_AUTH | HR_CSRF, h_logs_clear },
    { "/api/remote/key", HTTP_GET, HR_AUTH, h_key_get },
    { "/api/remote/key", HTTP_POST, HR_AUTH | HR_CSRF, h_key_new },
    { "/api/reset", HTTP_GET, 0, h_reset },
    { "/api/coredump", HTTP_GET, HR_AUTH, h_coredump },
    { "/api/coredump", HTTP_DELETE, HR_AUTH | HR_CSRF, h_coredump_del },
    { "/api/storage", HTTP_GET, HR_AUTH, h_storage },
    { "/api/storage/format", HTTP_POST, HR_WRITE, h_storage_format },
    { "/api/backup", HTTP_GET, HR_AUTH, h_backup_info },
    { "/api/backup", HTTP_POST, HR_AUTH | HR_CSRF, h_backup_write },
    { "/api/backup/restore", HTTP_POST, HR_AUTH | HR_CSRF, h_backup_restore },
    { "/api/backup/dismiss", HTTP_POST, HR_AUTH | HR_CSRF, h_backup_dismiss },
    { "/api/export", HTTP_GET, HR_AUTH, h_export },
};
const size_t api_sys_routes_n = sizeof api_sys_routes / sizeof api_sys_routes[0];
