/**
 * Time Manager - NTP Senkronizasyonu
 *
 * Zürich POSIX timezone: CET-1CEST,M3.5.0,M10.5.0/3
 * CET = UTC+1 (kış), CEST = UTC+2 (yaz: Mart son Pazar - Ekim son Pazar)
 */

#include "time_manager.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <sys/time.h>

static const char *TAG = "TIME";

#define ZURICH_TZ       "CET-1CEST,M3.5.0,M10.5.0/3"
#define NTP_SERVER_1    "pool.ntp.org"
#define NTP_SERVER_2    "time.google.com"
#define NTP_SERVER_3    "time.cloudflare.com"

static bool s_synced = false;

// NTP sync callback
static void ntp_sync_cb(struct timeval *tv)
{
    s_synced = true;

    char buf[32];
    struct tm ti;
    localtime_r(&tv->tv_sec, &ti);
    strftime(buf, sizeof(buf), TIME_FORMAT_LOG, &ti);

    ESP_LOGI(TAG, "NTP senkronize: %s (Zürich)", buf);
}

esp_err_t time_manager_init(void)
{
    setenv("TZ", ZURICH_TZ, 1);
    tzset();

    ESP_LOGI(TAG, "OK - Europe/Zurich (CET/CEST)");
    return ESP_OK;
}

esp_err_t time_manager_sync(void)
{
    if (esp_sntp_enabled()) {
        ESP_LOGW(TAG, "SNTP zaten çalışıyor");
        return ESP_OK;
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER_1);
    esp_sntp_setservername(1, NTP_SERVER_2);
    esp_sntp_setservername(2, NTP_SERVER_3);
    sntp_set_time_sync_notification_cb(ntp_sync_cb);

    esp_sntp_init();

    ESP_LOGI(TAG, "NTP başlatıldı: %s, %s, %s", NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    return ESP_OK;
}

void time_manager_stop(void)
{
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
        ESP_LOGI(TAG, "NTP durduruldu");
    }
}

bool time_manager_is_synced(void)
{
    return s_synced;
}

esp_err_t time_manager_get_time(struct tm *timeinfo)
{
    if (!timeinfo) return ESP_ERR_INVALID_ARG;

    time_t now;
    time(&now);
    localtime_r(&now, timeinfo);

    // 2020 öncesi tarih = henüz senkronize olmamış
    if (timeinfo->tm_year < (2020 - 1900)) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

void time_manager_get_time_str(char *buffer, size_t len, const char *format)
{
    if (!buffer || len == 0) return;

    struct tm ti;
    if (time_manager_get_time(&ti) == ESP_OK && s_synced) {
        strftime(buffer, len, format ? format : TIME_FORMAT_LOG, &ti);
    } else {
        strncpy(buffer, "---", len - 1);
        buffer[len - 1] = '\0';
    }
}

int64_t time_manager_get_uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
}

uint32_t time_manager_get_uptime_sec(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

void time_manager_get_elapsed_str(int64_t timestamp_ms, char *buffer, size_t len)
{
    if (!buffer || len == 0) return;

    int64_t elapsed = time_manager_get_uptime_ms() - timestamp_ms;
    if (elapsed < 0) {
        snprintf(buffer, len, "simdi");
        return;
    }

    int64_t sec = elapsed / 1000;
    int64_t min = sec / 60;
    int64_t hour = min / 60;
    int64_t day = hour / 24;

    if (day > 0) {
        snprintf(buffer, len, "%lld gun %lld saat once", day, hour % 24);
    } else if (hour > 0) {
        snprintf(buffer, len, "%lld saat %lld dakika once", hour, min % 60);
    } else if (min > 0) {
        snprintf(buffer, len, "%lld dakika once", min);
    } else {
        snprintf(buffer, len, "az once");
    }
}

void time_manager_get_log_time_str(char *buffer, size_t len)
{
    if (!buffer || len == 0) return;

    if (s_synced) {
        time_manager_get_time_str(buffer, len, TIME_FORMAT_LOG);
        return;
    }

    // NTP yok - uptime göster
    int64_t up_sec = esp_timer_get_time() / 1000000;
    int64_t d = up_sec / 86400;
    int64_t h = (up_sec % 86400) / 3600;
    int64_t m = (up_sec % 3600) / 60;

    if (d > 0) {
        snprintf(buffer, len, "^%lldg %llds %llddk", d, h, m);
    } else if (h > 0) {
        snprintf(buffer, len, "^%llds %llddk", h, m);
    } else {
        snprintf(buffer, len, "^%llddk %lldsn", m, up_sec % 60);
    }
}

void time_manager_print_info(void)
{
    uint32_t up = time_manager_get_uptime_sec();
    uint32_t d = up / 86400;
    uint32_t h = (up % 86400) / 3600;
    uint32_t m = (up % 3600) / 60;
    uint32_t s = up % 60;

    char uptime_str[48];
    if (d > 0) {
        snprintf(uptime_str, sizeof(uptime_str), "%lu gun %lu:%02lu:%02lu", d, h, m, s);
    } else {
        snprintf(uptime_str, sizeof(uptime_str), "%lu:%02lu:%02lu", h, m, s);
    }

    ESP_LOGI(TAG, "┌──────────────────────────────────────");
    ESP_LOGI(TAG, "│ NTP:       %s", s_synced ? "Senkronize" : "Bekleniyor...");

    if (s_synced) {
        char time_str[TIME_STR_MAX_LEN];
        time_manager_get_time_str(time_str, sizeof(time_str), TIME_FORMAT_LOG);
        ESP_LOGI(TAG, "│ Zaman:     %s", time_str);
        ESP_LOGI(TAG, "│ Timezone:  Europe/Zurich");
    }

    ESP_LOGI(TAG, "│ Uptime:    %s", uptime_str);
    ESP_LOGI(TAG, "└──────────────────────────────────────");
}
