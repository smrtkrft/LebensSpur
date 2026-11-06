#include <stdio.h>
#include <string.h>
#include "tmpl.h"

static size_t put(char *out, size_t o, size_t cap, const char *s, bool esc)
{
    for (; s && *s && o + 1 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (esc && (c == '"' || c == '\\')) { if (o + 2 >= cap) break; out[o++] = '\\'; out[o++] = (char)c; }
        else if (esc && c < 0x20) { if (o + 6 >= cap) break; o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    return o;
}

size_t tmpl_expand(const char *tpl, const tmpl_vars_t *v, char *out, size_t cap, bool esc)
{
    size_t o = 0;
    char num[12];
    for (const char *p = tpl; *p && o + 1 < cap;) {
        const char *val = NULL;
        size_t skip = 0;
        if (*p == '{') {
            static const char *const names[] = { "{status}", "{time}", "{device}", "{remaining}", "{k}", "{n}" };
            for (int i = 0; i < 6 && !val; i++) {
                size_t l = strlen(names[i]);
                if (strncmp(p, names[i], l) != 0) continue;
                skip = l;
                switch (i) {
                case 0: val = v->status; break;
                case 1: val = v->time; break;
                case 2: val = v->device; break;
                case 3: val = v->remaining; break;
                case 4: snprintf(num, sizeof num, "%d", v->k); val = num; break;
                default: snprintf(num, sizeof num, "%d", v->n); val = num; break;
                }
                if (!val) val = "";
            }
        }
        if (skip) { o = put(out, o, cap, val, esc); p += skip; }
        else out[o++] = *p++;
    }
    out[o] = 0;
    return o;
}
