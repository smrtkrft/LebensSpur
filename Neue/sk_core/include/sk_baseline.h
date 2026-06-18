#pragma once

// sk_baseline — APP-facing baseline commands required by every SmartKraft
// device per shared/cli_contract.md §3:
//   device.info, device.commands, device.status, device.manifest,
//   logs.get, time.set
//
// Auto-initialised by sk_core_init(). Devices do not call this directly.

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bumped together with shared/cli_contract.md.
#define SK_PROTOCOL_VERSION  "0.2.0"

esp_err_t sk_baseline_init(const char *fw_version, const char *build_info);

#ifdef __cplusplus
}
#endif
