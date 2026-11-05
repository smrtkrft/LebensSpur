/*
 * net_wifi — durum makinesi. Bütün geçişler on_event() içinde (varsayılan olay döngüsü görevi);
 * esp_timer callback'leri ve API yalnız LS_WIFI olayı gönderir. vTaskDelay / bloklayan tarama yok.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "mdns.h"
#include "cfg.h"
#include "evlog.h"
#include "net_wifi.h"

ESP_EVENT_DEFINE_BASE(LS_WIFI);
enum { EVT_RETRY, EVT_SCAN, EVT_PRIMARY_CHECK, EVT_AP_FALLBACK, EVT_TX_EVAL, EVT_TX_KICK, EVT_RECONNECT,
       EVT_APPLY_AP, EVT_APPLY_TX };

#define MAX_TRY            5
#define SCAN_FAST_MS       15000
#define SCAN_SLOW_MS       60000
#define SCAN_FAST_WINDOW_US (120LL * 1000000LL)
#define AP_FALLBACK_US     (10LL * 60 * 1000000LL)
#define PRIMARY_CHECK_US   (30LL * 60 * 1000000LL)
#define TX_EVAL_US         (10LL * 1000000LL)
#define TX_KICK_US         2500000LL
#define TX_SOFT_Q          32            /* yumuşak başlangıç ~8 dBm (fiilen 7) */
#define TX_AP_CLIENT_Q     56            /* telefon bağlanınca en çok 14 dBm */
#define PRIMARY_MIN_RSSI   (-75)

static char s_name[16];
static net_wifi_link_cb_t s_link_cb;
static esp_netif_t *s_sta, *s_ap;
static esp_timer_handle_t s_t_retry, s_t_scan, s_t_primary, s_t_ap, s_t_tx, s_t_kick;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* olay döngüsünde sahip olunan durum */
static cfg_wifi_p_t s_cred[2];             /* 0 birincil, 1 yedek (aynı yapı) */
_Static_assert(sizeof(cfg_wifi_p_t) == sizeof(cfg_wifi_b_t), "wifi_p ve wifi_b ayni duzende olmali");
static cfg_device_t s_dev;
static cfg_wifi_tx_t s_tx;
static int s_which = -1, s_try;
static uint8_t s_chan_hint[2];
static bool s_self_disc, s_pending_reconnect, s_sntp_started, s_bg_scan;
static int64_t s_fail_since;
static uint8_t s_tx_q, s_tx_cap_override, s_lower_streak;
static wifi_status_t s_st;                 /* s_mux altında kopyalanır */

const char *net_wifi_device_name(void) { return s_name; }

const char *net_wifi_err_class_name(wifi_err_class_t c)
{
    static const char *const n[] = { "none", "auth", "not_found", "security", "other" };
    return (unsigned)c < 5 ? n[c] : "other";
}

static void make_name(void)
{
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 6; i++) { h ^= mac[i]; h *= 0x100000001b3ULL; }
    static const char cs[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    char id[11];
    for (int i = 0; i < 10; i++) { id[i] = cs[h % (sizeof cs - 1)]; h /= (sizeof cs - 1); }
    id[10] = 0;
    snprintf(s_name, sizeof s_name, "LS-%s", id);
}

static void set_state(wifi_state_t st)
{
    portENTER_CRITICAL(&s_mux);
    s_st.state = st;
    portEXIT_CRITICAL(&s_mux);
}

static wifi_err_class_t classify(uint8_t reason, bool has_pass)
{
    switch (reason) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: case WIFI_REASON_HANDSHAKE_TIMEOUT: case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_AUTH_FAIL: case WIFI_REASON_CONNECTION_FAIL:
        return WERR_AUTH;
    case WIFI_REASON_NO_AP_FOUND:
        return has_pass ? WERR_NOT_FOUND : WERR_AUTH;
    case WIFI_REASON_BEACON_TIMEOUT: case WIFI_REASON_AUTH_EXPIRE: case WIFI_REASON_ASSOC_EXPIRE:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ? WERR_SECURITY : WERR_NOT_FOUND;
    case WIFI_REASON_SA_QUERY_TIMEOUT: case WIFI_REASON_IE_INVALID: case WIFI_REASON_INVALID_PMKID:
        return WERR_SECURITY;
    default:
        return WERR_OTHER;
    }
}

static void post(int32_t id) { esp_event_post(LS_WIFI, id, NULL, 0, 0); }
static void tmr_cb(void *arg) { post((int32_t)(intptr_t)arg); }
static esp_timer_handle_t mk_timer(int32_t id, const char *name)
{
    esp_timer_handle_t t = NULL;
    const esp_timer_create_args_t a = { .callback = tmr_cb, .arg = (void *)(intptr_t)id, .name = name };
    esp_timer_create(&a, &t);
    return t;
}
static void once(esp_timer_handle_t t, int64_t us) { esp_timer_stop(t); esp_timer_start_once(t, us); }

static bool have_cred(int i) { return s_cred[i].ssid[0] != 0; }

static void load_cfg(void)
{
    cfg_copy_section(CFG_SEC_WIFI_P, &s_cred[0]);
    cfg_copy_section(CFG_SEC_WIFI_B, &s_cred[1]);   /* wifi_b ile wifi_p aynı alan düzeni */
    cfg_copy_section(CFG_SEC_DEVICE, &s_dev);
    cfg_copy_section(CFG_SEC_WIFI_TX, &s_tx);
}

/* ---------- TX gücü ---------- */

static void tx_apply(uint8_t q)
{
    uint8_t cap = s_tx_cap_override ? s_tx_cap_override : s_tx.max_q;
    if (q > cap) q = cap;
    if (q < 28) q = 28;
    if (q == s_tx_q) return;
    if (esp_wifi_set_max_tx_power((int8_t)q) != ESP_OK) return;
    int8_t real = 0;
    esp_wifi_get_max_tx_power(&real);
    s_tx_q = (uint8_t)real;
    portENTER_CRITICAL(&s_mux);
    s_st.tx_q = s_tx_q;
    portEXIT_CRITICAL(&s_mux);
    evlog_mark(EVM_TX_POWER, real);
}

static uint8_t rssi_to_q(int rssi)
{
    if (rssi >= -55) return 28;
    if (rssi >= -62) return 44;
    if (rssi >= -70) return 56;
    if (rssi >= -76) return 66;
    return 80;
}

static void tx_eval(bool fast)
{
    uint8_t cap = s_tx_cap_override ? s_tx_cap_override : s_tx.max_q;
    uint8_t target = cap;
    if (s_tx.adaptive) {
        int worst = 0;
        bool any = false;
        wifi_sta_list_t l;
        if (esp_wifi_ap_get_sta_list(&l) == ESP_OK) {
            for (int i = 0; i < l.num; i++) {
                if (l.sta[i].rssi == 0) continue;
                if (!any || l.sta[i].rssi < worst) worst = l.sta[i].rssi;
                any = true;
            }
        }
        wifi_ap_record_t ap;
        if (s_st.state == WST_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            if (!any || ap.rssi < worst) worst = ap.rssi;
            any = true;
            portENTER_CRITICAL(&s_mux);
            s_st.rssi = ap.rssi;
            portEXIT_CRITICAL(&s_mux);
        }
        if (any && rssi_to_q(worst) < target) target = rssi_to_q(worst);
    }
    if (target > s_tx_q) { s_lower_streak = 0; tx_apply(target); }
    else if (target < s_tx_q && (fast || ++s_lower_streak >= 2)) { s_lower_streak = 0; tx_apply(target); }
}

static void wifi_ctx(void)
{
    wifi_mode_t m = WIFI_MODE_NULL;
    esp_wifi_get_mode(&m);
    evlog_set_wifi_ctx((uint8_t)m, s_st.ap_clients, s_st.rssi, (int8_t)(s_tx_q / 4));
}

/* ---------- AP ---------- */

static void ap_config(void)
{
    wifi_config_t c = { 0 };
    strlcpy((char *)c.ap.ssid, s_name, sizeof c.ap.ssid);
    c.ap.ssid_len = (uint8_t)strlen(s_name);
    c.ap.channel = 1;
    c.ap.max_connection = 4;
    if (s_dev.ap_pass[0]) {
        strlcpy((char *)c.ap.password, s_dev.ap_pass, sizeof c.ap.password);
        c.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        c.ap.authmode = WIFI_AUTH_OPEN;
    }
    esp_wifi_set_config(WIFI_IF_AP, &c);
}

static void set_ap(bool on, bool auto_opened)
{
    wifi_mode_t m = WIFI_MODE_NULL;
    esp_wifi_get_mode(&m);
    bool sta = have_cred(0) || have_cred(1);
    wifi_mode_t want = on ? (sta ? WIFI_MODE_APSTA : WIFI_MODE_AP) : WIFI_MODE_STA;
    if (m != want) {
        esp_wifi_set_mode(want);
        if (on) ap_config();
    }
    portENTER_CRITICAL(&s_mux);
    s_st.ap_on = on;
    s_st.ap_auto_opened = auto_opened;
    portEXIT_CRITICAL(&s_mux);
    wifi_ctx();
}

/* ---------- STA ---------- */

static void apply_static(const cfg_wifi_p_t *c)
{
    if (!c->ip[0]) { esp_netif_dhcpc_start(s_sta); return; }
    esp_netif_ip_info_t ip = { 0 };
    if (!ip4addr_aton(c->ip, (ip4_addr_t *)&ip.ip)) { esp_netif_dhcpc_start(s_sta); return; }
    if (!c->mask[0] || !ip4addr_aton(c->mask, (ip4_addr_t *)&ip.netmask)) ip.netmask.addr = PP_HTONL(0xFFFFFF00u);
    if (!c->gw[0] || !ip4addr_aton(c->gw, (ip4_addr_t *)&ip.gw)) {
        ip.gw.addr = (ip.ip.addr & ip.netmask.addr) | PP_HTONL(1u);
    }
    esp_netif_dhcpc_stop(s_sta);
    esp_netif_set_ip_info(s_sta, &ip);
    esp_netif_dns_info_t d = { 0 };
    d.ip.type = ESP_IPADDR_TYPE_V4;
    if (c->dns[0] && ip4addr_aton(c->dns, (ip4_addr_t *)&d.ip.u_addr.ip4)) {
        esp_netif_set_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &d);
        d.ip.u_addr.ip4.addr = ip.gw.addr;
        esp_netif_set_dns_info(s_sta, ESP_NETIF_DNS_BACKUP, &d);
    } else {
        d.ip.u_addr.ip4.addr = ip.gw.addr;
        esp_netif_set_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &d);
        ip4addr_aton("8.8.8.8", (ip4_addr_t *)&d.ip.u_addr.ip4);
        esp_netif_set_dns_info(s_sta, ESP_NETIF_DNS_BACKUP, &d);
    }
}

static void connect_to(int which)
{
    const cfg_wifi_p_t *c = &s_cred[which];
    wifi_config_t w = { 0 };
    strlcpy((char *)w.sta.ssid, c->ssid, sizeof w.sta.ssid);
    strlcpy((char *)w.sta.password, c->pass, sizeof w.sta.password);
    w.sta.threshold.authmode = c->pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    w.sta.pmf_cfg.required = false;
    w.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    w.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (s_chan_hint[which]) { w.sta.channel = s_chan_hint[which]; w.sta.scan_method = WIFI_FAST_SCAN; }
    else w.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    esp_wifi_set_config(WIFI_IF_STA, &w);
    apply_static(c);
    s_which = which;
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_st.ssid, c->ssid, sizeof s_st.ssid);
    s_st.using_backup = which == 1;
    portEXIT_CRITICAL(&s_mux);
    set_state(WST_CONNECTING);
    evlog_mark(EVM_STA_CONNECTING, which);
    esp_wifi_connect();
}

static void start_scan(bool background)
{
    wifi_scan_config_t sc = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                              .scan_time.active = { .min = 0, .max = 120 },
                              .home_chan_dwell_time = s_st.ap_clients ? 100 : 30 };
    s_bg_scan = background;
    if (esp_wifi_scan_start(&sc, false) == ESP_OK && !background) set_state(WST_SCANNING);
}

static void schedule_rescan(void)
{
    set_state(WST_RETRY_WAIT);
    int64_t now = esp_timer_get_time();
    if (!s_fail_since) s_fail_since = now;
    bool fast = now - s_fail_since < SCAN_FAST_WINDOW_US && s_st.ap_clients == 0;
    once(s_t_scan, (fast ? SCAN_FAST_MS : SCAN_SLOW_MS) * 1000LL);
}

static void on_scan_done(void)
{
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    int8_t best[2] = { -127, -127 };
    wifi_ap_record_t r;
    while (n-- && esp_wifi_scan_get_ap_record(&r) == ESP_OK) {      /* heap'siz, tek tek */
        for (int i = 0; i < 2; i++) {
            if (have_cred(i) && strcmp((const char *)r.ssid, s_cred[i].ssid) == 0 && r.rssi > best[i]) {
                best[i] = r.rssi;
                s_chan_hint[i] = r.primary;
            }
        }
    }
    esp_wifi_clear_ap_list();
    if (s_bg_scan) {                                   /* yedekteyken birincil denetimi: koparmadan */
        s_bg_scan = false;
        if (s_st.state == WST_CONNECTED && s_which == 1 && best[0] >= PRIMARY_MIN_RSSI) {
            evlog_write(EVL_INFO, "wifi", "birincil ag yeniden goruldu (rssi %d), gecis", best[0]);
            s_pending_reconnect = true;
            s_self_disc = true;
            set_state(WST_DISCONNECTING);
            esp_wifi_disconnect();
        }
        return;
    }
    if (best[0] > -127) connect_to(0);
    else if (best[1] > -127) connect_to(1);
    else if (have_cred(0) && s_cred[0].hidden) connect_to(0);
    else if (have_cred(1) && s_cred[1].hidden) connect_to(1);
    else schedule_rescan();
}

static void begin_sta(void)
{
    s_try = 0;
    s_fail_since = 0;
    if (have_cred(0) || have_cred(1)) start_scan(false);
    else set_state(WST_AP_ONLY);
}

static void on_disconnected(const wifi_event_sta_disconnected_t *d)
{
    bool was_connected = s_st.state == WST_CONNECTED;
    if (was_connected) {
        if (s_link_cb) s_link_cb(false);
        portENTER_CRITICAL(&s_mux);
        s_st.ip[0] = 0;
        portEXIT_CRITICAL(&s_mux);
    }
    if (s_self_disc) {                                 /* kendi kopardık: bekleyen eylem */
        s_self_disc = false;
        if (s_pending_reconnect) { s_pending_reconnect = false; load_cfg(); begin_sta(); }
        return;
    }
    evlog_mark(EVM_STA_LOST, d->reason);
    portENTER_CRITICAL(&s_mux);
    s_st.last_error.cls = classify(d->reason, s_which >= 0 && s_cred[s_which].pass[0]);
    s_st.last_error.reason = d->reason;
    s_st.last_error.rssi = d->rssi;
    portEXIT_CRITICAL(&s_mux);
    if (was_connected) {
        evlog_write(EVL_WARN, "wifi", "baglanti koptu: '%s' (neden %u, rssi %d)", s_st.ssid, d->reason, d->rssi);
        if (!s_dev.ap_enabled) once(s_t_ap, AP_FALLBACK_US);
        esp_timer_stop(s_t_primary);
        s_try = 0;
    }
    if (++s_try < MAX_TRY) {
        set_state(WST_CONNECTING);
        once(s_t_retry, (int64_t)s_try * 1000000LL);       /* 1, 2, 3, 4 sn */
        return;
    }
    if (s_which == 0 && have_cred(1)) {
        s_try = 0;
        evlog_write(EVL_WARN, "wifi", "birincil ag basarisiz (%s), yedek deneniyor",
                    net_wifi_err_class_name(s_st.last_error.cls));
        connect_to(1);
        return;
    }
    if (!s_fail_since) {
        evlog_write(EVL_WARN, "wifi", "WiFi baglanamadi (%s, neden %u); tarama surecek",
                    net_wifi_err_class_name(s_st.last_error.cls), d->reason);
    }
    s_try = 0;
    schedule_rescan();
}

static void sntp_synced(struct timeval *tv) { (void)tv; evlog_time_synced(); }

static void on_got_ip(const ip_event_got_ip_t *e)
{
    portENTER_CRITICAL(&s_mux);
    snprintf(s_st.ip, sizeof s_st.ip, IPSTR, IP2STR(&e->ip_info.ip));
    s_st.last_error.cls = WERR_NONE;
    portEXIT_CRITICAL(&s_mux);
    set_state(WST_CONNECTED);
    s_try = 0;
    s_fail_since = 0;
    esp_timer_stop(s_t_ap);
    esp_timer_stop(s_t_scan);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_st.rssi = ap.rssi;
    evlog_mark(EVM_STA_GOT_IP, s_st.rssi);
    evlog_write(EVL_INFO, "wifi", "baglandi: '%s' %s (rssi %d)%s", s_st.ssid, s_st.ip, s_st.rssi,
                s_which == 1 ? " [yedek]" : "");
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (s_st.ap_auto_opened) {                         /* otomatik açılan AP: ayara dön */
        set_ap(false, false);
        evlog_write(EVL_INFO, "wifi", "STA geri geldi, otomatik acilan AP kapatildi");
    }
    if (s_which == 1 && have_cred(0)) {
        esp_timer_stop(s_t_primary);
        esp_timer_start_periodic(s_t_primary, PRIMARY_CHECK_US);
    }
    if (!s_sntp_started) {
        esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        sc.sync_cb = sntp_synced;
        sc.wait_for_sync = false;
        s_sntp_started = esp_netif_sntp_init(&sc) == ESP_OK;
    }
    once(s_t_kick, TX_KICK_US);
    wifi_ctx();
    if (s_link_cb) s_link_cb(true);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START: begin_sta(); break;
        case WIFI_EVENT_SCAN_DONE: on_scan_done(); break;
        case WIFI_EVENT_STA_DISCONNECTED: on_disconnected(data); break;
        case WIFI_EVENT_AP_STACONNECTED:
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_sta_list_t l;
            uint8_t n = esp_wifi_ap_get_sta_list(&l) == ESP_OK ? (uint8_t)l.num : 0;
            portENTER_CRITICAL(&s_mux);
            s_st.ap_clients = n;
            portEXIT_CRITICAL(&s_mux);
            bool join = id == WIFI_EVENT_AP_STACONNECTED;
            evlog_mark(join ? EVM_AP_CLIENT_JOIN : EVM_AP_CLIENT_LEAVE, n);
            if (join && s_tx_q > TX_AP_CLIENT_Q) tx_apply(TX_AP_CLIENT_Q);   /* telefon yakın: pik düşür */
            evlog_write(EVL_INFO, "wifi", "AP: istemci %s (toplam %u)", join ? "baglandi" : "ayrildi", n);
            once(s_t_kick, TX_KICK_US);
            wifi_ctx();
            break;
        }
        default: break;
        }
        return;
    }
    if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            const ip_event_got_ip_t *e = data;
            if (e->ip_info.ip.addr == 0) { s_self_disc = true; s_pending_reconnect = true; esp_wifi_disconnect(); }
            else on_got_ip(e);
        } else if (id == IP_EVENT_STA_LOST_IP && s_st.state == WST_CONNECTED) {
            evlog_write(EVL_WARN, "wifi", "IP kaybedildi, yeniden baglaniliyor");
            s_self_disc = true;
            s_pending_reconnect = true;
            esp_wifi_disconnect();
        }
        return;
    }
    switch (id) {                                     /* LS_WIFI */
    case EVT_RETRY: if (s_st.state == WST_CONNECTING) esp_wifi_connect(); break;
    case EVT_SCAN: if (s_st.state == WST_RETRY_WAIT) start_scan(false); break;
    case EVT_PRIMARY_CHECK: if (s_st.state == WST_CONNECTED && s_which == 1) start_scan(true); break;
    case EVT_AP_FALLBACK:
        if (s_st.state != WST_CONNECTED && !s_st.ap_on) {
            set_ap(true, true);
            evlog_write(EVL_WARN, "wifi", "10 dk baglanti yok: AP otomatik acildi");
        }
        break;
    case EVT_TX_EVAL: tx_eval(false); break;
    case EVT_TX_KICK: tx_eval(true); break;
    case EVT_RECONNECT:
        load_cfg();
        esp_timer_stop(s_t_retry);
        esp_timer_stop(s_t_scan);
        if (s_st.state == WST_CONNECTED || s_st.state == WST_CONNECTING) {
            s_pending_reconnect = true;
            s_self_disc = true;
            set_state(WST_DISCONNECTING);
            esp_wifi_disconnect();
        } else {
            wifi_mode_t before = WIFI_MODE_NULL;
            esp_wifi_get_mode(&before);
            set_ap(s_dev.ap_enabled || !(have_cred(0) || have_cred(1)), false);
            /* mod STA'ya yeni geçtiyse STA_START olayı begin_sta'yı çağırır; çift tarama olmasın */
            if (before == WIFI_MODE_STA || before == WIFI_MODE_APSTA) begin_sta();
        }
        break;
    case EVT_APPLY_AP:
        load_cfg();
        set_ap(s_dev.ap_enabled || s_st.state != WST_CONNECTED, false);
        if (s_st.ap_on) ap_config();
        evlog_write(EVL_INFO, "wifi", "AP %s", s_st.ap_on ? "acik" : "kapali");
        break;
    case EVT_APPLY_TX: load_cfg(); tx_eval(true); break;
    default: break;
    }
}

esp_err_t net_wifi_start(net_wifi_link_cb_t cb)
{
    s_link_cb = cb;
    make_name();
    load_cfg();
    esp_err_t r = esp_netif_init();
    if (r == ESP_OK) r = esp_event_loop_create_default();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) return r;
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, s_name);            /* DHCP option 12: router listesinde ad */
    esp_netif_set_hostname(s_ap, s_name);
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    r = esp_wifi_init(&ic);
    if (r != ESP_OK) return r;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(LS_WIFI, ESP_EVENT_ANY_ID, on_event, NULL);
    s_t_retry = mk_timer(EVT_RETRY, "w_retry");
    s_t_scan = mk_timer(EVT_SCAN, "w_scan");
    s_t_primary = mk_timer(EVT_PRIMARY_CHECK, "w_prim");
    s_t_ap = mk_timer(EVT_AP_FALLBACK, "w_apfb");
    s_t_tx = mk_timer(EVT_TX_EVAL, "w_tx");
    s_t_kick = mk_timer(EVT_TX_KICK, "w_kick");

    bool sta = have_cred(0) || have_cred(1);
    bool ap = s_dev.ap_enabled || !sta;               /* kimlik yoksa AP her zaman */
    esp_wifi_set_mode(sta ? (ap ? WIFI_MODE_APSTA : WIFI_MODE_STA) : WIFI_MODE_AP);
    if (ap) ap_config();
    s_st.ap_on = ap;
    if (!ap) once(s_t_ap, AP_FALLBACK_US);            /* AP kapalı + STA hiç bağlanmazsa */
    r = esp_wifi_start();
    if (r != ESP_OK) return r;
    esp_wifi_set_max_tx_power(TX_SOFT_Q);              /* yumuşak başlangıç */
    s_tx_q = TX_SOFT_Q;
    once(s_t_kick, TX_KICK_US);
    esp_timer_start_periodic(s_t_tx, TX_EVAL_US);
    if (!sta) set_state(WST_AP_ONLY);

    char host[16];
    for (int i = 0; s_name[i] && i < 15; i++) host[i] = (char)(s_name[i] >= 'A' && s_name[i] <= 'Z' ? s_name[i] + 32 : s_name[i]);
    host[strlen(s_name) < 15 ? strlen(s_name) : 15] = 0;
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(host);
        mdns_instance_name_set(s_name);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    evlog_write(EVL_INFO, "wifi", "baslatildi: %s, AP %s, kayitli ag %d", s_name, ap ? "acik" : "kapali",
                have_cred(0) + have_cred(1));
    return ESP_OK;
}

void net_wifi_get_status(wifi_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t net_wifi_reconnect(void) { return esp_event_post(LS_WIFI, EVT_RECONNECT, NULL, 0, 0); }

esp_err_t net_wifi_apply_ap(void)
{
    cfg_device_t d;
    cfg_copy_section(CFG_SEC_DEVICE, &d);
    wifi_status_t st;
    net_wifi_get_status(&st);
    if (!d.ap_enabled && st.state != WST_CONNECTED) return ESP_ERR_INVALID_STATE;
    return esp_event_post(LS_WIFI, EVT_APPLY_AP, NULL, 0, 0);
}

esp_err_t net_wifi_apply_tx(void) { return esp_event_post(LS_WIFI, EVT_APPLY_TX, NULL, 0, 0); }

void net_wifi_set_tx_cap_q(uint8_t q)
{
    s_tx_cap_override = q;
    esp_event_post(LS_WIFI, EVT_APPLY_TX, NULL, 0, 0);
}
