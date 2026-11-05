/* http — 04-plan-son.md §2.7; adim 3.3'te doldurulur. */
#pragma once
#include <stddef.h>

typedef struct {
    const char *path;          /* "/index.html", "/s/app.js", "/lang/tr.json" ... */
    const unsigned char *start;
    size_t len;
    const char *ctype;
} http_embedded_t;

/* Gomulu GUI dosyalari (hepsi gzip); hash = gui_hash.txt icerigi. */
const http_embedded_t *http_embedded_files(size_t *count, const char **hash);
