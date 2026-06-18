// NimBLE adapter for the SKAPP CLI.
//
// Brings up the BLE stack, registers the SKAPP GATT service, and keeps
// advertising in sync with sk_auth pairing state. Heavy GATT logic lives in
// sk_transport_ble_gatt.c; this file is the bring-up and the event-bus
// bridge.

#include "sk_transport_ble.h"
#include "sk_transport_ble_gatt.h"
#include "sk_core.h"
#include "sk_auth.h"
#include "sk_capabilities.h"
#include "sk_cli.h"
#include "sk_errors.h"
#include "sk_event_bus.h"
#include "sk_identity.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
// esp_nimble_hci.h removed in ESP-IDF v5.x — HCI init now happens inside
// nimble_port_init(). No code change needed beyond dropping the include.
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nvs_flash.h"

static const char *TAG = "sk_ble";

#define SK_BLE_IDLE_TIMEOUT_US  (60LL * 1000 * 1000)   // 60 s after disconnect
// 10 min: while a peer is connected but neither side does any GATT
// activity, drop the link to save power. The owner can re-open it with
// a short button press (control.short-press → 60 s pairing window).
#define SK_BLE_PEER_IDLE_US     (10LL * 60 * 1000 * 1000)
// How often to evaluate the peer-idle condition. 30 s is fine — we're
// chasing a 10-minute deadline, not millisecond precision.
#define SK_BLE_PEER_CHECK_US    (30LL * 1000 * 1000)
#define SK_BLE_NO_CONN          0xFFFF

static uint8_t            s_own_addr_type;
static bool               s_initialized  = false;
static bool               s_radio_active = false;
static esp_timer_handle_t s_idle_timer   = NULL;     // 60 s post-disconnect kill
static esp_timer_handle_t s_peer_check_t = NULL;     // periodic peer-idle evaluator
static uint16_t           s_conn_handle  = SK_BLE_NO_CONN;
static int64_t            s_peer_activity_us = 0;

static void start_advertising(void);

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "connected conn=%d", event->connect.conn_handle);
            if (s_idle_timer) esp_timer_stop(s_idle_timer);
            s_conn_handle = event->connect.conn_handle;
            s_peer_activity_us = esp_timer_get_time();
            if (s_peer_check_t && !esp_timer_is_active(s_peer_check_t)) {
                esp_timer_start_periodic(s_peer_check_t, SK_BLE_PEER_CHECK_US);
            }
            skbt_gatt_on_connect(event->connect.conn_handle);
            sk_event_bus_publish("ble.state",
                                 "{\"state\":\"connected\"}");
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected conn=%d reason=0x%02X",
                 event->disconnect.conn.conn_handle, event->disconnect.reason);
        s_conn_handle = SK_BLE_NO_CONN;
        if (s_peer_check_t && esp_timer_is_active(s_peer_check_t)) {
            esp_timer_stop(s_peer_check_t);
        }
        skbt_gatt_on_disconnect(event->disconnect.conn.conn_handle);
        // Resume advertising so the peer (or owner) can quickly reconnect.
        // start_advertising() will publish the "advertising" state itself.
        start_advertising();
        if (s_idle_timer) esp_timer_start_once(s_idle_timer, SK_BLE_IDLE_TIMEOUT_US);
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        // Outbound traffic counts as "peer is reachable / engaged".
        s_peer_activity_us = esp_timer_get_time();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        s_peer_activity_us = esp_timer_get_time();
        // Peer event_tx CCCD subscribe ettiğinde bonded handshake'i
        // burada tetikliyoruz. event->subscribe.cur_notify true ise
        // peer notify-enable yazdı.
        if (event->subscribe.cur_notify) {
            skbt_gatt_on_subscribe(event->subscribe.conn_handle);
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU update conn=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change status=%d", event->enc_change.status);
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // Delete old bond and allow the new pairing only if pairing mode is open.
        if (sk_auth_pairing_state() == SK_AUTH_PAIRING_OPEN) {
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    default:
        break;
    }
    return 0;
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    bool pairable = sk_auth_pairing_state() == SK_AUTH_PAIRING_OPEN;

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)sk_identity_get();
    fields.name_len = strlen(sk_identity_get());
    fields.name_is_complete = 1;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    // Advertise manufacturer data indicating pairable / bonded.
    static const uint8_t mfg_bonded[]   = { 0xF1, 0x00, 'b', 'n', 'd' };
    static const uint8_t mfg_pairable[] = { 0xF1, 0x00, 'p', 'a', 'r' };
    fields.mfg_data     = pairable ? mfg_pairable : mfg_bonded;
    fields.mfg_data_len = 5;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min  = BLE_GAP_ADV_FAST_INTERVAL1_MIN,
        .itvl_max  = BLE_GAP_ADV_FAST_INTERVAL1_MAX,
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "adv_start FAILED rc=%d", rc);
        return;
    }
    s_radio_active = true;
    ESP_LOGI(TAG, "advertising as \"%s\" (rc=%d, pairable=%d)",
             sk_identity_get(), rc, (int)pairable);
    // Publish only when there's no peer connected yet — once a peer
    // connects we'll (re-)publish "connected" from the GAP event handler.
    if (s_conn_handle == SK_BLE_NO_CONN) {
        sk_event_bus_publish("ble.state", "{\"state\":\"advertising\"}");
    }
}

// Idle timer — bond varken cihaz advertising'i SÜREKLİ açık tutar ki
// eşleşmiş peer (SKAPP) wifi setup, dashboard, vs. için her an oturum
// açabilsin. Eski davranış: 60 sn sonra durdur (sadece pairing-kullanan
// cihazlar için anlamlıydı). Bond yokken ve pairing penceresi de
// kapalıyken yine durdur — anonim bir cihazın gereksiz adv yayını
// hem pil hem güvenlik için kötü.
static void idle_timer_cb(void *arg)
{
    (void)arg;
    if (skbt_gatt_is_connected()) return;
    if (sk_auth_has_bond()) {
        ESP_LOGI(TAG, "idle timeout — bond present, keeping advertising on");
        return;
    }
    if (sk_auth_pairing_state() == SK_AUTH_PAIRING_OPEN) {
        return;
    }
    ESP_LOGI(TAG, "idle timeout — no bond, stopping BLE advertising");
    sk_transport_ble_stop();
}

// Periodic peer-idle check: if a peer is connected but no GATT activity
// has happened in the last SK_BLE_PEER_IDLE_US window (10 minutes),
// terminate the link. Owner re-opens with a short button press.
static void peer_idle_check_cb(void *arg)
{
    (void)arg;
    if (s_conn_handle == SK_BLE_NO_CONN) return;
    int64_t idle = esp_timer_get_time() - s_peer_activity_us;
    if (idle < SK_BLE_PEER_IDLE_US) return;
    ESP_LOGI(TAG, "peer idle %lld s — terminating BLE link",
             (long long)(idle / 1000000));
    ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    // The DISCONNECT event handler will clear s_conn_handle and restart
    // the post-disconnect 60 s idle timer.
}

esp_err_t sk_transport_ble_start(void)
{
    if (s_radio_active) return ESP_OK;
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    start_advertising();
    return ESP_OK;
}

esp_err_t sk_transport_ble_stop(void)
{
    if (!s_radio_active) return ESP_OK;
    ble_gap_adv_stop();
    s_radio_active = false;
    if (s_idle_timer) esp_timer_stop(s_idle_timer);
    sk_event_bus_publish("ble.state", "{\"state\":\"off\"}");
    return ESP_OK;
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer_auto rc=%d", rc); return; }
    ble_svc_gap_device_name_set(sk_identity_get());
    start_advertising();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset reason=%d", reason); }

// wifi.connect handler bağlantı kurmadan ÖNCE bu event'i yayınlar:
// BLE peer'i terminate edip + advertising'i durdurarak ESP32-C6'nın
// single-radio'sunu WiFi STA association'ına bırakırız. Coex enabled
// olsa bile aktif iki yığın aynı anda heap basıncını crash'e taşıyor.
// SKAPP, bond varken sürekli adv açık olduğu için (idle_timer_cb fix'i)
// dashboard'a geçtiğinde yeniden bağlanır.
static void on_terminate_before_wifi(const sk_event_t *evt, void *user)
{
    (void)evt; (void)user;
    if (skbt_gatt_is_connected()) {
        ESP_LOGI(TAG, "wifi.connect → BLE peer'i terminate ediyorum");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    if (s_radio_active) {
        ble_gap_adv_stop();
        s_radio_active = false;
    }
}

// WiFi GOT_IP geldikten sonra BLE radyoyu güvenle yeniden açabiliriz —
// SKAPP'ın yeni dashboard ekranında oturum açması için advertising
// gerekli. Bond varken idle timer kapatmaz, sürekli açık kalır.
static void on_resume_after_wifi(const sk_event_t *evt, void *user)
{
    (void)evt; (void)user;
    if (s_radio_active) return;     // zaten açık
    ESP_LOGI(TAG, "WiFi GOT_IP — BLE advertising'i geri açıyorum");
    start_advertising();
}

static void pairing_event_handler(const sk_event_t *evt, void *user)
{
    (void)user;

    // Pairing window closed via timeout AND no peer connected → owner missed
    // the window, kill the radio so we stop advertising to nobody.
    if (evt->name && strcmp(evt->name, "pairing.mode.close") == 0 &&
        evt->payload_json &&
        strstr(evt->payload_json, "\"reason\":\"timeout\"") != NULL &&
        !skbt_gatt_is_connected()) {
        ESP_LOGI(TAG, "pairing window expired with no connect — stopping BLE");
        sk_transport_ble_stop();
        return;
    }

    // Otherwise refresh advertising so manufacturer-data flags
    // (pairable / bonded) match the new pairing state.
    if (s_radio_active) {
        ble_gap_adv_stop();
        start_advertising();
    } else {
        // Window opened but radio was off (after a previous timeout) —
        // resume advertising so the peer arriving now can be discovered.
        start_advertising();
    }
}

// Wipe the NimBLE bond store whenever sk_auth declares the bond
// revoked (ble.unpair / factory reset). Without this the SKAPP token
// is gone but the BLE link-layer pairing record stays — a peer can
// reconnect without re-pairing, defeating the unpair intent. Also
// terminate any live link so the (now-untrusted) peer doesn't keep
// using the open connection.
static void on_bond_revoked(const sk_event_t *evt, void *user)
{
    (void)evt; (void)user;
    if (s_conn_handle != SK_BLE_NO_CONN) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    int rc = ble_store_clear();
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_store_clear rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "BLE bond store wiped");
    }
}

static void any_event_handler(const sk_event_t *evt, void *user)
{
    (void)user;
    // Forward every non-auth-chatty event to the connected peer as a notify.
    if (!evt->name) return;
    if (strncmp(evt->name, "auth.", 5) == 0) return;  // internal

    // Gate: only forward once the peer has finished the secure-session
    // handshake. During the pairing window (SKBT_CONN_PAIRING) the peer
    // is busy doing ECDH and a notify storm from face/timer/power blocks
    // its `pairing.ecdh.exchange` write; the APP times out and drops the
    // connection. Pre-bond peers see no app events anyway — they only
    // care about the ECDH reply, which goes through a different path.
    if (!skbt_gatt_is_authenticated()) return;

    // Build a compact NDJSON line.
    char buf[512];
    int n;
    // evt->seq is uint32_t — on RISC-V that's `unsigned long`, so %u
    // mismatches under -Werror=format=. Cast to unsigned long + %lu
    // is portable across all ESP32 targets.
    if (evt->payload_json) {
        n = snprintf(buf, sizeof(buf),
                     "{\"evt\":\"%s\",\"seq\":%lu,\"data\":%s}\n",
                     evt->name, (unsigned long)evt->seq, evt->payload_json);
    } else {
        n = snprintf(buf, sizeof(buf),
                     "{\"evt\":\"%s\",\"seq\":%lu}\n",
                     evt->name, (unsigned long)evt->seq);
    }
    if (n > 0) skbt_gatt_notify_event(buf, (size_t)n);
}

// CLI: ble.status — report current advertising/connection state. Lets
// the user verify "BLE is on / I should be able to scan it" without
// guessing from the boot log.
static sk_err_t cmd_ble_status(sk_cli_ctx_t *ctx)
{
    const char *radio = s_radio_active ? "advertising" : "off";
    const char *peer  = (s_conn_handle != SK_BLE_NO_CONN) ? "connected" : "none";
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"name\":\"%s\",\"radio\":\"%s\",\"peer\":\"%s\",\"conn_handle\":%d}",
             sk_identity_get(), radio, peer, (int)s_conn_handle);
    sk_cli_ok(ctx, buf);
    return SK_OK;
}

static const sk_cli_command_t s_cli_status = {
    .name    = "ble.status",
    .summary = "Show BLE radio state and connected peer (if any)",
    .usage   = "ble status",
    .help_block =
        "Returns radio state and the currently connected peer (the SKAPP\n"
        "phone). Fields: state (idle | advertising | connected), peer\n"
        "identifier when connected, MTU, signal strength.\n"
        "\n"
        "Example:\n"
        "  ble status",
    .handler = cmd_ble_status,
};

esp_err_t sk_transport_ble_init(const sk_transport_ble_cfg_t *cfg)
{
    (void)cfg;
    if (s_initialized) return ESP_OK;

    // NVS must be ready before ble_store_config_init.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    // Secure Connections + bonding. No passkey: Just Works is fine because
    // the physical-button pairing gate enforces user intent.
    ble_hs_cfg.sm_io_cap       = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding      = 1;
    ble_hs_cfg.sm_mitm         = 0;
    ble_hs_cfg.sm_sc           = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(skbt_gatt_init());
    // ble_gatts_start() is called by ble_hs_start() inside the host task —
    // calling it manually here registers attributes against an entry pool
    // that the host then frees and reallocates, leaving stale pointers in
    // ble_att_svr_list and crashing on the next find_by_uuid scan.

    // Subscribe to pairing state changes for advertising refresh.
    int sub;
    sk_event_bus_subscribe("pairing.*",       pairing_event_handler, NULL, &sub);
    sk_event_bus_subscribe("auth.bond.revoked", on_bond_revoked,     NULL, &sub);
    sk_event_bus_subscribe("ble.terminate.before-wifi",
                           on_terminate_before_wifi, NULL, &sub);
    sk_event_bus_subscribe("ble.resume.after-wifi",
                           on_resume_after_wifi, NULL, &sub);
    sk_event_bus_subscribe("*",               any_event_handler,     NULL, &sub);

    // Idle-after-disconnect timer (created here, started on disconnect).
    const esp_timer_create_args_t idle_args = {
        .callback = idle_timer_cb,
        .name     = "sk_ble_idle",
    };
    esp_timer_create(&idle_args, &s_idle_timer);

    // Periodic peer-idle evaluator (started on connect, stopped on
    // disconnect). Fires every 30 s, terminates after 10 min of no
    // outbound GATT traffic.
    const esp_timer_create_args_t peer_check_args = {
        .callback = peer_idle_check_cb,
        .name     = "sk_ble_peer",
    };
    esp_timer_create(&peer_check_args, &s_peer_check_t);

    nimble_port_freertos_init(host_task);

    sk_cli_register(&s_cli_status);

    sk_capabilities_register_book("sk_transport_ble", "0.1.0");
    s_initialized = true;
    ESP_LOGI(TAG, "NimBLE CLI transport ready (device=%s)", sk_identity_get());
    return ESP_OK;
}
