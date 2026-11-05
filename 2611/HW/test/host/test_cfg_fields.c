/* cfg_fields.c: varsayılanlar, JSON doğrulama/uygulama, dışa aktarma. */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "cJSON.h"
#include "cfg.h"

static cfg_t c;
static char err[96];
static const char *field;
static char field_buf[32];

void setUp(void)
{
    memset(&c, 0xAA, sizeof(c));
    for (int s = 0; s < CFG_SEC__COUNT; s++) {
        cfg_fields_defaults(&cfg_sections[s], (char *)&c + cfg_sections[s].offset);
    }
    err[0] = 0;
    field = NULL;
}
void tearDown(void) {}

static esp_err_t apply(cfg_section_t s, const char *json)
{
    cJSON *o = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(o);
    esp_err_t r = cfg_fields_apply_json(&cfg_sections[s], (char *)&c + cfg_sections[s].offset, o, false,
                                        err, sizeof(err), &field);
    snprintf(field_buf, sizeof field_buf, "%s", field ? field : "");   /* alan adi JSON icini gosterir */
    field = field_buf;
    cJSON_Delete(o);
    return r;
}

static void test_defaults(void)
{
    TEST_ASSERT_EQUAL_UINT16(465, c.smtp.port);
    TEST_ASSERT_TRUE(c.smtp.tls_verify);
    TEST_ASSERT_EQUAL_STRING("", c.smtp.server);
    TEST_ASSERT_EQUAL_UINT8(66, c.wifi_tx.max_q);
    TEST_ASSERT_EQUAL_UINT8(2, c.timer.unit);          /* "day" */
    TEST_ASSERT_EQUAL_UINT8(7, c.timer.value);
    TEST_ASSERT_TRUE(c.actions.trig_relay);
    TEST_ASSERT_FALSE(c.actions.trig_mail);
    TEST_ASSERT_TRUE(c.device.ap_enabled);
}

static void test_apply_valid(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, apply(CFG_SEC_SMTP, "{\"server\":\"smtp.gmail.com\",\"port\":587,\"tls_verify\":false}"));
    TEST_ASSERT_EQUAL_STRING("smtp.gmail.com", c.smtp.server);
    TEST_ASSERT_EQUAL_UINT16(587, c.smtp.port);
    TEST_ASSERT_FALSE(c.smtp.tls_verify);
    TEST_ASSERT_EQUAL(ESP_OK, apply(CFG_SEC_TIMER, "{\"unit\":\"hour\",\"value\":12}"));
    TEST_ASSERT_EQUAL_UINT8(1, c.timer.unit);
    TEST_ASSERT_EQUAL_UINT8(12, c.timer.value);
}

static void test_apply_is_atomic_on_error(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, apply(CFG_SEC_SMTP, "{\"server\":\"a.b\",\"port\":25}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, apply(CFG_SEC_SMTP, "{\"server\":\"x.y\",\"port\":70000}"));
    TEST_ASSERT_EQUAL_STRING("port", field);
    TEST_ASSERT_EQUAL_STRING("a.b", c.smtp.server);   /* değişmedi */
    TEST_ASSERT_EQUAL_UINT16(25, c.smtp.port);
}

static void test_rejects_bad_input(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, apply(CFG_SEC_SMTP, "{\"nope\":1}"));
    TEST_ASSERT_EQUAL_STRING("nope", field);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, apply(CFG_SEC_SMTP, "{\"port\":\"25\"}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, apply(CFG_SEC_SMTP, "{\"port\":25.5}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, apply(CFG_SEC_SMTP, "{\"tls_verify\":1}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, apply(CFG_SEC_TIMER, "{\"unit\":\"week\"}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, apply(CFG_SEC_TIMER, "{\"value\":0}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, apply(CFG_SEC_AUTH, "{\"hash\":\"x\"}"));     /* internal */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, apply(CFG_SEC_EARLY_MAIL, "{\"subject\":\"a\\r\\nBcc: x\"}"));
    char big[80]; memset(big, 'a', sizeof big); big[79] = 0;
    char js[128]; snprintf(js, sizeof js, "{\"server\":\"%s\"}", big);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, apply(CFG_SEC_SMTP, js));                  /* 79 > 63 */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, apply(CFG_SEC_SMTP, "[1]"));
}

static void test_multiline_allowed_in_long_fields(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, apply(CFG_SEC_EARLY_MAIL, "{\"body\":\"satır 1\\nsatır 2\\tsekme\"}"));
    TEST_ASSERT_EQUAL_STRING("satır 1\nsatır 2\tsekme", c.early_mail.body);
}

static void test_secret_kept_when_empty_and_masked(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, apply(CFG_SEC_SMTP, "{\"pass\":\"geheim\"}"));
    TEST_ASSERT_EQUAL(ESP_OK, apply(CFG_SEC_SMTP, "{\"pass\":\"\",\"user\":\"u\"}"));
    TEST_ASSERT_EQUAL_STRING("geheim", c.smtp.pass);
    TEST_ASSERT_EQUAL(ESP_OK, apply(CFG_SEC_SMTP, "{\"pass\":\"***\"}"));             /* maske geri gelirse koru */
    TEST_ASSERT_EQUAL_STRING("geheim", c.smtp.pass);
    TEST_ASSERT_EQUAL(ESP_OK, apply(CFG_SEC_SMTP, "{\"pass\":null}"));               /* null = açıkça temizle */
    TEST_ASSERT_EQUAL_STRING("", c.smtp.pass);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, apply(CFG_SEC_SMTP, "{\"user\":null}"));           /* gizli olmayan alanda null yok */
    TEST_ASSERT_EQUAL(ESP_OK, apply(CFG_SEC_SMTP, "{\"pass\":\"geheim\"}"));

    cJSON *o = cfg_fields_to_json(&cfg_sections[CFG_SEC_SMTP], &c.smtp, CFG_EXPORT_API);
    TEST_ASSERT_EQUAL_STRING("***", cJSON_GetObjectItem(o, "pass")->valuestring);
    TEST_ASSERT_EQUAL_INT(465, cJSON_GetObjectItem(o, "port")->valueint);
    cJSON_Delete(o);

    o = cfg_fields_to_json(&cfg_sections[CFG_SEC_AUTH], &c.auth, CFG_EXPORT_API);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(o, "hash"));
    cJSON_Delete(o);
    o = cfg_fields_to_json(&cfg_sections[CFG_SEC_AUTH], &c.auth, CFG_EXPORT_BACKUP);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(o, "hash"));
    cJSON_Delete(o);
}

static void test_roundtrip_backup(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, apply(CFG_SEC_TIMER, "{\"unit\":\"min\",\"value\":5,\"warn_count\":2}"));
    cJSON *o = cfg_fields_to_json(&cfg_sections[CFG_SEC_TIMER], &c.timer, CFG_EXPORT_BACKUP);
    TEST_ASSERT_EQUAL_STRING("min", cJSON_GetObjectItem(o, "unit")->valuestring);
    cfg_timer_t t2;
    cfg_fields_defaults(&cfg_sections[CFG_SEC_TIMER], &t2);
    TEST_ASSERT_EQUAL(ESP_OK, cfg_fields_apply_json(&cfg_sections[CFG_SEC_TIMER], &t2, o, true, err, sizeof err, &field));
    TEST_ASSERT_EQUAL_MEMORY(&c.timer, &t2, sizeof t2);
    cJSON_Delete(o);
}

static void test_section_lookup(void)
{
    TEST_ASSERT_EQUAL_PTR(&cfg_sections[CFG_SEC_MQTT], cfg_section_by_name("mqtt"));
    TEST_ASSERT_NULL(cfg_section_by_name("grp0"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_apply_valid);
    RUN_TEST(test_apply_is_atomic_on_error);
    RUN_TEST(test_rejects_bad_input);
    RUN_TEST(test_multiline_allowed_in_long_fields);
    RUN_TEST(test_secret_kept_when_empty_and_masked);
    RUN_TEST(test_roundtrip_backup);
    RUN_TEST(test_section_lookup);
    return UNITY_END();
}
