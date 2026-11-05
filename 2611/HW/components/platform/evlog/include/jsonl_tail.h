/* jsonl_tail — satır dosyasını sondan başa doğru okur (saf; masaüstünde test edilir). */
#pragma once
#include <stdbool.h>
#include <stdio.h>

/* Satır geri çağrısı: false dönerse okuma durur. Satır NUL sonlu, sondaki \n yok. */
typedef bool (*jsonl_line_cb_t)(const char *line, void *ctx);

/* f içinde [0, end) aralığını sondan başa okur; en çok `limit` satır verir.
 * *next_end: okunmadan kalan kısmın sonu (0 = dosya bitti). Dönüş: verilen satır sayısı.
 * 512 bayttan uzun satırlar atlanır. */
int jsonl_read_back(FILE *f, long end, int limit, jsonl_line_cb_t cb, void *ctx, long *next_end);
