/*
 * evlog — RTC kaydı, açılış sınıflandırması, bloklamayan satır halkası, sys_restart.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "evlog.h"
#include "evlog_priv.h"

static const char *TAG = "evlog";

#define RTC_MAGIC     0x4C53324Eu      /* "LS2N" (ALTE kaydından farklı: eski kayıt geçersiz sayılır) */
#define RING_LINES    32
#define EVLOG_LINE_MAX      320
#define EPOCH_VALID   1700000000u

typedef struct {
    uint32_t magic;
    uint32_t boot_count;
    uint32_t uptime_s;
    uint32_t boot_epoch;
    uint32_t short_boots;
    uint32_t last_mark;
    uint32_t last_mark_up_s;
    int32_t  last_mark_arg;
    uint32_t heap_free;
    uint32_t heap_min;
    uint32_t restart_reason;
    uint8_t  wifi_mode, ap_clients;
    int8_t   sta_rssi, tx_dbm;
    uint32_t check;
} rtc_rec_t;

static RTC_NOINIT_ATTR rtc_rec_t s_rtc;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static char s_ring[RING_LINES][EVLOG_LINE_MAX];
static uint8_t s_head, s_count;
static uint32_t s_dropped;
static evlog_flush_hook_t s_flush_hook;
static evlog_boot_info_t s_boot;
static esp_timer_handle_t s_tick;
static evlog_restart_hook_t s_restart_hook;
void evlog_set_restart_hook(evlog_restart_hook_t h) { s_restart_hook = h; }

/* ---------- RTC ---------- */

static uint32_t fnv(const void *p, size_t n)
{
    uint32_t h = 0x811C9DC5u;
    for (const uint8_t *b = p; n--; b++) h = (h ^ *b) * 0x01000193u;
    return h;
}
static void rtc_commit(void) { s_rtc.check = fnv(&s_rtc, offsetof(rtc_rec_t, check)); }
static bool rtc_valid(void)
{
    return s_rtc.magic == RTC_MAGIC && s_rtc.check == fnv(&s_rtc, offsetof(rtc_rec_t, check));
}

static evlog_rr_t map_rr(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return EVR_POWERON;
    case ESP_RST_EXT: return EVR_EXT;
    case ESP_RST_SW: return EVR_SW;
    case ESP_RST_PANIC: return EVR_PANIC;
    case ESP_RST_INT_WDT: return EVR_INT_WDT;
    case ESP_RST_TASK_WDT: return EVR_TASK_WDT;
    case ESP_RST_WDT: return EVR_WDT;
    case ESP_RST_DEEPSLEEP: return EVR_DEEPSLEEP;
    case ESP_RST_BROWNOUT: return EVR_BROWNOUT;
    case ESP_RST_SDIO: return EVR_SDIO;
    case ESP_RST_USB: return EVR_USB;
    case ESP_RST_JTAG: return EVR_JTAG;
    case ESP_RST_EFUSE: return EVR_EFUSE;
    case ESP_RST_PWR_GLITCH: return EVR_PWR_GLITCH;
    case ESP_RST_CPU_LOCKUP: return EVR_CPU_LOCKUP;
    default: return EVR_UNKNOWN;
    }
}

static const char *wifi_mode_name(uint8_t m)
{
    static const char *const n[] = { "OFF", "STA", "AP", "APSTA" };
    return m < 4 ? n[m] : "?";
}

/* ---------- halka ---------- */

static void ring_push(const char *line)
{
    portENTER_CRITICAL(&s_mux);
    if (s_count == RING_LINES) {
        /* dolu: dosyaya boşaltılamıyor (/log yok ya da yavaş). En yeni olaylar daha değerli:
         * en eskinin üzerine yaz (açılış kaydı en eski olsa da RTC'de ve /api/device'ta özeti var). */
        s_head = (uint8_t)((s_head + 1) % RING_LINES);
        s_count--;
        s_dropped++;
    }
    {
        uint8_t idx = (uint8_t)((s_head + s_count) % RING_LINES);
        strlcpy(s_ring[idx], line, EVLOG_LINE_MAX);
        s_count++;
    }
    portEXIT_CRITICAL(&s_mux);
    if (s_flush_hook) s_flush_hook();
}

size_t evlog_pop_line(char *out, size_t cap)
{
    size_t n = 0;
    portENTER_CRITICAL(&s_mux);
    if (s_count) {
        n = strlcpy(out, s_ring[s_head], cap);
        s_head = (uint8_t)((s_head + 1) % RING_LINES);
        s_count--;
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

void evlog_set_flush_hook(evlog_flush_hook_t hook)
{
    s_flush_hook = hook;
    if (hook && s_count) hook();
}

uint32_t evlog_dropped(void) { return s_dropped; }

/* ---------- yazma ---------- */

static uint32_t now_epoch(void)
{
    time_t t = time(NULL);
    return t > (time_t)EPOCH_VALID ? (uint32_t)t : 0u;
}

uint32_t evlog_uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

static size_t json_escape(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const unsigned char *c = (const unsigned char *)src; *c && o + 7 < cap; c++) {
        if (*c == '"' || *c == '\\') { dst[o++] = '\\'; dst[o++] = (char)*c; }
        else if (*c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if (*c < 0x20) { o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", *c); }
        else dst[o++] = (char)*c;
    }
    dst[o] = 0;
    return o;
}

void evlog_write(evlog_level_t lvl, const char *cat, const char *fmt, ...)
{
    static const char *const lv[] = { "INFO", "WARN", "ERR" };
    char msg[160], esc[200], line[EVLOG_LINE_MAX];   /* ~680 B: esp_timer görevinde (3,5 KB) güvenli */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    json_escape(esc, sizeof esc, msg);
    snprintf(line, sizeof line,
             "{\"type\":\"event\",\"n\":%lu,\"ts\":%lu,\"up\":%lu,\"lvl\":\"%s\",\"cat\":\"%s\",\"msg\":\"%s\"}",
             (unsigned long)s_rtc.boot_count, (unsigned long)now_epoch(), (unsigned long)evlog_uptime_s(),
             lv[lvl <= EVL_ERR ? lvl : EVL_ERR], cat, esc);
    if (lvl == EVL_ERR) ESP_LOGE(TAG, "[%s] %s", cat, msg);
    else if (lvl == EVL_WARN) ESP_LOGW(TAG, "[%s] %s", cat, msg);
    else ESP_LOGI(TAG, "[%s] %s", cat, msg);
    ring_push(line);
}

void evlog_mark(evlog_mark_t ev, int32_t arg)
{
    portENTER_CRITICAL(&s_mux);
    s_rtc.last_mark = ev;
    s_rtc.last_mark_up_s = evlog_uptime_s();
    s_rtc.last_mark_arg = arg;
    rtc_commit();
    portEXIT_CRITICAL(&s_mux);
}

void evlog_set_wifi_ctx(uint8_t mode, uint8_t clients, int8_t rssi, int8_t tx_dbm)
{
    portENTER_CRITICAL(&s_mux);
    s_rtc.wifi_mode = mode;
    s_rtc.ap_clients = clients;
    s_rtc.sta_rssi = rssi;
    s_rtc.tx_dbm = tx_dbm;
    rtc_commit();
    portEXIT_CRITICAL(&s_mux);
}

void evlog_time_synced(void)
{
    uint32_t e = now_epoch();
    if (!e) return;
    portENTER_CRITICAL(&s_mux);
    s_rtc.boot_epoch = e - evlog_uptime_s();
    rtc_commit();
    portEXIT_CRITICAL(&s_mux);
}

void sys_restart(restart_reason_t r)
{
    portENTER_CRITICAL(&s_mux);
    s_rtc.restart_reason = r;
    rtc_commit();
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "yeniden baslatma (neden %d)", (int)r);
    if (s_restart_hook) s_restart_hook();  /* halkadaki satırlar dosyaya */
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}

/* 1 Hz: yalnız RAM/RTC güncellemesi (I/O yok) */
static void tick_cb(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_mux);
    s_rtc.uptime_s = evlog_uptime_s();
    s_rtc.heap_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    s_rtc.heap_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    rtc_commit();
    portEXIT_CRITICAL(&s_mux);
}

/* ---------- açılış ---------- */

static bool s_boot_deferred;
static evlog_cause_t s_cause;
static rtc_rec_t s_prev;

static void emit_boot(const rtc_rec_t *prev, evlog_cause_t c, bool ok, bool approx)
{
    uint32_t end_epoch = prev->boot_epoch ? prev->boot_epoch + prev->uptime_s : 0;
    char line[EVLOG_LINE_MAX];
    snprintf(line, sizeof line,
             "{\"type\":\"boot\",\"n\":%lu,\"ts\":%lu,\"up\":%lu,\"approx\":%s,\"cat\":\"%s\",\"cause\":\"%s\","
             "\"hint\":\"%s\",\"ev\":\"%s\",\"ev_dt\":%ld,\"ev_arg\":%ld,\"wifi\":\"%s\",\"cl\":%u,\"rssi\":%d,"
             "\"tx\":%d,\"heap_free\":%lu,\"heap_min\":%lu,\"short\":%lu}",
             (unsigned long)prev->boot_count, (unsigned long)end_epoch, (unsigned long)prev->uptime_s,
             approx ? "true" : "false", c.cat, c.cause, c.hint,
             ok ? evlog_mark_name((evlog_mark_t)prev->last_mark) : "NONE",
             ok ? (long)(prev->uptime_s - prev->last_mark_up_s) : -1L, ok ? (long)prev->last_mark_arg : 0L,
             ok ? wifi_mode_name(prev->wifi_mode) : "?", ok ? prev->ap_clients : 0, ok ? prev->sta_rssi : 0,
             ok ? prev->tx_dbm : 0, (unsigned long)prev->heap_free, (unsigned long)prev->heap_min,
             (unsigned long)s_rtc.short_boots);
    ring_push(line);
}

void evlog_init(void)
{
    bool ok = rtc_valid();
    rtc_rec_t prev;
    if (ok) prev = s_rtc; else memset(&prev, 0, sizeof prev);

    evlog_cause_t c = evlog_classify(map_rr(esp_reset_reason()), ok, (restart_reason_t)prev.restart_reason,
                                     (evlog_mark_t)prev.last_mark, prev.uptime_s - prev.last_mark_up_s);
    uint32_t short_boots = ok ? prev.short_boots : 0;

    memset(&s_rtc, 0, sizeof s_rtc);
    s_rtc.magic = RTC_MAGIC;
    s_rtc.boot_count = prev.boot_count + 1;
    s_rtc.last_mark = EVM_BOOT;
    rtc_commit();

    bool short_run = prev.boot_count > 0 && prev.uptime_s < EVLOG_MIN_RUN_S;
    s_rtc.short_boots = short_run ? short_boots + 1 : 0;
    rtc_commit();

    s_boot = (evlog_boot_info_t){ .boot_count = s_rtc.boot_count, .short_boots = s_rtc.short_boots,
                                  .prev_uptime_s = prev.uptime_s, .cat = c.cat, .cause = c.cause,
                                  .hint = c.hint, .rtc_ok = ok };
    ESP_LOGI(TAG, "acilis #%lu | onceki #%lu %lu sn (%s) | neden %s/%s",
             (unsigned long)s_rtc.boot_count, (unsigned long)prev.boot_count, (unsigned long)prev.uptime_s,
             ok ? "kesin" : "yok", c.cat, c.cause);

    const esp_timer_create_args_t ta = { .callback = tick_cb, .name = "evlog" };
    if (esp_timer_create(&ta, &s_tick) == ESP_OK) esp_timer_start_periodic(s_tick, 1000000);

    s_cause = c;
    s_prev = prev;
    if (!ok) {                    /* RTC silinmiş: tam güç kaybı ya da ilk açılış; kontrol noktası bekler */
        s_boot_deferred = true;
        return;
    }
    if (short_run && !evlog_short_boot_should_log(c.cat, s_rtc.short_boots)) return;
    emit_boot(&prev, c, true, false);
}

void evlog_apply_checkpoint(bool have, uint32_t ck_boot, uint32_t ck_up, uint32_t ck_epoch, uint32_t ck_heap_min)
{
    if (!s_boot_deferred) return;
    s_boot_deferred = false;
    if (!have || ck_boot == 0) return;                 /* gerçekten ilk açılış */
    s_prev.boot_count = ck_boot;
    s_prev.uptime_s = ck_up;
    s_prev.boot_epoch = ck_epoch;
    s_prev.heap_min = ck_heap_min;
    portENTER_CRITICAL(&s_mux);
    s_rtc.boot_count = ck_boot + 1;
    rtc_commit();
    portEXIT_CRITICAL(&s_mux);
    s_boot.boot_count = s_rtc.boot_count;
    s_boot.prev_uptime_s = ck_up;
    emit_boot(&s_prev, s_cause, false, true);
}

void evlog_checkpoint_values(uint32_t *boot, uint32_t *up, uint32_t *epoch, uint32_t *heap_min)
{
    portENTER_CRITICAL(&s_mux);
    *boot = s_rtc.boot_count;
    *up = s_rtc.uptime_s;
    *epoch = s_rtc.boot_epoch;
    *heap_min = s_rtc.heap_min;
    portEXIT_CRITICAL(&s_mux);
}

void evlog_boot_info(evlog_boot_info_t *out) { *out = s_boot; }
