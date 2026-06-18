/**
 * LebensSpur - WiFi Manager (v2 - sifirdan yazildi)
 *
 * Akilli WiFi baglanti yonetimi:
 *   1. Boot: AP mod baslar, sonra STA baglantisi denenir
 *   2. Baglanmadan once ag taramasi yapilir
 *   3. Taramada primary bulunursa -> primary'ye baglan
 *   4. Taramada sadece backup bulunursa -> backup'a baglan
 *   5. Hicbiri bulunamazsa -> gizli ag olabilir, direkt dene
 *   6. Baglanti koparsa -> aninda yeniden baglan
 *   7. Tum denemeler basarisizsa -> 30 sn sonra tekrar tara (sonsuz)
 *   8. AP modu kullanici kapattiysa asla geri acilmaz
 */

#include "wifi_manager.h"
#include "device_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "WIFI_MGR";

/* ========== Durum Degiskenleri ========== */
static bool s_is_connected = false;
static char s_ip_addr[16] = "192.168.4.1";
static char s_device_name[32] = {0};
static char s_mdns_hostname[32] = {0};
static char s_sta_ssid[33] = {0};       // Aktif baglanti SSID
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool s_ap_enabled = true;

/* ========== WiFi Kimlik Bilgileri (hafizada) ========== */
typedef struct {
    char ssid[33];
    char pass[65];
    char ip[16];
} wifi_cred_t;

static wifi_cred_t s_primary = {0};
static wifi_cred_t s_backup = {0};

/* ========== Retry / Reconnect ========== */
#define RETRY_MAX               5
#define RETRY_BASE_MS           1000    // Ilk retry bekleme suresi
#define RECONNECT_INTERVAL_MS   15000   // 15 saniye

static int s_retry_count = 0;
static bool s_trying_backup = false;
static TaskHandle_t s_reconnect_task = NULL;
static bool s_reconnect_needed = false;     // Reconnect task'e sinyal

/* ========== Forward Declarations ========== */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data);
static void apply_static_ip(const char *static_ip);
static void start_reconnect_task(void);
static void stop_reconnect_task(void);

/* ========== Yardimci Fonksiyonlar ========== */

// NVS'den WiFi bilgilerini oku
static void load_credentials_from_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi_cfg", NVS_READONLY, &nvs) != ESP_OK) return;

    size_t len;

    // Primary
    len = sizeof(s_primary.ssid);
    if (nvs_get_str(nvs, "ssid", s_primary.ssid, &len) != ESP_OK)
        s_primary.ssid[0] = '\0';
    len = sizeof(s_primary.pass);
    if (nvs_get_str(nvs, "password", s_primary.pass, &len) != ESP_OK)
        s_primary.pass[0] = '\0';
    len = sizeof(s_primary.ip);
    if (nvs_get_str(nvs, "static_ip", s_primary.ip, &len) != ESP_OK)
        s_primary.ip[0] = '\0';

    // Backup
    len = sizeof(s_backup.ssid);
    if (nvs_get_str(nvs, "backup_ssid", s_backup.ssid, &len) != ESP_OK)
        s_backup.ssid[0] = '\0';
    len = sizeof(s_backup.pass);
    if (nvs_get_str(nvs, "backup_pass", s_backup.pass, &len) != ESP_OK)
        s_backup.pass[0] = '\0';
    len = sizeof(s_backup.ip);
    if (nvs_get_str(nvs, "backup_ip", s_backup.ip, &len) != ESP_OK)
        s_backup.ip[0] = '\0';

    nvs_close(nvs);

    ESP_LOGI(TAG, "NVS yuklu - Primary:'%s'(pass:%d,ip:'%s') Backup:'%s'(pass:%d,ip:'%s')",
             s_primary.ssid, (int)strlen(s_primary.pass), s_primary.ip,
             s_backup.ssid, (int)strlen(s_backup.pass), s_backup.ip);
}

// Statik IP uygula
static void apply_static_ip(const char *static_ip)
{
    if (!static_ip || strlen(static_ip) == 0) {
        esp_netif_dhcpc_start(s_sta_netif);
        return;
    }

    esp_netif_dhcpc_stop(s_sta_netif);
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_str_to_ip4(static_ip, &ip_info.ip);
    esp_netif_str_to_ip4("255.255.255.0", &ip_info.netmask);
    ip_info.gw.addr = (ip_info.ip.addr & htonl(0xFFFFFF00)) | htonl(0x00000001);
    esp_netif_set_ip_info(s_sta_netif, &ip_info);

    esp_netif_dns_info_t dns;
    dns.ip.u_addr.ip4.addr = ip_info.gw.addr;
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
    dns.ip.u_addr.ip4.addr = ipaddr_addr("8.8.8.8");
    esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns);

    ESP_LOGI(TAG, "Statik IP: %s (GW: x.x.x.1, DNS: GW + 8.8.8.8)", static_ip);
}

// AP modunu bozmadan STA arayuzunu aktif et
// AP zaten calisiyorsa APSTA'ya gecer, AP kapatilmaz
static void ensure_sta_mode(void)
{
    wifi_mode_t cur = WIFI_MODE_NULL;
    esp_wifi_get_mode(&cur);

    switch (cur) {
    case WIFI_MODE_AP:
        // AP calisiyor, yanina STA ekle (AP kesilmez)
        if (s_ap_enabled) {
            esp_wifi_set_mode(WIFI_MODE_APSTA);
            ESP_LOGI(TAG, "Mod: AP -> APSTA");
        } else {
            esp_wifi_set_mode(WIFI_MODE_STA);
            ESP_LOGI(TAG, "Mod: AP -> STA");
        }
        break;
    case WIFI_MODE_APSTA:
        // Zaten APSTA, dokunma
        break;
    case WIFI_MODE_STA:
        // Zaten STA, AP gerekiyorsa ekle
        if (s_ap_enabled) {
            wifi_config_t ap_cfg = {
                .ap = {
                    .channel = WIFI_AP_CHANNEL,
                    .max_connection = WIFI_AP_MAX_CONN,
                    .authmode = WIFI_AUTH_OPEN,
                },
            };
            strncpy((char *)ap_cfg.ap.ssid, s_device_name, sizeof(ap_cfg.ap.ssid) - 1);
            ap_cfg.ap.ssid_len = strlen(s_device_name);
            esp_wifi_set_mode(WIFI_MODE_APSTA);
            esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
            ESP_LOGI(TAG, "Mod: STA -> APSTA");
        }
        break;
    default:
        // WiFi baslatilmamis - init'de baslatilmis olmali
        ESP_LOGW(TAG, "WiFi modu NULL, yeniden baslatiliyor");
        if (s_ap_enabled) {
            esp_wifi_set_mode(WIFI_MODE_APSTA);
            wifi_config_t ap_cfg = {
                .ap = {
                    .channel = WIFI_AP_CHANNEL,
                    .max_connection = WIFI_AP_MAX_CONN,
                    .authmode = WIFI_AUTH_OPEN,
                },
            };
            strncpy((char *)ap_cfg.ap.ssid, s_device_name, sizeof(ap_cfg.ap.ssid) - 1);
            ap_cfg.ap.ssid_len = strlen(s_device_name);
            esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        } else {
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
        esp_wifi_start();
        break;
    }
}

// Belirli bir credential ile baglan (wifi durdurma/baslatma YAPMAZ)
static void do_connect(const wifi_cred_t *cred)
{
    // Mevcut baglanti denemesini durdur
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));  // WiFi stack'in temizlenmesini bekle

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, cred->ssid, sizeof(sta_cfg.sta.ssid) - 1);
    if (strlen(cred->pass) > 0) {
        strncpy((char *)sta_cfg.sta.password, cred->pass, sizeof(sta_cfg.sta.password) - 1);
    }
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    /* Zayif sinyalli AP'lere de baglanabilsin */
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_cfg.sta.threshold.rssi = -95;

    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    apply_static_ip(strlen(cred->ip) > 0 ? cred->ip : NULL);

    strncpy(s_sta_ssid, cred->ssid, sizeof(s_sta_ssid) - 1);
    s_retry_count = 0;

    ESP_LOGI(TAG, "Baglaniliyor: '%s' (IP:%s)",
             cred->ssid, strlen(cred->ip) > 0 ? cred->ip : "DHCP");
    esp_wifi_connect();
}

/* ========== Ag Tarama ve Akilli Baglanti ========== */

// Ag tara, primary/backup bul, en iyisine baglan
static void scan_and_connect(void)
{
    bool has_pri = (strlen(s_primary.ssid) > 0);
    bool has_bk = (strlen(s_backup.ssid) > 0);

    if (!has_pri && !has_bk) {
        ESP_LOGW(TAG, "Kayitli WiFi yok, baglanti denemesi yapilmiyor");
        return;
    }

    // WiFi taramasi (blocking, kendi task'inde calisir - deadlock riski yok)
    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 500,   // Zayif sinyal icin daha uzun tarama
    };

    ESP_LOGI(TAG, "Ag taramasi baslatiliyor...");
    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, true);

    bool pri_found = false, bk_found = false;
    int8_t pri_rssi = -127, bk_rssi = -127;

    if (ret == ESP_OK) {
        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);

        if (count > 0) {
            uint16_t max_ap = (count > 20) ? 20 : count;
            wifi_ap_record_t *list = malloc(sizeof(wifi_ap_record_t) * max_ap);
            if (list) {
                esp_wifi_scan_get_ap_records(&max_ap, list);
                ESP_LOGI(TAG, "Taramada %d ag bulundu", max_ap);

                for (int i = 0; i < max_ap; i++) {
                    const char *ssid = (const char *)list[i].ssid;
                    if (has_pri && strcmp(ssid, s_primary.ssid) == 0) {
                        pri_found = true;
                        pri_rssi = list[i].rssi;
                    }
                    if (has_bk && strcmp(ssid, s_backup.ssid) == 0) {
                        bk_found = true;
                        bk_rssi = list[i].rssi;
                    }
                }
                free(list);
            } else {
                esp_wifi_scan_get_ap_records(&max_ap, NULL);
            }
        } else {
            ESP_LOGW(TAG, "Taramada hic ag bulunamadi");
        }
    } else {
        ESP_LOGW(TAG, "Tarama basarisiz: %s", esp_err_to_name(ret));
    }

    // Karar
    if (pri_found) {
        ESP_LOGI(TAG, "Primary '%s' bulundu (RSSI:%d)", s_primary.ssid, pri_rssi);
        s_trying_backup = false;
        do_connect(&s_primary);
    } else if (bk_found) {
        ESP_LOGI(TAG, "Backup '%s' bulundu (RSSI:%d)", s_backup.ssid, bk_rssi);
        s_trying_backup = true;
        do_connect(&s_backup);
    } else if (has_pri) {
        // Gizli ag olabilir - directed probe ile dene
        ESP_LOGI(TAG, "SSID bulunamadi - gizli ag? Direkt deneniyor: '%s'", s_primary.ssid);
        s_trying_backup = false;
        do_connect(&s_primary);
    } else if (has_bk) {
        ESP_LOGI(TAG, "SSID bulunamadi - gizli ag? Direkt deneniyor: '%s'", s_backup.ssid);
        s_trying_backup = true;
        do_connect(&s_backup);
    }
}

/* ========== Reconnect Task ========== */

static void reconnect_task_fn(void *arg)
{
    ESP_LOGI(TAG, "Reconnect gorevi baslatildi (%d sn aralik)", RECONNECT_INTERVAL_MS / 1000);
    int attempt = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_INTERVAL_MS));

        if (s_is_connected) {
            ESP_LOGI(TAG, "Baglanti var, reconnect gorevi sonlaniyor");
            s_reconnect_task = NULL;
            vTaskDelete(NULL);
            return;
        }

        // Retry devam ediyorsa atlama
        if (s_retry_count > 0) continue;

        attempt++;
        ESP_LOGI(TAG, "--- Periyodik ag taramasi (#%d) ---", attempt);
        scan_and_connect();
    }
}

static void start_reconnect_task(void)
{
    if (s_reconnect_task != NULL) return;
    s_reconnect_needed = true;
    xTaskCreate(reconnect_task_fn, "wifi_reconn", 4096, NULL, 2, &s_reconnect_task);
}

static void stop_reconnect_task(void)
{
    s_reconnect_needed = false;
    if (s_reconnect_task) {
        TaskHandle_t t = s_reconnect_task;
        s_reconnect_task = NULL;
        vTaskDelete(t);
    }
}

/* ========== mDNS ========== */

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init basarisiz!");
        return;
    }

    /* mDNS hostname kucuk harf olmali (RFC 6762) */
    strncpy(s_mdns_hostname, s_device_name, sizeof(s_mdns_hostname) - 1);
    s_mdns_hostname[sizeof(s_mdns_hostname) - 1] = '\0';
    for (int i = 0; s_mdns_hostname[i]; i++) {
        if (s_mdns_hostname[i] >= 'A' && s_mdns_hostname[i] <= 'Z')
            s_mdns_hostname[i] += 32;
    }

    mdns_hostname_set(s_mdns_hostname);
    mdns_instance_name_set(s_device_name);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", s_mdns_hostname);
}

/* ========== WiFi Event Handler ========== */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e = data;
            ESP_LOGI(TAG, "AP: cihaz baglandi (AID=%d)", e->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *e = data;
            ESP_LOGI(TAG, "AP: cihaz ayrildi (AID=%d)", e->aid);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *dis = data;
            s_is_connected = false;

            ESP_LOGW(TAG, "DISCONNECT '%s' reason:%d", dis->ssid, dis->reason);

            // reason:201 = NO_AP_FOUND, reason:210 = CONNECTION_FAIL
            // reason:2/8 = normal ayrilma (biz disconnect cagirdik)
            if (dis->reason == WIFI_REASON_ASSOC_LEAVE ||
                dis->reason == WIFI_REASON_AUTH_LEAVE) {
                // Biz disconnect cagirdik, retry yapma
                break;
            }

            if (s_retry_count < RETRY_MAX) {
                s_retry_count++;
                int delay_ms = RETRY_BASE_MS * s_retry_count;  // 1s, 2s, 3s, 4s, 5s
                ESP_LOGW(TAG, "Retry %d/%d (%d ms bekleniyor)...",
                         s_retry_count, RETRY_MAX, delay_ms);
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
                esp_wifi_connect();
            } else if (!s_trying_backup && strlen(s_backup.ssid) > 0) {
                // Primary tukendi, backup dene
                ESP_LOGW(TAG, "Primary basarisiz. Backup deneniyor: '%s'", s_backup.ssid);
                s_trying_backup = true;
                do_connect(&s_backup);
            } else {
                // Hepsi basarisiz - periyodik tarama baslat
                ESP_LOGE(TAG, "Tum denemeler basarisiz. %d sn sonra tekrar taranacak.",
                         RECONNECT_INTERVAL_MS / 1000);
                device_log_add(LOG_TYPE_WIFI, "WiFi baglanti basarisiz (tum denemeler)");
                s_retry_count = 0;
                s_trying_backup = false;
                start_reconnect_task();
            }
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "BAGLANDI! IP:%s (%s WiFi: '%s')",
                 s_ip_addr, s_trying_backup ? "Backup" : "Primary", s_sta_ssid);
        device_log_add(LOG_TYPE_WIFI, "WiFi baglandi: %s (%s, %s)",
                       s_sta_ssid, s_ip_addr, s_trying_backup ? "backup" : "primary");
        s_is_connected = true;
        s_retry_count = 0;
        s_trying_backup = false;
        stop_reconnect_task();

        /* Power save kapat - zayif sinyalde stabilite icin */
        esp_wifi_set_ps(WIFI_PS_NONE);

        /* STA IP alindi - mDNS'i bu arayuzde yeniden duyur */
        if (strlen(s_mdns_hostname) > 0) {
            mdns_hostname_set(s_mdns_hostname);
            ESP_LOGI(TAG, "mDNS STA'da duyuruldu: %s.local", s_mdns_hostname);
        }
    }
}

/* ========== Public API ========== */

esp_err_t wifi_manager_init(const char *device_name)
{
    ESP_LOGI(TAG, "WiFi manager baslatiliyor...");

    strncpy(s_device_name, device_name, sizeof(s_device_name) - 1);

    // NVS'den AP durumunu yukle
    nvs_handle_t nvs;
    if (nvs_open("wifi_cfg", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t ap_val = 1;
        nvs_get_u8(nvs, "ap_enabled", &ap_val);
        s_ap_enabled = (ap_val != 0);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "AP modu: %s", s_ap_enabled ? "ACIK" : "KAPALI");

    // NVS'den WiFi bilgilerini yukle
    load_credentials_from_nvs();

    // TCP/IP, event loop, netif
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    // WiFi init
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Event handler
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // mDNS
    start_mdns();

    // AP modda basla
    return wifi_manager_start_ap();
}

esp_err_t wifi_manager_start_ap(void)
{
    wifi_config_t ap_cfg = {
        .ap = {
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, s_device_name, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(s_device_name);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    snprintf(s_ip_addr, sizeof(s_ip_addr), "192.168.4.1");
    ESP_LOGI(TAG, "AP baslatildi - SSID: %s (Sifresiz)", s_device_name);
    return ESP_OK;
}

esp_err_t wifi_manager_connect_sta(const char *ssid, const char *password,
                                    const char *static_ip)
{
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    // NVS'den mevcut bilgileri yukle
    load_credentials_from_nvs();

    // Sifre bos geldi ama ayni SSID icin NVS'de kayitli sifre varsa koru
    char use_pass[65] = {0};
    if (password && strlen(password) > 0) {
        strncpy(use_pass, password, sizeof(use_pass) - 1);
    } else if (strcmp(s_primary.ssid, ssid) == 0 && strlen(s_primary.pass) > 0) {
        strncpy(use_pass, s_primary.pass, sizeof(use_pass) - 1);
        ESP_LOGI(TAG, "Sifre bos geldi - NVS'deki mevcut sifre korunuyor");
    }

    // Statik IP karsilastirma icin
    const char *sip = (static_ip && strlen(static_ip) > 0) ? static_ip : "";

    // Zaten ayni SSID'ye bagliysa ve ayarlar degismemisse yeniden baglanma
    if (s_is_connected && strcmp(s_sta_ssid, ssid) == 0 &&
        strcmp(s_primary.pass, use_pass) == 0 &&
        strcmp(s_primary.ip, sip) == 0) {
        ESP_LOGI(TAG, "Zaten '%s' agina bagli, degisiklik yok - atlanıyor", ssid);
        return ESP_OK;
    }

    // Primary'yi guncelle
    memset(&s_primary, 0, sizeof(s_primary));
    strncpy(s_primary.ssid, ssid, sizeof(s_primary.ssid) - 1);
    strncpy(s_primary.pass, use_pass, sizeof(s_primary.pass) - 1);
    if (static_ip && strlen(static_ip) > 0)
        strncpy(s_primary.ip, static_ip, sizeof(s_primary.ip) - 1);

    s_trying_backup = false;
    s_retry_count = 0;
    stop_reconnect_task();

    // Mevcut STA baglantisini kes (AP etkilenmez)
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    // AP modunu bozmadan STA arayuzunu aktif et
    ensure_sta_mode();

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    if (strlen(use_pass) > 0) {
        strncpy((char *)sta_cfg.sta.password, use_pass, sizeof(sta_cfg.sta.password) - 1);
    }
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta_cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);

    apply_static_ip(static_ip);

    ESP_LOGI(TAG, "esp_wifi_connect() cagriliyor (pass_len:%d)...", (int)strlen(use_pass));
    esp_wifi_connect();

    // NVS'ye kaydet
    nvs_handle_t nvs;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "ssid", ssid);
        nvs_set_str(nvs, "password", use_pass);
        if (static_ip && strlen(static_ip) > 0) {
            nvs_set_str(nvs, "static_ip", static_ip);
        } else {
            nvs_erase_key(nvs, "static_ip");
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
    ESP_LOGI(TAG, "STA baglanti baslatildi: '%s'", ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_auto_connect(void)
{
    load_credentials_from_nvs();

    if (strlen(s_primary.ssid) == 0 && strlen(s_backup.ssid) == 0) {
        ESP_LOGI(TAG, "Kayitli WiFi yok, AP modunda kaliniyor");
        return ESP_ERR_NOT_FOUND;
    }

    // AP modunu bozmadan STA arayuzunu aktif et
    ensure_sta_mode();

    // Kisa bekle, WiFi stack hazir olsun
    vTaskDelay(pdMS_TO_TICKS(500));

    // Tara ve baglan
    scan_and_connect();
    return ESP_OK;
}

bool wifi_manager_has_saved_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi_cfg", NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t len = 0;
    esp_err_t ret = nvs_get_str(nvs, "ssid", NULL, &len);
    nvs_close(nvs);
    return (ret == ESP_OK && len > 1);
}

bool wifi_manager_is_connected(void) { return s_is_connected; }
const char *wifi_manager_get_ip(void) { return s_ip_addr; }
const char *wifi_manager_get_device_name(void) { return s_device_name; }
const char *wifi_manager_get_ap_ssid(void) { return s_device_name; }
const char *wifi_manager_get_sta_ssid(void) { return s_sta_ssid; }

esp_err_t wifi_manager_save_backup(const char *ssid, const char *password,
                                    const char *static_ip)
{
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    // Sifre bos geldi ama ayni SSID icin NVS'de kayitli sifre varsa koru
    char use_pass[65] = {0};
    if (password && strlen(password) > 0) {
        strncpy(use_pass, password, sizeof(use_pass) - 1);
    } else if (strcmp(s_backup.ssid, ssid) == 0 && strlen(s_backup.pass) > 0) {
        strncpy(use_pass, s_backup.pass, sizeof(use_pass) - 1);
        ESP_LOGI(TAG, "Backup sifre bos geldi - NVS'deki mevcut sifre korunuyor");
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    nvs_set_str(nvs, "backup_ssid", ssid);
    if (strlen(use_pass) > 0) {
        nvs_set_str(nvs, "backup_pass", use_pass);
    } else {
        nvs_erase_key(nvs, "backup_pass");
    }
    if (static_ip && strlen(static_ip) > 0) {
        nvs_set_str(nvs, "backup_ip", static_ip);
    } else {
        nvs_erase_key(nvs, "backup_ip");
    }
    nvs_commit(nvs);
    nvs_close(nvs);

    // Hafizaya da kaydet
    memset(&s_backup, 0, sizeof(s_backup));
    strncpy(s_backup.ssid, ssid, sizeof(s_backup.ssid) - 1);
    strncpy(s_backup.pass, use_pass, sizeof(s_backup.pass) - 1);
    if (static_ip && strlen(static_ip) > 0)
        strncpy(s_backup.ip, static_ip, sizeof(s_backup.ip) - 1);

    ESP_LOGI(TAG, "Backup WiFi kaydedildi: '%s' (pass_len:%d)", ssid, (int)strlen(use_pass));
    return ESP_OK;
}

bool wifi_manager_has_backup_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi_cfg", NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t len = 0;
    esp_err_t ret = nvs_get_str(nvs, "backup_ssid", NULL, &len);
    nvs_close(nvs);
    return (ret == ESP_OK && len > 1);
}

const char *wifi_manager_get_backup_ssid(void)
{
    return s_backup.ssid;
}

esp_err_t wifi_manager_set_ap_enabled(bool enabled)
{
    // Guvenlik: STA bagli degilse AP kapatilamaz
    if (!enabled && !s_is_connected) {
        ESP_LOGW(TAG, "AP kapatilamaz: STA bagli degil!");
        return ESP_ERR_INVALID_STATE;
    }

    s_ap_enabled = enabled;

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    if (enabled && mode == WIFI_MODE_STA) {
        wifi_config_t ap_cfg = {
            .ap = {
                .channel = WIFI_AP_CHANNEL,
                .max_connection = WIFI_AP_MAX_CONN,
                .authmode = WIFI_AUTH_OPEN,
            },
        };
        strncpy((char *)ap_cfg.ap.ssid, s_device_name, sizeof(ap_cfg.ap.ssid) - 1);
        ap_cfg.ap.ssid_len = strlen(s_device_name);
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        ESP_LOGI(TAG, "AP ACILDI (APSTA)");
    } else if (!enabled && mode == WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_LOGI(TAG, "AP KAPATILDI (STA)");
    }

    // NVS'ye kaydet
    nvs_handle_t nvs;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "ap_enabled", enabled ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    return ESP_OK;
}

bool wifi_manager_is_ap_enabled(void)
{
    return s_ap_enabled;
}

esp_err_t wifi_manager_clear_primary(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    nvs_erase_key(nvs, "ssid");
    nvs_erase_key(nvs, "password");
    nvs_erase_key(nvs, "static_ip");
    nvs_commit(nvs);
    nvs_close(nvs);

    memset(&s_primary, 0, sizeof(s_primary));
    memset(s_sta_ssid, 0, sizeof(s_sta_ssid));

    ESP_LOGI(TAG, "Primary WiFi ayarlari silindi (NVS + hafiza)");
    return ESP_OK;
}

esp_err_t wifi_manager_clear_backup(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    nvs_erase_key(nvs, "backup_ssid");
    nvs_erase_key(nvs, "backup_pass");
    nvs_erase_key(nvs, "backup_ip");
    nvs_commit(nvs);
    nvs_close(nvs);

    memset(&s_backup, 0, sizeof(s_backup));

    ESP_LOGI(TAG, "Backup WiFi ayarlari silindi (NVS + hafiza)");
    return ESP_OK;
}
