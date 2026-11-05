/*
 * LebensSpur 2611 — açılış sırası ve görev kurulumu.
 * Ayrıntı: 2611/docs/04-plan-son.md §3.4 (düzeltilmiş açılış sırası).
 * İskelet aşaması: sürüm ve bellek ölçümü.
 */
#include <inttypes.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "http.h"
#include "cfg.h"
#include "evlog.h"
#include "storage.h"

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

    evlog_init();
    esp_err_t r = cfg_init();
    if (r != ESP_OK) evlog_write(EVL_ERR, "sys", "cfg_init: %s", esp_err_to_name(r));
    if (cfg_boot_note()) evlog_write(EVL_WARN, "sys", "%s", cfg_boot_note());
    evlog_write(EVL_INFO, "sys", "acilis v%s", d->version);

    int64_t t0 = esp_timer_get_time();
    r = storage_init();
    size_t tot = 0, used = 0;
    storage_info(STG_DATA, &tot, &used);
    evlog_write(r == ESP_OK ? EVL_INFO : EVL_WARN, "sys", "depolama: %s, %lu MB, /data %s %u/%u KB, %lld ms%s%s",
                esp_err_to_name(r), (unsigned long)(storage_chip_size() >> 20),
                storage_state_name(storage_state(STG_DATA)), (unsigned)(used >> 10), (unsigned)(tot >> 10),
                (esp_timer_get_time() - t0) / 1000, storage_note() ? " | " : "", storage_note() ? storage_note() : "");

    char line[320];
    while (evlog_pop_line(line, sizeof line)) ESP_LOGI(TAG, "halka: %s", line);   /* gecici: dosya 1.5'te */

    size_t n; const char *hash; size_t total = 0;
    const http_embedded_t *f = http_embedded_files(&n, &hash);
    for (size_t i = 0; i < n; i++) total += f[i].len;
    ESP_LOGI(TAG, "GUI: %u dosya, %u bayt gz, hash %s", (unsigned)n, (unsigned)total, hash);
}
