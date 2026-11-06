#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "semver.h"

#define PARTS 4                               /* LebensSpur: 1.0.0; 4. parça ara/deneme yayınları için (1.0.0.1) */
typedef struct { long n[PARTS]; const char *pre; size_t pre_len; bool ok; } sv_t;

static sv_t parse(const char *s)
{
    sv_t v = { { 0 }, NULL, 0, false };
    if (!s) return v;
    if (*s == 'v' || *s == 'V') s++;
    for (int i = 0; i < PARTS; i++) {
        if (!isdigit((unsigned char)*s)) return v;
        char *end;
        v.n[i] = strtol(s, &end, 10);
        s = end;
        if (*s != '.') break;
        s++;
    }
    if (*s == '-') {
        v.pre = ++s;
        while (*s && *s != '+') s++;
        v.pre_len = (size_t)(s - v.pre);
        if (!v.pre_len) return v;
    }
    if (*s == '+') while (*s) s++;
    v.ok = *s == 0;
    return v;
}

bool semver_valid(const char *v) { return parse(v).ok; }

/* ön sürüm tanımlayıcıları: sayısal < metin, sayısallar sayı olarak, metinler ASCII */
static int cmp_pre(const char *a, size_t la, const char *b, size_t lb)
{
    size_t ia = 0, ib = 0;
    while (ia < la && ib < lb) {
        size_t ea = ia, eb = ib;
        while (ea < la && a[ea] != '.') ea++;
        while (eb < lb && b[eb] != '.') eb++;
        bool na = true, nb = true;
        for (size_t k = ia; k < ea; k++) if (!isdigit((unsigned char)a[k])) na = false;
        for (size_t k = ib; k < eb; k++) if (!isdigit((unsigned char)b[k])) nb = false;
        int c;
        if (na && nb) {
            long x = strtol(a + ia, NULL, 10), y = strtol(b + ib, NULL, 10);
            c = x < y ? -1 : x > y;
        } else if (na != nb) {
            c = na ? -1 : 1;
        } else {
            size_t n = ea - ia < eb - ib ? ea - ia : eb - ib;
            c = strncmp(a + ia, b + ib, n);
            if (!c) c = (ea - ia) < (eb - ib) ? -1 : (ea - ia) > (eb - ib);
            c = c < 0 ? -1 : c > 0;
        }
        if (c) return c;
        ia = ea + 1;
        ib = eb + 1;
    }
    bool ra = ia < la, rb = ib < lb;
    return ra == rb ? 0 : ra ? 1 : -1;
}

int semver_cmp(const char *a, const char *b)
{
    sv_t x = parse(a), y = parse(b);
    if (!x.ok || !y.ok) return x.ok == y.ok ? 0 : x.ok ? 1 : -1;
    for (int i = 0; i < PARTS; i++) {
        if (x.n[i] != y.n[i]) return x.n[i] < y.n[i] ? -1 : 1;
    }
    if (!x.pre && !y.pre) return 0;
    if (!x.pre) return 1;                      /* 1.0.0 > 1.0.0-rc */
    if (!y.pre) return -1;
    return cmp_pre(x.pre, x.pre_len, y.pre, y.pre_len);
}
