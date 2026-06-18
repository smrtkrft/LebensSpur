/**
 * LebensSpur - Kalici Cihaz Log Sistemi
 *
 * Iki fazli baslatma:
 *   Faz 1: device_log_init()         - RAM buffer (ext_flash'tan ONCE)
 *   Faz 2: device_log_persist_init() - Flash'tan yukle + kalicilik aktiflestir
 *
 * Depolama: /ext/device_log.bin (binary, sabit boyut kayitlar)
 * Max 100 kayit, circular overwrite
 *
 * Onemli olaylar loglenir:
 *   - Restart nedeni, boot sayisi
 *   - WiFi/web server hatalari
 *   - OTA gecmisi (GUI + FW)
 *   - Factory reset, donanim olaylari
 *
 * Basit seyler loglanMAZ:
 *   - Ayar kaydi, sayac yenileme vb.
 */

#include "device_log.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "DEV_LOG";

// RAM ring buffer
static device_log_entry_t s_entries[DEVICE_LOG_MAX_ENTRIES];
static int s_head = 0;   // sonraki yazilacak pozisyon
static int s_count = 0;  // toplam kayit sayisi

// Kalicilik
static bool s_persist_ready = false;
static uint32_t s_boot_count = 0;

#define LOG_FILE_PATH "/ext/device_log.bin"
#define NVS_LOG_NS    "ls_devlog"

// Dosya header'i
typedef struct {
    uint32_t magic;       // 0x4C534C47 = "LSLG"
    uint32_t version;     // 1
    int32_t  head;        // s_head
    int32_t  count;       // s_count
} log_file_header_t;

#define LOG_FILE_MAGIC   0x4C534C47
#define LOG_FILE_VERSION 1

// ============================================================
// NVS boot counter
// ============================================================

static void load_boot_count(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_LOG_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_get_u32(nvs, "boot_cnt", &s_boot_count);
        s_boot_count++;
        nvs_set_u32(nvs, "boot_cnt", s_boot_count);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

// ============================================================
// Flash okuma/yazma
// ============================================================

static void save_to_flash(void)
{
    if (!s_persist_ready) return;

    FILE *f = fopen(LOG_FILE_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "Log dosyasi yazilamadi");
        return;
    }

    log_file_header_t hdr = {
        .magic = LOG_FILE_MAGIC,
        .version = LOG_FILE_VERSION,
        .head = s_head,
        .count = s_count,
    };

    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(s_entries, sizeof(device_log_entry_t), DEVICE_LOG_MAX_ENTRIES, f);
    fclose(f);
}

static bool load_from_flash(void)
{
    FILE *f = fopen(LOG_FILE_PATH, "rb");
    if (!f) return false;

    log_file_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return false;
    }

    if (hdr.magic != LOG_FILE_MAGIC || hdr.version != LOG_FILE_VERSION) {
        ESP_LOGW(TAG, "Log dosyasi gecersiz magic/version, sifirlanacak");
        fclose(f);
        return false;
    }

    if (hdr.head < 0 || hdr.head >= DEVICE_LOG_MAX_ENTRIES ||
        hdr.count < 0 || hdr.count > DEVICE_LOG_MAX_ENTRIES) {
        ESP_LOGW(TAG, "Log dosyasi gecersiz head/count, sifirlanacak");
        fclose(f);
        return false;
    }

    size_t read = fread(s_entries, sizeof(device_log_entry_t), DEVICE_LOG_MAX_ENTRIES, f);
    fclose(f);

    if (read != DEVICE_LOG_MAX_ENTRIES) {
        ESP_LOGW(TAG, "Log dosyasi eksik veri");
        return false;
    }

    s_head = hdr.head;
    s_count = hdr.count;
    return true;
}

// ============================================================
// Public API
// ============================================================

void device_log_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_head = 0;
    s_count = 0;
    s_persist_ready = false;

    // NVS boot sayacini yukle (NVS, ext_flash'tan once hazir)
    load_boot_count();
}

void device_log_persist_init(void)
{
    if (load_from_flash()) {
        ESP_LOGI(TAG, "Kalici loglar yuklendi: %d kayit (boot #%lu)",
                 s_count, (unsigned long)s_boot_count);
    } else {
        ESP_LOGI(TAG, "Yeni log dosyasi olusturulacak (boot #%lu)",
                 (unsigned long)s_boot_count);
    }
    s_persist_ready = true;
}

void device_log_add(device_log_type_t type, const char *fmt, ...)
{
    device_log_entry_t *e = &s_entries[s_head];
    e->timestamp = (uint32_t)(esp_timer_get_time() / 1000000);
    e->boot_count = s_boot_count;
    e->type = type;

    va_list args;
    va_start(args, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, args);
    va_end(args);

    s_head = (s_head + 1) % DEVICE_LOG_MAX_ENTRIES;
    if (s_count < DEVICE_LOG_MAX_ENTRIES) s_count++;

    // Flash'a yaz
    save_to_flash();
}

int device_log_get_count(void)
{
    return s_count;
}

const device_log_entry_t *device_log_get(int index)
{
    if (index < 0 || index >= s_count) return NULL;
    int pos = (s_head - 1 - index + DEVICE_LOG_MAX_ENTRIES) % DEVICE_LOG_MAX_ENTRIES;
    return &s_entries[pos];
}

uint32_t device_log_get_boot_count(void)
{
    return s_boot_count;
}

static const char *log_type_str(device_log_type_t t)
{
    switch (t) {
        case LOG_TYPE_SYSTEM:  return "system";
        case LOG_TYPE_WIFI:    return "wifi";
        case LOG_TYPE_TIMER:   return "timer";
        case LOG_TYPE_SMTP:    return "smtp";
        case LOG_TYPE_ERROR:   return "error";
        case LOG_TYPE_OTA:     return "ota";
        case LOG_TYPE_RESTART: return "restart";
        case LOG_TYPE_HW:      return "hw";
        default:               return "unknown";
    }
}

int device_log_to_json(char *buf, int bufsz, int max_count)
{
    int count = s_count;
    if (max_count > 0 && max_count < count) count = max_count;

    int off = 0;
    off += snprintf(buf + off, bufsz - off,
                    "{\"count\":%d,\"boot_count\":%lu,\"logs\":[",
                    count, (unsigned long)s_boot_count);

    for (int i = 0; i < count && off < bufsz - 10; i++) {
        const device_log_entry_t *e = device_log_get(i);
        if (!e) break;
        if (i > 0) off += snprintf(buf + off, bufsz - off, ",");

        // message icindeki tirnaklari escape et
        char escaped[256];
        int ei = 0;
        for (int j = 0; e->message[j] && ei < (int)sizeof(escaped) - 2; j++) {
            if (e->message[j] == '"' || e->message[j] == '\\') {
                escaped[ei++] = '\\';
            }
            if ((unsigned char)e->message[j] >= 0x20) {
                escaped[ei++] = e->message[j];
            }
        }
        escaped[ei] = '\0';

        off += snprintf(buf + off, bufsz - off,
                        "{\"ts\":%lu,\"boot\":%lu,\"type\":\"%s\",\"msg\":\"%s\"}",
                        (unsigned long)e->timestamp,
                        (unsigned long)e->boot_count,
                        log_type_str(e->type),
                        escaped);
    }

    off += snprintf(buf + off, bufsz - off, "]}");
    return off;
}
