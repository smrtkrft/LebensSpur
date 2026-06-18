# Minimal SmartKraft Device

Starter template. Boots sk_core (USB CLI included) and registers a single
`demo.ping` command. Use as the skeleton for a new device that doesn't
need OTA / HTTP API yet.

## Build

```bash
cd esp32/skapp-library/examples/minimal
idf.py set-target esp32c6
idf.py build flash monitor
```

On Windows host, the board enumerates as a COM port — connect any terminal
(PuTTY, `idf.py monitor`, Windows Terminal serial) at 115200 8N1.

## Try it

```
sk> help
Commands (type 'help <name>' for details):
  help                         List commands or show detail of one
  json.on                      Switch to NDJSON machine mode
  json.off                     Switch to human mode
  device.capabilities          List loaded books and command versions
  device.restart               Restart the device
  device.factory-reset         Wipe bond, settings, return to factory state
  demo.ping                    Reply with pong

sk> demo ping
ok. {"pong":true}
```

## Extend

To add a device-specific namespace, register commands before
`sk_transport_usb_init()`:

```c
static sk_err_t cmd_foo_bar(sk_cli_ctx_t *ctx) {
    // args available via sk_cli_arg(ctx, i) or sk_cli_arg_named(ctx, "key")
    sk_cli_ok(ctx, "{\"done\":true}");
    return SK_OK;
}
static const sk_cli_command_t cmd_foo_bar_def = {
    .name = "foo.bar",
    .summary = "Does the foo-bar thing",
    .usage = "foo bar [--count N]",
    .handler = cmd_foo_bar,
};
sk_cli_register(&cmd_foo_bar_def);
```

Add further books (sk_api, sk_ota) by updating `main/CMakeLists.txt`
`PRIV_REQUIRES` and `app_main`. BLE, WiFi, mDNS, TCP and auth are already
included in sk_core baseline.
