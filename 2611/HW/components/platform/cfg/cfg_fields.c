/*
 * cfg_fields — şema tablosuna göre varsayılan, doğrulama, JSON dönüşümü.
 * IDF bağımsız (yalnız esp_err.h + cJSON); masaüstünde test edilir.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "cfg.h"

#define MASK "***"
#define MULTILINE_MIN_LEN 255u

static void set_num(const cfg_field_t *f, void *p, uint32_t v)
{
    switch (f->type) {
    case CFG_T_BOOL: *(bool *)p = v != 0; break;
    case CFG_T_U8:
    case CFG_T_ENUM: *(uint8_t *)p = (uint8_t)v; break;
    case CFG_T_U16: *(uint16_t *)p = (uint16_t)v; break;
    case CFG_T_U32: *(uint32_t *)p = v; break;
    default: break;
    }
}

static uint32_t get_num(const cfg_field_t *f, const void *p)
{
    switch (f->type) {
    case CFG_T_BOOL: return *(const bool *)p ? 1u : 0u;
    case CFG_T_U8:
    case CFG_T_ENUM: return *(const uint8_t *)p;
    case CFG_T_U16: return *(const uint16_t *)p;
    case CFG_T_U32: return *(const uint32_t *)p;
    default: return 0;
    }
}

void cfg_fields_defaults(const cfg_section_desc_t *d, void *base)
{
    memset(base, 0, d->size);
    for (int i = 0; i < d->nfields; i++) {
        const cfg_field_t *f = &d->fields[i];
        void *p = (char *)base + f->offset;
        if (f->type == CFG_T_STR) {
            snprintf((char *)p, f->size, "%s", f->def_str);
        } else {
            set_num(f, p, f->def_num);
        }
    }
}

static const cfg_field_t *find_field(const cfg_section_desc_t *d, const char *name)
{
    for (int i = 0; i < d->nfields; i++) {
        if (strcmp(d->fields[i].name, name) == 0) return &d->fields[i];
    }
    return NULL;
}

static bool str_ok(const cfg_field_t *f, const char *s)
{
    if (strlen(s) > f->max) return false;
    bool multiline = f->max >= MULTILINE_MIN_LEN;
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c >= 0x20 && *c != 0x7F) continue;
        if (multiline && (*c == '\n' || *c == '\t')) continue;
        return false;
    }
    return true;
}

#define FAIL(fname, ...)                                     \
    do {                                                     \
        if (err) snprintf(err, errlen, __VA_ARGS__);         \
        if (errfield) *errfield = (fname);                   \
        return ESP_ERR_INVALID_ARG;                          \
    } while (0)

static esp_err_t validate_one(const cfg_field_t *f, const cJSON *it, char *err, size_t errlen,
                              const char **errfield)
{
    switch (f->type) {
    case CFG_T_STR:
        if (!cJSON_IsString(it)) FAIL(f->name, "metin bekleniyor");
        if (!str_ok(f, it->valuestring)) FAIL(f->name, "en çok %u karakter, kontrol karakteri yok", (unsigned)f->max);
        return ESP_OK;
    case CFG_T_BOOL:
        if (!cJSON_IsBool(it)) FAIL(f->name, "true/false bekleniyor");
        return ESP_OK;
    case CFG_T_ENUM:
        if (!cJSON_IsString(it)) FAIL(f->name, "seçenek metni bekleniyor");
        for (int k = 0; k < f->enum_count; k++) {
            if (strcmp(it->valuestring, f->enum_vals[k]) == 0) return ESP_OK;
        }
        FAIL(f->name, "geçersiz seçenek");
    default: {
        if (!cJSON_IsNumber(it)) FAIL(f->name, "sayı bekleniyor");
        double v = it->valuedouble;
        if (v != floor(v) || v < (double)f->min || v > (double)f->max) {
            FAIL(f->name, "%u..%u arası tam sayı", (unsigned)f->min, (unsigned)f->max);
        }
        return ESP_OK;
    }
    }
}

static void store_one(const cfg_field_t *f, void *base, const cJSON *it)
{
    void *p = (char *)base + f->offset;
    switch (f->type) {
    case CFG_T_STR:
        /* gizli alan: boş ya da maske geri geldiyse eski değer korunur */
        if ((f->flags & CFG_F_SECRET) && (it->valuestring[0] == 0 || strcmp(it->valuestring, MASK) == 0)) return;
        snprintf((char *)p, f->size, "%s", it->valuestring);
        return;
    case CFG_T_BOOL:
        set_num(f, p, cJSON_IsTrue(it) ? 1u : 0u);
        return;
    case CFG_T_ENUM:
        for (int k = 0; k < f->enum_count; k++) {
            if (strcmp(it->valuestring, f->enum_vals[k]) == 0) set_num(f, p, (uint32_t)k);
        }
        return;
    default:
        set_num(f, p, (uint32_t)it->valuedouble);
        return;
    }
}

esp_err_t cfg_fields_apply_json(const cfg_section_desc_t *d, void *base, const cJSON *obj,
                                bool allow_internal, char *err, size_t errlen, const char **errfield)
{
    if (errfield) *errfield = NULL;
    if (!cJSON_IsObject(obj)) FAIL(NULL, "JSON nesnesi bekleniyor");
    /* önce hepsini doğrula: hata varsa hiçbir alan yazılmaz */
    for (const cJSON *it = obj->child; it; it = it->next) {
        const cfg_field_t *f = it->string ? find_field(d, it->string) : NULL;
        if (!f || ((f->flags & CFG_F_INTERNAL) && !allow_internal)) FAIL(it->string, "bilinmeyen alan");
        esp_err_t r = validate_one(f, it, err, errlen, errfield);
        if (r != ESP_OK) return r;
    }
    for (const cJSON *it = obj->child; it; it = it->next) {
        store_one(find_field(d, it->string), base, it);
    }
    return ESP_OK;
}

cJSON *cfg_fields_to_json(const cfg_section_desc_t *d, const void *base, cfg_export_mode_t mode)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    for (int i = 0; i < d->nfields; i++) {
        const cfg_field_t *f = &d->fields[i];
        const void *p = (const char *)base + f->offset;
        if (mode == CFG_EXPORT_API && (f->flags & CFG_F_INTERNAL)) continue;
        switch (f->type) {
        case CFG_T_STR: {
            const char *s = (const char *)p;
            if (mode == CFG_EXPORT_API && (f->flags & CFG_F_SECRET)) s = s[0] ? MASK : "";
            cJSON_AddStringToObject(o, f->name, s);
            break;
        }
        case CFG_T_BOOL:
            cJSON_AddBoolToObject(o, f->name, get_num(f, p) != 0);
            break;
        case CFG_T_ENUM: {
            uint32_t v = get_num(f, p);
            cJSON_AddStringToObject(o, f->name, v < f->enum_count ? f->enum_vals[v] : f->enum_vals[0]);
            break;
        }
        default:
            cJSON_AddNumberToObject(o, f->name, (double)get_num(f, p));
            break;
        }
    }
    return o;
}

const cfg_section_desc_t *cfg_section_by_name(const char *name)
{
    for (int s = 0; s < CFG_SEC__COUNT; s++) {
        if (strcmp(cfg_sections[s].name, name) == 0) return &cfg_sections[s];
    }
    return NULL;
}
