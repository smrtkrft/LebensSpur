/*
 * api_groups — tetikleme mail grupları ve ekleri; kanal testleri ve iş durumu.
 * Ek yükleme httpd'de akışla yazılır (sys_worker kuralının bilinçli istisnası: gövde zaten akış).
 * Sınırlar: dosya ≤ 2 MB, grup başına ≤ 5 dosya ve ≤ 4 MB; ad [A-Za-z0-9._- ] ≤ 60, nokta ile başlamaz.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cJSON.h"
#include "api_priv.h"
#include "cfg.h"
#include "core.h"
#include "evlog.h"
#include "jobs.h"
#include "outbox.h"
#include "storage.h"

#define FILE_MAX  (2u * 1024u * 1024u)
#define GROUP_MAX (4u * 1024u * 1024u)
#define FILES_MAX 5

static int group_idx(http_req_t *r)
{
    int i = http_param_int(r, 0, -1);
    return (i >= 0 && i < CFG_GROUP_COUNT) ? i : -1;
}

static void dir_of(int g, char *out, size_t cap) { snprintf(out, cap, STG_DATA "/mail/g%d", g); }

/* klasördeki dosyalar: sayı ve toplam boyut; arr verilirse listeyi doldurur */
static int scan_files(int g, uint32_t *total, cJSON *arr)
{
    char dir[32], path[112];
    dir_of(g, dir, sizeof dir);
    DIR *d = opendir(dir);
    int n = 0;
    *total = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        struct stat st;
        snprintf(path, sizeof path, "%s/%.60s", dir, e->d_name);
        if (stat(path, &st) != 0) continue;
        n++;
        *total += (uint32_t)st.st_size;
        if (arr) {
            cJSON *f = cJSON_CreateObject();
            cJSON_AddStringToObject(f, "name", e->d_name);
            cJSON_AddNumberToObject(f, "size", (double)st.st_size);
            cJSON_AddItemToArray(arr, f);
        }
    }
    if (d) closedir(d);
    return n;
}

static bool name_ok(char *name)
{
    /* %XX çöz (yerinde) */
    char *w = name;
    for (const char *p = name; *p; p++) {
        if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char h[3] = { p[1], p[2], 0 };
            *w++ = (char)strtol(h, NULL, 16);
            p += 2;
        } else {
            *w++ = *p;
        }
    }
    *w = 0;
    size_t n = strlen(name);
    if (n == 0 || n > 60 || name[0] == '.') return false;
    for (const char *p = name; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '_' || *p == '-' || *p == ' ')) return false;
    }
    return true;
}

static esp_err_t h_groups(http_req_t *r)
{
    cJSON *arr = cJSON_CreateArray();
    static cfg_group_t g;
    bool fs = storage_acquire(STG_DATA);
    for (int i = 0; i < CFG_GROUP_COUNT; i++) {
        if (cfg_group_get(i, &g) != ESP_OK) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", i);
        cJSON_AddStringToObject(o, "name", g.name);
        int rc = 0;
        for (const char *p = g.recipients; *p; p++) if (*p == '@') rc++;
        cJSON_AddNumberToObject(o, "recipients", rc);
        uint32_t tot = 0;
        cJSON_AddNumberToObject(o, "files", fs ? scan_files(i, &tot, NULL) : 0);
        cJSON_AddItemToArray(arr, o);
    }
    if (fs) storage_release(STG_DATA);
    return http_ok(r, arr);
}

static esp_err_t h_group_get(http_req_t *r)
{
    int i = group_idx(r);
    static cfg_group_t g;
    if (i < 0 || cfg_group_get(i, &g) != ESP_OK) return http_fail(r, 404, "not_found", "", NULL);
    return http_ok(r, cfg_fields_to_json(&cfg_group_desc, &g, CFG_EXPORT_API));
}

static esp_err_t h_group_put(http_req_t *r)
{
    int i = group_idx(r);
    if (i < 0) return http_fail(r, 404, "not_found", "", NULL);
    char err[96];
    const char *field = NULL;
    if (cfg_group_set_json(i, http_body(r), err, sizeof err, &field) != ESP_OK) {
        return http_fail(r, 400, "validation", err, field);
    }
    return h_group_get(r);
}

static esp_err_t h_group_delete(http_req_t *r)
{
    int i = group_idx(r);
    if (i < 0) return http_fail(r, 404, "not_found", "", NULL);
    if (cfg_group_delete(i) != ESP_OK) return http_fail(r, 500, "internal", "", NULL);
    if (storage_acquire(STG_DATA)) {                    /* ekleri de sil (küçük, hızlı) */
        char dir[32], path[112];
        dir_of(i, dir, sizeof dir);
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(path, sizeof path, "%s/%.60s", dir, e->d_name);
            remove(path);
        }
        if (d) closedir(d);
        rmdir(dir);
        storage_release(STG_DATA);
    }
    return http_ok(r, NULL);
}

static esp_err_t h_files(http_req_t *r)
{
    int i = group_idx(r);
    if (i < 0) return http_fail(r, 404, "not_found", "", NULL);
    if (!storage_acquire(STG_DATA)) return http_fail(r, 409, "storage_absent", "", NULL);
    cJSON *o = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(o, "files");
    uint32_t tot;
    scan_files(i, &tot, arr);
    storage_release(STG_DATA);
    cJSON_AddNumberToObject(o, "total", tot);
    cJSON_AddNumberToObject(o, "limit_file", FILE_MAX);
    cJSON_AddNumberToObject(o, "limit_total", GROUP_MAX);
    cJSON_AddNumberToObject(o, "limit_count", FILES_MAX);
    return http_ok(r, o);
}

static esp_err_t h_file_put(http_req_t *r)
{
    int i = group_idx(r);
    char name[64];
    strlcpy(name, http_param(r, 1) ? http_param(r, 1) : "", sizeof name);
    if (i < 0 || !name_ok(name)) return http_fail(r, 400, "validation", "dosya adi", "name");
    static cfg_group_t g;
    if (cfg_group_get(i, &g) != ESP_OK) return http_fail(r, 409, "group_not_saved", "", NULL);
    size_t len = http_raw(r)->content_len;
    if (len == 0 || len > FILE_MAX) return http_fail(r, 413, "too_large", "en cok 2 MB", NULL);
    if (!storage_acquire(STG_DATA)) return http_fail(r, 409, "storage_absent", "", NULL);
    uint32_t tot;
    int n = scan_files(i, &tot, NULL);
    char dir[32], path[112];
    dir_of(i, dir, sizeof dir);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    struct stat st;
    bool exists = stat(path, &st) == 0;
    if (exists) { tot -= (uint32_t)st.st_size; n--; }
    if (n >= FILES_MAX || tot + len > GROUP_MAX) {
        storage_release(STG_DATA);
        return http_fail(r, 409, "limit", "grup basina en cok 5 dosya / 4 MB", NULL);
    }
    mkdir(STG_DATA "/mail", 0775);
    mkdir(dir, 0775);
    char tmp[124];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f != NULL;
    static char chunk[2048];
    size_t got = 0;
    int timeouts = 0;
    while (ok && got < len) {
        int k = httpd_req_recv(http_raw(r), chunk, len - got < sizeof chunk ? len - got : sizeof chunk);
        if (k == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (k <= 0) { ok = false; break; }
        ok = fwrite(chunk, 1, (size_t)k, f) == (size_t)k;
        got += (size_t)k;
    }
    if (f) { fflush(f); fsync(fileno(f)); fclose(f); }
    if (ok) { remove(path); ok = rename(tmp, path) == 0; }
    else remove(tmp);
    storage_release(STG_DATA);
    if (!ok) return http_fail(r, 500, "write_failed", "", NULL);
    evlog_write(EVL_INFO, "mail", "grup %d: ek yuklendi (%s, %u B)", i + 1, name, (unsigned)len);
    return http_ok(r, NULL);
}

static esp_err_t h_file_delete(http_req_t *r)
{
    int i = group_idx(r);
    char name[64];
    strlcpy(name, http_param(r, 1) ? http_param(r, 1) : "", sizeof name);
    if (i < 0 || !name_ok(name)) return http_fail(r, 400, "validation", "dosya adi", "name");
    if (!storage_acquire(STG_DATA)) return http_fail(r, 409, "storage_absent", "", NULL);
    char path[112];
    snprintf(path, sizeof path, STG_DATA "/mail/g%d/%s", i, name);
    int rr = remove(path);
    storage_release(STG_DATA);
    return rr == 0 ? http_ok(r, NULL) : http_fail(r, 404, "not_found", "", NULL);
}

/* ---------- testler ve işler ---------- */

static esp_err_t h_test(http_req_t *r)
{
    const char *w = http_param(r, 0);
    if (strcmp(w, "relay") == 0) {
        core_event_t ev = { .type = EV_RELAY_TEST, .u.relay_test.duration_s = 0 };
        return core_post(&ev) == ESP_OK ? http_ok(r, NULL) : http_fail(r, 503, "busy", "", NULL);
    }
    static const char *const names[] = { "smtp", "telegram", "webhook", "mqtt" };
    int ch = -1;
    for (int i = 0; i < 4; i++) if (strcmp(w, names[i]) == 0) ch = i;
    if (ch < 0) return http_fail(r, 404, "not_found", "", NULL);
    char kind[16];
    snprintf(kind, sizeof kind, "test_%s", names[ch]);
    job_id_t id = jobs_create(kind);
    core_event_t ev = { .type = EV_TEST_SEND, .u.test = { .ch = (uint8_t)ch, .job_id = id } };
    if (core_post(&ev) != ESP_OK) {
        jobs_update(id, JOB_FAILED, 0, "busy");
        return http_fail(r, 503, "busy", "", NULL);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "job_id", id);
    return http_ok(r, o);
}

static esp_err_t h_job(http_req_t *r)
{
    job_info_t j;
    if (!jobs_get((job_id_t)http_param_int(r, 0, 0), &j)) return http_fail(r, 404, "not_found", "", NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", j.id);
    cJSON_AddStringToObject(o, "kind", j.kind);
    cJSON_AddStringToObject(o, "state", jobs_state_name(j.state));
    cJSON_AddNumberToObject(o, "pct", j.pct);
    cJSON_AddStringToObject(o, "message", j.msg);
    return http_ok(r, o);
}

const http_route_t api_group_routes[] = {
    { "/api/groups", HTTP_GET, HR_AUTH, h_groups },
    { "/api/groups/{}", HTTP_GET, HR_AUTH, h_group_get },
    { "/api/groups/{}", HTTP_PUT, HR_WRITE, h_group_put },
    { "/api/groups/{}", HTTP_DELETE, HR_AUTH | HR_CSRF, h_group_delete },
    { "/api/groups/{}/files", HTTP_GET, HR_AUTH, h_files },
    { "/api/groups/{}/files/{}", HTTP_PUT, HR_AUTH | HR_CSRF, h_file_put },
    { "/api/groups/{}/files/{}", HTTP_DELETE, HR_AUTH | HR_CSRF, h_file_delete },
    { "/api/test/{}", HTTP_POST, HR_AUTH | HR_CSRF, h_test },
    { "/api/jobs/{}", HTTP_GET, HR_AUTH, h_job },
};
const size_t api_group_routes_n = sizeof api_group_routes / sizeof api_group_routes[0];
