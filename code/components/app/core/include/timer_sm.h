/*
 * timer_sm — geri sayım durum makinesi (saf C; zaman now_us olarak verilir).
 * Son tarih aritmetiği: kalan = deadline − now; tik yalnız "değerlendir" demektir,
 * tik kaçması ya da gecikmesi sayımı bozmaz. Cihaz kapalıyken geçen süre sayılmaz (karar 1):
 * açılışta kalan süre kalıcı kayıttan alınır, deadline = now + kalan.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "warn_sched.h"

typedef enum { TS_IDLE, TS_RUNNING, TS_VACATION, TS_STOPPED, TS_TRIGGERED } timer_state_t;

typedef struct {                 /* kalıcı kayıt (rt blob'unun sayaç kısmı) */
    uint8_t  state;
    uint8_t  warn_fired;
    uint32_t total_s;
    uint32_t remaining_s;        /* RUNNING/STOPPED: ana sayaç kalanı */
    uint32_t vac_remaining_s;
    uint32_t vac_total_s;
} timer_rt_t;

typedef struct {
    timer_state_t state;
    uint32_t total_s;
    int64_t  deadline_us;        /* RUNNING */
    uint32_t frozen_s;           /* STOPPED: donmuş kalan */
    int64_t  vac_deadline_us;    /* VACATION */
    uint32_t vac_total_s;
    warn_plan_t plan;
    uint8_t  warn_fired;         /* ateşlenen (ya da atlanan) uyarı sayısı */
    uint8_t  warn_n;
    uint32_t warn_first_s;
} timer_sm_t;

enum { TA_NONE = 0, TA_WARN = 1, TA_TRIGGER = 2, TA_VACATION_END = 4, TA_PERSIST = 8 };

/* rt NULL → IDLE. Uyarı ayarı cfg'den; plan geçersizse uyarısız çalışır (cfg zaten doğrular). */
void       timer_sm_init(timer_sm_t *s, const timer_rt_t *rt, uint8_t warn_n, uint32_t warn_first_s, int64_t now_us);
/* Yeni süre + uyarı ayarı; sayaç tam süreden başlar (tatildeyse tatil sürer). Plan geçersizse durum değişmez. */
warn_err_t timer_sm_set(timer_sm_t *s, uint32_t total_s, uint8_t warn_n, uint32_t warn_first_s, int64_t now_us);
/* Çalışırken uyarı ayarı değişti: plan yeniden, geçilmiş eşikler ateşlenmeden sayılır. */
warn_err_t timer_sm_set_warn(timer_sm_t *s, uint8_t warn_n, uint32_t warn_first_s, int64_t now_us);
int        timer_sm_checkin(timer_sm_t *s, int64_t now_us);          /* 0 ok, 1 tatilde (etkisiz), -1 süre yok */
int        timer_sm_cancel(timer_sm_t *s, int64_t now_us);           /* 0 ok, -1 çalışmıyor */
int        timer_sm_vacation(timer_sm_t *s, uint8_t days, int64_t now_us);   /* 0 ok, -1 süre yok/geçersiz */
int        timer_sm_vacation_cancel(timer_sm_t *s, int64_t now_us);  /* tam süreden başlar (ALTE) */
/* TA_* bitleri. TA_WARN'da *warn_idx (0 tabanlı); birden çok eşik geçildiyse yalnız sonuncusu verilir. */
unsigned   timer_sm_eval(timer_sm_t *s, int64_t now_us, uint8_t *warn_idx);
uint32_t   timer_sm_remaining_s(const timer_sm_t *s, int64_t now_us);
uint32_t   timer_sm_vac_remaining_s(const timer_sm_t *s, int64_t now_us);
void       timer_sm_to_rt(const timer_sm_t *s, int64_t now_us, timer_rt_t *rt);
