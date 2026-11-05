/* Model C uyarı planı — tüm kombinasyon taraması (rapor üretir, düzeltme için). */
#include <stdio.h>
#include <string.h>
#include "warn_sched.h"

static const char *unit_name[3] = { "dk", "sa", "gun" };
static const uint32_t unit_s[3] = { 60, 3600, 86400 };
static long viol, valid, rejected;
static char first_viol[40][160];
static int nfv;

static void v(const char *msg, uint32_t T, int n, uint32_t W)
{
    viol++;
    if (nfv < 40) snprintf(first_viol[nfv++], 160, "T=%u n=%d W=%u: %s", T, n, W, msg);
}

static void check(uint32_t T, int n, uint32_t Wreq)
{
    warn_plan_t p;
    warn_err_t e = warn_plan(T, (uint8_t)n, Wreq, &p);
    uint32_t W = Wreq ? Wreq : warn_auto_first(T);
    if (e != WARN_OK) { rejected++; return; }
    valid++;
    if (p.n != n) v("n uyumsuz", T, n, W);
    uint32_t L = warn_last_s(T);
    if (n == 1 && p.off_s[0] != L) v("tek uyari son uyari (L) degil", T, n, W);
    if (n > 1 && p.off_s[0] != W) v("ilk uyari W degil", T, n, W);
    if (p.off_s[n - 1] != L) v("son uyari L degil", T, n, W);
    if (L > 86400) v("son uyari 1 gunden erken", T, n, W);
    if (p.off_s[0] >= T) v("ilk uyari sureyi asiyor", T, n, W);
    for (int k = 1; k < n; k++) {
        if (p.off_s[k] >= p.off_s[k - 1]) v("azalan degil", T, n, W);
        if (p.off_s[k - 1] - p.off_s[k] < warn_floor_s(T)) v("aralik < taban", T, n, W);
    }
    if (p.off_s[n - 1] < warn_floor_s(T)) v("son uyari < taban", T, n, W);
    /* gecis sinirlari: tam esikte k+1 gecilmis, 1 sn once k */
    for (int k = 0; k < n; k++) {
        if (warn_passed(&p, p.off_s[k]) != k + 1) v("warn_passed esikte yanlis", T, n, W);
        if (warn_passed(&p, p.off_s[k] + 1) != k) v("warn_passed esik ustu yanlis", T, n, W);
    }
}

int main(void)
{
    /* 1) otomatik W: her birim × 1..60 × 1..10 */
    printf("== Otomatik ilk uyari: en cok sigan uyari sayisi ==\n");
    for (int u = 0; u < 3; u++) {
        int no_warn = 0, partial = 0;
        char pl[400] = "";
        for (uint32_t val = 1; val <= 60; val++) {
            uint32_t T = val * unit_s[u];
            uint8_t mx = warn_max_count(T, 0);
            for (int n = 1; n <= 10; n++) check(T, n, 0);
            if (mx == 0) no_warn++;
            else if (mx < 10) { partial++; char b[24]; snprintf(b, sizeof b, " %u%s:%u", val, unit_name[u], mx); strncat(pl, b, sizeof pl - strlen(pl) - 1); }
            /* monotonluk: n gecerliyse n-1 de gecerli olmali */
            for (int n = 2; n <= 10; n++) {
                warn_plan_t a, b;
                if (warn_plan(T, (uint8_t)n, 0, &a) == WARN_OK && warn_plan(T, (uint8_t)(n - 1), 0, &b) != WARN_OK)
                    v("monotonluk: n gecerli, n-1 degil", T, n, 0);
            }
            if (mx == 0) printf("  HIC UYARI SIGMIYOR: %u %s\n", val, unit_name[u]);
        }
        printf("  %s: uyarisiz=%d, 10'dan az sigan=%d ->%s\n", unit_name[u], no_warn, partial, pl);
    }
    /* 2) elle W */
    for (int u = 0; u < 3; u++)
        for (uint32_t val = 1; val <= 60; val++) {
            uint32_t T = val * unit_s[u];
            uint32_t Ws[] = { 59, 60, 61, 300, 3600, 86400, T / 4, T / 2, T - 61, T - 60, T - 1, T, T + 1 };
            for (unsigned i = 0; i < sizeof Ws / sizeof Ws[0]; i++)
                if (Ws[i]) for (int n = 1; n <= 10; n++) check(T, n, Ws[i]);
        }
    /* 3) ornekler */
    printf("== Ornekler ==\n");
    struct { uint32_t T; int n; const char *ad; } ex[] = {
        { 2 * 86400, 3, "2 gun, 3" }, { 7 * 86400, 4, "7 gun, 4" }, { 2 * 86400, 10, "2 gun, 10" }, { 600, 10, "10 dk, 10" }, { 86400, 3, "24 sa, 3" },
        { 600, 3, "10 dk, 3" }, { 60 * 86400, 10, "60 gun, 10" }, { 120, 1, "2 dk, 1" },
        { 60, 1, "1 dk, 1" }, { 3600, 10, "60 dk, 10" }, { 2 * 3600, 3, "2 sa, 3" }, { 20 * 86400, 4, "20 gun, 4" }, { 60 * 86400, 4, "60 gun, 4" }, { 86400, 10, "24 sa, 10" } };
    for (unsigned i = 0; i < sizeof ex / sizeof ex[0]; i++) {
        warn_plan_t p;
        warn_err_t e = warn_plan(ex[i].T, (uint8_t)ex[i].n, 0, &p);
        printf("  %-11s: ", ex[i].ad);
        if (e) printf("RED (hata %d, en cok %u)\n", e, warn_max_count(ex[i].T, 0));
        else { for (int k = 0; k < p.n; k++) printf("%s%u", k ? " · " : "", p.off_s[k]); printf(" sn kala\n"); }
    }
    printf("== Ozet: gecerli plan=%ld, reddedilen=%ld, KURAL IHLALI=%ld ==\n", valid, rejected, viol);
    for (int i = 0; i < nfv; i++) printf("  ihlal: %s\n", first_viol[i]);
    return viol ? 1 : 0;
}
