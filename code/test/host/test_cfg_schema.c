/* Uretilen sema tablosunun kendi icinde tutarliligi. */
#include <string.h>
#include "unity.h"
#include "cfg_schema.gen.h"

void setUp(void) {}
void tearDown(void) {}

static void check_desc(const cfg_section_desc_t *d)
{
    TEST_ASSERT_NOT_NULL(d->name);
    TEST_ASSERT_TRUE(strlen(d->nvs_key) <= 13);   /* grup: "grp" + rakam; NVS siniri 15 */
    TEST_ASSERT_TRUE(d->nfields > 0);
    for (int i = 0; i < d->nfields; i++) {
        const cfg_field_t *f = &d->fields[i];
        TEST_ASSERT_TRUE_MESSAGE(f->offset + f->size <= d->size, f->name);
        if (f->type == CFG_T_STR) {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(f->max + 1, f->size, f->name);
            TEST_ASSERT_NOT_NULL(f->def_str);
            TEST_ASSERT_TRUE(strlen(f->def_str) <= f->max);
        } else if (f->type == CFG_T_ENUM) {
            TEST_ASSERT_TRUE(f->enum_count > 0);
            TEST_ASSERT_TRUE(f->def_num < f->enum_count);
        } else {
            TEST_ASSERT_TRUE_MESSAGE(f->min <= f->def_num && f->def_num <= f->max, f->name);
        }
        for (int j = i + 1; j < d->nfields; j++) {
            TEST_ASSERT_NOT_EQUAL(0, strcmp(f->name, d->fields[j].name));
        }
    }
}

static void test_sections_consistent(void)
{
    for (int s = 0; s < CFG_SEC__COUNT; s++) {
        check_desc(&cfg_sections[s]);
        TEST_ASSERT_TRUE(cfg_sections[s].offset + cfg_sections[s].size <= sizeof(cfg_t));
    }
    check_desc(&cfg_group_desc);
}

static void test_secret_fields_marked(void)
{
    const cfg_section_desc_t *smtp = &cfg_sections[CFG_SEC_SMTP];
    int secrets = 0;
    for (int i = 0; i < smtp->nfields; i++) {
        if (smtp->fields[i].flags & CFG_F_SECRET) secrets++;
    }
    TEST_ASSERT_EQUAL_INT(1, secrets);
    TEST_ASSERT_EQUAL_STRING("smtp", smtp->name);
}

static void test_sizes_fit_nvs(void)
{
    TEST_ASSERT_TRUE(sizeof(cfg_group_t) <= 4000);
    TEST_ASSERT_TRUE(sizeof(cfg_t) < 8 * 1024);   /* gruplar disarida: cfg_t kucuk kalmali */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sections_consistent);
    RUN_TEST(test_secret_fields_marked);
    RUN_TEST(test_sizes_fit_nvs);
    return UNITY_END();
}
