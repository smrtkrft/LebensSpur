#include "unity.h"
#include "outbox.h"

#define S   1000000LL
#define MIN (60 * S)
static outbox_t o;
static const ob_channels_t all = { true, true, true, true };

void setUp(void) { outbox_init(&o); }
void tearDown(void) {}

static void test_trigger_plan_and_priority(void)
{
    outbox_plan_test(&o, CH_TELEGRAM, 9, 0);
    outbox_plan_early(&o, &all, 2, 0);
    TEST_ASSERT_EQUAL_INT(4, outbox_plan_trigger(&o, &all, 0x0005, 0));   /* 2 grup maili + webhook + mqtt */
    TEST_ASSERT_NULL(outbox_take_next(&o, 0, false));                     /* ağ yok */
    ob_job_t *j = outbox_take_next(&o, 0, true);
    TEST_ASSERT_EQUAL(JK_TRIGGER, j->kind);
    TEST_ASSERT_EQUAL(CH_SMTP, j->ch);
    TEST_ASSERT_EQUAL_UINT8(0, j->group);
    TEST_ASSERT_EQUAL(JS_RUNNING, j->state);
    ob_summary_t s = outbox_trigger_summary(&o);
    TEST_ASSERT_EQUAL_UINT8(4, s.pending);
}

static void test_no_telegram_on_trigger(void)
{
    ob_channels_t tg_only = { false, true, false, false };
    TEST_ASSERT_EQUAL_INT(0, outbox_plan_trigger(&o, &tg_only, 0x3FF, 0));
}

static void test_retry_schedule_and_giveup(void)
{
    ob_channels_t mail = { true, false, false, false };
    outbox_plan_trigger(&o, &mail, 0x1, 0);
    int64_t now = 0;
    const int64_t expect[] = { 1 * MIN, 5 * MIN, 15 * MIN, 30 * MIN, 30 * MIN, 30 * MIN };
    uint16_t seq = 0;
    for (int k = 0; k < 6; k++) {
        ob_job_t *j = outbox_take_next(&o, now, true);
        TEST_ASSERT_NOT_NULL(j);
        seq = j->seq;
        outbox_on_result(&o, seq, CR_RETRY, now, 0);                /* rnd 0 → aralığın alt ucu (−%10) */
        const ob_job_t *jj = outbox_find(&o, seq);
        TEST_ASSERT_EQUAL(JS_PENDING, jj->state);
        TEST_ASSERT_EQUAL_UINT8(k + 1, jj->attempt);
        TEST_ASSERT_EQUAL_INT64(now + expect[k] - expect[k] / 10, jj->not_before_us);
        TEST_ASSERT_NULL(outbox_take_next(&o, jj->not_before_us - 1, true));   /* vakti gelmeden alınmaz */
        now = jj->not_before_us;
    }
    /* 72 saat (açık kalınan süre) dolunca vazgeçilir */
    now = 72LL * 3600 * S;
    ob_job_t *j = outbox_take_next(&o, now, true);
    outbox_on_result(&o, j->seq, CR_RETRY, now, 0);
    TEST_ASSERT_EQUAL(JS_FAILED, outbox_find(&o, seq)->state);
    /* jitter aralığı: ±%10 */
    TEST_ASSERT_EQUAL_INT64(30 * MIN + 3 * MIN, outbox_retry_delay_us(JK_TRIGGER, 9, 6 * (uint32_t)MIN));
}

static void test_early_three_attempts_and_replace(void)
{
    ob_channels_t m = { true, false, false, false };
    outbox_plan_early(&o, &m, 0, 0);
    ob_job_t *j = outbox_take_next(&o, 0, true);
    uint16_t seq = j->seq;
    outbox_on_result(&o, seq, CR_RETRY, 0, 0);
    TEST_ASSERT_EQUAL(JS_PENDING, outbox_find(&o, seq)->state);
    outbox_plan_early(&o, &m, 1, 10 * S);                     /* yeni uyarı eskisini iptal eder */
    TEST_ASSERT_EQUAL(JS_CANCELLED, outbox_find(&o, seq)->state);
    j = outbox_take_next(&o, 10 * S, true);
    TEST_ASSERT_EQUAL_UINT8(1, j->warn_idx);
    seq = j->seq;
    outbox_on_result(&o, seq, CR_RETRY, 10 * S, 0);
    j = outbox_take_next(&o, 20 * MIN, true);
    outbox_on_result(&o, seq, CR_RETRY, 20 * MIN, 0);
    j = outbox_take_next(&o, 40 * MIN, true);
    outbox_on_result(&o, seq, CR_RETRY, 40 * MIN, 0);         /* 3. başarısızlık */
    TEST_ASSERT_EQUAL(JS_FAILED, outbox_find(&o, seq)->state);
}

static void test_config_error_waits_without_attempt(void)
{
    outbox_plan_trigger(&o, &all, 0, 0);                      /* webhook + mqtt */
    ob_job_t *j = outbox_take_next(&o, 0, true);
    uint16_t seq = j->seq;
    outbox_on_result(&o, seq, CR_CONFIG, 0, 0);
    const ob_job_t *jj = outbox_find(&o, seq);
    TEST_ASSERT_EQUAL(JS_PENDING, jj->state);
    TEST_ASSERT_EQUAL_UINT8(0, jj->attempt);
    TEST_ASSERT_TRUE(jj->not_before_us >= 30 * MIN);
    outbox_cfg_changed(&o, 5 * S);
    TEST_ASSERT_EQUAL_INT64(5 * S, outbox_find(&o, seq)->not_before_us);
    TEST_ASSERT_EQUAL_INT(-1, (int)outbox_retry_delay_us(JK_TEST, 1, 0));
}

static void test_checkin_cancels_unstarted_trigger(void)
{
    outbox_plan_trigger(&o, &all, 0x3, 0);
    ob_job_t *j = outbox_take_next(&o, 0, true);              /* biri gönderiliyor */
    TEST_ASSERT_EQUAL_INT(3, outbox_cancel_pending_trigger(&o));
    TEST_ASSERT_EQUAL(JS_RUNNING, j->state);                 /* süren tamamlanır */
    outbox_on_result(&o, j->seq, CR_OK, 1 * S, 0);
    ob_summary_t s = outbox_trigger_summary(&o);
    TEST_ASSERT_EQUAL_UINT8(1, s.done);
    TEST_ASSERT_EQUAL_UINT8(3, s.cancelled);
}

static void test_persistence_roundtrip(void)
{
    outbox_plan_early(&o, &all, 4, 0);
    outbox_plan_trigger(&o, &all, 0x204, 100 * S);            /* grup 2 ve 9 */
    ob_job_t *j = outbox_take_next(&o, 100 * S, true);        /* grup 2 */
    outbox_on_result(&o, j->seq, CR_RETRY, 100 * S, 0);
    j = outbox_take_next(&o, 100 * S, true);                  /* grup 9 */
    outbox_on_result(&o, j->seq, CR_OK, 100 * S, 0);
    outbox_rt_t rt;
    outbox_to_rt(&o, 3700 * S, &rt);
    TEST_ASSERT_EQUAL_HEX16(0x004, rt.mail_mask);
    TEST_ASSERT_EQUAL_UINT8(1, rt.attempts[2]);
    TEST_ASSERT_EQUAL_HEX8(0x3, rt.other_mask);
    TEST_ASSERT_EQUAL_UINT8(4, rt.early_idx);
    TEST_ASSERT_EQUAL_UINT32(3600, rt.since_trigger_s);
    outbox_t p;
    outbox_from_rt(&p, &rt, 50 * S);                          /* yeniden açılış */
    TEST_ASSERT_TRUE(p.trigger_active);
    TEST_ASSERT_EQUAL_INT64(50 * S - 3600 * S, p.trigger_at_us);
    ob_summary_t s = outbox_trigger_summary(&p);
    TEST_ASSERT_EQUAL_UINT8(3, s.pending);                    /* grup 2 maili + webhook + mqtt */
    j = outbox_take_next(&p, 50 * S, true);
    TEST_ASSERT_EQUAL(CH_SMTP, j->ch);
    TEST_ASSERT_EQUAL_UINT8(2, j->group);
    TEST_ASSERT_EQUAL_UINT8(1, j->attempt);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_trigger_plan_and_priority);
    RUN_TEST(test_no_telegram_on_trigger);
    RUN_TEST(test_retry_schedule_and_giveup);
    RUN_TEST(test_early_three_attempts_and_replace);
    RUN_TEST(test_config_error_waits_without_attempt);
    RUN_TEST(test_checkin_cancels_unstarted_trigger);
    RUN_TEST(test_persistence_roundtrip);
    return UNITY_END();
}
