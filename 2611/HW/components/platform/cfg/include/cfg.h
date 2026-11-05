/*
 * cfg — tek yapılandırma kaynağı (04-plan-son.md §2.1).
 *
 * Asıl kopya dahili NVS'te ("cfg" ad alanı, bölüm başına blob {schema,len}+veri);
 * RAM'de tek cfg_t. Gruplar (10 × ~2,8 KB) RAM'de tutulmaz, istendiğinde NVS'ten okunur.
 * Alan tanımları cfg_schema.json'dan üretilir (tools/gen_cfg.py).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cfg_schema.gen.h"

typedef struct cJSON cJSON;

/* ---- saf kısım (cfg_fields.c; masaüstünde test edilir) ---- */

typedef enum {
    CFG_EXPORT_API,      /* gizli alanlar "***" (boşsa ""), internal alanlar yok */
    CFG_EXPORT_BACKUP,   /* her şey açık (yalnız yedek dosyası için) */
} cfg_export_mode_t;

void   cfg_fields_defaults(const cfg_section_desc_t *d, void *base);
/* obj'yi doğrulayıp base'e uygular. Hata varsa base DEĞİŞMEZ.
 * Kurallar: bilinmeyen/internal anahtar → hata; tip/sınır → hata; gizli str boş → eski korunur;
 * kontrol karakterleri reddedilir (len ≥ 255 alanlarda \n ve \t serbest).
 * allow_internal: yedekten geri yüklemede internal alanlara izin verir.
 * errfield şema tablosunu ya da obj içini gösterir: obj silinmeden kullanılmalı. */
esp_err_t cfg_fields_apply_json(const cfg_section_desc_t *d, void *base, const cJSON *obj,
                                bool allow_internal, char *err, size_t errlen, const char **errfield);
cJSON *cfg_fields_to_json(const cfg_section_desc_t *d, const void *base, cfg_export_mode_t mode);
const cfg_section_desc_t *cfg_section_by_name(const char *name);   /* NULL → yok */

/* ---- cihaz kısmı (cfg.c; NVS) ---- */

/* NVS'i başlatır. "meta" yoksa (temiz kurulum / ALTE kalıntısı) ya da NVS bozuksa tüm NVS
 * silinir ve varsayılanlar yazılır; nedeni cfg_boot_note() ile okunur (evlog'a aktarılır). */
esp_err_t   cfg_init(void);
const char *cfg_boot_note(void);              /* NULL ya da "nvs_erased:<neden>" */
const cfg_t *cfg_get(void);                   /* salt okunur RAM kopyası (sahip görevde okunmalı) */
esp_err_t   cfg_copy_section(cfg_section_t s, void *out);  /* kilit altında tutarlı kopya (diğer görevler) */
uint32_t    cfg_seq(void);                    /* her başarılı kayıtta artar (yedek için) */

esp_err_t cfg_set_section_json(cfg_section_t s, const cJSON *obj, char *err, size_t errlen,
                               const char **errfield);
cJSON    *cfg_get_section_json(cfg_section_t s, cfg_export_mode_t mode);

esp_err_t cfg_group_get(int idx, cfg_group_t *out);   /* kullanılmıyorsa ESP_ERR_NOT_FOUND */
esp_err_t cfg_group_set_json(int idx, const cJSON *obj, char *err, size_t errlen, const char **errfield);
esp_err_t cfg_group_delete(int idx);
int       cfg_group_count(void);                      /* used=true olanlar */

esp_err_t cfg_factory_reset(void);                    /* kademe 1: cfg + rt silinir, meta yeniden */

typedef void (*cfg_change_cb_t)(cfg_section_t s);     /* grup değişimi: s = CFG_SEC__COUNT */
void cfg_set_change_cb(cfg_change_cb_t cb);

/* ---- yedek (cfg_backup.c): /data/config/config.jsonl (+ .prev), JSONL + CRC32, sırlar dahil ---- */
esp_err_t cfg_backup_write(void);                     /* sys_worker'da; değişiklikte kendiliğinden planlanır */
esp_err_t cfg_backup_info(uint32_t *seq);             /* geçerli yedek var mı (önce güncel, sonra önceki) */
esp_err_t cfg_backup_restore(void);                   /* sys_worker'da; sonra yeniden başlatma önerilir */
