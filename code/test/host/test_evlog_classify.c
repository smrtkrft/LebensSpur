#include <string.h>
#include "unity.h"
#include "evlog_classify.h"

void setUp(void) {}
void tearDown(void) {}

#define C(rr, ok, rst, m, age) evlog_classify(rr, ok, rst, m, age)

static void test_power(void)
{
    evlog_cause_t c = C(EVR_BROWNOUT, true, RST_NONE, EVM_MAIL_SEND, 3);
    TEST_ASSERT_EQUAL_STRING("power", c.cat);
    TEST_ASSERT_EQUAL_STRING("brownout", c.cause);
    TEST_ASSERT_EQUAL_STRING("brownout_load", c.hint);
    TEST_ASSERT_EQUAL_STRING("brownout_other", C(EVR_BROWNOUT, true, RST_NONE, EVM_MAIL_SEND, 31).hint);
    TEST_ASSERT_EQUAL_STRING("brownout_other", C(EVR_BROWNOUT, true, RST_NONE, EVM_TIMER_EXPIRED, 1).hint);
    TEST_ASSERT_EQUAL_STRING("brownout_other", C(EVR_BROWNOUT, false, RST_NONE, EVM_MAIL_SEND, 1).hint);
    TEST_ASSERT_EQUAL_STRING("power_loss", C(EVR_POWERON, false, RST_NONE, EVM_NONE, 0).cause);
    TEST_ASSERT_EQUAL_STRING("poweron_rst", C(EVR_POWERON, true, RST_NONE, EVM_NONE, 0).cause);
    TEST_ASSERT_EQUAL_STRING("pwr_glitch", C(EVR_PWR_GLITCH, true, RST_NONE, EVM_NONE, 0).cause);
}

static void test_software_and_user(void)
{
    TEST_ASSERT_EQUAL_STRING("panic", C(EVR_PANIC, true, RST_NONE, EVM_NONE, 0).cause);
    TEST_ASSERT_EQUAL_STRING("task_wdt", C(EVR_TASK_WDT, true, RST_NONE, EVM_NONE, 0).cause);
    TEST_ASSERT_EQUAL_STRING("software", C(EVR_INT_WDT, true, RST_NONE, EVM_NONE, 0).cat);
    evlog_cause_t c = C(EVR_SW, true, RST_USER_WEB, EVM_NONE, 0);
    TEST_ASSERT_EQUAL_STRING("user", c.cat);
    TEST_ASSERT_EQUAL_STRING("user_restart", c.cause);
    TEST_ASSERT_EQUAL_STRING("button_restart", C(EVR_SW, true, RST_USER_BUTTON, EVM_NONE, 0).cause);
    TEST_ASSERT_EQUAL_STRING("factory_reset", C(EVR_SW, true, RST_FACTORY, EVM_NONE, 0).cause);
    TEST_ASSERT_EQUAL_STRING("ota_fw", C(EVR_SW, true, RST_OTA, EVM_NONE, 0).cause);
    TEST_ASSERT_EQUAL_STRING("delivery_hang", C(EVR_SW, true, RST_DELIVERY_HANG, EVM_NONE, 0).cause);
    TEST_ASSERT_EQUAL_STRING("software", C(EVR_SW, true, RST_DELIVERY_HANG, EVM_NONE, 0).cat);
    c = C(EVR_SW, false, RST_USER_WEB, EVM_NONE, 0);       /* RTC geçersiz: neden bilinemez */
    TEST_ASSERT_EQUAL_STRING("sw_restart", c.cause);
    TEST_ASSERT_EQUAL_STRING("software", c.cat);
}

static void test_other(void)
{
    TEST_ASSERT_EQUAL_STRING("usb", C(EVR_USB, true, RST_NONE, EVM_NONE, 0).cause);
    TEST_ASSERT_EQUAL_STRING("other", C(EVR_EXT, true, RST_NONE, EVM_NONE, 0).cat);
    TEST_ASSERT_EQUAL_STRING("unknown", C(EVR_UNKNOWN, true, RST_NONE, EVM_NONE, 0).cause);
}

static void test_short_boot_rule(void)
{
    TEST_ASSERT_TRUE(evlog_short_boot_should_log("power", 1));
    TEST_ASSERT_FALSE(evlog_short_boot_should_log("power", 2));
    TEST_ASSERT_TRUE(evlog_short_boot_should_log("software", 10));
    TEST_ASSERT_TRUE(evlog_short_boot_should_log("software", 20));
    TEST_ASSERT_FALSE(evlog_short_boot_should_log("user", 1));
}

static void test_mark_names(void)
{
    TEST_ASSERT_EQUAL_STRING("MAIL_SEND", evlog_mark_name(EVM_MAIL_SEND));
    TEST_ASSERT_EQUAL_STRING("NONE", evlog_mark_name(EVM__COUNT));
    TEST_ASSERT_TRUE(evlog_mark_is_load_peak(EVM_RELAY));
    TEST_ASSERT_FALSE(evlog_mark_is_load_peak(EVM_BUTTON));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_power);
    RUN_TEST(test_software_and_user);
    RUN_TEST(test_other);
    RUN_TEST(test_short_boot_rule);
    RUN_TEST(test_mark_names);
    return UNITY_END();
}
