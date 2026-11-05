#include <math.h>
#include "warn_sched.h"

#define ONE_DAY 86400u

uint32_t warn_floor_s(uint32_t T)
{
    uint32_t f = T / 10u;
    return f < 15u ? 15u : f > WARN_MIN_GAP_S ? WARN_MIN_GAP_S : f;
}

uint32_t warn_auto_first(uint32_t T) { return T / 4u; }                       /* F1: son çeyrek */

uint32_t warn_last_s(uint32_t T)                                               /* L1: T/24, en çok 1 gün */
{
    uint32_t l = T / 24u, f = warn_floor_s(T);
    if (l > ONE_DAY) l = ONE_DAY;
    return l < f ? f : l;
}

warn_err_t warn_plan(uint32_t T, uint8_t n, uint32_t first_s, warn_plan_t *out)
{
    out->n = 0;
    if (n < 1 || n > WARN_MAX) return WARN_ERR_COUNT;
    const uint32_t F = warn_floor_s(T);
    const uint32_t L = warn_last_s(T);
    if (L >= T) return WARN_ERR_FIRST;
    if (n == 1) {                                  /* tek uyarı: son şans uyarısı */
        out->off_s[0] = L;
        out->n = 1;
        return WARN_OK;
    }
    uint32_t W = first_s ? first_s : warn_auto_first(T);
    if (W >= T || W <= L) return WARN_ERR_FIRST;
    if (W < L + (uint32_t)(n - 1) * F) return WARN_ERR_GAP;     /* n uyarı taban aralıkla sığmıyor */
    out->off_s[0] = W;
    out->off_s[n - 1] = L;
    /* geometrik: her uyarıda kalan süre aynı oranda azalır; sondan başa en kısa aralık F (D1) */
    for (int k = n - 2; k >= 1; k--) {
        uint32_t geo = (uint32_t)lround((double)W * pow((double)L / (double)W, (double)k / (double)(n - 1)));
        uint32_t min_k = out->off_s[k + 1] + F;
        out->off_s[k] = geo > min_k ? geo : min_k;
    }
    if (n > 2 && out->off_s[1] + F > W) {          /* yaslama ilk uyarıya taştı: eşit aralığa düş */
        for (int k = 1; k < n - 1; k++) {
            out->off_s[k] = W - (uint32_t)lround(((double)(W - L)) * k / (double)(n - 1));
        }
    }
    for (uint8_t k = 1; k < n; k++) {
        if (out->off_s[k - 1] < out->off_s[k] + F) return WARN_ERR_GAP;
    }
    out->n = n;
    return WARN_OK;
}

uint8_t warn_max_count(uint32_t T, uint32_t first_s)
{
    warn_plan_t p;
    uint8_t best = 0;
    for (uint8_t n = 1; n <= WARN_MAX; n++) {
        if (warn_plan(T, n, first_s, &p) == WARN_OK) best = n;
    }
    return best;
}

uint8_t warn_passed(const warn_plan_t *p, uint32_t remaining_s)
{
    uint8_t c = 0;
    for (uint8_t k = 0; k < p->n; k++) {
        if (p->off_s[k] >= remaining_s) c++;
    }
    return c;
}
