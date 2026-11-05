/*
 * evlog — kalıcı olay günlüğü (04-plan-son.md §2.2).
 *
 * - RTC kaydı: açılış sayısı, çalışma süresi, son olay, heap, WiFi bağlamı, yeniden başlatma nedeni.
 *   Brownout/panik/WDT/yazılım resetinde korunur; tam güç kaybında silinir.
 * - evlog_write() hiç bloklamaz: satır RAM halkasına girer, boşaltma kancası (sys_worker) dosyaya yazar.
 *   Her görevden ve esp_timer callback'inden çağrılabilir (ISR'den değil).
 * Satır biçimi ALTE ile aynı JSONL: {"type":"event","n","ts","up","lvl","cat","msg"} ve
 * {"type":"boot",...,"cat","cause","hint","ev","ev_dt",...}.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "evlog_classify.h"

typedef enum { EVL_INFO, EVL_WARN, EVL_ERR } evlog_level_t;

void evlog_init(void);
void evlog_write(evlog_level_t lvl, const char *cat, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void evlog_mark(evlog_mark_t ev, int32_t arg);              /* RTC "son olay" (ucuz, kritik bölge) */
void evlog_set_wifi_ctx(uint8_t mode, uint8_t clients, int8_t rssi, int8_t tx_dbm);
void evlog_time_synced(void);                               /* SNTP sonrası: açılış epoch'u */
void sys_restart(restart_reason_t r) __attribute__((noreturn));

typedef struct {
    uint32_t boot_count, short_boots, prev_uptime_s;
    const char *cat, *cause, *hint;
    bool rtc_ok;
} evlog_boot_info_t;
void evlog_boot_info(evlog_boot_info_t *out);
uint32_t evlog_uptime_s(void);
uint32_t evlog_dropped(void);                               /* halka dolduğu için düşen satır */

/* Boşaltma: halkada satır varken çağrılır (sys_worker kurar). */
typedef void (*evlog_flush_hook_t)(void);
void   evlog_set_flush_hook(evlog_flush_hook_t hook);
/* Halkadan en eski satırı alır (NUL sonlu); yoksa 0. Yalnız boşaltıcı görev çağırır. */
size_t evlog_pop_line(char *out, size_t cap);
