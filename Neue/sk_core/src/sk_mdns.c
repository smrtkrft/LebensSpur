#include "sk_mdns.h"
#include "sk_event_bus.h"
#include "sk_identity.h"

#include <string.h>

#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "sk_mdns";

static uint16_t    s_port       = 8080;
static char        s_fw[16]     = "0.0.0";
static bool        s_announced  = false;

static void announce(void)
{
    if (s_announced) return;
    const char *id = sk_identity_get();
    mdns_hostname_set(id);
    mdns_instance_name_set(id);

    mdns_txt_item_t txt[] = {
        { "devtype", (char *)sk_identity_get_prefix() },
        { "id",      (char *)id },
        { "fw",      s_fw },
    };
    esp_err_t err = mdns_service_add(NULL, "_skapp", "_tcp", s_port,
                                     txt, sizeof(txt)/sizeof(txt[0]));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_service_add: %s", esp_err_to_name(err));
        return;
    }
    s_announced = true;
    char payload[96];
    snprintf(payload, sizeof(payload),
             "{\"hostname\":\"%s\",\"service\":\"_skapp._tcp\",\"port\":%u}",
             id, (unsigned)s_port);
    sk_event_bus_publish("mdns.announced", payload);
    ESP_LOGI(TAG, "announced %s.local:%u", id, (unsigned)s_port);
}

static void on_wifi(const sk_event_t *evt, void *user)
{
    (void)user;
    if (!evt->payload_json) return;
    // Cheap check instead of JSON parse.
    if (strstr(evt->payload_json, "\"state\":\"connected\"")) {
        announce();
    }
}

esp_err_t sk_mdns_init(uint16_t cli_port, const char *fw_version)
{
    if (cli_port) s_port = cli_port;
    if (fw_version) {
        strncpy(s_fw, fw_version, sizeof(s_fw) - 1);
        s_fw[sizeof(s_fw) - 1] = '\0';
    }
    esp_err_t err = mdns_init();
    if (err != ESP_OK) return err;
    int sub;
    sk_event_bus_subscribe("wifi.state", on_wifi, NULL, &sub);
    return ESP_OK;
}
