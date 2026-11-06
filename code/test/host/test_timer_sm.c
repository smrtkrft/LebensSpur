#include "unity.h"
#include "timer_sm.h"

#define S  1000000LL
#define H  3600u
#define D  86400u
static timer_sm_t t;
static uint8_t wi;

void setUp(void) { timer_sm_init(&t, NULL, 3, 0, 0); wi = 0xFF; }
void tearDown(void) {}

static void test_idle_and_set(void)
{
    TEST_ASSERT_EQUAL(TS_IDLE, t.state);
    TEST_ASSERT_EQUAL_INT(-1, timer_sm_checkin(&t, 0));
    TEST_ASSERT_EQUAL(WARN_OK, timer_sm_set(&t, D, 3, 0, 0));
    TEST_ASSERT_EQUAL(TS_RUNNING, t.state);
    TEST_ASSERT_EQUAL_UINT32(D, timer_sm_remaining_s(&t, 0));
    TEST_ASSERT_EQUAL_UINT32(D - 10, timer_sm_remaining_s(&t, 10 * S));
    TEST_ASSERT_EQUAL_UINT32(D - 9, timer_sm_remaining_s(&t, 10 * S - 1));    /* yukarı yuvarlama: 0 yalnız bitince */
}

static void test_invalid_plan_leaves_state(void)
{
    timer_sm_set(&t, D, 3, 0, 0);
    TEST_ASSERT_EQUAL(WARN_ERR_GAP, timer_sm_set(&t, 600, 10, 0, 5 * S));
    TEST_ASSERT_EQUAL_UINT32(D, t.total_s);
    TEST_ASSERT_EQUAL_UINT32(D - 5, timer_sm_remaining_s(&t, 5 * S));
}

static void test_warnings_then_trigger(void)
{
    timer_sm_set(&t, D, 3, 0, 0);                      /* uyarılar 6 sa · 2,4 sa · 1 sa kala */
    TEST_ASSERT_EQUAL(TA_NONE, timer_sm_eval(&t, (int64_t)(D - 6 * H - 1) * S, &wi));
    TEST_ASSERT_EQUAL(TA_WARN | TA_PERSIST, timer_sm_eval(&t, (int64_t)(D - 6 * H) * S, &wi));
    TEST_ASSERT_EQUAL_UINT8(0, wi);
    TEST_ASSERT_EQUAL(TA_NONE, timer_sm_eval(&t, (int64_t)(D - 6 * H + 1) * S, &wi));
    TEST_ASSERT_EQUAL(TA_WARN | TA_PERSIST, timer_sm_eval(&t, (int64_t)(D - H) * S, &wi));   /* 2. atlandı */
    TEST_ASSERT_EQUAL_UINT8(2, wi);
    TEST_ASSERT_EQUAL(TA_TRIGGER | TA_PERSIST, timer_sm_eval(&t, (int64_t)D * S, &wi));
    TEST_ASSERT_EQUAL(TS_TRIGGERED, t.state);
    TEST_ASSERT_EQUAL(TA_NONE, timer_sm_eval(&t, (int64_t)(D + 5) * S, &wi));
}

static void test_checkin_restarts_from_any_active_state(void)
{
    timer_sm_set(&t, H, 1, 0, 0);
    timer_sm_eval(&t, (int64_t)H * S, &wi);
    TEST_ASSERT_EQUAL(TS_TRIGGERED, t.state);
    TEST_ASSERT_EQUAL_INT(0, timer_sm_checkin(&t, 100 * S));
    TEST_ASSERT_EQUAL(TS_RUNNING, t.state);
    TEST_ASSERT_EQUAL_UINT8(0, t.warn_fired);
    TEST_ASSERT_EQUAL_UINT32(H, timer_sm_remaining_s(&t, 100 * S));
    TEST_ASSERT_EQUAL_INT(0, timer_sm_cancel(&t, 200 * S));
    TEST_ASSERT_EQUAL(TS_STOPPED, t.state);
    TEST_ASSERT_EQUAL_UINT32(H - 100, timer_sm_remaining_s(&t, 9999 * S));   /* durdu, akmıyor */
    TEST_ASSERT_EQUAL_INT(0, timer_sm_checkin(&t, 300 * S));
    TEST_ASSERT_EQUAL_UINT32(H, timer_sm_remaining_s(&t, 300 * S));
}

static void test_vacation(void)
{
    timer_sm_set(&t, 7 * D, 3, 0, 0);
    TEST_ASSERT_EQUAL_INT(0, timer_sm_vacation(&t, 2, 10 * S));
    TEST_ASSERT_EQUAL_INT(1, timer_sm_checkin(&t, 20 * S));                 /* tatilde etkisiz */
    TEST_ASSERT_EQUAL(TA_NONE, timer_sm_eval(&t, (int64_t)(2 * D) * S, &wi));
    TEST_ASSERT_EQUAL_UINT32(7 * D, timer_sm_remaining_s(&t, 50 * S));
    TEST_ASSERT_EQUAL(TA_VACATION_END | TA_PERSIST, timer_sm_eval(&t, (int64_t)(2 * D + 10) * S, &wi));
    TEST_ASSERT_EQUAL(TS_RUNNING, t.state);
    TEST_ASSERT_EQUAL_UINT32(0, t.vac_total_s);                              /* tatil bitince toplam sıfır */
    TEST_ASSERT_EQUAL_UINT32(7 * D, timer_sm_remaining_s(&t, (int64_t)(2 * D + 10) * S));
    TEST_ASSERT_EQUAL_INT(0, timer_sm_vacation(&t, 1, 0));
    TEST_ASSERT_EQUAL_UINT32(1 * D, t.vac_total_s);
    TEST_ASSERT_EQUAL_INT(0, timer_sm_vacation_cancel(&t, 5 * S));          /* iptal: tam süreden */
    TEST_ASSERT_EQUAL_UINT32(0, t.vac_total_s);
    TEST_ASSERT_EQUAL_UINT32(7 * D, timer_sm_remaining_s(&t, 5 * S));
    TEST_ASSERT_EQUAL_INT(-1, timer_sm_vacation(&t, 61, 0));
}

static void test_persistence_power_off_not_counted(void)
{
    timer_sm_set(&t, D, 3, 0, 0);
    timer_sm_eval(&t, (int64_t)(D - 6 * H) * S, &wi);                       /* 1. uyarı ateşlendi */
    timer_rt_t rt;
    timer_sm_to_rt(&t, (int64_t)(D - 5 * H) * S, &rt);                      /* 5 sa kala kaydedildi */
    TEST_ASSERT_EQUAL_UINT32(5 * H, rt.remaining_s);
    timer_sm_t u;
    timer_sm_init(&u, &rt, 3, 0, 123 * S);                                  /* cihaz kapalıyken zaman geçti */
    TEST_ASSERT_EQUAL(TS_RUNNING, u.state);
    TEST_ASSERT_EQUAL_UINT32(5 * H, timer_sm_remaining_s(&u, 123 * S));     /* kapalı süre sayılmadı */
    TEST_ASSERT_EQUAL_UINT8(1, u.warn_fired);                               /* ateşlenen tekrar etmez */
    TEST_ASSERT_EQUAL(TA_NONE, timer_sm_eval(&u, 124 * S, &wi));
}

static void test_change_warn_while_running(void)
{
    timer_sm_set(&t, D, 2, 0, 0);
    TEST_ASSERT_EQUAL(WARN_OK, timer_sm_set_warn(&t, 5, 0, (int64_t)(D - 2 * H) * S));
    TEST_ASSERT_TRUE(t.warn_fired >= 3);                                    /* geçilmiş eşikler sayıldı */
    TEST_ASSERT_EQUAL(WARN_ERR_GAP, timer_sm_set_warn(&t, 10, H + 8 * 60, 0)); /* 9 aralık × 60 sn sığmaz */
    TEST_ASSERT_EQUAL(WARN_ERR_FIRST, timer_sm_set_warn(&t, 3, H, 0));         /* ilk = son uyarı */
    TEST_ASSERT_EQUAL_UINT8(5, t.warn_n);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_idle_and_set);
    RUN_TEST(test_invalid_plan_leaves_state);
    RUN_TEST(test_warnings_then_trigger);
    RUN_TEST(test_checkin_restarts_from_any_active_state);
    RUN_TEST(test_vacation);
    RUN_TEST(test_persistence_power_off_not_counted);
    RUN_TEST(test_change_warn_while_running);
    return UNITY_END();
}
