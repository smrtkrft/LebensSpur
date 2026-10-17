/**
 * LebensSpur - WiFi Manager
 *
 * Akilli WiFi baglanti yonetimi:
 *   1. Boot: AP mod baslar, sonra STA baglantisi denenir
 *   2. Baglanmadan once ag taramasi yapilir
 *   3. Taramada primary bulunursa -> primary'ye baglan
 *   4. Taramada sadece backup bulunursa -> backup'a baglan
 *   5. Hicbiri bulunamazsa -> gizli ag olabilir, direkt dene
 *   6. Baglanti koparsa -> zamanlayici ile yeniden baglan (event gorevi bloklanmaz)
 *   7. Tum denemeler basarisizsa -> 15 sn'de bir tekrar tara (sonsuz)
 *   8. AP modu kullanici kapattiysa asla geri acilmaz
 *
 * Guc yonetimi (TX gucu):
 *   WiFi TX aninda cekilen pik akim karsi cihazdan bagimsizdir; TX gucune (dBm)
 *   baglidir. Zayif beslemede bu pik brownout'a yol acar. Bu yuzden:
 *   - Acilista radyo 8 dBm ile acilir (yumusak baslangic), sonra tavana cikilir
 *   - Tavan varsayilan 17 dBm (20 yerine), kullanici 8..20 arasi ayarlayabilir
 *   - Uyarlanabilir mod: bagli linklerin en zayif RSSI'sine gore gereken guc,
 *     tavanla sinirli. Yukselme aninda, dusurme histerezisli.
 *   - AP'ye istemci baglandigi an (telefon yakindadir, GUI yuklenecek) guc
 *     14 dBm'e cekilir; RSSI olculunce gerekirse yukseltilir.
 */

#include "wifi_manager.h"
#include "sys_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
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
    wifi_static_cfg_t st;
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
static bool s_retry_exhausted_logged = false;
static esp_timer_handle_t s_retry_timer = NULL;   // STA retry gecikmesi

/* ========== TX Gucu / Guc Yonetimi ========== */
#define TX_EVAL_PERIOD_MS       10000   // Uyarlanabilir degerlendirme araligi
#define TX_KICK_DELAY_MS        2500    // Olay sonrasi hizli degerlendirme
#define TX_LOWER_STREAK         2       // Dusurmek icin ardisik ornek (histerezis)

static int  s_tx_max_dbm = WIFI_TX_DBM_DEFAULT;
static bool s_tx_adaptive = true;
static int  s_tx_now_dbm = 0;
static int  s_tx_lower_streak = 0;
static int  s_ap_clients = 0;
static esp_timer_handle_t s_tx_timer = NULL;      // periyodik
static esp_timer_handle_t s_tx_kick_timer = NULL; // tek seferlik
static bool s_sntp_started = false;

/* ========== Forward Declarations ========== */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
static void apply_static_ip(const wifi_static_cfg_t *st);
static void start_reconnect_task(void);
static void stop_reconnect_task(void);
static void tx_power_apply(int dbm);
static void tx_power_evaluate(void *arg);
static void tx_power_kick(void);
static void sta_retry_cb(void *arg);
static void update_log_ctx(void);
static void start_sntp(void);

/* ========== NVS ========== */

static void nvs_get_str_or_empty(nvs_handle_t nvs, const char *key, char *out, size_t size)
{
    size_t len = size;
    if (nvs_get_str(nvs, key, out, &len) != ESP_OK) out[0] = '\0';
}

static void load_cred(nvs_handle_t nvs, const char *prefix, wifi_cred_t *c)
{
    char key[16];
    snprintf(key, sizeof(key), "%sssid", prefix);  nvs_get_str_or_empty(nvs, key, c->ssid, sizeof(c->ssid));
    snprintf(key, sizeof(key), "%spass", prefix);  nvs_get_str_or_empty(nvs, key, c->pass, sizeof(c->pass));
    snprintf(key, sizeof(key), "%sip", prefix);    nvs_get_str_or_empty(nvs, key, c->st.ip, sizeof(c->st.ip));
    snprintf(key, sizeof(key), "%sgw", prefix);    nvs_get_str_or_empty(nvs, key, c->st.gw, sizeof(c->st.gw));
    snprintf(key, sizeof(key), "%smask", prefix);  nvs_get_str_or_empty(nvs, key, c->st.mask, sizeof(c->st.mask));
    snprintf(key, sizeof(key), "%sdns", prefix);   nvs_get_str_or_empty(nvs, key, c->st.dns, sizeof(c->st.dns));
}

static void set_or_erase(nvs_handle_t nvs, const char *key, const char *val)
{
    if (val && *val) nvs_set_str(nvs, key, val);
    else nvs_erase_key(nvs, key);
}

static void save_cred(nvs_handle_t nvs, const char *prefix, const wifi_cred_t *c)
{
    char key[16];
    snprintf(key, sizeof(key), "%sssid", prefix);  set_or_erase(nvs, key, c->ssid);
    snprintf(key, sizeof(key), "%spass", prefix);  set_or_erase(nvs, key, c->pass);
    snprintf(key, sizeof(key), "%sip", prefix);    set_or_erase(nvs, key, c->st.ip);
    snprintf(key, sizeof(key), "%sgw", prefix);    set_or_erase(nvs, key, c->st.gw);
    snprintf(key, sizeof(key), "%smask", prefix);  set_or_erase(nvs, key, c->st.mask);
    snprintf(key, sizeof(key), "%sdns", prefix);   set_or_erase(nvs, key, c->st.dns);
}

// NVS'den WiFi bilgilerini oku. Anahtarlar: p_* (primary), b_* (backup).
// Eski surum anahtarlari (ssid/password/static_ip, backup_*) bir kez tasinir.
static void load_credentials_from_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &nvs) != ESP_OK) return;

    load_cred(nvs, "p_", &s_primary);
    load_cred(nvs, "b_", &s_backup);

    /* Eski anahtar tasima (tek sefer) */
    char tmp[65];
    size_t len = sizeof(tmp);
    if (s_primary.ssid[0] == '\0' && nvs_get_str(nvs, "ssid", tmp, &len) == ESP_OK && tmp[0]) {
        strncpy(s_primary.ssid, tmp, sizeof(s_primary.ssid) - 1);
        nvs_get_str_or_empty(nvs, "password", s_primary.pass, sizeof(s_primary.pass));
        nvs_get_str_or_empty(nvs, "static_ip", s_primary.st.ip, sizeof(s_primary.st.ip));
        save_cred(nvs, "p_", &s_primary);
        nvs_erase_key(nvs, "ssid"); nvs_erase_key(nvs, "password"); nvs_erase_key(nvs, "static_ip");
        ESP_LOGI(TAG, "Eski primary WiFi kaydi yeni anahtarlara tasindi");
    }
    len = sizeof(tmp);
    if (s_backup.ssid[0] == '\0' && nvs_get_str(nvs, "backup_ssid", tmp, &len) == ESP_OK && tmp[0]) {
        strncpy(s_backup.ssid, tmp, sizeof(s_backup.ssid) - 1);
        nvs_get_str_or_empty(nvs, "backup_pass", s_backup.pass, sizeof(s_backup.pass));
        nvs_get_str_or_empty(nvs, "backup_ip", s_backup.st.ip, sizeof(s_backup.st.ip));
        save_cred(nvs, "b_", &s_backup);
        nvs_erase_key(nvs, "backup_ssid"); nvs_erase_key(nvs, "backup_pass"); nvs_erase_key(nvs, "backup_ip");
        ESP_LOGI(TAG, "Eski backup WiFi kaydi yeni anahtarlara tasindi");
    }
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "NVS yuklu - Primary:'%s'(pass:%d,ip:'%s') Backup:'%s'(pass:%d,ip:'%s')",
             s_primary.ssid, (int)strlen(s_primary.pass), s_primary.st.ip,
             s_backup.ssid, (int)strlen(s_backup.pass), s_backup.st.ip);
}

/* ========== IP / Mod yardimcilari ========== */

// Statik IP uygula (gw/mask/dns bos ise varsayilan turet)
static void apply_static_ip(const wifi_static_cfg_t *st)
{
    if (!st || st->ip[0] == '\0') {
        esp_netif_dhcpc_start(s_sta_netif);
        return;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_str_to_ip4(st->ip, &ip_info.ip) != ESP_OK) {
        ESP_LOGW(TAG, "Gecersiz statik IP '%s', DHCP kullaniliyor", st->ip);
        esp_netif_dhcpc_start(s_sta_netif);
        return;
    }
    if (st->mask[0] == '\0' || esp_netif_str_to_ip4(st->mask, &ip_info.netmask) != ESP_OK) {
        esp_netif_str_to_ip4("255.255.255.0", &ip_info.netmask);
    }
    if (st->gw[0] == '\0' || esp_netif_str_to_ip4(st->gw, &ip_info.gw) != ESP_OK) {
        ip_info.gw.addr = (ip_info.ip.addr & ip_info.netmask.addr) | htonl(0x00000001);
    }

    esp_netif_dhcpc_stop(s_sta_netif);
    esp_netif_set_ip_info(s_sta_netif, &ip_info);

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    if (st->dns[0] != '\0' && esp_netif_str_to_ip4(st->dns, (esp_ip4_addr_t *)&dns.ip.u_addr.ip4) == ESP_OK) {
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
        dns.ip.u_addr.ip4.addr = ip_info.gw.addr;
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns);
    } else {
        dns.ip.u_addr.ip4.addr = ip_info.gw.addr;
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
        dns.ip.u_addr.ip4.addr = ipaddr_addr("8.8.8.8");
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns);
    }

    ESP_LOGI(TAG, "Statik IP: %s mask " IPSTR " gw " IPSTR " dns %s",
             st->ip, IP2STR(&ip_info.netmask), IP2STR(&ip_info.gw),
             st->dns[0] ? st->dns : "gw+8.8.8.8");
}

static void build_ap_config(wifi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->ap.channel = WIFI_AP_CHANNEL;
    cfg->ap.max_connection = WIFI_AP_MAX_CONN;
    cfg->ap.authmode = WIFI_AUTH_OPEN;
    strncpy((char *)cfg->ap.ssid, s_device_name, sizeof(cfg->ap.ssid) - 1);
    cfg->ap.ssid_len = strlen(s_device_name);
}

// AP modunu bozmadan STA arayuzunu aktif et
static void ensure_sta_mode(void)
{
    wifi_mode_t cur = WIFI_MODE_NULL;
    esp_wifi_get_mode(&cur);
    wifi_config_t ap_cfg;

    switch (cur) {
    case WIFI_MODE_AP:
        esp_wifi_set_mode(s_ap_enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA);
        ESP_LOGI(TAG, "Mod: AP -> %s", s_ap_enabled ? "APSTA" : "STA");
        break;
    case WIFI_MODE_APSTA:
        break;
    case WIFI_MODE_STA:
        if (s_ap_enabled) {
            build_ap_config(&ap_cfg);
            esp_wifi_set_mode(WIFI_MODE_APSTA);
            esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
            ESP_LOGI(TAG, "Mod: STA -> APSTA");
        }
        break;
    default:
        ESP_LOGW(TAG, "WiFi modu NULL, yeniden baslatiliyor");
        if (s_ap_enabled) {
            build_ap_config(&ap_cfg);
            esp_wifi_set_mode(WIFI_MODE_APSTA);
            esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        } else {
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
        esp_wifi_start();
        break;
    }
    update_log_ctx();
}

static void build_sta_config(wifi_config_t *cfg, const char *ssid, const char *pass)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy((char *)cfg->sta.ssid, ssid, sizeof(cfg->sta.ssid) - 1);
    if (pass && *pass) strncpy((char *)cfg->sta.password, pass, sizeof(cfg->sta.password) - 1);
    cfg->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    /* Zayif sinyalli AP'lere de baglanabilsin; sifresiz ag icin esik OPEN */
    cfg->sta.threshold.authmode = (pass && *pass) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg->sta.threshold.rssi = -95;
}

// Belirli bir credential ile baglan (wifi durdurma/baslatma YAPMAZ)
static void do_connect(const wifi_cred_t *cred)
{
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));  // WiFi stack'in temizlenmesini bekle

    wifi_config_t sta_cfg;
    build_sta_config(&sta_cfg, cred->ssid, cred->pass);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    apply_static_ip(&cred->st);

    strncpy(s_sta_ssid, cred->ssid, sizeof(s_sta_ssid) - 1);
    s_retry_count = 0;

    ESP_LOGI(TAG, "Baglaniliyor: '%s' (IP:%s)", cred->ssid, cred->st.ip[0] ? cred->st.ip : "DHCP");
    sys_log_mark_event(SL_EV_STA_CONNECTING, 0);
    esp_wifi_connect();
}

/* ========== Ag Tarama ve Akilli Baglanti ========== */

static void scan_and_connect(void)
{
    bool has_pri = (strlen(s_primary.ssid) > 0);
    bool has_bk = (strlen(s_backup.ssid) > 0);

    if (!has_pri && !has_bk) {
        ESP_LOGW(TAG, "Kayitli WiFi yok, baglanti denemesi yapilmiyor");
        return;
    }

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
                    if (has_pri && strcmp(ssid, s_primary.ssid) == 0) { pri_found = true; pri_rssi = list[i].rssi; }
                    if (has_bk && strcmp(ssid, s_backup.ssid) == 0)   { bk_found = true;  bk_rssi = list[i].rssi; }
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

    if (pri_found) {
        ESP_LOGI(TAG, "Primary '%s' bulundu (RSSI:%d)", s_primary.ssid, pri_rssi);
        s_trying_backup = false;
        do_connect(&s_primary);
    } else if (bk_found) {
        ESP_LOGI(TAG, "Backup '%s' bulundu (RSSI:%d)", s_backup.ssid, bk_rssi);
        s_trying_backup = true;
        do_connect(&s_backup);
    } else if (has_pri) {
        ESP_LOGI(TAG, "SSID bulunamadi - gizli ag? Direkt deneniyor: '%s'", s_primary.ssid);
        s_trying_backup = false;
        do_connect(&s_primary);
    } else {
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
        if (s_retry_count > 0) continue;   // Retry devam ediyorsa atlama

        attempt++;
        ESP_LOGI(TAG, "--- Periyodik ag taramasi (#%d) ---", attempt);
        scan_and_connect();
    }
}

static void start_reconnect_task(void)
{
    if (s_reconnect_task != NULL) return;
    xTaskCreate(reconnect_task_fn, "wifi_reconn", 4096, NULL, 2, &s_reconnect_task);
}

static void stop_reconnect_task(void)
{
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
        if (s_mdns_hostname[i] >= 'A' && s_mdns_hostname[i] <= 'Z') s_mdns_hostname[i] += 32;
    }
    mdns_hostname_set(s_mdns_hostname);
    mdns_instance_name_set(s_device_name);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", s_mdns_hostname);
}

/* ========== WiFi Event Handler ========== */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e = data;
            s_ap_clients++;
            ESP_LOGI(TAG, "AP: cihaz baglandi (AID=%d, toplam %d)", e->aid, s_ap_clients);
            /* Brownout analizi icin kritik baglam: istemci baglandi -> GUI yuklenecek */
            sys_log_mark_event(SL_EV_AP_CLIENT_JOIN, e->aid);
            update_log_ctx();
            sys_log_write("INFO", "wifi", "AP: istemci baglandi (AID %d, toplam %d, tx %d dBm)",
                          e->aid, s_ap_clients, s_tx_now_dbm);
            /* On tedbir: AP'ye baglanan istemci (telefon) hemen hemen her zaman
             * yakindadir ve ardindan GUI yuklenir (yogun TX). RSSI olculene kadar
             * beklemeden gucu makul bir seviyeye cek; RSSI degerlendirmesi sonra
             * gerekirse yukseltir. Kritik ilk 15 sn penceresini bu kapatir. */
            if (s_tx_adaptive && s_tx_now_dbm > 14) {
                tx_power_apply(14);
            }
            tx_power_kick();
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *e = data;
            if (s_ap_clients > 0) s_ap_clients--;
            ESP_LOGI(TAG, "AP: cihaz ayrildi (AID=%d, kalan %d)", e->aid, s_ap_clients);
            sys_log_mark_event(SL_EV_AP_CLIENT_LEAVE, e->aid);
            update_log_ctx();
            tx_power_kick();
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *dis = data;
            bool was_connected = s_is_connected;
            s_is_connected = false;

            ESP_LOGW(TAG, "DISCONNECT '%s' reason:%d", dis->ssid, dis->reason);
            sys_log_mark_event(SL_EV_STA_LOST, dis->reason);
            update_log_ctx();

            // reason 2/8 = normal ayrilma (biz disconnect cagirdik) -> retry yapma
            if (dis->reason == WIFI_REASON_ASSOC_LEAVE || dis->reason == WIFI_REASON_AUTH_LEAVE) {
                break;
            }

            /* Gercek kopma (bagliyken dustu): tek satir kaydet, retry spam'i yok */
            if (was_connected) {
                s_retry_exhausted_logged = false;
                sys_log_write("WARN", "wifi", "WiFi koptu: '%s' (reason %d, rssi %d, tx %d dBm)",
                              (const char *)dis->ssid, dis->reason, dis->rssi, s_tx_now_dbm);
            }

            if (s_retry_count < RETRY_MAX) {
                s_retry_count++;
                int delay_ms = RETRY_BASE_MS * s_retry_count;  // 1s, 2s, 3s, 4s, 5s
                ESP_LOGW(TAG, "Retry %d/%d (%d ms sonra)...", s_retry_count, RETRY_MAX, delay_ms);
                /* Event task'ini bloklamak AP/IP/mDNS olaylarini 5 sn'ye kadar
                 * geciktiriyordu; gecikme zamanlayiciya tasindi. */
                if (s_retry_timer) {
                    esp_timer_stop(s_retry_timer);
                    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000ULL);
                } else {
                    esp_wifi_connect();
                }
            } else if (!s_trying_backup && strlen(s_backup.ssid) > 0) {
                ESP_LOGW(TAG, "Primary basarisiz. Backup deneniyor: '%s'", s_backup.ssid);
                s_trying_backup = true;
                do_connect(&s_backup);
            } else {
                ESP_LOGE(TAG, "Tum denemeler basarisiz. %d sn sonra tekrar taranacak.",
                         RECONNECT_INTERVAL_MS / 1000);
                if (!s_retry_exhausted_logged) {
                    s_retry_exhausted_logged = true;
                    sys_log_write("WARN", "wifi", "WiFi baglanamadi: '%s' (son reason %d), %d sn'de bir taranacak",
                                  (const char *)dis->ssid, dis->reason, RECONNECT_INTERVAL_MS / 1000);
                }
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
        s_is_connected = true;
        s_retry_count = 0;
        s_retry_exhausted_logged = false;
        stop_reconnect_task();

        /* Power save kapat - zayif sinyalde stabilite icin.
         * Not: modem-sleep ortalama akimi dusurur ama pik akimi degistirmez;
         * pik icin TX gucu yonetimi (tx_power_*) kullanilir. */
        esp_wifi_set_ps(WIFI_PS_NONE);

        {
            int rssi = wifi_manager_get_sta_rssi();
            sys_log_mark_event(SL_EV_STA_GOT_IP, rssi);
            update_log_ctx();
            sys_log_write("INFO", "wifi", "WiFi baglandi: '%s' IP %s (%s, rssi %d, tx %d dBm)",
                          s_sta_ssid, s_ip_addr, s_trying_backup ? "backup" : "primary",
                          rssi, s_tx_now_dbm);
        }
        s_trying_backup = false;
        tx_power_kick();
        start_sntp();

        /* STA IP alindi - mDNS'i bu arayuzde yeniden duyur */
        if (strlen(s_mdns_hostname) > 0) {
            mdns_hostname_set(s_mdns_hostname);
        }
    }
}

/* ========== Public API ========== */

esp_err_t wifi_manager_init(const char *device_name)
{
    ESP_LOGI(TAG, "WiFi manager baslatiliyor...");
    strncpy(s_device_name, device_name, sizeof(s_device_name) - 1);

    // NVS'den AP durumu + TX gucu ayarlari
    nvs_handle_t nvs;
    if (nvs_open("wifi_cfg", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t ap_val = 1;
        nvs_get_u8(nvs, "ap_enabled", &ap_val);
        s_ap_enabled = (ap_val != 0);
        uint8_t v = WIFI_TX_DBM_DEFAULT;
        if (nvs_get_u8(nvs, "tx_max", &v) == ESP_OK && v >= WIFI_TX_DBM_MIN && v <= WIFI_TX_DBM_MAX) {
            s_tx_max_dbm = v;
        }
        uint8_t a = 1;
        nvs_get_u8(nvs, "tx_adapt", &a);
        s_tx_adaptive = (a != 0);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "AP modu: %s | TX gucu: tavan %d dBm, uyarlanabilir %s",
             s_ap_enabled ? "ACIK" : "KAPALI", s_tx_max_dbm, s_tx_adaptive ? "ACIK" : "KAPALI");

    load_credentials_from_nvs();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    /* Zamanlayicilar: retry, TX periyodik, TX olay */
    const esp_timer_create_args_t ra = { .callback = sta_retry_cb, .name = "sta_retry" };
    esp_timer_create(&ra, &s_retry_timer);
    const esp_timer_create_args_t pa = { .callback = tx_power_evaluate, .name = "tx_eval" };
    esp_timer_create(&pa, &s_tx_timer);
    const esp_timer_create_args_t ka = { .callback = tx_power_evaluate, .name = "tx_kick" };
    esp_timer_create(&ka, &s_tx_kick_timer);

    start_mdns();
    return wifi_manager_start_ap();
}

esp_err_t wifi_manager_start_ap(void)
{
    wifi_config_t ap_cfg;
    build_ap_config(&ap_cfg);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Yumusak baslangic: radyo dusuk gucte acilir, kondansatorler dolduktan
     * sonra (TX_KICK_DELAY_MS) hedef guce cikilir. */
    tx_power_apply(WIFI_TX_DBM_BOOT);
    if (s_tx_timer) esp_timer_start_periodic(s_tx_timer, (uint64_t)TX_EVAL_PERIOD_MS * 1000ULL);
    tx_power_kick();

    snprintf(s_ip_addr, sizeof(s_ip_addr), "192.168.4.1");
    update_log_ctx();
    ESP_LOGI(TAG, "AP baslatildi - SSID: %s (Sifresiz, max %d istemci)", s_device_name, WIFI_AP_MAX_CONN);
    return ESP_OK;
}

static void copy_static(wifi_static_cfg_t *dst, const wifi_static_cfg_t *src)
{
    memset(dst, 0, sizeof(*dst));
    if (!src || src->ip[0] == '\0') return;
    strncpy(dst->ip, src->ip, sizeof(dst->ip) - 1);
    strncpy(dst->gw, src->gw, sizeof(dst->gw) - 1);
    strncpy(dst->mask, src->mask, sizeof(dst->mask) - 1);
    strncpy(dst->dns, src->dns, sizeof(dst->dns) - 1);
}

esp_err_t wifi_manager_connect_sta(const char *ssid, const char *password,
                                    const wifi_static_cfg_t *st)
{
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    // Sifre bos geldi ama ayni SSID icin kayitli sifre varsa koru
    char use_pass[65] = {0};
    if (password && strlen(password) > 0) {
        strncpy(use_pass, password, sizeof(use_pass) - 1);
    } else if (strcmp(s_primary.ssid, ssid) == 0 && strlen(s_primary.pass) > 0) {
        strncpy(use_pass, s_primary.pass, sizeof(use_pass) - 1);
        ESP_LOGI(TAG, "Sifre bos geldi - kayitli sifre korunuyor");
    }

    wifi_static_cfg_t new_st;
    copy_static(&new_st, st);

    // Zaten ayni SSID'ye bagliysa ve ayarlar degismemisse yeniden baglanma
    if (s_is_connected && strcmp(s_sta_ssid, ssid) == 0 &&
        strcmp(s_primary.pass, use_pass) == 0 &&
        memcmp(&s_primary.st, &new_st, sizeof(new_st)) == 0) {
        ESP_LOGI(TAG, "Zaten '%s' agina bagli, degisiklik yok - atlaniyor", ssid);
        return ESP_OK;
    }

    memset(&s_primary, 0, sizeof(s_primary));
    strncpy(s_primary.ssid, ssid, sizeof(s_primary.ssid) - 1);
    strncpy(s_primary.pass, use_pass, sizeof(s_primary.pass) - 1);
    s_primary.st = new_st;

    // NVS'ye kaydet (baglanti sonucundan bagimsiz: reboot sonrasi tekrar denensin)
    nvs_handle_t nvs;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &nvs) == ESP_OK) {
        save_cred(nvs, "p_", &s_primary);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    s_trying_backup = false;
    s_retry_count = 0;
    stop_reconnect_task();
    ensure_sta_mode();
    do_connect(&s_primary);

    ESP_LOGI(TAG, "STA baglanti baslatildi: '%s' (pass_len:%d)", ssid, (int)strlen(use_pass));
    return ESP_OK;
}

esp_err_t wifi_manager_auto_connect(void)
{
    if (strlen(s_primary.ssid) == 0 && strlen(s_backup.ssid) == 0) {
        ESP_LOGI(TAG, "Kayitli WiFi yok, AP modunda kaliniyor");
        return ESP_ERR_NOT_FOUND;
    }
    ensure_sta_mode();
    vTaskDelay(pdMS_TO_TICKS(500));   // WiFi stack hazir olsun
    scan_and_connect();
    return ESP_OK;
}

bool wifi_manager_has_saved_config(void) { return s_primary.ssid[0] != '\0'; }
bool wifi_manager_is_connected(void)     { return s_is_connected; }
const char *wifi_manager_get_ip(void)    { return s_ip_addr; }
const char *wifi_manager_get_device_name(void) { return s_device_name; }
const char *wifi_manager_get_ap_ssid(void)     { return s_device_name; }
const char *wifi_manager_get_sta_ssid(void)    { return s_sta_ssid[0] ? s_sta_ssid : s_primary.ssid; }
const wifi_static_cfg_t *wifi_manager_get_primary_static(void) { return &s_primary.st; }
const wifi_static_cfg_t *wifi_manager_get_backup_static(void)  { return &s_backup.st; }

esp_err_t wifi_manager_save_backup(const char *ssid, const char *password,
                                    const wifi_static_cfg_t *st)
{
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    char use_pass[65] = {0};
    if (password && strlen(password) > 0) {
        strncpy(use_pass, password, sizeof(use_pass) - 1);
    } else if (strcmp(s_backup.ssid, ssid) == 0 && strlen(s_backup.pass) > 0) {
        strncpy(use_pass, s_backup.pass, sizeof(use_pass) - 1);
        ESP_LOGI(TAG, "Backup sifre bos geldi - kayitli sifre korunuyor");
    }

    memset(&s_backup, 0, sizeof(s_backup));
    strncpy(s_backup.ssid, ssid, sizeof(s_backup.ssid) - 1);
    strncpy(s_backup.pass, use_pass, sizeof(s_backup.pass) - 1);
    copy_static(&s_backup.st, st);

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;
    save_cred(nvs, "b_", &s_backup);
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Backup WiFi kaydedildi: '%s' (pass_len:%d)", ssid, (int)strlen(use_pass));
    return ESP_OK;
}

bool wifi_manager_has_backup_config(void)    { return s_backup.ssid[0] != '\0'; }
const char *wifi_manager_get_backup_ssid(void) { return s_backup.ssid; }

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
        wifi_config_t ap_cfg;
        build_ap_config(&ap_cfg);
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        ESP_LOGI(TAG, "AP ACILDI (APSTA)");
        sys_log_write("INFO", "wifi", "AP modu acildi");
    } else if (!enabled && mode == WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_ap_clients = 0;
        ESP_LOGI(TAG, "AP KAPATILDI (STA)");
        sys_log_write("INFO", "wifi", "AP modu kapatildi (yalnizca router baglantisi)");
    }
    update_log_ctx();

    nvs_handle_t nvs;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "ap_enabled", enabled ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    return ESP_OK;
}

bool wifi_manager_is_ap_enabled(void) { return s_ap_enabled; }

esp_err_t wifi_manager_clear_primary(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;
    memset(&s_primary, 0, sizeof(s_primary));
    save_cred(nvs, "p_", &s_primary);
    nvs_commit(nvs);
    nvs_close(nvs);
    memset(s_sta_ssid, 0, sizeof(s_sta_ssid));
    ESP_LOGI(TAG, "Primary WiFi ayarlari silindi (NVS + hafiza)");
    return ESP_OK;
}

esp_err_t wifi_manager_clear_backup(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open("wifi_cfg", NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;
    memset(&s_backup, 0, sizeof(s_backup));
    save_cred(nvs, "b_", &s_backup);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Backup WiFi ayarlari silindi (NVS + hafiza)");
    return ESP_OK;
}

/* ========== TX Gucu Yonetimi ==========
 *
 * Kriter: "kac cihaz bagli" degil, "linkin ihtiyac duydugu guc".
 *   - Hic link yok  -> tavan (kesif/beacon icin menzil korunur)
 *   - Link var       -> en zayif linkin RSSI'sine gore gerekli guc, tavanla sinirli
 *   - Yukselme aninda, dusurme 2 ardisik ornekten sonra (histerezis)
 */

static int rssi_to_dbm(int rssi)
{
    if (rssi >= -55) return 8;    // cok yakin (telefon 1-2 m)
    if (rssi >= -62) return 11;
    if (rssi >= -70) return 14;
    if (rssi >= -76) return 17;
    return WIFI_TX_DBM_MAX;
}

static void tx_power_apply(int dbm)
{
    if (dbm < WIFI_TX_DBM_MIN) dbm = WIFI_TX_DBM_MIN;
    if (dbm > WIFI_TX_DBM_MAX) dbm = WIFI_TX_DBM_MAX;
    if (dbm == s_tx_now_dbm) return;

    esp_err_t err = esp_wifi_set_max_tx_power((int8_t)(dbm * 4));  // birim 0.25 dBm
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TX gucu ayarlanamadi (%d dBm): %s", dbm, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "TX gucu: %d -> %d dBm", s_tx_now_dbm, dbm);
    s_tx_now_dbm = dbm;
    update_log_ctx();
}

static void tx_power_evaluate(void *arg)
{
    (void)arg;
    int target = s_tx_max_dbm;

    if (s_tx_adaptive) {
        int worst = 0;          // 0 = link yok
        bool any = false;

        /* AP istemcileri */
        wifi_sta_list_t list;
        if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) {
            s_ap_clients = list.num;
            for (int i = 0; i < list.num; i++) {
                int r = list.sta[i].rssi;
                if (r == 0) continue;           // henuz olculmedi
                if (!any || r < worst) worst = r;
                any = true;
            }
        }
        /* STA (router) linki */
        wifi_ap_record_t ap;
        if (s_is_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            if (!any || ap.rssi < worst) worst = ap.rssi;
            any = true;
        }

        if (any) {
            int need = rssi_to_dbm(worst);
            if (need < target) target = need;
        }
    }

    if (target > s_tx_now_dbm) {
        s_tx_lower_streak = 0;
        tx_power_apply(target);             // yukselme: hemen
    } else if (target < s_tx_now_dbm) {
        if (++s_tx_lower_streak >= TX_LOWER_STREAK) {
            s_tx_lower_streak = 0;
            tx_power_apply(target);         // dusurme: histerezisli
        }
    } else {
        s_tx_lower_streak = 0;
    }
}

static void tx_power_kick(void)
{
    if (!s_tx_kick_timer) return;
    s_tx_lower_streak = TX_LOWER_STREAK - 1;    /* olay tetikli degerlendirme: histerezis tek adim */
    esp_timer_stop(s_tx_kick_timer);
    esp_timer_start_once(s_tx_kick_timer, (uint64_t)TX_KICK_DELAY_MS * 1000ULL);
}

esp_err_t wifi_manager_set_tx_power(int max_dbm, bool adaptive)
{
    if (max_dbm < WIFI_TX_DBM_MIN || max_dbm > WIFI_TX_DBM_MAX) return ESP_ERR_INVALID_ARG;
    s_tx_max_dbm = max_dbm;
    s_tx_adaptive = adaptive;
    s_tx_lower_streak = TX_LOWER_STREAK;    // yeni tavan hemen uygulansin
    tx_power_evaluate(NULL);
    sys_log_mark_event(SL_EV_TX_POWER, s_tx_now_dbm);
    sys_log_write("INFO", "wifi", "TX gucu ayari: tavan %d dBm, uyarlanabilir %s (simdi %d dBm)",
                  max_dbm, adaptive ? "acik" : "kapali", s_tx_now_dbm);

    nvs_handle_t nvs;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "tx_max", (uint8_t)max_dbm);
        nvs_set_u8(nvs, "tx_adapt", adaptive ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    return ESP_OK;
}

int  wifi_manager_get_tx_power_max(void)     { return s_tx_max_dbm; }
int  wifi_manager_get_tx_power_now(void)     { return s_tx_now_dbm; }
bool wifi_manager_get_tx_adaptive(void)      { return s_tx_adaptive; }
int  wifi_manager_get_ap_client_count(void)  { return s_ap_clients; }

int wifi_manager_get_sta_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_is_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

static void sta_retry_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

/* ========== sys_log baglami ========== */

static void update_log_ctx(void)
{
    wifi_mode_t m = WIFI_MODE_NULL;
    esp_wifi_get_mode(&m);
    sys_log_wifi_mode_t code = SL_WIFI_OFF;
    if (m == WIFI_MODE_AP) code = SL_WIFI_AP;
    else if (m == WIFI_MODE_STA) code = SL_WIFI_STA;
    else if (m == WIFI_MODE_APSTA) code = SL_WIFI_APSTA;
    sys_log_set_wifi_ctx(code, (uint8_t)s_ap_clients, (int8_t)wifi_manager_get_sta_rssi(),
                         (int8_t)s_tx_now_dbm);
}

/* ========== SNTP (log zaman damgasi icin) ========== */

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    sys_log_time_synced();
}

static void start_sntp(void)
{
    if (s_sntp_started) return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = sntp_sync_cb;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP baslatildi (pool.ntp.org)");
    }
}
