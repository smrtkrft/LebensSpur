/**
 * LebensSpur - Ana Program
 * ESP32-C6 | Dahili 4MB Flash + Harici 32MB Flash
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_mac.h"

#include "wifi_manager.h"
#include "web_server.h"
#include "ext_flash.h"
#include "flash_sync.h"
#include "timer_manager.h"
#include "smtp_client.h"
#include "device_log.h"
#include "output.h"
#include "telegram_client.h"
#include "gui_ota.h"
#include "fw_ota.h"
#include "esp_ota_ops.h"

static const char *TAG = "MAIN";
static char device_name[20] = {0};

/* Buton callback - timer'i yeniden baslat */
static void on_button_pressed(void)
{
    ESP_LOGI(TAG, "Buton basildi - timer restart");

    /* Relais aciksa kapat */
    if (output_relay_is_on()) {
        output_relay_off();
    }

    esp_err_t err = timer_manager_restart();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Timer restart basarisiz: %s", esp_err_to_name(err));
        device_log_add(LOG_TYPE_ERROR, "Timer restart basarisiz: %s", esp_err_to_name(err));
    }

    /* Web arayuzune anlik bildirim */
    web_server_send_timer_event();
}

/* Factory reset callback - butonla 5x basma */
static void on_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory Reset (buton 5x)");
    nvs_flash_erase();
    ext_flash_format_user_data();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* Alarm tetiklenince - erken uyari mail gonder (ayri task) */
static void alarm_mail_task(void *arg)
{
    int alarm_index = (int)(intptr_t)arg;
    ESP_LOGI(TAG, "Erken uyari mail gonderiliyor (alarm #%d)", alarm_index);

    // Action aktif mi kontrol et
    nvs_handle_t nvs;
    uint8_t early_mail_active = 0;
    if (nvs_open("ls_actions", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "early_mail", &early_mail_active);
        nvs_close(nvs);
    }
    if (!early_mail_active) {
        ESP_LOGI(TAG, "Erken uyari mail pasif, atlanıyor");
        vTaskDelete(NULL);
        return;
    }

    // SMTP ayarli mi kontrol et
    if (!smtp_client_has_config()) {
        ESP_LOGW(TAG, "SMTP yapilandirilmamis, erken uyari mail gonderilemedi");
        vTaskDelete(NULL);
        return;
    }

    // Erken uyari mail config'ini NVS'den oku
    char email[64] = {0}, subject[64] = {0}, body[256] = {0};
    if (nvs_open("ls_ew_mail", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len;
        len = sizeof(email); nvs_get_str(nvs, "email", email, &len);
        len = sizeof(subject); nvs_get_str(nvs, "subject", subject, &len);
        len = sizeof(body); nvs_get_str(nvs, "body", body, &len);
        nvs_close(nvs);
    }

    if (strlen(email) == 0) {
        ESP_LOGW(TAG, "Erken uyari mail adresi bos");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = smtp_client_send(email, subject, body);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Erken uyari mail gonderildi: %s", email);
    } else {
        ESP_LOGW(TAG, "Erken uyari mail hatasi: %s", esp_err_to_name(err));
    }

    vTaskDelete(NULL);
}

/* Alarm tetiklenince - erken uyari Telegram mesaji gonder (ayri task) */
static void alarm_telegram_task(void *arg)
{
    int alarm_index = (int)(intptr_t)arg;
    ESP_LOGI(TAG, "Erken uyari Telegram gonderiliyor (alarm #%d)", alarm_index);

    // Action aktif mi kontrol et
    nvs_handle_t nvs;
    uint8_t early_tg_active = 0;
    if (nvs_open("ls_actions", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "early_tg", &early_tg_active);
        nvs_close(nvs);
    }
    if (!early_tg_active) {
        ESP_LOGI(TAG, "Erken uyari Telegram pasif, atlaniyor");
        vTaskDelete(NULL);
        return;
    }

    // Telegram ayarli mi kontrol et
    if (!telegram_client_has_config()) {
        ESP_LOGW(TAG, "Telegram yapilandirilmamis, erken uyari gonderilemedi");
        vTaskDelete(NULL);
        return;
    }

    // Mesaj sablonunu oku
    telegram_config_t cfg;
    telegram_client_get_config(&cfg);

    const char *msg = (strlen(cfg.message) > 0) ? cfg.message : "LebensSpur: Zamanlayiciniz yakinda sona erecek!";
    esp_err_t err = telegram_client_send(msg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Erken uyari Telegram gonderildi");
    } else {
        ESP_LOGW(TAG, "Erken uyari Telegram hatasi: %s", esp_err_to_name(err));
    }

    vTaskDelete(NULL);
}

static void on_alarm_triggered(int alarm_index)
{
    ESP_LOGI(TAG, "Alarm tetiklendi: #%d", alarm_index);
    xTaskCreate(alarm_mail_task, "alarm_mail", 10240,
                (void *)(intptr_t)alarm_index, 5, NULL);
    xTaskCreate(alarm_telegram_task, "alarm_tg", 8192,
                (void *)(intptr_t)alarm_index, 5, NULL);
}

/* Tetikleme mail gonder (ayri task) */
static void trigger_mail_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Tetikleme mailleri gonderiliyor");

    // Action aktif mi kontrol et
    nvs_handle_t nvs;
    uint8_t trig_mail_active = 0;
    if (nvs_open("ls_actions", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "trig_mail", &trig_mail_active);
        nvs_close(nvs);
    }
    if (!trig_mail_active) {
        ESP_LOGI(TAG, "Tetikleme mail pasif, atlaniyor");
        vTaskDelete(NULL);
        return;
    }

    if (!smtp_client_has_config()) {
        ESP_LOGW(TAG, "SMTP yapilandirilmamis, tetikleme mailleri gonderilemedi");
        vTaskDelete(NULL);
        return;
    }

    // Tum gruplari oku ve her grubun alicilarina mail gonder
    if (nvs_open("ls_tg_mail", NVS_READONLY, &nvs) != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }

    uint8_t count = 0;
    nvs_get_u8(nvs, "count", &count);

    for (int i = 0; i < count && i < 10; i++) {
        char key[20];
        char subj[64] = {0}, body[256] = {0}, rcpt[256] = {0};
        size_t len;

        snprintf(key, sizeof(key), "g%d_subj", i);
        len = sizeof(subj); nvs_get_str(nvs, key, subj, &len);

        snprintf(key, sizeof(key), "g%d_body", i);
        len = sizeof(body); nvs_get_str(nvs, key, body, &len);

        snprintf(key, sizeof(key), "g%d_rcpt", i);
        len = sizeof(rcpt); nvs_get_str(nvs, key, rcpt, &len);

        if (strlen(rcpt) == 0) continue;

        // Dosya eklerini topla
        #define MAX_ATTACH 5
        char *attach_paths[MAX_ATTACH];
        int attach_count = 0;
        char mail_dir[40];
        snprintf(mail_dir, sizeof(mail_dir), "/ext/mail/g%d", i);

        DIR *dir = opendir(mail_dir);
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL && attach_count < MAX_ATTACH) {
                if (ent->d_type != DT_REG) continue;
                attach_paths[attach_count] = malloc(300);
                if (attach_paths[attach_count]) {
                    snprintf(attach_paths[attach_count], 300, "%s/%s", mail_dir, ent->d_name);
                    attach_count++;
                }
            }
            closedir(dir);
        }

        // Virgullerle ayrilmis alicilara tek tek gonder
        char rcpt_copy[256];
        strncpy(rcpt_copy, rcpt, sizeof(rcpt_copy) - 1);
        rcpt_copy[sizeof(rcpt_copy) - 1] = '\0';

        char *saveptr = NULL;
        char *token = strtok_r(rcpt_copy, ",", &saveptr);
        while (token) {
            // Bosluk trim
            while (*token == ' ') token++;
            if (strlen(token) > 0) {
                esp_err_t err;
                if (attach_count > 0) {
                    err = smtp_client_send_with_attachments(
                        token, subj, body,
                        (const char **)attach_paths, attach_count);
                } else {
                    err = smtp_client_send(token, subj, body);
                }
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Tetikleme mail gonderildi: %s (grup %d, %d ek)", token, i, attach_count);
                } else {
                    ESP_LOGW(TAG, "Tetikleme mail hatasi: %s -> %s", token, esp_err_to_name(err));
                }
            }
            token = strtok_r(NULL, ",", &saveptr);
        }

        // Ek yollarini serbest birak
        for (int j = 0; j < attach_count; j++) {
            free(attach_paths[j]);
        }
    }

    nvs_close(nvs);
    vTaskDelete(NULL);
}

/* Timer dolunca callback - relais'i tetikle + tetikleme mailleri gonder */
static void on_timer_expired(void)
{
    ESP_LOGI(TAG, "Timer doldu - relais tetikleniyor");

    // Relay aktif mi kontrol et
    nvs_handle_t nvs;
    uint8_t trig_relay = 1; // varsayilan aktif
    if (nvs_open("ls_actions", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "trig_relay", &trig_relay);
        nvs_close(nvs);
    }
    if (trig_relay) {
        output_relay_trigger();
    }

    /* Web arayuzune anlik bildirim */
    web_server_send_timer_event();

    /* Tetikleme mail gruplarina mail gonder */
    xTaskCreate(trigger_mail_task, "trig_mail", 10240, NULL, 5, NULL);
}

// MAC adresinden FNV-1a hash ile 10 haneli benzersiz cihaz ID olustur
// Ornek: LS-K7M2X9P4HB
static void generate_device_name(void)
{
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);

    // FNV-1a 64-bit hash (tum 6 byte MAC kullanilir)
    uint64_t hash = 0xcbf29ce484222325ULL;  // FNV offset basis
    for (int i = 0; i < 6; i++) {
        hash ^= mac[i];
        hash *= 0x100000001b3ULL;            // FNV prime
    }

    // 10 haneli alfanumerik ID olustur (A-Z, 0-9 = 36 karakter)
    static const char charset[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    // I ve O cikarildi (1 ve 0 ile karismamasi icin) = 33 karakter
    const int base = sizeof(charset) - 1;  // 33

    char id[11];
    for (int i = 0; i < 10; i++) {
        id[i] = charset[hash % base];
        hash /= base;
    }
    id[10] = '\0';

    snprintf(device_name, sizeof(device_name), "LS-%s", id);
    ESP_LOGI(TAG, "Cihaz ID: %s (MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
             device_name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== LebensSpur Baslatiliyor ===");

    // Benzersiz cihaz adi olustur
    generate_device_name();

    // NVS Flash baslatma
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS flash siliniyor ve yeniden baslatiliyor...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS Flash baslatildi");

    // Device log baslatma
    device_log_init();

    // Harici flash baslatma (SPI - W25Q256)
    ret = ext_flash_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Harici flash baslatilamadi! Hata: %s", esp_err_to_name(ret));
        device_log_add(LOG_TYPE_ERROR, "Harici flash hatasi: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Harici flash baslatildi (32MB)");
        // Kalici loglari flash'tan yukle (ext_flash hazir)
        device_log_persist_init();
        // Boot/restart olayi logla
        const char *rst_reason;
        switch (esp_reset_reason()) {
            case ESP_RST_POWERON:   rst_reason = "Power on"; break;
            case ESP_RST_SW:        rst_reason = "Software restart"; break;
            case ESP_RST_PANIC:     rst_reason = "Panic/crash"; break;
            case ESP_RST_INT_WDT:   rst_reason = "Interrupt WDT"; break;
            case ESP_RST_TASK_WDT:  rst_reason = "Task WDT"; break;
            case ESP_RST_WDT:       rst_reason = "Watchdog"; break;
            case ESP_RST_DEEPSLEEP: rst_reason = "Deep sleep"; break;
            case ESP_RST_BROWNOUT:  rst_reason = "Brownout (dusuk voltaj)"; break;
            default:                rst_reason = "Bilinmeyen"; break;
        }
        device_log_add(LOG_TYPE_RESTART, "Sistem basladi (boot #%lu) - Neden: %s",
                       (unsigned long)device_log_get_boot_count(), rst_reason);
        // Varsayilan web dosyalarini olustur (yoksa)
        ext_flash_create_default_files();
    }

    // Flash senkronizasyon baslatma
    ret = flash_sync_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Flash senkronizasyon baslatilamadi: %s", esp_err_to_name(ret));
        device_log_add(LOG_TYPE_ERROR, "Flash sync hatasi: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Flash senkronizasyon hazir");
    }

    // Zamanlayici baslatma
    ret = timer_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Timer manager baslatilamadi: %s", esp_err_to_name(ret));
        device_log_add(LOG_TYPE_ERROR, "Timer hatasi: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Timer manager hazir");
    }

    // Output baslatma (buton + relais)
    ret = output_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Output baslatilamadi: %s", esp_err_to_name(ret));
        device_log_add(LOG_TYPE_ERROR, "Output hatasi: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Output hazir (Buton + Relais)");
        output_set_button_callback(on_button_pressed);
        output_set_factory_reset_callback(on_factory_reset);
        timer_manager_set_expiry_callback(on_timer_expired);
        timer_manager_set_alarm_callback(on_alarm_triggered);
        timer_manager_set_tick_callback(web_server_send_timer_event);
    }

    // SMTP client baslatma
    ret = smtp_client_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SMTP client baslatilamadi: %s", esp_err_to_name(ret));
        device_log_add(LOG_TYPE_ERROR, "SMTP hatasi: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SMTP client hazir");
    }

    // Telegram client baslatma
    ret = telegram_client_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Telegram client baslatilamadi: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Telegram client hazir");
    }

    // GUI OTA baslatma
    ret = gui_ota_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GUI OTA baslatilamadi: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "GUI OTA hazir (aktif slot: %c)", ext_flash_get_active_slot());
    }

    // Firmware OTA baslatma
    ret = fw_ota_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "FW OTA baslatilamadi: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "FW OTA hazir (partition: %s)", fw_ota_get_running_label());
    }

    // WiFi AP modda baslat (cihaz adi ile)
    ret = wifi_manager_init(device_name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi baslatilamadi! Hata: %s", esp_err_to_name(ret));
        device_log_add(LOG_TYPE_ERROR, "WiFi hatasi: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "WiFi AP modu baslatildi");
    }

    // Web server baslatma (AP uzerinden erisim icin WiFi'dan ONCE baslamali)
    // auto_connect blocking olabilir, web server önce hazir olmali
    ret = web_server_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Web server baslatilamadi! Hata: %s", esp_err_to_name(ret));
        device_log_add(LOG_TYPE_ERROR, "Web server hatasi: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Web server baslatildi");
        device_log_add(LOG_TYPE_SYSTEM, "Sistem hazir (slot:%c)",
                       ext_flash_get_active_slot());
    }

    // OTA rollback korumasi: basarili boot'u onayla
    // Web server ayaga kalktigindan firmware gecerli sayilir
    esp_ota_mark_app_valid_cancel_rollback();

    // ============================================================
    // Flash Bilgileri
    // ============================================================
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========== FLASH BILGILERI ==========");

    // Dahili flash bilgisi
    uint32_t int_flash_size;
    if (esp_flash_get_size(NULL, &int_flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "[DAHILI FLASH]");
        ESP_LOGI(TAG, "  Toplam: %lu KB (%lu MB)", (unsigned long)(int_flash_size / 1024),
                 (unsigned long)(int_flash_size / (1024 * 1024)));

        // Partition bazinda kullanim (harici flash partition'lari haric)
        size_t app_used = 0;
        esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
        while (it != NULL) {
            const esp_partition_t *part = esp_partition_get(it);
            // Harici flash partition'larini atla
            if (strcmp(part->label, "slot_a") != 0 &&
                strcmp(part->label, "slot_b") != 0 &&
                strcmp(part->label, "user_data") != 0) {
                ESP_LOGI(TAG, "  Partition: %-12s | Boyut: %6lu KB | Offset: 0x%06lx",
                         part->label, (unsigned long)(part->size / 1024), (unsigned long)part->address);
                app_used += part->size;
            }
            it = esp_partition_next(it);
        }
        esp_partition_iterator_release(it);

        size_t int_free = int_flash_size - app_used;
        ESP_LOGI(TAG, "  Kullanilan: %lu KB | Bos: %lu KB",
                 (unsigned long)(app_used / 1024), (unsigned long)(int_free / 1024));
    }

    // Harici flash bilgisi - 3 Partition
    ESP_LOGI(TAG, "[HARICI FLASH - 32MB, 3 Partition]");

    // Slot A (GUI Web)
    if (ext_flash_is_slot_a_mounted()) {
        size_t sa_total = 0, sa_free = 0;
        if (ext_flash_get_slot_a_info(&sa_total, &sa_free) == ESP_OK) {
            ESP_LOGI(TAG, "  Slot A (GUI) : %zu KB toplam, %zu KB kullanilan, %zu KB bos",
                     sa_total / 1024, (sa_total - sa_free) / 1024, sa_free / 1024);
        }
    } else {
        ESP_LOGW(TAG, "  Slot A (GUI) : Mount edilemedi!");
    }

    // Slot B (GUI OTA)
    if (ext_flash_is_slot_b_mounted()) {
        size_t sb_total = 0, sb_free = 0;
        if (ext_flash_get_slot_b_info(&sb_total, &sb_free) == ESP_OK) {
            ESP_LOGI(TAG, "  Slot B (OTA) : %zu KB toplam, %zu KB kullanilan, %zu KB bos",
                     sb_total / 1024, (sb_total - sb_free) / 1024, sb_free / 1024);
        }
    } else {
        ESP_LOGW(TAG, "  Slot B (OTA) : Mount edilemedi!");
    }

    // User Data
    if (ext_flash_is_user_data_mounted()) {
        size_t ud_total = 0, ud_free = 0;
        if (ext_flash_get_info(&ud_total, &ud_free) == ESP_OK) {
            ESP_LOGI(TAG, "  User Data    : %zu KB toplam, %zu KB kullanilan, %zu KB bos",
                     ud_total / 1024, (ud_total - ud_free) / 1024, ud_free / 1024);
        }
    } else {
        ESP_LOGW(TAG, "  User Data    : Mount edilemedi!");
    }
    ESP_LOGI(TAG, "=====================================");
    ESP_LOGI(TAG, "");

    ESP_LOGI(TAG, "=== LebensSpur Hazir ===");
    ESP_LOGI(TAG, "Cihaz: %s", device_name);
    ESP_LOGI(TAG, "AP SSID: %s (Sifresiz)", device_name);
    ESP_LOGI(TAG, "mDNS: %s.local", device_name);
    ESP_LOGI(TAG, "AP IP: 192.168.4.1");

    // Kayitli WiFi varsa otomatik baglan (tara -> en iyi aga baglan)
    // Web server ve AP zaten hazir, bu blocking olsa bile sorun yok
    wifi_manager_auto_connect();
}
