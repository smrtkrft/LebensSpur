/*
 * storage — harici SPI flash: iki LittleFS bölümü (04-plan-son.md §2.3, karar K2).
 *   /log  (lslog, 1 MB @0)   : olay günlüğü. Bozulursa kendiliğinden biçimlenir; kademe-2'de korunur.
 *   /data (lsdata, kalan)    : yedek, ekler, dosyalar. Sessiz biçimleme YOK: bozuksa ERROR, kullanıcı karar verir.
 * Her bölümde ".ls2611" düzen işareti: yoksa bölüm başka bir düzenden kalmıştır (ALTE slotları) ve
 * 2611 verisi içermez; biçimlenir.
 * Dosya erişimi storage_acquire/release arasında yapılır; biçimleme sayaç sıfırlanınca çalışır.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum { STG_ABSENT, STG_MOUNTED, STG_ERROR, STG_FORMATTING } storage_state_t;
#define STG_LOG  "/log"
#define STG_DATA "/data"

esp_err_t       storage_init(void);
storage_state_t storage_state(const char *mount);
const char     *storage_state_name(storage_state_t s);
bool            storage_acquire(const char *mount);   /* MOUNTED değilse false */
void            storage_release(const char *mount);
esp_err_t       storage_format(const char *mount);    /* sayaç 0 olana dek ≤5 s bekler, yoksa ESP_ERR_TIMEOUT */
/* Kademe-2: /data'yı baştan sona siler (sırlar okunamaz kalsın) ve biçimler. progress(0..100) */
esp_err_t       storage_erase_data_full(void (*progress)(int pct));
esp_err_t       storage_info(const char *mount, size_t *total, size_t *used);
uint32_t        storage_chip_size(void);
const char     *storage_note(void);                   /* açılışta alınan karar (evlog'a), yoksa NULL */
