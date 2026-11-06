#include <limits.h>
#include <string.h>
#include "outbox.h"

#define US  1000000LL
#define MIN (60LL * US)

void outbox_init(outbox_t *o)
{
    memset(o, 0, sizeof *o);
    o->next_seq = 1;
}

static ob_job_t *alloc(outbox_t *o)
{
    for (int i = 0; i < OUTBOX_MAX; i++) {
        if (o->jobs[i].state == JS_FREE) return &o->jobs[i];
    }
    /* yer yoksa bitmiş en eski kaydı yeniden kullan */
    ob_job_t *best = NULL;
    uint16_t best_age = 0;
    for (int i = 0; i < OUTBOX_MAX; i++) {
        ob_job_t *j = &o->jobs[i];
        uint16_t age = (uint16_t)(o->next_seq - j->seq);
        if (j->state >= JS_DONE && (!best || age > best_age)) { best = j; best_age = age; }
    }
    return best;
}

static int add(outbox_t *o, job_kind_t k, channel_t ch, uint8_t group, uint8_t widx, int64_t now)
{
    ob_job_t *j = alloc(o);
    if (!j) return -1;
    *j = (ob_job_t){ .seq = o->next_seq++, .kind = (uint8_t)k, .ch = (uint8_t)ch, .group = group,
                     .warn_idx = widx, .state = JS_PENDING, .not_before_us = now };
    if (o->next_seq == 0) o->next_seq = 1;
    return j->seq;
}

int outbox_plan_trigger(outbox_t *o, const ob_channels_t *c, uint16_t mask, int64_t now)
{
    for (int i = 0; i < OUTBOX_MAX; i++) {          /* özet yalnız bu tetiklemeyi göstersin */
        ob_job_t *j = &o->jobs[i];
        if (j->kind == JK_TRIGGER && j->state >= JS_DONE) j->state = JS_FREE;
    }
    o->trigger_seq++;
    o->trigger_active = true;
    o->trigger_at_us = now;
    int n = 0;
    if (c->mail) {
        for (uint8_t g = 0; g < OUTBOX_GROUPS; g++) {
            if (mask & (1u << g)) n += add(o, JK_TRIGGER, CH_SMTP, g, 0, now) > 0;
        }
    }
    if (c->webhook) n += add(o, JK_TRIGGER, CH_WEBHOOK, 0, 0, now) > 0;
    if (c->mqtt) n += add(o, JK_TRIGGER, CH_MQTT, 0, 0, now) > 0;
    return n;      /* Telegram tetiklemede yok (karar 6) */
}

int outbox_plan_early(outbox_t *o, const ob_channels_t *c, uint8_t widx, int64_t now)
{
    for (int i = 0; i < OUTBOX_MAX; i++) {          /* yeni uyarı eskisinin yerini alır */
        ob_job_t *j = &o->jobs[i];
        if (j->kind == JK_EARLY_WARN && j->state == JS_PENDING) j->state = JS_CANCELLED;
    }
    int n = 0;
    if (c->mail) n += add(o, JK_EARLY_WARN, CH_SMTP, OUTBOX_OWNER, widx, now) > 0;
    if (c->telegram) n += add(o, JK_EARLY_WARN, CH_TELEGRAM, 0, widx, now) > 0;
    if (c->webhook) n += add(o, JK_EARLY_WARN, CH_WEBHOOK, 0, widx, now) > 0;
    if (c->mqtt) n += add(o, JK_EARLY_WARN, CH_MQTT, 0, widx, now) > 0;
    return n;
}

int outbox_plan_test(outbox_t *o, channel_t ch, uint16_t ext_id, int64_t now)
{
    int seq = add(o, JK_TEST, ch, ch == CH_SMTP ? OUTBOX_OWNER : 0, 0, now);
    if (seq > 0) ((ob_job_t *)outbox_find(o, (uint16_t)seq))->ext_id = ext_id;
    return seq;
}

ob_job_t *outbox_take_next(outbox_t *o, int64_t now, bool link_up)
{
    if (!link_up) return NULL;
    ob_job_t *best = NULL;
    for (int i = 0; i < OUTBOX_MAX; i++) {
        ob_job_t *j = &o->jobs[i];
        if (j->state != JS_PENDING || j->not_before_us > now) continue;
        if (!best || j->kind < best->kind || (j->kind == best->kind && (int16_t)(j->seq - best->seq) < 0)) best = j;
    }
    if (best) best->state = JS_RUNNING;
    return best;
}

int64_t outbox_retry_delay_us(job_kind_t k, uint8_t attempt, uint32_t rnd)
{
    static const int64_t trig[] = { 1 * MIN, 5 * MIN, 15 * MIN, 30 * MIN };
    static const int64_t early[] = { 1 * MIN, 5 * MIN };
    int64_t base;
    if (k == JK_TRIGGER) base = trig[attempt >= 1 && attempt <= 4 ? attempt - 1 : 3];
    else if (k == JK_EARLY_WARN) base = (attempt >= 1 && attempt <= 2) ? early[attempt - 1] : -1;
    else base = -1;
    if (base < 0) return -1;                                       /* vazgeç */
    int64_t span = base / 5;                                       /* ±%10 */
    return base - span / 2 + (int64_t)(rnd % (uint32_t)(span + 1));
}

static bool should_give_up(const outbox_t *o, const ob_job_t *j, int64_t now)
{
    return j->kind == JK_TRIGGER && now - o->trigger_at_us >= (int64_t)OUTBOX_GIVEUP_S * US;
}

void outbox_on_result(outbox_t *o, uint16_t seq, channel_result_t r, int64_t now, uint32_t rnd)
{
    ob_job_t *j = (ob_job_t *)outbox_find(o, seq);
    if (!j || j->state != JS_RUNNING) return;
    j->config_blocked = false;
    switch (r) {
    case CR_OK:
        j->state = JS_DONE;
        return;
    case CR_PERM:
        j->state = JS_FAILED;
        return;
    case CR_CONFIG:                                   /* ayar düzelene kadar bekler, deneme harcamaz */
        if (j->kind == JK_TEST || should_give_up(o, j, now)) { j->state = JS_FAILED; return; }
        j->config_blocked = true;
        j->state = JS_PENDING;
        j->not_before_us = now + 30 * MIN;
        return;
    case CR_RETRY: {
        j->attempt++;
        int64_t d = outbox_retry_delay_us((job_kind_t)j->kind, j->attempt, rnd);
        if (d < 0 || should_give_up(o, j, now + d)) { j->state = JS_FAILED; return; }
        j->state = JS_PENDING;
        j->not_before_us = now + d;
        return;
    }
    }
}

int outbox_cancel_pending_trigger(outbox_t *o)
{
    int n = 0;
    for (int i = 0; i < OUTBOX_MAX; i++) {
        ob_job_t *j = &o->jobs[i];
        if (j->kind == JK_TRIGGER && j->state == JS_PENDING) { j->state = JS_CANCELLED; n++; }
    }
    o->trigger_active = false;
    return n;
}

void outbox_cfg_changed(outbox_t *o, int64_t now)
{
    for (int i = 0; i < OUTBOX_MAX; i++) {
        ob_job_t *j = &o->jobs[i];
        if (j->state == JS_PENDING && j->config_blocked) j->not_before_us = now;
    }
}

int64_t outbox_next_due_us(const outbox_t *o)
{
    int64_t t = INT64_MAX;
    for (int i = 0; i < OUTBOX_MAX; i++) {
        if (o->jobs[i].state == JS_PENDING && o->jobs[i].not_before_us < t) t = o->jobs[i].not_before_us;
    }
    return t;
}

ob_summary_t outbox_trigger_summary(const outbox_t *o)
{
    ob_summary_t s = { 0 };
    for (int i = 0; i < OUTBOX_MAX; i++) {
        const ob_job_t *j = &o->jobs[i];
        if (j->kind != JK_TRIGGER || j->state == JS_FREE) continue;
        if (j->state == JS_PENDING || j->state == JS_RUNNING) s.pending++;
        else if (j->state == JS_DONE) s.done++;
        else if (j->state == JS_FAILED) s.failed++;
        else s.cancelled++;
    }
    return s;
}

const ob_job_t *outbox_find(const outbox_t *o, uint16_t seq)
{
    for (int i = 0; i < OUTBOX_MAX; i++) {
        if (o->jobs[i].state != JS_FREE && o->jobs[i].seq == seq) return &o->jobs[i];
    }
    return NULL;
}

static bool open_state(uint8_t s) { return s == JS_PENDING || s == JS_RUNNING; }

void outbox_to_rt(const outbox_t *o, int64_t now, outbox_rt_t *rt)
{
    memset(rt, 0, sizeof *rt);
    rt->trigger_seq = o->trigger_seq;
    rt->trig_active = o->trigger_active;
    rt->early_idx = 0xFF;
    if (o->trigger_active) rt->since_trigger_s = (uint32_t)((now - o->trigger_at_us) / US);
    for (int i = 0; i < OUTBOX_MAX; i++) {
        const ob_job_t *j = &o->jobs[i];
        if (!open_state(j->state)) continue;
        if (j->kind == JK_TRIGGER) {
            int slot = j->ch == CH_SMTP ? j->group : j->ch == CH_WEBHOOK ? OUTBOX_GROUPS : OUTBOX_GROUPS + 1;
            if (j->ch == CH_SMTP) rt->mail_mask |= (uint16_t)(1u << j->group);
            else rt->other_mask |= (uint8_t)(j->ch == CH_WEBHOOK ? 1 : 2);
            rt->attempts[slot] = j->attempt;
        } else if (j->kind == JK_EARLY_WARN) {
            rt->early_idx = j->warn_idx;
            rt->early_mask |= (uint8_t)(1u << j->ch);
        }
    }
}

void outbox_from_rt(outbox_t *o, const outbox_rt_t *rt, int64_t now)
{
    outbox_init(o);
    o->trigger_seq = rt->trigger_seq;
    o->trigger_active = rt->trig_active;
    o->trigger_at_us = now - (int64_t)rt->since_trigger_s * US;      /* kapalı süre sayılmaz */
    for (uint8_t g = 0; g < OUTBOX_GROUPS; g++) {
        if (rt->mail_mask & (1u << g)) {
            int s = add(o, JK_TRIGGER, CH_SMTP, g, 0, now);
            if (s > 0) ((ob_job_t *)outbox_find(o, (uint16_t)s))->attempt = rt->attempts[g];
        }
    }
    for (int b = 0; b < 2; b++) {
        if (rt->other_mask & (1u << b)) {
            int s = add(o, JK_TRIGGER, b ? CH_MQTT : CH_WEBHOOK, 0, 0, now);
            if (s > 0) ((ob_job_t *)outbox_find(o, (uint16_t)s))->attempt = rt->attempts[OUTBOX_GROUPS + b];
        }
    }
    if (rt->early_idx != 0xFF) {
        for (int ch = 0; ch < CH__COUNT; ch++) {
            if (rt->early_mask & (1u << ch)) {
                add(o, JK_EARLY_WARN, (channel_t)ch, ch == CH_SMTP ? OUTBOX_OWNER : 0, rt->early_idx, now);
            }
        }
    }
}
