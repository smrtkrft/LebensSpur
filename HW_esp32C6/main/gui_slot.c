/**
 * GUI Slot Manager - A/B GUI Versiyon Yonetimi
 *
 * /ext/gui.json metadata ile aktif/yedek slot takibi.
 * Her zaman inaktif slot'a yazilir, aktif slot korunur.
 *
 * Health check: GUI yuklendikten sonra /api/gui/health
 * cagirilmazsa boot_count artar. 3 basarisiz boot sonrasi
 * otomatik rollback yapilir.
 */

#include "gui_slot.h"
#include "file_manager.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "GUI_SLOT";

static gui_slot_meta_t s_meta = {
    .active = GUI_SLOT_A,
    .ver_a = "",
    .ver_b = "",
    .boot_count = 0,
    .health_confirmed = false,
};
static bool s_initialized = false;

// ============================================================================
// Metadata I/O
// ============================================================================

static esp_err_t load_meta(void)
{
    char buf[256];
    esp_err_t ret = file_manager_read_string(GUI_META_FILE, buf, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "gui.json okunamadi, varsayilan");
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        ESP_LOGW(TAG, "gui.json parse hatasi");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *active = cJSON_GetObjectItem(json, "active");
    if (cJSON_IsString(active)) {
        s_meta.active = (strcmp(active->valuestring, "b") == 0) ? GUI_SLOT_B : GUI_SLOT_A;
    }

    cJSON *va = cJSON_GetObjectItem(json, "ver_a");
    if (cJSON_IsString(va)) {
        strncpy(s_meta.ver_a, va->valuestring, sizeof(s_meta.ver_a) - 1);
    }

    cJSON *vb = cJSON_GetObjectItem(json, "ver_b");
    if (cJSON_IsString(vb)) {
        strncpy(s_meta.ver_b, vb->valuestring, sizeof(s_meta.ver_b) - 1);
    }

    cJSON *bc = cJSON_GetObjectItem(json, "boot_count");
    if (cJSON_IsNumber(bc)) {
        s_meta.boot_count = bc->valueint;
    }

    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t save_meta(void)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\n"
        "  \"active\": \"%s\",\n"
        "  \"ver_a\": \"%s\",\n"
        "  \"ver_b\": \"%s\",\n"
        "  \"boot_count\": %d\n"
        "}\n",
        s_meta.active == GUI_SLOT_B ? "b" : "a",
        s_meta.ver_a,
        s_meta.ver_b,
        s_meta.boot_count
    );

    return file_manager_write_string(GUI_META_FILE, buf);
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t gui_slot_init(void)
{
    if (s_initialized) return ESP_OK;

    load_meta();  // Hata olursa varsayilan degerler kullanilir
    s_meta.health_confirmed = false;

    s_initialized = true;
    ESP_LOGI(TAG, "OK - aktif=%s, ver_a=%s, ver_b=%s, boot=%d",
             s_meta.active == GUI_SLOT_B ? "B" : "A",
             s_meta.ver_a[0] ? s_meta.ver_a : "-",
             s_meta.ver_b[0] ? s_meta.ver_b : "-",
             s_meta.boot_count);

    return ESP_OK;
}

const char *gui_slot_get_active_path(void)
{
    return (s_meta.active == GUI_SLOT_B) ? GUI_SLOT_B_PATH : GUI_SLOT_A_PATH;
}

const char *gui_slot_get_inactive_path(void)
{
    return (s_meta.active == GUI_SLOT_B) ? GUI_SLOT_A_PATH : GUI_SLOT_B_PATH;
}

gui_slot_id_t gui_slot_get_active(void)
{
    return s_meta.active;
}

const char *gui_slot_get_active_version(void)
{
    return (s_meta.active == GUI_SLOT_B) ? s_meta.ver_b : s_meta.ver_a;
}

const char *gui_slot_get_backup_version(void)
{
    return (s_meta.active == GUI_SLOT_B) ? s_meta.ver_a : s_meta.ver_b;
}

esp_err_t gui_slot_switch(const char *new_version)
{
    // Inaktif slot'u aktif yap
    gui_slot_id_t new_active = (s_meta.active == GUI_SLOT_B) ? GUI_SLOT_A : GUI_SLOT_B;

    // Versiyon guncelle
    if (new_active == GUI_SLOT_A) {
        strncpy(s_meta.ver_a, new_version ? new_version : "?", sizeof(s_meta.ver_a) - 1);
    } else {
        strncpy(s_meta.ver_b, new_version ? new_version : "?", sizeof(s_meta.ver_b) - 1);
    }

    s_meta.active = new_active;
    s_meta.boot_count = 0;
    s_meta.health_confirmed = false;

    esp_err_t ret = save_meta();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Slot degistirildi: %s (v%s)",
                 new_active == GUI_SLOT_B ? "B" : "A",
                 new_version ? new_version : "?");
    }
    return ret;
}

esp_err_t gui_slot_rollback(void)
{
    const char *backup_ver = gui_slot_get_backup_version();
    if (!backup_ver[0]) {
        ESP_LOGW(TAG, "Rollback: yedek slot bos!");
        return ESP_ERR_NOT_FOUND;
    }

    gui_slot_id_t old = s_meta.active;
    s_meta.active = (old == GUI_SLOT_B) ? GUI_SLOT_A : GUI_SLOT_B;
    s_meta.boot_count = 0;
    s_meta.health_confirmed = false;

    esp_err_t ret = save_meta();
    if (ret == ESP_OK) {
        ESP_LOGW(TAG, "ROLLBACK: %s -> %s (v%s)",
                 old == GUI_SLOT_B ? "B" : "A",
                 s_meta.active == GUI_SLOT_B ? "B" : "A",
                 gui_slot_get_active_version());
    }
    return ret;
}

esp_err_t gui_slot_health_ok(void)
{
    if (s_meta.health_confirmed) return ESP_OK;

    s_meta.health_confirmed = true;
    s_meta.boot_count = 0;

    ESP_LOGI(TAG, "Health OK - boot_count sifirlandi");
    return save_meta();
}

bool gui_slot_check_health(void)
{
    if (!s_initialized) return false;

    // Boot sayacini artir
    s_meta.boot_count++;
    save_meta();

    ESP_LOGI(TAG, "Boot sayaci: %d/%d", s_meta.boot_count, GUI_HEALTH_MAX_FAILS);

    // Limit asildi mi?
    if (s_meta.boot_count >= GUI_HEALTH_MAX_FAILS) {
        const char *backup = gui_slot_get_backup_version();
        if (backup[0]) {
            ESP_LOGW(TAG, "Otomatik rollback tetiklendi (%d basarisiz boot)",
                     s_meta.boot_count);
            gui_slot_rollback();
            return true;
        } else {
            ESP_LOGE(TAG, "Rollback yapilamaz: yedek slot bos");
        }
    }

    return false;
}

bool gui_slot_has_gui(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/index.html", gui_slot_get_active_path());
    return file_manager_exists(path);
}

const gui_slot_meta_t *gui_slot_get_meta(void)
{
    return &s_meta;
}
