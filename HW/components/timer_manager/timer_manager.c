/**
 * LebensSpur - Zamanlayici Yonetimi
 * NVS uzerinde kalici geri sayim zamanlayicisi
 * esp_timer ile saniye bazinda geri sayim
 * Tatil modu: ayri geri sayim, ana timer'i kontrol eder
 */

#include "timer_manager.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "TIMER_MGR";

#define NVS_NAMESPACE     "ls_timer"
#define NVS_KEY_ENABLED   "t_enabled"
#define NVS_KEY_REMAINING "t_remain"
#define NVS_KEY_TOTAL     "t_total"
#define NVS_KEY_ALARMS    "t_alarms"
#define NVS_KEY_VAC_EN    "v_enabled"
#define NVS_KEY_VAC_REM   "v_remain"
#define NVS_KEY_VAC_TOT   "v_total"
#define NVS_SAVE_INTERVAL 60

static esp_timer_handle_t s_timer = NULL;

// Ana timer durumu
static volatile bool    s_enabled = false;
static volatile int32_t s_remaining = 0;
static volatile int32_t s_total = 0;

// Tatil modu durumu
static volatile bool    s_vacation_enabled = false;
static volatile int32_t s_vacation_remaining = 0;
static volatile int32_t s_vacation_total = 0;

// Alarm sayisi
static volatile uint8_t s_alarm_count = 3;

static volatile int s_save_counter = 0;

// Expiry callback
static void (*s_expiry_cb)(void) = NULL;

// Alarm callback
static void (*s_alarm_cb)(int alarm_index) = NULL;
static volatile int s_alarms_fired = 0;     // Kac alarm tetiklendi
static volatile int32_t s_next_alarm_threshold = 0;  // Sonraki alarm esik degeri

// SSE sync callback (her N saniyede cagirilir)
static void (*s_tick_cb)(void) = NULL;
#define SSE_SYNC_INTERVAL 2

// Alarm esik degerini hesapla
// Alarmlar sondan geriye dogru: threshold = total * (fired+1) / (alarm_count+1)
static void recalc_alarm_threshold(void)
{
    if (s_alarm_count == 0 || s_total <= 0) {
        s_next_alarm_threshold = 0;
        return;
    }
    // remaining <= threshold olunca alarm tetiklenir
    // Ornek: total=24h, alarm_count=3 -> threshold'lar: 18h, 12h, 6h
    s_next_alarm_threshold = (int32_t)((int64_t)s_total * (s_alarm_count - s_alarms_fired) / (s_alarm_count + 1));
    if (s_alarms_fired >= s_alarm_count) {
        s_next_alarm_threshold = 0;  // Tum alarmlar tetiklendi
    }
}

// ============================================================
// NVS islemleri
// ============================================================

static void save_to_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;

    nvs_set_u8(nvs, NVS_KEY_ENABLED, s_enabled ? 1 : 0);
    nvs_set_i32(nvs, NVS_KEY_REMAINING, s_remaining);
    nvs_set_i32(nvs, NVS_KEY_TOTAL, s_total);
    nvs_set_u8(nvs, NVS_KEY_ALARMS, s_alarm_count);
    nvs_set_u8(nvs, NVS_KEY_VAC_EN, s_vacation_enabled ? 1 : 0);
    nvs_set_i32(nvs, NVS_KEY_VAC_REM, s_vacation_remaining);
    nvs_set_i32(nvs, NVS_KEY_VAC_TOT, s_vacation_total);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void load_from_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        s_enabled = false;
        s_remaining = 0;
        s_total = 0;
        s_vacation_enabled = false;
        s_vacation_remaining = 0;
        s_vacation_total = 0;
        s_alarm_count = 3;
        return;
    }

    uint8_t en = 0, vac_en = 0, alarms = 3;
    int32_t rem = 0, tot = 0, vac_rem = 0, vac_tot = 0;

    nvs_get_u8(nvs, NVS_KEY_ENABLED, &en);
    nvs_get_i32(nvs, NVS_KEY_REMAINING, &rem);
    nvs_get_i32(nvs, NVS_KEY_TOTAL, &tot);
    nvs_get_u8(nvs, NVS_KEY_ALARMS, &alarms);
    nvs_get_u8(nvs, NVS_KEY_VAC_EN, &vac_en);
    nvs_get_i32(nvs, NVS_KEY_VAC_REM, &vac_rem);
    nvs_get_i32(nvs, NVS_KEY_VAC_TOT, &vac_tot);
    nvs_close(nvs);

    s_enabled = (en == 1);
    s_remaining = rem;
    s_total = tot;
    s_alarm_count = (alarms > 10) ? 3 : alarms;
    s_vacation_enabled = (vac_en == 1);
    s_vacation_remaining = vac_rem;
    s_vacation_total = vac_tot;

    if (s_remaining <= 0) {
        s_enabled = false;
        s_remaining = 0;
    }
    if (s_total <= 0) {
        s_total = 0;
    }
    if (s_vacation_remaining <= 0) {
        s_vacation_enabled = false;
        s_vacation_remaining = 0;
    }
    if (s_vacation_total <= 0) {
        s_vacation_total = 0;
    }
}

// ============================================================
// esp_timer tick
// ============================================================

static volatile int s_tick_counter = 0;

static void timer_tick_cb(void *arg)
{
    (void)arg;

    // Tatil modu aktifse: tatil sayacini azalt
    if (s_vacation_enabled && s_vacation_remaining > 0) {
        s_vacation_remaining--;

        if (s_vacation_remaining <= 0) {
            // Tatil bitti -> ana timer'i otomatik baslat
            s_vacation_enabled = false;
            s_vacation_remaining = 0;

            if (s_total > 0) {
                s_remaining = s_total;
                s_enabled = true;
                s_alarms_fired = 0;
                recalc_alarm_threshold();
                ESP_LOGI(TAG, "Tatil bitti! Ana timer otomatik baslatildi: %ld saniye",
                         (long)s_remaining);
            }

            save_to_nvs();
            if (s_tick_cb) s_tick_cb();
            return;
        }

        // NVS checkpoint
        s_save_counter++;
        if (s_save_counter >= NVS_SAVE_INTERVAL) {
            s_save_counter = 0;
            save_to_nvs();
            ESP_LOGD(TAG, "Tatil NVS checkpoint: %ld saniye kaldi",
                     (long)s_vacation_remaining);
        }

        // SSE sync (her N saniyede)
        s_tick_counter++;
        if (s_tick_counter >= SSE_SYNC_INTERVAL) {
            s_tick_counter = 0;
            if (s_tick_cb) s_tick_cb();
        }
        return;
    }

    // Normal ana timer
    if (!s_enabled || s_remaining <= 0) return;

    s_remaining--;

    // Alarm kontrol - esik degerine ulasildiysa alarm tetikle
    if (s_alarm_cb && s_next_alarm_threshold > 0 && s_remaining > 0 &&
        s_remaining <= s_next_alarm_threshold) {
        int idx = s_alarms_fired;
        s_alarms_fired++;
        recalc_alarm_threshold();
        ESP_LOGI(TAG, "Alarm tetiklendi: #%d (remaining=%ld, next_threshold=%ld)",
                 idx, (long)s_remaining, (long)s_next_alarm_threshold);
        s_alarm_cb(idx);
    }

    if (s_remaining <= 0) {
        s_enabled = false;
        s_remaining = 0;
        save_to_nvs();
        ESP_LOGI(TAG, "Zamanlayici bitti!");
        if (s_expiry_cb) {
            s_expiry_cb();
        }
        return;
    }

    s_save_counter++;
    if (s_save_counter >= NVS_SAVE_INTERVAL) {
        s_save_counter = 0;
        save_to_nvs();
        ESP_LOGD(TAG, "NVS checkpoint: %ld saniye kaldi", (long)s_remaining);
    }

    // SSE sync (her N saniyede)
    s_tick_counter++;
    if (s_tick_counter >= SSE_SYNC_INTERVAL) {
        s_tick_counter = 0;
        if (s_tick_cb) s_tick_cb();
    }
}

static void start_hw_timer(void)
{
    if (s_timer) {
        esp_timer_stop(s_timer);
        esp_timer_delete(s_timer);
        s_timer = NULL;
    }

    const esp_timer_create_args_t args = {
        .callback = timer_tick_cb,
        .name = "countdown",
    };

    esp_err_t ret = esp_timer_create(&args, &s_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Timer olusturulamadi: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_timer_start_periodic(s_timer, 1000000);  // 1 saniye
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Timer baslatilamadi: %s", esp_err_to_name(ret));
    }
}

static void stop_hw_timer(void)
{
    if (s_timer) {
        esp_timer_stop(s_timer);
        esp_timer_delete(s_timer);
        s_timer = NULL;
    }
}

// Herhangi bir geri sayim aktifse hw timer calistir
static void ensure_hw_timer(void)
{
    bool need_timer = (s_enabled && s_remaining > 0) ||
                      (s_vacation_enabled && s_vacation_remaining > 0);

    if (need_timer && !s_timer) {
        start_hw_timer();
    } else if (!need_timer && s_timer) {
        stop_hw_timer();
    }
}

// ============================================================
// Public API - Ana Timer
// ============================================================

esp_err_t timer_manager_init(void)
{
    load_from_nvs();

    ESP_LOGI(TAG, "Timer yuklendi: enabled=%d, remaining=%ld, total=%ld",
             s_enabled, (long)s_remaining, (long)s_total);
    ESP_LOGI(TAG, "Tatil: enabled=%d, remaining=%ld, total=%ld",
             s_vacation_enabled, (long)s_vacation_remaining, (long)s_vacation_total);
    ESP_LOGI(TAG, "Alarm sayisi: %d", s_alarm_count);

    s_alarms_fired = 0;
    recalc_alarm_threshold();

    ensure_hw_timer();

    if (s_vacation_enabled && s_vacation_remaining > 0) {
        ESP_LOGI(TAG, "Tatil geri sayimi devam ediyor: %ld saniye", (long)s_vacation_remaining);
    } else if (s_enabled && s_remaining > 0) {
        ESP_LOGI(TAG, "Geri sayim devam ediyor: %ld saniye", (long)s_remaining);
    }

    return ESP_OK;
}

esp_err_t timer_manager_set(const char *unit, int value)
{
    if (!unit || value < 1 || value > 60) {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t multiplier = 0;
    if (strcmp(unit, "gun") == 0) {
        multiplier = 86400;
    } else if (strcmp(unit, "saat") == 0) {
        multiplier = 3600;
    } else if (strcmp(unit, "dakika") == 0) {
        multiplier = 60;
    } else {
        ESP_LOGE(TAG, "Gecersiz birim: %s", unit);
        return ESP_ERR_INVALID_ARG;
    }

    stop_hw_timer();

    s_total = value * multiplier;
    s_remaining = s_total;
    s_enabled = true;
    s_save_counter = 0;
    s_alarms_fired = 0;

    /* Alarm sayisini otomatik clamp et: max = value / 2 */
    int max_alarms = value / 2;
    if (max_alarms < 0) max_alarms = 0;
    if (s_alarm_count > (uint8_t)max_alarms) {
        s_alarm_count = (uint8_t)max_alarms;
        ESP_LOGI(TAG, "Alarm sayisi otomatik dusuruldu: %d (max=%d)", s_alarm_count, max_alarms);
    }

    recalc_alarm_threshold();

    save_to_nvs();
    start_hw_timer();

    ESP_LOGI(TAG, "Zamanlayici ayarlandi: %d %s = %ld saniye",
             value, unit, (long)s_remaining);

    return ESP_OK;
}

timer_state_t timer_manager_get_state(void)
{
    timer_state_t state;
    state.enabled = s_enabled;
    state.remaining_seconds = s_remaining;
    state.total_seconds = s_total;
    state.vacation_enabled = s_vacation_enabled;
    state.vacation_remaining = s_vacation_remaining;
    state.vacation_total = s_vacation_total;
    state.alarm_count = s_alarm_count;
    return state;
}

esp_err_t timer_manager_cancel(void)
{
    stop_hw_timer();
    s_enabled = false;
    s_remaining = 0;
    s_save_counter = 0;
    save_to_nvs();

    ESP_LOGI(TAG, "Zamanlayici iptal edildi");
    return ESP_OK;
}

esp_err_t timer_manager_restart(void)
{
    if (s_total <= 0) {
        ESP_LOGW(TAG, "Restart: onceden ayarlanmis sure yok");
        return ESP_ERR_INVALID_STATE;
    }

    stop_hw_timer();

    s_remaining = s_total;
    s_enabled = true;
    s_save_counter = 0;
    s_alarms_fired = 0;
    recalc_alarm_threshold();

    save_to_nvs();
    start_hw_timer();

    ESP_LOGI(TAG, "Zamanlayici yeniden basladi: %ld saniye", (long)s_remaining);
    return ESP_OK;
}

// ============================================================
// Public API - Tatil Modu
// ============================================================

esp_err_t timer_manager_set_vacation(int days)
{
    if (days < 1 || days > 60) {
        return ESP_ERR_INVALID_ARG;
    }

    stop_hw_timer();

    // Ana timer'i sifirla ve durdur
    if (s_total > 0) {
        s_remaining = s_total;  // basa don
    }
    s_enabled = false;

    // Tatil geri sayimini baslat
    s_vacation_total = days * 86400;  // gun -> saniye
    s_vacation_remaining = s_vacation_total;
    s_vacation_enabled = true;
    s_save_counter = 0;

    save_to_nvs();
    start_hw_timer();

    ESP_LOGI(TAG, "Tatil modu baslatildi: %d gun = %ld saniye",
             days, (long)s_vacation_remaining);

    return ESP_OK;
}

esp_err_t timer_manager_cancel_vacation(void)
{
    stop_hw_timer();

    s_vacation_enabled = false;
    s_vacation_remaining = 0;
    s_save_counter = 0;

    // Ana timer'i otomatik baslat (tatil iptal edildi)
    if (s_total > 0) {
        s_remaining = s_total;
        s_enabled = true;
        ESP_LOGI(TAG, "Tatil iptal, ana timer baslatildi: %ld saniye", (long)s_remaining);
    }

    save_to_nvs();
    ensure_hw_timer();

    ESP_LOGI(TAG, "Tatil modu iptal edildi");
    return ESP_OK;
}

// ============================================================
// Public API - Alarm
// ============================================================

esp_err_t timer_manager_set_alarm_count(int count)
{
    if (count < 0 || count > 10) {
        return ESP_ERR_INVALID_ARG;
    }

    s_alarm_count = (uint8_t)count;
    save_to_nvs();

    ESP_LOGI(TAG, "Alarm sayisi ayarlandi: %d", s_alarm_count);
    return ESP_OK;
}

// ============================================================
// Public API - Expiry Callback
// ============================================================

void timer_manager_set_expiry_callback(void (*cb)(void))
{
    s_expiry_cb = cb;
}

void timer_manager_set_tick_callback(void (*cb)(void))
{
    s_tick_cb = cb;
}

void timer_manager_set_alarm_callback(void (*cb)(int alarm_index))
{
    s_alarm_cb = cb;
}
