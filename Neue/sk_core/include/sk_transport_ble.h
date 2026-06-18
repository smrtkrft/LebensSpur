#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// SKAPP custom GATT service UUID (128-bit, random-assigned for SmartKraft):
//   f100d001-7a5b-4c1e-8d2f-4a6b9c3e1d01 — service
//   f100d002-7a5b-4c1e-8d2f-4a6b9c3e1d01 — cmd_rx  (write, notify auth ack)
//   f100d003-7a5b-4c1e-8d2f-4a6b9c3e1d01 — event_tx (notify, incl. handshake)
//
// All payloads are UTF-8 NDJSON lines (one JSON object per write). If a line
// exceeds the negotiated ATT MTU the writer splits it at the application
// layer using length-prefix framing (4-byte big-endian length + chunk).

typedef struct {
    int  task_priority;  // advertising / event task priority, default 4
} sk_transport_ble_cfg_t;

// Initialize NimBLE, register the SKAPP GATT service, start advertising.
// The advertising flag "pairable" is on only while sk_auth_pairing_state()
// is OPEN. After pairing the service remains reachable only on bonded
// peers.
esp_err_t sk_transport_ble_init(const sk_transport_ble_cfg_t *cfg);

// Resume advertising. Called when the pairing window opens or when the
// owner short-presses the control button on a previously stopped radio.
// No-op if already advertising.
esp_err_t sk_transport_ble_start(void);

// Stop advertising — peer can no longer discover us. Triggered automatically
// by:
//   - the pairing window closing with reason="timeout" and no peer connected,
//   - the 60 s idle timer that starts when a peer disconnects.
// Active connections are NOT torn down — only new discovery is blocked.
// No-op if already stopped.
esp_err_t sk_transport_ble_stop(void);

#ifdef __cplusplus
}
#endif
