#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Announce the device on mDNS:
//   hostname : <sk_identity>.local
//   service  : _skapp._tcp
//   port     : `cli_port` (TCP NDJSON)
//   TXT      : devtype, id, fw
//
// Called once after WiFi STA has an IP. Updates the announcement when
// wifi.state events arrive. Safe to call before WiFi is up — it defers.
esp_err_t sk_mdns_init(uint16_t cli_port, const char *fw_version);

#ifdef __cplusplus
}
#endif
