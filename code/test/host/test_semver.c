#include "unity.h"
#include "semver.h"

void setUp(void) {}
void tearDown(void) {}

static void test_order(void)
{
    /* SemVer §11 örnek zinciri + v öneki + eksik parça + yapı eki */
    TEST_ASSERT_EQUAL(-1, semver_cmp("1.0.0", "1.0.1"));
    TEST_ASSERT_EQUAL(1, semver_cmp("1.1.0", "1.0.9"));
    TEST_ASSERT_EQUAL(1, semver_cmp("2.0.0", "1.99.99"));
    TEST_ASSERT_EQUAL(1, semver_cmp("1.0.10", "1.0.9"));          /* sayısal, metin değil */
    TEST_ASSERT_EQUAL(0, semver_cmp("v1.2.3", "1.2.3"));
    TEST_ASSERT_EQUAL(0, semver_cmp("1.2", "1.2.0"));
    TEST_ASSERT_EQUAL(0, semver_cmp("1.0.0+build5", "1.0.0"));
    TEST_ASSERT_EQUAL(-1, semver_cmp("1.0.0-alpha", "1.0.0-alpha.1"));
    TEST_ASSERT_EQUAL(-1, semver_cmp("1.0.0-alpha.1", "1.0.0-alpha.beta"));
    TEST_ASSERT_EQUAL(-1, semver_cmp("1.0.0-alpha.beta", "1.0.0-beta"));
    TEST_ASSERT_EQUAL(-1, semver_cmp("1.0.0-beta", "1.0.0-beta.2"));
    TEST_ASSERT_EQUAL(-1, semver_cmp("1.0.0-beta.2", "1.0.0-beta.11"));
    TEST_ASSERT_EQUAL(-1, semver_cmp("1.0.0-beta.11", "1.0.0-rc.1"));
    TEST_ASSERT_EQUAL(-1, semver_cmp("1.0.0-rc.1", "1.0.0"));
    TEST_ASSERT_EQUAL(1, semver_cmp("1.0.0", "1.0.0-rc.1"));
    TEST_ASSERT_EQUAL(0, semver_cmp("1.0.0-rc.1", "1.0.0-rc.1"));
}

static void test_four_parts(void)
{
    TEST_ASSERT_EQUAL(1, semver_cmp("1.0.1.1", "1.0.1"));
    TEST_ASSERT_EQUAL(1, semver_cmp("1.0.1.1", "1.0.0"));
    TEST_ASSERT_EQUAL(1, semver_cmp("v1.0.1.1", "1.0.1-test"));
    TEST_ASSERT_EQUAL(-1, semver_cmp("1.0.1.1", "1.0.1.2"));
    TEST_ASSERT_EQUAL(1, semver_cmp("1.0.1.10", "1.0.1.9"));
    TEST_ASSERT_EQUAL(1, semver_cmp("1.0.2", "1.0.1.99"));
    TEST_ASSERT_EQUAL(0, semver_cmp("1.0.1.0", "1.0.1"));
    TEST_ASSERT_EQUAL(-1, semver_cmp("1.0.1.1-rc.1", "1.0.1.1"));
    TEST_ASSERT_FALSE(semver_valid("1.0.1.1.1"));                /* 5 parça yok */
}

static void test_invalid(void)
{
    TEST_ASSERT_FALSE(semver_valid(""));
    TEST_ASSERT_FALSE(semver_valid("abc"));
    TEST_ASSERT_FALSE(semver_valid("1.0.0 x"));
    TEST_ASSERT_FALSE(semver_valid("1.0.0-"));
    TEST_ASSERT_FALSE(semver_valid(NULL));
    TEST_ASSERT_TRUE(semver_valid("v0.0.70"));
    TEST_ASSERT_EQUAL(1, semver_cmp("0.0.1", "bozuk"));            /* geçersiz her zaman küçük */
    TEST_ASSERT_EQUAL(-1, semver_cmp("DMF_v1.0.6", "1.0.0"));      /* başka ürünün tag'i güncelleme sayılmaz */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_order);
    RUN_TEST(test_four_parts);
    RUN_TEST(test_invalid);
    return UNITY_END();
}
