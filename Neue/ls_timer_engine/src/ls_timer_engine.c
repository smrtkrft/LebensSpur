// =====================================================================
// ls_timer_engine — implementation. See header.
//
// Tek task, tek queue, lock'suz. Tüm state mutasyonu task içinde olur,
// CLI handler'ları queue üzerinden gönderir. NVS yazımları senkron;
// tick frekansı 1 Hz olduğu için bottleneck değil.
//
// Time source: time(NULL) — wall clock. SKAPP `time.set` push'u olmadan
// time geçersiz kalır, tick task wall-time-ready bekler ve countdown
// ilerlemez. Bu BF webhook fix'inden miras alınmış güvenli davranış.
// =====================================================================

#include "ls_timer_engine.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "sk_cli.h"
#include "sk_errors.h"
#include "sk_event_bus.h"

static const char *TAG = "ls_timer";

#define NVS_NS                   "ls_tmr"
#define NVS_KEY_UNIT             "unit"
#define NVS_KEY_VALUE            "value"
#define NVS_KEY_ALARMS           "alarms"
#define NVS_KEY_STATE            "state"
#define NVS_KEY_DEADLINE         "deadline"
#define NVS_KEY_VAC_END          "vac_end"
#define NVS_KEY_VAC_REM          "vac_rem"
#define NVS_KEY_ALARM_MASK       "amask"

#define MIN_VALUE                1
#define MAX_VALUE                60
#define MAX_ALARMS               10
#define MAX_VACATION_DAYS        60

// time(NULL) wall-time eşiği — bu değerin altındaki epoch'lar (örn. 0,
// boot sonrası ilk birkaç sn) güvenilmez kabul edilir; SKAPP `time.set`
// push'u beklenir.
#define TIME_VALID_THRESHOLD     1700000000  // 2023-11-15 yaklaşık

#define DEFAULT_UNIT             LS_TIMER_UNIT_HOUR
#define DEFAULT_VALUE            24
#define DEFAULT_ALARMS           3

// ---------------------------------------------------------------------
// Event queue
// ---------------------------------------------------------------------

typedef enum {
    EVT_TICK,
    EVT_CMD_SET,
    EVT_CMD_START,
    EVT_CMD_STOP,
    EVT_CMD_RESET,
    EVT_CMD_VAC_SET,
    EVT_CMD_VAC_CANCEL,
} evt_type_t;

typedef enum {
    RESET_BY_MANUAL = 0,
    RESET_BY_API    = 1,
} reset_by_t;

typedef struct {
    evt_type_t type;
    int        i_arg1;     // CMD_SET: unit, CMD_VAC_SET: days
    int        i_arg2;     // CMD_SET: value
    int        i_arg3;     // CMD_SET: alarm_count (-1 = unchanged)
    reset_by_t reset_by;   // CMD_RESET only
} evt_t;

static const char *reset_by_str(reset_by_t b)
{
    return (b == RESET_BY_API) ? "api" : "manual";
}

static QueueHandle_t      s_evt_q  = NULL;
static esp_timer_handle_t s_tick_t = NULL;

// ---------------------------------------------------------------------
// Persisted + runtime state
// ---------------------------------------------------------------------

static ls_timer_config_t s_cfg = {
    .unit        = DEFAULT_UNIT,
    .value       = DEFAULT_VALUE,
    .alarm_count = DEFAULT_ALARMS,
};

static ls_timer_state_t s_state             = LS_TIMER_INACTIVE;
static int64_t          s_deadline_epoch    = 0;   // wall-clock deadline (reboot recovery + UI)
static int64_t          s_vacation_end_epoch = 0;
static uint32_t         s_vacation_remaining_sec = 0;
static uint16_t         s_alarms_fired_mask = 0;
// Uptime-based countdown — wall clock olmasa da çalışır. enter_countdown
// sırasında set edilir, tick task burayı esas alır. Wall clock geçerli ise
// s_deadline_epoch de paralel hesaplanır (UI ve NVS reboot recovery için).
static int64_t          s_deadline_uptime_us = 0;
static uint32_t         s_remaining_at_start = 0;
// Vacation süresi uptime tabanlı; wall clock devreye girince
// publish/UI için end_epoch de paralel doldurulur.
static int64_t          s_vacation_end_uptime_us = 0;

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

static uint32_t unit_to_sec(ls_timer_unit_t u)
{
    switch (u) {
    case LS_TIMER_UNIT_MINUTE: return 60u;
    case LS_TIMER_UNIT_HOUR:   return 3600u;
    case LS_TIMER_UNIT_DAY:    return 86400u;
    default:                   return 0u;
    }
}

static uint32_t total_duration_sec(const ls_timer_config_t *cfg)
{
    return unit_to_sec(cfg->unit) * (uint32_t)cfg->value;
}

static bool time_is_valid(int64_t epoch)
{
    return epoch >= TIME_VALID_THRESHOLD;
}

static int64_t now_epoch(void)
{
    return (int64_t)time(NULL);
}

const char *ls_timer_engine_state_str(void)
{
    switch (s_state) {
    case LS_TIMER_INACTIVE:  return "inactive";
    case LS_TIMER_COUNTDOWN: return "countdown";
    case LS_TIMER_VACATION:  return "vacation";
    case LS_TIMER_TRIGGERED: return "triggered";
    default:                 return "?";
    }
}

const char *ls_timer_engine_unit_str(ls_timer_unit_t unit)
{
    switch (unit) {
    case LS_TIMER_UNIT_MINUTE: return "minute";
    case LS_TIMER_UNIT_HOUR:   return "hour";
    case LS_TIMER_UNIT_DAY:    return "day";
    default:                   return "?";
    }
}

static ls_timer_unit_t unit_from_str(const char *s, bool *ok)
{
    *ok = true;
    if (!s) { *ok = false; return DEFAULT_UNIT; }
    if (strcmp(s, "minute") == 0) return LS_TIMER_UNIT_MINUTE;
    if (strcmp(s, "hour")   == 0) return LS_TIMER_UNIT_HOUR;
    if (strcmp(s, "day")    == 0) return LS_TIMER_UNIT_DAY;
    *ok = false;
    return DEFAULT_UNIT;
}

// ---------------------------------------------------------------------
// NVS load / save
// ---------------------------------------------------------------------

static esp_err_t nvs_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK; // ilk boot
    if (err != ESP_OK) return err;

    uint8_t  u8;
    uint16_t u16;
    int64_t  i64;

    if (nvs_get_u8(h, NVS_KEY_UNIT, &u8) == ESP_OK) {
        // Defansif: bilinmeyen unit değerini default'a düşür.
        if (u8 == LS_TIMER_UNIT_MINUTE || u8 == LS_TIMER_UNIT_HOUR || u8 == LS_TIMER_UNIT_DAY) {
            s_cfg.unit = (ls_timer_unit_t)u8;
        }
    }
    if (nvs_get_u16(h, NVS_KEY_VALUE, &u16) == ESP_OK) {
        if (u16 >= MIN_VALUE && u16 <= MAX_VALUE) s_cfg.value = u16;
    }
    if (nvs_get_u8(h, NVS_KEY_ALARMS, &u8) == ESP_OK) {
        if (u8 <= MAX_ALARMS) s_cfg.alarm_count = u8;
    }
    if (nvs_get_u8(h, NVS_KEY_STATE, &u8) == ESP_OK) {
        if (u8 <= LS_TIMER_TRIGGERED) s_state = (ls_timer_state_t)u8;
    }
    if (nvs_get_i64(h, NVS_KEY_DEADLINE, &i64) == ESP_OK) s_deadline_epoch = i64;
    if (nvs_get_i64(h, NVS_KEY_VAC_END, &i64) == ESP_OK)  s_vacation_end_epoch = i64;
    uint32_t u32;
    if (nvs_get_u32(h, NVS_KEY_VAC_REM, &u32) == ESP_OK)  s_vacation_remaining_sec = u32;
    if (nvs_get_u16(h, NVS_KEY_ALARM_MASK, &u16) == ESP_OK) s_alarms_fired_mask = u16;

    nvs_close(h);
    return ESP_OK;
}

static void nvs_save_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_UNIT, (uint8_t)s_cfg.unit);
    nvs_set_u16(h, NVS_KEY_VALUE, s_cfg.value);
    nvs_set_u8(h, NVS_KEY_ALARMS, s_cfg.alarm_count);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_runtime(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_STATE, (uint8_t)s_state);
    nvs_set_i64(h, NVS_KEY_DEADLINE, s_deadline_epoch);
    nvs_set_i64(h, NVS_KEY_VAC_END, s_vacation_end_epoch);
    nvs_set_u32(h, NVS_KEY_VAC_REM, s_vacation_remaining_sec);
    nvs_set_u16(h, NVS_KEY_ALARM_MASK, s_alarms_fired_mask);
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------
// Event publishers
// ---------------------------------------------------------------------

static uint32_t remaining_sec_now(void)
{
    if (s_state != LS_TIMER_COUNTDOWN) return 0u;
    // Önce uptime tabanlı hesap (wall clock'a bağımsız, garantili)
    if (s_deadline_uptime_us > 0) {
        int64_t now_us = esp_timer_get_time();
        int64_t d_us = s_deadline_uptime_us - now_us;
        if (d_us <= 0) return 0u;
        return (uint32_t)((d_us + 500000) / 1000000);
    }
    // Fallback: wall clock (reboot sonrası uptime kayboldu)
    int64_t now = now_epoch();
    if (!time_is_valid(now)) return 0u;
    int64_t d = s_deadline_epoch - now;
    if (d < 0) return 0u;
    return (uint32_t)d;
}

static void publish_state(void)
{
    uint32_t rem = remaining_sec_now();
    sk_event_bus_publishf("timer.state",
        "{\"state\":\"%s\",\"remaining_sec\":%" PRIu32 "}",
        ls_timer_engine_state_str(), rem);
}

static void publish_tick(uint32_t rem)
{
    sk_event_bus_publishf("timer.tick",
        "{\"remaining_sec\":%" PRIu32 "}", rem);
}

static void publish_alarm(int index, uint32_t rem)
{
    sk_event_bus_publishf("timer.alarm",
        "{\"index\":%d,\"of\":%u,\"remaining_sec\":%" PRIu32 "}",
        index + 1, (unsigned)s_cfg.alarm_count, rem);
}

static void publish_triggered(uint32_t duration_sec)
{
    sk_event_bus_publishf("timer.triggered",
        "{\"duration_sec\":%" PRIu32 "}", duration_sec);
}

static void publish_reset(const char *by)
{
    sk_event_bus_publishf("timer.reset", "{\"by\":\"%s\"}", by ? by : "manual");
}

static void publish_vacation(bool active)
{
    if (active) {
        sk_event_bus_publishf("timer.vacation",
            "{\"active\":true,\"end_epoch\":%" PRId64 "}", s_vacation_end_epoch);
    } else {
        sk_event_bus_publish("timer.vacation", "{\"active\":false}");
    }
}

// ---------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------

static void enter_countdown(uint32_t remaining_sec, const char *reset_by)
{
    s_state = LS_TIMER_COUNTDOWN;
    s_remaining_at_start = remaining_sec;
    // Uptime tabanlı deadline — wall clock olmasa da geri sayım çalışır.
    s_deadline_uptime_us = esp_timer_get_time() + (int64_t)remaining_sec * 1000000LL;

    int64_t now = now_epoch();
    if (time_is_valid(now)) {
        // Wall clock varsa paralel olarak epoch deadline'ı da set et —
        // UI'da takvim/saat görünmesi ve reboot recovery için.
        s_deadline_epoch = now + (int64_t)remaining_sec;
    } else {
        ESP_LOGI(TAG, "wall time not set, using uptime-based countdown (%us)",
                 (unsigned)remaining_sec);
        s_deadline_epoch = 0;
    }
    s_alarms_fired_mask = 0;
    nvs_save_runtime();
    publish_state();
    if (reset_by) publish_reset(reset_by);
}

static void enter_inactive(void)
{
    s_state = LS_TIMER_INACTIVE;
    s_deadline_epoch = 0;
    s_deadline_uptime_us = 0;
    s_remaining_at_start = 0;
    s_vacation_end_epoch = 0;
    s_vacation_end_uptime_us = 0;
    s_vacation_remaining_sec = 0;
    s_alarms_fired_mask = 0;
    nvs_save_runtime();
    publish_state();
}

static void enter_vacation(uint32_t days)
{
    int64_t now = now_epoch();
    uint32_t rem = (s_state == LS_TIMER_COUNTDOWN) ? remaining_sec_now()
                                                    : total_duration_sec(&s_cfg);
    s_vacation_remaining_sec = rem;
    // Uptime tabanlı end — wall clock olmasa da geri sayım çalışır.
    s_vacation_end_uptime_us = esp_timer_get_time() + (int64_t)days * 86400LL * 1000000LL;
    // Wall clock yalnızca geçerliyse epoch hesabı yap; aksi halde 0 bırak
    // (negatif olarak küçük bir epoch yazıp ileride yanlış expire tetiklemeyi önler).
    s_vacation_end_epoch = time_is_valid(now)
        ? (now + (int64_t)days * 86400)
        : 0;
    s_state = LS_TIMER_VACATION;
    s_deadline_epoch = 0;
    s_deadline_uptime_us = 0;
    nvs_save_runtime();
    publish_vacation(true);
    publish_state();
}

static void exit_vacation_to_countdown(void)
{
    publish_vacation(false);
    uint32_t resume_rem = s_vacation_remaining_sec;
    s_vacation_end_epoch = 0;
    s_vacation_end_uptime_us = 0;
    s_vacation_remaining_sec = 0;
    // Eğer vacation süresince hiçbir countdown kalmamışsa (rem=0), countdown
    // başlatmak anlamsız — INACTIVE'a düş.
    if (resume_rem == 0) {
        enter_inactive();
        return;
    }
    enter_countdown(resume_rem, NULL);
}

static void enter_triggered(void)
{
    uint32_t dur = total_duration_sec(&s_cfg);
    s_state = LS_TIMER_TRIGGERED;
    s_deadline_epoch = 0;
    s_deadline_uptime_us = 0;
    nvs_save_runtime();
    publish_state();
    publish_triggered(dur);
}

// ---------------------------------------------------------------------
// Tick handling
// ---------------------------------------------------------------------

static uint32_t alarm_threshold_sec(int alarm_index)
{
    // i=0 → en geç çalan (1 birim kala), i=alarm_count-1 → en erken
    // çalan (alarm_count birim kala). "remaining_sec <= threshold_i &&
    // !fired_i" tetiklenir.
    return ((uint32_t)alarm_index + 1u) * unit_to_sec(s_cfg.unit);
}

static void check_alarms(uint32_t remaining)
{
    uint8_t n = s_cfg.alarm_count;
    if (n == 0 || n > MAX_ALARMS) return;
    // Alarm i'nin eşiği (i+1) birim. Eğer (i+1) > value ise toplam süreden
    // büyük — countdown başlarken hemen tetiklenir. Bu spurious firing'i
    // önlemek için value'dan büyük indexli alarmları atlıyoruz.
    uint16_t cap = s_cfg.value;
    bool dirty = false;
    for (int i = 0; i < n; ++i) {
        if ((uint16_t)(i + 1) > cap) break;
        uint16_t bit = (uint16_t)(1u << i);
        if (s_alarms_fired_mask & bit) continue;
        uint32_t th = alarm_threshold_sec(i);
        if (th == 0) continue;
        if (remaining <= th) {
            s_alarms_fired_mask |= bit;
            publish_alarm(i, remaining);
            dirty = true;
        }
    }
    if (dirty) nvs_save_runtime();
}

static void handle_tick(void)
{
    if (s_state == LS_TIMER_COUNTDOWN) {
        // Uptime-based primary path (her zaman çalışır, wall clock'a bağımsız).
        if (s_deadline_uptime_us > 0) {
            int64_t now_us = esp_timer_get_time();
            int64_t d_us = s_deadline_uptime_us - now_us;
            if (d_us <= 0) {
                enter_triggered();
                return;
            }
            uint32_t rem_u = (uint32_t)((d_us + 500000) / 1000000);
            // Wall clock yeni geçerli olmuş olabilir — UI/NVS reboot recovery için
            // epoch deadline'ı paralel doldur.
            if (s_deadline_epoch == 0) {
                int64_t now = now_epoch();
                if (time_is_valid(now)) {
                    s_deadline_epoch = now + (int64_t)rem_u;
                    nvs_save_runtime();
                }
            }
            publish_tick(rem_u);
            check_alarms(rem_u);
            return;
        }
        // Reboot fallback: uptime yok ama wall clock var — eski deadline'dan
        // kalan süreyi çıkar, uptime tracking'i yeniden başlat.
        int64_t now = now_epoch();
        if (time_is_valid(now) && s_deadline_epoch > 0) {
            int64_t rem = s_deadline_epoch - now;
            if (rem <= 0) {
                enter_triggered();
                return;
            }
            s_deadline_uptime_us = esp_timer_get_time() + rem * 1000000LL;
            ESP_LOGI(TAG, "reboot recovery: %llds left, uptime track restarted",
                     (long long)rem);
            return;
        }
        // Hiçbir referans yok — bekle (gerçekleşmemeli, enter_countdown set ediyor).
        return;
    }

    if (s_state == LS_TIMER_VACATION) {
        // Uptime-based primary path (wall clock'a bağımsız).
        if (s_vacation_end_uptime_us > 0) {
            int64_t now_us = esp_timer_get_time();
            if (now_us >= s_vacation_end_uptime_us) {
                exit_vacation_to_countdown();
                return;
            }
            // Wall clock yeni geçerli oldu ise paralel epoch'u doldur (UI için).
            if (s_vacation_end_epoch == 0) {
                int64_t now = now_epoch();
                if (time_is_valid(now)) {
                    int64_t rem_us = s_vacation_end_uptime_us - now_us;
                    s_vacation_end_epoch = now + (rem_us + 500000) / 1000000;
                    nvs_save_runtime();
                }
            }
            return;
        }
        // Reboot fallback: uptime kayboldu, wall clock referans olarak kaldı.
        int64_t now = now_epoch();
        if (!time_is_valid(now)) return;
        if (s_vacation_end_epoch > 0 && now >= s_vacation_end_epoch) {
            exit_vacation_to_countdown();
            return;
        }
        // Uptime tracking'i yeniden başlat — sonraki tick'lerde uptime path kullanılır.
        if (s_vacation_end_epoch > 0) {
            int64_t rem = s_vacation_end_epoch - now;
            if (rem > 0) {
                s_vacation_end_uptime_us = esp_timer_get_time() + rem * 1000000LL;
            }
        }
        return;
    }

    // INACTIVE, TRIGGERED — tick'lerde iş yok.
}

// ---------------------------------------------------------------------
// Task + queue glue
// ---------------------------------------------------------------------

static void tick_cb(void *arg)
{
    // esp_timer dispatch default context = task (not ISR), so plain
    // xQueueSend is correct here. If ever switched to ESP_TIMER_ISR
    // dispatch, swap to xQueueSendFromISR.
    (void)arg;
    if (!s_evt_q) return;
    evt_t e = { .type = EVT_TICK };
    xQueueSend(s_evt_q, &e, 0);
}

static void apply_evt(const evt_t *e)
{
    switch (e->type) {
    case EVT_TICK:
        handle_tick();
        break;
    case EVT_CMD_SET:
        if (e->i_arg1 >= 0) s_cfg.unit = (ls_timer_unit_t)e->i_arg1;
        if (e->i_arg2 >= 0) s_cfg.value = (uint16_t)e->i_arg2;
        if (e->i_arg3 >= 0) s_cfg.alarm_count = (uint8_t)e->i_arg3;
        nvs_save_config();
        publish_state();
        break;
    case EVT_CMD_START: {
        // Idempotent restart: her zaman yeniden arm et. Daha önce COUNTDOWN'da
        // olsak bile config değişmiş veya kullanıcı geri sayımı baştan
        // istemiş olabilir (eski LS UI'sında "Start" butonu da bu davranış).
        uint32_t total = total_duration_sec(&s_cfg);
        if (total == 0) {
            ESP_LOGW(TAG, "start ignored: total_duration_sec=0 (cfg corrupted?)");
            break;
        }
        enter_countdown(total, NULL);
        break;
    }
    case EVT_CMD_STOP:
        enter_inactive();
        break;
    case EVT_CMD_RESET: {
        if (s_state == LS_TIMER_COUNTDOWN || s_state == LS_TIMER_TRIGGERED) {
            uint32_t total = total_duration_sec(&s_cfg);
            if (total == 0) {
                ESP_LOGW(TAG, "reset ignored: total_duration_sec=0");
                break;
            }
            enter_countdown(total, reset_by_str(e->reset_by));
        }
        break;
    }
    case EVT_CMD_VAC_SET:
        if (s_state == LS_TIMER_COUNTDOWN || s_state == LS_TIMER_INACTIVE) {
            enter_vacation((uint32_t)e->i_arg1);
        }
        break;
    case EVT_CMD_VAC_CANCEL:
        if (s_state == LS_TIMER_VACATION) {
            exit_vacation_to_countdown();
        }
        break;
    }
}

static void task_main(void *arg)
{
    (void)arg;
    evt_t e;
    for (;;) {
        if (xQueueReceive(s_evt_q, &e, portMAX_DELAY) == pdTRUE) {
            apply_evt(&e);
        }
    }
}

// ---------------------------------------------------------------------
// CLI handlers
// ---------------------------------------------------------------------

// Standart usage hint, eksik veya yanlis arg durumunda tek noktadan emit.
static void emit_set_usage(sk_cli_ctx_t *ctx)
{
    sk_cli_usage(ctx,
        "timer set <unit> <value> [alarm <N>]",
        "unit:   minute | hour | day\n"
        "value:  1..60\n"
        "alarm:  0..10 (optional, default 3)",
        "timer set hour 8\n"
        "timer set hour 24 alarm 3\n"
        "timer set minute 5 alarm 2");
}

static sk_err_t cli_set(sk_cli_ctx_t *ctx)
{
    // Pozisyonel (human) + JSON key (machine) tek satirda: sk_cli_arg_after
    // machine mode'da JSON kanali ile cevap verir, human mode'da NULL doner.
    const char *u_str = sk_cli_arg_after(ctx, "unit");
    const char *v_str = sk_cli_arg_after(ctx, "value");
    if (!u_str) u_str = sk_cli_arg(ctx, 0);
    if (!v_str) v_str = sk_cli_arg(ctx, 1);

    // Alarm: human "alarm N" keyword pair, machine "alarms":N JSON key.
    long alarms_l = -1;
    if (!sk_cli_arg_after_long(ctx, "alarm", &alarms_l)) {
        sk_cli_arg_after_long(ctx, "alarms", &alarms_l);
    }

    // Hicbir arg yok → tam usage.
    if (!u_str && !v_str && alarms_l < 0) {
        emit_set_usage(ctx);
        return SK_ERR_MISSING_ARG;
    }

    int unit_i = -1, value_i = -1, alarms_i = -1;

    if (u_str) {
        bool ok;
        ls_timer_unit_t u = unit_from_str(u_str, &ok);
        if (!ok) {
            sk_cli_usage(ctx,
                "timer set <unit> <value> [alarm <N>]",
                "unit: minute | hour | day", NULL);
            return SK_ERR_INVALID_ARG;
        }
        unit_i = (int)u;
    }
    if (v_str) {
        int v = atoi(v_str);
        if (v < MIN_VALUE || v > MAX_VALUE) {
            sk_cli_usage(ctx,
                "timer set <unit> <value> [alarm <N>]",
                "value: 1..60", NULL);
            return SK_ERR_INVALID_ARG;
        }
        value_i = v;
    }
    if (alarms_l >= 0) {
        if (alarms_l > MAX_ALARMS) {
            sk_cli_usage(ctx,
                "timer set <unit> <value> [alarm <N>]",
                "alarm: 0..10", NULL);
            return SK_ERR_INVALID_ARG;
        }
        alarms_i = (int)alarms_l;
    }

    // En az bir parametre olmali: birim verildiyse deger de zorunlu, tersi de.
    // Sadece alarm geldiyse onu kabul ediyoruz (mevcut config korunur).
    if (unit_i >= 0 && value_i < 0) {
        emit_set_usage(ctx);
        return SK_ERR_MISSING_ARG;
    }
    if (value_i >= 0 && unit_i < 0) {
        emit_set_usage(ctx);
        return SK_ERR_MISSING_ARG;
    }

    evt_t e = {
        .type   = EVT_CMD_SET,
        .i_arg1 = unit_i,
        .i_arg2 = value_i,
        .i_arg3 = alarms_i,
    };
    xQueueSend(s_evt_q, &e, portMAX_DELAY);
    sk_cli_ok(ctx, NULL);
    return SK_OK;
}

static sk_err_t cli_start(sk_cli_ctx_t *ctx)
{
    evt_t e = { .type = EVT_CMD_START };
    xQueueSend(s_evt_q, &e, portMAX_DELAY);
    sk_cli_ok(ctx, NULL);
    return SK_OK;
}

static sk_err_t cli_stop(sk_cli_ctx_t *ctx)
{
    evt_t e = { .type = EVT_CMD_STOP };
    xQueueSend(s_evt_q, &e, portMAX_DELAY);
    sk_cli_ok(ctx, NULL);
    return SK_OK;
}

static sk_err_t cli_reset(sk_cli_ctx_t *ctx)
{
    // Human kullanicilar `timer reset` yazar, by parametresi sadece SKAPP /
    // sk_api gibi machine cagiranlarin "{"by":"api"}" gondermek icin var.
    const char *by = sk_cli_arg_after(ctx, "by");
    reset_by_t  b  = RESET_BY_MANUAL;
    if (by && strcmp(by, "api") == 0) b = RESET_BY_API;
    evt_t e = { .type = EVT_CMD_RESET, .reset_by = b };
    xQueueSend(s_evt_q, &e, portMAX_DELAY);
    sk_cli_ok(ctx, NULL);
    return SK_OK;
}

// Mask icinde set bit sayisini sayar (alarms_fired count).
static unsigned alarms_fired_count(uint16_t mask)
{
    unsigned c = 0;
    while (mask) { c += (mask & 1u); mask >>= 1; }
    return c;
}

// VACATION durumunda kalan tatil gunlerini hesaplar (yuvarlama: ceil-ish, en
// az 1 gun kalmis gibi gosterir tatil hala aktifken).
static uint32_t vacation_remaining_days(void)
{
    if (s_vacation_end_uptime_us > 0) {
        int64_t d_us = s_vacation_end_uptime_us - esp_timer_get_time();
        if (d_us <= 0) return 0;
        int64_t d_sec = (d_us + 500000) / 1000000;
        return (uint32_t)((d_sec + 86399) / 86400);  // ceil
    }
    if (s_vacation_end_epoch > 0) {
        int64_t now = now_epoch();
        if (!time_is_valid(now)) return 0;
        int64_t d = s_vacation_end_epoch - now;
        if (d <= 0) return 0;
        return (uint32_t)((d + 86399) / 86400);
    }
    return 0;
}

static sk_err_t cli_status(sk_cli_ctx_t *ctx)
{
    uint32_t rem = remaining_sec_now();
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\","
        "\"remaining_sec\":%" PRIu32 ","
        "\"deadline_epoch\":%" PRId64 ","
        "\"vacation_end_epoch\":%" PRId64 ","
        "\"vacation_remaining_sec\":%" PRIu32 ","
        "\"alarms_fired_mask\":%u}",
        ls_timer_engine_state_str(),
        rem,
        s_deadline_epoch,
        s_vacation_end_epoch,
        s_vacation_remaining_sec,
        (unsigned)s_alarms_fired_mask);

    if (sk_cli_is_machine_mode(ctx)) {
        sk_cli_ok(ctx, buf);
        return SK_OK;
    }

    // Human mode, friendly rows.
    sk_cli_kv(ctx, "State", ls_timer_engine_state_str());

    if (s_state == LS_TIMER_COUNTDOWN) {
        char dur[32];
        sk_cli_fmt_duration(dur, sizeof(dur), rem);
        sk_cli_kvf(ctx, "Remaining", "%s", dur);
        unsigned fired = alarms_fired_count(s_alarms_fired_mask);
        sk_cli_kvf(ctx, "Alarms",
                   "%u configured, %u fired",
                   (unsigned)s_cfg.alarm_count, fired);
    } else if (s_state == LS_TIMER_VACATION) {
        uint32_t days = vacation_remaining_days();
        sk_cli_kvf(ctx, "Vacation ends", "in %" PRIu32 " day(s)", days);
        char dur[32];
        sk_cli_fmt_duration(dur, sizeof(dur), s_vacation_remaining_sec);
        sk_cli_kvf(ctx, "Saved time", "%s", dur);
    } else if (s_state == LS_TIMER_TRIGGERED) {
        sk_cli_kv(ctx, "Triggered", "yes");
    }
    // INACTIVE: only the State row is needed.

    return SK_OK;
}

static sk_err_t cli_get(sk_cli_ctx_t *ctx)
{
    uint32_t total = total_duration_sec(&s_cfg);
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"unit\":\"%s\",\"value\":%u,\"alarms\":%u,"
        "\"total_duration_sec\":%" PRIu32 "}",
        ls_timer_engine_unit_str(s_cfg.unit),
        (unsigned)s_cfg.value,
        (unsigned)s_cfg.alarm_count,
        total);

    if (sk_cli_is_machine_mode(ctx)) {
        sk_cli_ok(ctx, buf);
        return SK_OK;
    }

    // Human mode, friendly summary.
    char dur[32];
    sk_cli_fmt_duration(dur, sizeof(dur), total);
    sk_cli_kv (ctx, "Unit",            ls_timer_engine_unit_str(s_cfg.unit));
    sk_cli_kvf(ctx, "Value",           "%u", (unsigned)s_cfg.value);
    sk_cli_kvf(ctx, "Alarm count",     "%u", (unsigned)s_cfg.alarm_count);
    sk_cli_kvf(ctx, "Total duration",  "%s", dur);
    return SK_OK;
}

static sk_err_t cli_vac_set(sk_cli_ctx_t *ctx)
{
    // Machine (JSON {"days":N}) → sk_cli_arg_after kanali dolar.
    // Human (`vacation set 7`) → pozisyonel arg.
    const char *d_str = sk_cli_arg_after(ctx, "days");
    if (!d_str) d_str = sk_cli_arg(ctx, 0);
    if (!d_str) {
        sk_cli_usage(ctx,
            "vacation set <days>",
            "days: 1..60",
            "vacation set 7\n"
            "vacation set 30");
        return SK_ERR_MISSING_ARG;
    }
    int days = atoi(d_str);
    if (days < 1 || days > MAX_VACATION_DAYS) {
        sk_cli_usage(ctx,
            "vacation set <days>",
            "days: 1..60",
            "vacation set 7\n"
            "vacation set 30");
        return SK_ERR_INVALID_ARG;
    }
    evt_t e = { .type = EVT_CMD_VAC_SET, .i_arg1 = days };
    xQueueSend(s_evt_q, &e, portMAX_DELAY);
    sk_cli_ok(ctx, NULL);
    return SK_OK;
}

static sk_err_t cli_vac_cancel(sk_cli_ctx_t *ctx)
{
    evt_t e = { .type = EVT_CMD_VAC_CANCEL };
    xQueueSend(s_evt_q, &e, portMAX_DELAY);
    sk_cli_ok(ctx, NULL);
    return SK_OK;
}

static const sk_cli_command_t s_cmds[] = {
    { .name = "timer.set",
      .summary = "Set duration: <unit> <value> (minute|hour|day, 1..60) [alarm <0..10>]",
      .usage   = "timer set <unit> <value> [alarm <N>]",
      .help_block =
          "Set the countdown duration and (optionally) the number of\n"
          "early-warning alarms.\n"
          "\n"
          "  unit:   Time unit. One of: minute | hour | day\n"
          "  value:  Numeric duration, range 1..60\n"
          "  alarm:  Optional. Number of early-warning alarms, 0..10.\n"
          "          Alarms fire backwards from the deadline, one unit\n"
          "          apart. E.g. with unit=hour, value=24, alarm=3 the\n"
          "          alarms fire at 21h, 22h, 23h elapsed (i.e. 3h, 2h,\n"
          "          1h remaining).\n"
          "\n"
          "Notes:\n"
          "  - 'unit' and 'value' must be supplied together.\n"
          "  - Passing only 'alarm <N>' updates the alarm count and keeps\n"
          "    the existing unit/value.\n"
          "  - Changing the config does NOT auto-start the countdown.\n"
          "    Run 'timer start' afterwards to arm it.\n"
          "\n"
          "Examples:\n"
          "  timer set hour 8\n"
          "  timer set hour 24 alarm 3\n"
          "  timer set minute 5 alarm 2\n"
          "  timer set day 7",
      .handler = cli_set },
    { .name = "timer.get",
      .summary = "Show current timer configuration",
      .usage   = "timer get",
      .help_block =
          "Reports the current timer configuration (the values written by\n"
          "the most recent 'timer set').\n"
          "\n"
          "Fields returned:\n"
          "  unit             Time unit: minute | hour | day\n"
          "  value            Duration in 'unit's, 1..60\n"
          "  alarms           Number of early-warning alarms, 0..10\n"
          "  total_duration   Convenience: unit * value, in seconds\n"
          "\n"
          "This is configuration only. For the live runtime state\n"
          "(remaining time, current state machine value, fired alarms)\n"
          "use 'timer status' instead.\n"
          "\n"
          "Example:\n"
          "  timer get",
      .handler = cli_get },
    { .name = "timer.start",
      .summary = "Start the countdown from full duration",
      .usage   = "timer start",
      .help_block =
          "Arms the countdown using the currently configured unit/value\n"
          "and transitions the device into the 'countdown' state.\n"
          "\n"
          "Behaviour:\n"
          "  - Always starts from the FULL configured duration, even if a\n"
          "    countdown was already in progress (idempotent re-arm).\n"
          "  - Alarm fired-mask is cleared, so every configured alarm\n"
          "    will fire again.\n"
          "  - The countdown survives reboots: deadline is persisted to\n"
          "    NVS and recomputed against the wall clock on boot.\n"
          "\n"
          "Prerequisite: run 'timer set' first to choose unit/value, or\n"
          "rely on the defaults (24 hours, 3 alarms).\n"
          "\n"
          "Example:\n"
          "  timer set hour 24 alarm 3\n"
          "  timer start",
      .handler = cli_start },
    { .name = "timer.stop",
      .summary = "Stop the countdown (state -> inactive)",
      .usage   = "timer stop",
      .help_block =
          "Cancels any running countdown or vacation and forces the state\n"
          "back to 'inactive'.\n"
          "\n"
          "Effect:\n"
          "  - Clears the active deadline.\n"
          "  - Clears the saved vacation remaining-time.\n"
          "  - Clears the alarm fired-mask.\n"
          "  - Does NOT touch the configured unit/value/alarm count.\n"
          "\n"
          "To start counting again later, run 'timer start'. To re-arm\n"
          "from the current duration without going through 'stop' first,\n"
          "use 'timer reset'.\n"
          "\n"
          "Example:\n"
          "  timer stop",
      .handler = cli_stop },
    { .name = "timer.reset",
      .summary = "Reset countdown back to full duration",
      .usage   = "timer reset",
      .help_block =
          "Re-arms the current countdown from the full configured\n"
          "duration. Equivalent to pressing the device's reset button.\n"
          "\n"
          "Behaviour:\n"
          "  - Only effective in 'countdown' or 'triggered' state. In\n"
          "    'inactive' or 'vacation' it is a no-op.\n"
          "  - Clears the alarm fired-mask, so alarms fire again.\n"
          "  - Publishes a 'timer.reset' event on the bus. Subscribers\n"
          "    (mail groups, relay, outbound API) react accordingly.\n"
          "\n"
          "The optional internal arg 'by' is used by sk_api / SKAPP to\n"
          "distinguish a manual reset (default) from an API-triggered\n"
          "reset. Human users do not need to supply it.\n"
          "\n"
          "Examples:\n"
          "  timer reset",
      .handler = cli_reset },
    { .name = "timer.status",
      .summary = "Show current timer runtime state",
      .usage   = "timer status",
      .help_block =
          "Reports the live runtime state of the countdown.\n"
          "\n"
          "State values:\n"
          "  inactive    No countdown is armed. Use 'timer start' to arm.\n"
          "  countdown   A countdown is running towards the deadline.\n"
          "  vacation    Countdown is paused; remaining time is saved\n"
          "              and will resume when vacation ends (or is\n"
          "              cancelled with 'vacation cancel').\n"
          "  triggered   The deadline was reached and the trigger fired.\n"
          "              Stays here until 'timer reset' or 'timer start'.\n"
          "\n"
          "Additional human-mode rows (per state):\n"
          "  Remaining       only in 'countdown' (e.g. '1h 30m')\n"
          "  Alarms          'X configured, Y fired' in 'countdown'\n"
          "  Vacation ends   days left in 'vacation'\n"
          "  Saved time      countdown remainder preserved during vacation\n"
          "  Triggered       'yes' in 'triggered'\n"
          "\n"
          "For the static configuration (unit, value, alarm count) use\n"
          "'timer get' instead.\n"
          "\n"
          "Example:\n"
          "  timer status",
      .handler = cli_status },
    { .name = "vacation.set",
      .summary = "Enter vacation mode for N days (1..60), countdown paused",
      .usage   = "vacation set <days>",
      .help_block =
          "Pauses the countdown for the given number of days. During\n"
          "vacation the state machine sits in 'vacation' and the saved\n"
          "remaining-time is held untouched.\n"
          "\n"
          "  days:  Number of days to stay on vacation. Range 1..60.\n"
          "\n"
          "Behaviour:\n"
          "  - Allowed from 'countdown' and 'inactive'. Not from\n"
          "    'triggered' (reset first).\n"
          "  - If entered from 'countdown', the remaining time is saved\n"
          "    and restored after vacation ends.\n"
          "  - If entered from 'inactive', the configured full duration\n"
          "    is used as the resume value.\n"
          "  - Vacation auto-ends after 'days' days; the device then\n"
          "    transitions back to 'countdown' (or 'inactive' if there\n"
          "    was nothing to resume).\n"
          "  - To end vacation early, use 'vacation cancel'.\n"
          "\n"
          "Examples:\n"
          "  vacation set 7\n"
          "  vacation set 30",
      .handler = cli_vac_set },
    { .name = "vacation.cancel",
      .summary = "Exit vacation mode and resume countdown",
      .usage   = "vacation cancel",
      .help_block =
          "Ends vacation mode early. The countdown immediately resumes\n"
          "from the saved remaining-time captured when vacation started.\n"
          "\n"
          "Behaviour:\n"
          "  - Only effective in 'vacation' state. No-op otherwise.\n"
          "  - If the saved remaining-time is zero (rare), the device\n"
          "    falls back to 'inactive' instead of 'countdown'.\n"
          "  - Publishes 'timer.vacation' (active=false) on the event bus.\n"
          "\n"
          "Example:\n"
          "  vacation cancel",
      .handler = cli_vac_cancel },
};

// ---------------------------------------------------------------------
// Factory reset hook
// ---------------------------------------------------------------------

// On device.factory-reset.requested: wipe ls_tmr NVS namespace + reset
// all in-memory state to defaults. Pairing-completeness fix: previously
// the timer's persisted state (deadline, vacation, alarms-fired mask)
// survived factory reset and a new owner inherited the previous owner's
// countdown.
static void on_factory_reset(const sk_event_t *evt, void *user_ctx)
{
    (void)evt; (void)user_ctx;
    ESP_LOGW(TAG, "factory reset received — wiping ls_timer NVS + state");

    // 1) Reset in-memory state to defaults.
    s_cfg = (ls_timer_config_t){
        .unit        = DEFAULT_UNIT,
        .value       = DEFAULT_VALUE,
        .alarm_count = DEFAULT_ALARMS,
    };
    s_state                  = LS_TIMER_INACTIVE;
    s_deadline_epoch         = 0;
    s_vacation_end_epoch     = 0;
    s_vacation_remaining_sec = 0;
    s_alarms_fired_mask      = 0;
    s_deadline_uptime_us     = 0;
    s_remaining_at_start     = 0;
    s_vacation_end_uptime_us = 0;

    // 2) Wipe NVS namespace.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    // 3) Publish state so any UI re-syncs.
    publish_state();
}

// ---------------------------------------------------------------------
// External reset requests (e.g. ls_reset_api HTTP endpoint)
// ---------------------------------------------------------------------

// Sahiplerini sk_event_bus'a publish eder; subscriber bu fonksiyonda
// queue'ya CMD_RESET çevirir. ls_reset_api `{"by":"api"}` payload'ı ile
// publish eder.
static void on_reset_requested(const sk_event_t *evt, void *user)
{
    (void)user;
    reset_by_t b = RESET_BY_API;
    if (evt && evt->payload_json) {
        // Sade arama: "by":"manual" varsa manual kabul et.
        if (strstr(evt->payload_json, "\"manual\"")) b = RESET_BY_MANUAL;
    }
    if (!s_evt_q) return;
    evt_t e = { .type = EVT_CMD_RESET, .reset_by = b };
    xQueueSend(s_evt_q, &e, 0);
}

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

void ls_timer_engine_status(ls_timer_status_t *out)
{
    if (!out) return;
    out->state                  = s_state;
    out->remaining_sec          = remaining_sec_now();
    out->deadline_epoch         = s_deadline_epoch;
    out->vacation_end_epoch     = s_vacation_end_epoch;
    out->vacation_remaining_sec = s_vacation_remaining_sec;
    out->alarms_fired_mask      = s_alarms_fired_mask;
}

void ls_timer_engine_config(ls_timer_config_t *out)
{
    if (!out) return;
    *out = s_cfg;
}

esp_err_t ls_timer_engine_init(void)
{
    nvs_load();

    // Boot recovery: NVS'ten COUNTDOWN yüklendi ama hiçbir deadline referansı
    // yok (uptime persistable değil, wall clock soğuk boot'ta geçersiz olabilir).
    // Bu durumda state'i INACTIVE'a çek; kullanıcı yeniden `timer start` der.
    // Aksi halde kullanıcı `timer status` her zaman remaining=0 görür ve
    // start re-arm etmeden işe yaramaz görünür.
    if (s_state == LS_TIMER_COUNTDOWN) {
        int64_t now = now_epoch();
        bool wall_ok = time_is_valid(now) && s_deadline_epoch > 0 && s_deadline_epoch > now;
        if (!wall_ok) {
            ESP_LOGW(TAG, "boot: countdown state had no reconstructable deadline, resetting to inactive");
            s_state = LS_TIMER_INACTIVE;
            s_deadline_epoch = 0;
            s_alarms_fired_mask = 0;
            // NVS senkronize et — yarım kalmış state diske dönmesin.
            nvs_handle_t h;
            if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, NVS_KEY_STATE, (uint8_t)s_state);
                nvs_set_i64(h, NVS_KEY_DEADLINE, 0);
                nvs_set_u16(h, NVS_KEY_ALARM_MASK, 0);
                nvs_commit(h);
                nvs_close(h);
            }
        } else {
            // Wall clock geçerli — uptime tracking'i reboot'tan sonra
            // yeniden başlat ki tick path uptime'ı kullanabilsin.
            int64_t rem = s_deadline_epoch - now;
            s_deadline_uptime_us = esp_timer_get_time() + rem * 1000000LL;
            ESP_LOGI(TAG, "boot: countdown resumed (%llds left)", (long long)rem);
        }
    }

    // VACATION boot recovery: wall clock + epoch ile rekonstrüksiyon dene,
    // olmazsa INACTIVE'a düş. Uptime persistable değil, NVS sadece epoch
    // tutar — wall clock yoksa vacation güvenli şekilde sürdürülemez.
    if (s_state == LS_TIMER_VACATION) {
        int64_t now = now_epoch();
        bool wall_ok = time_is_valid(now) && s_vacation_end_epoch > 0;
        if (!wall_ok) {
            ESP_LOGW(TAG, "boot: vacation state had no reconstructable end, resetting to inactive");
            s_state = LS_TIMER_INACTIVE;
            s_vacation_end_epoch = 0;
            s_vacation_remaining_sec = 0;
            nvs_handle_t h;
            if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, NVS_KEY_STATE, (uint8_t)s_state);
                nvs_set_i64(h, NVS_KEY_VAC_END, 0);
                nvs_set_u32(h, NVS_KEY_VAC_REM, 0);
                nvs_commit(h);
                nvs_close(h);
            }
        } else if (now >= s_vacation_end_epoch) {
            // Vacation reboot sırasında bitmiş — countdown'a otomatik geç.
            ESP_LOGI(TAG, "boot: vacation already expired, resuming countdown");
            // exit_vacation_to_countdown init'ten önce queue/task hazır değil,
            // doğrudan state geçişini yap.
            uint32_t resume_rem = s_vacation_remaining_sec;
            s_vacation_end_epoch = 0;
            s_vacation_remaining_sec = 0;
            if (resume_rem > 0) {
                s_state = LS_TIMER_COUNTDOWN;
                s_deadline_epoch = now + (int64_t)resume_rem;
                s_deadline_uptime_us = esp_timer_get_time() + (int64_t)resume_rem * 1000000LL;
                s_alarms_fired_mask = 0;
            } else {
                s_state = LS_TIMER_INACTIVE;
            }
            // NVS senkronize.
            nvs_handle_t h;
            if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, NVS_KEY_STATE, (uint8_t)s_state);
                nvs_set_i64(h, NVS_KEY_VAC_END, 0);
                nvs_set_u32(h, NVS_KEY_VAC_REM, 0);
                nvs_set_i64(h, NVS_KEY_DEADLINE, s_deadline_epoch);
                nvs_set_u16(h, NVS_KEY_ALARM_MASK, 0);
                nvs_commit(h);
                nvs_close(h);
            }
        } else {
            // Vacation devam ediyor — uptime tracking'i yeniden başlat.
            int64_t rem = s_vacation_end_epoch - now;
            s_vacation_end_uptime_us = esp_timer_get_time() + rem * 1000000LL;
            ESP_LOGI(TAG, "boot: vacation resumed (%llds left)", (long long)rem);
        }
    }

    s_evt_q = xQueueCreate(16, sizeof(evt_t));
    if (!s_evt_q) return ESP_ERR_NO_MEM;

    BaseType_t ok = xTaskCreate(task_main, "ls_timer", 4096, NULL, 5, NULL);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;

    const esp_timer_create_args_t targs = {
        .callback = tick_cb,
        .name     = "ls_timer_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_tick_t));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick_t, 1000 * 1000));  // 1 Hz

    for (size_t i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); ++i) {
        esp_err_t err = sk_cli_register(&s_cmds[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "cli register failed for %s: %s",
                     s_cmds[i].name, esp_err_to_name(err));
        }
    }

    // External reset request channel: ls_reset_api → timer.reset.requested
    int sub;
    sk_event_bus_subscribe("timer.reset.requested",
                           on_reset_requested, NULL, &sub);

    // Factory reset hook — wipe NVS + reset RAM defaults.
    sk_event_bus_subscribe("device.factory-reset.requested",
                           on_factory_reset, NULL, &sub);

    ESP_LOGI(TAG, "init: state=%s unit=%s value=%u alarms=%u",
             ls_timer_engine_state_str(),
             ls_timer_engine_unit_str(s_cfg.unit),
             (unsigned)s_cfg.value, (unsigned)s_cfg.alarm_count);

    return ESP_OK;
}
