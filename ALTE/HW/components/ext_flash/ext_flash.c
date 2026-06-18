/**
 * LebensSpur - Harici Flash Modulu
 * W25Q256 (32MB) SPI Flash - 3 Partition Yapisi
 *
 * Partition Haritasi:
 *   Slot A    : 0x000000 - 0x1FFFFF  (2 MB)  - GUI Web Arayuzu (LittleFS)
 *   Slot B    : 0x200000 - 0x3FFFFF  (2 MB)  - Yedek / OTA slot (rezerve)
 *   User Data : 0x400000 - 0x1FFFFFF (28 MB)  - Kullanici verileri (LittleFS)
 *
 * Pin Baglantilari:
 *   CS   = D3 = GPIO21
 *   MISO = D0 = GPIO0
 *   MOSI = D4 = GPIO22
 *   SCLK = D6 = GPIO16
 */

#include "ext_flash.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "driver/spi_common.h"
#include "esp_flash.h"
#include "esp_flash_spi_init.h"
#include "nvs_flash.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

static const char *TAG = "EXT_FLASH";

// SPI Pin tanimlamalari
#define EXT_FLASH_SPI_HOST   SPI2_HOST
#define PIN_CS               21   // D3 = GPIO21
#define PIN_MISO             0    // D0 = GPIO0
#define PIN_MOSI             22   // D4 = GPIO22
#define PIN_SCLK             16   // D6 = GPIO16

// ============================================================
// Partition Haritasi (32MB = 0x2000000)
// ============================================================
#define SLOT_A_OFFSET        0x000000
#define SLOT_A_SIZE          (2 * 1024 * 1024)    // 2 MB
#define SLOT_A_LABEL         "slot_a"
#define SLOT_A_MOUNT         "/slot_a"

#define SLOT_B_OFFSET        0x200000
#define SLOT_B_SIZE          (2 * 1024 * 1024)    // 2 MB
#define SLOT_B_LABEL         "slot_b"
#define SLOT_B_MOUNT         "/slot_b"

#define USER_DATA_OFFSET     0x400000
#define USER_DATA_SIZE       (28 * 1024 * 1024)   // 28 MB
#define USER_DATA_LABEL      "user_data"
#define USER_DATA_MOUNT      "/ext"

static esp_flash_t *s_ext_flash = NULL;
static bool s_slot_a_mounted = false;
static bool s_slot_b_mounted = false;
static bool s_user_data_mounted = false;
static char s_active_slot = 'a';  // NVS'den yuklenir

#define NVS_GUI_OTA_NS "ls_gui_ota"

// NVS'den aktif slot bilgisini yukle
static void load_active_slot(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_GUI_OTA_NS, NVS_READONLY, &nvs) == ESP_OK) {
        char slot[4] = {0};
        size_t len = sizeof(slot);
        if (nvs_get_str(nvs, "active_slot", slot, &len) == ESP_OK) {
            if (slot[0] == 'b') {
                s_active_slot = 'b';
            }
        }
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "Aktif GUI slot: %c", s_active_slot);
}

esp_err_t ext_flash_init(void)
{
    ESP_LOGI(TAG, "Harici flash baslatiliyor...");
    ESP_LOGI(TAG, "Pinler: CS=GPIO%d, MISO=GPIO%d, MOSI=GPIO%d, SCLK=GPIO%d",
             PIN_CS, PIN_MISO, PIN_MOSI, PIN_SCLK);

    // SPI bus konfigurasyonu
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64 * 1024,
    };

    esp_err_t ret = spi_bus_initialize(EXT_FLASH_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus baslatilamadi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Harici flash chip konfigurasyonu
    esp_flash_spi_device_config_t dev_cfg = {
        .host_id = EXT_FLASH_SPI_HOST,
        .cs_io_num = PIN_CS,
        .io_mode = SPI_FLASH_DIO,
        .freq_mhz = 40,
        .cs_id = 0,
    };

    ret = spi_bus_add_flash_device(&s_ext_flash, &dev_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Flash cihazi eklenemedi: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_flash_init(s_ext_flash);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Flash init hatasi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Flash boyutunu kontrol et
    uint32_t flash_size;
    ret = esp_flash_get_size(s_ext_flash, &flash_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Flash boyutu alinamadi: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Harici flash boyutu: %lu MB", (unsigned long)(flash_size / (1024 * 1024)));

    // ============================================================
    // 3 Partition Kaydi
    // ============================================================
    ESP_LOGI(TAG, "Partition haritasi:");
    ESP_LOGI(TAG, "  Slot A    : 0x%06X - 0x%06X (%d MB) - GUI Web",
             SLOT_A_OFFSET, SLOT_A_OFFSET + SLOT_A_SIZE - 1, SLOT_A_SIZE / (1024 * 1024));
    ESP_LOGI(TAG, "  Slot B    : 0x%06X - 0x%06X (%d MB) - Rezerve",
             SLOT_B_OFFSET, SLOT_B_OFFSET + SLOT_B_SIZE - 1, SLOT_B_SIZE / (1024 * 1024));
    ESP_LOGI(TAG, "  User Data : 0x%06X - 0x%06X (%d MB) - Kullanici",
             USER_DATA_OFFSET, USER_DATA_OFFSET + USER_DATA_SIZE - 1, USER_DATA_SIZE / (1024 * 1024));

    // Slot A partition (GUI Web Arayuzu)
    const esp_partition_t *slot_a_part;
    ret = esp_partition_register_external(s_ext_flash, SLOT_A_OFFSET, SLOT_A_SIZE,
                                          SLOT_A_LABEL, ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_ANY, &slot_a_part);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Slot A partition kaydi hatasi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Slot B partition (Rezerve - sadece kayit, mount yok)
    const esp_partition_t *slot_b_part;
    ret = esp_partition_register_external(s_ext_flash, SLOT_B_OFFSET, SLOT_B_SIZE,
                                          SLOT_B_LABEL, ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_ANY, &slot_b_part);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Slot B partition kaydi hatasi: %s", esp_err_to_name(ret));
        return ret;
    }

    // User Data partition (Kullanici verileri)
    const esp_partition_t *user_data_part;
    ret = esp_partition_register_external(s_ext_flash, USER_DATA_OFFSET, USER_DATA_SIZE,
                                          USER_DATA_LABEL, ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_ANY, &user_data_part);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "User Data partition kaydi hatasi: %s", esp_err_to_name(ret));
        return ret;
    }

    // ============================================================
    // LittleFS Mount - Slot A (GUI)
    // ============================================================
    esp_vfs_littlefs_conf_t slot_a_conf = {
        .base_path = SLOT_A_MOUNT,
        .partition_label = SLOT_A_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    ret = esp_vfs_littlefs_register(&slot_a_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Slot A LittleFS mount hatasi: %s", esp_err_to_name(ret));
    } else {
        s_slot_a_mounted = true;
        ESP_LOGI(TAG, "Slot A mount edildi: %s (LittleFS, 2MB, GUI)", SLOT_A_MOUNT);
    }

    // ============================================================
    // LittleFS Mount - Slot B (GUI OTA)
    // ============================================================
    esp_vfs_littlefs_conf_t slot_b_conf = {
        .base_path = SLOT_B_MOUNT,
        .partition_label = SLOT_B_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    ret = esp_vfs_littlefs_register(&slot_b_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Slot B LittleFS mount hatasi: %s", esp_err_to_name(ret));
    } else {
        s_slot_b_mounted = true;
        ESP_LOGI(TAG, "Slot B mount edildi: %s (LittleFS, 2MB, OTA)", SLOT_B_MOUNT);
    }

    // ============================================================
    // LittleFS Mount - User Data
    // ============================================================
    esp_vfs_littlefs_conf_t user_data_conf = {
        .base_path = USER_DATA_MOUNT,
        .partition_label = USER_DATA_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    ret = esp_vfs_littlefs_register(&user_data_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "User Data LittleFS mount hatasi: %s", esp_err_to_name(ret));
    } else {
        s_user_data_mounted = true;
        ESP_LOGI(TAG, "User Data mount edildi: %s (LittleFS, 28MB)", USER_DATA_MOUNT);
    }

    if (!s_slot_a_mounted && !s_user_data_mounted) {
        return ESP_FAIL;
    }

    // NVS'den aktif slot bilgisini yukle
    load_active_slot();

    return ESP_OK;
}

// ============================================================
// Slot A (GUI) dosya islemleri - /slot_a mount noktasi
// ============================================================

int ext_flash_slot_a_read_file(const char *path, char *buf, size_t buf_size)
{
    if (!s_slot_a_mounted || !path || !buf) {
        return -1;
    }

    char full_path[128];
    if (path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", SLOT_A_MOUNT, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", SLOT_A_MOUNT, path);
    }

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Slot A dosya acilamadi: %s", full_path);
        return -1;
    }

    int bytes_read = fread(buf, 1, buf_size - 1, f);
    buf[bytes_read] = '\0';
    fclose(f);

    ESP_LOGD(TAG, "Slot A dosya okundu: %s (%d byte)", full_path, bytes_read);
    return bytes_read;
}

long ext_flash_slot_a_get_file_size(const char *path)
{
    if (!s_slot_a_mounted || !path) return -1;

    char full_path[128];
    if (path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", SLOT_A_MOUNT, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", SLOT_A_MOUNT, path);
    }

    struct stat st;
    if (stat(full_path, &st) != 0) return -1;
    return (long)st.st_size;
}

int ext_flash_slot_a_read_file_chunk(const char *path, char *buf, size_t buf_size, long offset)
{
    if (!s_slot_a_mounted || !path || !buf) return -1;

    char full_path[128];
    if (path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", SLOT_A_MOUNT, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", SLOT_A_MOUNT, path);
    }

    FILE *f = fopen(full_path, "rb");
    if (!f) return -1;

    if (offset > 0) {
        fseek(f, offset, SEEK_SET);
    }

    int bytes_read = fread(buf, 1, buf_size, f);
    fclose(f);
    return bytes_read;
}

// ============================================================
// Slot A dosya handle islemleri (tek fopen/fclose ile tum chunklari oku)
// ============================================================

struct ext_flash_file {
    FILE *fp;
    long size;
};

ext_flash_file_t ext_flash_slot_a_fopen(const char *path, long *out_size)
{
    if (!s_slot_a_mounted || !path) return NULL;

    char full_path[128];
    if (path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", SLOT_A_MOUNT, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", SLOT_A_MOUNT, path);
    }

    FILE *fp = fopen(full_path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    struct ext_flash_file *f = malloc(sizeof(struct ext_flash_file));
    if (!f) {
        fclose(fp);
        return NULL;
    }
    f->fp = fp;
    f->size = size;

    if (out_size) *out_size = size;
    return f;
}

int ext_flash_slot_a_fread(ext_flash_file_t f, char *buf, size_t buf_size)
{
    if (!f || !f->fp || !buf) return -1;
    return (int)fread(buf, 1, buf_size, f->fp);
}

void ext_flash_slot_a_fclose(ext_flash_file_t f)
{
    if (!f) return;
    if (f->fp) fclose(f->fp);
    free(f);
}

// ============================================================
// User Data dosya islemleri - /ext mount noktasi (geriye uyumlu)
// ============================================================

int ext_flash_read_file(const char *path, char *buf, size_t buf_size)
{
    if (!s_user_data_mounted || !path || !buf) {
        return -1;
    }

    char full_path[128];
    if (path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", USER_DATA_MOUNT, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", USER_DATA_MOUNT, path);
    }

    FILE *f = fopen(full_path, "r");
    if (!f) {
        ESP_LOGW(TAG, "Dosya acilamadi: %s", full_path);
        return -1;
    }

    int bytes_read = fread(buf, 1, buf_size - 1, f);
    buf[bytes_read] = '\0';
    fclose(f);

    ESP_LOGD(TAG, "Dosya okundu: %s (%d byte)", full_path, bytes_read);
    return bytes_read;
}

esp_err_t ext_flash_write_file(const char *path, const char *data, size_t len)
{
    if (!s_user_data_mounted || !path || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[128];
    if (path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", USER_DATA_MOUNT, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", USER_DATA_MOUNT, path);
    }

    FILE *f = fopen(full_path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Dosya yazilamadi: %s", full_path);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, len, f);
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "Yazma hatasi: %zu/%zu byte", written, len);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Dosya yazildi: %s (%zu byte)", full_path, written);
    return ESP_OK;
}

esp_err_t ext_flash_delete_file(const char *path)
{
    if (!s_user_data_mounted || !path) {
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[128];
    if (path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", USER_DATA_MOUNT, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", USER_DATA_MOUNT, path);
    }

    if (remove(full_path) != 0) {
        ESP_LOGE(TAG, "Dosya silinemedi: %s", full_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Dosya silindi: %s", full_path);
    return ESP_OK;
}

// ============================================================
// Aktif Slot Yonetimi
// ============================================================

char ext_flash_get_active_slot(void)
{
    return s_active_slot;
}

esp_err_t ext_flash_set_active_slot(char slot)
{
    if (slot != 'a' && slot != 'b') return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_GUI_OTA_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    char val[2] = { slot, '\0' };
    err = nvs_set_str(nvs, "active_slot", val);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        s_active_slot = slot;
        ESP_LOGI(TAG, "Aktif slot degistirildi: %c", slot);
    }
    return err;
}

const char *ext_flash_get_active_mount(void)
{
    return (s_active_slot == 'b') ? SLOT_B_MOUNT : SLOT_A_MOUNT;
}

const char *ext_flash_get_inactive_mount(void)
{
    return (s_active_slot == 'b') ? SLOT_A_MOUNT : SLOT_B_MOUNT;
}

// ============================================================
// Aktif slot dosya islemleri (hangi slot aktifse oradan oku)
// ============================================================

ext_flash_file_t ext_flash_active_fopen(const char *path, long *out_size)
{
    const char *mount = ext_flash_get_active_mount();
    bool mounted = (s_active_slot == 'b') ? s_slot_b_mounted : s_slot_a_mounted;
    if (!mounted || !path) return NULL;

    char full_path[128];
    if (path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", mount, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", mount, path);
    }

    FILE *fp = fopen(full_path, "rb");
    if (!fp) {
        // Aktif slot'ta yoksa fallback: diger slot'u dene
        return ext_flash_slot_a_fopen(path, out_size);
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    struct ext_flash_file *f = malloc(sizeof(struct ext_flash_file));
    if (!f) {
        fclose(fp);
        return NULL;
    }
    f->fp = fp;
    f->size = size;

    if (out_size) *out_size = size;
    return f;
}

int ext_flash_active_fread(ext_flash_file_t f, char *buf, size_t buf_size)
{
    return ext_flash_slot_a_fread(f, buf, buf_size); // ayni struct, ayni okuma
}

void ext_flash_active_fclose(ext_flash_file_t f)
{
    ext_flash_slot_a_fclose(f); // ayni struct, ayni kapatma
}

long ext_flash_active_get_file_size(const char *path)
{
    const char *mount = ext_flash_get_active_mount();
    bool mounted = (s_active_slot == 'b') ? s_slot_b_mounted : s_slot_a_mounted;
    if (!mounted || !path) return -1;

    char full_path[128];
    if (path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", mount, path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", mount, path);
    }

    struct stat st;
    if (stat(full_path, &st) != 0) {
        // Fallback: Slot A
        return ext_flash_slot_a_get_file_size(path);
    }
    return (long)st.st_size;
}

// ============================================================
// Durum sorgulama
// ============================================================

bool ext_flash_is_mounted(void)
{
    return s_slot_a_mounted || s_user_data_mounted;
}

bool ext_flash_is_slot_a_mounted(void)
{
    return s_slot_a_mounted;
}

bool ext_flash_is_slot_b_mounted(void)
{
    return s_slot_b_mounted;
}

bool ext_flash_is_user_data_mounted(void)
{
    return s_user_data_mounted;
}

esp_err_t ext_flash_get_info(size_t *total, size_t *free_space)
{
    if (!s_user_data_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t total_bytes = 0, used_bytes = 0;
    esp_err_t ret = esp_littlefs_info(USER_DATA_LABEL, &total_bytes, &used_bytes);
    if (ret != ESP_OK) {
        return ret;
    }

    if (total) *total = total_bytes;
    if (free_space) *free_space = total_bytes - used_bytes;

    return ESP_OK;
}

esp_err_t ext_flash_get_slot_a_info(size_t *total, size_t *free_space)
{
    if (!s_slot_a_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t total_bytes = 0, used_bytes = 0;
    esp_err_t ret = esp_littlefs_info(SLOT_A_LABEL, &total_bytes, &used_bytes);
    if (ret != ESP_OK) {
        return ret;
    }

    if (total) *total = total_bytes;
    if (free_space) *free_space = total_bytes - used_bytes;

    return ESP_OK;
}

esp_err_t ext_flash_get_slot_b_info(size_t *total, size_t *free_space)
{
    if (!s_slot_b_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t total_bytes = 0, used_bytes = 0;
    esp_err_t ret = esp_littlefs_info(SLOT_B_LABEL, &total_bytes, &used_bytes);
    if (ret != ESP_OK) {
        return ret;
    }

    if (total) *total = total_bytes;
    if (free_space) *free_space = total_bytes - used_bytes;

    return ESP_OK;
}

// ============================================================
// GUI dosyalari (LS/GUI/ klasorunden embed edilir)
// CMake EMBED_TXTFILES ile derleme sirasinda binary'ye gomulur
// Slot A'ya yazilir
// ============================================================

extern const char gui_index_html_start[] asm("_binary_index_html_start");
extern const char gui_index_html_end[]   asm("_binary_index_html_end");
extern const char gui_style_css_start[]  asm("_binary_style_css_start");
extern const char gui_style_css_end[]    asm("_binary_style_css_end");
extern const char gui_app_js_start[]     asm("_binary_app_js_start");
extern const char gui_app_js_end[]       asm("_binary_app_js_end");
extern const char gui_config_json_start[] asm("_binary_config_json_start");
extern const char gui_config_json_end[]   asm("_binary_config_json_end");
extern const char gui_logo_png_start[]      asm("_binary_logo_png_start");
extern const char gui_logo_png_end[]        asm("_binary_logo_png_end");
extern const char gui_darklogo_png_start[]  asm("_binary_darklogo_png_start");
extern const char gui_darklogo_png_end[]    asm("_binary_darklogo_png_end");
extern const char gui_lang_en_json_start[]  asm("_binary_en_json_start");
extern const char gui_lang_en_json_end[]    asm("_binary_en_json_end");
extern const char gui_lang_de_json_start[]  asm("_binary_de_json_start");
extern const char gui_lang_de_json_end[]    asm("_binary_de_json_end");
extern const char gui_lang_es_json_start[]  asm("_binary_es_json_start");
extern const char gui_lang_es_json_end[]    asm("_binary_es_json_end");
extern const char gui_lang_fr_json_start[]  asm("_binary_fr_json_start");
extern const char gui_lang_fr_json_end[]    asm("_binary_fr_json_end");
extern const char gui_lang_it_json_start[]  asm("_binary_it_json_start");
extern const char gui_lang_it_json_end[]    asm("_binary_it_json_end");
extern const char gui_lang_tr_json_start[]  asm("_binary_tr_json_start");
extern const char gui_lang_tr_json_end[]    asm("_binary_tr_json_end");

#define WEB_FILES_VERSION "0.0.69"

static bool file_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0);
}

// Dizin ve icindeki tum dosyalari recursive sil
static void delete_dir_contents(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    char path[320];

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                delete_dir_contents(path);
                rmdir(path);
                ESP_LOGI(TAG, "  Dizin silindi: %s", path);
            } else {
                remove(path);
                ESP_LOGI(TAG, "  Dosya silindi: %s", path);
            }
        }
    }

    closedir(dir);
}

static void write_file_to_slot_a(const char *name, const char *content, size_t len)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", SLOT_A_MOUNT, name);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "  [HATA] Dosya olusturulamadi: %s (errno=%d)", name, errno);
        return;
    }
    size_t written = fwrite(content, 1, len, f);
    fclose(f);

    if (written != len) {
        ESP_LOGE(TAG, "  [HATA] Yazma eksik: %s (%zu/%zu byte)", name, written, len);
        return;
    }

    // Dogrulama: dosyayi geri oku ve boyutunu kontrol et
    f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "  [HATA] Dogrulama: dosya acilamadi: %s", name);
        return;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fclose(f);

    if ((size_t)file_size != len) {
        ESP_LOGE(TAG, "  [HATA] Dogrulama: boyut uyumsuz: %s (yazilan=%zu, diskteki=%ld)", name, len, file_size);
    } else {
        ESP_LOGI(TAG, "  [OK] %s (%zu byte) -> Slot A", name, len);
    }
}

static void ensure_dir_slot_a(const char *dir)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", SLOT_A_MOUNT, dir);
    mkdir(path, 0755);
}

// Versiyon kontrolu: Slot A'daki config.json icindeki version ile karsilastir
static bool needs_update(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/config.json", SLOT_A_MOUNT);

    if (!file_exists(path)) {
        ESP_LOGI(TAG, "config.json bulunamadi, guncelleme gerekli");
        return true;
    }

    char buf[256];
    FILE *f = fopen(path, "r");
    if (!f) return true;
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    buf[len] = '\0';
    fclose(f);

    ESP_LOGI(TAG, "Slot A config.json: %s", buf);
    ESP_LOGI(TAG, "Beklenen versiyon: " WEB_FILES_VERSION);

    // Versiyonu ara (bosluklu veya bosluksuz formati destekle)
    char *ver = strstr(buf, WEB_FILES_VERSION);
    if (ver == NULL) {
        ESP_LOGI(TAG, "Versiyon uyusmuyor, guncelleme gerekli");
        return true;
    }

    ESP_LOGI(TAG, "Versiyon eslesti, guncelleme gerekli degil");
    return false;
}

esp_err_t ext_flash_format_user_data(void)
{
    if (!s_user_data_mounted) {
        ESP_LOGE(TAG, "User Data mount degil, format yapilamaz");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "User Data partition formatlaniyor...");
    delete_dir_contents(USER_DATA_MOUNT);
    ESP_LOGI(TAG, "User Data partition formatlanidi");
    return ESP_OK;
}

esp_err_t ext_flash_create_default_files(void)
{
    if (!s_slot_a_mounted) {
        ESP_LOGE(TAG, "Slot A mount degil, GUI dosyalari yazilamaz");
        return ESP_ERR_INVALID_STATE;
    }

    if (!needs_update()) {
        ESP_LOGI(TAG, "Web dosyalari guncel (v" WEB_FILES_VERSION ") [Slot A]");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Web dosyalari Slot A'ya yaziliyor (v" WEB_FILES_VERSION ")...");

    // Onceki tum dosyalari sil
    ESP_LOGI(TAG, "Slot A eski dosyalar siliniyor...");
    delete_dir_contents(SLOT_A_MOUNT);

    // Yeni dosyalari Slot A'ya yaz
    write_file_to_slot_a("index.html", gui_index_html_start, strlen(gui_index_html_start));
    write_file_to_slot_a("style.css",  gui_style_css_start,  strlen(gui_style_css_start));
    write_file_to_slot_a("app.js",     gui_app_js_start,     strlen(gui_app_js_start));
    write_file_to_slot_a("config.json", gui_config_json_start, strlen(gui_config_json_start));

    // EMBED_FILES (binary) - null terminator yok, end-start dogru
    ensure_dir_slot_a("pic");
    write_file_to_slot_a("pic/logo.png", gui_logo_png_start, gui_logo_png_end - gui_logo_png_start);
    write_file_to_slot_a("pic/darklogo.png", gui_darklogo_png_start, gui_darklogo_png_end - gui_darklogo_png_start);

    // Dil dosyalari
    ensure_dir_slot_a("lang");
    write_file_to_slot_a("lang/en.json", gui_lang_en_json_start, strlen(gui_lang_en_json_start));
    write_file_to_slot_a("lang/de.json", gui_lang_de_json_start, strlen(gui_lang_de_json_start));
    write_file_to_slot_a("lang/es.json", gui_lang_es_json_start, strlen(gui_lang_es_json_start));
    write_file_to_slot_a("lang/fr.json", gui_lang_fr_json_start, strlen(gui_lang_fr_json_start));
    write_file_to_slot_a("lang/it.json", gui_lang_it_json_start, strlen(gui_lang_it_json_start));
    write_file_to_slot_a("lang/tr.json", gui_lang_tr_json_start, strlen(gui_lang_tr_json_start));

    ESP_LOGI(TAG, "Web dosyalari Slot A'ya yazildi");

    // Firmware guncelleme sonrasi: Slot A yeniden yazildiysa aktif slot'u A'ya zorla
    if (s_active_slot != 'a') {
        ESP_LOGW(TAG, "Slot A yeniden yazildi, aktif slot A'ya zorlaniyor");
        ext_flash_set_active_slot('a');
    }

    return ESP_OK;
}
