/* tmpl — mesaj şablonu yer tutucuları (saf C). ALTE: {status} {time} {device}; 2611: + {remaining} {k} {n}. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *status;      /* "early_warning" | "triggered" | "test" */
    const char *time;        /* ISO8601 ya da "+<uptime>s" */
    const char *device;
    const char *remaining;   /* insan okunur kalan süre */
    int k, n;                /* uyarı sırası (1 tabanlı) / sayısı; 0 = yok */
} tmpl_vars_t;

/* json_escape: değerler JSON dizesi içine güvenle girsin (webhook/MQTT gövdesi). Sığmazsa kırpar, NUL sonlu. */
size_t tmpl_expand(const char *tpl, const tmpl_vars_t *v, char *out, size_t cap, bool json_escape);
