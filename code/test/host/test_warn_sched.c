/* Model C (F1 + L1 + geometrik) — kullanıcıyla kararlaştırılan örnekler (docs/06-alarm-tasarim.md). */
#include "unity.h"
#include "warn_sched.h"

#define H 3600u
#define D 86400u
static warn_plan_t p;

void setUp(void) {}
void tearDown(void) {}

static void test_user_examples(void)
{
    TEST_ASSERT_EQUAL(WARN_OK, warn_plan(D, 4, 0, &p));            /* 24 sa: 18. ve 23. saat */
    TEST_ASSERT_EQUAL_UINT32(6 * H, p.off_s[0]);
    TEST_ASSERT_EQUAL_UINT32(1 * H, p.off_s[3]);
    TEST_ASSERT_EQUAL(WARN_OK, warn_plan(60 * D, 4, 0, &p));       /* 60 gün: 45. gün, son gün */
    TEST_ASSERT_EQUAL_UINT32(15 * D, p.off_s[0]);
    TEST_ASSERT_EQUAL_UINT32(1 * D, p.off_s[3]);
    TEST_ASSERT_EQUAL(WARN_OK, warn_plan(2 * D, 3, 0, &p));        /* 2 gün: ilk 24 saatte değil */
    TEST_ASSERT_EQUAL_UINT32(12 * H, p.off_s[0]);
    TEST_ASSERT_EQUAL_UINT32(2 * H, p.off_s[2]);
    TEST_ASSERT_EQUAL(WARN_OK, warn_plan(48 * H, 3, 0, &p));       /* birimden bağımsız: 48 sa = 2 gün */
    TEST_ASSERT_EQUAL_UINT32(12 * H, p.off_s[0]);
}

static void test_geometric_ratio_constant(void)
{
    TEST_ASSERT_EQUAL(WARN_OK, warn_plan(60 * D, 10, 0, &p));
    double r0 = (double)p.off_s[1] / p.off_s[0];
    for (int k = 2; k < 10; k++) {
        double r = (double)p.off_s[k] / p.off_s[k - 1];
        TEST_ASSERT_FLOAT_WITHIN(0.01, r0, r);
    }
}

static void test_single_is_last_chance(void)
{
    TEST_ASSERT_EQUAL(WARN_OK, warn_plan(7 * D, 1, 0, &p));
    TEST_ASSERT_EQUAL_UINT8(1, p.n);
    TEST_ASSERT_EQUAL_UINT32(7 * H, p.off_s[0]);
    TEST_ASSERT_EQUAL(WARN_OK, warn_plan(60, 1, 0, &p));            /* 1 dk sayaç da 1 uyarı alır */
    TEST_ASSERT_EQUAL_UINT32(15, p.off_s[0]);
}

static void test_manual_first_and_errors(void)
{
    TEST_ASSERT_EQUAL(WARN_OK, warn_plan(7 * D, 3, 3 * D, &p));
    TEST_ASSERT_EQUAL_UINT32(3 * D, p.off_s[0]);
    TEST_ASSERT_EQUAL(WARN_ERR_FIRST, warn_plan(7 * D, 3, 7 * D, &p));     /* süreye eşit */
    TEST_ASSERT_EQUAL(WARN_ERR_FIRST, warn_plan(7 * D, 3, 7 * H, &p));     /* son uyarıdan önce değil */
    TEST_ASSERT_EQUAL(WARN_ERR_COUNT, warn_plan(D, 0, 0, &p));
    TEST_ASSERT_EQUAL(WARN_ERR_COUNT, warn_plan(D, 11, 0, &p));
    TEST_ASSERT_EQUAL(WARN_ERR_GAP, warn_plan(600, 10, 0, &p));
    TEST_ASSERT_EQUAL_UINT8(2, warn_max_count(600, 0));
    TEST_ASSERT_EQUAL_UINT8(10, warn_max_count(H, 0));                   /* 60 dk: 10 sığar */
    TEST_ASSERT_EQUAL(WARN_OK, warn_plan(H, 10, 0, &p));
}

static void test_passed_count(void)
{
    TEST_ASSERT_EQUAL(WARN_OK, warn_plan(D, 3, 0, &p));                  /* 6 sa · 2,4 sa · 1 sa */
    TEST_ASSERT_EQUAL_UINT8(0, warn_passed(&p, D));
    TEST_ASSERT_EQUAL_UINT8(1, warn_passed(&p, 6 * H));
    TEST_ASSERT_EQUAL_UINT8(2, warn_passed(&p, 2 * H));
    TEST_ASSERT_EQUAL_UINT8(3, warn_passed(&p, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_user_examples);
    RUN_TEST(test_geometric_ratio_constant);
    RUN_TEST(test_single_is_last_chance);
    RUN_TEST(test_manual_first_and_errors);
    RUN_TEST(test_passed_count);
    return UNITY_END();
}
