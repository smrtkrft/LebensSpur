#pragma once

// Centralized error code catalog for all sk_* and device-specific code.
// The device emits { "err": "ERR_CODE", "params": {...} } — translation to a
// user-facing message is done by the APP (i18n). The English message is only
// for device-side logs and the human-mode CLI.
//
// New codes should be added via X-macro below.

#ifdef __cplusplus
extern "C" {
#endif

// X-macro: one entry per error code. `X(symbol, "kod string", "EN message")`.
//
// Symbols are ints assigned in declaration order starting at 1.
// SK_OK is special (0) and reserved for the absence of an error.
#define SK_ERROR_TABLE(X)                                                       \
    /* protocol / dispatcher / auth */                                          \
    X(SK_ERR_UNKNOWN_COMMAND,          "ERR_UNKNOWN_COMMAND",          "Unknown command")                    \
    X(SK_ERR_INVALID_ARG,              "ERR_INVALID_ARG",              "Argument is invalid")                \
    X(SK_ERR_MISSING_ARG,              "ERR_MISSING_ARG",              "Required argument is missing")       \
    X(SK_ERR_NOT_AUTHENTICATED,        "ERR_NOT_AUTHENTICATED",        "Handshake not completed")            \
    X(SK_ERR_NONCE_REPLAY,             "ERR_NONCE_REPLAY",             "Nonce already used")                 \
    X(SK_ERR_TIMESTAMP_OUT_OF_WINDOW,  "ERR_TIMESTAMP_OUT_OF_WINDOW",  "Timestamp outside accepted window")  \
    X(SK_ERR_HMAC_INVALID,             "ERR_HMAC_INVALID",             "Message signature does not verify")  \
    X(SK_ERR_CONFIRM_TOKEN_REQUIRED,   "ERR_CONFIRM_TOKEN_REQUIRED",   "Critical command needs confirm token")\
    X(SK_ERR_CONFIRM_TOKEN_INVALID,    "ERR_CONFIRM_TOKEN_INVALID",    "Confirm token is invalid or expired")\
    /* storage / files */                                                       \
    X(SK_ERR_NVS_READ,                 "ERR_NVS_READ",                 "NVS read failed")                    \
    X(SK_ERR_NVS_WRITE,                "ERR_NVS_WRITE",                "NVS write failed")                   \
    X(SK_ERR_NVS_FULL,                 "ERR_NVS_FULL",                 "NVS namespace full")                 \
    X(SK_ERR_FILE_NOT_FOUND,           "ERR_FILE_NOT_FOUND",           "File not found")                     \
    X(SK_ERR_FILE_TOO_LARGE_FOR_BLE,   "ERR_FILE_TOO_LARGE_FOR_BLE",   "File exceeds BLE size limit; use WiFi/USB") \
    X(SK_ERR_FILE_CRC_MISMATCH,        "ERR_FILE_CRC_MISMATCH",        "File chunk CRC mismatch")            \
    X(SK_ERR_FILE_HASH_MISMATCH,       "ERR_FILE_HASH_MISMATCH",       "File hash mismatch on finalize")     \
    X(SK_ERR_NO_SPACE,                 "ERR_NO_SPACE",                 "Not enough storage")                 \
    /* network */                                                               \
    X(SK_ERR_WIFI_NOT_CONNECTED,       "ERR_WIFI_NOT_CONNECTED",       "WiFi STA is not connected")          \
    X(SK_ERR_WIFI_AUTH,                "ERR_WIFI_AUTH",                "WiFi authentication failed")         \
    X(SK_ERR_WIFI_TIMEOUT,             "ERR_WIFI_TIMEOUT",             "WiFi connection timed out")          \
    X(SK_ERR_WIFI_SCAN_FAIL,           "ERR_WIFI_SCAN_FAIL",           "WiFi scan failed")                   \
    X(SK_ERR_INVALID_CIDR,             "ERR_INVALID_CIDR",             "Static IP format invalid")           \
    /* timer / generic value */                                                 \
    X(SK_ERR_INVALID_UNIT,             "ERR_INVALID_UNIT",             "Invalid unit")                       \
    X(SK_ERR_INVALID_VALUE,            "ERR_INVALID_VALUE",            "Value out of range")                 \
    X(SK_ERR_NOT_FOUND,                "ERR_NOT_FOUND",                "Not found")                          \
    /* integrations */                                                          \
    X(SK_ERR_SMTP_NO_CONFIG,           "ERR_SMTP_NO_CONFIG",           "SMTP not configured")                \
    X(SK_ERR_SMTP_AUTH,                "ERR_SMTP_AUTH",                "SMTP authentication rejected")       \
    X(SK_ERR_SMTP_CONNECT,             "ERR_SMTP_CONNECT",             "SMTP connection failed")             \
    X(SK_ERR_SMTP_TLS,                 "ERR_SMTP_TLS",                 "SMTP TLS handshake failed")          \
    X(SK_ERR_INVALID_EMAIL,            "ERR_INVALID_EMAIL",            "Email address format invalid")       \
    /* Telegram error codes REMOVED — preset is no longer supported.       */ \
    /* Devices that need Telegram delivery use a generic webhook against a */ \
    /* bot proxy instead.                                                  */ \
    /* ota / lifecycle */                                                       \
    X(SK_ERR_OTA_IN_PROGRESS,          "ERR_OTA_IN_PROGRESS",          "OTA already in progress")            \
    X(SK_ERR_NO_UPDATE,                "ERR_NO_UPDATE",                "No update available")                \
    X(SK_ERR_NO_ROLLBACK,              "ERR_NO_ROLLBACK",              "No valid firmware to roll back to")  \
    X(SK_ERR_BUSY,                     "ERR_BUSY",                     "Device is busy")                     \
    /* BLE / pairing */                                                         \
    X(SK_ERR_BLE_NO_BOND,              "ERR_BLE_NO_BOND",              "No BLE bond; pair first")            \
    X(SK_ERR_PAIRING_MODE_CLOSED,      "ERR_PAIRING_MODE_CLOSED",      "Pairing mode is closed")             \
    /* sk_api outbound */                                                       \
    X(SK_ERR_API_TIMEOUT,              "ERR_API_TIMEOUT",              "API call timed out")                 \
    X(SK_ERR_API_CONNECT,              "ERR_API_CONNECT",              "API connect failed")                 \
    X(SK_ERR_API_TLS,                  "ERR_API_TLS",                  "API TLS handshake failed")           \
    X(SK_ERR_API_BAD_STATUS,           "ERR_API_BAD_STATUS",           "API got non-2xx status")             \
    X(SK_ERR_API_NOT_FOUND,            "ERR_API_NOT_FOUND",            "Endpoint name not configured")       \
    X(SK_ERR_API_INVALID_TYPE,         "ERR_API_INVALID_TYPE",         "Unknown endpoint type")              \
    X(SK_ERR_API_NOT_CONFIGURED,       "ERR_API_NOT_CONFIGURED",       "Endpoint missing required field")    \
    X(SK_ERR_API_DISABLED,             "ERR_API_DISABLED",             "API master switch is off")           \
    X(SK_ERR_API_OFFLINE,              "ERR_API_OFFLINE",              "WiFi STA not connected")             \
    /* internal / catch-all */                                                  \
    X(SK_ERR_INTERNAL,                 "ERR_INTERNAL",                 "Internal device error")

typedef enum {
    SK_OK = 0,
#define X(sym, ...) sym,
    SK_ERROR_TABLE(X)
#undef X
    SK_ERR__COUNT
} sk_err_t;

// Canonical string for the NDJSON `err` field. Returns "ERR_UNKNOWN" if code
// is out of range. Never NULL.
const char *sk_err_code_string(sk_err_t code);

// English message used in device logs and human-mode CLI. APP translates via
// i18n based on the code string + params. Never NULL.
const char *sk_err_message(sk_err_t code);

#ifdef __cplusplus
}
#endif
