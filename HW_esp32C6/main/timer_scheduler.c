/**
 * Timer Scheduler - Dead Man's Switch Mantigi
 *
 * FreeRTOS software timer ile 1 saniyelik tick.
 * Config'den ayarlari okur, deadline hesaplar.
 * Relay kontrolu relay_manager uzerinden (Katman 0).
 * GPIO/buton isleri button_manager uzerinden (Katman 0).
 */

#include "timer_scheduler.h"
#include "config_manager.h"
#include "relay_manager.h"
#include "mail_sender.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "TIMER";

// FreeRTOS timer
static TimerHandle_t s_tick_timer = NULL;

// Durum
static timer_state_t s_state = TIMER_STATE_DISABLED;
static timer_config_t s_config;
static timer_runtime_t s_runtime;

// Istatistikler
static uint32_t s_warning_count = 0;

// Callback'ler
static timer_warning_cb_t s_warning_cb = NULL;
static timer_trigger_cb_t s_trigger_cb = NULL;
static timer_reset_cb_t s_reset_cb = NULL;

// ============================================================================
// Zaman yardimcilari
// ============================================================================

static int64_t get_current_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void get_current_hm(int *hour, int *minute)
{
    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);
    *hour = ti.tm_hour;
    *minute = ti.tm_min;
}

static bool parse_time(const char *str, int *h, int *m)
{
    if (!str || strlen(str) < 5) return false;
    return sscanf(str, "%d:%d", h, m) == 2;
}

bool timer_is_in_active_hours(void)
{
    int sh = 8, sm = 0, eh = 22, em = 0;
    parse_time(s_config.check_start, &sh, &sm);
    parse_time(s_config.check_end, &eh, &em);

    int now_h, now_m;
    get_current_hm(&now_h, &now_m);

    int now_t = now_h * 60 + now_m;
    int start_t = sh * 60 + sm;
    int end_t = eh * 60 + em;

    // Gece yarisini gecen durum (ornek: 22:00 - 06:00)
    if (end_t < start_t) {
        return (now_t >= start_t || now_t < end_t);
    }
    return (now_t >= start_t && now_t < end_t);
}

// ============================================================================
// Relay aksiyon (relay_manager kullanir)
// ============================================================================

static void execute_relay_action(const char *action)
{
    if (!action || action[0] == '\0' || strcmp(action, "none") == 0) {
        return;
    }

    if (strcmp(action, "on") == 0) {
        relay_on();
    } else if (strcmp(action, "off") == 0) {
        relay_off();
    } else if (strcmp(action, "pulse") == 0) {
        relay_trigger();  // Config'deki pulse ayarlarina gore calisir
    }
}

// ============================================================================
// Durum guncelleme
// ============================================================================

static void update_state(void)
{
    if (!s_config.enabled) {
        s_state = TIMER_STATE_DISABLED;
        return;
    }

    int64_t now = get_current_ms();
    int64_t deadline = s_runtime.next_deadline;
    int64_t warning_ms = (int64_t)s_config.warning_minutes * 60 * 1000;
    int64_t warning_time = deadline - warning_ms;

    if (s_runtime.triggered) {
        s_state = TIMER_STATE_TRIGGERED;

    } else if (now >= deadline) {
        // Deadline gecti - tetikle
        s_state = TIMER_STATE_TRIGGERED;
        s_runtime.triggered = true;
        s_runtime.trigger_count++;

        ESP_LOGW(TAG, "ALARM! Deadline gecti.");

        // Relay aksiyonu
        execute_relay_action(s_config.relay_action);

        // Alarm maili
        mail_send_to_all_groups(MAIL_TYPE_ALARM);

        if (s_trigger_cb) {
            s_trigger_cb();
        }

        // Runtime kaydet
        config_save_runtime(&s_runtime);

    } else if (now >= warning_time) {
        // Uyari suresine girildi
        if (s_state != TIMER_STATE_WARNING) {
            s_state = TIMER_STATE_WARNING;
            s_warning_count++;

            uint32_t remaining_min = (uint32_t)((deadline - now) / (60 * 1000));
            ESP_LOGW(TAG, "Uyari! %lu dakika kaldi.", remaining_min);

            // Uyari maili
            mail_send_to_all_groups(MAIL_TYPE_WARNING);

            if (s_warning_cb) {
                s_warning_cb(remaining_min);
            }
        }
    } else {
        s_state = TIMER_STATE_ACTIVE;
    }
}

// ============================================================================
// Timer tick (1 saniye)
// ============================================================================

static void tick_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    timer_tick();
}

void timer_tick(void)
{
    if (s_state == TIMER_STATE_DISABLED || s_state == TIMER_STATE_PAUSED) {
        return;
    }

    // Sadece aktif saatlerde deadline kontrol et
    if (timer_is_in_active_hours()) {
        update_state();
    }
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t timer_scheduler_init(void)
{
    // Config yukle
    if (config_load_timer(&s_config) != ESP_OK) {
        ESP_LOGW(TAG, "Timer config yuklenemedi, varsayilanlar");
        timer_config_t def = TIMER_CONFIG_DEFAULT();
        memcpy(&s_config, &def, sizeof(s_config));
    }

    // Runtime yukle
    if (config_load_runtime(&s_runtime) != ESP_OK) {
        ESP_LOGW(TAG, "Runtime yuklenemedi, sifirlaniyor");
        timer_runtime_t def = TIMER_RUNTIME_DEFAULT();
        memcpy(&s_runtime, &def, sizeof(s_runtime));

        if (s_config.enabled) {
            s_runtime.next_deadline = get_current_ms() +
                ((int64_t)s_config.interval_hours * 3600 * 1000);
        }
    }

    // Ilk durum
    s_state = s_config.enabled ? TIMER_STATE_ACTIVE : TIMER_STATE_DISABLED;

    // FreeRTOS timer (1 saniye)
    s_tick_timer = xTimerCreate("tmr_tick", pdMS_TO_TICKS(1000), pdTRUE,
                                NULL, tick_callback);
    if (!s_tick_timer) {
        ESP_LOGE(TAG, "Timer olusturulamadi");
        return ESP_FAIL;
    }

    if (xTimerStart(s_tick_timer, 0) != pdPASS) {
        ESP_LOGE(TAG, "Timer baslatilamadi");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OK - %s, %lu saat, uyari %lu dk, %s-%s",
             s_config.enabled ? "AKTIF" : "PASIF",
             s_config.interval_hours,
             s_config.warning_minutes,
             s_config.check_start,
             s_config.check_end);

    return ESP_OK;
}

esp_err_t timer_scheduler_deinit(void)
{
    if (s_tick_timer) {
        xTimerStop(s_tick_timer, 0);
        xTimerDelete(s_tick_timer, 0);
        s_tick_timer = NULL;
    }
    s_state = TIMER_STATE_DISABLED;
    ESP_LOGI(TAG, "Timer durduruldu");
    return ESP_OK;
}

esp_err_t timer_reset(void)
{
    ESP_LOGI(TAG, "Timer sifirlaniyor...");

    int64_t now = get_current_ms();
    s_runtime.next_deadline = now + ((int64_t)s_config.interval_hours * 3600 * 1000);
    s_runtime.last_reset = now;
    s_runtime.triggered = false;
    s_runtime.reset_count++;

    if (s_config.enabled) {
        s_state = TIMER_STATE_ACTIVE;
    }

    // Releyi kapat (tetiklenmisse)
    relay_off();

    // Runtime kaydet
    config_save_runtime(&s_runtime);

    if (s_reset_cb) {
        s_reset_cb();
    }

    ESP_LOGI(TAG, "Sifirlandi. Sonraki: +%lu saat", s_config.interval_hours);
    return ESP_OK;
}

esp_err_t timer_set_enabled(bool enabled)
{
    s_config.enabled = enabled;

    if (enabled) {
        timer_reset();
    } else {
        s_state = TIMER_STATE_DISABLED;
        relay_off();
    }

    return config_save_timer(&s_config);
}

esp_err_t timer_pause(void)
{
    if (s_state == TIMER_STATE_DISABLED) {
        return ESP_ERR_INVALID_STATE;
    }
    s_state = TIMER_STATE_PAUSED;
    ESP_LOGI(TAG, "Timer duraklatildi");
    return ESP_OK;
}

esp_err_t timer_resume(void)
{
    if (s_state != TIMER_STATE_PAUSED) {
        return ESP_ERR_INVALID_STATE;
    }
    s_state = TIMER_STATE_ACTIVE;
    ESP_LOGI(TAG, "Timer devam ediyor");
    return ESP_OK;
}

esp_err_t timer_get_status(timer_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;

    int64_t now = get_current_ms();
    int64_t deadline = s_runtime.next_deadline;

    status->state = s_state;
    status->remaining_seconds = (deadline > now) ? (uint32_t)((deadline - now) / 1000) : 0;
    status->warning_seconds = s_config.warning_minutes * 60;
    status->last_reset_time = s_runtime.last_reset;
    status->next_deadline = deadline;
    status->reset_count = s_runtime.reset_count;
    status->warning_count = s_warning_count;
    status->trigger_count = s_runtime.trigger_count;
    status->in_active_hours = timer_is_in_active_hours();

    return ESP_OK;
}

// ============================================================================
// Callback'ler
// ============================================================================

void timer_set_warning_callback(timer_warning_cb_t cb) { s_warning_cb = cb; }
void timer_set_trigger_callback(timer_trigger_cb_t cb) { s_trigger_cb = cb; }
void timer_set_reset_callback(timer_reset_cb_t cb)     { s_reset_cb = cb; }

// ============================================================================
// Debug
// ============================================================================

void timer_print_stats(void)
{
    timer_status_t st;
    timer_get_status(&st);

    const char *names[] = {"PASIF", "AKTIF", "UYARI", "TETIKLENDI", "DURAKLATILDI"};

    ESP_LOGI(TAG, "┌──────────────────────────────────────");
    ESP_LOGI(TAG, "│ Durum:     %s", names[st.state]);
    ESP_LOGI(TAG, "│ Kalan:     %lu sn", st.remaining_seconds);
    ESP_LOGI(TAG, "│ Aktif:     %s", st.in_active_hours ? "EVET" : "HAYIR");
    ESP_LOGI(TAG, "│ Reset:     %lu kez", st.reset_count);
    ESP_LOGI(TAG, "│ Uyari:     %lu kez", st.warning_count);
    ESP_LOGI(TAG, "│ Tetik:     %lu kez", st.trigger_count);
    ESP_LOGI(TAG, "│ Relay:     %s", relay_get_energy_output() ? "ACIK" : "KAPALI");
    ESP_LOGI(TAG, "└──────────────────────────────────────");
}
