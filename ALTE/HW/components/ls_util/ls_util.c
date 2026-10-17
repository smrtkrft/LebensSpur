#include "ls_util.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "cJSON.h"

int ls_json_get_str(const char *json, const char *key, char *out, size_t out_size)
{
    if (!json || !key || !out || out_size == 0) return -1;
    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;
    int len = -1;
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (v && cJSON_IsString(v) && v->valuestring) {
        len = (int)strlen(v->valuestring);
        if ((size_t)len >= out_size) len = (int)out_size - 1;
        memcpy(out, v->valuestring, (size_t)len);
        out[len] = '\0';
    } else if (v && cJSON_IsNumber(v)) {
        /* Sayi gelmis (orn. port: 465): metne cevir */
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%d", (int)v->valuedouble);
        len = (int)strlen(tmp);
        if ((size_t)len >= out_size) len = (int)out_size - 1;
        memcpy(out, tmp, (size_t)len);
        out[len] = '\0';
    } else if (v && cJSON_IsBool(v)) {
        const char *b = cJSON_IsTrue(v) ? "true" : "false";
        len = (int)strlen(b);
        if ((size_t)len >= out_size) len = (int)out_size - 1;
        memcpy(out, b, (size_t)len);
        out[len] = '\0';
    }
    cJSON_Delete(root);
    return len;
}

bool ls_json_get_int(const char *json, const char *key, int *out)
{
    if (!json || !key || !out) return false;
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    bool ok = false;
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (v && cJSON_IsNumber(v)) {
        *out = (int)v->valuedouble;
        ok = true;
    } else if (v && cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
        char *end = NULL;
        long n = strtol(v->valuestring, &end, 10);
        if (end && end != v->valuestring) { *out = (int)n; ok = true; }
    }
    cJSON_Delete(root);
    return ok;
}

bool ls_json_get_bool(const char *json, const char *key, bool *out)
{
    if (!json || !key || !out) return false;
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    bool ok = false;
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (v && cJSON_IsBool(v)) {
        *out = cJSON_IsTrue(v); ok = true;
    } else if (v && cJSON_IsNumber(v)) {
        *out = (v->valuedouble != 0); ok = true;
    } else if (v && cJSON_IsString(v) && v->valuestring) {
        *out = (strcmp(v->valuestring, "true") == 0 || strcmp(v->valuestring, "1") == 0);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

size_t ls_json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t si = 0, di = 0;
    if (!dst || dst_size == 0) return 0;
    if (!src) { dst[0] = '\0'; return 0; }
    while (src[si] && di < dst_size - 1) {
        char c = src[si++];
        const char *esc = NULL;
        switch (c) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default: break;
        }
        if (esc) {
            size_t el = strlen(esc);
            if (di + el > dst_size - 1) break;
            memcpy(&dst[di], esc, el);
            di += el;
        } else if ((unsigned char)c >= 0x20) {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
    return di;
}

void ls_strip_crlf(char *s)
{
    if (!s) return;
    for (; *s; s++) {
        if (*s == '\r' || *s == '\n') *s = ' ';
    }
}
