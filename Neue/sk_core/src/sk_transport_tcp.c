// TCP NDJSON transport for the SKAPP CLI. One listener task accepts
// clients; each client gets its own task that performs the Mutual
// Challenge-Response handshake (via sk_secure_session) and then shuttles
// CLI lines. TCP only serves bonded peers — pairing must happen over BLE
// first.

#include "sk_transport_tcp.h"
#include "sk_secure_session.h"
#include "sk_cli.h"
#include "sk_auth.h"
#include "sk_capabilities.h"
#include "sk_event_bus.h"
#include "sk_errors.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sk_tcp";

static sk_transport_tcp_cfg_t s_cfg = {
    .port = 8080, .max_clients = 2, .task_priority = 4, .task_stack = 6144,
};

#define CLIENT_MAX 4
// Per-client line buffer. Sized for the worst-case signed write request:
//   body  = {"cmd":"userdata.write","id":N,"args":{"offset":N,"data_b64":"<base64>"}}
//   wire  = {"body":"...","sig":"<32hex>","nonce":N,"ts":N}
// One userdata chunk is USERDATA_CLI_CHUNK = 4096 bytes; base64 inflates
// that to ~5464 chars, plus ~200 chars of envelope/escape overhead lands
// the wire line just under 6 KB. The previous 1024-byte buffer silently
// dropped every chunk write — Notebook saving timed out without any
// firmware-side log because the overflow path resets `line_len` and
// keeps reading more bytes. 8 KB leaves ~2 KB headroom for any future
// envelope fields.
#define CLIENT_LINE_BUF 8192
typedef struct {
    int                 sock;
    sk_secure_session_t session;
    char                line[CLIENT_LINE_BUF];
    size_t              line_len;
} client_t;

static client_t s_clients[CLIENT_MAX];

static bool s_listening = false;

static void client_writer(const char *chunk, size_t len, void *user)
{
    int sock = (int)(intptr_t)user;
    if (sock < 0 || !chunk || !len) return;
    send(sock, chunk, len, 0);
}

static void send_line(int sock, const char *s)
{
    send(sock, s, strlen(s), 0);
}

static void handle_line(client_t *c, const char *line)
{
    // Recovery path mirroring sk_transport_ble_gatt.c:283-298. A bonded
    // peer whose local bond got wiped (SKAPP data cleared, OS migration,
    // factory reset of the phone) can't compute auth.response, so it
    // sends pairing.ecdh.exchange instead. While the pairing window is
    // open, route that one line to the pairing dispatcher and tear down
    // the socket — the peer reconnects bonded with the new token.
    // Without this, TCP recovery is impossible whenever any bond exists
    // on the device and the peer ends up looping on auth.challenge.
    if (sk_auth_pairing_state() == SK_AUTH_PAIRING_OPEN &&
        !sk_secure_session_authed(&c->session) &&
        strstr(line, "\"cmd\":\"pairing.ecdh.exchange\"") != NULL) {
        ESP_LOGI(TAG, "bonded path → repair: peer sent pairing.ecdh.exchange");
        (void)sk_auth_pairing_dispatch_line(line, strlen(line),
                                            client_writer,
                                            (void *)(intptr_t)c->sock);
        // One-shot: peer reconnects bonded after the OK reply.
        close(c->sock);
        c->sock = -1;
        return;
    }

    sk_session_feed_t r = sk_secure_session_feed_line(&c->session, line);
    switch (r) {
    case SK_SESSION_FEED_AUTH_PROGRESSED:
        return;

    case SK_SESSION_FEED_AUTH_INVALID:
        ESP_LOGW(TAG, "invalid handshake — closing sock %d", c->sock);
        close(c->sock);
        c->sock = -1;
        return;

    case SK_SESSION_FEED_PASSTHROUGH:
        if (sk_secure_session_authed(&c->session)) {
            // Every command must come as a signed envelope; the helper
            // verifies HMAC + nonce, then dispatches the inner body.
            sk_secure_session_dispatch_signed(&c->session, line, client_writer,
                                              (void *)(intptr_t)c->sock);
        } else {
            send_line(c->sock, "{\"ok\":false,\"err\":\"ERR_NOT_AUTHENTICATED\"}\n");
        }
        return;
    }
}

// Pairing-mode line handler for TCP. Used only when sk_secure_session
// can't begin (no bond) and pairing mode is currently OPEN. Reads one
// `pairing.ecdh.exchange` line, runs the transport-agnostic dispatcher,
// then signals the caller via the return value whether to expect more
// data (NOT_OPEN keeps the socket open just long enough to flush the
// error JSON; OK / ERR closes immediately so the peer reconnects on
// the bonded path).
static sk_auth_pairing_result_t pairing_handle_one_line(client_t *c,
                                                       const char *line)
{
    return sk_auth_pairing_dispatch_line(line, strlen(line),
                                         client_writer,
                                         (void *)(intptr_t)c->sock);
}

static void client_task(void *arg)
{
    int idx = (int)(intptr_t)arg;
    client_t *c = &s_clients[idx];

    sk_secure_session_reset(&c->session);
    esp_err_t err = sk_secure_session_begin(&c->session, client_writer,
                                            (void *)(intptr_t)c->sock);

    // No bond yet → if pairing window is open, run the same ECDH flow
    // BLE GATT does. Otherwise drop. The peer is expected to send
    // exactly one pairing.ecdh.exchange line, then reconnect on the
    // bonded path for the secure-session handshake.
    bool pairing_mode = false;
    if (err != ESP_OK) {
        if (sk_auth_pairing_state() == SK_AUTH_PAIRING_OPEN) {
            ESP_LOGI(TAG, "client %d: bond missing, pairing window open — "
                          "accepting pairing.ecdh.exchange", idx);
            pairing_mode = true;
        } else {
            ESP_LOGW(TAG, "session begin failed (%s) — closing",
                     esp_err_to_name(err));
            close(c->sock);
            c->sock = -1;
            vTaskDelete(NULL);
            return;
        }
    }

    char rx[512];
    while (c->sock >= 0) {
        int n = recv(c->sock, rx, sizeof(rx), 0);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            char ch = rx[i];
            if (ch == '\n' || ch == '\r') {
                if (c->line_len > 0) {
                    c->line[c->line_len] = '\0';
                    if (pairing_mode) {
                        sk_auth_pairing_result_t r =
                            pairing_handle_one_line(c, c->line);
                        c->line_len = 0;
                        // Pairing is one-shot regardless of outcome —
                        // OK: peer reconnects bonded; ERR/NOT_OPEN: peer
                        // saw the JSON error and we close. Drop the
                        // socket either way.
                        (void)r;
                        goto done;
                    }
                    handle_line(c, c->line);
                    c->line_len = 0;
                }
            } else if (c->line_len < sizeof(c->line) - 1) {
                c->line[c->line_len++] = ch;
            } else {
                c->line_len = 0;  // overflow => drop
            }
        }
    }
done:
    ESP_LOGI(TAG, "client %d closed", idx);
    if (c->sock >= 0) close(c->sock);
    c->sock = -1;
    c->line_len = 0;
    sk_secure_session_reset(&c->session);
    vTaskDelete(NULL);
}

static int alloc_client_slot(void)
{
    for (int i = 0; i < CLIENT_MAX; i++) {
        if (s_clients[i].sock < 0) return i;
    }
    return -1;
}

static void listen_task(void *arg)
{
    (void)arg;
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) { ESP_LOGE(TAG, "socket: %d", errno); vTaskDelete(NULL); return; }
    int one = 1; setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(s_cfg.port),
    };
    if (bind(listen_sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGE(TAG, "bind: %d", errno); close(listen_sock); vTaskDelete(NULL); return;
    }
    if (listen(listen_sock, 2) < 0) {
        ESP_LOGE(TAG, "listen: %d", errno); close(listen_sock); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "listening on port %u", (unsigned)s_cfg.port);

    while (1) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int cs = accept(listen_sock, (struct sockaddr *)&ca, &cl);
        if (cs < 0) continue;
        int slot = alloc_client_slot();
        if (slot < 0) { close(cs); continue; }
        s_clients[slot].sock = cs;
        s_clients[slot].line_len = 0;
        // Auth state lives inside `session` and is reset by client_task
        // via sk_secure_session_reset() — no separate flag on client_t.
        char name[24]; snprintf(name, sizeof(name), "sk_tcp_cli%d", slot);
        xTaskCreate(client_task, name, s_cfg.task_stack, (void *)(intptr_t)slot,
                    s_cfg.task_priority, NULL);
    }
}

static void on_wifi_event(const sk_event_t *evt, void *user)
{
    (void)user;
    if (!evt->payload_json) return;
    if (strstr(evt->payload_json, "\"state\":\"connected\"") && !s_listening) {
        xTaskCreate(listen_task, "sk_tcp_listen", s_cfg.task_stack, NULL,
                    s_cfg.task_priority, NULL);
        s_listening = true;
    }
}

esp_err_t sk_transport_tcp_init(const sk_transport_tcp_cfg_t *cfg)
{
    // Field-by-field merge: only non-zero fields in `cfg` override our
    // defaults. A struct-assign here would zero out the defaults the
    // moment a caller passes a partial config (e.g. `{ .port = 8080 }`),
    // which is exactly how main.c calls us — that bug silently produced
    // task_stack=0/task_priority=0 and made every xTaskCreate fail.
    if (cfg) {
        if (cfg->port)          s_cfg.port          = cfg->port;
        if (cfg->max_clients)   s_cfg.max_clients   = cfg->max_clients;
        if (cfg->task_priority) s_cfg.task_priority = cfg->task_priority;
        if (cfg->task_stack)    s_cfg.task_stack    = cfg->task_stack;
    }
    if (!s_cfg.port) s_cfg.port = 8080;
    for (int i = 0; i < CLIENT_MAX; i++) s_clients[i].sock = -1;

    // Spawn the listener IMMEDIATELY — don't wait for the wifi.state
    // event. lwIP is already up by the time main_app_init() reaches us
    // (esp_netif_init + sk_wifi_init both ran earlier in main.c), so
    // bind(INADDR_ANY:8080) succeeds even before WiFi associates. The
    // accept() call simply waits until traffic shows up. This eliminates
    // two races we hit in the field: (1) xTaskCreate silently failing
    // and leaving s_listening=true forever; (2) the wifi.state.connected
    // event being missed because subscribe ordering put us after the
    // first publish_state() call.
    BaseType_t ok = xTaskCreate(listen_task, "sk_tcp_listen",
                                s_cfg.task_stack, NULL,
                                s_cfg.task_priority, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed (stack=%u prio=%u)",
                 (unsigned)s_cfg.task_stack, (unsigned)s_cfg.task_priority);
        return ESP_ERR_NO_MEM;
    }
    s_listening = true;
    // Eager-listener confirmation: appears at boot, before WiFi associates.
    // Lets us tell from a single monitor capture whether the new firmware
    // is actually running on the device.
    ESP_LOGI(TAG, "init OK — listener task spawned for port %u",
             (unsigned)s_cfg.port);

    // Keep the wifi.state subscription as a defensive no-op: if the
    // listener task crashes for any reason and clears s_listening
    // (future-proofing), a wifi-up event will respawn it.
    int sub;
    sk_event_bus_subscribe("wifi.state", on_wifi_event, NULL, &sub);
    sk_capabilities_register_book("sk_transport_tcp", "0.1.0");
    return ESP_OK;
}
