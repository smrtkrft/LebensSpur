/*
 * storage — harici SPI flash (CS 21 / MISO 0 / MOSI 22 / SCLK 16), SPI2, DMA kapalı, DIO 40 MHz.
 * Karar ve gerekçeler: docs/03-arastirma/04-depolama.md
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/spi_common.h"
#include "esp_flash.h"
#include "esp_flash_spi_init.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "storage.h"

static const char *TAG = "storage";

#define HOST       SPI2_HOST
#define PIN_CS     21
#define PIN_MISO   0
#define PIN_MOSI   22
#define PIN_SCLK   16
#define LOG_SIZE   (1024u * 1024u)
#define MIN_CHIP   (4u * 1024u * 1024u)
#define MAX_CHIP   (32u * 1024u * 1024u)
#define MARKER     "/.ls2611"
#define BLANK_PROBE 8192u
#define ERASE_STEP (64u * 1024u)

typedef struct {
    const char *mount, *label;
    const esp_partition_t *part;
    storage_state_t state;
    int users;
} vol_t;

static vol_t s_vol[2] = {
    { STG_LOG, "lslog", NULL, STG_ABSENT, 0 },
    { STG_DATA, "lsdata", NULL, STG_ABSENT, 0 },
};
static esp_flash_t *s_chip;
static uint32_t s_chip_size;
static SemaphoreHandle_t s_lock;
/* Çip kilidi: /log ve /data aynı SPI flash çipinde. İki bölüme eşzamanlı erişim (ör. sys_worker /log'a
 * yazarken httpd /data'da opendir) ~15 sn takılmaya yol açtı (2026-11-05, cihazda tekrarlandı).
 * Her dosya işlemi storage_acquire..release arasında bu özyinelemeli kilidi tutar → erişimler sıralı. */
static SemaphoreHandle_t s_chip_lock;
static char s_note[96];

static vol_t *vol(const char *mount)
{
    for (int i = 0; i < 2; i++) {
        if (mount && strcmp(mount, s_vol[i].mount) == 0) return &s_vol[i];
    }
    return NULL;
}

static void note(const char *fmt, const char *a, const char *b)
{
    size_t n = strlen(s_note);
    if (n) n += (size_t)snprintf(s_note + n, sizeof s_note - n, "; ");
    if (n < sizeof s_note) snprintf(s_note + n, sizeof s_note - n, fmt, a, b);
}

static esp_err_t vfs_mount(vol_t *v, bool format_if_fail)
{
    esp_vfs_littlefs_conf_t c = { .base_path = v->mount, .partition_label = v->label,
                                  .format_if_mount_failed = format_if_fail };
    return esp_vfs_littlefs_register(&c);
}

static bool marker_present(vol_t *v)
{
    char p[24];
    snprintf(p, sizeof p, "%s%s", v->mount, MARKER);
    struct stat st;
    return stat(p, &st) == 0;
}

static esp_err_t marker_write(vol_t *v)
{
    char p[24];
    snprintf(p, sizeof p, "%s%s", v->mount, MARKER);
    FILE *f = fopen(p, "w");
    if (!f) return ESP_FAIL;
    fputs("2611\n", f);
    fclose(f);
    return ESP_OK;
}

static bool region_blank(const esp_partition_t *p)
{
    static uint8_t buf[512];
    for (uint32_t off = 0; off < BLANK_PROBE; off += sizeof buf) {
        if (esp_partition_read(p, off, buf, sizeof buf) != ESP_OK) return false;
        for (size_t i = 0; i < sizeof buf; i++) {
            if (buf[i] != 0xFF) return false;
        }
    }
    return true;
}

/* biçimle + bağla + işaret; çağıran kilidi tutar */
static esp_err_t format_locked(vol_t *v)
{
    if (esp_littlefs_mounted(v->label)) esp_vfs_littlefs_unregister(v->label);
    esp_err_t r = esp_littlefs_format(v->label);
    if (r == ESP_OK) r = vfs_mount(v, false);
    if (r == ESP_OK) r = marker_write(v);
    v->state = r == ESP_OK ? STG_MOUNTED : STG_ERROR;
    return r;
}

static void mount_volume(vol_t *v)
{
    bool is_log = v == &s_vol[0];
    esp_err_t r = vfs_mount(v, false);
    if (r != ESP_OK) {
        if (is_log || region_blank(v->part)) {
            note("%s: %s -> bicimlendi", v->mount, is_log ? "baglanamadi" : "bos");
            format_locked(v);
        } else {
            note("%s: %s", v->mount, "bozuk, bicimlenmedi (ERROR)");
            v->state = STG_ERROR;
        }
        return;
    }
    if (!marker_present(v)) {
        /* başka düzenden kalma dosya sistemi (ALTE slotu) ya da yeni biçimlenmiş: 2611 verisi yok */
        note("%s: %s -> bicimlendi", v->mount, "duzen isareti yok");
        format_locked(v);
        return;
    }
    v->state = STG_MOUNTED;
}

esp_err_t storage_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_chip_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock || !s_chip_lock) return ESP_ERR_NO_MEM;
    spi_bus_config_t bus = { .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO, .sclk_io_num = PIN_SCLK,
                             .quadwp_io_num = -1, .quadhd_io_num = -1 };
    esp_err_t r = spi_bus_initialize(HOST, &bus, SPI_DMA_DISABLED);
    if (r != ESP_OK) return r;
    esp_flash_spi_device_config_t dev = { .host_id = HOST, .cs_io_num = PIN_CS, .io_mode = SPI_FLASH_DIO,
                                          .freq_mhz = 40, .cs_id = 0 };
    r = spi_bus_add_flash_device(&s_chip, &dev);
    if (r == ESP_OK) r = esp_flash_init(s_chip);
    if (r == ESP_OK) r = esp_flash_get_size(s_chip, &s_chip_size);
    if (r != ESP_OK) {
        note("%s %s", "harici flash yok:", esp_err_to_name(r));
        return r;
    }
    if (s_chip_size < MIN_CHIP || s_chip_size > MAX_CHIP) {
        note("%s %s", "harici flash boyutu desteklenmiyor", "");
        return ESP_ERR_INVALID_SIZE;
    }
    r = esp_partition_register_external(s_chip, 0, LOG_SIZE, s_vol[0].label, ESP_PARTITION_TYPE_DATA,
                                        ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, &s_vol[0].part);
    if (r == ESP_OK) {
        r = esp_partition_register_external(s_chip, LOG_SIZE, s_chip_size - LOG_SIZE, s_vol[1].label,
                                            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,
                                            &s_vol[1].part);
    }
    if (r != ESP_OK) return r;
    for (int i = 0; i < 2; i++) mount_volume(&s_vol[i]);
    ESP_LOGI(TAG, "cip %lu MB; /log %s, /data %s", (unsigned long)(s_chip_size >> 20),
             storage_state_name(s_vol[0].state), storage_state_name(s_vol[1].state));
    return ESP_OK;
}

storage_state_t storage_state(const char *mount)
{
    vol_t *v = vol(mount);
    return v ? v->state : STG_ABSENT;
}

const char *storage_state_name(storage_state_t s)
{
    static const char *const n[] = { "absent", "mounted", "error", "formatting" };
    return (unsigned)s < 4 ? n[s] : "error";
}

bool storage_acquire(const char *mount)
{
    vol_t *v = vol(mount);
    if (!v || !s_lock) return false;
    xSemaphoreTakeRecursive(s_chip_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = v->state == STG_MOUNTED;
    if (ok) v->users++;
    xSemaphoreGive(s_lock);
    if (!ok) xSemaphoreGiveRecursive(s_chip_lock);
    return ok;
}

void storage_release(const char *mount)
{
    vol_t *v = vol(mount);
    if (!v || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (v->users > 0) v->users--;
    xSemaphoreGive(s_lock);
    xSemaphoreGiveRecursive(s_chip_lock);
}

/* FORMATTING'e geçip kullanıcıların bitmesini bekler; kilit alınmış dönerse ESP_OK */
static esp_err_t begin_exclusive(vol_t *v)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (v->state == STG_ABSENT || v->state == STG_FORMATTING) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    v->state = STG_FORMATTING;
    for (int i = 0; i < 50 && v->users > 0; i++) {
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(100));
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (v->users > 0) {
        v->state = STG_MOUNTED;
        xSemaphoreGive(s_lock);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t storage_format(const char *mount)
{
    vol_t *v = vol(mount);
    if (!v || !v->part) return ESP_ERR_INVALID_ARG;
    xSemaphoreTakeRecursive(s_chip_lock, portMAX_DELAY);
    esp_err_t r = begin_exclusive(v);
    if (r == ESP_OK) {
        r = format_locked(v);
        xSemaphoreGive(s_lock);
    }
    xSemaphoreGiveRecursive(s_chip_lock);
    return r;
}

esp_err_t storage_erase_data_full(void (*progress)(int pct))
{
    vol_t *v = &s_vol[1];
    if (!v->part) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_chip_lock, portMAX_DELAY);
    esp_err_t r = begin_exclusive(v);
    if (r != ESP_OK) { xSemaphoreGiveRecursive(s_chip_lock); return r; }
    if (esp_littlefs_mounted(v->label)) esp_vfs_littlefs_unregister(v->label);
    uint32_t size = (uint32_t)v->part->size;
    for (uint32_t off = 0; off < size && r == ESP_OK; off += ERASE_STEP) {
        r = esp_partition_erase_range(v->part, off, ERASE_STEP);
        if (progress) progress((int)((uint64_t)(off + ERASE_STEP) * 100u / size));
    }
    if (r == ESP_OK) r = format_locked(v);
    else v->state = STG_ERROR;
    xSemaphoreGive(s_lock);
    xSemaphoreGiveRecursive(s_chip_lock);
    return r;
}

esp_err_t storage_info(const char *mount, size_t *total, size_t *used)
{
    if (!storage_acquire(mount)) return ESP_ERR_INVALID_STATE;
    esp_err_t r = esp_littlefs_info(vol(mount)->label, total, used);
    storage_release(mount);
    return r;
}

uint32_t storage_chip_size(void) { return s_chip_size; }
const char *storage_note(void) { return s_note[0] ? s_note : NULL; }
