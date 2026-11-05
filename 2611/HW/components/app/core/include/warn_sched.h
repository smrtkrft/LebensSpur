/*
 * warn_sched — erken uyarı planı (model C, docs/06-alarm-tasarim.md). Saf C, saniye cinsinden.
 * k. uyarı süre dolmadan off_s[k] sn önce; off_s azalan sırada (ilk uyarı en büyük).
 * n=1: yalnız son uyarı (L). n≥2: ilk (W) ve son (L) sabit, aradakiler geometrik.
 */
#pragma once
#include <stdint.h>

#define WARN_MAX       10
#define WARN_MIN_GAP_S 60u   /* kısa sayaçlarda warn_floor_s ile ölçeklenir (D2) */

typedef struct {
    uint8_t  n;
    uint32_t off_s[WARN_MAX];
} warn_plan_t;

typedef enum {
    WARN_OK = 0,
    WARN_ERR_COUNT,      /* n 1..WARN_MAX dışında */
    WARN_ERR_FIRST,      /* ilk uyarı süreye sığmıyor (W >= T ya da W < floor) */
    WARN_ERR_GAP,        /* n uyarı floor aralıkla sığmıyor */
} warn_err_t;

uint32_t   warn_auto_first(uint32_t total_s);                 /* F1: T/4 (son çeyrek) */
uint32_t   warn_floor_s(uint32_t total_s);                    /* en kısa uyarı/aralık: clamp(T/10, 15, 60) */
uint32_t   warn_last_s(uint32_t total_s);                     /* L1: T/24, en çok 1 gün, en az floor */
warn_err_t warn_plan(uint32_t total_s, uint8_t n, uint32_t first_s, warn_plan_t *out);  /* first_s 0 = otomatik */
uint8_t    warn_max_count(uint32_t total_s, uint32_t first_s);                          /* 0 = hiç sığmıyor */
/* kalan süreye göre zaten geçilmiş eşik sayısı (off_s[k] >= remaining olanlar) */
uint8_t    warn_passed(const warn_plan_t *p, uint32_t remaining_s);
