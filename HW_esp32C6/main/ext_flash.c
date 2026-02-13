/**
 * External Flash Manager - W25Q256 Implementasyonu
 *
 * SPI2_HOST üzerinden DIO modda 20MHz hızda çalışır.
 * esp_flash API kullanır (ESP-IDF native).
 */

#include "ext_flash.h"
#include "driver/spi_master.h"
#include "esp_flash_spi_init.h"
#include "esp_log.h"

static const char *TAG = "EXT_FLASH";

static esp_flash_t *s_flash = NULL;
static uint32_t s_size = 0;
static uint32_t s_id = 0;
static bool s_initialized = false;

esp_err_t ext_flash_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "W25Q256 başlatılıyor (CS=%d MISO=%d MOSI=%d SCLK=%d)...",
             EXT_FLASH_CS_PIN, EXT_FLASH_MISO_PIN, EXT_FLASH_MOSI_PIN, EXT_FLASH_SCLK_PIN);

    // SPI bus yapılandırması
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = EXT_FLASH_MOSI_PIN,
        .miso_io_num = EXT_FLASH_MISO_PIN,
        .sclk_io_num = EXT_FLASH_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init başarısız: %s", esp_err_to_name(ret));
        return ret;
    }

    // Flash cihaz yapılandırması
    esp_flash_spi_device_config_t dev_cfg = {
        .host_id = SPI2_HOST,
        .cs_id = 0,
        .cs_io_num = EXT_FLASH_CS_PIN,
        .io_mode = SPI_FLASH_DIO,
        .freq_mhz = 20,
    };

    ret = spi_bus_add_flash_device(&s_flash, &dev_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Flash device eklenemedi: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_flash_init(s_flash);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Flash init başarısız: %s", esp_err_to_name(ret));
        return ret;
    }

    // Flash bilgilerini oku
    esp_flash_get_size(s_flash, &s_size);
    esp_flash_read_id(s_flash, &s_id);

    s_initialized = true;
    ESP_LOGI(TAG, "OK - %lu MB, ID: 0x%06lX", s_size / (1024 * 1024), s_id);

    return ESP_OK;
}

esp_err_t ext_flash_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    if (s_flash) {
        spi_bus_remove_flash_device(s_flash);
        s_flash = NULL;
    }

    spi_bus_free(SPI2_HOST);

    s_initialized = false;
    s_size = 0;
    s_id = 0;

    ESP_LOGI(TAG, "Kapatıldı");
    return ESP_OK;
}

esp_flash_t *ext_flash_get_handle(void)
{
    return s_flash;
}

uint32_t ext_flash_get_size(void)
{
    return s_size;
}

uint32_t ext_flash_get_id(void)
{
    return s_id;
}

bool ext_flash_is_ready(void)
{
    return s_initialized && s_flash != NULL;
}

esp_err_t ext_flash_erase_chip(void)
{
    if (!s_flash) return ESP_ERR_INVALID_STATE;

    ESP_LOGW(TAG, "Tüm flash siliniyor... Bu uzun sürebilir!");
    return esp_flash_erase_chip(s_flash);
}

esp_err_t ext_flash_erase_region(uint32_t address, uint32_t size)
{
    if (!s_flash) return ESP_ERR_INVALID_STATE;
    return esp_flash_erase_region(s_flash, address, size);
}

esp_err_t ext_flash_read(uint32_t address, void *buffer, uint32_t size)
{
    if (!s_flash) return ESP_ERR_INVALID_STATE;
    return esp_flash_read(s_flash, buffer, address, size);
}

esp_err_t ext_flash_write(uint32_t address, const void *buffer, uint32_t size)
{
    if (!s_flash) return ESP_ERR_INVALID_STATE;
    return esp_flash_write(s_flash, buffer, address, size);
}

void ext_flash_print_info(void)
{
    ESP_LOGI(TAG, "┌──────────────────────────────────────");

    if (s_initialized) {
        uint8_t mfr = (s_id >> 16) & 0xFF;
        const char *mfr_name = (mfr == 0xEF) ? "Winbond" : "Bilinmeyen";

        ESP_LOGI(TAG, "│ Durum:     HAZIR");
        ESP_LOGI(TAG, "│ Boyut:     %lu MB", s_size / (1024 * 1024));
        ESP_LOGI(TAG, "│ JEDEC ID:  0x%06lX (%s)", s_id, mfr_name);
        ESP_LOGI(TAG, "│ SPI:       SPI2, DIO, 20MHz");
    } else {
        ESP_LOGW(TAG, "│ Durum:     HAZIR DEĞİL");
    }

    ESP_LOGI(TAG, "└──────────────────────────────────────");
}
