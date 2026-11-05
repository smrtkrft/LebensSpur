/*
 * LebensSpur 2611 — açılış sırası ve görev kurulumu.
 * Ayrıntı: 2611/docs/04-plan-son.md §3.4 (düzeltilmiş açılış sırası).
 * İskelet aşaması: sürüm ve bellek ölçümü.
 */
#include <inttypes.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http.h"
#include "cfg.h"
#include "evlog.h"
#include "storage.h"
#include "sys_worker.h"
#include "core.h"
#include "io.h"
#include "net_wifi.h"
#include "api.h"
#include "delivery.h"
#include "cJSON.h"

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

/* gecici teshis (adim 3.4'te API'ye tasinir): gunlugun son satirlari */
static bool print_line(const char *l, void *ctx) { (*(int *)ctx)++; ESP_LOGI(TAG, "gunluk: %.150s", l); return true; }
static void log_tail(void)
{
    int n = 0; uint32_t next;
    vTaskDelay(pdMS_TO_TICKS(200));
    evlog_read(0, 4, print_line, &n, &next);
    ESP_LOGI(TAG, "gunluk: %d satir, sonraki imlec %lu", n, (unsigned long)next);
}

static void on_link(bool up) { core_post(&(core_event_t){ .type = EV_NET_LINK, .u.link.up = up }); }

void app_main(void)
{
    io_relay_early_init();                       /* ilk iş: röle güvenli seviyede (NVS'ten önce) */
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

    sys_worker_start();
    evlog_file_start();
    log_tail();

    evlog_boot_info_t bi;
    evlog_boot_info(&bi);
    bool brownout = strcmp(bi.cause, "brownout") == 0;
    if (brownout) {                                  /* K5: brownout→reboot döngüsünü kır, içerik değişmez */
        core_quiet_for(30);
        evlog_write(EVL_WARN, "power", "brownout sonrasi acilis: TX tavani dusuk, teslimat 30 sn ertelendi");
    }
    r = core_start();
    if (r != ESP_OK) evlog_write(EVL_ERR, "sys", "core_start: %s", esp_err_to_name(r));

#ifdef LS_DEV_WIFI_SSID
    if (strcmp(cfg_get()->wifi_p.ssid, LS_DEV_WIFI_SSID) != 0) {   /* geliştirme kancası: farklıysa bir kez yaz */
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", LS_DEV_WIFI_SSID);
        cJSON_AddStringToObject(o, "pass", LS_DEV_WIFI_PASS);
        esp_err_t w = cfg_set_section_json(CFG_SEC_WIFI_P, o, NULL, 0, NULL);
        cJSON_Delete(o);
        evlog_write(EVL_WARN, "sys", "gelistirme: birincil WiFi derlemeden yazildi (%s)", esp_err_to_name(w));
    }
#endif
    r = net_wifi_start(on_link);
    if (r != ESP_OK) evlog_write(EVL_ERR, "sys", "wifi: %s", esp_err_to_name(r));
    if (brownout) net_wifi_set_tx_cap_q(32);       /* ~8 dBm; ilk kararlı bağlantıdan sonra kaldırılabilir */
    r = delivery_start();
    if (r != ESP_OK) evlog_write(EVL_ERR, "sys", "delivery: %s", esp_err_to_name(r));

    size_t nroutes;
    const http_route_t *routes = api_routes(&nroutes);
    http_sse_set_source(api_status_json);
    core_set_status_cb(http_sse_broadcast);
    r = http_start(routes, nroutes);
    if (r != ESP_OK) evlog_write(EVL_ERR, "sys", "http: %s", esp_err_to_name(r));
    log_heap("hazir");



    size_t n; const char *hash; size_t total = 0;
    const http_embedded_t *f = http_embedded_files(&n, &hash);
    for (size_t i = 0; i < n; i++) total += f[i].len;
    ESP_LOGI(TAG, "GUI: %u dosya, %u bayt gz, hash %s", (unsigned)n, (unsigned)total, hash);
}
