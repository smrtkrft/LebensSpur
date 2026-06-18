idf_component_register(
    SRCS
        # Core foundation
        "src/sk_core.c"
        "src/sk_identity.c"
        "src/sk_errors.c"
        "src/sk_event_bus.c"
        # Structured event log ring buffer (NimBLE-safe async queue).
        # Spec: esp32/COMMON_LOG_SPEC.md
        "src/sk_log.c"
        "src/sk_cli.c"
        "src/sk_capabilities.c"
        # APP-facing baseline commands per shared/cli_contract.md §3
        "src/sk_baseline.c"
        # USB Serial/JTAG transport
        "src/sk_transport_usb.c"
        # Auth — pairing, ECDH, mutual C-R, HMAC, confirm tokens
        "src/sk_auth.c"
        "src/sk_auth_ecdh.c"
        "src/sk_auth_handshake.c"
        "src/sk_auth_hmac.c"
        "src/sk_auth_confirm.c"
        # Optional content-access passphrase (PBKDF2 + lockout + mode toggles)
        "src/sk_passphrase.c"
        # Connection-level mutual C-R + signed envelope verifier
        "src/sk_secure_session.c"
        # WiFi STA + mDNS
        "src/sk_wifi.c"
        "src/sk_mdns.c"
        # BLE GATT transport (NimBLE)
        "src/sk_transport_ble.c"
        "src/sk_transport_ble_gatt.c"
        # WiFi TCP NDJSON transport
        "src/sk_transport_tcp.c"
        # I/O hardware abstraction
        "src/sk_button.c"
        "src/sk_led.c"
        # Control button gesture interpreter + device.restart/factory-reset CLI
        "src/sk_control.c"
        # Firmware OTA — manifest-driven, two-step (check → start), sha256 verify
        "src/sk_ota.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES freertos esp_event json esp_wifi esp_netif bt
    PRIV_REQUIRES nvs_flash esp_system esp_partition mbedtls esp_timer
                  console vfs esp_vfs_console mdns lwip driver
                  app_update esp_https_ota esp_http_client esp_app_format
)
