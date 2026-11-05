/* cfg.c <-> cfg_backup.c iç arayüzü */
#pragma once
#include "cfg.h"
void      cfg_priv_lock(void);
void      cfg_priv_unlock(void);
void     *cfg_priv_section(const cfg_section_desc_t *d);       /* RAM kopyası (kilit altında) */
esp_err_t cfg_priv_persist(const char *key, const cfg_section_desc_t *d, const void *data);
esp_err_t cfg_priv_group_read(int idx, cfg_group_t *out);       /* kilit altında */
void      cfg_priv_group_mark(int idx, bool used);
void      cfg_backup_schedule(void);                            /* değişiklikte: 300 ms birleştirip yedek yaz */
