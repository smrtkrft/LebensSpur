#include <stdint.h>
#include "unity.h"
#include "relay_sm.h"
#include "button_sm.h"

#define S 1000000LL
static relay_sm_t r;

void setUp(void) {}
void tearDown(void) {}

static void cfg(uint8_t d, uint8_t p, uint16_t dur) { relay_cfg_t c = { d, p, dur }; relay_sm_init(&r, &c); }

static void test_continuous_with_duration(void)
{
    cfg(0, 0, 10);
    relay_sm_trigger(&r, 0, 0);
    TEST_ASSERT_TRUE(relay_sm_eval(&r, 0));
    TEST_ASSERT_TRUE(relay_sm_eval(&r, 9 * S));
    TEST_ASSERT_EQUAL_UINT32(1, relay_sm_remaining_s(&r, 9 * S));
    TEST_ASSERT_FALSE(relay_sm_eval(&r, 10 * S));
    TEST_ASSERT_FALSE(relay_sm_active(&r));
}

static void test_unlimited_until_off(void)
{
    cfg(0, 0, 0);
    relay_sm_trigger(&r, 0, 0);
    TEST_ASSERT_TRUE(relay_sm_eval(&r, 1000000 * S));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, relay_sm_remaining_s(&r, 5 * S));
    relay_sm_off(&r);
    TEST_ASSERT_FALSE(relay_sm_eval(&r, 1000001 * S));
}

static void test_delay_then_pulse(void)
{
    cfg(3, 2, 9);                                  /* 3 sn gecikme, 2/2 darbe, 9 sn */
    relay_sm_trigger(&r, 0, 0);
    TEST_ASSERT_FALSE(relay_sm_eval(&r, 2 * S));
    TEST_ASSERT_EQUAL_UINT32(10, relay_sm_remaining_s(&r, 2 * S));
    TEST_ASSERT_TRUE(relay_sm_eval(&r, 3 * S));    /* 3–5 açık */
    TEST_ASSERT_FALSE(relay_sm_eval(&r, 5 * S));   /* 5–7 kapalı */
    TEST_ASSERT_TRUE(relay_sm_eval(&r, 7 * S));    /* 7–9 açık */
    TEST_ASSERT_FALSE(relay_sm_eval(&r, 9 * S));   /* 9–11 kapalı */
    TEST_ASSERT_TRUE(relay_sm_eval(&r, 11 * S));
    TEST_ASSERT_FALSE(relay_sm_eval(&r, 12 * S));  /* süre 3+9=12'de bitti */
    TEST_ASSERT_FALSE(relay_sm_active(&r));
}

static void test_late_eval_catches_up(void)
{
    cfg(1, 1, 0);
    relay_sm_trigger(&r, 0, 0);
    TEST_ASSERT_FALSE(relay_sm_eval(&r, (int64_t)(1 + 2 * 100 + 1) * S + 1));   /* 201,x: kapalı faz */
    TEST_ASSERT_TRUE(relay_sm_eval(&r, (int64_t)(1 + 2 * 100 + 2) * S));
}

static void test_override_and_retrigger(void)
{
    cfg(0, 0, 0);
    relay_sm_trigger(&r, 0, 5);                    /* test: 5 sn */
    TEST_ASSERT_FALSE(relay_sm_eval(&r, 5 * S));
    relay_sm_trigger(&r, 10 * S, 3);
    relay_sm_trigger(&r, 11 * S, 3);               /* yeniden tetik eskisini sıfırlar */
    TEST_ASSERT_TRUE(relay_sm_eval(&r, 13 * S));
    TEST_ASSERT_FALSE(relay_sm_eval(&r, 14 * S));
}

static void test_button(void)
{
    button_sm_t b = { 0 };
    TEST_ASSERT_EQUAL(BA_NONE, button_sm_window_end(&b));
    button_sm_press(&b, 0);
    TEST_ASSERT_EQUAL(BA_CHECKIN, button_sm_window_end(&b));
    button_sm_press(&b, 0); button_sm_press(&b, 300000);
    TEST_ASSERT_EQUAL(BA_CHECKIN, button_sm_window_end(&b));
    button_sm_press(&b, 0); button_sm_press(&b, 300000); button_sm_press(&b, 600000);
    TEST_ASSERT_EQUAL(BA_RESTART, button_sm_window_end(&b));
    TEST_ASSERT_EQUAL(BA_NONE, button_sm_press(&b, 0));
    for (int i = 1; i < 4; i++) TEST_ASSERT_EQUAL(BA_NONE, button_sm_press(&b, i * 300000));
    TEST_ASSERT_EQUAL(BA_FACTORY1, button_sm_press(&b, 1200000));
    TEST_ASSERT_EQUAL(BA_NONE, button_sm_window_end(&b));
    button_sm_press(&b, 0); button_sm_press(&b, 300000);
    button_sm_press(&b, 4000000);                  /* 3 sn aşıldı: yeni dizi */
    TEST_ASSERT_EQUAL(BA_CHECKIN, button_sm_window_end(&b));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_continuous_with_duration);
    RUN_TEST(test_unlimited_until_off);
    RUN_TEST(test_delay_then_pulse);
    RUN_TEST(test_late_eval_catches_up);
    RUN_TEST(test_override_and_retrigger);
    RUN_TEST(test_button);
    return UNITY_END();
}
