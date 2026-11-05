/*
 * cfg_backup — yapılandırmanın harici flash yedeği (/data/config, karar 15: sırlar dahil).
 * Biçim JSONL, bölüm başına satır (≤12 KB heap; tek büyük JSON ~100 KB isterdi):
 *   {"meta":{"ver":1,"seq":N,"fw":"x.y.z"}}
 *   {"s":"smtp","d":{...}}            (her bölüm)
 *   {"g":3,"d":{...}}                  (kullanılan her grup)
 *   {"end":"<crc32 hex>"}              (önceki tüm baytların CRC32'si)
 * Yazım: config.tmp → (config.jsonl → config.prev) → config.jsonl; rename atomik (LittleFS).
 */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "cfg_priv.h"
#include "storage.h"
#include "sys_worker.h"

static const char *TAG = "cfg_backup";
#define DIR   STG_DATA "/config"
#define F_CUR DIR "/config.jsonl"
#define F_PRV DIR "/config.prev.jsonl"
#define F_TMP DIR "/config.tmp"
#define LINE_CAP 6144

static atomic_bool s_pending;
static char s_line[LINE_CAP];
static cfg_group_t s_grp;

static bool put_line(FILE *f, cJSON *o, uint32_t *crc)
{
    bool ok = o && cJSON_PrintPreallocated(o, s_line, LINE_CAP - 2, false);
    cJSON_Delete(o);
    if (!ok) return false;
    size_t n = strlen(s_line);
    s_line[n++] = '\n';
    *crc = esp_rom_crc32_le(*crc, (const uint8_t *)s_line, (uint32_t)n);
    return fwrite(s_line, 1, n, f) == n;
}

static cJSON *wrap(const char *k, cJSON *v, const char *k2, cJSON *v2)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) { cJSON_Delete(v); cJSON_Delete(v2); return NULL; }
    cJSON_AddItemToObject(o, k, v);
    if (k2) cJSON_AddItemToObject(o, k2, v2);
    return o;
}

esp_err_t cfg_backup_write(void)
{
    if (!storage_acquire(STG_DATA)) return ESP_ERR_INVALID_STATE;
    mkdir(DIR, 0775);
    FILE *f = fopen(F_TMP, "w");
    esp_err_t r = f ? ESP_OK : ESP_FAIL;
    uint32_t crc = 0;
    cfg_priv_lock();
    if (r == ESP_OK) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "ver", 1);
        cJSON_AddNumberToObject(m, "seq", (double)cfg_seq());
        cJSON_AddStringToObject(m, "fw", esp_app_get_description()->version);
        if (!put_line(f, wrap("meta", m, NULL, NULL), &crc)) r = ESP_FAIL;
    }
    for (int s = 0; s < CFG_SEC__COUNT && r == ESP_OK; s++) {
        const cfg_section_desc_t *d = &cfg_sections[s];
        cJSON *data = cfg_fields_to_json(d, cfg_priv_section(d), CFG_EXPORT_BACKUP);
        if (!put_line(f, wrap("s", cJSON_CreateString(d->name), "d", data), &crc)) r = ESP_FAIL;
    }
    for (int i = 0; i < CFG_GROUP_COUNT && r == ESP_OK; i++) {
        if (cfg_priv_group_read(i, &s_grp) != ESP_OK) continue;
        cJSON *data = cfg_fields_to_json(&cfg_group_desc, &s_grp, CFG_EXPORT_BACKUP);
        if (!put_line(f, wrap("g", cJSON_CreateNumber(i), "d", data), &crc)) r = ESP_FAIL;
    }
    cfg_priv_unlock();
    if (r == ESP_OK) {
        int n = snprintf(s_line, LINE_CAP, "{\"end\":\"%08lx\"}\n", (unsigned long)crc);
        if (fwrite(s_line, 1, (size_t)n, f) != (size_t)n) r = ESP_FAIL;
    }
    if (f) {
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    if (r == ESP_OK) {
        remove(F_PRV);
        rename(F_CUR, F_PRV);
        if (rename(F_TMP, F_CUR) != 0) r = ESP_FAIL;
    } else {
        remove(F_TMP);
    }
    storage_release(STG_DATA);
    ESP_LOGI(TAG, "yedek %s (seq %lu)", r == ESP_OK ? "yazildi" : "YAZILAMADI", (unsigned long)cfg_seq());
    return r;
}

static void backup_job(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));            /* art arda kayıtları birleştir */
    atomic_store(&s_pending, false);
    cfg_backup_write();
}

void cfg_backup_schedule(void)
{
    if (storage_state(STG_DATA) != STG_MOUNTED) return;
    if (!atomic_exchange(&s_pending, true)) {
        if (sys_worker_post(backup_job, NULL) != ESP_OK) atomic_store(&s_pending, false);
    }
}

/* dosyayı doğrular; geçerliyse seq'i döndürür. apply=true ise içeriği NVS'e uygular. */
static esp_err_t process(const char *path, bool apply, uint32_t *seq_out)
{
    FILE *f = fopen(path, "r");
    if (!f) return ESP_ERR_NOT_FOUND;
    uint32_t crc = 0;
    bool ended = false, crc_ok = false;
    esp_err_t r = ESP_OK;
    while (r == ESP_OK && fgets(s_line, LINE_CAP, f)) {
        size_t n = strlen(s_line);
        if (n == 0 || s_line[n - 1] != '\n') { r = ESP_ERR_INVALID_SIZE; break; }
        cJSON *o = cJSON_ParseWithLength(s_line, n);
        if (!o) { r = ESP_ERR_INVALID_RESPONSE; break; }
        const cJSON *end = cJSON_GetObjectItem(o, "end");
        if (cJSON_IsString(end)) {
            char want[12];
            snprintf(want, sizeof want, "%08lx", (unsigned long)crc);
            crc_ok = strcmp(want, end->valuestring) == 0;
            ended = true;
            cJSON_Delete(o);
            break;
        }
        crc = esp_rom_crc32_le(crc, (const uint8_t *)s_line, (uint32_t)n);
        const cJSON *meta = cJSON_GetObjectItem(o, "meta"), *data = cJSON_GetObjectItem(o, "d");
        const cJSON *sq = meta ? cJSON_GetObjectItem(meta, "seq") : NULL;
        if (cJSON_IsNumber(sq) && seq_out) *seq_out = (uint32_t)sq->valuedouble;
        if (apply && data) {
            const cJSON *sname = cJSON_GetObjectItem(o, "s"), *gidx = cJSON_GetObjectItem(o, "g");
            char err[64];
            cfg_priv_lock();
            if (cJSON_IsString(sname)) {
                const cfg_section_desc_t *d = cfg_section_by_name(sname->valuestring);
                if (d) {
                    static uint8_t tmp[4000];
                    cfg_fields_defaults(d, tmp);
                    if (cfg_fields_apply_json(d, tmp, data, true, err, sizeof err, NULL) == ESP_OK &&
                        cfg_priv_persist(d->nvs_key, d, tmp) == ESP_OK) {
                        memcpy(cfg_priv_section(d), tmp, d->size);
                    }
                }
            } else if (cJSON_IsNumber(gidx) && gidx->valueint >= 0 && gidx->valueint < CFG_GROUP_COUNT) {
                cfg_fields_defaults(&cfg_group_desc, &s_grp);
                if (cfg_fields_apply_json(&cfg_group_desc, &s_grp, data, true, err, sizeof err, NULL) == ESP_OK) {
                    char key[8];
                    snprintf(key, sizeof key, "%s%d", cfg_group_desc.nvs_key, gidx->valueint);
                    s_grp.used = true;
                    if (cfg_priv_persist(key, &cfg_group_desc, &s_grp) == ESP_OK) {
                        cfg_priv_group_mark(gidx->valueint, true);
                    }
                }
            }
            cfg_priv_unlock();
        }
        cJSON_Delete(o);
    }
    fclose(f);
    if (r == ESP_OK && !(ended && crc_ok)) r = ESP_ERR_INVALID_CRC;
    return r;
}

esp_err_t cfg_backup_info(uint32_t *seq)
{
    if (!storage_acquire(STG_DATA)) return ESP_ERR_INVALID_STATE;
    esp_err_t r = process(F_CUR, false, seq);
    if (r != ESP_OK) r = process(F_PRV, false, seq);
    storage_release(STG_DATA);
    return r;
}

esp_err_t cfg_backup_restore(void)
{
    if (!storage_acquire(STG_DATA)) return ESP_ERR_INVALID_STATE;
    const char *src = process(F_CUR, false, NULL) == ESP_OK ? F_CUR :
                      process(F_PRV, false, NULL) == ESP_OK ? F_PRV : NULL;
    esp_err_t r = src ? process(src, true, NULL) : ESP_ERR_NOT_FOUND;
    storage_release(STG_DATA);
    ESP_LOGI(TAG, "geri yukleme: %s (%s)", esp_err_to_name(r), src ? src : "-");
    return r;
}
