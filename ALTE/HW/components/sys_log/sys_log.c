/**
 * LebensSpur - Sistem Olay Kaydi (sys_log)
 *
 * Tasarim:
 *   RTC_NOINIT (LP bellek) kaydi  -> brownout/panic/WDT/SW resette korunur
 *   /ext/logs/events.log          -> JSONL, her satir bir kayit (harici flash, user_data)
 *   /ext/logs/checkpoint.json     -> 10 dk'da bir; tam guc kaybi icin yedek
 *
 * Acilis analizi (sys_log_init):
 *   1. esp_reset_reason() + RTC kaydinin gecerliligi -> kesin siniflandirma
 *   2. Onceki calisma >= 5 sn ise events.log'a "boot" kaydi
 *   3. < 5 sn ise sayilir (short), bir sonraki uzun kayda eklenir
 */

#include "sys_log.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stddef.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "ext_flash.h"

static const char *TAG = "SYS_LOG";

/* ========== Ayarlar ========== */
#define LOG_DIR             "/ext/logs"
#define LOG_FILE            LOG_DIR "/events.log"
#define LOG_TMP_FILE        LOG_DIR "/events.tmp"
#define CKPT_FILE           LOG_DIR "/checkpoint.json"

#define LOG_MAX_BYTES       (64 * 1024)     /* Asilirsa yarisi atilir */
#define LOG_READ_MAX_BYTES  (24 * 1024)     /* API okumasi: dosya sonundan en fazla bu kadar */
#define MIN_RUN_S           5               /* Bundan kisa calisma loga girmez */
#define CKPT_INTERVAL_S     600             /* Kontrol noktasi: 10 dk */
#define HEALTH_INTERVAL_S   (12 * 3600)     /* Heap/uptime sagligi: 12 saat */
#define LOAD_PEAK_WINDOW_S  30              /* Brownout'u yuk olayina baglama penceresi */

#define RTC_MAGIC           0x4C534C47u     /* 'LSLG' */

/* ========== RTC kaydi ========== */
typedef struct {
    uint32_t magic;
    uint32_t boot_count;        /* Bu acilis dahil toplam acilis */
    uint32_t uptime_s;          /* Saniyede bir guncellenir */
    uint32_t boot_epoch;        /* SNTP senkronu sonrasi: acilis zamani (0=bilinmiyor) */
    uint32_t short_boots;       /* Loga girmeyen kisa acilis sayaci */
    uint32_t last_event;        /* sys_log_event_t */
    uint32_t last_event_up_s;   /* Son olay aninda uptime */
    int32_t  last_event_arg;
    uint32_t heap_free;
    uint32_t heap_min;
    uint32_t restart_reason;    /* sys_log_restart_t (esp_restart oncesi) */
    uint8_t  wifi_mode;         /* sys_log_wifi_mode_t */
    uint8_t  ap_clients;
    int8_t   sta_rssi;
    int8_t   tx_dbm;
    uint32_t check;             /* FNV-1a, 'check' haric tum alanlar */
} rtc_rec_t;

static RTC_NOINIT_ATTR rtc_rec_t s_rtc;
static portMUX_TYPE s_rtc_mux = portMUX_INITIALIZER_UNLOCKED;
#define RTC_LOCK()   portENTER_CRITICAL(&s_rtc_mux)
#define RTC_UNLOCK() portEXIT_CRITICAL(&s_rtc_mux)

/* ========== Durum ========== */
static SemaphoreHandle_t s_mutex = NULL;
static bool s_fs_ok = false;
static uint32_t s_write_count = 0;
static TaskHandle_t s_task = NULL;
static const char *s_last_cause = "unknown";
static const char *s_last_cat = "unknown";
static uint32_t s_prev_uptime = 0;

/* ========== Yardimcilar ========== */

static uint32_t rtc_checksum(const rtc_rec_t *r)
{
    const uint8_t *p = (const uint8_t *)r;
    size_t n = offsetof(rtc_rec_t, check);
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

static bool rtc_valid(void)
{
    return s_rtc.magic == RTC_MAGIC && s_rtc.check == rtc_checksum(&s_rtc);
}

static void rtc_commit(void)
{
    s_rtc.magic = RTC_MAGIC;
    s_rtc.check = rtc_checksum(&s_rtc);
}

static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static uint32_t now_epoch(void)
{
    if (s_rtc.boot_epoch == 0) return 0;
    return s_rtc.boot_epoch + uptime_s();
}

static const char *event_name(uint32_t ev)
{
    switch (ev) {
        case SL_EV_BOOT:            return "BOOT";
        case SL_EV_AP_CLIENT_JOIN:  return "AP_CLIENT_JOIN";
        case SL_EV_AP_CLIENT_LEAVE: return "AP_CLIENT_LEAVE";
        case SL_EV_STA_CONNECTING:  return "STA_CONNECTING";
        case SL_EV_STA_GOT_IP:      return "STA_GOT_IP";
        case SL_EV_STA_LOST:        return "STA_LOST";
        case SL_EV_GUI_SERVE:       return "GUI_SERVE";
        case SL_EV_MAIL_SEND:       return "MAIL_SEND";
        case SL_EV_TG_SEND:         return "TG_SEND";
        case SL_EV_WEBHOOK:         return "WEBHOOK";
        case SL_EV_OTA_FW:          return "OTA_FW";
        case SL_EV_OTA_GUI:         return "OTA_GUI";
        case SL_EV_TX_POWER:        return "TX_POWER";
        case SL_EV_RELAY:           return "RELAY";
        case SL_EV_TIMER_EXPIRED:   return "TIMER_EXPIRED";
        case SL_EV_BUTTON:          return "BUTTON";
        case SL_EV_USER_RESTART:    return "USER_RESTART";
        default:                    return "NONE";
    }
}

static const char *wifi_mode_name(uint8_t m)
{
    switch (m) {
        case SL_WIFI_AP:    return "AP";
        case SL_WIFI_STA:   return "STA";
        case SL_WIFI_APSTA: return "APSTA";
        default:            return "OFF";
    }
}

/* Yuk piki ureten olaylar: brownout bu olaylardan kisa sure sonra geldiyse
 * "akim yetersiz" yorumu yapilir. */
static bool event_is_load_peak(uint32_t ev)
{
    /* BOOT: radyo acilisi + flash init -> acilis akim piki
     * MAIL/TG: TLS el sikisma (CPU) + yogun TX; RELAY: bobin akimi */
    return ev == SL_EV_BOOT || ev == SL_EV_AP_CLIENT_JOIN || ev == SL_EV_GUI_SERVE ||
           ev == SL_EV_STA_CONNECTING || ev == SL_EV_STA_GOT_IP ||
           ev == SL_EV_MAIL_SEND || ev == SL_EV_TG_SEND || ev == SL_EV_WEBHOOK ||
           ev == SL_EV_OTA_FW || ev == SL_EV_OTA_GUI || ev == SL_EV_RELAY;
}

/* JSON string icin minimal escape (" ve \) */
static void json_escape_into(char *dst, size_t dst_size, const char *src)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 2 < dst_size; si++) {
        unsigned char c = (unsigned char)src[si];
        if (c == '"' || c == '\\') {
            dst[di++] = '\\';
            dst[di++] = (char)c;
        } else if (c < 0x20) {
            dst[di++] = ' ';
        } else {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
}

static long file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

/* ========== Dosya islemleri (mutex altinda cagrilir) ========== */

/* Dosya buyudukce ilk yarisini at (satir sinirinda). */
static void rotate_if_needed_locked(void)
{
    long sz = file_size(LOG_FILE);
    if (sz <= LOG_MAX_BYTES) return;

    FILE *in = fopen(LOG_FILE, "r");
    if (!in) return;

    /* Ortadan sonraki ilk satir basina atla */
    fseek(in, sz / 2, SEEK_SET);
    int c;
    while ((c = fgetc(in)) != EOF && c != '\n') { }

    FILE *out = fopen(LOG_TMP_FILE, "w");
    if (!out) { fclose(in); return; }

    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
    remove(LOG_FILE);
    rename(LOG_TMP_FILE, LOG_FILE);
    ESP_LOGI(TAG, "Log dondu: %ld -> %ld byte", sz, file_size(LOG_FILE));
}

static void append_line_locked(const char *line)
{
    if (!s_fs_ok) return;
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) {
        ESP_LOGE(TAG, "Log dosyasi acilamadi");
        return;
    }
    fputs(line, f);
    fputc('\n', f);
    fclose(f);

    if ((++s_write_count % 32) == 0) {
        rotate_if_needed_locked();
    }
}

static void write_checkpoint_locked(void)
{
    if (!s_fs_ok) return;
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"boot\":%lu,\"up\":%lu,\"epoch\":%lu,\"heap_min\":%lu}",
             (unsigned long)s_rtc.boot_count, (unsigned long)s_rtc.uptime_s,
             (unsigned long)s_rtc.boot_epoch, (unsigned long)s_rtc.heap_min);
    FILE *f = fopen(CKPT_FILE, "w");
    if (!f) return;
    fputs(buf, f);
    fclose(f);
}

/* Kontrol noktasini oku; yoksa alanlar 0 kalir. */
static void read_checkpoint(uint32_t *boot, uint32_t *up, uint32_t *epoch, uint32_t *heap_min)
{
    *boot = *up = *epoch = *heap_min = 0;
    if (!s_fs_ok) return;
    FILE *f = fopen(CKPT_FILE, "r");
    if (!f) return;
    char buf[200] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    const char *p;
    if ((p = strstr(buf, "\"boot\":")))     *boot     = (uint32_t)strtoul(p + 7, NULL, 10);
    if ((p = strstr(buf, "\"up\":")))       *up       = (uint32_t)strtoul(p + 5, NULL, 10);
    if ((p = strstr(buf, "\"epoch\":")))    *epoch    = (uint32_t)strtoul(p + 8, NULL, 10);
    if ((p = strstr(buf, "\"heap_min\":"))) *heap_min = (uint32_t)strtoul(p + 11, NULL, 10);
}

/* ========== Acilis analizi ========== */

typedef struct {
    const char *cat;    /* power | software | user | other | unknown */
    const char *cause;  /* kisa kod */
    const char *msg;    /* insan okur aciklama */
    const char *hint;   /* yorum / olasi neden */
} cause_info_t;

static cause_info_t classify(esp_reset_reason_t rr, bool rtc_ok, const rtc_rec_t *prev)
{
    cause_info_t c = { "unknown", "unknown", "Bilinmeyen reset nedeni", "" };

    switch (rr) {
    case ESP_RST_BROWNOUT: {
        c.cat = "power";
        c.cause = "brownout";
        c.msg = "Besleme gerilimi dustu (brownout reset)";
        if (rtc_ok && event_is_load_peak(prev->last_event) &&
            prev->uptime_s - prev->last_event_up_s <= LOAD_PEAK_WINDOW_S) {
            c.hint = "WiFi/TLS/role yuk pikinde akim yetersiz: besleme kapasitesi veya tampon kondansator";
        } else {
            c.hint = "Yuk pikine bagli degil: besleme veya AC-DC donusturucu kararsiz";
        }
        break;
    }
    case ESP_RST_PWR_GLITCH:
        c.cat = "power"; c.cause = "pwr_glitch";
        c.msg = "Guc hattinda ani dalgalanma (glitch) algilandi";
        c.hint = "AC-DC cikisi veya 3.3V hatti gurultulu; filtreleme/kondansator";
        break;
    case ESP_RST_POWERON:
        c.cat = "power";
        if (!rtc_ok) {
            c.cause = "power_loss";
            c.msg = "Tam guc kesintisi (RTC bellek silindi)";
            c.hint = "Elektrik gitti veya AC-DC cikisi tamamen coktu";
        } else {
            c.cause = "poweron_rst";
            c.msg = "Power-on reset (RTC bellek korunmus)";
            c.hint = "EN/reset hatti tetiklendi veya cok kisa guc kesintisi";
        }
        break;
    case ESP_RST_PANIC:
        c.cat = "software"; c.cause = "panic";
        c.msg = "Yazilim cokmesi (panic / exception)";
        c.hint = "Son olay ve heap degerleri hata kaynagini gosterir";
        break;
    case ESP_RST_INT_WDT:
        c.cat = "software"; c.cause = "int_wdt";
        c.msg = "Kesme watchdog: sistem kilitlendi (interrupt WDT)";
        c.hint = "Bir surucu/kesme cok uzun bloke etti";
        break;
    case ESP_RST_TASK_WDT:
        c.cat = "software"; c.cause = "task_wdt";
        c.msg = "Gorev watchdog: bir gorev yanit vermedi (task WDT)";
        c.hint = "Donma yakalandi ve cihaz kendini kurtardi";
        break;
    case ESP_RST_WDT:
        c.cat = "software"; c.cause = "wdt";
        c.msg = "Donanim watchdog (RTC WDT) tetiklendi";
        c.hint = "";
        break;
    case ESP_RST_CPU_LOCKUP:
        c.cat = "software"; c.cause = "cpu_lockup";
        c.msg = "CPU kilitlendi (double exception)";
        c.hint = "";
        break;
    case ESP_RST_SW: {
        c.cat = "user";
        uint32_t r = rtc_ok ? prev->restart_reason : SL_RST_NONE;
        switch (r) {
            case SL_RST_USER_WEB:    c.cause = "user_restart";   c.msg = "Kullanici yeniden baslatti (web arayuzu)"; break;
            case SL_RST_USER_BUTTON: c.cause = "button_restart"; c.msg = "Kullanici yeniden baslatti (buton 3x)"; break;
            case SL_RST_FACTORY:     c.cause = "factory_reset";  c.msg = "Fabrika sifirlama sonrasi yeniden baslatma"; break;
            case SL_RST_OTA_FW:      c.cause = "ota_fw";         c.msg = "Firmware guncellemesi sonrasi yeniden baslatma"; break;
            case SL_RST_OTA_GUI:     c.cause = "ota_gui";        c.msg = "Arayuz guncellemesi sonrasi yeniden baslatma"; break;
            default:
                c.cat = "software"; c.cause = "sw_restart";
                c.msg = "Yazilim yeniden baslatma (esp_restart, kaynak belirtilmemis)";
                break;
        }
        break;
    }
    case ESP_RST_DEEPSLEEP: c.cat = "other"; c.cause = "deepsleep"; c.msg = "Derin uykudan uyanma"; break;
    case ESP_RST_EXT:       c.cat = "other"; c.cause = "ext_pin";   c.msg = "Harici reset pini"; break;
    case ESP_RST_SDIO:      c.cat = "other"; c.cause = "sdio";      c.msg = "SDIO reset"; break;
    case ESP_RST_USB:       c.cat = "other"; c.cause = "usb";       c.msg = "USB reset (programlama/seri)"; break;
    case ESP_RST_JTAG:      c.cat = "other"; c.cause = "jtag";      c.msg = "JTAG reset"; break;
    case ESP_RST_EFUSE:     c.cat = "other"; c.cause = "efuse";     c.msg = "eFuse hatasi"; break;
    default: break;
    }
    return c;
}

static void analyze_previous_run_and_log(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    bool rtc_ok = rtc_valid();
    rtc_rec_t prev;
    if (rtc_ok) {
        prev = s_rtc;
    } else {
        memset(&prev, 0, sizeof(prev));
    }

    /* Tam guc kaybinda RTC bos: kontrol noktasindan yaklasik degerleri al */
    bool approx = false;
    uint32_t ck_boot, ck_up, ck_epoch, ck_heap_min;
    read_checkpoint(&ck_boot, &ck_up, &ck_epoch, &ck_heap_min);
    if (!rtc_ok) {
        prev.boot_count = ck_boot;
        prev.uptime_s   = ck_up;
        prev.boot_epoch = ck_epoch;
        prev.heap_min   = ck_heap_min;
        approx = (ck_boot > 0);
    }

    cause_info_t c = classify(rr, rtc_ok, &prev);
    s_last_cause = c.cause;
    s_last_cat = c.cat;
    s_prev_uptime = prev.uptime_s;

    uint32_t prev_boot_no = prev.boot_count;
    uint32_t prev_uptime  = prev.uptime_s;
    uint32_t short_boots  = rtc_ok ? prev.short_boots : 0;

    /* Yeni acilis icin RTC kaydini kur */
    uint32_t new_boot_count = prev.boot_count + 1;
    memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.boot_count = new_boot_count;
    s_rtc.heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    s_rtc.heap_free = esp_get_free_heap_size();
    s_rtc.last_event = SL_EV_BOOT;

    ESP_LOGI(TAG, "Acilis #%lu | onceki: #%lu %lu sn (%s) | neden: %s (%s)",
             (unsigned long)new_boot_count, (unsigned long)prev_boot_no,
             (unsigned long)prev_uptime, approx ? "yaklasik" : (rtc_ok ? "kesin" : "yok"),
             c.cause, c.msg);

    /* Ilk acilis (hic kayit yok): loga yazma */
    if (prev_boot_no == 0) {
        s_rtc.short_boots = 0;
        rtc_commit();
        return;
    }
    bool short_run = (prev_uptime < MIN_RUN_S);
    if (short_run) {
        s_rtc.short_boots = short_boots + 1;
        rtc_commit();
        /* Kisa calismalar normalde loglanmaz. Ancak neden GUC veya YAZILIM ise
         * (acilista brownout = boot dongusu) kaydedilmezse en yaygin ariza
         * gorunmez olur: 1., 10., 20., ... kisa acilista toplu tek satir yaz. */
        bool serious = (strcmp(c.cat, "power") == 0 || strcmp(c.cat, "software") == 0);
        uint32_t n = s_rtc.short_boots;
        if (!serious || !(n == 1 || n % 10 == 0)) {
            ESP_LOGW(TAG, "Kisa calisma (%lu sn) loga yazilmadi, kisa acilis sayisi=%lu",
                     (unsigned long)prev_uptime, (unsigned long)n);
            return;
        }
        short_boots = n;        /* kayda toplam kisa acilis sayisi girsin */
    } else {
        s_rtc.short_boots = 0;
        rtc_commit();
    }

    /* Onceki calismanin bitis zamani (biliniyorsa) */
    uint32_t end_epoch = (prev.boot_epoch > 0) ? prev.boot_epoch + prev_uptime : 0;

    char line[640];
    snprintf(line, sizeof(line),
        "{\"type\":\"boot\",\"n\":%lu,\"ts\":%lu,\"up\":%lu,\"approx\":%s,"
        "\"cat\":\"%s\",\"cause\":\"%s\",\"msg\":\"%s\",\"hint\":\"%s\","
        "\"ev\":\"%s\",\"ev_dt\":%ld,\"ev_arg\":%ld,"
        "\"wifi\":\"%s\",\"cl\":%u,\"rssi\":%d,\"tx\":%d,"
        "\"heap_free\":%lu,\"heap_min\":%lu,\"short\":%lu}",
        (unsigned long)prev_boot_no, (unsigned long)end_epoch, (unsigned long)prev_uptime,
        approx ? "true" : "false",
        c.cat, c.cause, c.msg, c.hint,
        rtc_ok ? event_name(prev.last_event) : "NONE",
        rtc_ok ? (long)(prev.uptime_s - prev.last_event_up_s) : -1L,
        rtc_ok ? (long)prev.last_event_arg : 0L,
        rtc_ok ? wifi_mode_name(prev.wifi_mode) : "?",
        rtc_ok ? prev.ap_clients : 0,
        rtc_ok ? prev.sta_rssi : 0,
        rtc_ok ? prev.tx_dbm : 0,
        (unsigned long)prev.heap_free, (unsigned long)prev.heap_min,
        (unsigned long)short_boots);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    append_line_locked(line);
    xSemaphoreGive(s_mutex);
}

/* ========== Izleme gorevi ========== */

static void monitor_task(void *arg)
{
    (void)arg;
    /* Ilk kontrol noktasi 60 sn'de (init'teki up=0 kaydini kisa surede ezer),
     * sonra 10 dakikada bir. */
    uint32_t last_ckpt = 60 - CKPT_INTERVAL_S;
    uint32_t last_health = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uint32_t up = uptime_s();
        uint32_t hf = esp_get_free_heap_size();
        uint32_t hm = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
        RTC_LOCK();
        s_rtc.uptime_s = up;
        s_rtc.heap_free = hf;
        s_rtc.heap_min = hm;
        rtc_commit();
        RTC_UNLOCK();

        if (up - last_ckpt >= CKPT_INTERVAL_S) {
            last_ckpt = up;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            write_checkpoint_locked();
            xSemaphoreGive(s_mutex);
        }

        if (up - last_health >= HEALTH_INTERVAL_S) {
            last_health = up;
            sys_log_write("INFO", "sys", "Saglik: %lu sn calisma, heap bos %lu, heap min %lu, "
                          "wifi %s, istemci %u, rssi %d, tx %d dBm",
                          (unsigned long)up, (unsigned long)s_rtc.heap_free,
                          (unsigned long)s_rtc.heap_min, wifi_mode_name(s_rtc.wifi_mode),
                          s_rtc.ap_clients, s_rtc.sta_rssi, s_rtc.tx_dbm);
        }
    }
}

/* esp_restart() her cagrildiginda: kaynak belirtilmemisse GENERIC isaretle */
static void shutdown_handler(void)
{
    uint32_t up = uptime_s();
    RTC_LOCK();
    if (s_rtc.restart_reason == SL_RST_NONE) {
        s_rtc.restart_reason = SL_RST_GENERIC;
    }
    s_rtc.uptime_s = up;
    rtc_commit();
    RTC_UNLOCK();
}

/* ========== Public API ========== */

esp_err_t sys_log_init(void)
{
    if (s_mutex) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_fs_ok = ext_flash_is_user_data_mounted();
    if (s_fs_ok) {
        mkdir(LOG_DIR, 0755);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        rotate_if_needed_locked();
        xSemaphoreGive(s_mutex);
    } else {
        ESP_LOGW(TAG, "User data mount degil: kayitlar sadece RTC'de tutulacak");
    }

    analyze_previous_run_and_log();

    /* Ilk kontrol noktasi: boot sayaci hemen kalici olsun */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    write_checkpoint_locked();
    xSemaphoreGive(s_mutex);

    esp_register_shutdown_handler(shutdown_handler);

    xTaskCreate(monitor_task, "sys_log", 4096, NULL, 1, &s_task);
    return ESP_OK;
}

void sys_log_mark_event(sys_log_event_t ev, int32_t arg)
{
    uint32_t up = uptime_s();
    RTC_LOCK();
    s_rtc.last_event = ev;
    s_rtc.last_event_up_s = up;
    s_rtc.last_event_arg = arg;
    s_rtc.uptime_s = up;
    rtc_commit();
    RTC_UNLOCK();
}

void sys_log_set_wifi_ctx(sys_log_wifi_mode_t mode, uint8_t ap_clients,
                          int8_t sta_rssi, int8_t tx_dbm)
{
    RTC_LOCK();
    s_rtc.wifi_mode = (uint8_t)mode;
    s_rtc.ap_clients = ap_clients;
    s_rtc.sta_rssi = sta_rssi;
    s_rtc.tx_dbm = tx_dbm;
    rtc_commit();
    RTC_UNLOCK();
}

void sys_log_set_restart_reason(sys_log_restart_t reason)
{
    uint32_t up = uptime_s();
    RTC_LOCK();
    s_rtc.restart_reason = (uint32_t)reason;
    s_rtc.uptime_s = up;
    rtc_commit();
    RTC_UNLOCK();
}

void sys_log_time_synced(void)
{
    time_t now = time(NULL);
    if (now < 1700000000) return;   /* 2023 oncesi = gecersiz */
    uint32_t up = uptime_s();
    RTC_LOCK();
    s_rtc.boot_epoch = (uint32_t)now - up;
    rtc_commit();
    RTC_UNLOCK();
    ESP_LOGI(TAG, "Saat senkron: acilis epoch=%lu", (unsigned long)s_rtc.boot_epoch);
}

void sys_log_write(const char *level, const char *cat, const char *fmt, ...)
{
    /* Cagiran task'in yigini kucuk olabilir (WiFi event task): tamponlar heap'te */
    enum { MSG_SZ = 256, ESC_SZ = 320, LINE_SZ = 420 };
    char *blk = malloc(MSG_SZ + ESC_SZ);     /* msg + esc */
    char *line = malloc(LINE_SZ);            /* ayri blok: -Werror=restrict */
    if (!blk || !line) { free(blk); free(line); return; }
    char *msg = blk, *esc = blk + MSG_SZ;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, MSG_SZ, fmt, ap);
    va_end(ap);

    json_escape_into(esc, ESC_SZ, msg);

    snprintf(line, LINE_SZ,
             "{\"type\":\"event\",\"n\":%lu,\"ts\":%lu,\"up\":%lu,"
             "\"lvl\":\"%s\",\"cat\":\"%s\",\"msg\":\"%s\"}",
             (unsigned long)s_rtc.boot_count, (unsigned long)now_epoch(),
             (unsigned long)uptime_s(), level ? level : "INFO", cat ? cat : "sys", esc);

    ESP_LOGI(TAG, "[%s/%s] %s", level ? level : "INFO", cat ? cat : "sys", msg);

    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        append_line_locked(line);
        xSemaphoreGive(s_mutex);
    }
    free(line);
    free(blk);
}

char *sys_log_read_tail_json(int max_entries)
{
    if (max_entries <= 0) max_entries = 100;

    if (!s_fs_ok || !s_mutex) {
        char *empty = malloc(3);
        if (empty) strcpy(empty, "[]");
        return empty;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    long total = file_size(LOG_FILE);
    if (total <= 0) {
        xSemaphoreGive(s_mutex);
        char *empty = malloc(3);
        if (empty) strcpy(empty, "[]");
        return empty;
    }
    /* En yeni kayitlar dosyanin SONUNDA: buyukse son kismi oku */
    long sz = total;
    long skip = 0;
    if (sz > LOG_READ_MAX_BYTES) {
        sz = LOG_READ_MAX_BYTES;
        skip = total - sz;
    }

    char *raw = malloc(sz + 1);
    if (!raw) { xSemaphoreGive(s_mutex); return NULL; }

    FILE *f = fopen(LOG_FILE, "r");
    if (!f) { free(raw); xSemaphoreGive(s_mutex); return NULL; }
    if (skip > 0) fseek(f, skip, SEEK_SET);
    size_t n = fread(raw, 1, sz, f);
    fclose(f);
    xSemaphoreGive(s_mutex);
    raw[n] = '\0';

    /* Satir baslangiclarini topla */
    int cap = 256, cnt = 0;
    char **lines = malloc(sizeof(char *) * cap);
    if (!lines) { free(raw); return NULL; }

    char *p = raw;
    if (skip > 0) {                 /* kesik ilk satiri atla */
        char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : p + strlen(p);
    }
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        if (*p == '{') {
            if (cnt == cap) {
                cap *= 2;
                char **tmp = realloc(lines, sizeof(char *) * cap);
                if (!tmp) break;
                lines = tmp;
            }
            lines[cnt++] = p;
        }
        if (!nl) break;
        p = nl + 1;
    }

    /* En yeni once, en fazla max_entries */
    int first = cnt - max_entries;
    if (first < 0) first = 0;

    size_t out_cap = (size_t)n + (size_t)cnt * 2 + 8;
    char *out = malloc(out_cap);
    if (!out) { free(lines); free(raw); return NULL; }

    size_t pos = 0;
    out[pos++] = '[';
    for (int i = cnt - 1; i >= first; i--) {
        size_t l = strlen(lines[i]);
        if (pos + l + 2 >= out_cap) break;
        if (i != cnt - 1) out[pos++] = ',';
        memcpy(out + pos, lines[i], l);
        pos += l;
    }
    out[pos++] = ']';
    out[pos] = '\0';

    free(lines);
    free(raw);
    return out;
}

esp_err_t sys_log_clear(void)
{
    if (!s_fs_ok || !s_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    remove(LOG_FILE);
    s_write_count = 0;
    xSemaphoreGive(s_mutex);
    sys_log_write("INFO", "sys", "Kayitlar kullanici tarafindan temizlendi");
    return ESP_OK;
}

uint32_t sys_log_boot_count(void)       { return s_rtc.boot_count; }
uint32_t sys_log_short_boots(void)      { return s_rtc.short_boots; }
uint32_t sys_log_boot_epoch(void)       { return s_rtc.boot_epoch; }
const char *sys_log_last_cause(void)    { return s_last_cause; }
const char *sys_log_last_cause_cat(void){ return s_last_cat; }
uint32_t sys_log_prev_uptime(void)      { return s_prev_uptime; }
