/* gui_files — gömülü web arayüzü tablosu (build.py çıktıları). */
#include <string.h>
#include "http.h"

extern const unsigned char index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const unsigned char index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const unsigned char app_js_gz_start[] asm("_binary_app_js_gz_start");
extern const unsigned char app_js_gz_end[] asm("_binary_app_js_gz_end");
extern const unsigned char app_css_gz_start[] asm("_binary_app_css_gz_start");
extern const unsigned char app_css_gz_end[] asm("_binary_app_css_gz_end");
extern const unsigned char schema_json_gz_start[] asm("_binary_schema_json_gz_start");
extern const unsigned char schema_json_gz_end[] asm("_binary_schema_json_gz_end");
extern const unsigned char favicon_svg_gz_start[] asm("_binary_favicon_svg_gz_start");
extern const unsigned char favicon_svg_gz_end[] asm("_binary_favicon_svg_gz_end");
extern const unsigned char keys_json_gz_start[] asm("_binary_keys_json_gz_start");
extern const unsigned char keys_json_gz_end[] asm("_binary_keys_json_gz_end");
extern const unsigned char tr_json_gz_start[] asm("_binary_tr_json_gz_start");
extern const unsigned char tr_json_gz_end[] asm("_binary_tr_json_gz_end");
extern const unsigned char en_json_gz_start[] asm("_binary_en_json_gz_start");
extern const unsigned char en_json_gz_end[] asm("_binary_en_json_gz_end");
extern const unsigned char de_json_gz_start[] asm("_binary_de_json_gz_start");
extern const unsigned char de_json_gz_end[] asm("_binary_de_json_gz_end");
extern const unsigned char es_json_gz_start[] asm("_binary_es_json_gz_start");
extern const unsigned char es_json_gz_end[] asm("_binary_es_json_gz_end");
extern const unsigned char fr_json_gz_start[] asm("_binary_fr_json_gz_start");
extern const unsigned char fr_json_gz_end[] asm("_binary_fr_json_gz_end");
extern const unsigned char it_json_gz_start[] asm("_binary_it_json_gz_start");
extern const unsigned char it_json_gz_end[] asm("_binary_it_json_gz_end");
extern const char gui_hash_txt_start[] asm("_binary_gui_hash_txt_start");

static http_embedded_t s_files[] = {
    { "/index.html", index_html_gz_start, 0, "text/html; charset=utf-8" },
    { "/s/app.js", app_js_gz_start, 0, "application/javascript" },
    { "/s/app.css", app_css_gz_start, 0, "text/css" },
    { "/s/schema.json", schema_json_gz_start, 0, "application/json" },
    { "/favicon.svg", favicon_svg_gz_start, 0, "image/svg+xml" },
    { "/s/lang/keys.json", keys_json_gz_start, 0, "application/json" },
    { "/s/lang/tr.json", tr_json_gz_start, 0, "application/json" },
    { "/s/lang/en.json", en_json_gz_start, 0, "application/json" },
    { "/s/lang/de.json", de_json_gz_start, 0, "application/json" },
    { "/s/lang/es.json", es_json_gz_start, 0, "application/json" },
    { "/s/lang/fr.json", fr_json_gz_start, 0, "application/json" },
    { "/s/lang/it.json", it_json_gz_start, 0, "application/json" },
};
static const unsigned char *const s_ends[] = {
    index_html_gz_end,
    app_js_gz_end,
    app_css_gz_end,
    schema_json_gz_end,
    favicon_svg_gz_end,
    keys_json_gz_end,
    tr_json_gz_end,
    en_json_gz_end,
    de_json_gz_end,
    es_json_gz_end,
    fr_json_gz_end,
    it_json_gz_end
};

const http_embedded_t *http_embedded_files(size_t *count, const char **hash)
{
    for (size_t i = 0; i < sizeof(s_files) / sizeof(s_files[0]); i++) {
        s_files[i].len = (size_t)(s_ends[i] - s_files[i].start);
    }
    *count = sizeof(s_files) / sizeof(s_files[0]);
    *hash = gui_hash_txt_start;
    return s_files;
}
