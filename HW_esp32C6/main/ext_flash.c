/**
 * @file ext_flash.c
 * @brief External Flash Manager - W25Q256 Implementation
 */

#include "ext_flash.h"
#include "driver/spi_master.h"
#include "esp_flash_spi_init.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ext_flash";

static esp_flash_t *s_ext_flash = NULL;
static uint32_t s_flash_size = 0;
static uint32_t s_flash_id = 0;

esp_err_t ext_flash_init(void)
{
    if (s_ext_flash != NULL) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing W25Q256 external flash...");
    ESP_LOGI(TAG, "Pins: CS=%d, MISO=%d, MOSI=%d, SCLK=%d",
             EXT_FLASH_CS_PIN, EXT_FLASH_MISO_PIN, EXT_FLASH_MOSI_PIN, EXT_FLASH_SCLK_PIN);
    
    // SPI bus configuration
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
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Flash device configuration
    esp_flash_spi_device_config_t dev_cfg = {
        .host_id = SPI2_HOST,
        .cs_id = 0,
        .cs_io_num = EXT_FLASH_CS_PIN,
        .io_mode = SPI_FLASH_DIO,
        .freq_mhz = 20,  // Conservative start
    };
    
    ret = spi_bus_add_flash_device(&s_ext_flash, &dev_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add flash device: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_flash_init(s_ext_flash);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Flash init failed: %s", esp_err_to_name(ret));
        spi_bus_remove_flash_device(s_ext_flash);
        s_ext_flash = NULL;
        return ret;
    }
    
    // Get flash size
    ret = esp_flash_get_size(s_ext_flash, &s_flash_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not get flash size");
        s_flash_size = EXT_FLASH_SIZE_BYTES; // Assume 32MB
    }
    
    // Get flash ID
    ret = esp_flash_read_id(s_ext_flash, &s_flash_id);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not get flash ID");
        s_flash_id = 0;
    }
    
    ESP_LOGI(TAG, "External flash initialized successfully");
    ext_flash_print_info();
    
    return ESP_OK;
}

esp_flash_t* ext_flash_get_handle(void)
{
    return s_ext_flash;
}

uint32_t ext_flash_get_size(void)
{
    return s_flash_size;
}

uint32_t ext_flash_get_id(void)
{
    return s_flash_id;
}

esp_err_t ext_flash_erase_chip(void)
{
    if (!s_ext_flash) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "Erasing entire flash... This may take 1-2 minutes!");
    return esp_flash_erase_chip(s_ext_flash);
}

esp_err_t ext_flash_erase_sector(uint32_t address)
{
    if (!s_ext_flash) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_flash_erase_region(s_ext_flash, address, 4096);
}

esp_err_t ext_flash_read(uint32_t address, void *buffer, uint32_t size)
{
    if (!s_ext_flash) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_flash_read(s_ext_flash, buffer, address, size);
}

esp_err_t ext_flash_write(uint32_t address, const void *buffer, uint32_t size)
{
    if (!s_ext_flash) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_flash_write(s_ext_flash, buffer, address, size);
}

bool ext_flash_is_ready(void)
{
    return s_ext_flash != NULL;
}

void ext_flash_print_info(void)
{
    ESP_LOGI(TAG, "========== EXTERNAL FLASH INFO ==========");
    
    if (s_ext_flash) {
        ESP_LOGI(TAG, "Status:      READY");
        ESP_LOGI(TAG, "Size:        %lu bytes (%lu MB)", s_flash_size, s_flash_size / (1024 * 1024));
        ESP_LOGI(TAG, "Flash ID:    0x%06lX", s_flash_id);
        
        uint8_t mfr = (s_flash_id >> 16) & 0xFF;
        const char *mfr_name = (mfr == 0xEF) ? "Winbond" : "Unknown";
        ESP_LOGI(TAG, "Manufacturer: %s (0x%02X)", mfr_name, mfr);
    } else {
        ESP_LOGW(TAG, "Status:      NOT READY");
    }
    
    ESP_LOGI(TAG, "==========================================");
}
