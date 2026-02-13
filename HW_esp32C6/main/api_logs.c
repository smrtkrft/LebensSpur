#include "api_logs.h"
#include "web_server.h"
#include "web_server_internal.h"
#include "log_manager.h"
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

esp_err_t h_api_logs_get(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    cJSON *root = cJSON_CreateObject();
    cJSON *entries = cJSON_CreateArray();
    if (!root || !entries) {
        if (root) cJSON_Delete(root);
        if (entries) cJSON_Delete(entries);
        return web_server_send_error(req, 500, "No memory");
    }

    char files[LOG_MGR_MAX_FILES][64];
    size_t count = 0;

    if (log_manager_list_files(files, LOG_MGR_MAX_FILES, &count) == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            char *buf = malloc(4096);
            if (!buf) continue;

            size_t read_size = 0;
            if (log_manager_read_file(files[i], buf, 4096, &read_size) == ESP_OK) {
                buf[read_size] = '\0';
                char *line = strtok(buf, "\n");
                while (line) {
                    if (strlen(line) > 0) {
                        cJSON *entry = cJSON_CreateObject();
                        if (entry) {
                            cJSON_AddNumberToObject(entry, "timestamp",
                                (double)(time(NULL) - (count - i) * 3600));
                            cJSON_AddStringToObject(entry, "category", "system");
                            cJSON_AddStringToObject(entry, "message", line);
                            cJSON_AddItemToArray(entries, entry);
                        }
                    }
                    line = strtok(NULL, "\n");
                }
            }
            free(buf);
        }
    }

    cJSON_AddItemToObject(root, "entries", entries);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!out) return web_server_send_error(req, 500, "No memory");

    esp_err_t ret = web_server_send_json(req, out);
    cJSON_free(out);
    return ret;
}

esp_err_t h_api_logs_delete(httpd_req_t *req)
{
    if (!check_auth(req)) return send_unauthorized(req);
    ws_request_count++;

    if (log_manager_clear_all() == ESP_OK) {
        return web_server_send_json(req, "{\"success\":true}");
    }
    return web_server_send_error(req, 500, "Clear logs failed");
}
