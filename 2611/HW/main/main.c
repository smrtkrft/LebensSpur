/*
 * LebensSpur 2611 — açılış sırası ve görev kurulumu.
 * Ayrıntı: 2611/docs/04-plan-son.md §3.4 (düzeltilmiş açılış sırası).
 * İskelet aşaması: sürüm ve bellek ölçümü.
 */
#include <inttypes.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "http.h"
#include "cfg.h"

static const char *TAG = "main";

static void log_heap(const char *when)
{
    multi_heap_info_t i;
    heap_caps_get_info(&i, MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "heap[%s]: bos=%u en_buyuk=%u en_az=%u toplam=%u", when,
             (unsigned)i.total_free_bytes, (unsigned)i.largest_free_block,
             (unsigned)i.minimum_free_bytes,
             (unsigned)(i.total_free_bytes + i.total_allocated_bytes));
}

void app_main(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    ESP_LOGI(TAG, "%s v%s (IDF %s)", d->project_name, d->version, d->idf_ver);
    log_heap("acilis");

    esp_err_t r = cfg_init();
    ESP_LOGI(TAG, "cfg_init: %s, not=%s, seq=%u", esp_err_to_name(r),
             cfg_boot_note() ? cfg_boot_note() : "-", (unsigned)cfg_seq());

    size_t n; const char *hash; size_t total = 0;
    const http_embedded_t *f = http_embedded_files(&n, &hash);
    for (size_t i = 0; i < n; i++) total += f[i].len;
    ESP_LOGI(TAG, "GUI: %u dosya, %u bayt gz, hash %s", (unsigned)n, (unsigned)total, hash);
}
