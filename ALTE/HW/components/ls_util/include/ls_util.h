#pragma once
/**
 * LebensSpur - Ortak yardimcilar
 *  - ls_json_get_str  : JSON metninden string alan oku (cJSON; tek dogru ayristirici,
 *                       kacisli tirnak/ters bolu/yeni satir dogru cozulur)
 *  - ls_json_get_int  : sayi veya sayi iceren string alan oku
 *  - ls_json_get_bool : true/false, 1/0 veya "true"/"false" string
 *  - ls_json_escape   : JSON string icine gomulecek metni kacisla (" \ \n \r \t)
 *  - ls_strip_crlf    : SMTP basligi gibi tek satir alanlardan CR/LF temizle
 */
#include <stddef.h>
#include <stdbool.h>

/** JSON icindeki "key" string alanini out'a kopyalar. Donus: uzunluk, yoksa/bozuksa -1. */
int ls_json_get_str(const char *json, const char *key, char *out, size_t out_size);

/** Sayi alani (veya sayi iceren string). Donus: bulundu mu. */
bool ls_json_get_int(const char *json, const char *key, int *out);

/** Bool alani (true/false, 1/0, "true"/"false"/"1"/"0"). Donus: bulundu mu. */
bool ls_json_get_bool(const char *json, const char *key, bool *out);

/** JSON string escape. Donus: yazilan uzunluk. dst her zaman NUL ile biter. */
size_t ls_json_escape(const char *src, char *dst, size_t dst_size);

/** CR/LF karakterlerini bosluga cevirir (yerinde). Baslik enjeksiyonunu engeller. */
void ls_strip_crlf(char *s);
