#include "sk_wifi.h"
#include "sk_auth.h"
#include "sk_capabilities.h"
#include "sk_cli.h"
#include "sk_cli_internal.h"   // wifi.scan worker captures writer + machine_id
                               // off ctx — needs the struct definition.
#include "sk_errors.h"
#include "sk_event_bus.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sk_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "sk_wifi";

#define NVS_NS           "sk_wifi"
#define KEY_PRIMARY_SSID "p_ssid"
#define KEY_PRIMARY_PASS "p_pass"
#define KEY_PRIMARY_IP   "p_ip"
#define KEY_BACKUP_SSID  "b_ssid"
#define KEY_BACKUP_PASS  "b_pass"
#define KEY_BACKUP_IP    "b_ip"

#define BIT_CONNECTED   BIT0
#define BIT_FAIL        BIT1

static esp_netif_t        *s_netif = NULL;
static EventGroupHandle_t  s_evt   = NULL;
static char                s_ssid[SK_WIFI_SSID_MAX + 1] = {0};
static char                s_ip[16] = "0.0.0.0";
static int                 s_rssi  = 0;
static bool                s_static_ip = false;
static bool                s_ready = false;

// Sticky disconnect intent. Set by sk_wifi_disconnect (= user said "stop")
// and cleared the moment sk_wifi_connect_sta runs (= user said "connect to
// X"). While true, the WIFI_EVENT_STA_DISCONNECTED handler stops calling
// esp_wifi_connect() — which previously made `wifi.disconnect` a no-op
// because the disconnect event itself triggered an immediate reconnect.
static bool                s_disconnect_pinned = false;

// Which slot the live connection is using:
//   0 = none / never connected, 1 = primary, 2 = backup.
// `wifi.status` exposes this so the user can see *where* the device
// actually landed, not just "what's saved".
static int                 s_active_slot = 0;

// Consecutive STA_DISCONNECTED events without a GOT_IP in between.
// Used by maybe_swap_slot() to fall over from primary→backup (and back)
// after FAILS_BEFORE_SWAP attempts. Reset on every successful GOT_IP.
static int                 s_consecutive_fails = 0;
#define FAILS_BEFORE_SWAP  5

// GOT_IP sonrası BLE advertising'i geri açmak için defer timer. sys_evt
// task'inin 2KB stack'i event-bus publish'i kaldıramadığı için ayrı bir
// timer task'inde çalışıyor.
static esp_timer_handle_t  s_resume_ble_timer = NULL;
static void resume_ble_after_wifi_cb(void *arg) {
    (void)arg;
    sk_event_bus_publish("ble.resume.after-wifi", NULL);
}

// Forward decls — definitions live below (after the public API and CLI handlers).
static void wifi_register_cli_and_hooks(void);
static esp_err_t connect_slot(int slot);   // 1=primary, 2=backup; ESP_ERR_NOT_FOUND if empty

static const char *slot_name(int slot)
{
    switch (slot) {
    case 1:  return "primary";
    case 2:  return "backup";
    default: return "none";
    }
}

static void publish_state(const char *state)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"static\":%s,\"slot\":\"%s\"}",
             state, s_ssid, s_ip, s_rssi,
             s_static_ip ? "true" : "false",
             slot_name(s_active_slot));
    sk_event_bus_publish("wifi.state", buf);
}

static void wifi_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            // Only attempt connect if a target SSID has been set (either
            // by sk_wifi_connect_sta or by auto-connect from NVS). Without
            // this gate, sk_wifi_init's unconditional esp_wifi_start would
            // trigger an empty-SSID connect → DISCONNECTED → retry → loop.
            if (s_ssid[0]) esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            // RSSI not in the connected event payload; query the AP info
            // record so the structured log can show signal strength.
            wifi_ap_record_t ap;
            int rssi = 0;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                rssi = ap.rssi;
                s_rssi = rssi;
            }
            SK_LOG_I("wifi", "connect",
                     "ssid=%s rssi=%d slot=%s",
                     s_ssid, rssi, slot_name(s_active_slot));
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *de = (wifi_event_sta_disconnected_t *)data;
            int reason = de ? (int)de->reason : 0;
            s_ip[0] = '\0'; strcpy(s_ip, "0.0.0.0"); s_rssi = 0;
            xEventGroupSetBits(s_evt, BIT_FAIL);
            publish_state("disconnected");

            SK_LOG_W("wifi", "disconnect",
                     "ssid=%s reason=%d retries=%d",
                     s_ssid, reason, s_consecutive_fails);

            // Honour explicit "stay disconnected" intent from CLI.
            if (s_disconnect_pinned) break;
            // No SSID set yet — first boot with no creds. Don't busy-loop.
            if (!s_ssid[0]) break;

            // After FAILS_BEFORE_SWAP consecutive failures on the active
            // slot, try the OTHER slot (if it has credentials). Counter
            // resets on GOT_IP. The swap itself triggers a fresh
            // STA_START → CONNECT cycle, so we only call esp_wifi_connect
            // on the no-swap path.
            s_consecutive_fails++;
            int other = (s_active_slot == 1) ? 2 : (s_active_slot == 2) ? 1 : 0;
            if (s_consecutive_fails >= FAILS_BEFORE_SWAP && other != 0) {
                SK_LOG_E("wifi", "connect.fail",
                         "ssid=%s reason=%d attempts=%d",
                         s_ssid, reason, s_consecutive_fails);
                if (connect_slot(other) == ESP_OK) {
                    s_consecutive_fails = 0;   // give the new slot a clean budget
                    break;
                }
                // Other slot empty — keep grinding on the active one.
            } else if (s_consecutive_fails >= FAILS_BEFORE_SWAP) {
                // No other slot to fall back to — total fail.
                SK_LOG_E("wifi", "connect.fail",
                         "ssid=%s reason=%d attempts=%d",
                         s_ssid, reason, s_consecutive_fails);
            }
            esp_wifi_connect();
            break;
        }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        xEventGroupSetBits(s_evt, BIT_CONNECTED);
        s_consecutive_fails = 0;   // good slot, reset the swap budget
        // BLE advertising'i burada AÇMA — sys_evt task'inin 2KB stack'i
        // event_bus_publish + NimBLE çağrı zincirini kaldıramıyor (stack
        // protection fault). Bunun yerine ayrı bir esp_timer ile defer
        // edip büyük stack'li task'a postpone ediyoruz.
        if (s_resume_ble_timer) {
            esp_timer_start_once(s_resume_ble_timer, 100 * 1000);
        }
        // Switch the radio to maximum modem-sleep as soon as we have
        // an IP. WIFI_PS_MAX_MODEM lets the chip sleep between DTIM
        // beacons; current draws drop from ~80 mA to ~3-15 mA while
        // staying associated. We do this AFTER GOT_IP because some
        // routers reject association attempts in PS mode.
        esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
        if (ps_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_ps(MAX_MODEM) failed: %s",
                     esp_err_to_name(ps_err));
        }
        publish_state("connected");
        char payload[64];
        snprintf(payload, sizeof(payload), "{\"ip\":\"%s\"}", s_ip);
        sk_event_bus_publish("wifi.ip.acquired", payload);
        SK_LOG_I("wifi", "ip.acquired", "ip=%s", s_ip);
    }
}

esp_err_t sk_wifi_init(void)
{
    if (s_ready) return ESP_OK;

    // Defer-timer for resuming BLE advertising after GOT_IP. Created
    // here once; started inside the IP event handler.
    const esp_timer_create_args_t resume_args = {
        .callback = resume_ble_after_wifi_cb,
        .name     = "sk_wifi_resume_ble",
    };
    esp_timer_create(&resume_args, &s_resume_ble_timer);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    // s_evt has to exist BEFORE the disconnect handler can fire — the
    // handler dereferences it via xEventGroupSetBits. Earlier code
    // created s_evt after esp_event_handler_register; safe today only
    // because esp_wifi_start hadn't been called yet, but a single line
    // moved out of order would NULL-deref the bus.
    s_evt = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_cb, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_cb, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    sk_capabilities_register_book("sk_network", "0.1.0");
    wifi_register_cli_and_hooks();

    // Start the wifi driver unconditionally — this is what enables
    // operations like wifi.scan and wifi.connect to work on a fresh
    // device with no provisioned credentials. The empty-SSID connect
    // is gated inside wifi_event_cb so we don't busy-loop.
    ESP_ERROR_CHECK(esp_wifi_start());

    s_ready = true;

    // Best-effort auto-connect. Missing credentials are normal on first
    // boot — the user provisions WiFi later via BLE pairing or the
    // `wifi.connect` CLI command. Init success means "WiFi stack is up
    // and ready", not "we are connected" — caller must not treat
    // ESP_ERR_NOT_FOUND from auto_connect as an init failure.
    esp_err_t auto_err = sk_wifi_auto_connect();
    if (auto_err != ESP_OK && auto_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "auto-connect failed: %s", esp_err_to_name(auto_err));
    }
    return ESP_OK;
}

// -- Credential storage -----------------------------------------------------

static esp_err_t save_cred(const char *k_ssid, const char *k_pass, const char *k_ip,
                           const sk_wifi_cred_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, k_ssid, c->ssid);
    nvs_set_str(h, k_pass, c->password);
    nvs_set_str(h, k_ip,   c->static_ip);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

static bool load_cred(const char *k_ssid, const char *k_pass, const char *k_ip,
                      sk_wifi_cred_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz;
    sz = sizeof(c->ssid);     if (nvs_get_str(h, k_ssid, c->ssid,     &sz) != ESP_OK) { nvs_close(h); return false; }
    sz = sizeof(c->password); nvs_get_str(h, k_pass, c->password, &sz);
    sz = sizeof(c->static_ip); nvs_get_str(h, k_ip,   c->static_ip, &sz);
    nvs_close(h);
    return c->ssid[0] != '\0';
}

bool sk_wifi_has_primary(void) { sk_wifi_cred_t c; return load_cred(KEY_PRIMARY_SSID, KEY_PRIMARY_PASS, KEY_PRIMARY_IP, &c); }
bool sk_wifi_has_backup(void)  { sk_wifi_cred_t c; return load_cred(KEY_BACKUP_SSID,  KEY_BACKUP_PASS,  KEY_BACKUP_IP,  &c); }
esp_err_t sk_wifi_save_primary(const sk_wifi_cred_t *c) { return save_cred(KEY_PRIMARY_SSID, KEY_PRIMARY_PASS, KEY_PRIMARY_IP, c); }
esp_err_t sk_wifi_save_backup (const sk_wifi_cred_t *c) { return save_cred(KEY_BACKUP_SSID,  KEY_BACKUP_PASS,  KEY_BACKUP_IP,  c); }

static esp_err_t clear_cred(const char *k_ssid, const char *k_pass, const char *k_ip)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_ERR_NOT_FOUND;
    nvs_erase_key(h, k_ssid); nvs_erase_key(h, k_pass); nvs_erase_key(h, k_ip);
    nvs_commit(h); nvs_close(h);
    return ESP_OK;
}

esp_err_t sk_wifi_clear_primary(void) { return clear_cred(KEY_PRIMARY_SSID, KEY_PRIMARY_PASS, KEY_PRIMARY_IP); }
esp_err_t sk_wifi_clear_backup(void)  { return clear_cred(KEY_BACKUP_SSID,  KEY_BACKUP_PASS,  KEY_BACKUP_IP);  }

// -- Connect flow -----------------------------------------------------------

// Parse "192.168.1.50/24" or bare "192.168.1.50" (default /24) into
// ip + netmask + an inferred gateway (network address with the host
// portion = 1, the de-facto convention for residential routers).
// Returns ESP_OK on success; non-OK leaves info untouched.
static esp_err_t parse_static_ip(const char *spec, esp_netif_ip_info_t *info)
{
    if (!spec || !*spec) return ESP_ERR_INVALID_ARG;
    int a, b, c_, d, prefix;
    int n = sscanf(spec, "%d.%d.%d.%d/%d", &a, &b, &c_, &d, &prefix);
    if (n == 4)      prefix = 24;            // bare IP → /24 default
    else if (n != 5) return ESP_ERR_INVALID_ARG;
    if (a < 0 || a > 255 || b < 0 || b > 255 || c_ < 0 || c_ > 255 || d < 0 || d > 255) return ESP_ERR_INVALID_ARG;
    if (prefix < 1 || prefix > 32) return ESP_ERR_INVALID_ARG;

    uint32_t ip   = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c_ << 8) | (uint32_t)d;
    uint32_t mask = (prefix == 32) ? 0xFFFFFFFFu : (~((1u << (32 - prefix)) - 1));
    uint32_t net  = ip & mask;
    uint32_t gw   = net | 1u;        // ".1" convention

    info->ip.addr      = esp_netif_htonl(ip);
    info->netmask.addr = esp_netif_htonl(mask);
    info->gw.addr      = esp_netif_htonl(gw);
    return ESP_OK;
}

esp_err_t sk_wifi_connect_sta(const sk_wifi_cred_t *c)
{
    if (!c) return ESP_ERR_INVALID_ARG;

    // Force a clean slate before reconfiguring. esp_wifi_set_config does
    // NOT abort an in-flight connect attempt — if the boot auto-connect
    // is mid-association with stale credentials, the driver happily
    // finishes that attempt and silently ignores the new SSID. This was
    // the long-tail "wifi.connect did nothing" bug. Pin the disconnect
    // intent so our STA_DISCONNECTED handler doesn't race-fire a
    // reconnect with stale config in the gap before we call connect()
    // ourselves below.
    s_disconnect_pinned = true;
    esp_wifi_disconnect();   // ESP_ERR_WIFI_NOT_CONNECT is fine, ignore
    // Brief settle time so the disconnect event lands before set_config.
    // Without this the driver can still be in CONNECTING and reject the
    // new config. 50 ms is comfortably above the dispatch latency.
    vTaskDelay(pdMS_TO_TICKS(50));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid,     c->ssid,     sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, c->password, sizeof(wc.sta.password) - 1);
    // Boot path'te abort etmemek için ESP_ERROR_CHECK YERİNE manuel
    // handling — bozuk SSID/pass NVS'te kayıtlıysa auto_connect zincirini
    // reboot loop'a sokmasın.
    esp_err_t cfg_err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (cfg_err != ESP_OK) {
        s_disconnect_pinned = false;     // restore so future calls work
        ESP_LOGE(TAG, "set_config failed: %s — bozuk credential?",
                 esp_err_to_name(cfg_err));
        return cfg_err;
    }

    // Static IP path: stop DHCP client, pin manual ip/netmask/gateway.
    // Empty / malformed spec falls back to DHCP (default behaviour).
    s_static_ip = false;
    if (c->static_ip[0] != '\0' && s_netif) {
        esp_netif_ip_info_t info = {0};
        esp_err_t pe = parse_static_ip(c->static_ip, &info);
        if (pe == ESP_OK) {
            esp_netif_dhcpc_stop(s_netif);     // ignore err if not running
            esp_err_t se = esp_netif_set_ip_info(s_netif, &info);
            if (se == ESP_OK) {
                s_static_ip = true;
                ESP_LOGI(TAG, "static ip pinned: %s", c->static_ip);
            } else {
                ESP_LOGW(TAG, "set_ip_info failed: %s", esp_err_to_name(se));
            }
        } else {
            ESP_LOGW(TAG, "static_ip parse failed for '%s' — falling back to DHCP",
                     c->static_ip);
            esp_netif_dhcpc_start(s_netif);
        }
    } else if (s_netif) {
        // Make sure DHCP is on when we don't have a static config —
        // otherwise a previous static-IP session leaves the netif stuck.
        esp_netif_dhcpc_start(s_netif);
    }

    strncpy(s_ssid, c->ssid, sizeof(s_ssid) - 1);
    s_disconnect_pinned = false;       // user expressed intent to connect
    ESP_LOGI(TAG, "connecting to %s", c->ssid);
    xEventGroupClearBits(s_evt, BIT_CONNECTED | BIT_FAIL);
    publish_state("connecting");
    // Driver is already running (started in sk_wifi_init). Trigger the
    // actual association now that we have a target SSID.
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) return err;
    return ESP_OK;
}

esp_err_t sk_wifi_disconnect(void)
{
    s_disconnect_pinned   = true;      // suppress the auto-reconnect leak
    s_active_slot         = 0;
    s_consecutive_fails   = 0;
    return esp_wifi_disconnect();
}

// Connect to a specific slot (1=primary, 2=backup). Returns ESP_ERR_NOT_FOUND
// if that slot is empty. Stamps s_active_slot so subsequent disconnect /
// fallback logic knows which side is live.
static esp_err_t connect_slot(int slot)
{
    sk_wifi_cred_t c;
    bool ok = (slot == 1)
              ? load_cred(KEY_PRIMARY_SSID, KEY_PRIMARY_PASS, KEY_PRIMARY_IP, &c)
              : (slot == 2)
                ? load_cred(KEY_BACKUP_SSID, KEY_BACKUP_PASS, KEY_BACKUP_IP, &c)
                : false;
    if (!ok) return ESP_ERR_NOT_FOUND;
    s_active_slot = slot;
    ESP_LOGI(TAG, "switching to %s slot (%s)", slot_name(slot), c.ssid);
    return sk_wifi_connect_sta(&c);
}

esp_err_t sk_wifi_auto_connect(void)
{
    if (connect_slot(1) == ESP_OK) return ESP_OK;
    if (connect_slot(2) == ESP_OK) return ESP_OK;
    ESP_LOGI(TAG, "no stored WiFi credentials");
    return ESP_ERR_NOT_FOUND;
}

void sk_wifi_status(sk_wifi_status_t *out)
{
    if (!out) return;
    // Callable from anywhere on the boot path — including before
    // sk_wifi_init runs (e.g. CLI banner printed by sk_transport_usb's
    // task, which is spawned earlier than sk_wifi_init in main.c).
    // Report a clean "off" snapshot in that window instead of dereferencing
    // a NULL event group.
    if (!s_evt) {
        memset(out, 0, sizeof(*out));
        strcpy(out->ip, "0.0.0.0");
        return;
    }
    out->connected = (xEventGroupGetBits(s_evt) & BIT_CONNECTED) != 0;
    strncpy(out->ssid, s_ssid, sizeof(out->ssid) - 1);
    strncpy(out->ip,   s_ip,   sizeof(out->ip)   - 1);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_rssi = ap.rssi;
    }
    out->rssi      = s_rssi;
    out->static_ip = s_static_ip;
}

// Insertion sort by RSSI desc — tiny N (≤12), good cache behaviour.
static void sort_scan_by_rssi_desc(sk_wifi_scan_entry_t *list, int n)
{
    for (int i = 1; i < n; i++) {
        sk_wifi_scan_entry_t key = list[i];
        int j = i - 1;
        while (j >= 0 && list[j].rssi < key.rssi) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = key;
    }
}

int sk_wifi_scan(sk_wifi_scan_entry_t *out, int max)
{
    if (!out || max <= 0) return 0;

    // Defensive pre-flight: ESP-IDF's esp_wifi_scan_start refuses to run
    // (ESP_ERR_WIFI_STATE) while the driver is mid-association on a stale
    // SSID. On a freshly provisioned device the auto-connect loop has
    // typically been retrying an empty/wrong SSID every 1-2 sec; that
    // collides with the SKAPP-driven scan request right after BLE pairing.
    // Stop the in-flight attempt (sticky), give NimBLE a tick, then scan.
    // We deliberately do NOT clear s_ssid — if creds are saved the user
    // can still trigger a manual `wifi.connect` later. Auto-reconnect
    // resumes naturally once `s_disconnect_pinned` is cleared by the next
    // sk_wifi_connect_sta call (e.g. from the wizard's password screen).
    s_disconnect_pinned = true;
    esp_wifi_disconnect();   // ignore ESP_ERR_WIFI_NOT_CONNECT
    vTaskDelay(pdMS_TO_TICKS(50));

    wifi_scan_config_t cfg = {0};
    esp_err_t serr = esp_wifi_scan_start(&cfg, true);
    if (serr != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s",
                 esp_err_to_name(serr));
        // Negative return signals "scan rejected by driver" to the
        // worker, which surfaces an explicit error envelope to SKAPP
        // instead of an empty data array (the latter would look like
        // "no networks in range" to the user — a misleading symptom).
        return -1;
    }
    uint16_t ap_count = (uint16_t)max;
    wifi_ap_record_t *recs = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!recs) return -1;
    esp_err_t rerr = esp_wifi_scan_get_ap_records(&ap_count, recs);
    if (rerr != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records failed: %s",
                 esp_err_to_name(rerr));
        free(recs);
        return -1;
    }
    int n = ap_count > max ? max : ap_count;
    if (n == 0) {
        SK_LOG_W("wifi", "scan.empty", "found=0");
    }
    for (int i = 0; i < n; i++) {
        strncpy(out[i].ssid, (const char *)recs[i].ssid, SK_WIFI_SSID_MAX);
        out[i].ssid[SK_WIFI_SSID_MAX] = '\0';
        out[i].rssi = recs[i].rssi;
        out[i].auth = (int)recs[i].authmode;
    }
    free(recs);
    // ESP-IDF returns scan hits in channel order; the human user wants
    // strongest first, so re-sort. (No dedup of repeated SSIDs — the
    // raw view is more useful for diagnosing mesh / repeater setups.)
    sort_scan_by_rssi_desc(out, n);
    return n;
}

// -- CLI handlers ----------------------------------------------------------

static sk_err_t cmd_wifi_status(sk_cli_ctx_t *ctx)
{
    sk_wifi_status_t st;
    sk_wifi_status(&st);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"static_ip\":%s,\"active_slot\":\"%s\","
             "\"has_primary\":%s,\"has_backup\":%s,\"fails\":%d}",
             st.connected ? "true" : "false",
             st.ssid, st.ip, st.rssi,
             st.static_ip ? "true" : "false",
             slot_name(s_active_slot),
             sk_wifi_has_primary() ? "true" : "false",
             sk_wifi_has_backup()  ? "true" : "false",
             s_consecutive_fails);
    sk_cli_ok(ctx, buf);
    return SK_OK;
}

// `wifi.scan` runs `esp_wifi_scan_start(blocking=true)` which holds the
// caller for 2-5 seconds. Inline that on the BLE GATT path and the
// NimBLE host task is blocked the entire time → connection-supervision
// timeout fires → peer drops (or in pathological cases the radio coex
// arbiter panics). Same pattern as wifi.connect: defer to a worker task
// and emit the response via the captured writer once the scan completes.
//
// Single-flight: a second scan request while the first is in flight is
// rejected with SK_ERR_BUSY. Reusing the scan worker across multiple
// callers would need to merge their writers, which is more complexity
// than this 5-second-rare command warrants.

typedef struct {
    sk_cli_writer_t writer;
    void           *writer_user;
    int             machine_id;
    bool            is_machine_mode;
} wifi_scan_job_t;

static volatile bool s_scan_active = false;

static void wifi_scan_worker(void *arg)
{
    wifi_scan_job_t *job = (wifi_scan_job_t *)arg;

    enum { MAX = 12 };
    sk_wifi_scan_entry_t *list = calloc(MAX, sizeof(*list));
    if (!list) {
        const char *err = job->is_machine_mode
            ? "{\"id\":-1,\"ok\":false,\"err\":\"ERR_INTERNAL\",\"params\":{\"reason\":\"oom\"}}\n"
            : "error: ERR_INTERNAL — out of memory\n";
        if (job->writer) job->writer(err, strlen(err), job->writer_user);
        s_scan_active = false;
        free(job);
        vTaskDelete(NULL);
        return;
    }

    int n = sk_wifi_scan(list, MAX);

    // Driver-level failure (e.g. ESP_ERR_WIFI_STATE because the driver is
    // still mid-association, or PHY init incomplete). Surface this to SKAPP
    // as an explicit error instead of an empty data array — an empty list
    // looks like "no APs visible" to the wizard, which is the wrong
    // diagnosis when the real problem is "scan was refused by the driver".
    if (n < 0) {
        free(list);
        const char *err = job->is_machine_mode
            ? "{\"id\":-1,\"ok\":false,\"err\":\"ERR_INTERNAL\",\"params\":{\"reason\":\"scan_rejected\"}}\n"
            : "error: ERR_INTERNAL — scan rejected by wifi driver\n";
        // Patch in the real id for machine mode (the -1 placeholder above
        // is just the static template).
        if (job->is_machine_mode) {
            char *dyn = malloc(192);
            if (dyn) {
                int dn = snprintf(dyn, 192,
                    "{\"id\":%d,\"ok\":false,\"err\":\"ERR_INTERNAL\",\"params\":{\"reason\":\"scan_rejected\"}}\n",
                    job->machine_id);
                if (job->writer && dn > 0) job->writer(dyn, (size_t)dn, job->writer_user);
                free(dyn);
            } else if (job->writer) {
                job->writer(err, strlen(err), job->writer_user);
            }
        } else if (job->writer) {
            job->writer(err, strlen(err), job->writer_user);
        }
        s_scan_active = false;
        free(job);
        vTaskDelete(NULL);
        return;
    }

    // 12 entries × ~80 bytes (ssid+rssi+auth) + 64 byte envelope
    // + per-entry comma → comfortably under 1.5 KB.
    char *buf = malloc(2048);
    if (!buf) {
        free(list);
        const char *err = job->is_machine_mode
            ? "{\"id\":-1,\"ok\":false,\"err\":\"ERR_INTERNAL\",\"params\":{\"reason\":\"oom\"}}\n"
            : "error: ERR_INTERNAL — out of memory\n";
        if (job->writer) job->writer(err, strlen(err), job->writer_user);
        s_scan_active = false;
        free(job);
        vTaskDelete(NULL);
        return;
    }

    size_t off = 0;
    if (job->is_machine_mode) {
        off += snprintf(buf + off, 2048 - off,
                        "{\"id\":%d,\"ok\":true,\"data\":[", job->machine_id);
    } else {
        off += snprintf(buf + off, 2048 - off, "ok. [");
    }
    for (int i = 0; i < n && off < 1900; i++) {
        off += snprintf(buf + off, 2048 - off,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                        i == 0 ? "" : ",",
                        list[i].ssid, list[i].rssi, list[i].auth);
    }
    if (job->is_machine_mode) {
        off += snprintf(buf + off, 2048 - off, "]}\n");
    } else {
        off += snprintf(buf + off, 2048 - off, "]\n");
    }

    if (job->writer) job->writer(buf, off, job->writer_user);

    free(buf);
    free(list);
    s_scan_active = false;
    free(job);
    vTaskDelete(NULL);
}

static sk_err_t cmd_wifi_scan(sk_cli_ctx_t *ctx)
{
    if (s_scan_active) {
        sk_cli_err(ctx, SK_ERR_BUSY, "{\"reason\":\"scan_in_flight\"}");
        return SK_OK;
    }

    wifi_scan_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"oom\"}");
        return SK_OK;
    }
    // Capture writer + machine_id at handler call time — ctx is a stack
    // var that goes out of scope when this function returns, but the
    // writer fn pointer (usb_writer / ble_writer) is a stable global.
    job->writer          = ctx->writer;
    job->writer_user     = ctx->writer_user;
    job->machine_id      = ctx->machine_id;
    job->is_machine_mode = ctx->is_machine_mode;

    s_scan_active = true;
    BaseType_t ok = xTaskCreate(wifi_scan_worker, "sk_wifi_scan",
                                4096, job, 4, NULL);
    if (ok != pdPASS) {
        s_scan_active = false;
        free(job);
        sk_cli_err(ctx, SK_ERR_INTERNAL, "{\"reason\":\"task_spawn\"}");
        return SK_OK;
    }
    // The worker writes the actual response (with full scan list) when it
    // finishes ~2-5 seconds from now. Mark `wrote_envelope = true` on the
    // ctx so the dispatcher doesn't emit an empty error envelope when we
    // return SK_OK without having called sk_cli_ok ourselves.
    ctx->wrote_envelope = true;
    return SK_OK;
}

// "primary" / "backup" → slot index; returns -1 if the token does not look
// like a slot specifier at all, which lets the positional parser distinguish
// slot from other args. Legacy aliases (wifi1/wifi2/1/2) intentionally
// dropped — two names per concept was confusing in `help`.
static int parse_slot_token(const char *s)
{
    if (!s || !*s) return -1;
    if (strcmp(s, "primary") == 0) return 0;
    if (strcmp(s, "backup")  == 0) return 1;
    return -1;
}

// Loose IPv4 detector used by the positional parser to decide whether a
// trailing token is a static-IP override or a slot name. Accepts both
// "192.168.1.50" and "192.168.1.50/24".
static bool looks_like_ipv4(const char *s)
{
    if (!s || !*s) return false;
    int dots = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '.') { dots++; continue; }
        if (*p == '/') break;
        if (!isdigit((unsigned char)*p)) return false;
    }
    return dots == 3;
}

// `--slot primary|backup|wifi1|wifi2|1|2`. Default 0 (primary) when the
// flag is absent, -1 when present but unrecognised.
static int parse_slot_arg(sk_cli_ctx_t *ctx)
{
    const char *s = sk_cli_arg_named(ctx, "slot");
    if (!s) return 0;       // default
    return parse_slot_token(s);
}

// -- BLE-safe deferred connect ---------------------------------------------
//
// `wifi.connect` over BLE used to call ble_gap_terminate + vTaskDelay +
// esp_wifi_connect inline. Problem: the CLI handler runs ON the NimBLE
// host task (cmd_rx_access → feed_rx → skbt_gatt_on_cmd_rx → secure-session
// dispatch → handler). Re-entering the GAP layer + blocking that task
// for 300ms + heavy esp_wifi_* calls on its small stack was the source of
// the SKAPP-driven "constant reboot" crash. The handler now hands the
// work to a dedicated worker task instead.
//
// Single-flight: two concurrent workers calling esp_wifi_disconnect /
// set_config / connect end up racing on the WiFi driver's internal state
// machine — the result on ESP32-C6 has been an LL_ASSERT panic in the
// PHY layer. The atomic `s_worker_active` flag rejects a second wifi.connect
// while the first is still running. The handler returns CLI error ERR_BUSY
// in that case so the caller can retry after a short delay.

typedef struct {
    sk_wifi_cred_t cred;
} wifi_connect_job_t;

static volatile bool s_worker_active = false;

static void wifi_connect_worker(void *arg)
{
    wifi_connect_job_t *job = (wifi_connect_job_t *)arg;

    // 1) Let the response envelope (sk_cli_ok from cmd_wifi_connect) leave
    //    the wire BEFORE we tear down the BLE link. On the BLE-driven path
    //    the response is queued as a notify on the NimBLE host task and
    //    flushed asynchronously — terminating the peer too early drops the
    //    notify silently. 100 ms is plenty for both USB fwrite() and a
    //    queued NimBLE notify to land.
    vTaskDelay(pdMS_TO_TICKS(100));

    // 2) Off the BLE host task now — safe to terminate the peer.
    sk_event_bus_publish("ble.terminate.before-wifi", NULL);

    // 3) Give NimBLE another beat to dispatch the disconnect before we
    //    start hammering the radio with WiFi association traffic. The
    //    coex layer is happier when the two stacks transition cleanly.
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t err = sk_wifi_connect_sta(&job->cred);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect_sta failed: %s — credential bozuk olabilir",
                 esp_err_to_name(err));
    }
    free(job);

    // Release the single-flight slot so the next wifi.connect can proceed.
    // Done after sk_wifi_connect_sta returns, NOT after GOT_IP — connect_sta
    // is fully synchronous on the worker task, association then happens on
    // the WiFi event task which doesn't need the slot.
    s_worker_active = false;

    vTaskDelete(NULL);
}

static esp_err_t schedule_wifi_connect(const sk_wifi_cred_t *c)
{
    // Single-flight: if a previous worker is still running, reject. The
    // caller (cmd_wifi_connect) maps ESP_ERR_INVALID_STATE → SK_ERR_BUSY
    // so the user gets a clear message instead of a panic.
    if (s_worker_active) return ESP_ERR_INVALID_STATE;

    wifi_connect_job_t *job = calloc(1, sizeof(*job));
    if (!job) return ESP_ERR_NO_MEM;
    memcpy(&job->cred, c, sizeof(*c));

    // Set the flag BEFORE xTaskCreate so a racing schedule call is
    // rejected even if the new worker hasn't yet been scheduled.
    s_worker_active = true;
    BaseType_t ok = xTaskCreate(wifi_connect_worker, "sk_wifi_conn",
                                4096, job, 4, NULL);
    if (ok != pdPASS) {
        s_worker_active = false;
        free(job);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// `wifi.connect` — accepts both positional and named forms:
//
//   Positional (human CLI, the simple path):
//     wifi connect <ssid> [password] [192.168.1.50[/24]] [primary|backup|wifi1|wifi2]
//
//   Named (machine mode + scripting):
//     wifi connect --ssid X --password Y [--static-ip 192.168.1.50/24]
//                  [--slot primary|backup]
//
// Trailing positional args after <password> are auto-classified: anything
// that looks like an IPv4 becomes static-ip, anything that names a slot
// becomes slot. No confirm-token requirement — physical USB access or an
// authenticated SKAPP session is the gate; making the user copy a hex
// token through two commands just to set a WiFi password was the wrong
// trade-off.
static sk_err_t cmd_wifi_connect(sk_cli_ctx_t *ctx)
{
    const char *ssid      = sk_cli_arg_named(ctx, "ssid");
    const char *password  = sk_cli_arg_named(ctx, "password");
    const char *static_ip = sk_cli_arg_named(ctx, "static-ip");
    int slot = parse_slot_arg(ctx);

    // Positional fallback: only when the caller didn't pass --ssid (named).
    // SKAPP machine mode artık JSON `argv: ["ssid","password",...]` array'i
    // gönderebiliyor (sk_cli.c dispatch_machine bunu human_argv slotuna
    // dolduruyor). Bu yüzden kısıt sadece "named ssid var mı?" — machine
    // mode olup olmaması artık önemli değil. Skips --key/value pairs so
    // users can mix flags and positionals.
    if (!ssid && sk_cli_argc(ctx) > 0) {
        const char *positional[8] = {0};
        int pn = 0;
        int argc = sk_cli_argc(ctx);
        for (int i = 0; i < argc && pn < 8; i++) {
            const char *a = sk_cli_arg(ctx, i);
            if (a && a[0] == '-' && a[1] == '-') { i++; continue; } // skip flag pair
            positional[pn++] = a;
        }
        if (pn >= 1) ssid     = positional[0];
        if (pn >= 2) password = positional[1];
        for (int i = 2; i < pn; i++) {
            const char *t = positional[i];
            if (!static_ip && looks_like_ipv4(t)) {
                static_ip = t;
                continue;
            }
            int s = parse_slot_token(t);
            if (s >= 0) slot = s;
        }
    }

    if (!ssid || !*ssid) {
        sk_cli_err(ctx, SK_ERR_MISSING_ARG, "{\"field\":\"ssid\"}");
        return SK_OK;
    }
    if (slot < 0) {
        sk_cli_err(ctx, SK_ERR_INVALID_ARG, "{\"field\":\"slot\"}");
        return SK_OK;
    }

    sk_wifi_cred_t c = {0};
    strncpy(c.ssid, ssid, sizeof(c.ssid) - 1);
    if (password)  strncpy(c.password,  password,  sizeof(c.password)  - 1);
    if (static_ip) strncpy(c.static_ip, static_ip, sizeof(c.static_ip) - 1);

    if (slot == 0) sk_wifi_save_primary(&c);
    else           sk_wifi_save_backup(&c);

    // Backup-slot save is non-disruptive — runtime escalation only
    // promotes backup after FAILS_BEFORE_SWAP failures on primary.
    if (slot != 0) {
        sk_cli_ok(ctx, "{\"slot\":\"backup\",\"connecting\":false,\"saved\":true}");
        return SK_OK;
    }

    s_active_slot = 1;
    s_consecutive_fails = 0;

    // Reject if another connect is already in flight — concurrent
    // esp_wifi_disconnect / set_config / connect calls race on the WiFi
    // driver's state machine. Reply BUSY so SKAPP / user can retry.
    esp_err_t sched = schedule_wifi_connect(&c);
    if (sched == ESP_ERR_INVALID_STATE) {
        sk_cli_err(ctx, SK_ERR_BUSY,
                   "{\"reason\":\"connect_in_flight\"}");
        return SK_OK;
    }
    if (sched != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_INTERNAL,
                   "{\"reason\":\"schedule_failed\"}");
        return SK_OK;
    }

    // Reply only after the worker is queued. The 100 ms pre-sleep inside
    // the worker guarantees this notify clears the wire before BLE
    // terminate.
    sk_cli_ok(ctx, "{\"slot\":\"primary\",\"connecting\":true,\"saved\":true}");
    return SK_OK;
}

static sk_err_t cmd_wifi_disconnect(sk_cli_ctx_t *ctx)
{
    sk_wifi_disconnect();
    sk_cli_ok(ctx, "{\"disconnected\":true}");
    return SK_OK;
}

static sk_err_t cmd_wifi_forget(sk_cli_ctx_t *ctx)
{
    const char *tok = sk_cli_confirm_token(ctx);
    if (!tok) { sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_REQUIRED, NULL); return SK_OK; }
    if (sk_auth_confirm_consume(tok) != ESP_OK) {
        sk_cli_err(ctx, SK_ERR_CONFIRM_TOKEN_INVALID, NULL);
        return SK_OK;
    }
    // --slot omitted = wipe both (preserves prior behaviour). Specifying
    // a slot only clears that one — keeps the other intact for the user
    // to keep using. When primary is wiped while it was the active slot
    // and backup exists, escalate to backup instead of just disconnecting.
    const char *slot_str = sk_cli_arg_named(ctx, "slot");
    if (!slot_str) {
        sk_wifi_clear_primary();
        sk_wifi_clear_backup();
        sk_wifi_disconnect();
        sk_cli_ok(ctx, "{\"forgotten\":\"all\"}");
        return SK_OK;
    }
    int slot = parse_slot_arg(ctx);
    if (slot < 0) {
        sk_cli_err(ctx, SK_ERR_INVALID_ARG, "{\"field\":\"slot\"}");
        return SK_OK;
    }
    if (slot == 0) {
        sk_wifi_clear_primary();
        // If backup is provisioned and primary was the live slot, swap.
        if (s_active_slot == 1 && sk_wifi_has_backup()) {
            connect_slot(2);
            sk_cli_ok(ctx, "{\"forgotten\":\"primary\",\"swapped_to\":\"backup\"}");
        } else {
            sk_wifi_disconnect();   // no fallback, just drop
            sk_cli_ok(ctx, "{\"forgotten\":\"primary\"}");
        }
    } else {
        sk_wifi_clear_backup();
        sk_cli_ok(ctx, "{\"forgotten\":\"backup\"}");
    }
    return SK_OK;
}

// `wifi.list` — both slots in one snapshot. SSIDs only (no passwords —
// they would never be useful to read back, only dangerous to leak).
// Includes static-ip per slot so the user can verify their override
// was stored correctly.
static sk_err_t cmd_wifi_list(sk_cli_ctx_t *ctx)
{
    sk_wifi_cred_t p = {0}, b = {0};
    bool have_p = load_cred(KEY_PRIMARY_SSID, KEY_PRIMARY_PASS, KEY_PRIMARY_IP, &p);
    bool have_b = load_cred(KEY_BACKUP_SSID,  KEY_BACKUP_PASS,  KEY_BACKUP_IP,  &b);

    char buf[384];
    int off = 0;
    off += snprintf(buf + off, sizeof(buf) - off,
                    "{\"active\":\"%s\",\"slots\":[", slot_name(s_active_slot));
    if (have_p) {
        off += snprintf(buf + off, sizeof(buf) - off,
                        "{\"slot\":\"primary\",\"ssid\":\"%s\",\"static_ip\":\"%s\","
                        "\"active\":%s}",
                        p.ssid, p.static_ip,
                        s_active_slot == 1 ? "true" : "false");
    }
    if (have_b) {
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%s{\"slot\":\"backup\",\"ssid\":\"%s\",\"static_ip\":\"%s\","
                        "\"active\":%s}",
                        have_p ? "," : "", b.ssid, b.static_ip,
                        s_active_slot == 2 ? "true" : "false");
    }
    off += snprintf(buf + off, sizeof(buf) - off, "]}");
    sk_cli_ok(ctx, buf);
    return SK_OK;
}

static const sk_cli_command_t s_cmds[] = {
    { .name = "wifi.status",
      .summary = "Show current connection state, SSID, IP, signal strength",
      .usage   = "wifi status",
      .help_block =
          "Returns the live WiFi state. Fields:\n"
          "  state         disconnected | connecting | connected | failed\n"
          "  ssid          network the device is associated with (if any)\n"
          "  ip            assigned IPv4 address\n"
          "  rssi          signal strength in dBm (closer to 0 is stronger;\n"
          "                -50 great, -70 OK, -90 marginal)\n"
          "  active_slot   which saved network is in use (primary | backup | none)\n"
          "\n"
          "Read-only.\n"
          "\n"
          "Example:\n"
          "  wifi status",
      .handler = cmd_wifi_status },

    { .name = "wifi.scan",
      .summary = "Scan visible WiFi networks nearby (up to 12)",
      .usage   = "wifi scan",
      .help_block =
          "Lists every access point the radio can hear right now: SSID,\n"
          "RSSI, channel, auth type. Scan takes 2-5 seconds — the response\n"
          "is sent when the scan completes, not immediately.\n"
          "\n"
          "Use this before `wifi connect` to confirm the network you're\n"
          "after is in range and to see its exact SSID spelling.\n"
          "\n"
          "Example:\n"
          "  wifi scan",
      .handler = cmd_wifi_scan },

    { .name = "wifi.list",
      .summary = "List saved WiFi networks",
      .usage   = "wifi list",
      .help_block =
          "Shows networks the device has credentials saved for. Up to two\n"
          "networks can be saved in two slots — see `help wifi.connect` for\n"
          "the slot mechanic.\n"
          "\n"
          "Example:\n"
          "  wifi list",
      .handler = cmd_wifi_list },

    { .name = "wifi.connect",
      .summary = "Connect to a WiFi network (saves credentials)",
      .usage   = "wifi connect <ssid> [password] [192.168.1.50[/24]] [primary|backup]",
      .help_block =
          "Connects to the given network and saves the credentials so the\n"
          "device reconnects automatically after a reboot.\n"
          "\n"
          "Simple form (most users only need this):\n"
          "  wifi connect HomeWiFi pass1234\n"
          "\n"
          "With a static IPv4 address (defaults to /24 netmask):\n"
          "  wifi connect HomeWiFi pass1234 192.168.1.111\n"
          "  wifi connect HomeWiFi pass1234 192.168.1.111/24\n"
          "\n"
          "Two-network fallback (advanced):\n"
          "  The device keeps two saved-credential slots, primary and\n"
          "  backup. If primary fails to associate after a few attempts,\n"
          "  it falls over to backup — useful if you carry the device\n"
          "  between home and office.\n"
          "\n"
          "  wifi connect HomeWiFi pass1234              # saved as primary (default)\n"
          "  wifi connect OfficeWiFi pass5678 backup     # saved as backup\n"
          "\n"
          "Trailing args are auto-classified by shape — IPv4-looking tokens\n"
          "go to --static-ip, the words primary/backup go to --slot. Named\n"
          "flags are also accepted for scripting:\n"
          "  wifi connect --ssid X --password Y [--static-ip 192.168.1.50/24]\n"
          "                                     [--slot primary|backup]",
      .handler = cmd_wifi_connect },

    { .name = "wifi.disconnect",
      .summary = "Disconnect from the current WiFi network",
      .usage   = "wifi disconnect",
      .help_block =
          "Drops the active WiFi association. The intent is sticky — the\n"
          "device will NOT auto-reconnect until you run `wifi connect`\n"
          "again. Saved credentials are kept (use `wifi forget` to delete\n"
          "them).\n"
          "\n"
          "Example:\n"
          "  wifi disconnect",
      .handler = cmd_wifi_disconnect },

    { .name = "wifi.forget",
      .summary = "Delete saved WiFi credentials (one slot or both)",
      .usage   = "wifi forget [--slot primary|backup]",
      .critical = true,
      .help_block =
          "Removes saved credentials. Without --slot, both slots are wiped.\n"
          "With --slot, only that one is removed (the other keeps working).\n"
          "Critical — see `help device.confirm-token` for the confirm flow.",
      .handler = cmd_wifi_forget },
};

// -- Factory reset hook ----------------------------------------------------

static void on_factory_reset(const sk_event_t *evt, void *user)
{
    (void)evt; (void)user;
    sk_wifi_clear_primary();
    sk_wifi_clear_backup();
    sk_wifi_disconnect();
}

// Called from sk_wifi_init once core is up.
static void wifi_register_cli_and_hooks(void)
{
    for (size_t i = 0; i < sizeof(s_cmds) / sizeof(s_cmds[0]); i++) {
        sk_cli_register(&s_cmds[i]);
    }
    int sub;
    sk_event_bus_subscribe("device.factory-reset.requested",
                           on_factory_reset, NULL, &sub);
}
