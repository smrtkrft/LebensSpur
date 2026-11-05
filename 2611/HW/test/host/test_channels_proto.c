#include <string.h>
#include "unity.h"
#include "mqtt_pkt.h"
#include "smtp_proto.h"
#include "tmpl.h"

static char out[2048];
void setUp(void) {}
void tearDown(void) {}

static void test_smtp_lines_and_classify(void)
{
    int c; bool last;
    TEST_ASSERT_EQUAL_INT(0, smtp_parse_line("250-smtp.gmail.com at your service", &c, &last));
    TEST_ASSERT_EQUAL_INT(250, c); TEST_ASSERT_FALSE(last);
    TEST_ASSERT_EQUAL_INT(0, smtp_parse_line("250 SMTPUTF8", &c, &last)); TEST_ASSERT_TRUE(last);
    TEST_ASSERT_EQUAL_INT(0, smtp_parse_line("354", &c, &last)); TEST_ASSERT_TRUE(last);
    TEST_ASSERT_EQUAL_INT(-1, smtp_parse_line("25x ok", &c, &last));
    TEST_ASSERT_EQUAL_INT(-1, smtp_parse_line("250x", &c, &last));
    TEST_ASSERT_EQUAL(CR_OK, smtp_classify(250));
    TEST_ASSERT_EQUAL(CR_OK, smtp_classify(354));
    TEST_ASSERT_EQUAL(CR_RETRY, smtp_classify(421));
    TEST_ASSERT_EQUAL(CR_RETRY, smtp_classify(451));
    TEST_ASSERT_EQUAL(CR_CONFIG, smtp_classify(535));
    TEST_ASSERT_EQUAL(CR_PERM, smtp_classify(550));
    TEST_ASSERT_EQUAL(CR_PERM, smtp_classify(554));
}

static void test_ehlo_caps(void)
{
    smtp_caps_t c = { 0 };
    smtp_ehlo_cap("SIZE 35882577", &c);
    smtp_ehlo_cap("AUTH LOGIN XOAUTH2", &c);        /* Outlook: PLAIN yok */
    smtp_ehlo_cap("STARTTLS", &c);
    smtp_ehlo_cap("8BITMIME", &c);
    TEST_ASSERT_EQUAL_UINT32(35882577, c.size_max);
    TEST_ASSERT_TRUE(c.auth_login); TEST_ASSERT_FALSE(c.auth_plain);
    TEST_ASSERT_TRUE(c.starttls); TEST_ASSERT_TRUE(c.eightbit);
    smtp_ehlo_cap("AUTH=PLAIN LOGIN", &c);
    TEST_ASSERT_TRUE(c.auth_plain);
}

static void test_body_escape(void)
{
    TEST_ASSERT_TRUE(smtp_body_escape("a\n.gizli\r\nb\n.", out, sizeof out) > 0);
    TEST_ASSERT_EQUAL_STRING("a\r\n..gizli\r\nb\r\n..", out);
    TEST_ASSERT_TRUE(smtp_body_escape(".bas", out, sizeof out) > 0);
    TEST_ASSERT_EQUAL_STRING("..bas", out);
    char small[4];
    TEST_ASSERT_EQUAL_UINT(0, smtp_body_escape("uzun metin", small, sizeof small));
}

static void test_base64_and_auth(void)
{
    b64_encode((const uint8_t *)"Man", 3, out, sizeof out); TEST_ASSERT_EQUAL_STRING("TWFu", out);
    b64_encode((const uint8_t *)"Ma", 2, out, sizeof out); TEST_ASSERT_EQUAL_STRING("TWE=", out);
    b64_encode((const uint8_t *)"M", 1, out, sizeof out); TEST_ASSERT_EQUAL_STRING("TQ==", out);
    smtp_auth_plain("u", "p", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("AHUAcA==", out);                      /* \0u\0p */
    static uint8_t raw[2964];
    for (size_t i = 0; i < sizeof raw; i++) raw[i] = (uint8_t)i;
    static char lines[4200];
    size_t n = b64_encode_lines(raw, sizeof raw, lines, sizeof lines);
    TEST_ASSERT_EQUAL_UINT(52 * 78, n);                              /* 2964 B → 52 satır × (76+CRLF) */
    TEST_ASSERT_EQUAL_CHAR('\r', lines[76]);
}

static void test_subject(void)
{
    smtp_encode_subject("Uyari 1/3", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("Uyari 1/3", out);
    smtp_encode_subject("Uyarı: süre doluyor", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("=?UTF-8?B?VXlhcsSxOiBzw7xyZSBkb2x1eW9y?=", out);
    /* uzun: kodlanmış sözcükler ≤75 kr ve UTF-8 karakter bölünmez */
    smtp_encode_subject("ğğğğğğğğğğğğğğğğğğğğğğğğğğğğğğğğğğğğğğğğ", out, sizeof out);
    TEST_ASSERT_NOT_NULL(strstr(out, "?=\r\n =?UTF-8?B?"));
    for (char *w = out; (w = strstr(w, "=?UTF-8?B?")); ) {
        char *e = strstr(w + 10, "?=");
        TEST_ASSERT_TRUE(e - w + 2 <= 75);
        TEST_ASSERT_EQUAL_INT(0, ((e - (w + 10)) % 4));               /* tam base64 blokları */
        w = e;
    }
}

static void test_mqtt(void)
{
    uint8_t b[256];
    size_t n = mqtt_build_connect(b, sizeof b, "ls-1", "u", "p", 30);
    TEST_ASSERT_EQUAL_HEX8(0x10, b[0]);
    TEST_ASSERT_EQUAL_UINT8(n - 2, b[1]);
    TEST_ASSERT_EQUAL_HEX8(0xC2, b[9]);                              /* user+pass+temiz */
    n = mqtt_build_publish(b, sizeof b, "ls/durum", (const uint8_t *)"tetik", 5, 1, false, 7);
    TEST_ASSERT_EQUAL_HEX8(0x32, b[0]);
    TEST_ASSERT_EQUAL_UINT8(2 + 8 + 2 + 5, b[1]);
    TEST_ASSERT_EQUAL_HEX8(7, b[2 + 2 + 8 + 1]);
    TEST_ASSERT_EQUAL_MEMORY("tetik", b + n - 5, 5);
    const uint8_t ack[] = { 0x20, 2, 0, 0 }, ack5[] = { 0x20, 2, 0, 5 }, pa[] = { 0x40, 2, 0, 7 };
    TEST_ASSERT_EQUAL_INT(0, mqtt_parse_connack(ack, 4));
    TEST_ASSERT_EQUAL_INT(5, mqtt_parse_connack(ack5, 4));
    TEST_ASSERT_EQUAL_INT(0, mqtt_parse_puback(pa, 4, 7));
    TEST_ASSERT_EQUAL_INT(-1, mqtt_parse_puback(pa, 4, 8));
    uint8_t big[300];
    n = mqtt_build_publish(big, sizeof big, "t", (const uint8_t *)out, 200, 0, true, 0);
    size_t hdr;
    TEST_ASSERT_EQUAL_INT(203, mqtt_remaining_len(big, n, &hdr));    /* 2 baytlık uzunluk alanı */
    TEST_ASSERT_EQUAL_UINT(3, hdr);
    TEST_ASSERT_EQUAL_HEX8(0x31, big[0]);
}

static void test_tmpl(void)
{
    tmpl_vars_t v = { "triggered", "2026-11-05T21:00:00Z", "LS-1EKK63U5BA", "2 sa", 2, 3 };
    tmpl_expand("{\"s\":\"{status}\",\"d\":\"{device}\",\"k\":{k},\"x\":\"{bilinmeyen}\"}", &v, out, sizeof out, true);
    TEST_ASSERT_EQUAL_STRING("{\"s\":\"triggered\",\"d\":\"LS-1EKK63U5BA\",\"k\":2,\"x\":\"{bilinmeyen}\"}", out);
    v.device = "a\"b\nc";
    tmpl_expand("{device}", &v, out, sizeof out, true);
    TEST_ASSERT_EQUAL_STRING("a\\\"b\\u000ac", out);
    tmpl_expand("{remaining} kala ({k}/{n})", &v, out, sizeof out, false);
    TEST_ASSERT_EQUAL_STRING("2 sa kala (2/3)", out);
    char tiny[8];
    tmpl_expand("{status}{status}", &v, tiny, sizeof tiny, false);
    TEST_ASSERT_EQUAL_UINT(7, strlen(tiny));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_smtp_lines_and_classify);
    RUN_TEST(test_ehlo_caps);
    RUN_TEST(test_body_escape);
    RUN_TEST(test_base64_and_auth);
    RUN_TEST(test_subject);
    RUN_TEST(test_mqtt);
    RUN_TEST(test_tmpl);
    return UNITY_END();
}
