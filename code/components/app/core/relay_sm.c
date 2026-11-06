#include <limits.h>
#include "relay_sm.h"

#define US 1000000LL

void relay_sm_init(relay_sm_t *r, const relay_cfg_t *c)
{
    *r = (relay_sm_t){ .state = RS_OFF, .cfg = *c };
}

void relay_sm_set_cfg(relay_sm_t *r, const relay_cfg_t *c) { r->cfg = *c; }

static void begin_active(relay_sm_t *r, int64_t at)
{
    r->end_deadline_us = r->dur_s ? at + (int64_t)r->dur_s * US : INT64_MAX;
    if (r->cfg.pulse_s) {
        r->state = RS_PULSE_ON;
        r->phase_deadline_us = at + (int64_t)r->cfg.pulse_s * US;
    } else {
        r->state = RS_ON;
    }
}

void relay_sm_trigger(relay_sm_t *r, int64_t now, uint16_t override_s)
{
    r->dur_s = override_s ? override_s : r->cfg.duration_s;
    if (r->cfg.delay_s) {
        r->state = RS_DELAY;
        r->phase_deadline_us = now + (int64_t)r->cfg.delay_s * US;
    } else {
        begin_active(r, now);
    }
}

void relay_sm_off(relay_sm_t *r) { r->state = RS_OFF; }

bool relay_sm_eval(relay_sm_t *r, int64_t now)
{
    for (int guard = 0; guard < 10000; guard++) {    /* geç değerlendirmede kaçan fazları yetiştir */
        if (r->state == RS_DELAY && now >= r->phase_deadline_us) {
            begin_active(r, r->phase_deadline_us);
            continue;
        }
        if (r->state != RS_OFF && r->state != RS_DELAY && now >= r->end_deadline_us) {
            r->state = RS_OFF;
            break;
        }
        if ((r->state == RS_PULSE_ON || r->state == RS_PULSE_OFF) && now >= r->phase_deadline_us) {
            r->state = r->state == RS_PULSE_ON ? RS_PULSE_OFF : RS_PULSE_ON;
            r->phase_deadline_us += (int64_t)r->cfg.pulse_s * US;
            continue;
        }
        break;
    }
    return r->state == RS_ON || r->state == RS_PULSE_ON;
}

bool relay_sm_active(const relay_sm_t *r) { return r->state != RS_OFF; }

uint32_t relay_sm_remaining_s(const relay_sm_t *r, int64_t now)
{
    if (r->state == RS_OFF) return 0;
    if (r->state == RS_DELAY) {
        if (!r->dur_s) return UINT32_MAX;
        int64_t left = r->phase_deadline_us - now + (int64_t)r->dur_s * US;
        return left <= 0 ? 0u : (uint32_t)((left + US - 1) / US);
    }
    if (r->end_deadline_us == INT64_MAX) return UINT32_MAX;
    int64_t left = r->end_deadline_us - now;
    return left <= 0 ? 0u : (uint32_t)((left + US - 1) / US);
}
