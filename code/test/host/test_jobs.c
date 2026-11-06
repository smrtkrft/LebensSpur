#include "unity.h"
#include "jobs.h"

void setUp(void) { jobs_reset(); }
void tearDown(void) {}

static void test_create_update_get(void)
{
    job_id_t a = jobs_create("ota");
    TEST_ASSERT_NOT_EQUAL(0, a);
    job_info_t j;
    TEST_ASSERT_TRUE(jobs_get(a, &j));
    TEST_ASSERT_EQUAL(JOB_QUEUED, j.state);
    TEST_ASSERT_EQUAL_INT8(-1, j.pct);
    jobs_update(a, JOB_RUNNING, 150, "indiriliyor");
    TEST_ASSERT_TRUE(jobs_get(a, &j));
    TEST_ASSERT_EQUAL_INT8(100, j.pct);
    TEST_ASSERT_EQUAL_STRING("indiriliyor", j.msg);
    TEST_ASSERT_EQUAL_STRING("running", jobs_state_name(j.state));
    TEST_ASSERT_FALSE(jobs_get(0, &j));
    TEST_ASSERT_FALSE(jobs_get(999, &j));
}

static void test_reuse_prefers_finished(void)
{
    job_id_t ids[8];
    for (int i = 0; i < 8; i++) ids[i] = jobs_create("t");
    jobs_update(ids[3], JOB_DONE, 100, NULL);
    job_id_t n = jobs_create("yeni");
    job_info_t j;
    TEST_ASSERT_FALSE(jobs_get(ids[3], &j));     /* bitmiş olan yerini verdi */
    TEST_ASSERT_TRUE(jobs_get(ids[0], &j));      /* süren en eski korunur */
    TEST_ASSERT_TRUE(jobs_get(n, &j));
    job_id_t m = jobs_create("dolu");             /* bitmiş yok: en eski (ids[0]) gider */
    TEST_ASSERT_FALSE(jobs_get(ids[0], &j));
    TEST_ASSERT_TRUE(jobs_get(m, &j));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_update_get);
    RUN_TEST(test_reuse_prefers_finished);
    return UNITY_END();
}
