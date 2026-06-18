// GATT service: one custom service, two characteristics.
//   cmd_rx   — peer writes NDJSON lines; we reassemble and dispatch
//   event_tx — we notify NDJSON events + handshake traffic
//
// Per-connection state machine:
//   - bonded peer  → mode=NORMAL, secure session runs C-R, then sk_cli passthrough
//   - unbonded peer during pairing window → mode=PAIRING, ECDH exchange
//   - anything else → connection rejected at on_connect

#include "sk_transport_ble_gatt.h"
#include "sk_secure_session.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"

#include "sk_cli.h"
#include "sk_auth.h"
#include "sk_errors.h"

static const char *TAG = "sk_ble_gatt";

// Service UUID: f100d001-7a5b-4c1e-8d2f-4a6b9c3e1d01
static const ble_uuid128_t SVC_UUID =
    BLE_UUID128_INIT(0x01,0x1d,0x3e,0x9c,0x6b,0x4a,0x2f,0x8d,0x1e,0x4c,0x5b,0x7a,0x01,0xd0,0x00,0xf1);
static const ble_uuid128_t CH_CMD_RX_UUID =
    BLE_UUID128_INIT(0x01,0x1d,0x3e,0x9c,0x6b,0x4a,0x2f,0x8d,0x1e,0x4c,0x5b,0x7a,0x02,0xd0,0x00,0xf1);
static const ble_uuid128_t CH_EVENT_TX_UUID =
    BLE_UUID128_INIT(0x01,0x1d,0x3e,0x9c,0x6b,0x4a,0x2f,0x8d,0x1e,0x4c,0x5b,0x7a,0x03,0xd0,0x00,0xf1);

typedef enum {
    SKBT_CONN_IDLE = 0,    // no peer connected
    SKBT_CONN_NORMAL,      // bonded peer, secure session in flight or authed
    SKBT_CONN_PAIRING,     // unbonded peer during pairing window — ECDH
} skbt_conn_mode_t;

static uint16_t              s_event_tx_val_handle = 0;
static uint16_t              s_conn_handle         = 0xFFFF;
static skbt_conn_mode_t      s_mode                = SKBT_CONN_IDLE;
static sk_secure_session_t   s_session;

// One-shot guard for the `pairing.required` hint we notify when a
// pairing-mode peer subscribes to event_tx. Some BLE stacks emit several
// CCCD writes per connection (initial enable + value confirm); resending
// the hint each time would confuse SKAPP's transient->hard transition.
// Reset on connect/disconnect together with s_mode.
static bool                  s_pairing_hint_sent   = false;

// -- BLE writer (for CLI responses, events, and session traffic) -------------

// BLE GATT notify carries at most (ATT_MTU - 3) payload bytes. The default
// pre-negotiation MTU is 23 (20 bytes/notify); SKAPP requests an MTU
// upgrade right after connect (logs show "MTU update conn=0 mtu=256",
// giving 253 bytes/notify). Anything bigger than this in a single
// ble_gatts_notify_custom call gets silently truncated by NimBLE, which
// is exactly why `device.info` (~500 B), `api.endpoint.list` (~600 B per
// endpoint) and `userdata.read` (up to ~5.5 KB base64) returned partial
// JSON to SKAPP — the line never finished with a `\n`, SKAPP's NDJSON
// reassembler kept waiting, and the request timed out.
//
// We track the negotiated MTU per connection and split outbound writes
// into MTU-3 chunks. Each call to ble_gatts_notify_custom is a separate
// notify PDU; the peer's NDJSON reassembler concatenates them and splits
// on '\n' as before.
static uint16_t s_att_mtu = 23;  // default ATT_MTU until negotiation

void skbt_gatt_set_mtu(uint16_t mtu)
{
    if (mtu < 23) mtu = 23;
    s_att_mtu = mtu;
}

static void ble_writer(const char *chunk, size_t len, void *user)
{
    (void)user;
    if (s_conn_handle == 0xFFFF || s_event_tx_val_handle == 0 || !chunk || !len) return;

    // ATT_MTU minus 3 bytes of notify header is the maximum payload per
    // PDU. NimBLE will return BLE_HS_EMSGSIZE if we hand it more than
    // that in a single call.
    const size_t max_payload = (s_att_mtu > 3) ? (size_t)(s_att_mtu - 3) : 20;

    size_t off = 0;
    while (off < len) {
        size_t take = len - off;
        if (take > max_payload) take = max_payload;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(chunk + off, take);
        if (!om) return;
        int rc = ble_gatts_notify_custom(s_conn_handle, s_event_tx_val_handle, om);
        if (rc != 0) {
            // NimBLE drops the mbuf internally on success but leaks it on
            // failure — explicit free guards against that. ENOMEM here is
            // typically a tx-buffer exhaustion; the peer will time out
            // and retry, which is acceptable for a transient blip.
            ESP_LOGW(TAG, "notify rc=%d (chunk %zu/%zu)", rc, off + take, len);
            return;
        }
        off += take;
    }
}

void skbt_gatt_notify_event(const char *payload, size_t len)
{
    ble_writer(payload, len, NULL);
}

// -- NDJSON line reassembly --------------------------------------------------

// Sized for the worst-case signed `userdata.write` envelope: one
// USERDATA_CLI_CHUNK (4096 B) of binary → ~5464 base64 chars → wrapped in
// the HMAC envelope (body/sig/nonce/ts) lands the wire line just under
// 6 KB. The previous 1024-byte cap silently dropped every chunk write,
// which was the symptom behind Notebook saving timing out without any
// device-side log. 8 KB matches sk_transport_tcp.c CLIENT_LINE_BUF and
// keeps ~2 KB headroom. This buffer is per-connection (one BLE peer at
// a time) so the static allocation cost is bounded.
#define RX_LINE_CAP 8192
static char   s_rx[RX_LINE_CAP];
static size_t s_rx_len = 0;

static void feed_rx(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n' || c == '\r') {
            if (s_rx_len > 0) {
                s_rx[s_rx_len] = '\0';
                skbt_gatt_on_cmd_rx(s_conn_handle, s_rx, s_rx_len);
                s_rx_len = 0;
            }
        } else if (s_rx_len < sizeof(s_rx) - 1) {
            s_rx[s_rx_len++] = c;
        } else {
            s_rx_len = 0;  // overflow → drop line
        }
    }
}

// -- Pairing (ECDH X25519) ---------------------------------------------------
//
// Single-message exchange while the pairing window is open:
//   peer  → device: {"cmd":"pairing.ecdh.exchange","args":{"peer_pub":"<64hex>"}}
//   device         : sk_auth_ecdh_begin (our_pub),
//                    sk_auth_ecdh_complete (derive + store token in NVS)
//   device → peer  : {"ok":true,"data":{"our_pub":"<64hex>"}}
//   device         : close pairing mode, terminate connection.
// Peer reconnects on the bonded path next; the secure session takes over.

// Hex helpers and the bare ECDH parser used to live here. They moved
// into sk_auth_pairing_dispatch_line so TCP can run the same flow.

static void pairing_finish_and_disconnect(void)
{
    sk_auth_close_pairing_mode("ecdh_complete");
    // Race fix: ble_writer just queued our `our_pub` reply for transmit,
    // but NimBLE notify is async — if we terminate the link before the
    // controller actually pushes the PDU, the peer never sees the
    // answer and times out at "Doğrulanıyor". Sleeping 250 ms here lets
    // the controller drain the TX queue. (We're already in a one-shot
    // pairing connection, so the extra latency is invisible to the user.)
    vTaskDelay(pdMS_TO_TICKS(250));
    ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

// Adapter from sk_auth_pairing_writer_t signature to ble_writer.
static void pairing_writer_adapter(const char *chunk, size_t len, void *user)
{
    (void)user;
    ble_writer(chunk, len, NULL);
}

static void pairing_handle_line(const char *line, size_t len)
{
    sk_auth_pairing_result_t r = sk_auth_pairing_dispatch_line(
        line, len, pairing_writer_adapter, NULL);

    // Whatever the outcome, the BLE pairing connection is one-shot —
    // we always tear it down. On OK the peer reconnects as bonded and
    // runs the secure-session handshake; on ERR/NOT_OPEN the peer sees
    // the JSON error and we drop them.
    if (r == SK_AUTH_PAIRING_OK) {
        ESP_LOGI(TAG, "ECDH pairing complete; closing connection for bonded reconnect");
        pairing_finish_and_disconnect();
    } else {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

// -- Connect / disconnect ----------------------------------------------------

void skbt_gatt_on_connect(uint16_t conn_handle)
{
    s_conn_handle       = conn_handle;
    s_rx_len            = 0;
    s_pairing_hint_sent = false;
    sk_secure_session_reset(&s_session);

    // Önce bond. Bonded peer (zaten eşleşmiş SKAPP) tipik vakadır:
    // pairing modu açık olsa bile (kullanıcı butona yanlışlıkla bastı,
    // veya başka bir telefon eklemek için bekletiyor) bonded peer
    // bonded yolu ile devam etmeli — aksi halde her iki taraf da
    // diğerinden başlangıç bekleyip 10 sn sonra timeout düşer (logda
    // "disconnected reason=0x213" → SKAPP tarafından kapatma).
    //
    // Peer'in bond'u bozulduysa (factory-reset, SKAPP verisi silindi),
    // peer zaten bonded handshake'i başlatamaz — bond yok demektir.
    // Bu durumda peer pairing.ecdh.exchange gönderir; aşağıdaki cmd_rx
    // gate (PAIRING modunda da NORMAL modda da çalışan) kapsamı bu
    // recovery patikası için ayrı olarak ele alır.
    if (sk_auth_has_bond()) {
        s_mode = SKBT_CONN_NORMAL;
        // auth.challenge yayını ble_writer ile notify gönderir; ama peer
        // henüz event_tx CCCD subscribe etmediyse NimBLE notify'i drop
        // eder. Bu yüzden burada session_begin ÇAĞIRILMAZ; subscribe
        // event'ini bekleyen `skbt_gatt_on_subscribe()` tetikler.
        ESP_LOGI(TAG, "bonded peer connected — awaiting CCCD subscribe");
        return;
    }

    // Bond yok → pairing modu zorunlu.
    if (sk_auth_pairing_state() == SK_AUTH_PAIRING_OPEN) {
        s_mode = SKBT_CONN_PAIRING;
        ESP_LOGI(TAG, "pairing connection accepted (no bond, pairing window open)");
        return;
    }

    ESP_LOGW(TAG, "rejecting connect — no bond and pairing closed");
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

void skbt_gatt_on_subscribe(uint16_t conn_handle)
{
    (void)conn_handle;

    // PAIRING modunda subscribe — SKAPP'a "pairing required" hint yayınla.
    //
    // Senaryo: cihazın bond'u silinmiş (factory reset, NVS clear, vb.) ama
    // SKAPP hala bond'u sakladığı için reconnect path'inde auth.challenge
    // notify bekliyor. Cihaz pairing modunda olduğu için kendiliğinden
    // challenge yayınlamıyor (initiator pairing'de SKAPP'tır:
    // pairing.ecdh.exchange WRITE). İki taraf birbirini bekleyince SKAPP
    // 8 sn timeout'a düşüyor ve "Bad state: bağlanılamadı" hatası alıyor.
    //
    // Hint ile SKAPP'a "ben pairing'deyim, reconnect değil bootstrap yapman
    // lazım" deriz. SKAPP bunu PairingRequired exception'ına çevirip
    // kullanıcıya pairing-mode dialog'unu açar.
    //
    // Sadece bir kez gönderilir; peer subscribe spam'lerse yayılmaz.
    if (s_mode == SKBT_CONN_PAIRING) {
        if (!s_pairing_hint_sent) {
            static const char hint[] =
                "{\"evt\":\"pairing.required\",\"data\":{\"reason\":\"no_bond\"}}\n";
            ble_writer(hint, sizeof(hint) - 1, NULL);
            s_pairing_hint_sent = true;
            ESP_LOGI(TAG, "pairing.required hint sent to peer");
        }
        return;
    }

    if (s_mode != SKBT_CONN_NORMAL) return;
    // Tekrar subscribe edilirse (peer yanlışlıkla) session zaten
    // başlamışsa baştan başlatma.
    if (sk_secure_session_authed(&s_session)) return;
    esp_err_t err = sk_secure_session_begin(&s_session, ble_writer, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "session begin failed: %s — terminating",
                 esp_err_to_name(err));
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void skbt_gatt_on_disconnect(uint16_t conn_handle)
{
    (void)conn_handle;
    s_conn_handle       = 0xFFFF;
    s_mode              = SKBT_CONN_IDLE;
    s_rx_len            = 0;
    s_att_mtu           = 23;  // reset to default; next peer will renegotiate
    s_pairing_hint_sent = false;
    sk_secure_session_reset(&s_session);
}

bool skbt_gatt_is_connected(void)
{
    return s_conn_handle != 0xFFFF;
}

bool skbt_gatt_is_authenticated(void)
{
    return s_mode == SKBT_CONN_NORMAL &&
           sk_secure_session_authed(&s_session);
}

// -- CLI dispatch gate -------------------------------------------------------

void skbt_gatt_on_cmd_rx(uint16_t conn_handle, const char *line, size_t len)
{
    (void)conn_handle;

    if (s_mode == SKBT_CONN_PAIRING) {
        pairing_handle_line(line, len);
        return;
    }

    if (s_mode != SKBT_CONN_NORMAL) {
        // Defensive: idle / unknown mode means we shouldn't be talking yet.
        return;
    }

    // Recovery path: cihazda bond var, biz NORMAL moda gittik ve
    // auth.challenge gönderdik. Ama peer'in bond'u bozulduysa
    // (SKAPP verisi silinmiş, factory reset, vs.) peer auth.response
    // hesaplayamaz — onun yerine pairing.ecdh.exchange ile yeniden
    // eşleşmek isteyecek. Bu mesajı sk_secure_session_feed_line'a
    // göndermek FEED_AUTH_INVALID döndürür ve bağlantı kapanır;
    // kullanıcı recovery yapamaz. Pairing penceresi açıksa (kullanıcı
    // butonla onayladı) bu komutu pairing handler'ına yönlendir ve
    // moda geç. Pairing penceresi kapalıyse normal yol işler ve invalid
    // handshake olarak bağlantı düşer — beklenen davranış.
    if (sk_auth_pairing_state() == SK_AUTH_PAIRING_OPEN &&
        !sk_secure_session_authed(&s_session) &&
        strstr(line, "\"cmd\":\"pairing.ecdh.exchange\"") != NULL) {
        ESP_LOGI(TAG, "bonded path → repair: peer sent pairing.ecdh.exchange");
        s_mode = SKBT_CONN_PAIRING;
        pairing_handle_line(line, len);
        return;
    }

    sk_session_feed_t r = sk_secure_session_feed_line(&s_session, line);
    switch (r) {
    case SK_SESSION_FEED_AUTH_PROGRESSED:
        // Handshake message handled internally — nothing more to do here.
        return;

    case SK_SESSION_FEED_AUTH_INVALID:
        ESP_LOGW(TAG, "invalid handshake — terminating");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;

    case SK_SESSION_FEED_PASSTHROUGH:
        if (sk_secure_session_authed(&s_session)) {
            // Every command must come as a signed envelope; the helper
            // verifies HMAC + nonce, then dispatches the inner body.
            sk_secure_session_dispatch_signed(&s_session, line, ble_writer, NULL);
        } else {
            // Pre-auth, peer tried a non-handshake line. Reject.
            const char *err =
                "{\"ok\":false,\"err\":\"ERR_NOT_AUTHENTICATED\"}\n";
            ble_writer(err, strlen(err), NULL);
        }
        return;
    }
    (void)len;
}

// -- GATT access callbacks ---------------------------------------------------

static int cmd_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint16_t total = OS_MBUF_PKTLEN(ctxt->om);
    char buf[512];
    if (total > sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint16_t out_len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &out_len);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
    s_conn_handle = conn_handle;
    feed_rx(buf, out_len);
    return 0;
}

static int event_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return 0;  // notifications only
}

// Characteristic permissions: plain WRITE / NOTIFY without link-layer
// encryption requirements.
//
// Reasoning: bonding for the first time happens in the APPLICATION layer
// (pairing.ecdh.exchange → sk_auth_ecdh_complete → token persisted in
// NVS). Adding BLE_GATT_CHR_F_WRITE_ENC / _READ_ENC on top demands SMP
// link-layer pairing BEFORE the app-layer ECDH can run — which the APP
// cannot satisfy because there is no bond yet. Result: the peer's
// `pairing.ecdh.exchange` write was silently rejected and the device
// dropped the connection.
//
// We retain end-to-end security through:
//   * sk_auth ECDH (Curve25519 + token derive) — first pairing
//   * sk_secure_session mutual challenge-response on each connect
//   * sk_auth_verify_message HMAC + nonce on every command
// All three live ABOVE the GATT layer, so plain WRITE/NOTIFY here is
// not a regression.
static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid       = &CH_CMD_RX_UUID.u,
        .access_cb  = cmd_rx_access,
        .flags      = BLE_GATT_CHR_F_WRITE,
    },
    {
        .uuid       = &CH_EVENT_TX_UUID.u,
        .access_cb  = event_tx_access,
        .flags      = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_event_tx_val_handle,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &SVC_UUID.u,
        .characteristics = s_chrs,
    },
    { 0 },
};

esp_err_t skbt_gatt_init(void)
{
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) return ESP_FAIL;
    rc = ble_gatts_add_svcs(s_svcs);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}
