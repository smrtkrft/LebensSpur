#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t port;           // default 8080
    uint8_t  max_clients;    // default 2
    int      task_priority;  // default 4
    int      task_stack;     // default 6144
} sk_transport_tcp_cfg_t;

// Bring up the TCP NDJSON server. Starts listening only once WiFi STA is
// connected (subscribes to wifi.state). Each accepted client must complete
// Mutual Challenge-Response within 5 s or the connection is dropped.
esp_err_t sk_transport_tcp_init(const sk_transport_tcp_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
