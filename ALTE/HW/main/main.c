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
#include "esp_timer.h"
#include "esp_ota_ops.h"

#include "wifi_manager.h"
#include "web_server.h"
#include "ext_flash.h"
#include "timer_manager.h"
#include "smtp_client.h"
#include "sys_log.h"
#include "output.h"
#include "telegram_client.h"
#include "gui_ota.h"
#include "fw_ota.h"
#include "api_client.h"

static const char *TAG = "MAIN";
static char device_name[20] = {0};

/* OTA sonrasi yeni firmware ancak bu kadar sure sorunsuz calisirsa onaylanir;
 * oncesinde cokerse bootloader onceki surume doner. */
#define OTA_VALID_AFTER_S   60

/* ========== NVS yardimcilari ========== */

static uint8_t nvs_get_flag(const char *ns, const char *key, uint8_t def)
{
    nvs_handle_t nvs;
    uint8_t v = def;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, key, &v);
        nvs_close(nvs);
    }
    return v;
}

/* ========== OTA onayi (60 sn stabil calisma sonrasi) ========== */

static void ota_confirm_task(void *arg)
{
    (void)arg;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        sys_log_write("INFO", "ota", "Firmware %s dogrulandi (%d sn stabil)",
                      running->label, OTA_VALID_AFTER_S);
    }
    vTaskDelete(NULL);
}

static void ota_confirm_cb(void *arg)
{
    (void)arg;
    /* esp_timer gorevinden flash'a yazilmaz: ayri gorevde */
    xTaskCreate(ota_confirm_task, "ota_ok", 4096, NULL, 3, NULL);
}

/* ========== Buton ========== */

/* Buton callback - timer'i yeniden baslat */
static void on_button_pressed(void)
{
    ESP_LOGI(TAG, "Buton basildi - timer restart");
    sys_log_mark_event(SL_EV_BUTTON, 0);

    /* Relais aciksa kapat */
    if (output_relay_is_on()) {
        output_relay_off();
    }

    esp_err_t err = timer_manager_restart();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Timer restart basarisiz: %s", esp_err_to_name(err));
        sys_log_write("WARN", "timer", "Buton: geri sayim yeniden baslatilamadi (%s)", esp_err_to_name(err));
    } else {
        timer_state_t st = timer_manager_get_state();
        sys_log_write("INFO", "timer", "Buton: geri sayim sifirlandi (%ld sn)", (long)st.total_seconds);
    }

    /* Web arayuzune anlik bildirim */
    web_server_send_timer_event();
}

/* Factory reset callback - butonla 5x basma */
static void on_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory Reset (buton 5x)");
    sys_log_set_restart_reason(SL_RST_FACTORY);
    sys_log_write("WARN", "user", "Fabrika sifirlama (buton 5x)");
    nvs_flash_erase();
    ext_flash_format_user_data();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* ========== Erken uyari (alarm) ========== */

/* Alarm tetiklenince - erken uyari mail gonder (ayri task) */
static void alarm_mail_task(void *arg)
{
    int alarm_index = (int)(intptr_t)arg;
    ESP_LOGI(TAG, "Erken uyari mail gonderiliyor (alarm #%d)", alarm_index);

    if (!nvs_get_flag("ls_actions", "early_mail", 0)) {
        ESP_LOGI(TAG, "Erken uyari mail pasif, atlaniyor");
        vTaskDelete(NULL);
        return;
    }

    if (!smtp_client_has_config()) {
        ESP_LOGW(TAG, "SMTP yapilandirilmamis, erken uyari mail gonderilemedi");
        sys_log_write("WARN", "mail", "Erken uyari #%d: SMTP yapilandirilmamis", alarm_index + 1);
        vTaskDelete(NULL);
        return;
    }

    // Erken uyari mail config'ini NVS'den oku
    nvs_handle_t nvs;
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
        sys_log_write("WARN", "mail", "Erken uyari #%d: alici adresi bos", alarm_index + 1);
        vTaskDelete(NULL);
        return;
    }

    sys_log_mark_event(SL_EV_MAIL_SEND, alarm_index);
    esp_err_t err = smtp_client_send(email, subject, body);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Erken uyari mail gonderildi: %s", email);
        sys_log_write("INFO", "mail", "Erken uyari #%d maili gonderildi: %s", alarm_index + 1, email);
    } else {
        ESP_LOGW(TAG, "Erken uyari mail hatasi: %s", esp_err_to_name(err));
        sys_log_write("ERR", "mail", "Erken uyari #%d maili gonderilemedi: %s (%s)",
                      alarm_index + 1, email, esp_err_to_name(err));
    }

    vTaskDelete(NULL);
}

/* Alarm tetiklenince - erken uyari Telegram mesaji gonder (ayri task) */
static void alarm_telegram_task(void *arg)
{
    int alarm_index = (int)(intptr_t)arg;
    ESP_LOGI(TAG, "Erken uyari Telegram gonderiliyor (alarm #%d)", alarm_index);

    if (!nvs_get_flag("ls_actions", "early_tg", 0)) {
        ESP_LOGI(TAG, "Erken uyari Telegram pasif, atlaniyor");
        vTaskDelete(NULL);
        return;
    }

    if (!telegram_client_has_config()) {
        ESP_LOGW(TAG, "Telegram yapilandirilmamis, erken uyari gonderilemedi");
        sys_log_write("WARN", "tg", "Erken uyari #%d: Telegram yapilandirilmamis", alarm_index + 1);
        vTaskDelete(NULL);
        return;
    }

    telegram_config_t cfg;
    telegram_client_get_config(&cfg);

    const char *msg = (strlen(cfg.message) > 0) ? cfg.message : "LebensSpur: Zamanlayiciniz yakinda sona erecek!";
    sys_log_mark_event(SL_EV_TG_SEND, alarm_index);
    esp_err_t err = telegram_client_send(msg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Erken uyari Telegram gonderildi");
        sys_log_write("INFO", "tg", "Erken uyari #%d Telegram mesaji gonderildi", alarm_index + 1);
    } else {
        ESP_LOGW(TAG, "Erken uyari Telegram hatasi: %s", esp_err_to_name(err));
        sys_log_write("ERR", "tg", "Erken uyari #%d Telegram gonderilemedi: %s",
                      alarm_index + 1, esp_err_to_name(err));
    }

    vTaskDelete(NULL);
}

/* Alarm/tetikleme: webhook + MQTT (ayri task) */
static void api_notify_task(void *arg)
{
    const char *status = (const char *)arg;
    api_client_notify_all(status);
    vTaskDelete(NULL);
}

static void on_alarm_triggered(int alarm_index)
{
    ESP_LOGI(TAG, "Alarm tetiklendi: #%d", alarm_index);
    timer_state_t st = timer_manager_get_state();
    sys_log_write("WARN", "timer", "Erken uyari #%d/%d tetiklendi (kalan %ld sn)",
                  alarm_index + 1, st.alarm_count, (long)st.remaining_seconds);
    xTaskCreate(alarm_mail_task, "alarm_mail", 10240,
                (void *)(intptr_t)alarm_index, 5, NULL);
    xTaskCreate(alarm_telegram_task, "alarm_tg", 8192,
                (void *)(intptr_t)alarm_index, 5, NULL);
    if (nvs_get_flag("ls_actions", "early_api", 0)) {
        xTaskCreate(api_notify_task, "alarm_api", 8192, (void *)"alarm", 5, NULL);
    }
}

/* ========== Tetikleme (sure doldu) ========== */

/* Tetikleme mail gonder (ayri task) */
static void trigger_mail_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Tetikleme mailleri gonderiliyor");

    if (!nvs_get_flag("ls_actions", "trig_mail", 0)) {
        ESP_LOGI(TAG, "Tetikleme mail pasif, atlaniyor");
        vTaskDelete(NULL);
        return;
    }

    if (!smtp_client_has_config()) {
        ESP_LOGW(TAG, "SMTP yapilandirilmamis, tetikleme mailleri gonderilemedi");
        sys_log_write("ERR", "mail", "Tetikleme: SMTP yapilandirilmamis, mail gonderilemedi");
        vTaskDelete(NULL);
        return;
    }

    nvs_handle_t nvs;
    if (nvs_open("ls_tg_mail", NVS_READONLY, &nvs) != ESP_OK) {
        sys_log_write("ERR", "mail", "Tetikleme: mail gruplari okunamadi");
        vTaskDelete(NULL);
        return;
    }

    uint8_t count = 0;
    nvs_get_u8(nvs, "count", &count);
    int sent_ok = 0, sent_fail = 0;

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
            while (*token == ' ') token++;
            if (strlen(token) > 0) {
                esp_err_t err;
                sys_log_mark_event(SL_EV_MAIL_SEND, i);
                if (attach_count > 0) {
                    err = smtp_client_send_with_attachments(
                        token, subj, body,
                        (const char **)attach_paths, attach_count);
                } else {
                    err = smtp_client_send(token, subj, body);
                }
                if (err == ESP_OK) {
                    sent_ok++;
                    ESP_LOGI(TAG, "Tetikleme mail gonderildi: %s (grup %d, %d ek)", token, i, attach_count);
                    sys_log_write("INFO", "mail", "Tetikleme maili gonderildi: %s (grup %d, %d ek)",
                                  token, i + 1, attach_count);
                } else {
                    sent_fail++;
                    ESP_LOGW(TAG, "Tetikleme mail hatasi: %s -> %s", token, esp_err_to_name(err));
                    sys_log_write("ERR", "mail", "Tetikleme maili gonderilemedi: %s (grup %d): %s",
                                  token, i + 1, esp_err_to_name(err));
                }
            }
            token = strtok_r(NULL, ",", &saveptr);
        }

        for (int j = 0; j < attach_count; j++) {
            free(attach_paths[j]);
        }
    }

    nvs_close(nvs);
    sys_log_write(sent_fail ? "WARN" : "INFO", "mail", "Tetikleme mailleri: %d basarili, %d basarisiz",
                  sent_ok, sent_fail);
    vTaskDelete(NULL);
}

/* Timer dolunca callback - relais'i tetikle + tetikleme mailleri gonder */
static void on_timer_expired(void)
{
    ESP_LOGI(TAG, "Timer doldu - relais tetikleniyor");
    sys_log_mark_event(SL_EV_TIMER_EXPIRED, 0);

    uint8_t trig_relay = nvs_get_flag("ls_actions", "trig_relay", 1);
    sys_log_write("WARN", "timer", "GERI SAYIM DOLDU: LebensSpur protokolu tetiklendi (role %s)",
                  trig_relay ? "aktif" : "pasif");
    if (trig_relay) {
        output_relay_trigger();
    }

    /* Web arayuzune anlik bildirim */
    web_server_send_timer_event();

    /* Tetikleme mail gruplarina mail gonder */
    xTaskCreate(trigger_mail_task, "trig_mail", 10240, NULL, 5, NULL);

    /* Webhook / MQTT */
    if (nvs_get_flag("ls_actions", "trig_api", 0)) {
        xTaskCreate(api_notify_task, "trig_api", 8192, (void *)"triggered", 5, NULL);
    }
}

/* ========== Cihaz kimligi ========== */

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

    // 10 haneli alfanumerik ID olustur (I ve O cikarildi: 1 ve 0 ile karismasin)
    static const char charset[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    const int base = sizeof(charset) - 1;

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

/* ========== Flash bilgisi (acilis logu) ========== */

static void log_flash_info(void)
{
    ESP_LOGI(TAG, "========== FLASH BILGILERI ==========");
    uint32_t int_flash_size;
    if (esp_flash_get_size(NULL, &int_flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "[DAHILI FLASH] %lu MB", (unsigned long)(int_flash_size / (1024 * 1024)));
    }
    size_t total = 0, free_sp = 0;
    if (ext_flash_is_slot_a_mounted() && ext_flash_get_slot_a_info(&total, &free_sp) == ESP_OK) {
        ESP_LOGI(TAG, "[HARICI] Slot A (GUI) : %zu KB toplam, %zu KB bos", total / 1024, free_sp / 1024);
    } else {
        ESP_LOGW(TAG, "[HARICI] Slot A (GUI) : mount edilemedi");
    }
    if (ext_flash_is_slot_b_mounted() && ext_flash_get_slot_b_info(&total, &free_sp) == ESP_OK) {
        ESP_LOGI(TAG, "[HARICI] Slot B (OTA) : %zu KB toplam, %zu KB bos", total / 1024, free_sp / 1024);
    } else {
        ESP_LOGW(TAG, "[HARICI] Slot B (OTA) : mount edilemedi");
    }
    if (ext_flash_is_user_data_mounted() && ext_flash_get_info(&total, &free_sp) == ESP_OK) {
        ESP_LOGI(TAG, "[HARICI] User Data    : %zu KB toplam, %zu KB bos", total / 1024, free_sp / 1024);
    } else {
        ESP_LOGW(TAG, "[HARICI] User Data    : mount edilemedi");
    }
    ESP_LOGI(TAG, "=====================================");
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== LebensSpur Baslatiliyor ===");

    generate_device_name();

    // NVS Flash baslatma
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS flash siliniyor ve yeniden baslatiliyor...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Harici flash baslatma (SPI - W25Q256)
    esp_err_t ext_ret = ext_flash_init();
    if (ext_ret != ESP_OK) {
        ESP_LOGE(TAG, "Harici flash baslatilamadi! Hata: %s", esp_err_to_name(ext_ret));
    } else {
        ESP_LOGI(TAG, "Harici flash baslatildi");
    }

    // Sistem olay kaydi: onceki kapanisi analiz eder, acilis kaydini yazar.
    // Harici flash yoksa da calisir (RTC bellek), dosyaya yazamaz.
    sys_log_init();
    if (ext_ret != ESP_OK) {
        sys_log_write("ERR", "sys", "Harici flash baslatilamadi: %s", esp_err_to_name(ext_ret));
    } else {
        // Varsayilan web dosyalarini olustur (yoksa / surum degistiyse)
        ext_flash_create_default_files();
    }

    // Zamanlayici baslatma
    ret = timer_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Timer manager baslatilamadi: %s", esp_err_to_name(ret));
        sys_log_write("ERR", "timer", "Zamanlayici baslatilamadi: %s", esp_err_to_name(ret));
    }

    // Output baslatma (buton + relais)
    ret = output_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Output baslatilamadi: %s", esp_err_to_name(ret));
        sys_log_write("ERR", "sys", "Buton/role baslatilamadi: %s", esp_err_to_name(ret));
    } else {
        output_set_button_callback(on_button_pressed);
        output_set_factory_reset_callback(on_factory_reset);
    }
    timer_manager_set_expiry_callback(on_timer_expired);
    timer_manager_set_alarm_callback(on_alarm_triggered);
    timer_manager_set_tick_callback(web_server_send_timer_event);

    // SMTP / Telegram / OTA
    if (smtp_client_init() != ESP_OK)     ESP_LOGW(TAG, "SMTP client baslatilamadi");
    if (telegram_client_init() != ESP_OK) ESP_LOGW(TAG, "Telegram client baslatilamadi");
    api_client_init();
    if (gui_ota_init() != ESP_OK)         ESP_LOGW(TAG, "GUI OTA baslatilamadi");
    if (fw_ota_init() != ESP_OK)          ESP_LOGW(TAG, "FW OTA baslatilamadi");

    // WiFi AP modda baslat (cihaz adi ile)
    ret = wifi_manager_init(device_name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi baslatilamadi! Hata: %s", esp_err_to_name(ret));
        sys_log_write("ERR", "wifi", "WiFi baslatilamadi: %s", esp_err_to_name(ret));
    }

    // Web server (AP uzerinden erisim icin auto_connect'ten ONCE hazir olmali)
    ret = web_server_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Web server baslatilamadi! Hata: %s", esp_err_to_name(ret));
        sys_log_write("ERR", "sys", "Web server baslatilamadi: %s", esp_err_to_name(ret));
    }

    // OTA rollback korumasi: onay OTA_VALID_AFTER_S sn sonra (ota_confirm_cb).
    // Hemen onaylamak, yeni firmware bir dakika sonra cokse bile geri donusu engelliyordu.
    {
        const esp_timer_create_args_t ta = { .callback = ota_confirm_cb, .name = "ota_ok" };
        esp_timer_handle_t th = NULL;
        if (esp_timer_create(&ta, &th) == ESP_OK) {
            esp_timer_start_once(th, (uint64_t)OTA_VALID_AFTER_S * 1000000ULL);
        } else {
            esp_ota_mark_app_valid_cancel_rollback();   // zamanlayici yoksa eski davranis
        }
    }

    log_flash_info();

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "=== LebensSpur Hazir === v%s | Cihaz: %s | AP SSID: %s | mDNS: %s.local | acilis #%lu (kisa: %lu)",
             app->version, device_name, device_name, device_name,
             (unsigned long)sys_log_boot_count(), (unsigned long)sys_log_short_boots());
    sys_log_mark_event(SL_EV_BOOT, 0);
    {
        timer_state_t st = timer_manager_get_state();
        sys_log_write("INFO", "sys", "Sistem hazir v%s (slot %c, geri sayim %s, kalan %ld sn)",
                      app->version, ext_flash_get_active_slot(),
                      st.enabled ? "aktif" : "pasif", (long)st.remaining_seconds);
    }

    // Kayitli WiFi varsa otomatik baglan (tara -> en iyi aga baglan)
    wifi_manager_auto_connect();
}
