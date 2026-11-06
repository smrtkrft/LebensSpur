#include <string.h>
#include "timer_sm.h"

#define US 1000000LL

static uint32_t left_s(int64_t deadline, int64_t now)
{
    int64_t d = deadline - now;
    return d <= 0 ? 0u : (uint32_t)((d + US - 1) / US);     /* yukarı yuvarla: 0 yalnız gerçekten bitince */
}

static void replan(timer_sm_t *s)
{
    if (warn_plan(s->total_s, s->warn_n, s->warn_first_s, &s->plan) != WARN_OK) s->plan.n = 0;
}

static void start_full(timer_sm_t *s, int64_t now)
{
    s->state = TS_RUNNING;
    s->deadline_us = now + (int64_t)s->total_s * US;
    s->warn_fired = 0;
}

void timer_sm_init(timer_sm_t *s, const timer_rt_t *rt, uint8_t n, uint32_t first, int64_t now)
{
    memset(s, 0, sizeof *s);
    s->warn_n = n;
    s->warn_first_s = first;
    if (!rt || rt->total_s == 0 || rt->state > TS_TRIGGERED) return;   /* IDLE */
    s->total_s = rt->total_s;
    replan(s);
    s->state = (timer_state_t)rt->state;
    switch (s->state) {
    case TS_RUNNING:
        if (rt->remaining_s == 0) { s->state = TS_TRIGGERED; s->warn_fired = s->plan.n; break; }
        s->deadline_us = now + (int64_t)rt->remaining_s * US;
        /* geçilmiş eşikler ateşlenmeden sayılır (teslim edilmeyen son uyarıyı outbox taşır) */
        s->warn_fired = warn_passed(&s->plan, rt->remaining_s);
        if (rt->warn_fired > s->warn_fired) s->warn_fired = rt->warn_fired;
        break;
    case TS_STOPPED:
        s->frozen_s = rt->remaining_s;
        break;
    case TS_TRIGGERED:
        s->warn_fired = s->plan.n;                    /* tetiklenmiş: tüm uyarılar geçti */
        break;
    case TS_VACATION:
        s->vac_total_s = rt->vac_total_s;
        s->vac_deadline_us = now + (int64_t)rt->vac_remaining_s * US;
        break;
    default:
        break;
    }
}

warn_err_t timer_sm_set(timer_sm_t *s, uint32_t total_s, uint8_t n, uint32_t first, int64_t now)
{
    warn_plan_t p;
    warn_err_t e = warn_plan(total_s, n, first, &p);
    if (e != WARN_OK) return e;
    s->total_s = total_s;
    s->warn_n = n;
    s->warn_first_s = first;
    s->plan = p;
    if (s->state == TS_VACATION) return WARN_OK;     /* tatil bitince yeni süreyle başlar */
    start_full(s, now);
    return WARN_OK;
}

warn_err_t timer_sm_set_warn(timer_sm_t *s, uint8_t n, uint32_t first, int64_t now)
{
    if (s->total_s == 0) {
        s->warn_n = n;
        s->warn_first_s = first;
        return WARN_OK;
    }
    warn_plan_t p;
    warn_err_t e = warn_plan(s->total_s, n, first, &p);
    if (e != WARN_OK) return e;
    s->warn_n = n;
    s->warn_first_s = first;
    s->plan = p;
    if (s->state == TS_RUNNING) s->warn_fired = warn_passed(&p, left_s(s->deadline_us, now));
    return WARN_OK;
}

int timer_sm_checkin(timer_sm_t *s, int64_t now)
{
    if (s->total_s == 0) return -1;
    if (s->state == TS_VACATION) return 1;
    start_full(s, now);
    return 0;
}

int timer_sm_cancel(timer_sm_t *s, int64_t now)
{
    if (s->state == TS_RUNNING) {
        s->frozen_s = left_s(s->deadline_us, now);
        s->state = TS_STOPPED;
        return 0;
    }
    if (s->state == TS_TRIGGERED) {
        s->frozen_s = 0;
        s->state = TS_STOPPED;
        return 0;
    }
    return -1;
}

int timer_sm_vacation(timer_sm_t *s, uint8_t days, int64_t now)
{
    if (s->total_s == 0 || days < 1 || days > 60) return -1;
    s->state = TS_VACATION;
    s->vac_total_s = (uint32_t)days * 86400u;
    s->vac_deadline_us = now + (int64_t)s->vac_total_s * US;
    return 0;
}

int timer_sm_vacation_cancel(timer_sm_t *s, int64_t now)
{
    if (s->state != TS_VACATION) return -1;
    start_full(s, now);
    return 0;
}

unsigned timer_sm_eval(timer_sm_t *s, int64_t now, uint8_t *warn_idx)
{
    if (s->state == TS_VACATION) {
        if (now < s->vac_deadline_us) return TA_NONE;
        start_full(s, now);
        return TA_VACATION_END | TA_PERSIST;
    }
    if (s->state != TS_RUNNING) return TA_NONE;
    uint32_t rem = left_s(s->deadline_us, now);
    if (rem == 0) {
        s->state = TS_TRIGGERED;
        return TA_TRIGGER | TA_PERSIST;
    }
    uint8_t passed = warn_passed(&s->plan, rem);
    if (passed > s->warn_fired) {
        *warn_idx = (uint8_t)(passed - 1);          /* birden çok eşik: yalnız en sonuncusu */
        s->warn_fired = passed;
        return TA_WARN | TA_PERSIST;
    }
    return TA_NONE;
}

uint32_t timer_sm_remaining_s(const timer_sm_t *s, int64_t now)
{
    switch (s->state) {
    case TS_RUNNING: return left_s(s->deadline_us, now);
    case TS_STOPPED: return s->frozen_s;
    case TS_VACATION: return s->total_s;
    default: return 0;
    }
}

uint32_t timer_sm_vac_remaining_s(const timer_sm_t *s, int64_t now)
{
    return s->state == TS_VACATION ? left_s(s->vac_deadline_us, now) : 0;
}

void timer_sm_to_rt(const timer_sm_t *s, int64_t now, timer_rt_t *rt)
{
    memset(rt, 0, sizeof *rt);
    rt->state = (uint8_t)s->state;
    rt->warn_fired = s->warn_fired;
    rt->total_s = s->total_s;
    rt->remaining_s = timer_sm_remaining_s(s, now);
    rt->vac_total_s = s->vac_total_s;
    rt->vac_remaining_s = timer_sm_vac_remaining_s(s, now);
}
