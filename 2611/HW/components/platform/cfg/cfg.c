/*
 * cfg — NVS tarafı. Ad alanı "cfg": bölüm başına blob {schema,len}+veri, "meta", "grp0".."grp9".
 * Temiz kurulum kuralı (karar 10): "meta" yoksa NVS'in tamamı silinir (ALTE kalıntısı dahil).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cfg.h"

static const char *TAG = "cfg";
#define NS        "cfg"
#define NS_RT     "rt"
#define META_KEY  "meta"
#define META_MAGIC 0x3632534Cu   /* "LS26" */

typedef struct { uint32_t magic; uint16_t version; uint16_t _pad; uint32_t seq; } meta_t;
typedef struct { uint16_t schema; uint16_t len; } blob_hdr_t;

static cfg_t s_cfg;
static meta_t s_meta;
static uint16_t s_group_used;            /* bit i: grp i kullanımda */
static SemaphoreHandle_t s_lock;
static cfg_change_cb_t s_change_cb;
static char s_boot_note[48];
/* NVS blob G/C tamponu: yığında 4 KB açılamaz (main görevi 3,5 KB). Yalnız s_lock altında
 * (ya da cfg_init sırasında, henüz başka görev yokken) kullanılır. */
static uint8_t s_io[sizeof(blob_hdr_t) + 4000];

static void *sec_ptr(const cfg_section_desc_t *d) { return (char *)&s_cfg + d->offset; }

static esp_err_t blob_read(nvs_handle_t h, const char *key, const cfg_section_desc_t *d, void *out)
{
    uint8_t *buf = s_io;
    size_t len = sizeof(s_io);
    esp_err_t r = nvs_get_blob(h, key, buf, &len);
    if (r != ESP_OK) return r;
    blob_hdr_t hd;
    memcpy(&hd, buf, sizeof hd);
    if (len != sizeof(blob_hdr_t) + d->size || hd.schema != d->schema || hd.len != d->size) {
        return ESP_ERR_INVALID_VERSION;
    }
    memcpy(out, buf + sizeof(blob_hdr_t), d->size);
    return ESP_OK;
}

static esp_err_t blob_write(nvs_handle_t h, const char *key, const cfg_section_desc_t *d, const void *data)
{
    uint8_t *buf = s_io;
    blob_hdr_t hd = { .schema = d->schema, .len = d->size };
    memcpy(buf, &hd, sizeof hd);
    memcpy(buf + sizeof hd, data, d->size);
    esp_err_t r = nvs_set_blob(h, key, buf, sizeof hd + d->size);
    return r == ESP_OK ? nvs_commit(h) : r;
}

static esp_err_t meta_write(nvs_handle_t h)
{
    esp_err_t r = nvs_set_blob(h, META_KEY, &s_meta, sizeof s_meta);
    return r == ESP_OK ? nvs_commit(h) : r;
}

static void group_key(int idx, char key[8]) { snprintf(key, 8, "%s%d", cfg_group_desc.nvs_key, idx); }

static esp_err_t nvs_reset_all(const char *why)
{
    snprintf(s_boot_note, sizeof s_boot_note, "nvs_erased:%s", why);
    ESP_LOGW(TAG, "NVS siliniyor (%s)", why);
    nvs_flash_deinit();
    esp_err_t r = nvs_flash_erase();
    return r == ESP_OK ? nvs_flash_init() : r;
}

esp_err_t cfg_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND || r == ESP_FAIL) {
        r = nvs_reset_all(r == ESP_ERR_NVS_NO_FREE_PAGES ? "no_free_pages" :
                          r == ESP_ERR_NVS_NEW_VERSION_FOUND ? "new_version" : "init_fail");
    }
    if (r != ESP_OK) return r;

    nvs_handle_t h;
    r = nvs_open(NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    size_t ml = sizeof s_meta;
    if (nvs_get_blob(h, META_KEY, &s_meta, &ml) != ESP_OK || ml != sizeof s_meta || s_meta.magic != META_MAGIC) {
        nvs_close(h);
        r = nvs_reset_all("no_meta");        /* temiz kurulum ya da ALTE kalıntısı */
        if (r != ESP_OK) return r;
        r = nvs_open(NS, NVS_READWRITE, &h);
        if (r != ESP_OK) return r;
        s_meta = (meta_t){ .magic = META_MAGIC, .version = 1, .seq = 0 };
        r = meta_write(h);
        if (r != ESP_OK) { nvs_close(h); return r; }
    }

    for (int s = 0; s < CFG_SEC__COUNT; s++) {
        const cfg_section_desc_t *d = &cfg_sections[s];
        if (blob_read(h, d->nvs_key, d, sec_ptr(d)) != ESP_OK) {
            cfg_fields_defaults(d, sec_ptr(d));    /* yok ya da şema değişti */
        }
    }
    s_group_used = 0;
    static cfg_group_t g;                         /* yığın değil: 2,8 KB */
    for (int i = 0; i < CFG_GROUP_COUNT; i++) {
        char key[8];
        group_key(i, key);
        if (blob_read(h, key, &cfg_group_desc, &g) == ESP_OK && g.used) s_group_used |= (uint16_t)(1u << i);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "hazir: seq=%u grup=%d", (unsigned)s_meta.seq, cfg_group_count());
    return ESP_OK;
}

const char *cfg_boot_note(void) { return s_boot_note[0] ? s_boot_note : NULL; }
const cfg_t *cfg_get(void) { return &s_cfg; }
uint32_t cfg_seq(void) { return s_meta.seq; }
void cfg_set_change_cb(cfg_change_cb_t cb) { s_change_cb = cb; }

static esp_err_t persist(const char *key, const cfg_section_desc_t *d, const void *data)
{
    nvs_handle_t h;
    esp_err_t r = nvs_open(NS, NVS_READWRITE, &h);
    if (r != ESP_OK) return r;
    r = blob_write(h, key, d, data);
    if (r == ESP_OK) {
        s_meta.seq++;
        r = meta_write(h);
    }
    nvs_close(h);
    return r;
}

esp_err_t cfg_set_section_json(cfg_section_t s, const cJSON *obj, char *err, size_t errlen,
                               const char **errfield)
{
    if ((unsigned)s >= CFG_SEC__COUNT) return ESP_ERR_INVALID_ARG;
    const cfg_section_desc_t *d = &cfg_sections[s];
    static uint8_t tmp[4000];                     /* s_lock altında kullanılır */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(tmp, sec_ptr(d), d->size);
    esp_err_t r = cfg_fields_apply_json(d, tmp, obj, false, err, errlen, errfield);
    if (r == ESP_OK) r = persist(d->nvs_key, d, tmp);
    if (r == ESP_OK) memcpy(sec_ptr(d), tmp, d->size);
    xSemaphoreGive(s_lock);
    if (r == ESP_OK && s_change_cb) s_change_cb(s);
    return r;
}

cJSON *cfg_get_section_json(cfg_section_t s, cfg_export_mode_t mode)
{
    if ((unsigned)s >= CFG_SEC__COUNT) return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *o = cfg_fields_to_json(&cfg_sections[s], sec_ptr(&cfg_sections[s]), mode);
    xSemaphoreGive(s_lock);
    return o;
}

esp_err_t cfg_copy_section(cfg_section_t s, void *out)
{
    if ((unsigned)s >= CFG_SEC__COUNT) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, sec_ptr(&cfg_sections[s]), cfg_sections[s].size);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static esp_err_t group_get_locked(int idx, cfg_group_t *out)
{
    if (!(s_group_used & (1u << idx))) return ESP_ERR_NOT_FOUND;
    nvs_handle_t h;
    esp_err_t r = nvs_open(NS, NVS_READONLY, &h);
    if (r != ESP_OK) return r;
    char key[8];
    group_key(idx, key);
    r = blob_read(h, key, &cfg_group_desc, out);
    nvs_close(h);
    return r;
}

esp_err_t cfg_group_get(int idx, cfg_group_t *out)
{
    if (idx < 0 || idx >= CFG_GROUP_COUNT) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t r = group_get_locked(idx, out);
    xSemaphoreGive(s_lock);
    return r;
}

esp_err_t cfg_group_set_json(int idx, const cJSON *obj, char *err, size_t errlen, const char **errfield)
{
    if (idx < 0 || idx >= CFG_GROUP_COUNT) return ESP_ERR_INVALID_ARG;
    static cfg_group_t g;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (group_get_locked(idx, &g) != ESP_OK) cfg_fields_defaults(&cfg_group_desc, &g);
    esp_err_t r = cfg_fields_apply_json(&cfg_group_desc, &g, obj, false, err, errlen, errfield);
    if (r == ESP_OK) {
        g.used = true;
        char key[8];
        group_key(idx, key);
        r = persist(key, &cfg_group_desc, &g);
    }
    if (r == ESP_OK) s_group_used |= (uint16_t)(1u << idx);
    xSemaphoreGive(s_lock);
    if (r == ESP_OK && s_change_cb) s_change_cb(CFG_SEC__COUNT);
    return r;
}

esp_err_t cfg_group_delete(int idx)
{
    if (idx < 0 || idx >= CFG_GROUP_COUNT) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t r = nvs_open(NS, NVS_READWRITE, &h);
    if (r == ESP_OK) {
        char key[8];
        group_key(idx, key);
        r = nvs_erase_key(h, key);
        if (r == ESP_ERR_NVS_NOT_FOUND) r = ESP_OK;
        if (r == ESP_OK) r = nvs_commit(h);
        nvs_close(h);
    }
    if (r == ESP_OK) s_group_used &= (uint16_t)~(1u << idx);
    xSemaphoreGive(s_lock);
    if (r == ESP_OK && s_change_cb) s_change_cb(CFG_SEC__COUNT);
    return r;
}

int cfg_group_count(void) { return __builtin_popcount(s_group_used); }

esp_err_t cfg_factory_reset(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t r = nvs_open(NS_RT, NVS_READWRITE, &h);
    if (r == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    r = nvs_open(NS, NVS_READWRITE, &h);
    if (r == ESP_OK) {
        r = nvs_erase_all(h);
        uint32_t seq = s_meta.seq + 1;
        s_meta = (meta_t){ .magic = META_MAGIC, .version = 1, .seq = seq };
        if (r == ESP_OK) r = meta_write(h);
        nvs_close(h);
    }
    for (int s = 0; s < CFG_SEC__COUNT; s++) cfg_fields_defaults(&cfg_sections[s], sec_ptr(&cfg_sections[s]));
    s_group_used = 0;
    xSemaphoreGive(s_lock);
    return r;
}
